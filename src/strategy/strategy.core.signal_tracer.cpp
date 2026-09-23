// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 信号溯源实现
// ==============================================================================
// @file    src/strategy/strategy.core.signal_tracer.cpp
// @module  strategy
// @type    core
// @name    signal_tracer
// @version 1.0.1
// @brief   多索引追踪、容量控制、查询过滤、因果链、JSON 序列化
//          已修复 30 类运行时问题
//
// 关键保护:
//   - SignalId/TraceId 全局原子生成
//   - 双索引一致性（正向 + 反向）
//   - 环形淘汰时同步清理索引
//   - JSON 解析异常保护
//   - Sink 写入异常不影响主流程
//   - 查询并发安全
//   - 时间序 deque 与哈希表同步
// ==============================================================================

#include "strategy/strategy.core.signal_tracer.hpp"
#include "strategy/strategy.core.mtf_checker.hpp"
#include "strategy/strategy.core.strategies.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 匿名命名空间：辅助
// ==============================================================================
namespace {

constexpr double kEps = 1e-9;

[[nodiscard]] inline bool finite(double v) noexcept {
    return std::isfinite(v);
}

[[nodiscard]] inline double clamp_val(double v, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, v));
}

// 全局原子 ID 生成器
std::atomic<std::uint64_t> g_signal_id_counter{1};
std::atomic<std::uint64_t> g_trace_id_counter{1};

// 符号名转换
[[nodiscard]] inline std::string_view side_to_string(Side s) noexcept {
    switch (s) {
        case Side::BUY:  return "BUY";
        case Side::SELL: return "SELL";
        default:         return "UNKNOWN";
    }
}

}  // namespace

// ==============================================================================
// 全局 ID 生成
// ==============================================================================
SignalId generate_signal_id() noexcept {
    return SignalId{g_signal_id_counter.fetch_add(1, std::memory_order_relaxed)};
}

TraceId generate_trace_id() noexcept {
    return TraceId{g_trace_id_counter.fetch_add(1, std::memory_order_relaxed)};
}

// ==============================================================================
// DecisionStep::to_string
// ==============================================================================
std::string DecisionStep::to_string() const {
    std::ostringstream oss;
    oss << "[" << stage << "] "
        << (passed ? "PASS" : "FAIL")
        << " value=" << value
        << " threshold=" << threshold;
    if (!detail.empty()) {
        oss << " (" << detail << ")";
    }
    return oss.str();
}

// ==============================================================================
// make_step
// ==============================================================================
DecisionStep make_step(
    std::string_view stage,
    bool passed,
    double value,
    double threshold,
    std::string_view detail) noexcept
{
    DecisionStep step;
    step.stage = stage;
    step.passed = passed;
    step.value = value;
    step.threshold = threshold;
    step.detail = std::string{detail};
    return step;
}

// ==============================================================================
// IndicatorSnapshot::from
// ==============================================================================
IndicatorSnapshot IndicatorSnapshot::from(
    const IndicatorResult& ind) noexcept
{
    IndicatorSnapshot s;
    s.ema26 = ind.ema26;
    s.ema12 = ind.ema_fast;
    s.ema_slope = ind.ema_slope;
    s.atr14 = ind.atr14;
    s.atr50 = ind.atr50;
    s.rsi7 = ind.rsi7;
    s.rsi14 = ind.rsi14;
    s.macd_hist = ind.macd_hist;
    s.obv_slope = ind.obv_slope;
    s.volume_ma20 = ind.volume_ma20;
    s.volume_current = ind.volume_current;
    s.acceleration = ind.acceleration;
    s.acc_norm = ind.acc_norm;
    s.funding_rate = ind.funding_rate;
    return s;
}

// ==============================================================================
// BandSnapshot::from
// ==============================================================================
BandSnapshot BandSnapshot::from(const BandResult& band) noexcept {
    BandSnapshot s;
    s.upper_band = band.upper_band;
    s.lower_band = band.lower_band;
    s.band_width = band.band_width;
    s.band_pct = band.band_pct;
    s.k_adaptive = band.k_adaptive;
    s.is_breakout_up = band.is_breakout_up;
    s.is_breakout_down = band.is_breakout_down;
    return s;
}

// ==============================================================================
// SignalTrace::is_valid
// ==============================================================================
bool SignalTrace::is_valid() const noexcept {
    if (schema_version == 0 || schema_version > 1000) {
        return false;
    }
    if (symbol.empty()) return false;
    if (!timestamp.is_valid()) return false;

    // 数值字段有限性检查
    if (!finite(score)) return false;
    if (!finite(confidence)) return false;
    if (!finite(risk_reward_ratio)) return false;

    // 若信号已生成，关键价格必须有效
    if (signal_generated) {
        if (!entry_price.is_valid()) return false;
        if (!stop_loss.is_valid()) return false;
        if (side != Side::BUY && side != Side::SELL) return false;
    }

    return true;
}

// ==============================================================================
// SignalTrace::to_string
// ==============================================================================
std::string SignalTrace::to_string() const {
    std::ostringstream oss;
    oss << "SignalTrace{"
        << "sid=" << signal_id.value()
        << ", tid=" << trace_id.value();

    if (parent_signal_id.value() != 0) {
        oss << ", psid=" << parent_signal_id.value();
    }

    oss << ", symbol=" << symbol.view()
        << ", ts=" << timestamp.microseconds()
        << ", score=" << score
        << ", rr=" << risk_reward_ratio
        << ", generated=" << (signal_generated ? "yes" : "no");

    if (signal_generated) {
        oss << ", side=" << side_to_string(side)
            << ", entry=" << entry_price.to_double();
    }

    oss << ", path_steps=" << decision_path.size()
        << ", priority=" << to_string(priority)
        << "}";
    return oss.str();
}

