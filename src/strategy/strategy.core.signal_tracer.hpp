// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 信号溯源
// ==============================================================================
// @file    src/strategy/strategy.core.signal_tracer.hpp
// @module  strategy
// @type    core
// @name    signal_tracer
// @version 1.0.1
// @brief   信号决策链路追踪，支持完整输入快照、因果链、多维查询
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 完整快照：记录指标、带宽、评分、AI、MTF、参数版本
//   - 因果链：parent_id + trace_id 支持上下游追溯
//   - 多索引：SignalId / TraceId / Timestamp 三索引
//   - 容量控制：环形 + LRU + 优先级淘汰
//   - 异步写入：Sink 接口，失败不阻塞主流程
//   - 去重：同一 SignalId 只记录一次
//   - 采样：高频时可降采样
//   - 可观测：统计 + 诊断
//   - 可回放：JSON 序列化
//
// 追踪内容:
//   - 输入：K 线 / 指标 / 带宽 / 评分 / AI / MTF / 市场状态
//   - 参数：参数版本 + 关键参数快照
//   - 决策：评分 / 方向 / 盈亏比 / 仓位
//   - 输出：Signal + 是否触发
//   - 时序：各阶段耗时
//   - 因果：parent_id + trace_id + children
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_SIGNAL_TRACER_HPP
#define QUANT_STRATEGY_CORE_SIGNAL_TRACER_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ==============================================================================
// 项目内部
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.timestamp.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "indicator/indicator.core.calculator.hpp"
#include "indicator/indicator.core.band.hpp"

// ==============================================================================
// 前向声明
// ==============================================================================
namespace quant::strategy {
    struct MTFContext;
    enum class MarketRegime : std::uint8_t;
    enum class StrategyType : std::uint8_t;
}

namespace quant::strategy {

// ==============================================================================
// 常量
// ==============================================================================
namespace tracer_config {

// 默认容量
inline constexpr std::size_t kDefaultCapacity = 4096;

// 最小/最大容量
inline constexpr std::size_t kMinCapacity = 64;
inline constexpr std::size_t kMaxCapacity = 100000;

// TTL（秒）
inline constexpr std::int64_t kDefaultTtlSec = 7 * 24 * 3600;   // 7 天

// 采样率
inline constexpr double kDefaultSampleRate = 1.0;

// 批量 flush 大小
inline constexpr std::size_t kDefaultBatchSize = 64;

// schema 版本
inline constexpr std::uint32_t kSchemaVersion = 1;

}  // namespace tracer_config

// ==============================================================================
// 追踪优先级
// ==============================================================================
enum class TracePriority : std::uint8_t {
    Low     = 0,
    Normal  = 1,
    High    = 2,
    Critical= 3,   // 永不淘汰
};

[[nodiscard]] constexpr std::string_view to_string(TracePriority p) noexcept {
    switch (p) {
        case TracePriority::Low:      return "Low";
        case TracePriority::Normal:   return "Normal";
        case TracePriority::High:     return "High";
        case TracePriority::Critical: return "Critical";
    }
    return "Unknown";
}

// ==============================================================================
// 决策路径单步
// ==============================================================================
struct DecisionStep {
    std::string_view stage;       // 阶段名（"trend_check" / "score" / "rr" ...）
    bool passed{false};            // 是否通过
    double value{0.0};             // 关键值
    double threshold{0.0};         // 阈值
    std::string detail;            // 详情

    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 指标快照（不可变拷贝）
// ==============================================================================
struct IndicatorSnapshot {
    double ema26{0.0};
    double ema12{0.0};
    double ema_slope{0.0};
    double atr14{0.0};
    double atr50{0.0};
    double rsi7{0.0};
    double rsi14{0.0};
    double macd_hist{0.0};
    double obv_slope{0.0};
    double volume_ma20{0.0};
    double volume_current{0.0};
    double acceleration{0.0};
    double acc_norm{0.0};
    double funding_rate{0.0};

    [[nodiscard]] static IndicatorSnapshot from(
        const IndicatorResult& ind) noexcept;
};

// ==============================================================================
// 带宽快照
// ==============================================================================
struct BandSnapshot {
    double upper_band{0.0};
    double lower_band{0.0};
    double band_width{0.0};
    double band_pct{0.0};
    double k_adaptive{0.0};
    bool is_breakout_up{false};
    bool is_breakout_down{false};

