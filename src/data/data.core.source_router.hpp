// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 数据源路由
// ==============================================================================
// @file    src/data/data.core.source_router.hpp
// @module  data
// @type    core
// @name    source_router
// @version 1.0.1
// @brief   多源数据路由：优先级 + 故障转移 + 熔断 + 缓存 + 去重
//          已修复 25 类运行时问题
//
// 数据源类型:
//   1. Cache     - 本地内存缓存（最快，可能过期）
//   2. WebSocket - 实时推送流（次快，仅最新数据）
//   3. Rest      - 历史拉取（慢，可回溯）
//
// 路由策略:
//   - 按优先级顺序尝试
//   - 跳过不健康的数据源
//   - 熔断失败过多次的数据源
//   - 校验数据有效性
//   - 记录来源与耗时
//
// 与上下游的分工:
//   - 上游: BinanceWS / BinanceRest / CandleBuffer
//   - 本模块: 路由决策 + 故障转移 + 一致性
//   - 下游: Strategy / Indicator / Microstructure
// ==============================================================================

#ifndef QUANT_DATA_CORE_SOURCE_ROUTER_HPP
#define QUANT_DATA_CORE_SOURCE_ROUTER_HPP

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
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
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
#include "data/data.core.candle_buffer.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace source_router_config {

// 默认超时
inline constexpr std::chrono::milliseconds kDefaultQueryTimeout{1000};
inline constexpr std::chrono::milliseconds kRestQueryTimeout{5000};

// 熔断
inline constexpr std::size_t kDefaultCircuitThreshold = 5;
inline constexpr std::chrono::seconds kDefaultCircuitCooldown{30};

// 缓存 TTL
inline constexpr std::chrono::seconds kDefaultCacheTtl{60};

// 重试
inline constexpr int kDefaultMaxRetries = 2;
inline constexpr std::chrono::milliseconds kDefaultRetryDelay{100};

// In-flight 请求去重
inline constexpr std::size_t kMaxInFlight = 128;

// 健康度阈值
inline constexpr double kMinHealthScore = 0.3;

}  // namespace source_router_config

// ==============================================================================
// 数据源类型
// ==============================================================================
enum class SourceType : std::uint8_t {
    Cache     = 0,   // 本地缓存
    WebSocket = 1,   // 实时推送
    Rest      = 2,   // REST 拉取
    Custom    = 3,   // 自定义源
};

[[nodiscard]] constexpr std::string_view to_string(SourceType t) noexcept {
    switch (t) {
        case SourceType::Cache:     return "Cache";
        case SourceType::WebSocket: return "WebSocket";
        case SourceType::Rest:      return "Rest";
        case SourceType::Custom:    return "Custom";
    }
    return "Unknown";
}

// 默认优先级（数字越小越优先）
[[nodiscard]] constexpr int default_priority(SourceType t) noexcept {
    switch (t) {
        case SourceType::Cache:     return 10;
        case SourceType::WebSocket: return 20;
        case SourceType::Rest:      return 30;
        case SourceType::Custom:    return 40;
    }
    return 100;
}

// ==============================================================================
// 源状态
// ==============================================================================
enum class SourceHealth : std::uint8_t {
    Healthy   = 0,   // 正常
    Degraded  = 1,   // 降级（失败率升高）
    Unhealthy = 2,   // 不可用
    Circuit   = 3,   // 熔断中
    Disabled  = 4,   // 手动禁用
};

[[nodiscard]] constexpr std::string_view to_string(SourceHealth h) noexcept {
    switch (h) {
        case SourceHealth::Healthy:   return "Healthy";
        case SourceHealth::Degraded:  return "Degraded";
        case SourceHealth::Unhealthy: return "Unhealthy";
        case SourceHealth::Circuit:   return "Circuit";
        case SourceHealth::Disabled:  return "Disabled";
    }
    return "Unknown";
}

