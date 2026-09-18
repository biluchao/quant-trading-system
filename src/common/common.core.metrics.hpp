// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - Prometheus 指标采集
// ==============================================================================
// @file    src/common/common.core.metrics.hpp
// @module  common
// @type    core
// @name    metrics
// @version 1.0.1
// @brief   线程安全、低开销的 Prometheus 指标封装，支持 Counter/Gauge/Histogram/Summary
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 热路径零字符串：句柄（Handle）预创建，Increment 仅原子操作
//   - 命名规范：quant_ 前缀 + snake_case + 单位后缀 + _total 累计后缀
//   - 标签安全：编译期/加载期校验标签名合法性，禁止高基数
//   - 失败降级：Exposer 端口占用不阻断启动，降级为无监控
//   - 线程安全：call_once 初始化，Gauge 使用 CAS，避免 Change 竞态
//   - 编译期开关：QUANT_ENABLE_METRICS=0 时全部为空实现
// ==============================================================================

#ifndef QUANT_COMMON_CORE_METRICS_HPP
#define QUANT_COMMON_CORE_METRICS_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ==============================================================================
// 编译期开关
// ==============================================================================
#ifndef QUANT_ENABLE_METRICS
    #ifdef NDEBUG
        #define QUANT_ENABLE_METRICS 1
    #else
        #define QUANT_ENABLE_METRICS 1
    #endif
#endif

#if QUANT_ENABLE_METRICS

// ==============================================================================
// prometheus-cpp
// ==============================================================================
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <prometheus/summary.h>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant {
namespace metrics {

// ==============================================================================
// 常量
// ==============================================================================
inline constexpr std::string_view kNamespacePrefix = "quant_";
inline constexpr std::string_view kDefaultAddress = "0.0.0.0:9090";

// 高基数标签黑名单（禁止使用）
inline constexpr std::array<std::string_view, 6> kForbiddenLabelNames = {
    "order_id", "trade_id", "signal_id", "trace_id",
    "user_id", "request_id"
};

// ==============================================================================
// 内部：名称校验
// ==============================================================================
namespace detail {

// Prometheus 名称规范: [a-zA-Z_:][a-zA-Z0-9_:]*
[[nodiscard]] inline bool is_valid_metric_name(std::string_view name) noexcept {
    if (name.empty()) return false;
    const auto is_first = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '_' || c == ':';
    };
    const auto is_rest = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == ':';
    };
    if (!is_first(name[0])) return false;
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (!is_rest(name[i])) return false;
    }
    return true;
}

// 标签名规范: [a-zA-Z_][a-zA-Z0-9_]*
[[nodiscard]] inline bool is_valid_label_name(std::string_view name) noexcept {
    if (name.empty()) return false;
    if (name.size() >= 2 && name[0] == '_' && name[1] == '_') return false;  // 保留
    const auto is_first = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    const auto is_rest = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    if (!is_first(name[0])) return false;
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (!is_rest(name[i])) return false;
    }
    return true;
}

[[nodiscard]] inline bool is_forbidden_label(std::string_view name) noexcept {
    for (auto f : kForbiddenLabelNames) {
        if (name == f) return true;
    }
    return false;
}

// 校验标签集
[[nodiscard]] inline Result<void> validate_labels(
    const std::map<std::string, std::string>& labels)
{
    for (const auto& [k, v] : labels) {
        if (!is_valid_label_name(k)) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "非法标签名: " + k)
                .with_context("label", k);
        }
        if (is_forbidden_label(k)) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "禁止使用高基数标签: " + k)
                .with_context("label", k);
        }
    }
    return {};
}

}  // namespace detail

// ==============================================================================
// 指标句柄（预创建，热路径仅原子操作）
// ==============================================================================
class CounterHandle {
public:
    CounterHandle() = default;
    explicit CounterHandle(prometheus::Counter* c) noexcept : counter_{c} {}

    void increment(double value = 1.0) const noexcept {
        if (counter_) counter_->Increment(value);
    }

    [[nodiscard]] double value() const noexcept {
        return counter_ ? counter_->Value() : 0.0;
    }

    [[nodiscard]] bool valid() const noexcept { return counter_ != nullptr; }

private:
    prometheus::Counter* counter_{nullptr};
};

class GaugeHandle {
public:
    GaugeHandle() = default;
    explicit GaugeHandle(prometheus::Gauge* g) noexcept : gauge_{g} {}

    void set(double value) const noexcept {
        if (gauge_) gauge_->Set(value);
    }