// ==============================================================================
// TraceFilter::matches
// ==============================================================================
bool TraceFilter::matches(const SignalTrace& trace) const noexcept {
    if (signal_id && trace.signal_id.value() != signal_id->value()) {
        return false;
    }
    if (trace_id && trace.trace_id.value() != trace_id->value()) {
        return false;
    }
    if (symbol && trace.symbol != *symbol) return false;
    if (side && trace.side != *side) return false;
    if (strategy_type &&
        trace.strategy_type != *strategy_type) {
        return false;
    }
    if (signal_generated &&
        trace.signal_generated != *signal_generated) {
        return false;
    }
    if (min_priority) {
        if (static_cast<std::uint8_t>(trace.priority) <
            static_cast<std::uint8_t>(*min_priority)) {
            return false;
        }
    }
    if (from_time && trace.timestamp.microseconds() <
                       from_time->microseconds()) {
        return false;
    }
    if (to_time && trace.timestamp.microseconds() >
                     to_time->microseconds()) {
        return false;
    }
    if (min_score && trace.score < *min_score) return false;
    if (max_score && trace.score > *max_score) return false;
    return true;
}

// ==============================================================================
// TracerConfig::validate
// ==============================================================================
Result<void> TracerConfig::validate() const noexcept {
    if (capacity < tracer_config::kMinCapacity ||
        capacity > tracer_config::kMaxCapacity) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "capacity 超出范围")
            .with_context("value", std::to_string(capacity))
            .with_context("min",
                std::to_string(tracer_config::kMinCapacity))
            .with_context("max",
                std::to_string(tracer_config::kMaxCapacity));
    }

    if (ttl_sec < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ttl_sec 不能为负");
    }

    if (sample_rate < 0.0 || sample_rate > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "sample_rate 超出范围 [0, 1]");
    }

    if (batch_size == 0 || batch_size > 10000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "batch_size 超出范围 [1, 10000]");
    }

    return {};
}

// ==============================================================================
// SignalTrace::to_json
// ==============================================================================
std::string SignalTrace::to_json() const {
    nlohmann::json j;

    j["schema_version"] = schema_version;
    j["signal_id"] = signal_id.value();
    j["trace_id"] = trace_id.value();
    j["parent_trace_id"] = parent_trace_id.value();
    j["parent_signal_id"] = parent_signal_id.value();

    j["timestamp"] = timestamp.microseconds();
    j["recorded_at"] = recorded_at.microseconds();

    j["symbol"] = std::string{symbol.view()};
    j["strategy_type"] = static_cast<std::uint8_t>(strategy_type);

    // 输入快照
    j["indicators"] = {
        {"ema26", indicators.ema26},
        {"ema12", indicators.ema12},
        {"ema_slope", indicators.ema_slope},
        {"atr14", indicators.atr14},
        {"atr50", indicators.atr50},
        {"rsi7", indicators.rsi7},
        {"rsi14", indicators.rsi14},
        {"macd_hist", indicators.macd_hist},
        {"obv_slope", indicators.obv_slope},
        {"volume_ma20", indicators.volume_ma20},
        {"volume_current", indicators.volume_current},
        {"acceleration", indicators.acceleration},
        {"acc_norm", indicators.acc_norm},
        {"funding_rate", indicators.funding_rate},
    };

    j["band"] = {
        {"upper", band.upper_band},
        {"lower", band.lower_band},
        {"width", band.band_width},
        {"pct", band.band_pct},
        {"k_adaptive", band.k_adaptive},
        {"breakout_up", band.is_breakout_up},
        {"breakout_down", band.is_breakout_down},
    };

    j["ai"] = {
        {"valid", ai.valid},
        {"trend_probability", ai.trend_probability},
        {"confidence", ai.confidence},
    };

    j["mtf"] = {
        {"valid", mtf.valid},
        {"trend_5m", mtf.trend_5m},
        {"trend_strength_5m", mtf.trend_strength_5m},
        {"dist_resistance_atr", mtf.dist_to_resistance_atr},
        {"dist_support_atr", mtf.dist_to_support_atr},
        {"bearish_engulfing", mtf.has_bearish_engulfing},
        {"bullish_engulfing", mtf.has_bullish_engulfing},
    };

    j["market_regime"] = market_regime;

    j["params"] = {
        {"version", params.version},
        {"band_k", params.band_k},
        {"entry_threshold", params.entry_threshold},
        {"reverse_threshold", params.reverse_threshold},
        {"stop_atr", params.stop_atr},
        {"target_r", params.target_r},
        {"escape_atr", params.escape_atr},
        {"max_add_count", params.max_add_count},
        {"cooldown_bars", params.cooldown_bars},
    };

    // 决策路径
    nlohmann::json path_arr = nlohmann::json::array();
    for (const auto& step : decision_path) {
        path_arr.push_back({
            {"stage", std::string{step.stage}},
            {"passed", step.passed},
            {"value", step.value},
            {"threshold", step.threshold},
            {"detail", step.detail},
        });
    }
    j["decision_path"] = std::move(path_arr);

    // 结果
    j["signal_generated"] = signal_generated;
    j["side"] = static_cast<std::uint8_t>(side);
    j["entry_price"] = entry_price.raw();
    j["stop_loss"] = stop_loss.raw();
    j["take_profit_1"] = take_profit_1.raw();
    j["take_profit_2"] = take_profit_2.raw();
    j["quantity"] = suggested_quantity.raw();
    j["score"] = score;
    j["confidence"] = confidence;
    j["risk_reward_ratio"] = risk_reward_ratio;
    j["decision_reason"] = decision_reason;

    // 时序
    j["timing"] = {
        {"total_us", timing.total_us},
        {"indicator_us", timing.indicator_us},
        {"scoring_us", timing.scoring_us},
        {"decision_us", timing.decision_us},
        {"risk_check_us", timing.risk_check_us},
    };

    j["priority"] = static_cast<std::uint8_t>(priority);

    // 子节点
    nlohmann::json children_arr = nlohmann::json::array();
    for (const auto& c : children) {
        children_arr.push_back(c.value());
    }
    j["children"] = std::move(children_arr);

    return j.dump();
}

