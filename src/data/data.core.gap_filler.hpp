// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - K 线缺口补齐
// ==============================================================================
// @file    src/data/data.core.gap_filler.hpp
// @module  data
// @type    core
// @name    gap_filler
// @version 1.0.1
// @brief   检测并自动补齐 K 线缓冲区中的缺口
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 检测与补齐分离：先检测，按策略补齐
//   - 异步非阻塞：缺口检测在独立线程
//   - 分批拉取：避免单次大量请求
//   - 优先级：近期缺口优先
//   - 去重：已处理缺口不重复
//   - 容错：网络失败自动重试，超限告警
//   - 维护时段识别：交易所维护不作为缺口
//   - 可观测：完整统计 + 进度回调
// ==============================================================================

#ifndef QUANT_DATA_CORE_GAP_FILLER_HPP
#define QUANT_DATA_CORE_GAP_FILLER_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.metrics.hpp"
#include "data/data.core.candle_buffer.hpp"
#include "data/data.api.binance_rest.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace gap_filler_config {

// 缺口检测
inline constexpr std::size_t kMaxGapBars = 10'000;          // 单次最大缺口
inline constexpr std::size_t kBatchSize = 500;              // 每批拉取数量
inline constexpr std::size_t kMaxConcurrentFills = 4;       // 最大并发补齐

// 重试
inline constexpr int kMaxRetries = 3;
inline constexpr std::chrono::milliseconds kRetryInitialDelay{500};
inline constexpr std::chrono::milliseconds kRetryMaxDelay{10'000};

// 时间窗口
inline constexpr std::chrono::hours kRecentGapThreshold{24};    // 24 小时内的缺口优先
inline constexpr std::chrono::days kMaxGapAge{30};              // 超过 30 天不补齐

// 检测间隔
inline constexpr std::chrono::seconds kDefaultScanInterval{30};

// 维护时段识别
inline constexpr std::int64_t kMaintenanceWindowToleranceUs{
    5 * 60 * 1'000'000LL};   // 5 分钟容差

}  // namespace gap_filler_config

// ==============================================================================
// 缺口检测结果
// ==============================================================================
struct GapInfo {
    Timestamp start;              // 缺口第一根 K 线时间
    Timestamp end;                // 缺口最后一根 K 线时间
    std::size_t missing_count;    // 缺失根数
    bool is_recent;               // 是否近期缺口（24h 内）
    bool is_fillable;             // 是否可补齐（未超期）

    [[nodiscard]] bool is_valid() const noexcept {
        return start.is_valid() && end >= start && missing_count > 0;
    }

    [[nodiscard]] std::chrono::seconds age() const noexcept {
        const auto now_us = Timestamp::now().microseconds();
        const auto gap_us = start.microseconds();
        const auto age_us = now_us - gap_us;
        return std::chrono::seconds{age_us / 1'000'000LL};
    }
};

// ==============================================================================
// 补齐状态
// ==============================================================================
enum class FillStatus : std::uint8_t {
    Success         = 0,    // 全部补齐
    PartialSuccess  = 1,    // 部分补齐
    Failed          = 2,    // 失败
    Skipped         = 3,    // 跳过（维护/超期/重复）
    TooLarge        = 4,    // 缺口过大，拒绝
    NotFillable     = 5,    // 不可补齐
};

[[nodiscard]] constexpr std::string_view to_string(FillStatus s) noexcept {
    switch (s) {
        case FillStatus::Success:        return "Success";
        case FillStatus::PartialSuccess: return "PartialSuccess";
        case FillStatus::Failed:         return "Failed";
        case FillStatus::Skipped:        return "Skipped";
        case FillStatus::TooLarge:       return "TooLarge";
        case FillStatus::NotFillable:    return "NotFillable";
    }
    return "Unknown";
}

// ==============================================================================
// 补齐结果
// ==============================================================================
struct FillResult {
    GapInfo gap;
    FillStatus status{FillStatus::Failed};
    std::size_t filled_count{0};
    std::size_t failed_count{0};
    std::string_view reason;
    std::int64_t duration_us{0};

