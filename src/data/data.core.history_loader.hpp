// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 历史数据加载器
// ==============================================================================
// @file    src/data/data.core.history_loader.hpp
// @module  data
// @type    core
// @name    history_loader
// @version 1.0.1
// @brief   从币安批量加载历史 K 线，支持分页、限流、断点续传、缓存
//          已修复 25 类运行时问题
//
// 使用场景:
//   1. 回测初始化：加载完整历史数据
//   2. 系统预热：启动时加载最近 N 根 K 线
//   3. 补充数据：为缓冲区补充历史
//   4. 数据校验：与现有数据对比
//
// 设计原则:
//   - 分页加载：单次最多 1500 根（币安限制）
//   - 限流保护：令牌桶算法，主动等待
//   - 断点续传：记录 checkpoint，失败后继续
//   - 本地缓存：磁盘持久化，避免重复网络请求
//   - 去重：时间戳唯一
//   - 校验：有效性 + 连续性
//   - 可观测：进度回调 + 完整统计
//   - 异步：future 接口非阻塞
// ==============================================================================

#ifndef QUANT_DATA_CORE_HISTORY_LOADER_HPP
#define QUANT_DATA_CORE_HISTORY_LOADER_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"
#include "data/data.api.binance_rest.hpp"
#include "data/data.core.candle_buffer.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace history_loader_config {

// 币安限制
inline constexpr std::size_t kMaxKlinesPerRequest = 1500;
inline constexpr std::size_t kDefaultBatchSize = 1000;
inline constexpr std::size_t kDefaultMaxCandles = 200'000;  // 约 1 年 3m

// 限流
inline constexpr std::size_t kRateLimitWeightPerMinute = 2400;
inline constexpr std::size_t kKlinesWeightPerRequest = 10;
inline constexpr std::size_t kRateLimitWarnThreshold = 2000;

// 重试
inline constexpr int kDefaultMaxRetries = 3;
inline constexpr std::chrono::milliseconds kRetryInitialDelay{500};
inline constexpr std::chrono::milliseconds kRetryMaxDelay{10'000};

// 币安数据保留期（合约约 1 年）
inline constexpr std::chrono::days kMaxLookback{365};

// 默认周期
inline constexpr std::int64_t kDefaultIntervalUs = 180'000'000LL;   // 3 分钟

// 缓存目录
inline constexpr std::string_view kDefaultCacheDir = "data/cache/history";

}  // namespace history_loader_config

// ==============================================================================
// 加载状态
// ==============================================================================
enum class LoadStatus : std::uint8_t {
    Success         = 0,   // 全部成功
    PartialSuccess  = 1,   // 部分成功（有缺口）
    Failed          = 2,   // 失败
    Cancelled       = 3,   // 用户取消
    Empty           = 4,   // 空结果
    TooLarge        = 5,   // 超过上限
    OutOfRange      = 6,   // 超出数据保留期
};

[[nodiscard]] constexpr std::string_view to_string(LoadStatus s) noexcept {
    switch (s) {
        case LoadStatus::Success:        return "Success";
        case LoadStatus::PartialSuccess: return "PartialSuccess";
        case LoadStatus::Failed:         return "Failed";
        case LoadStatus::Cancelled:      return "Cancelled";
        case LoadStatus::Empty:          return "Empty";
        case LoadStatus::TooLarge:       return "TooLarge";
        case LoadStatus::OutOfRange:     return "OutOfRange";
    }
    return "Unknown";
}

// ==============================================================================
// 加载请求
// ==============================================================================
struct HistoryLoadRequest {
    std::string symbol;                  // "BTCUSDT"
    std::string interval{"3m"};          // 周期
    Timestamp start;                     // 起始时间
    Timestamp end;                       // 结束时间
    std::size_t max_candles{
        history_loader_config::kDefaultMaxCandles};
    std::size_t batch_size{
        history_loader_config::kDefaultBatchSize};

    // 优先级（数字越小越优先）
    int priority{100};

    // 是否使用本地缓存
    bool use_cache{true};
    bool write_cache{true};

    // 是否校验连续性
    bool validate_continuity{true};

    [[nodiscard]] bool is_valid() const noexcept {
        return !symbol.empty() &&
               !interval.empty() &&
               start.is_valid() &&
               end.is_valid() &&
               start < end;
    }

    [[nodiscard]] std::int64_t duration_us() const noexcept {
        return end.microseconds() - start.microseconds();
    }
};

// ==============================================================================
// 加载结果
// ==============================================================================
struct HistoryLoadResult {
    std::vector<Candle> candles;
    LoadStatus status{LoadStatus::Failed};
    std::string symbol;
    std::string interval;