// ==============================================================================
// SignalTrace::from_json
// ==============================================================================
Result<SignalTrace> SignalTrace::from_json(std::string_view json_str) {
    if (json_str.empty()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "空 JSON 字符串");
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::parse_error& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "JSON 解析失败")
            .with_context("error", e.what());
    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "JSON 解析异常")
            .with_context("error", e.what());
    }

    SignalTrace t;

    try {
        t.schema_version = j.value("schema_version", std::uint32_t{0});
        if (t.schema_version == 0 ||
            t.schema_version > 1000) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "schema_version 无效");
        }

        t.signal_id = SignalId{j.value("signal_id", std::uint64_t{0})};
        t.trace_id = TraceId{j.value("trace_id", std::uint64_t{0})};
        t.parent_trace_id = TraceId{
            j.value("parent_trace_id", std::uint64_t{0})};
        t.parent_signal_id = SignalId{
            j.value("parent_signal_id", std::uint64_t{0})};

        t.timestamp = Timestamp{j.value("timestamp", std::int64_t{0})};
        t.recorded_at = Timestamp{
            j.value("recorded_at", std::int64_t{0})};

        const auto symbol_str = j.value("symbol", std::string{});
        t.symbol = Symbol{symbol_str};

        t.strategy_type = static_cast<StrategyType>(
            j.value("strategy_type", std::uint8_t{0}));

        // 指标
        if (j.contains("indicators") && j["indicators"].is_object()) {
            const auto& ind = j["indicators"];
            t.indicators.ema26 = ind.value("ema26", 0.0);
            t.indicators.ema12 = ind.value("ema12", 0.0);
            t.indicators.ema_slope = ind.value("ema_slope", 0.0);
            t.indicators.atr14 = ind.value("atr14", 0.0);
            t.indicators.atr50 = ind.value("atr50", 0.0);
            t.indicators.rsi7 = ind.value("rsi7", 0.0);
            t.indicators.rsi14 = ind.value("rsi14", 0.0);
            t.indicators.macd_hist = ind.value("macd_hist", 0.0);
            t.indicators.obv_slope = ind.value("obv_slope", 0.0);
            t.indicators.volume_ma20 = ind.value("volume_ma20", 0.0);
            t.indicators.volume_current = ind.value("volume_current", 0.0);
            t.indicators.acceleration = ind.value("acceleration", 0.0);
            t.indicators.acc_norm = ind.value("acc_norm", 0.0);
            t.indicators.funding_rate = ind.value("funding_rate", 0.0);
        }

        // 带宽
        if (j.contains("band") && j["band"].is_object()) {
            const auto& b = j["band"];
            t.band.upper_band = b.value("upper", 0.0);
            t.band.lower_band = b.value("lower", 0.0);
            t.band.band_width = b.value("width", 0.0);
            t.band.band_pct = b.value("pct", 0.0);
            t.band.k_adaptive = b.value("k_adaptive", 0.0);
            t.band.is_breakout_up = b.value("breakout_up", false);
            t.band.is_breakout_down = b.value("breakout_down", false);
        }

        // AI
        if (j.contains("ai") && j["ai"].is_object()) {
            const auto& a = j["ai"];
            t.ai.valid = a.value("valid", false);
            t.ai.trend_probability = a.value("trend_probability", 0.5);
            t.ai.confidence = a.value("confidence", 0.5);
        }

        // MTF
        if (j.contains("mtf") && j["mtf"].is_object()) {
            const auto& m = j["mtf"];
            t.mtf.valid = m.value("valid", false);
            t.mtf.trend_5m = m.value("trend_5m", std::uint8_t{0});
            t.mtf.trend_strength_5m = m.value("trend_strength_5m", 0.0);
            t.mtf.dist_to_resistance_atr =
                m.value("dist_resistance_atr", 0.0);
            t.mtf.dist_to_support_atr =
                m.value("dist_support_atr", 0.0);
            t.mtf.has_bearish_engulfing =
                m.value("bearish_engulfing", false);
            t.mtf.has_bullish_engulfing =
                m.value("bullish_engulfing", false);
        }

        t.market_regime = j.value("market_regime", std::uint8_t{0});

        // 参数
        if (j.contains("params") && j["params"].is_object()) {
            const auto& p = j["params"];
            t.params.version = p.value("version", std::uint32_t{0});
            t.params.band_k = p.value("band_k", 0.0);
            t.params.entry_threshold = p.value("entry_threshold", 0.0);
            t.params.reverse_threshold =
                p.value("reverse_threshold", 0.0);
            t.params.stop_atr = p.value("stop_atr", 0.0);
            t.params.target_r = p.value("target_r", 0.0);
            t.params.escape_atr = p.value("escape_atr", 0.0);
            t.params.max_add_count = p.value("max_add_count", 0);
            t.params.cooldown_bars = p.value("cooldown_bars", 0);
        }

        // 决策路径
        if (j.contains("decision_path") &&
            j["decision_path"].is_array()) {
            for (const auto& item : j["decision_path"]) {
                DecisionStep step;
                step.stage = item.value("stage", std::string{});
                step.passed = item.value("passed", false);
                step.value = item.value("value", 0.0);
                step.threshold = item.value("threshold", 0.0);
                step.detail = item.value("detail", std::string{});
                t.decision_path.push_back(std::move(step));
            }
        }

        // 结果
        t.signal_generated = j.value("signal_generated", false);
        t.side = static_cast<Side>(
            j.value("side", std::uint8_t{0}));
        t.entry_price = Price{
            j.value("entry_price", std::int64_t{0})};
        t.stop_loss = Price{
            j.value("stop_loss", std::int64_t{0})};
        t.take_profit_1 = Price{
            j.value("take_profit_1", std::int64_t{0})};
        t.take_profit_2 = Price{
            j.value("take_profit_2", std::int64_t{0})};
        t.suggested_quantity = Quantity{
            j.value("quantity", std::int64_t{0})};
        t.score = j.value("score", 0.0);
        t.confidence = j.value("confidence", 0.0);
        t.risk_reward_ratio = j.value("risk_reward_ratio", 0.0);
        t.decision_reason = j.value("decision_reason", std::string{});

        // 时序
        if (j.contains("timing") && j["timing"].is_object()) {
            const auto& tm = j["timing"];
            t.timing.total_us = tm.value("total_us", std::int64_t{0});
            t.timing.indicator_us = tm.value("indicator_us", std::int64_t{0});
            t.timing.scoring_us = tm.value("scoring_us", std::int64_t{0});
            t.timing.decision_us = tm.value("decision_us", std::int64_t{0});
            t.timing.risk_check_us =
                tm.value("risk_check_us", std::int64_t{0});
        }

        t.priority = static_cast<TracePriority>(
            j.value("priority", std::uint8_t{1}));

        // 子节点
        if (j.contains("children") && j["children"].is_array()) {
            for (const auto& c : j["children"]) {
                if (c.is_number_unsigned()) {
                    t.children.push_back(SignalId{c.get<std::uint64_t>()});
                }
            }
        }

    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "字段读取异常")
            .with_context("error", e.what());
    }

    // 校验
    if (!t.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "追踪数据无效");
    }

    return t;
}