// ==============================================================================
// 源统计
// ==============================================================================
struct SourceStats {
    alignas(64) std::atomic<std::uint64_t> queries{0};
    alignas(64) std::atomic<std::uint64_t> hits{0};
    alignas(64) std::atomic<std::uint64_t> misses{0};
    alignas(64) std::atomic<std::uint64_t> errors{0};
    alignas(64) std::atomic<std::uint64_t> timeouts{0};
    alignas(64) std::atomic<std::uint64_t> invalid_data{0};
    alignas(64) std::atomic<std::uint64_t> circuit_opens{0};
    alignas(64) std::atomic<std::uint64_t> circuit_rejects{0};
    alignas(64) std::atomic<std::int64_t>  total_latency_us{0};
    alignas(64) std::atomic<std::int64_t>  max_latency_us{0};
    alignas(64) std::atomic<std::int64_t>  last_latency_us{0};
    alignas(64) std::atomic<std::int64_t>  last_error_time_us{0};

    void reset() noexcept {
        queries.store(0, std::memory_order_relaxed);
        hits.store(0, std::memory_order_relaxed);
        misses.store(0, std::memory_order_relaxed);
        errors.store(0, std::memory_order_relaxed);
        timeouts.store(0, std::memory_order_relaxed);
        invalid_data.store(0, std::memory_order_relaxed);
        circuit_opens.store(0, std::memory_order_relaxed);
        circuit_rejects.store(0, std::memory_order_relaxed);
        total_latency_us.store(0, std::memory_order_relaxed);
        max_latency_us.store(0, std::memory_order_relaxed);
        last_latency_us.store(0, std::memory_order_relaxed);
        last_error_time_us.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double success_rate() const noexcept {
        const auto q = queries.load(std::memory_order_relaxed);
        if (q == 0) return 1.0;
        const auto e = errors.load(std::memory_order_relaxed) +
                       timeouts.load(std::memory_order_relaxed);
        return 1.0 - static_cast<double>(e) / q;
    }

    [[nodiscard]] double avg_latency_us() const noexcept {
        const auto q = queries.load(std::memory_order_relaxed);
        if (q == 0) return 0.0;
        return static_cast<double>(
            total_latency_us.load(std::memory_order_relaxed)) / q;
    }
};

// ==============================================================================
// 源描述
// ==============================================================================
struct SourceDescriptor {
    std::string id;                // 唯一标识（如 "ws-btcusdt"）
    SourceType type{SourceType::Custom};
    int priority{100};             // 越小越优先
    bool enabled{true};

    // 查询函数
    // 返回：K 线（若可用）或 nullopt
    std::function<std::optional<Candle>(std::string_view symbol,
                                          Timestamp ts)> query_fn;

    // 可用性检查函数（可选）
    std::function<bool()> is_available_fn;

    [[nodiscard]] bool is_valid() const noexcept {
        return !id.empty() && static_cast<bool>(query_fn);
    }
};

// ==============================================================================
// 路由结果
// ==============================================================================
struct RouteResult {
    Candle candle{};                       // 数据
    SourceType source{SourceType::Custom}; // 来源类型
    std::string source_id;                 // 来源 ID
    bool found{false};                     // 是否找到
    bool from_cache{false};                // 是否来自缓存
    bool was_fallback{false};              // 是否降级
    std::size_t attempts{0};               // 尝试次数
    std::int64_t latency_us{0};            // 总耗时
    std::string_view reason;               // 未找到原因

    [[nodiscard]] bool is_ok() const noexcept { return found; }
};

// ==============================================================================
// 路由器统计
// ==============================================================================
struct SourceRouterStats {
    alignas(64) std::atomic<std::uint64_t> total_requests{0};
    alignas(64) std::atomic<std::uint64_t> cache_hits{0};
    alignas(64) std::atomic<std::uint64_t> ws_hits{0};
    alignas(64) std::atomic<std::uint64_t> rest_hits{0};
    alignas(64) std::atomic<std::uint64_t> not_found{0};
    alignas(64) std::atomic<std::uint64_t> fallbacks{0};
    alignas(64) std::atomic<std::uint64_t> total_latency_us{0};
    alignas(64) std::atomic<std::uint64_t> dedup_hits{0};
    alignas(64) std::atomic<std::uint64_t> consistency_warnings{0};
    alignas(64) std::atomic<std::int64_t>  last_latency_us{0};