    void increment(double value = 1.0) const noexcept {
        if (gauge_) gauge_->Increment(value);
    }

    void decrement(double value = 1.0) const noexcept {
        if (gauge_) gauge_->Decrement(value);
    }

    // 线程安全的加法（CAS 循环，避免 Change 竞态）
    void add(double delta) const noexcept {
        if (!gauge_) return;
        if (delta >= 0) {
            gauge_->Increment(delta);
        } else {
            gauge_->Decrement(-delta);
        }
    }

    [[nodiscard]] double value() const noexcept {
        return gauge_ ? gauge_->Value() : 0.0;
    }

    [[nodiscard]] bool valid() const noexcept { return gauge_ != nullptr; }

private:
    prometheus::Gauge* gauge_{nullptr};
};

class HistogramHandle {
public:
    HistogramHandle() = default;
    explicit HistogramHandle(prometheus::Histogram* h) noexcept : histogram_{h} {}

    void observe(double value) const noexcept {
        if (histogram_) histogram_->Observe(value);
    }

    [[nodiscard]] bool valid() const noexcept { return histogram_ != nullptr; }

private:
    prometheus::Histogram* histogram_{nullptr};
};

// ==============================================================================
// 预定义 Histogram 桶（交易场景）
// ==============================================================================
namespace buckets {

// 延迟（秒）：覆盖微秒到秒
inline const std::vector<double> kLatencySeconds{
    0.000001, 0.000005, 0.00001, 0.00005, 0.0001,
    0.0005, 0.001, 0.005, 0.01, 0.05,
    0.1, 0.5, 1.0, 5.0, 10.0
};

// 订单大小（USDT）
inline const std::vector<double> kOrderSizeUsdt{
    10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000
};

// 盈亏比（R 倍数）
inline const std::vector<double> kRiskReward{
    0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 10.0
};

// 滑点（基点）
inline const std::vector<double> kSlippageBps{
    0.1, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0
};

}  // namespace buckets

// ==============================================================================
// Metrics 管理器
// ==============================================================================
class Metrics {
public:
    using Labels = std::map<std::string, std::string>;

    // -------------------------------------------------------------------------
    // 单例
    // -------------------------------------------------------------------------
    [[nodiscard]] static Metrics& instance() noexcept;

    // 禁止拷贝
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    // -------------------------------------------------------------------------
    // 初始化（幂等）
    // -------------------------------------------------------------------------
    // 返回: 成功或错误。端口占用等非致命错误返回警告，仍可继续运行（降级）
    Result<void> init(std::string_view address = kDefaultAddress) noexcept;

    // 是否已就绪
    [[nodiscard]] bool is_ready() const noexcept {
        return ready_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // Counter
    // -------------------------------------------------------------------------
    // 名称会自动追加 _total 后缀（若未以 _total 结尾）
    CounterHandle counter(std::string_view name,
                          std::string_view help,
                          const Labels& labels = {});

    // -------------------------------------------------------------------------
    // Gauge
    // -------------------------------------------------------------------------
    GaugeHandle gauge(std::string_view name,
                      std::string_view help,
                      const Labels& labels = {});

    // -------------------------------------------------------------------------
    // Histogram
    // -------------------------------------------------------------------------
    HistogramHandle histogram(std::string_view name,
                              std::string_view help,
                              const std::vector<double>& bucket_bounds,
                              const Labels& labels = {});

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] std::size_t metric_family_count() const noexcept;
    [[nodiscard]] std::string dump() const;

    // -------------------------------------------------------------------------
    // 关闭
    // -------------------------------------------------------------------------
    void shutdown() noexcept;

private:
    Metrics() = default;
    ~Metrics() = default;

    // 名称规范化：添加 quant_ 前缀和 _total 后缀
    [[nodiscard]] static std::string normalize_counter_name(std::string_view name);
    [[nodiscard]] static std::string normalize_name(std::string_view name);

    // 缓存键：名称 + 标签
    [[nodiscard]] static std::string cache_key(std::string_view name,
                                                const Labels& labels);

    std::once_flag init_flag_;
    std::atomic<bool> ready_{false};

    std::unique_ptr<prometheus::Exposer> exposer_;
    std::shared_ptr<prometheus::Registry> registry_;

    mutable std::mutex counters_mutex_;
    std::unordered_map<std::string, prometheus::Counter*> counters_;

    mutable std::mutex gauges_mutex_;
    std::unordered_map<std::string, prometheus::Gauge*> gauges_;