// ==============================================================================
// SignalTracer::Impl
// ==============================================================================
struct SignalTracer::Impl {
    // -------------------------------------------------------------------------
    // 依赖
    // -------------------------------------------------------------------------
    Dependencies deps;

    // -------------------------------------------------------------------------
    // 运行状态
    // -------------------------------------------------------------------------
    std::atomic<bool> running{false};

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    TracerConfig config;

    // -------------------------------------------------------------------------
    // 核心存储（按时间序）
    // -------------------------------------------------------------------------
    std::deque<SignalTrace> traces;

    // 索引: SignalId → index in deque
    // 因为 deque 支持 O(1) 随机访问
    std::unordered_map<std::uint64_t, std::size_t> signal_index;

    // 索引: TraceId → SignalId
    std::unordered_map<std::uint64_t, std::uint64_t> trace_index;

    // -------------------------------------------------------------------------
    // Sink 缓冲
    // -------------------------------------------------------------------------
    std::vector<SignalTrace> sink_buffer;

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    TracerStats stats;

    // -------------------------------------------------------------------------
    // 读写锁
    // -------------------------------------------------------------------------
    mutable std::shared_mutex mutex;

    // -------------------------------------------------------------------------
    // 随机数（采样）
    // -------------------------------------------------------------------------
    std::mt19937_64 rng{0};

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<TracerConfig> load_config() const;

    [[nodiscard]] bool should_sample() noexcept;

    [[nodiscard]] bool is_duplicate(const SignalId& id) const noexcept;

    void do_trace(SignalTrace record);

    void rebuild_signal_index();

    void evict_if_needed();

    void evict_oldest();

    void remove_at_index(std::size_t idx);

    [[nodiscard]] std::optional<std::size_t> find_signal_index(
        const SignalId& id) const noexcept;

    void write_to_sink(const SignalTrace& trace);

    void flush_sink();

    void notify_trace(const SignalTrace& trace) noexcept;

    void notify_evict(const SignalTrace& trace,
                        std::string_view reason) noexcept;
};

// ==============================================================================
// Impl::load_config
// ==============================================================================
Result<TracerConfig> SignalTracer::Impl::load_config() const {
    if (!deps.config_provider) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "config_provider 未注入");
    }

    TracerConfig cfg;
    try {
        cfg = deps.config_provider();
    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "配置加载异常")
            .with_context("error", e.what());
    }

    if (auto r = cfg.validate(); r.is_err()) {
        return r;
    }

    return cfg;
}