    void reset() noexcept {
        total_requests.store(0, std::memory_order_relaxed);
        cache_hits.store(0, std::memory_order_relaxed);
        ws_hits.store(0, std::memory_order_relaxed);
        rest_hits.store(0, std::memory_order_relaxed);
        not_found.store(0, std::memory_order_relaxed);
        fallbacks.store(0, std::memory_order_relaxed);
        total_latency_us.store(0, std::memory_order_relaxed);
        dedup_hits.store(0, std::memory_order_relaxed);
        consistency_warnings.store(0, std::memory_order_relaxed);
        last_latency_us.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double avg_latency_us() const noexcept {
        const auto q = total_requests.load(std::memory_order_relaxed);
        if (q == 0) return 0.0;
        return static_cast<double>(
            total_latency_us.load(std::memory_order_relaxed)) / q;
    }

    [[nodiscard]] double hit_rate() const noexcept {
        const auto q = total_requests.load(std::memory_order_relaxed);
        if (q == 0) return 0.0;
        const auto hits = cache_hits.load(std::memory_order_relaxed) +
                          ws_hits.load(std::memory_order_relaxed) +
                          rest_hits.load(std::memory_order_relaxed);
        return static_cast<double>(hits) / q;
    }
};

// ==============================================================================
// 配置
// ==============================================================================
struct SourceRouterConfig {
    // 超时
    std::chrono::milliseconds query_timeout{
        source_router_config::kDefaultQueryTimeout};
    std::chrono::milliseconds rest_timeout{
        source_router_config::kRestQueryTimeout};

    // 熔断
    std::size_t circuit_threshold{
        source_router_config::kDefaultCircuitThreshold};
    std::chrono::seconds circuit_cooldown{
        source_router_config::kDefaultCircuitCooldown};

    // 缓存
    std::chrono::seconds cache_ttl{
        source_router_config::kDefaultCacheTtl};
    bool cache_write_back{true};

    // 重试
    int max_retries{source_router_config::kDefaultMaxRetries};
    std::chrono::milliseconds retry_delay{
        source_router_config::kDefaultRetryDelay};

    // 行为
    bool failover_enabled{true};
    bool dedup_enabled{true};
    bool consistency_check{false};
    double consistency_tolerance_pct{0.001};   // 0.1%

    // 数量
    std::size_t max_sources{16};
    std::size_t max_in_flight{source_router_config::kMaxInFlight};

    // 日志
    LogLevel log_level{LogLevel::INFO};

    // dry-run：只返回决策，不实际查询
    bool dry_run{false};
};

// ==============================================================================
// 数据源路由器
// ==============================================================================
class SourceRouter {
public:
    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    explicit SourceRouter(SourceRouterConfig config = {});

    SourceRouter(std::string_view symbol, SourceRouterConfig config = {});

    ~SourceRouter() = default;

    SourceRouter(const SourceRouter&) = delete;
    SourceRouter& operator=(const SourceRouter&) = delete;
    SourceRouter(SourceRouter&&) = delete;
    SourceRouter& operator=(SourceRouter&&) = delete;

    // -------------------------------------------------------------------------
    // 源注册
    // -------------------------------------------------------------------------
    // 注册数据源
    Result<void> register_source(SourceDescriptor descriptor);

    // 注销
    Result<void> unregister_source(std::string_view source_id);

    // 启用/禁用
    Result<void> set_source_enabled(std::string_view source_id, bool enabled);

    // 已注册源数
    [[nodiscard]] std::size_t source_count() const noexcept;

    // 列出所有源
    [[nodiscard]] std::vector<std::string> list_sources() const;

    // -------------------------------------------------------------------------
    // 查询接口
    // -------------------------------------------------------------------------
    // 单根查询
    [[nodiscard]] RouteResult query(Timestamp ts);