    [[nodiscard]] static BandSnapshot from(
        const BandResult& band) noexcept;
};

// ==============================================================================
// AI 快照
// ==============================================================================
struct AISnapshot {
    bool valid{false};
    double trend_probability{0.5};
    double confidence{0.5};
    std::string_view model_version;
};

// ==============================================================================
// MTF 快照
// ==============================================================================
struct MTFSnapshot {
    bool valid{false};
    std::uint8_t trend_5m{0};        // 0=Neutral,1=Up,2=Down
    double trend_strength_5m{0.0};
    double dist_to_resistance_atr{0.0};
    double dist_to_support_atr{0.0};
    bool has_bearish_engulfing{false};
    bool has_bullish_engulfing{false};
};

// ==============================================================================
// 参数快照
// ==============================================================================
struct ParamSnapshot {
    std::uint32_t version{0};
    double band_k{0.0};
    double entry_threshold{0.0};
    double reverse_threshold{0.0};
    double stop_atr{0.0};
    double target_r{0.0};
    double escape_atr{0.0};
    int max_add_count{0};
    int cooldown_bars{0};
};

// ==============================================================================
// 时序记录
// ==============================================================================
struct TraceTiming {
    std::int64_t total_us{0};
    std::int64_t indicator_us{0};
    std::int64_t scoring_us{0};
    std::int64_t decision_us{0};
    std::int64_t risk_check_us{0};
};

// ==============================================================================
// 信号追踪记录
// ==============================================================================
struct SignalTrace {
    // 标识
    std::uint32_t schema_version{tracer_config::kSchemaVersion};
    SignalId signal_id{};
    TraceId trace_id{};
    TraceId parent_trace_id{};
    SignalId parent_signal_id{};

    // 时间
    Timestamp timestamp{};
    Timestamp recorded_at{};

    // 上下文
    Symbol symbol;
    StrategyType strategy_type{};

    // 输入快照
    Candle candle;
    IndicatorSnapshot indicators;
    BandSnapshot band;
    AISnapshot ai;
    MTFSnapshot mtf;
    std::uint8_t market_regime{0};

    // 参数
    ParamSnapshot params;

    // 决策路径
    std::vector<DecisionStep> decision_path;

    // 决策结果
    bool signal_generated{false};
    Side side{Side::UNKNOWN};
    Price entry_price{};
    Price stop_loss{};
    Price take_profit_1{};
    Price take_profit_2{};
    Quantity suggested_quantity{};
    double score{0.0};
    double confidence{0.0};
    double risk_reward_ratio{0.0};

    // 决策原因
    std::string decision_reason;

    // 时序
    TraceTiming timing;

    // 优先级
    TracePriority priority{TracePriority::Normal};

    // 子节点（下游信号）
    std::vector<SignalId> children;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] std::string to_string() const;

    // JSON 序列化
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] static Result<SignalTrace> from_json(
        std::string_view json);
};

// ==============================================================================
// 追踪查询过滤器
// ==============================================================================
struct TraceFilter {
    std::optional<SignalId> signal_id;
    std::optional<TraceId> trace_id;
    std::optional<Symbol> symbol;
    std::optional<Side> side;
    std::optional<StrategyType> strategy_type;
    std::optional<bool> signal_generated;
    std::optional<TracePriority> min_priority;
    std::optional<Timestamp> from_time;
    std::optional<Timestamp> to_time;
    std::optional<double> min_score;
    std::optional<double> max_score;
    std::size_t max_results{100};

    [[nodiscard]] bool matches(const SignalTrace& trace) const noexcept;
};

// ==============================================================================
// 追踪统计
// ==============================================================================
struct TracerStats {
    std::atomic<std::uint64_t> traced_count{0};
    std::atomic<std::uint64_t> deduplicated_count{0};
    std::atomic<std::uint64_t> sampled_out_count{0};
    std::atomic<std::uint64_t> evicted_count{0};
    std::atomic<std::uint64_t> query_count{0};
    std::atomic<std::uint64_t> query_hit_count{0};
    std::atomic<std::uint64_t> query_miss_count{0};
    std::atomic<std::uint64_t> sink_write_count{0};
    std::atomic<std::uint64_t> sink_failed_count{0};
    std::atomic<std::uint64_t> invalid_trace_count{0};
    std::atomic<std::uint64_t> serialize_error_count{0};

    void reset() noexcept {
        traced_count.store(0, std::memory_order_relaxed);
        deduplicated_count.store(0, std::memory_order_relaxed);
        sampled_out_count.store(0, std::memory_order_relaxed);
        evicted_count.store(0, std::memory_order_relaxed);
        query_count.store(0, std::memory_order_relaxed);
        query_hit_count.store(0, std::memory_order_relaxed);
        query_miss_count.store(0, std::memory_order_relaxed);
        sink_write_count.store(0, std::memory_order_relaxed);
        sink_failed_count.store(0, std::memory_order_relaxed);
        invalid_trace_count.store(0, std::memory_order_relaxed);
        serialize_error_count.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double query_hit_rate() const noexcept {
        const auto total = query_count.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            query_hit_count.load(std::memory_order_relaxed)) /
            static_cast<double>(total);
    }
};

// ==============================================================================
// Sink 接口（异步输出到外部存储）
// ==============================================================================
class ITraceSink {
public:
    virtual ~ITraceSink() = default;

    ITraceSink(const ITraceSink&) = delete;
    ITraceSink& operator=(const ITraceSink&) = delete;

    // 批量写入
    [[nodiscard]] virtual Result<void> write_batch(
        const std::vector<SignalTrace>& traces) = 0;

    // flush
    virtual void flush() noexcept {}