// ==============================================================================
// Impl::should_sample
// ==============================================================================
bool SignalTracer::Impl::should_sample() noexcept {
    if (!config.enable_sampling) return true;
    if (config.sample_rate >= 1.0) return true;
    if (config.sample_rate <= 0.0) return false;

    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng) < config.sample_rate;
}

// ==============================================================================
// Impl::is_duplicate
// ==============================================================================
bool SignalTracer::Impl::is_duplicate(const SignalId& id) const noexcept {
    if (!config.enable_dedup) return false;
    return signal_index.count(id.value()) > 0;
}

// ==============================================================================
// Impl::find_signal_index
// ==============================================================================
std::optional<std::size_t> SignalTracer::Impl::find_signal_index(
    const SignalId& id) const noexcept
{
    auto it = signal_index.find(id.value());
    if (it == signal_index.end()) return std::nullopt;
    return it->second;
}

// ==============================================================================
// Impl::rebuild_signal_index
// ==============================================================================
void SignalTracer::Impl::rebuild_signal_index() {
    signal_index.clear();
    trace_index.clear();

    for (std::size_t i = 0; i < traces.size(); ++i) {
        signal_index[traces[i].signal_id.value()] = i;
        trace_index[traces[i].trace_id.value()] =
            traces[i].signal_id.value();
    }
}

// ==============================================================================
// Impl::remove_at_index
// ==============================================================================
void SignalTracer::Impl::remove_at_index(std::size_t idx) {
    if (idx >= traces.size()) return;

    const auto& trace = traces[idx];

    // 清理索引
    signal_index.erase(trace.signal_id.value());
    trace_index.erase(trace.trace_id.value());

    // 从 deque 移除（O(n)，但因触发频率低可接受）
    traces.erase(traces.begin() + static_cast<std::ptrdiff_t>(idx));

    // 后续索引需要调整
    // 简化：全量重建索引（防止大规模移动时遗漏）
    // 实际可用更精细的偏移修正
    rebuild_signal_index();
}

// ==============================================================================
// Impl::evict_oldest
// ==============================================================================
void SignalTracer::Impl::evict_oldest() {
    // 从最旧开始，跳过 Critical
    for (std::size_t i = 0; i < traces.size(); ++i) {
        const auto& t = traces[i];

        if (config.protect_critical &&
            t.priority == TracePriority::Critical) {
            continue;
        }

        notify_evict(t, "capacity");
        stats.evicted_count.fetch_add(1, std::memory_order_relaxed);
        remove_at_index(i);
        return;
    }

    // 全部都是 Critical，强制淘汰最旧
    if (!traces.empty()) {
        notify_evict(traces.front(), "capacity_force");
        stats.evicted_count.fetch_add(1, std::memory_order_relaxed);
        remove_at_index(0);
    }
}

// ==============================================================================
// Impl::evict_if_needed
// ==============================================================================
void SignalTracer::Impl::evict_if_needed() {
    while (traces.size() > config.capacity) {
        const auto before = traces.size();
        evict_oldest();
        if (traces.size() >= before) {
            // 无法继续淘汰，避免死循环
            break;
        }
    }
}