    [[nodiscard]] bool is_success() const noexcept {
        return status == FillStatus::Success ||
               status == FillStatus::PartialSuccess;
    }
};

// ==============================================================================
// 缺口补齐统计
// ==============================================================================
struct GapFillerStats {
    alignas(64) std::atomic<std::uint64_t> scans_total{0};
    alignas(64) std::atomic<std::uint64_t> gaps_detected{0};
    alignas(64) std::atomic<std::uint64_t> gaps_filled{0};
    alignas(64) std::atomic<std::uint64_t> gaps_partial{0};
    alignas(64) std::atomic<std::uint64_t> gaps_failed{0};
    alignas(64) std::atomic<std::uint64_t> gaps_skipped{0};
    alignas(64) std::atomic<std::uint64_t> bars_filled{0};
    alignas(64) std::atomic<std::uint64_t> rest_requests{0};
    alignas(64) std::atomic<std::uint64_t> rest_failures{0};
    alignas(64) std::atomic<std::uint64_t> retries_total{0};
    alignas(64) std::atomic<std::int64_t>  last_fill_latency_us{0};
    alignas(64) std::atomic<std::uint64_t> max_gap_seen{0};

    void reset() noexcept {
        scans_total.store(0, std::memory_order_relaxed);
        gaps_detected.store(0, std::memory_order_relaxed);
        gaps_filled.store(0, std::memory_order_relaxed);
        gaps_partial.store(0, std::memory_order_relaxed);
        gaps_failed.store(0, std::memory_order_relaxed);
        gaps_skipped.store(0, std::memory_order_relaxed);
        bars_filled.store(0, std::memory_order_relaxed);
        rest_requests.store(0, std::memory_order_relaxed);
        rest_failures.store(0, std::memory_order_relaxed);
        retries_total.store(0, std::memory_order_relaxed);
        last_fill_latency_us.store(0, std::memory_order_relaxed);
        max_gap_seen.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double success_rate() const noexcept {
        const auto detected = gaps_detected.load(std::memory_order_relaxed);
        if (detected == 0) return 0.0;
        return static_cast<double>(gaps_filled.load(std::memory_order_relaxed))
               / detected;
    }
};

// ==============================================================================
// 补齐策略
// ==============================================================================
enum class GapFillPolicy : std::uint8_t {
    Never           = 0,    // 从不补齐，仅检测
    RecentOnly      = 1,    // 仅补齐近期（24h 内）
    Within30Days    = 2,    // 补齐 30 天内
    Always          = 3,    // 尝试补齐所有
};

// ==============================================================================
// 配置
// ==============================================================================
struct GapFillerConfig {
    // 缺口检测
    std::int64_t interval_us{180'000'000LL};   // 3 分钟
    std::size_t max_gap_bars{gap_filler_config::kMaxGapBars};
    std::size_t batch_size{gap_filler_config::kBatchSize};

    // 补齐策略
    GapFillPolicy policy{GapFillPolicy::Within30Days};
    bool auto_scan{true};                       // 后台自动扫描
    std::chrono::seconds scan_interval{
        gap_filler_config::kDefaultScanInterval};

    // 重试
    int max_retries{gap_filler_config::kMaxRetries};
    std::chrono::milliseconds retry_initial_delay{
        gap_filler_config::kRetryInitialDelay};
    std::chrono::milliseconds retry_max_delay{
        gap_filler_config::kRetryMaxDelay};

    // 时间窗口
    std::chrono::hours recent_threshold{
        gap_filler_config::kRecentGapThreshold};
    std::chrono::days max_gap_age{gap_filler_config::kMaxGapAge};

    // 维护时段
    bool detect_maintenance{true};
    std::int64_t maintenance_tolerance_us{
        gap_filler_config::kMaintenanceWindowToleranceUs};