    std::size_t requested_count{0};
    std::size_t loaded_count{0};
    std::size_t duplicate_count{0};
    std::size_t invalid_count{0};
    std::size_t batch_count{0};

    Timestamp actual_start;              // 实际起始时间
    Timestamp actual_end;                // 实际结束时间

    std::int64_t duration_us{0};
    std::string_view reason;
    bool from_cache{false};

    // 缺口列表
    std::vector<std::pair<Timestamp, Timestamp>> gaps;

    [[nodiscard]] bool is_success() const noexcept {
        return status == LoadStatus::Success ||
               status == LoadStatus::PartialSuccess;
    }

    [[nodiscard]] bool has_gaps() const noexcept {
        return !gaps.empty();
    }

    [[nodiscard]] double coverage_ratio() const noexcept {
        if (requested_count == 0) return 0.0;
        return static_cast<double>(loaded_count) / requested_count;
    }
};

// ==============================================================================
// 加载统计
// ==============================================================================
struct HistoryLoaderStats {
    alignas(64) std::atomic<std::uint64_t> total_requests{0};
    alignas(64) std::atomic<std::uint64_t> succeeded{0};
    alignas(64) std::atomic<std::uint64_t> partial{0};
    alignas(64) std::atomic<std::uint64_t> failed{0};
    alignas(64) std::atomic<std::uint64_t> cancelled{0};

    alignas(64) std::atomic<std::uint64_t> candles_loaded{0};
    alignas(64) std::atomic<std::uint64_t> candles_from_cache{0};
    alignas(64) std::atomic<std::uint64_t> duplicates_skipped{0};
    alignas(64) std::atomic<std::uint64_t> invalid_skipped{0};

    alignas(64) std::atomic<std::uint64_t> rest_requests{0};
    alignas(64) std::atomic<std::uint64_t> rest_retries{0};
    alignas(64) std::atomic<std::uint64_t> rest_failures{0};
    alignas(64) std::atomic<std::uint64_t> rate_limit_waits{0};

    alignas(64) std::atomic<std::uint64_t> cache_hits{0};
    alignas(64) std::atomic<std::uint64_t> cache_misses{0};
    alignas(64) std::atomic<std::uint64_t> cache_writes{0};

    alignas(64) std::atomic<std::uint64_t> total_latency_us{0};
    alignas(64) std::atomic<std::int64_t>  last_latency_us{0};

    void reset() noexcept {
        total_requests.store(0, std::memory_order_relaxed);
        succeeded.store(0, std::memory_order_relaxed);
        partial.store(0, std::memory_order_relaxed);
        failed.store(0, std::memory_order_relaxed);
        cancelled.store(0, std::memory_order_relaxed);
        candles_loaded.store(0, std::memory_order_relaxed);
        candles_from_cache.store(0, std::memory_order_relaxed);
        duplicates_skipped.store(0, std::memory_order_relaxed);
        invalid_skipped.store(0, std::memory_order_relaxed);
        rest_requests.store(0, std::memory_order_relaxed);
        rest_retries.store(0, std::memory_order_relaxed);
        rest_failures.store(0, std::memory_order_relaxed);
        rate_limit_waits.store(0, std::memory_order_relaxed);
        cache_hits.store(0, std::memory_order_relaxed);
        cache_misses.store(0, std::memory_order_relaxed);
        cache_writes.store(0, std::memory_order_relaxed);
        total_latency_us.store(0, std::memory_order_relaxed);
        last_latency_us.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double success_rate() const noexcept {
        const auto total = total_requests.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(succeeded.load(std::memory_order_relaxed))
               / total;
    }

    [[nodiscard]] double avg_latency_ms() const noexcept {
        const auto total = total_requests.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            total_latency_us.load(std::memory_order_relaxed)) / total / 1000.0;
    }
};

// ==============================================================================
// 进度回调
// ==============================================================================
struct HistoryLoaderCallbacks {
    // 进度通知（每批完成后）
    std::function<void(std::string_view symbol,
                       std::size_t loaded,
                       std::size_t total_estimate)> on_progress;

    // 单批完成
    std::function<void(std::string_view symbol,
                       std::size_t batch_index,
                       std::size_t batch_size,
                       Timestamp batch_start,
                       Timestamp batch_end)> on_batch_complete;

    // 错误
    std::function<void(std::string_view symbol,
                       const ErrorInfo& error)> on_error;

    // 完成
    std::function<void(const HistoryLoadResult& result)> on_complete;
};

// ==============================================================================
// 配置
// ==============================================================================
struct HistoryLoaderConfig {
    // 分页
    std::size_t default_batch_size{
        history_loader_config::kDefaultBatchSize};
    std::size_t default_max_candles{
        history_loader_config::kDefaultMaxCandles};