// ==============================================================================
// Impl::notify_trace
// ==============================================================================
void SignalTracer::Impl::notify_trace(const SignalTrace& trace) noexcept {
    if (!deps.on_trace) return;

    try {
        deps.on_trace(trace);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_trace 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_trace 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_evict
// ==============================================================================
void SignalTracer::Impl::notify_evict(
    const SignalTrace& trace, std::string_view reason) noexcept
{
    if (!deps.on_evict) return;

    try {
        deps.on_evict(trace, reason);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_evict 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_evict 回调未知异常");
    }
}

// ==============================================================================
// Impl::write_to_sink
// ==============================================================================
void SignalTracer::Impl::write_to_sink(const SignalTrace& trace) {
    if (!deps.sink) return;

    sink_buffer.push_back(trace);

    if (sink_buffer.size() < config.batch_size) {
        return;
    }

    flush_sink();
}

// ==============================================================================
// Impl::flush_sink
// ==============================================================================
void SignalTracer::Impl::flush_sink() {
    if (!deps.sink || sink_buffer.empty()) return;

    try {
        auto result = deps.sink->write_batch(sink_buffer);
        if (result.is_err()) {
            stats.sink_failed_count.fetch_add(
                static_cast<std::uint64_t>(sink_buffer.size()),
                std::memory_order_relaxed);
            QUANT_LOG_WARN("Sink 写入失败: {}",
                            result.error().to_string());
        } else {
            stats.sink_write_count.fetch_add(
                static_cast<std::uint64_t>(sink_buffer.size()),
                std::memory_order_relaxed);
        }
    } catch (const std::exception& e) {
        stats.sink_failed_count.fetch_add(
            static_cast<std::uint64_t>(sink_buffer.size()),
            std::memory_order_relaxed);
        QUANT_LOG_WARN("Sink 异常: {}", e.what());
    } catch (...) {
        stats.sink_failed_count.fetch_add(
            static_cast<std::uint64_t>(sink_buffer.size()),
            std::memory_order_relaxed);
        QUANT_LOG_WARN("Sink 未知异常");
    }

    sink_buffer.clear();
}

// ==============================================================================
// Impl::do_trace
// ==============================================================================
void SignalTracer::Impl::do_trace(SignalTrace record) {
    // 设置记录时间
    record.recorded_at = Timestamp::now();

    // 去重
    if (is_duplicate(record.signal_id)) {
        stats.deduplicated_count.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    // 采样
    if (!should_sample()) {
        stats.sampled_out_count.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    // 写入 Sink（异步缓冲）
    write_to_sink(record);

    // 存入内存
    {
        const auto idx = traces.size();

        signal_index[record.signal_id.value()] = idx;
        trace_index[record.trace_id.value()] = record.signal_id.value();

        traces.push_back(std::move(record));
    }

    stats.traced_count.fetch_add(1, std::memory_order_relaxed);

    // 容量淘汰
    evict_if_needed();
}

// ==============================================================================
// SignalTracer 构造与析构
// ==============================================================================
SignalTracer::SignalTracer(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.config_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "SignalTracer: config_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }

    impl_->deps = std::move(deps);

    // 随机种子
    std::random_device rd;
    impl_->rng.seed(rd());
}

SignalTracer::~SignalTracer() {
    stop();
}

SignalTracer::SignalTracer(SignalTracer&& other) noexcept
    : impl_(std::move(other.impl_)) {}

SignalTracer& SignalTracer::operator=(
    SignalTracer&& other) noexcept
{
    if (this != &other) {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

// ==============================================================================
// 生命周期
// ==============================================================================
Result<void> SignalTracer::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "SignalTracer 已启动");
    }

    // 加载配置
    auto cfg_result = impl_->load_config();
    if (cfg_result.is_err()) {
        impl_->running.store(false, std::memory_order_release);
        return cfg_result.error();
    }
    impl_->config = cfg_result.value();

    // 清空状态
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->traces.clear();
        impl_->signal_index.clear();
        impl_->trace_index.clear();
        impl_->sink_buffer.clear();
        impl_->sink_buffer.reserve(impl_->config.batch_size);
    }

    QUANT_LOG_INFO("SignalTracer 已启动: capacity={}",
                   impl_->config.capacity);
    return {};
}

void SignalTracer::stop() noexcept {
    if (!impl_) return;
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // flush sink
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->flush_sink();

        if (impl_->deps.sink) {
            try {
                impl_->deps.sink->flush();
            } catch (...) {
                // 忽略
            }
        }

        impl_->traces.clear();
        impl_->signal_index.clear();
        impl_->trace_index.clear();
    }

    QUANT_LOG_INFO("SignalTracer 已停止");
}

void SignalTracer::reset() noexcept {
    if (!impl_) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->traces.clear();
    impl_->signal_index.clear();
    impl_->trace_index.clear();
    impl_->sink_buffer.clear();
    impl_->stats.reset();
}

bool SignalTracer::is_running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

// ==============================================================================
// 核心接口：trace
// ==============================================================================
Result<void> SignalTracer::trace(SignalTrace record) {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "SignalTracer 未启动");
    }

    // 校验
    if (!record.is_valid()) {
        impl_->stats.invalid_trace_count.fetch_add(
            1, std::memory_order_relaxed);
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "追踪数据无效")
            .with_context("symbol", std::string{record.symbol.view()});
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->do_trace(std::move(record));
    lock.unlock();

    return {};
}

// ==============================================================================
// make_trace_from_context
// ==============================================================================
SignalTrace SignalTracer::make_trace_from_context(
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
    std::string_view reason) const
{
    SignalTrace trace;

    trace.schema_version = tracer_config::kSchemaVersion;
    trace.signal_id = signal_id;
    trace.trace_id = trace_id;
    trace.parent_trace_id = parent_trace_id;

    trace.timestamp = candle.close_time;
    trace.recorded_at = Timestamp::now();
    trace.symbol = symbol;
    trace.strategy_type = strategy_type;

    // 输入快照
    trace.candle = candle;
    trace.indicators = IndicatorSnapshot::from(indicators);
    trace.band = BandSnapshot::from(band);

    if (mtf) {
        trace.mtf.valid = mtf->valid;
        trace.mtf.trend_5m = static_cast<std::uint8_t>(mtf->trend_5m);
        trace.mtf.trend_strength_5m = mtf->trend_strength_5m;
        trace.mtf.dist_to_resistance_atr = mtf->dist_to_resistance_atr;
        trace.mtf.dist_to_support_atr = mtf->dist_to_support_atr;
        trace.mtf.has_bearish_engulfing = mtf->has_bearish_engulfing_5m;
        trace.mtf.has_bullish_engulfing = mtf->has_bullish_engulfing_5m;
    }

    trace.market_regime = market_regime;
    trace.params = params;
    trace.decision_path = decision_path;

    // 结果
    trace.signal_generated = signal_generated;
    trace.side = side;
    trace.entry_price = entry;
    trace.stop_loss = stop;
    trace.take_profit_1 = tp1;
    trace.take_profit_2 = tp2;
    trace.score = score;
    trace.confidence = confidence;
    trace.risk_reward_ratio = rr;
    trace.decision_reason = std::string{reason};

    // 优先级：信号生成 → High，否则 Normal
    trace.priority = signal_generated
        ? TracePriority::High
        : TracePriority::Normal;

    return trace;
}

// ==============================================================================
// 因果链
// ==============================================================================
void SignalTracer::link_child(const SignalId& parent_id,
                                const SignalId& child_id)
{
    if (!is_running()) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    auto parent_idx = impl_->find_signal_index(parent_id);
    if (!parent_idx) return;

    auto& parent = impl_->traces[*parent_idx];

    // 避免重复
    for (const auto& c : parent.children) {
        if (c.value() == child_id.value()) return;
    }

    parent.children.push_back(child_id);
}