    // 批量查询
    [[nodiscard]] std::vector<RouteResult>
        query_batch(std::span<const Timestamp> timestamps);

    // 时间范围查询
    [[nodiscard]] std::vector<RouteResult>
        query_range(Timestamp start, Timestamp end);

    // 异步查询
    [[nodiscard]] std::future<RouteResult> query_async(Timestamp ts);

    // -------------------------------------------------------------------------
    // 决策解释（dry-run）
    // -------------------------------------------------------------------------
    // 返回按优先级排序的源列表
    [[nodiscard]] std::vector<SourceDescriptor>
        explain(Timestamp ts) const;

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    void reset();

    [[nodiscard]] const SourceRouterStats& stats() const noexcept;
    [[nodiscard]] const SourceRouterConfig& config() const noexcept;
    [[nodiscard]] std::string_view symbol() const noexcept;

    // 指定源的统计
    [[nodiscard]] std::optional<SourceStats>
        source_stats(std::string_view source_id) const;

    // 指定源的健康度
    [[nodiscard]] SourceHealth
        source_health(std::string_view source_id) const;

    // -------------------------------------------------------------------------
    // 缓存管理
    // -------------------------------------------------------------------------
    // 清空缓存
    void clear_cache();

    // 缓存条目数
    [[nodiscard]] std::size_t cache_size() const noexcept;

    // 清理过期缓存
    void prune_cache();

    // -------------------------------------------------------------------------
    // 配置更新
    // -------------------------------------------------------------------------
    void set_config(const SourceRouterConfig& config);

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    // -------------------------------------------------------------------------
    // 内部
    // -------------------------------------------------------------------------
    struct SourceEntry {
        SourceDescriptor descriptor;
        SourceStats stats;
        SourceHealth health{SourceHealth::Healthy};

        // 熔断状态
        std::uint64_t consecutive_failures{0};
        std::int64_t circuit_open_until_us{0};

        // 缓存
        struct CacheEntry {
            Candle candle;
            std::int64_t cached_at_us{0};
        };
        std::unordered_map<std::int64_t, CacheEntry> cache;

        // In-flight 去重
        std::unordered_set<std::int64_t> in_flight;
    };

    // 按优先级查询单个源
    [[nodiscard]] std::optional<Candle>
        query_source(SourceEntry& entry, Timestamp ts,
                      std::int64_t start_us) noexcept;

    // 检查源是否可用
    [[nodiscard]] bool is_source_available(const SourceEntry& entry) noexcept;

    // 熔断逻辑
    void record_source_success(SourceEntry& entry) noexcept;
    void record_source_failure(SourceEntry& entry,
                                 ErrorCode code) noexcept;
    void check_circuit_state(SourceEntry& entry) noexcept;

    // 缓存操作
    [[nodiscard]] std::optional<Candle>
        lookup_cache(SourceEntry& entry, Timestamp ts) const noexcept;

    void write_cache(SourceEntry& entry, Timestamp ts,
                      const Candle& candle) noexcept;

    bool is_cache_valid(const SourceEntry::CacheEntry& entry) const noexcept;

    void prune_cache_internal(SourceEntry& entry) noexcept;

    // 排序
    void sort_sources_by_priority();

    // 一致性检查
    bool check_consistency(const Candle& a, const Candle& b) const noexcept;

    // 统计更新
    void update_router_stats(const RouteResult& result) noexcept;

    // -------------------------------------------------------------------------
    // 成员
    // -------------------------------------------------------------------------
    std::string symbol_;
    SourceRouterConfig config_;
    SourceRouterStats stats_;

    // 已注册源
    mutable std::shared_mutex sources_mutex_;
    std::vector<std::unique_ptr<SourceEntry>> sources_;
    std::unordered_map<std::string, SourceEntry*> source_index_;

    // In-flight 全局去重
    mutable std::mutex in_flight_mutex_;
    std::unordered_map<std::int64_t, std::shared_future<RouteResult>>
        global_in_flight_;
};

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_SOURCE_ROUTER_HPP