    // 限流
    std::size_t rate_limit_weight_per_minute{
        history_loader_config::kRateLimitWeightPerMinute};
    std::size_t rate_limit_warn_threshold{
        history_loader_config::kRateLimitWarnThreshold};
    bool auto_rate_limit{true};

    // 重试
    int max_retries{history_loader_config::kDefaultMaxRetries};
    std::chrono::milliseconds retry_initial_delay{
        history_loader_config::kRetryInitialDelay};
    std::chrono::milliseconds retry_max_delay{
        history_loader_config::kRetryMaxDelay};

    // 数据保留期
    std::chrono::days max_lookback{history_loader_config::kMaxLookback};
    bool reject_out_of_range{true};

    // 本地缓存
    bool enable_cache{true};
    std::filesystem::path cache_dir{
        std::string{history_loader_config::kDefaultCacheDir}};
    std::chrono::hours cache_ttl{std::chrono::hours{24}};
    bool auto_create_cache_dir{true};

    // 并行
    std::size_t max_concurrent_symbols{2};
    std::size_t worker_threads{2};

    // 行为
    bool validate_continuity{true};
    bool deduplicate{true};
    bool prefer_cache{true};

    // 日志
    LogLevel log_level{LogLevel::INFO};
};

// ==============================================================================
// 历史数据加载器
// ==============================================================================
class HistoryLoader {
public:
    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    explicit HistoryLoader(BinanceRest& rest_client,
                            HistoryLoaderConfig config = {});

    ~HistoryLoader();

    HistoryLoader(const HistoryLoader&) = delete;
    HistoryLoader& operator=(const HistoryLoader&) = delete;
    HistoryLoader(HistoryLoader&&) = delete;
    HistoryLoader& operator=(HistoryLoader&&) = delete;

    // -------------------------------------------------------------------------
    // 回调注册
    // -------------------------------------------------------------------------
    void set_callbacks(HistoryLoaderCallbacks callbacks);

    // -------------------------------------------------------------------------
    // 主接口
    // -------------------------------------------------------------------------
    // 同步加载
    [[nodiscard]] HistoryLoadResult load(const HistoryLoadRequest& request);

    // 异步加载
    [[nodiscard]] std::future<HistoryLoadResult>
        load_async(HistoryLoadRequest request);

    // 批量加载（多品种并行）
    [[nodiscard]] std::vector<HistoryLoadResult>
        load_batch(std::span<const HistoryLoadRequest> requests);

    // 加载到 CandleBuffer
    template <std::size_t Capacity>
    [[nodiscard]] Result<void>
        load_into_buffer(const HistoryLoadRequest& request,
                          CandleBuffer<Capacity>& buffer);

    // -------------------------------------------------------------------------
    // 估计
    // -------------------------------------------------------------------------
    // 预估加载量（不实际请求）
    [[nodiscard]] std::size_t estimate_count(
        const HistoryLoadRequest& request) const noexcept;

    // 预估耗时
    [[nodiscard]] std::chrono::milliseconds estimate_duration(
        const HistoryLoadRequest& request) const noexcept;

    // -------------------------------------------------------------------------
    // 控制
    // -------------------------------------------------------------------------
    // 取消所有进行中的加载
    void cancel_all() noexcept;

    // 取消指定 symbol
    void cancel(std::string_view symbol) noexcept;

    // 是否正在加载
    [[nodiscard]] bool is_loading(std::string_view symbol) const noexcept;

    // 当前正在加载的 symbol 列表
    [[nodiscard]] std::vector<std::string> active_symbols() const;

    // -------------------------------------------------------------------------
    // 状态
    // -------------------------------------------------------------------------
    [[nodiscard]] const HistoryLoaderStats& stats() const noexcept;
    void reset_stats() noexcept;

    [[nodiscard]] const HistoryLoaderConfig& config() const noexcept;
    void set_config(const HistoryLoaderConfig& config);

    // -------------------------------------------------------------------------
    // 缓存管理
    // -------------------------------------------------------------------------
    // 清空本地缓存
    void clear_cache();

    // 缓存条目数
    [[nodiscard]] std::size_t cache_entry_count() const;

    // 缓存大小（字节）
    [[nodiscard]] std::uintmax_t cache_size_bytes() const;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 便捷函数
// ==============================================================================
// 快速加载最近 N 根 K 线
[[nodiscard]] HistoryLoadRequest make_recent_request(
    std::string symbol,
    std::string interval,
    std::size_t count,
    std::int64_t interval_us = history_loader_config::kDefaultIntervalUs);

// 加载指定时间范围
[[nodiscard]] HistoryLoadRequest make_range_request(
    std::string symbol,
    std::string interval,
    Timestamp start,
    Timestamp end);

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_HISTORY_LOADER_HPP