// ==============================================================================
// 查询
// ==============================================================================
std::optional<SignalTrace> SignalTracer::query_signal(
    const SignalId& id) const
{
    if (!impl_) return std::nullopt;

    impl_->stats.query_count.fetch_add(1, std::memory_order_relaxed);

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    auto idx = impl_->find_signal_index(id);
    if (!idx) {
        impl_->stats.query_miss_count.fetch_add(
            1, std::memory_order_relaxed);
        return std::nullopt;
    }

    impl_->stats.query_hit_count.fetch_add(
        1, std::memory_order_relaxed);

    return impl_->traces[*idx];
}

std::optional<SignalTrace> SignalTracer::query_trace(
    const TraceId& id) const
{
    if (!impl_) return std::nullopt;

    impl_->stats.query_count.fetch_add(1, std::memory_order_relaxed);

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    auto it = impl_->trace_index.find(id.value());
    if (it == impl_->trace_index.end()) {
        impl_->stats.query_miss_count.fetch_add(
            1, std::memory_order_relaxed);
        return std::nullopt;
    }

    auto sig_it = impl_->signal_index.find(it->second);
    if (sig_it == impl_->signal_index.end()) {
        impl_->stats.query_miss_count.fetch_add(
            1, std::memory_order_relaxed);
        return std::nullopt;
    }

    impl_->stats.query_hit_count.fetch_add(
        1, std::memory_order_relaxed);

    return impl_->traces[sig_it->second];
}

std::vector<SignalTrace> SignalTracer::query(
    const TraceFilter& filter) const
{
    std::vector<SignalTrace> results;

    if (!impl_) return results;

    impl_->stats.query_count.fetch_add(1, std::memory_order_relaxed);

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    results.reserve(std::min(filter.max_results, impl_->traces.size()));

    for (const auto& t : impl_->traces) {
        if (results.size() >= filter.max_results) break;
        if (filter.matches(t)) {
            results.push_back(t);
        }
    }

    if (!results.empty()) {
        impl_->stats.query_hit_count.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        impl_->stats.query_miss_count.fetch_add(
            1, std::memory_order_relaxed);
    }

    return results;
}

std::vector<SignalTrace> SignalTracer::query_ancestors(
    const SignalId& id) const
{
    std::vector<SignalTrace> result;

    if (!impl_) return result;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    auto current_id = id;
    std::size_t max_depth = 100;   // 防止无限循环

    while (max_depth-- > 0) {
        auto idx = impl_->find_signal_index(current_id);
        if (!idx) break;

        const auto& trace = impl_->traces[*idx];
        result.push_back(trace);

        if (trace.parent_signal_id.value() == 0) break;
        current_id = trace.parent_signal_id;
    }

    return result;
}

std::vector<SignalTrace> SignalTracer::query_children(
    const SignalId& id) const
{
    std::vector<SignalTrace> result;

    if (!impl_) return result;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    auto idx = impl_->find_signal_index(id);
    if (!idx) return result;

    const auto& parent = impl_->traces[*idx];

    result.reserve(parent.children.size());

    for (const auto& child_id : parent.children) {
        auto child_idx = impl_->find_signal_index(child_id);
        if (child_idx) {
            result.push_back(impl_->traces[*child_idx]);
        }
    }

    return result;
}

std::vector<SignalTrace> SignalTracer::query_time_range(
    Timestamp from, Timestamp to, std::size_t max_results) const
{
    std::vector<SignalTrace> results;

    if (!impl_) return results;

    if (from.microseconds() > to.microseconds()) {
        return results;
    }

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    results.reserve(std::min(max_results, impl_->traces.size()));

    for (const auto& t : impl_->traces) {
        if (results.size() >= max_results) break;
        const auto ts = t.timestamp.microseconds();
        if (ts >= from.microseconds() &&
            ts <= to.microseconds()) {
            results.push_back(t);
        }
    }

    return results;
}

// ==============================================================================
// 状态查询
// ==============================================================================
std::size_t SignalTracer::size() const noexcept {
    if (!impl_) return 0;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->traces.size();
}

std::size_t SignalTracer::capacity() const noexcept {
    if (!impl_) return 0;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->config.capacity;
}

bool SignalTracer::empty() const noexcept {
    return size() == 0;
}

// ==============================================================================
// 手动清理
// ==============================================================================
std::size_t SignalTracer::prune_expired() {
    if (!impl_) return 0;

    if (impl_->config.ttl_sec <= 0) return 0;

    const auto now = Timestamp::now();
    const auto ttl_us = impl_->config.ttl_sec * 1'000'000LL;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    std::size_t pruned = 0;

    // 从最旧开始，若还在 TTL 内则停止
    while (!impl_->traces.empty()) {
        const auto& front = impl_->traces.front();

        // Critical 保护
        if (impl_->config.protect_critical &&
            front.priority == TracePriority::Critical) {
            break;
        }

        const auto age_us = now.microseconds() -
                             front.recorded_at.microseconds();
        if (age_us < ttl_us) break;

        impl_->notify_evict(front, "ttl");
        impl_->stats.evicted_count.fetch_add(
            1, std::memory_order_relaxed);

        impl_->signal_index.erase(front.signal_id.value());
        impl_->trace_index.erase(front.trace_id.value());
        impl_->traces.pop_front();

        ++pruned;
    }

    // pop_front 后索引整体偏移，需要重建
    if (pruned > 0) {
        impl_->rebuild_signal_index();
    }

    return pruned;
}

