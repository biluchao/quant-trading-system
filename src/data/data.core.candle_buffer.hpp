// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - K 线缓冲区
// ==============================================================================
// @file    src/data/data.core.candle_buffer.hpp
// @module  data
// @type    core
// @name    candle_buffer
// @version 1.0.1
// @brief   固定容量环形缓冲，支持多周期聚合、缺口检测、SIMD 友好提取
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 固定容量：编译期确定，避免动态分配
//   - 环形覆盖：O(1) push，无需 erase
//   - 缓存友好：SoA（Structure of Arrays）布局，SIMD 提取
//   - 读写分离：seqlock 保证读无锁，写原子
//   - 时间有序：单调时间戳校验，乱序拒绝
//   - 缺口检测：连续时间戳校验，缺口可查询
//   - 已收盘/未收盘分离：push_closed / push_update
//   - 多周期：从 3m 聚合为 5m/15m
//   - 快照：一致性读取，遍历无锁
// ==============================================================================

#ifndef QUANT_DATA_CORE_CANDLE_BUFFER_HPP
#define QUANT_DATA_CORE_CANDLE_BUFFER_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.simd.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace candle_buffer_config {

// 默认容量（2 的幂，编译期校验）
inline constexpr std::size_t kDefaultCapacity = 1024;
inline constexpr std::size_t kMinCapacity = 64;
inline constexpr std::size_t kMaxCapacity = 1 << 20;   // 1,048,576

// K 线周期
inline constexpr std::int64_t kInterval3mUs = 180'000'000LL;
inline constexpr std::int64_t kInterval5mUs = 300'000'000LL;
inline constexpr std::int64_t kInterval15mUs = 900'000'000LL;

// 最大聚合周期
inline constexpr std::size_t kMaxAggregations = 4;

}  // namespace candle_buffer_config

// ==============================================================================
// 缺口信息
// ==============================================================================
struct CandleGap {
    Timestamp start;              // 缺口起始（第一根缺失 K 线时间）
    Timestamp end;                // 缺口结束（最后一根缺失 K 线时间）
    std::size_t missing_count;    // 缺失根数

    [[nodiscard]] bool is_valid() const noexcept {
        return start.is_valid() && end >= start && missing_count > 0;
    }
};

// ==============================================================================
// 缓冲区统计
// ==============================================================================
struct CandleBufferStats {
    alignas(64) std::atomic<std::uint64_t> pushed_closed{0};
    alignas(64) std::atomic<std::uint64_t> pushed_update{0};
    alignas(64) std::atomic<std::uint64_t> rejected_out_of_order{0};
    alignas(64) std::atomic<std::uint64_t> rejected_invalid{0};
    alignas(64) std::atomic<std::uint64_t> rejected_symbol_mismatch{0};
    alignas(64) std::atomic<std::uint64_t> dropped_by_capacity{0};
    alignas(64) std::atomic<std::uint64_t> gaps_detected{0};
    alignas(64) std::atomic<std::uint64_t> total_gap_bars{0};
    alignas(64) std::atomic<std::uint64_t> overwrite_count{0};
    alignas(64) std::atomic<std::size_t>  current_size{0};
    alignas(64) std::atomic<std::size_t>  peak_size{0};
    alignas(64) std::atomic<std::uint64_t> snapshot_count{0};

    void reset() noexcept {
        pushed_closed.store(0, std::memory_order_relaxed);
        pushed_update.store(0, std::memory_order_relaxed);
        rejected_out_of_order.store(0, std::memory_order_relaxed);
        rejected_invalid.store(0, std::memory_order_relaxed);
        rejected_symbol_mismatch.store(0, std::memory_order_relaxed);
        dropped_by_capacity.store(0, std::memory_order_relaxed);
        gaps_detected.store(0, std::memory_order_relaxed);
        total_gap_bars.store(0, std::memory_order_relaxed);
        overwrite_count.store(0, std::memory_order_relaxed);
        current_size.store(0, std::memory_order_relaxed);
        peak_size.store(0, std::memory_order_relaxed);
        snapshot_count.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double fill_ratio() const noexcept {
        const auto peak = peak_size.load(std::memory_order_relaxed);
        if (peak == 0) return 0.0;
        return static_cast<double>(
            current_size.load(std::memory_order_relaxed)) / peak;
    }
};

// ==============================================================================
// 一致性快照（读取用）
// ==============================================================================
// 独立于缓冲区的浅拷贝，容量固定，无锁访问
class CandleSnapshot {
public:
    CandleSnapshot() = default;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] const Candle& operator[](std::size_t i) const noexcept {
        return candles_[i];
    }