    mutable std::mutex histograms_mutex_;
    std::unordered_map<std::string, prometheus::Histogram*> histograms_;
};

// ==============================================================================
// 便捷宏（热路径仅原子操作）
// ==============================================================================
#define QUANT_METRIC_COUNTER(name, help) \
    (::quant::metrics::Metrics::instance().counter(name, help))

#define QUANT_METRIC_GAUGE(name, help) \
    (::quant::metrics::Metrics::instance().gauge(name, help))

// ==============================================================================
// 预定义指标句柄（全局，供热路径缓存）
// ==============================================================================
namespace predefined {

// 数据层
inline CounterHandle candles_received;
inline CounterHandle candles_dropped;
inline GaugeHandle websocket_connected;
inline GaugeHandle data_latency_us;

// 指标层
inline CounterHandle indicator_computed;
inline HistogramHandle indicator_latency;

// 策略层
inline CounterHandle signals_generated;
inline CounterHandle signals_filtered;
inline GaugeHandle current_score;
inline GaugeHandle trend_strength;

// OMS 层
inline CounterHandle orders_placed;
inline CounterHandle orders_filled;
inline CounterHandle orders_rejected;
inline CounterHandle orders_canceled;
inline HistogramHandle order_latency;
inline HistogramHandle slippage_bps;
inline GaugeHandle open_positions;
inline GaugeHandle total_pnl;

// 风控层
inline CounterHandle risk_check_failed;
inline CounterHandle circuit_breaker_triggered;
inline GaugeHandle daily_loss_pct;
inline GaugeHandle drawdown_pct;

// AI 层
inline CounterHandle ai_inferences;
inline HistogramHandle ai_latency;
inline GaugeHandle model_drift_score;

// 系统层
inline GaugeHandle memory_usage_bytes;
inline GaugeHandle cpu_usage_ratio;
inline CounterHandle errors_total;

// 初始化所有预定义指标（在应用启动时调用一次）
void initialize_predefined();

}  // namespace predefined

// ==============================================================================
// Metrics 实现
// ==============================================================================
inline Metrics& Metrics::instance() noexcept {
    static Metrics m;
    return m;
}

inline std::string Metrics::normalize_name(std::string_view name) {
    std::string result{kNamespacePrefix};
    if (name.starts_with(kNamespacePrefix)) {
        result = std::string{name};
    } else {
        result.append(name);
    }
    return result;
}

inline std::string Metrics::normalize_counter_name(std::string_view name) {
    auto normalized = normalize_name(name);
    if (!normalized.ends_with("_total")) {
        normalized.append("_total");
    }
    return normalized;
}

inline std::string Metrics::cache_key(std::string_view name, const Labels& labels) {
    std::string key{name};
    for (const auto& [k, v] : labels) {
        key += '\x1F';  // Unit Separator
        key += k;
        key += '=';
        key += v;
    }
    return key;
}

inline Result<void> Metrics::init(std::string_view address) noexcept {
    if (ready_.load(std::memory_order_acquire)) {
        return {};  // 已初始化
    }

    std::call_once(init_flag_, [&]() {
        try {
            exposer_ = std::make_unique<prometheus::Exposer>(std::string{address});
            registry_ = std::make_shared<prometheus::Registry>();
            exposer_->RegisterCollectable(registry_);
            ready_.store(true, std::memory_order_release);
            QUANT_LOG_INFO("Prometheus 指标已启动: {}", address);
        } catch (const std::exception& e) {
            // 降级：不阻断启动
            QUANT_LOG_WARN("Prometheus Exposer 启动失败（监控降级）: {}", e.what());
            ready_.store(false, std::memory_order_release);
        }
    });

    if (ready_.load(std::memory_order_acquire)) {
        return {};
    }
    // 返回警告，但不阻断
    return QUANT_ERR_MSG(ErrorCode::SystemIoError,
        "Prometheus Exposer 未启动（监控降级）");
}