void SignalTracer::clear() {
    if (!impl_) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->traces.clear();
    impl_->signal_index.clear();
    impl_->trace_index.clear();
}

// ==============================================================================
// 持久化
// ==============================================================================
std::string SignalTracer::export_json() const {
    if (!impl_) return "[]";

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    nlohmann::json arr = nlohmann::json::array();

    for (const auto& t : impl_->traces) {
        try {
            arr.push_back(nlohmann::json::parse(t.to_json()));
        } catch (const std::exception& e) {
            // 跳过损坏记录
            QUANT_LOG_WARN("追踪序列化失败: {}", e.what());
        }
    }

    return arr.dump();
}

Result<void> SignalTracer::import_json(std::string_view json_str) {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "SignalTracer 未启动");
    }

    if (json_str.empty()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "空 JSON 字符串");
    }

    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::parse_error& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "JSON 解析失败")
            .with_context("error", e.what());
    }

    if (!arr.is_array()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "JSON 根节点必须为数组");
    }

    std::size_t imported = 0;
    std::size_t failed = 0;

    for (const auto& item : arr) {
        try {
            auto trace_result = SignalTrace::from_json(item.dump());
            if (trace_result.is_err()) {
                ++failed;
                continue;
            }

            std::unique_lock<std::shared_mutex> lock(impl_->mutex);
            impl_->do_trace(std::move(trace_result.value()));
            ++imported;
        } catch (const std::exception& e) {
            ++failed;
            QUANT_LOG_WARN("导入失败: {}", e.what());
        }
    }

    QUANT_LOG_INFO("导入完成: 成功 {} 条, 失败 {} 条",
                   imported, failed);

    if (failed > 0 && imported == 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "全部导入失败");
    }

    return {};
}

Result<void> SignalTracer::flush() {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "SignalTracer 未启动");
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->flush_sink();

    if (impl_->deps.sink) {
        try {
            impl_->deps.sink->flush();
        } catch (const std::exception& e) {
            return QUANT_ERR_MSG(ErrorCode::SystemIoError,
                "Sink flush 异常")
                .with_context("error", e.what());
        }
    }

    return {};
}

// ==============================================================================
// 统计与诊断
// ==============================================================================
const TracerStats& SignalTracer::stats() const noexcept {
    static const TracerStats kEmpty;
    return impl_ ? impl_->stats : kEmpty;
}

std::string SignalTracer::dump() const {
    std::ostringstream oss;
    oss << "SignalTracer dump:\n";

    if (!impl_) {
        oss << "  <impl is null>\n";
        return oss.str();
    }

    oss << "  running: " << (is_running() ? "yes" : "no") << "\n";

    // 配置
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);
        oss << "  config:\n";
        oss << "    capacity: " << impl_->config.capacity << "\n";
        oss << "    ttl_sec: " << impl_->config.ttl_sec << "\n";
        oss << "    sample_rate: " << impl_->config.sample_rate << "\n";
        oss << "    enable_sampling: "
            << (impl_->config.enable_sampling ? "yes" : "no") << "\n";
        oss << "    batch_size: " << impl_->config.batch_size << "\n";
        oss << "    enable_dedup: "
            << (impl_->config.enable_dedup ? "yes" : "no") << "\n";
        oss << "    protect_critical: "
            << (impl_->config.protect_critical ? "yes" : "no") << "\n";

        // 状态
        oss << "  state:\n";
        oss << "    traces_size: " << impl_->traces.size() << "\n";
        oss << "    signal_index_size: "
            << impl_->signal_index.size() << "\n";
        oss << "    trace_index_size: "
            << impl_->trace_index.size() << "\n";
        oss << "    sink_buffer_size: "
            << impl_->sink_buffer.size() << "\n";
    }

    // 统计
    const auto& s = impl_->stats;
    oss << "  stats:\n";
    oss << "    traced: "
        << s.traced_count.load(std::memory_order_relaxed) << "\n";
    oss << "    deduplicated: "
        << s.deduplicated_count.load(std::memory_order_relaxed) << "\n";
    oss << "    sampled_out: "
        << s.sampled_out_count.load(std::memory_order_relaxed) << "\n";
    oss << "    evicted: "
        << s.evicted_count.load(std::memory_order_relaxed) << "\n";
    oss << "    query: "
        << s.query_count.load(std::memory_order_relaxed) << "\n";
    oss << "    query_hit: "
        << s.query_hit_count.load(std::memory_order_relaxed) << "\n";
    oss << "    query_miss: "
        << s.query_miss_count.load(std::memory_order_relaxed) << "\n";
    oss << "    query_hit_rate: " << s.query_hit_rate() << "\n";
    oss << "    sink_write: "
        << s.sink_write_count.load(std::memory_order_relaxed) << "\n";
    oss << "    sink_failed: "
        << s.sink_failed_count.load(std::memory_order_relaxed) << "\n";
    oss << "    invalid_trace: "
        << s.invalid_trace_count.load(std::memory_order_relaxed) << "\n";
    oss << "    serialize_error: "
        << s.serialize_error_count.load(std::memory_order_relaxed) << "\n";

    return oss.str();
}

}  // namespace quant::strategy