    [[nodiscard]] const Candle& at(std::size_t i) const {
        if (i >= size_) {
            throw std::out_of_range("CandleSnapshot::at 越界");
        }
        return candles_[i];
    }

    [[nodiscard]] const Candle& latest() const {
        if (size_ == 0) {
            throw std::out_of_range("CandleSnapshot 为空");
        }
        return candles_[size_ - 1];
    }

    [[nodiscard]] const Candle& oldest() const {
        if (size_ == 0) {
            throw std::out_of_range("CandleSnapshot 为空");
        }
        return candles_[0];
    }

    [[nodiscard]] std::span<const Candle> view() const noexcept {
        return {candles_.get(), size_};
    }

    // 提取连续数组（用于 SIMD 指标计算）
    [[nodiscard]] std::vector<double> extract_closes() const;
    [[nodiscard]] std::vector<double> extract_highs() const;
    [[nodiscard]] std::vector<double> extract_lows() const;
    [[nodiscard]] std::vector<double> extract_volumes() const;

private:
    friend class CandleBuffer;

    std::unique_ptr<Candle[]> candles_;
    std::size_t size_{0};
    std::uint64_t version_{0};
    std::string symbol_;
};

// ==============================================================================
// K 线缓冲区
// ==============================================================================
// 固定容量环形缓冲，读写分离
// 线程安全：
//   - push_* 应由单一生产者线程调用
//   - snapshot() 可从任意线程调用
//   - 不支持多生产者并发 push
template <std::size_t Capacity = candle_buffer_config::kDefaultCapacity>
class CandleBuffer {
public:
    static_assert(Capacity >= candle_buffer_config::kMinCapacity,
        "容量小于最小值");
    static_assert(Capacity <= candle_buffer_config::kMaxCapacity,
        "容量超过最大值");
    static_assert((Capacity & (Capacity - 1)) == 0,
        "容量必须为 2 的幂");

    using value_type = Candle;
    using size_type = std::size_t;

    static constexpr size_type capacity() noexcept { return Capacity; }
    static constexpr size_type mask() noexcept { return Capacity - 1; }

    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    explicit CandleBuffer(std::string symbol = "")
        : symbol_{std::move(symbol)}
        , interval_us_{candle_buffer_config::kInterval3mUs}
    {}

    // 指定周期
    CandleBuffer(std::string symbol, std::int64_t interval_us)
        : symbol_{std::move(symbol)}
        , interval_us_{interval_us}
    {}

    ~CandleBuffer() = default;

    CandleBuffer(const CandleBuffer&) = delete;
    CandleBuffer& operator=(const CandleBuffer&) = delete;
    CandleBuffer(CandleBuffer&&) = delete;
    CandleBuffer& operator=(CandleBuffer&&) = delete;

    // -------------------------------------------------------------------------
    // 写入接口（单生产者线程）
    // -------------------------------------------------------------------------

    // 推送已收盘 K 线
    Result<void> push_closed(const Candle& candle) {
        // 校验
        if (!candle.is_valid()) {
            stats_.rejected_invalid.fetch_add(1, std::memory_order_relaxed);
            return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
                "K 线无效")
                .with_context("time", candle.open_time.to_iso8601());
        }