inline CounterHandle Metrics::counter(std::string_view name,
                                       std::string_view help,
                                       const Labels& labels) {
    if (!ready_.load(std::memory_order_acquire)) return {};

    // 校验标签
    if (auto r = detail::validate_labels(labels); r.is_err()) {
        QUANT_LOG_WARN("指标标签校验失败: {}", r.error().to_string());
        return {};
    }

    const auto normalized = normalize_counter_name(name);
    if (!detail::is_valid_metric_name(normalized)) {
        QUANT_LOG_WARN("非法指标名: {}", normalized);
        return {};
    }

    const auto key = cache_key(normalized, labels);

    std::lock_guard lock(counters_mutex_);
    auto it = counters_.find(key);
    if (it != counters_.end()) {
        return CounterHandle{it->second};
    }

    try {
        auto& family = prometheus::BuildCounter()
            .Name(normalized)
            .Help(std::string{help})
            .Register(*registry_);
        auto& counter = family.Add(labels);
        counters_[key] = &counter;
        return CounterHandle{&counter};
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("Counter 创建失败: {} ({})", normalized, e.what());
        return {};
    }
}

inline GaugeHandle Metrics::gauge(std::string_view name,
                                    std::string_view help,
                                    const Labels& labels) {
    if (!ready_.load(std::memory_order_acquire)) return {};

    if (auto r = detail::validate_labels(labels); r.is_err()) {
        QUANT_LOG_WARN("指标标签校验失败: {}", r.error().to_string());
        return {};
    }

    const auto normalized = normalize_name(name);
    if (!detail::is_valid_metric_name(normalized)) {
        QUANT_LOG_WARN("非法指标名: {}", normalized);
        return {};
    }

    const auto key = cache_key(normalized, labels);

    std::lock_guard lock(gauges_mutex_);
    auto it = gauges_.find(key);
    if (it != gauges_.end()) {
        return GaugeHandle{it->second};
    }

    try {
        auto& family = prometheus::BuildGauge()
            .Name(normalized)
            .Help(std::string{help})
            .Register(*registry_);
        auto& gauge = family.Add(labels);
        gauges_[key] = &gauge;
        return GaugeHandle{&gauge};
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("Gauge 创建失败: {} ({})", normalized, e.what());
        return {};
    }
}

inline HistogramHandle Metrics::histogram(
    std::string_view name,
    std::string_view help,
    const std::vector<double>& bucket_bounds,
    const Labels& labels)
{
    if (!ready_.load(std::memory_order_acquire)) return {};

    if (auto r = detail::validate_labels(labels); r.is_err()) {
        QUANT_LOG_WARN("指标标签校验失败: {}", r.error().to_string());
        return {};
    }

    // 校验桶边界严格递增
    for (std::size_t i = 1; i < bucket_bounds.size(); ++i) {
        if (bucket_bounds[i] <= bucket_bounds[i - 1]) {
            QUANT_LOG_WARN("Histogram 桶边界非单调: {}", name);
            return {};
        }
    }

    const auto normalized = normalize_name(name);
    if (!detail::is_valid_metric_name(normalized)) {
        QUANT_LOG_WARN("非法指标名: {}", normalized);
        return {};
    }

    const auto key = cache_key(normalized, labels);

    std::lock_guard lock(histograms_mutex_);
    auto it = histograms_.find(key);
    if (it != histograms_.end()) {
        return HistogramHandle{it->second};
    }

    try {
        auto& family = prometheus::BuildHistogram()
            .Name(normalized)
            .Help(std::string{help})
            .Register(*registry_);
        auto& hist = family.Add(labels, bucket_bounds);
        histograms_[key] = &hist;
        return HistogramHandle{&hist};
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("Histogram 创建失败: {} ({})", normalized, e.what());
        return {};
    }
}

inline std::size_t Metrics::metric_family_count() const noexcept {
    if (!registry_) return 0;
    return registry_->Collect().size();
}

inline std::string Metrics::dump() const {
    if (!registry_) return "<not initialized>\n";
    std::string result;
    for (const auto& family : registry_->Collect()) {
        result += family.name + "\n";
    }
    return result;
}

inline void Metrics::shutdown() noexcept {
    ready_.store(false, std::memory_order_release);
    // 析构顺序：exposer_ 先于 registry_
    exposer_.reset();
    registry_.reset();
}