    // 名称
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

protected:
    ITraceSink() = default;
};

// ==============================================================================
// 追踪配置
// ==============================================================================
struct TracerConfig {
    // 容量
    std::size_t capacity{tracer_config::kDefaultCapacity};

    // TTL
    std::int64_t ttl_sec{tracer_config::kDefaultTtlSec};

    // 采样率
    double sample_rate{tracer_config::kDefaultSampleRate};

    // 是否启用采样
    bool enable_sampling{false};

    // 批量 flush 大小
    std::size_t batch_size{tracer_config::kDefaultBatchSize};

    // 是否启用去重
    bool enable_dedup{true};

    // 是否保留 Critical 优先级（永不淘汰）
    bool protect_critical{true};

    // 是否异步写入 Sink
    bool async_sink{true};

    // 是否在查询时统计
    bool track_query_stats{true};

    [[nodiscard]] Result<void> validate() const noexcept;

    [[nodiscard]] static TracerConfig defaults() noexcept {
        return TracerConfig{};
    }
};

// ==============================================================================
// 信号追踪器
// ==============================================================================
class SignalTracer {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 配置提供者（必需）
        std::function<TracerConfig()> config_provider;

        // Sink（可选，外部存储）
        std::shared_ptr<ITraceSink> sink;

        // 追踪事件回调（可选）
        std::function<void(const SignalTrace&)> on_trace;

        // 淘汰回调（可选）
        std::function<void(const SignalTrace&, std::string_view reason)>
            on_evict;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit SignalTracer(Dependencies deps);
    ~SignalTracer();

    SignalTracer(const SignalTracer&) = delete;
    SignalTracer& operator=(const SignalTracer&) = delete;
    SignalTracer(SignalTracer&&) noexcept;
    SignalTracer& operator=(SignalTracer&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 核心接口：记录追踪
    // -------------------------------------------------------------------------
    // 记录一条信号追踪
    // 幂等性：同一 SignalId 只记录一次（若 enable_dedup）
    Result<void> trace(SignalTrace record);

    // 便捷：从上下文构造追踪
    [[nodiscard]] SignalTrace make_trace_from_context(
        const SignalId& signal_id,
        const TraceId& trace_id,
        const TraceId& parent_trace_id,
        const Symbol& symbol,
        StrategyType strategy_type,
        const Candle& candle,
        const IndicatorResult& indicators,
        const BandResult& band,
        const MTFContext* mtf,
        std::uint8_t market_regime,
        const ParamSnapshot& params,
        const std::vector<DecisionStep>& decision_path,
        bool signal_generated,
        Side side,
        Price entry,
        Price stop,
        Price tp1,
        Price tp2,
        double score,
        double confidence,
        double rr,
        std::string_view reason) const;

    // -------------------------------------------------------------------------
    // 关联：设置子节点
    // -------------------------------------------------------------------------
    // 将 child_id 加入 parent_id 的 children 列表
    void link_child(const SignalId& parent_id, const SignalId& child_id);

    // -------------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------------
    // 按 SignalId 查询
    [[nodiscard]] std::optional<SignalTrace> query_signal(
        const SignalId& id) const;

    // 按 TraceId 查询
    [[nodiscard]] std::optional<SignalTrace> query_trace(
        const TraceId& id) const;

    // 按条件查询
    [[nodiscard]] std::vector<SignalTrace> query(
        const TraceFilter& filter) const;

    // 查询因果链（从当前节点向上追溯到根）
    [[nodiscard]] std::vector<SignalTrace> query_ancestors(
        const SignalId& id) const;

    // 查询子节点
    [[nodiscard]] std::vector<SignalTrace> query_children(
        const SignalId& id) const;

    // 按时间区间查询
    [[nodiscard]] std::vector<SignalTrace> query_time_range(
        Timestamp from, Timestamp to,
        std::size_t max_results = 100) const;

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    // -------------------------------------------------------------------------
    // 手动清理
    // -------------------------------------------------------------------------
    // 清理过期记录
    std::size_t prune_expired();

    // 清空所有
    void clear();

    // -------------------------------------------------------------------------
    // 持久化
    // -------------------------------------------------------------------------
    // 导出所有追踪为 JSON
    [[nodiscard]] std::string export_json() const;

    // 从 JSON 导入
    Result<void> import_json(std::string_view json);

    // 手动 flush 到 Sink
    Result<void> flush();

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const TracerStats& stats() const noexcept;
    [[nodiscard]] std::string dump() const;

private:
    // =========================================================================
    // 内部实现
    // =========================================================================
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 辅助函数
// ==============================================================================

// 生成 SignalId（原子递增）
[[nodiscard]] SignalId generate_signal_id() noexcept;

// 生成 TraceId（原子递增）
[[nodiscard]] TraceId generate_trace_id() noexcept;

// 从 Signal 构造 DecisionStep（示例）
[[nodiscard]] DecisionStep make_step(
    std::string_view stage,
    bool passed,
    double value,
    double threshold,
    std::string_view detail) noexcept;

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_SIGNAL_TRACER_HPP