    // 行为
    bool dry_run{false};                        // 仅检测不补齐
    bool prioritize_recent{true};               // 近期优先
    bool allow_partial_fill{true};              // 允许部分成功
};

// ==============================================================================
// 回调接口
// ==============================================================================
struct GapFillerCallbacks {
    // 检测到缺口（补齐前）
    std::function<void(const GapInfo& gap)> on_gap_detected;

    // 补齐完成
    std::function<void(const FillResult& result)> on_fill_complete;

    // 进度通知
    std::function<void(std::size_t filled, std::size_t total)> on_progress;

    // 错误
    std::function<void(const ErrorInfo& error)> on_error;
};

// ==============================================================================
// 缺口补齐器
// ==============================================================================
// 线程模型：
//   - 后台扫描线程：定期检测缺口
//   - 补齐线程池：分批拉取缺失数据
//   - 主线程可调用 detect()/fill() 同步接口
//
// 线程安全：所有公开接口线程安全
template <std::size_t BufferCapacity = 1024>
class GapFiller {
public:
    using Buffer = CandleBuffer<BufferCapacity>;

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit GapFiller(BinanceRest& rest_client,
                        const GapFillerConfig& config = {});
    ~GapFiller();

    GapFiller(const GapFiller&) = delete;
    GapFiller& operator=(const GapFiller&) = delete;
    GapFiller(GapFiller&&) = delete;
    GapFiller& operator=(GapFiller&&) = delete;

    // -------------------------------------------------------------------------
    // 回调注册
    // -------------------------------------------------------------------------
    void set_callbacks(GapFillerCallbacks callbacks);

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    // 启动后台扫描线程
    Result<void> start();

    // 停止（等待所有补齐完成）
    void stop() noexcept;

    // -------------------------------------------------------------------------
    // 注册缓冲区
    // -------------------------------------------------------------------------
    // 注册缓冲区，缺口补齐将作用于此
    // symbol 必须匹配
    Result<void> register_buffer(std::string_view symbol,
                                   Buffer* buffer);

    // 注销缓冲区
    void unregister_buffer(std::string_view symbol);

    // -------------------------------------------------------------------------
    // 检测接口（同步）
    // -------------------------------------------------------------------------
    // 检测指定缓冲区中的缺口
    [[nodiscard]] std::vector<GapInfo>
        detect(std::string_view symbol) const;

    // 检测所有已注册缓冲区
    [[nodiscard]] std::unordered_map<std::string, std::vector<GapInfo>>
        detect_all() const;

    // -------------------------------------------------------------------------
    // 补齐接口（同步）
    // -------------------------------------------------------------------------
    // 补齐指定缺口
    [[nodiscard]] FillResult fill(const GapInfo& gap,
                                    std::string_view symbol);

    // 补齐指定缓冲区的所有缺口
    [[nodiscard]] std::vector<FillResult>
        fill_all(std::string_view symbol);

    // 补齐所有已注册缓冲区
    [[nodiscard]] std::vector<FillResult> fill_all_buffers();

    // -------------------------------------------------------------------------
    // 异步补齐
    // -------------------------------------------------------------------------
    // 提交异步补齐任务，返回 future
    [[nodiscard]] std::future<FillResult>
        fill_async(const GapInfo& gap, std::string_view symbol);

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] const GapFillerStats& stats() const noexcept;
    void reset_stats() noexcept;

    [[nodiscard]] const GapFillerConfig& config() const noexcept;
    void set_config(const GapFillerConfig& config);

    [[nodiscard]] bool is_running() const noexcept;

    [[nodiscard]] std::size_t registered_buffer_count() const noexcept;

    // -------------------------------------------------------------------------
    // 已处理缺口管理（去重）
    // -------------------------------------------------------------------------
    // 清空已处理缺口记录（测试用）
    void clear_processed_gaps();

    // 已处理的缺口数
    [[nodiscard]] std::size_t processed_gap_count() const noexcept;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 便捷类型别名
// ==============================================================================
using StandardGapFiller = GapFiller<1024>;
using LargeGapFiller = GapFiller<4096>;

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_GAP_FILLER_HPP