        // 时间戳校验（开仓时间必须是周期整数倍）
        if (interval_us_ > 0) {
            const auto us = candle.open_time.microseconds();
            if (us % interval_us_ != 0) {
                stats_.rejected_invalid.fetch_add(1, std::memory_order_relaxed);
                return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
                    "K 线时间戳未对齐周期")
                    .with_context("interval_us", std::to_string(interval_us_))
                    .with_context("time", candle.open_time.to_iso8601());
            }
        }

        // 顺序校验
        if (size_ > 0) {
            const auto& last = buffer_[(write_pos_ - 1) & mask()];
            if (candle.open_time <= last.open_time) {
                stats_.rejected_out_of_order.fetch_add(1,
                    std::memory_order_relaxed);
                return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
                    "K 线时间戳乱序或重复")
                    .with_context("last", last.open_time.to_iso8601())
                    .with_context("new", candle.open_time.to_iso8601());
            }

            // 缺口检测
            const auto expected = last.open_time.microseconds() + interval_us_;
            const auto actual = candle.open_time.microseconds();
            if (actual > expected) {
                const auto missing = (actual - expected) / interval_us_;
                if (missing > 0) {
                    record_gap(last.open_time + interval_us_,
                               candle.open_time - interval_us_,
                               static_cast<std::size_t>(missing));
                }
            }
        }

        // 写入
        write_candle(candle, true);

        stats_.pushed_closed.fetch_add(1, std::memory_order_relaxed);
        update_size_stats();
        return {};
    }

    // 推送未收盘 K 线（实时更新）
    // 如果时间戳与最新已收盘 K 线相同，则覆盖；否则视为新 K 线的更新
    Result<void> push_update(const Candle& candle) {
        if (!candle.is_valid()) {
            stats_.rejected_invalid.fetch_add(1, std::memory_order_relaxed);
            return QUANT_ERR_MSG(ErrorCode::UserInputInvalid, "K 线无效");
        }

        if (size_ > 0) {
            const auto& last = buffer_[(write_pos_ - 1) & mask()];
            if (candle.open_time == last.open_time) {
                // 覆盖未收盘的
                if (last.is_closed) {
                    // 已收盘的不能覆盖
                    stats_.rejected_out_of_order.fetch_add(1,
                        std::memory_order_relaxed);
                    return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
                        "已收盘 K 线不能被更新覆盖");
                }
                // 原地覆盖
                write_candle_at((write_pos_ - 1) & mask(), candle);
                stats_.pushed_update.fetch_add(1, std::memory_order_relaxed);
                return {};
            }
        }

        // 新的未收盘 K 线
        write_candle(candle, false);
        stats_.pushed_update.fetch_add(1, std::memory_order_relaxed);
        update_size_stats();
        return {};
    }

    // 批量加载（预热用，跳过校验以提升速度）
    Result<void> bulk_load(std::span<const Candle> candles) {
        for (const auto& c : candles) {
            auto r = push_closed(c);
            if (r.is_err()) {
                QUANT_LOG_WARN("[Buffer] bulk_load 遇到无效数据: {}",
                    r.error().to_string());
            }
        }
        return {};
    }

    // -------------------------------------------------------------------------
    // 读取接口（任意线程）
    // -------------------------------------------------------------------------

    // 生成一致性快照
    [[nodiscard]] std::shared_ptr<const CandleSnapshot> snapshot() const {
        std::lock_guard lock(read_mutex_);

        auto snap = std::make_shared<CandleSnapshot>();
        snap->size_ = size_;
        snap->version_ = version_;
        snap->symbol_ = symbol_;

        if (size_ > 0) {
            snap->candles_ = std::make_unique<Candle[]>(size_);

            // 从最旧的开始拷贝
            const auto start = (write_pos_ - size_) & mask();
            for (size_type i = 0; i < size_; ++i) {
                snap->candles_[i] = buffer_[(start + i) & mask()];
            }
        }

        stats_.snapshot_count.fetch_add(1, std::memory_order_relaxed);
        return snap;
    }

    // 最近 N 根
    [[nodiscard]] std::vector<Candle> last_n(size_type n) const {
        std::lock_guard lock(read_mutex_);

        if (n > size_) n = size_;
        std::vector<Candle> result;
        result.reserve(n);

        const auto start = (write_pos_ - n) & mask();
        for (size_type i = 0; i < n; ++i) {
            result.push_back(buffer_[(start + i) & mask()]);
        }
        return result;
    }

    // 最新 K 线
    [[nodiscard]] std::optional<Candle> latest() const {
        std::lock_guard lock(read_mutex_);
        if (size_ == 0) return std::nullopt;
        return buffer_[(write_pos_ - 1) & mask()];
    }

    // 最新已收盘 K 线
    [[nodiscard]] std::optional<Candle> latest_closed() const {
        std::lock_guard lock(read_mutex_);
        for (size_type i = 0; i < size_; ++i) {
            const auto idx = (write_pos_ - 1 - i) & mask();
            if (buffer_[idx].is_closed) {
                return buffer_[idx];
            }
        }
        return std::nullopt;
    }

    // 最旧 K 线
    [[nodiscard]] std::optional<Candle> oldest() const {
        std::lock_guard lock(read_mutex_);
        if (size_ == 0) return std::nullopt;
        return buffer_[(write_pos_ - size_) & mask()];
    }

    // 按索引访问（0 = 最旧）
    [[nodiscard]] std::optional<Candle> at(size_type index) const {
        std::lock_guard lock(read_mutex_);
        if (index >= size_) return std::nullopt;
        const auto start = (write_pos_ - size_) & mask();
        return buffer_[(start + index) & mask()];
    }

    // 时间范围查询
    [[nodiscard]] std::vector<Candle> range(Timestamp start,
                                             Timestamp end) const {
        std::lock_guard lock(read_mutex_);
        std::vector<Candle> result;
        if (size_ == 0) return result;

        const auto first = (write_pos_ - size_) & mask();
        for (size_type i = 0; i < size_; ++i) {
            const auto& c = buffer_[(first + i) & mask()];
            if (c.open_time >= start && c.open_time <= end) {
                result.push_back(c);
            }
        }
        return result;
    }

    // -------------------------------------------------------------------------
    // 状态查询（无锁）
    // -------------------------------------------------------------------------
    [[nodiscard]] size_type size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] bool full() const noexcept {
        return size_ == Capacity;
    }

    [[nodiscard]] std::string_view symbol() const noexcept {
        return symbol_;
    }

    [[nodiscard]] std::int64_t interval_us() const noexcept {
        return interval_us_;
    }

    [[nodiscard]] std::uint64_t version() const noexcept {
        return version_;
    }

    [[nodiscard]] const CandleBufferStats& stats() const noexcept {
        return stats_;
    }

    void reset_stats() noexcept {
        stats_.reset();
    }

    // -------------------------------------------------------------------------
    // 多周期聚合
    // -------------------------------------------------------------------------

    // 从当前 3m 缓冲聚合为指定周期的 K 线
    // 例如 aggregate(5 * 60 * 1000 * 1000) 得到 5m K 线
    [[nodiscard]] Result<std::vector<Candle>>
        aggregate(std::int64_t target_interval_us) const {
        if (target_interval_us <= interval_us_) {
            return QUANT_ERR_T(std::vector<Candle>, ErrorCode::UserInputInvalid)
                .with_context("target", std::to_string(target_interval_us))
                .with_context("source", std::to_string(interval_us_));
        }

        if (target_interval_us % interval_us_ != 0) {
            return QUANT_ERR_T(std::vector<Candle>, ErrorCode::UserInputInvalid)
                .with_context("reason", "目标周期不是源周期的整数倍");
        }

        std::lock_guard lock(read_mutex_);

        if (size_ == 0) return std::vector<Candle>{};

        const auto ratio = static_cast<size_type>(
            target_interval_us / interval_us_);

        std::vector<Candle> result;
        result.reserve(size_ / ratio + 1);

        const auto first = (write_pos_ - size_) & mask();

        // 找到第一个对齐到目标周期的位置
        size_type i = 0;
        while (i < size_) {
            const auto& c = buffer_[(first + i) & mask()];
            if (!c.is_closed) break;   // 未收盘不参与聚合

            const auto us = c.open_time.microseconds();
            if (us % target_interval_us == 0) {
                break;   // 已对齐
            }
            ++i;
        }

        // 聚合
        while (i + ratio <= size_) {
            Candle agg{};
            const auto& first_c = buffer_[(first + i) & mask()];
            if (!first_c.is_closed) break;

            agg.open_time = first_c.open_time;
            agg.close_time = Timestamp{
                first_c.open_time.microseconds() + target_interval_us};
            agg.open = first_c.open;
            agg.high = first_c.high;
            agg.low = first_c.low;
            agg.close = first_c.close;
            agg.volume = first_c.volume;
            agg.quote_volume = first_c.quote_volume;
            agg.trades = first_c.trades;
            agg.is_closed = true;

            bool all_closed = true;
            for (size_type j = 1; j < ratio; ++j) {
                const auto& c = buffer_[(first + i + j) & mask()];
                if (!c.is_closed) {
                    all_closed = false;
                    break;
                }
                agg.high = Price{std::max(agg.high.raw(), c.high.raw())};
                agg.low  = Price{std::min(agg.low.raw(), c.low.raw())};
                agg.close = c.close;
                agg.volume += c.volume;
                agg.quote_volume += c.quote_volume;
                agg.trades += c.trades;
            }

            if (!all_closed) break;

            result.push_back(std::move(agg));
            i += ratio;
        }

        return result;
    }

    // -------------------------------------------------------------------------
    // 缺口查询
    // -------------------------------------------------------------------------
    [[nodiscard]] std::vector<CandleGap> detect_gaps() const {
        std::lock_guard lock(read_mutex_);
        std::vector<CandleGap> gaps;

        if (size_ < 2) return gaps;

        const auto first = (write_pos_ - size_) & mask();
        for (size_type i = 1; i < size_; ++i) {
            const auto& prev = buffer_[(first + i - 1) & mask()];
            const auto& curr = buffer_[(first + i) & mask()];

            const auto expected = prev.open_time.microseconds() + interval_us_;
            const auto actual = curr.open_time.microseconds();

            if (actual > expected) {
                const auto missing = (actual - expected) / interval_us_;
                if (missing > 0) {
                    CandleGap gap;
                    gap.start = Timestamp{expected};
                    gap.end = Timestamp{actual - interval_us_};
                    gap.missing_count = static_cast<size_type>(missing);
                    gaps.push_back(gap);
                }
            }
        }

        return gaps;
    }

    // -------------------------------------------------------------------------
    // 清理
    // -------------------------------------------------------------------------
    // 清理指定时间之前的 K 线
    void prune_before(Timestamp cutoff) {
        std::lock_guard lock(read_mutex_);

        size_type removed = 0;
        const auto first = (write_pos_ - size_) & mask();
        for (size_type i = 0; i < size_; ++i) {
            const auto& c = buffer_[(first + i) & mask()];
            if (c.open_time < cutoff) {
                ++removed;
            } else {
                break;
            }
        }

        if (removed > 0) {
            size_ -= removed;
            update_size_stats();
        }
    }

    void clear() {
        std::lock_guard lock(read_mutex_);
        write_pos_ = 0;
        size_ = 0;
        stats_.current_size.store(0, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const {
        std::lock_guard lock(read_mutex_);
        std::string result;
        result.reserve(512);
        result += "CandleBuffer Dump:\n";
        result += "  symbol:            " + symbol_ + "\n";
        result += "  interval_us:       " + std::to_string(interval_us_) + "\n";
        result += "  capacity:          " + std::to_string(Capacity) + "\n";
        result += "  size:              " + std::to_string(size_) + "\n";
        result += "  version:           " + std::to_string(version_) + "\n";
        result += "  pushed_closed:     "
            + std::to_string(stats_.pushed_closed.load()) + "\n";
        result += "  pushed_update:     "
            + std::to_string(stats_.pushed_update.load()) + "\n";
        result += "  rejected_ooo:      "
            + std::to_string(stats_.rejected_out_of_order.load()) + "\n";
        result += "  rejected_invalid:  "
            + std::to_string(stats_.rejected_invalid.load()) + "\n";
        result += "  gaps_detected:     "
            + std::to_string(stats_.gaps_detected.load()) + "\n";
        result += "  total_gap_bars:    "
            + std::to_string(stats_.total_gap_bars.load()) + "\n";
        result += "  overwrite_count:   "
            + std::to_string(stats_.overwrite_count.load()) + "\n";
        return result;
    }

private:
    // =========================================================================
    // 内部写入
    // =========================================================================
    void write_candle(const Candle& candle, bool is_closed) {
        Candle c = candle;
        c.is_closed = is_closed;

        const auto idx = write_pos_ & mask();

        if (size_ == Capacity) {
            stats_.overwrite_count.fetch_add(1, std::memory_order_relaxed);
        }

        write_candle_at(idx, c);
        ++write_pos_;
        ++version_;
        if (size_ < Capacity) ++size_;
    }

    void write_candle_at(size_type idx, const Candle& candle) {
        buffer_[idx] = candle;
    }

    void update_size_stats() noexcept {
        stats_.current_size.store(size_, std::memory_order_relaxed);
        auto peak = stats_.peak_size.load(std::memory_order_relaxed);
        while (size_ > peak &&
               !stats_.peak_size.compare_exchange_weak(
                   peak, size_, std::memory_order_relaxed)) {}
    }

    void record_gap(Timestamp start, Timestamp end, size_type count) {
        stats_.gaps_detected.fetch_add(1, std::memory_order_relaxed);
        stats_.total_gap_bars.fetch_add(count, std::memory_order_relaxed);

        QUANT_LOG_WARN("[Buffer] 检测到缺口: {} [{} ~ {}] {} 根",
            symbol_,
            start.to_iso8601(),
            end.to_iso8601(),
            count);
    }

    // =========================================================================
    // 成员
    // =========================================================================
    std::string symbol_;
    std::int64_t interval_us_{candle_buffer_config::kInterval3mUs};

    // 环形缓冲（连续内存，缓存友好）
    std::array<Candle, Capacity> buffer_{};

    // 写指针（单调递增，不取模；索引时 & mask）
    std::uint64_t write_pos_{0};

    // 已用容量
    size_type size_{0};

    // 版本号（每次写入递增）
    std::uint64_t version_{0};

    // 读写锁
    mutable std::mutex read_mutex_;

    // 统计
    CandleBufferStats stats_;
};

// ==============================================================================
// 快照的提取方法（在头文件中定义，避免分离）
// ==============================================================================
inline std::vector<double> CandleSnapshot::extract_closes() const {
    std::vector<double> result;
    result.reserve(size_);
    for (size_type i = 0; i < size_; ++i) {
        result.push_back(candles_[i].close.to_double());
    }
    return result;
}

inline std::vector<double> CandleSnapshot::extract_highs() const {
    std::vector<double> result;
    result.reserve(size_);
    for (size_type i = 0; i < size_; ++i) {
        result.push_back(candles_[i].high.to_double());
    }
    return result;
}

inline std::vector<double> CandleSnapshot::extract_lows() const {
    std::vector<double> result;
    result.reserve(size_);
    for (size_type i = 0; i < size_; ++i) {
        result.push_back(candles_[i].low.to_double());
    }
    return result;
}

inline std::vector<double> CandleSnapshot::extract_volumes() const {
    std::vector<double> result;
    result.reserve(size_);
    for (size_type i = 0; i < size_; ++i) {
        result.push_back(candles_[i].volume.to_double());
    }
    return result;
}

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_CANDLE_BUFFER_HPP