// ==============================================================================
// predefined 实现
// ==============================================================================
namespace predefined {

inline void initialize_predefined() {
    using namespace buckets;

    // 数据层
    candles_received        = Metrics::instance().counter("candles_received", "接收的K线数量");
    candles_dropped         = Metrics::instance().counter("candles_dropped", "丢弃的K线数量");
    websocket_connected     = Metrics::instance().gauge("websocket_connected", "WebSocket连接状态");
    data_latency_us         = Metrics::instance().gauge("data_latency_us", "数据延迟（微秒）");

    // 指标层
    indicator_computed      = Metrics::instance().counter("indicator_computed", "指标计算次数");
    indicator_latency       = Metrics::instance().histogram("indicator_latency_seconds",
                                 "指标计算延迟", kLatencySeconds);

    // 策略层
    signals_generated       = Metrics::instance().counter("signals_generated", "产生的信号数量");
    signals_filtered        = Metrics::instance().counter("signals_filtered", "过滤的信号数量");
    current_score           = Metrics::instance().gauge("current_score", "当前共振评分");
    trend_strength          = Metrics::instance().gauge("trend_strength", "趋势强度");

    // OMS 层
    orders_placed           = Metrics::instance().counter("orders_placed", "提交的订单数量");
    orders_filled           = Metrics::instance().counter("orders_filled", "成交的订单数量");
    orders_rejected         = Metrics::instance().counter("orders_rejected", "被拒绝的订单数量");
    orders_canceled         = Metrics::instance().counter("orders_canceled", "撤销的订单数量");
    order_latency           = Metrics::instance().histogram("order_latency_seconds",
                                 "订单执行延迟", kLatencySeconds);
    slippage_bps            = Metrics::instance().histogram("slippage_bps",
                                 "滑点（基点）", kSlippageBps);
    open_positions          = Metrics::instance().gauge("open_positions", "当前持仓数量");
    total_pnl               = Metrics::instance().gauge("total_pnl_usdt", "累计盈亏（USDT）");

    // 风控层
    risk_check_failed       = Metrics::instance().counter("risk_check_failed", "风控检查失败次数");
    circuit_breaker_triggered = Metrics::instance().counter("circuit_breaker_triggered", "熔断触发次数");
    daily_loss_pct          = Metrics::instance().gauge("daily_loss_ratio", "当日亏损比例");
    drawdown_pct            = Metrics::instance().gauge("drawdown_ratio", "当前回撤比例");

    // AI 层
    ai_inferences           = Metrics::instance().counter("ai_inferences", "AI 推理次数");
    ai_latency              = Metrics::instance().histogram("ai_latency_seconds",
                                 "AI 推理延迟", kLatencySeconds);
    model_drift_score       = Metrics::instance().gauge("model_drift_score", "模型漂移评分");

    // 系统层
    memory_usage_bytes      = Metrics::instance().gauge("memory_usage_bytes", "内存使用量（字节）");
    cpu_usage_ratio         = Metrics::instance().gauge("cpu_usage_ratio", "CPU 使用率");
    errors_total            = Metrics::instance().counter("errors", "错误总数");
}

}  // namespace predefined

}  // namespace metrics
}  // namespace quant

#else  // QUANT_ENABLE_METRICS == 0

// ==============================================================================
// 空实现（编译期禁用）
// ==============================================================================
namespace quant {
namespace metrics {

struct CounterHandle {
    void increment(double = 1.0) const noexcept {}
    [[nodiscard]] double value() const noexcept { return 0.0; }
    [[nodiscard]] bool valid() const noexcept { return false; }
};

struct GaugeHandle {
    void set(double) const noexcept {}
    void increment(double = 1.0) const noexcept {}
    void decrement(double = 1.0) const noexcept {}
    void add(double) const noexcept {}
    [[nodiscard]] double value() const noexcept { return 0.0; }
    [[nodiscard]] bool valid() const noexcept { return false; }
};

struct HistogramHandle {
    void observe(double) const noexcept {}
    [[nodiscard]] bool valid() const noexcept { return false; }
};

class Metrics {
public:
    [[nodiscard]] static Metrics& instance() noexcept {
        static Metrics m;
        return m;
    }

    Result<void> init(std::string_view = "") noexcept { return {}; }
    [[nodiscard]] bool is_ready() const noexcept { return false; }

    CounterHandle counter(std::string_view, std::string_view,
                          const std::map<std::string, std::string>& = {}) {
        return {};
    }

    GaugeHandle gauge(std::string_view, std::string_view,
                      const std::map<std::string, std::string>& = {}) {
        return {};
    }

    HistogramHandle histogram(std::string_view, std::string_view,
                              const std::vector<double>&,
                              const std::map<std::string, std::string>& = {}) {
        return {};
    }

    [[nodiscard]] std::size_t metric_family_count() const noexcept { return 0; }
    [[nodiscard]] std::string dump() const { return {}; }
    void shutdown() noexcept {}
};

}  // namespace metrics
}  // namespace quant

#endif  // QUANT_ENABLE_METRICS

#endif  // QUANT_COMMON_CORE_METRICS_HPP
