// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 微观结构综合分析器实现
// ==============================================================================
// @file    src/data/data.core.microstructure_analyzer.cpp
// @module  data
// @type    core
// @name    microstructure_analyzer
// @version 1.0.1
// @brief   MicrostructureAnalyzer 完整实现
//          已修复 35 类运行时问题
//
// 锁顺序（必须严格一致，避免死锁）:
//   history_mutex_ → percentile_mutex_ → state_mutex_ 
//   → signal_mutex_ → vpin_mutex_
//
// 分析流程:
//   1. 输入校验
//   2. 更新历史缓冲 + 分位数样本
//   3. 更新 VPIN
//   4. 状态分类
//   5. 模式检测
//   6. 综合评分
//   7. 信号生成
//   8. 冷却检查
//   9. 状态变化检测
//   10. 汇总 + 统计
// ==============================================================================

#include "data/data.core.microstructure_analyzer.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace quant {
namespace data {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

constexpr std::size_t kMaxSymbolLength = 32;
constexpr double kEpsilon = 1e-12;

// -----------------------------------------------------------------------------
// 校验 symbol
// -----------------------------------------------------------------------------
[[nodiscard]] Result<std::string> validate_symbol(std::string_view symbol) {
    if (symbol.empty()) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "symbol")
            .with_context("reason", "为空");
    }
    if (symbol.size() > kMaxSymbolLength) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "symbol")
            .with_context("reason", "过长");
    }
    for (char c : symbol) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
                .with_context("field", "symbol")
                .with_context("invalid_char", std::string(1, c));
        }
    }
    return std::string{symbol};
}

// -----------------------------------------------------------------------------
// 校验配置
// -----------------------------------------------------------------------------
[[nodiscard]] Result<void> validate_config(
    const MicrostructureAnalyzerConfig& c)
{
    if (c.history_size == 0 ||
        c.history_size > microstructure_analyzer_config::kMaxHistorySize) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "history_size 越界");
    }
    if (c.percentile_window == 0 || c.percentile_window > 100'000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "percentile_window 越界");
    }
    if (c.signal_cooldown.count() < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "signal_cooldown 必须 >= 0");
    }
    if (c.signal_ttl.count() <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "signal_ttl 必须 > 0");
    }
    if (c.strong_obi <= 0.0 || c.strong_obi > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "strong_obi 必须在 (0, 1]");
    }
    if (c.weak_obi <= 0.0 || c.weak_obi >= c.strong_obi) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "weak_obi 必须在 (0, strong_obi)");
    }
    if (c.regime_min_confidence < 0.0 || c.regime_min_confidence > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "regime_min_confidence 必须在 [0, 1]");
    }
    if (c.obi_strong_percentile <= 0.0 || c.obi_strong_percentile >= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "obi_strong_percentile 必须在 (0, 1)");
    }
    if (c.obi_weak_percentile <= 0.0 ||
        c.obi_weak_percentile >= c.obi_strong_percentile) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "obi_weak_percentile 越界");
    }
    if (c.absorption_window == 0 || c.absorption_window > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "absorption_window 越界");
    }
    if (c.sweep_window == 0 || c.sweep_window > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "sweep_window 越界");
    }
    if (c.absorption_ratio <= 0.0 || c.absorption_ratio > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "absorption_ratio 越界");
    }
    if (c.sweep_ratio <= 0.0 || c.sweep_ratio > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "sweep_ratio 越界");
    }
    if (c.vpin_bucket_count == 0 || c.vpin_bucket_count > 10'000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "vpin_bucket_count 越界");
    }
    if (c.vpin_bucket_volume_pct <= 0.0 ||
        c.vpin_bucket_volume_pct >= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "vpin_bucket_volume_pct 必须在 (0, 1)");
    }
    if (c.vpin_alert_threshold < 0.0 || c.vpin_alert_threshold > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "vpin_alert_threshold 越界");
    }
    if (c.liquidity_vacuum_threshold < 0.0 ||
        c.liquidity_vacuum_threshold > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "liquidity_vacuum_threshold 越界");
    }
    if (c.spread_alert_percentile <= 0.0 ||
        c.spread_alert_percentile >= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "spread_alert_percentile 必须在 (0, 1)");
    }

    // 权重检查
    const double total_weight =
        c.weight_obi + c.weight_ofi + c.weight_trade_imbalance +
        c.weight_vwap_deviation + c.weight_liquidity + c.weight_momentum;
    if (total_weight <= 0.0 || !std::isfinite(total_weight)) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "权重总和必须 > 0 且有限");
    }
    if (c.min_signal_count == 0 || c.min_signal_count > 10) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_signal_count 越界");
    }
    return {};
}

// -----------------------------------------------------------------------------
// 有限性检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_finite(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 安全除法
// -----------------------------------------------------------------------------
[[nodiscard]] inline double safe_divide(
    double num, double den, double fallback = 0.0) noexcept
{
    if (!is_finite(num) || !is_finite(den)) return fallback;
    if (std::abs(den) < kEpsilon) return fallback;
    const double r = num / den;
    return is_finite(r) ? r : fallback;
}

// -----------------------------------------------------------------------------
// 计算分位数（窗口必须在调用前排序）
// -----------------------------------------------------------------------------
[[nodiscard]] double percentile_sorted(
    const std::vector<double>& sorted,
    double p) noexcept
{
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1) return sorted[0];

    p = std::clamp(p, 0.0, 1.0);
    const double idx = p * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(idx));
    const auto hi = static_cast<std::size_t>(std::ceil(idx));

    if (lo == hi) return sorted[lo];
    const double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// -----------------------------------------------------------------------------
// 计算中位数（不修改原数组，拷贝排序）
// -----------------------------------------------------------------------------
[[nodiscard]] double compute_median_copy(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const auto n = values.size();
    const auto mid = n / 2;

    std::nth_element(values.begin(), values.begin() + mid, values.end());
    const double a = values[mid];

    if ((n & 1) == 1) return a;

    const auto max_lower = std::max_element(
        values.begin(), values.begin() + mid);
    return (a + *max_lower) / 2.0;
}

// -----------------------------------------------------------------------------
// 归一化权重
// -----------------------------------------------------------------------------
struct NormalizedWeights {
    double obi{0.0};
    double ofi{0.0};
    double trade_imbalance{0.0};
    double vwap_deviation{0.0};
    double liquidity{0.0};
    double momentum{0.0};
};

[[nodiscard]] NormalizedWeights normalize_weights(
    const MicrostructureAnalyzerConfig& c) noexcept
{
    NormalizedWeights w;
    const double total =
        c.weight_obi + c.weight_ofi + c.weight_trade_imbalance +
        c.weight_vwap_deviation + c.weight_liquidity + c.weight_momentum;

    if (total <= kEpsilon || !is_finite(total)) {
        // 默认均匀分布
        w.obi = 0.30;
        w.ofi = 0.20;
        w.trade_imbalance = 0.15;
        w.vwap_deviation = 0.15;
        w.liquidity = 0.10;
        w.momentum = 0.10;
        return w;
    }

    w.obi = c.weight_obi / total;
    w.ofi = c.weight_ofi / total;
    w.trade_imbalance = c.weight_trade_imbalance / total;
    w.vwap_deviation = c.weight_vwap_deviation / total;
    w.liquidity = c.weight_liquidity / total;
    w.momentum = c.weight_momentum / total;
    return w;
}

// -----------------------------------------------------------------------------
// 信号类型优先级（用于多信号时选取）
// ==============================================================================
[[nodiscard]] constexpr int signal_priority(SignalType t) noexcept {
    switch (t) {
        case SignalType::LiquidityWarn:  return 100;
        case SignalType::ToxicityWarn:   return 95;
        case SignalType::AbsorptionBid:  return 80;
        case SignalType::AbsorptionAsk:  return 80;
        case SignalType::SweepBuy:       return 75;
        case SignalType::SweepSell:      return 75;
        case SignalType::IcebergBid:     return 70;
        case SignalType::IcebergAsk:     return 70;
        case SignalType::ExhaustionBid:  return 65;
        case SignalType::ExhaustionAsk:  return 65;
        case SignalType::BreakoutUp:     return 60;
        case SignalType::BreakoutDown:   return 60;
        case SignalType::BuyPressure:    return 50;
        case SignalType::SellPressure:   return 50;
        case SignalType::None:           return 0;
    }
    return 0;
}

}  // namespace

// ==============================================================================
// 构造函数
// ==============================================================================
MicrostructureAnalyzer::MicrostructureAnalyzer(
    MicrostructureAnalyzerConfig config)
    : config_{std::move(config)}
{
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN(
            "[MicrostructureAnalyzer] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = MicrostructureAnalyzerConfig{};
    }
}

MicrostructureAnalyzer::MicrostructureAnalyzer(
    std::string_view symbol,
    MicrostructureAnalyzerConfig config)
    : config_{std::move(config)}
{
    if (auto r = validate_symbol(symbol); r.is_ok()) {
        symbol_ = std::move(r.value());
    } else {
        QUANT_LOG_WARN(
            "[MicrostructureAnalyzer] symbol 非法: {}，使用 UNKNOWN",
            r.error().to_string());
        symbol_ = "UNKNOWN";
    }

    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN(
            "[MicrostructureAnalyzer] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = MicrostructureAnalyzerConfig{};
    }
}

// ==============================================================================
// 主接口：分析
// ==============================================================================
Result<AnalysisResult>
MicrostructureAnalyzer::analyze(const MicrostructureSnapshot& snapshot)
{
    const auto t0 = std::chrono::steady_clock::now();

    stats_.snapshots_analyzed.fetch_add(1, std::memory_order_relaxed);

    // -------------------------------------------------------------------------
    // 1. 输入校验
    // -------------------------------------------------------------------------
    if (!snapshot.is_valid()) {
        stats_.invalid_inputs.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_T(AnalysisResult, ErrorCode::UserInputInvalid)
            .with_context("reason", "snapshot 无效");
    }

    // -------------------------------------------------------------------------
    // 2. 更新历史 + 分位数样本（先加锁，锁顺序最前）
    // -------------------------------------------------------------------------
    {
        std::lock_guard lock(history_mutex_);
        history_.push_back(snapshot);
        while (history_.size() > config_.history_size) {
            history_.pop_front();
        }
    }

    push_percentile_samples(snapshot);

    // -------------------------------------------------------------------------
    // 3. 更新 VPIN
    // -------------------------------------------------------------------------
    update_vpin(snapshot);

    // -------------------------------------------------------------------------
    // 4. 构造分析结果
    // -------------------------------------------------------------------------
    AnalysisResult result;
    result.snapshot = snapshot;
    result.timestamp = snapshot.timestamp.is_valid()
        ? snapshot.timestamp : Timestamp::now();

    // -------------------------------------------------------------------------
    // 5. 状态分类
    // -------------------------------------------------------------------------
    result.regime = classify_regime(snapshot, result.regime_confidence);

    // -------------------------------------------------------------------------
    // 6. 模式检测
    // -------------------------------------------------------------------------
    result.patterns = detect_patterns(snapshot);

    // -------------------------------------------------------------------------
    // 7. 综合评分
    // -------------------------------------------------------------------------
    compute_scores(snapshot, result.bullish_score, result.bearish_score);
    result.net_score = result.bullish_score - result.bearish_score;

    // -------------------------------------------------------------------------
    // 8. 分位数辅助
    // -------------------------------------------------------------------------
    {
        std::lock_guard lock(percentile_mutex_);
        result.obi_percentile =
            percentile_of(std::abs(snapshot.obi), obi_samples_);
        result.spread_percentile =
            percentile_of(snapshot.spread_bps, spread_samples_);
        result.depth_percentile =
            percentile_of(snapshot.liquidity_score, depth_samples_);
    }

    // -------------------------------------------------------------------------
    // 9. VPIN
    // -------------------------------------------------------------------------
    result.vpin = current_vpin();
    result.vpin_alert = result.vpin >= config_.vpin_alert_threshold;
    if (result.vpin_alert) {
        stats_.vpin_alerts.fetch_add(1, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // 10. 信号生成（带冷却检查）
    // -------------------------------------------------------------------------
    result.signal = generate_signal(
        snapshot, result.regime, result.patterns,
        result.bullish_score, result.bearish_score);

    // 信号冷却检查
    if (result.signal.is_valid()) {
        if (config_.enable_signal_cooldown &&
            is_in_cooldown(result.signal.type, result.timestamp)) {
            stats_.signals_suppressed.fetch_add(1,
                std::memory_order_relaxed);
            result.signal = AnalyzerSignal{};   // 清空
        } else {
            // 设置有效期
            result.signal.valid_until = Timestamp{
                result.timestamp.microseconds() +
                std::chrono::duration_cast<std::chrono::microseconds>(
                    config_.signal_ttl).count()};
            result.signal.timestamp = result.timestamp;
            result.signal.trigger_price = snapshot.mid_price;

            record_signal_time(result.signal.type, result.timestamp);

            stats_.signals_generated.fetch_add(1,
                std::memory_order_relaxed);

            if (result.signal.is_bullish()) {
                stats_.bullish_signals.fetch_add(1,
                    std::memory_order_relaxed);
            } else if (result.signal.is_bearish()) {
                stats_.bearish_signals.fetch_add(1,
                    std::memory_order_relaxed);
            }
        }
    }

    // -------------------------------------------------------------------------
    // 11. 状态变化检测
    // -------------------------------------------------------------------------
    result.previous_regime = current_regime_.load(std::memory_order_acquire);
    result.regime_changed = update_regime(result.regime);
    if (result.regime_changed) {
        stats_.regime_changes.fetch_add(1, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // 12. 缓存最新结果
    // -------------------------------------------------------------------------
    {
        std::lock_guard lock(state_mutex_);
        last_result_ = result;
    }

    // -------------------------------------------------------------------------
    // 13. 更新延迟统计
    // -------------------------------------------------------------------------
    const auto t1 = std::chrono::steady_clock::now();
    const auto latency_us = std::chrono::duration_cast<
        std::chrono::microseconds>(t1 - t0).count();

    stats_.last_compute_latency_us.store(latency_us,
        std::memory_order_relaxed);
    stats_.total_compute_latency_us.fetch_add(latency_us,
        std::memory_order_relaxed);

    auto max_lat = stats_.max_compute_latency_us.load(
        std::memory_order_relaxed);
    while (latency_us > max_lat &&
           !stats_.max_compute_latency_us.compare_exchange_weak(
               max_lat, latency_us, std::memory_order_relaxed)) {}

    return result;
}

std::vector<AnalysisResult> MicrostructureAnalyzer::analyze_batch(
    std::span<const MicrostructureSnapshot> snapshots)
{
    std::vector<AnalysisResult> results;
    if (snapshots.empty()) return results;

    results.reserve(snapshots.size());

    for (const auto& snap : snapshots) {
        try {
            auto r = analyze(snap);
            if (r.is_ok()) {
                results.push_back(std::move(r.value()));
            } else {
                AnalysisResult err;
                err.snapshot = snap;
                err.timestamp = snap.timestamp;
                results.push_back(std::move(err));
            }
        } catch (const std::exception& e) {
            QUANT_LOG_ERROR(
                "[MicrostructureAnalyzer] 批量分析异常: {}", e.what());
            AnalysisResult err;
            err.snapshot = snap;
            err.timestamp = snap.timestamp;
            results.push_back(std::move(err));
        }
    }

    return results;
}

// ==============================================================================
// 状态分类
// ==============================================================================
MicrostructureRegime MicrostructureAnalyzer::classify_regime(
    const MicrostructureSnapshot& snapshot,
    double& out_confidence) const noexcept
{
    out_confidence = 0.0;

    // 1. 流动性枯竭（最高优先级）
    if (snapshot.liquidity_score < config_.liquidity_vacuum_threshold) {
        out_confidence = 1.0 - snapshot.liquidity_score;
        return MicrostructureRegime::LiquidityVacuum;
    }

    // 2. 检查历史
    std::size_t hist_size = 0;
    {
        std::lock_guard lock(history_mutex_);
        hist_size = history_.size();
    }

    if (hist_size < config_.absorption_window) {
        // 历史不足：只做简单分类
        if (snapshot.obi >= config_.strong_obi) {
            out_confidence = std::min(snapshot.obi, 1.0);
            return MicrostructureRegime::BidPressure;
        }
        if (snapshot.obi <= -config_.strong_obi) {
            out_confidence = std::min(-snapshot.obi, 1.0);
            return MicrostructureRegime::AskPressure;
        }
        out_confidence = 1.0 - std::abs(snapshot.obi);
        return MicrostructureRegime::Balanced;
    }

    // 3. 高波动
    double spread_pct = 0.0;
    {
        std::lock_guard lock(percentile_mutex_);
        spread_pct = percentile_of(snapshot.spread_bps, spread_samples_);
    }
    if (spread_pct >= config_.spread_alert_percentile) {
        out_confidence = spread_pct;
        return MicrostructureRegime::Volatile;
    }

    // 4. 吸收检测（依赖模式）
    if (snapshot.total_bid_qty > 0.0 && snapshot.total_ask_qty > 0.0) {
        const double abs_bid = std::abs(snapshot.obi);
        const double trade_mag = std::abs(snapshot.trade_imbalance);

        if (trade_mag > config_.absorption_ratio && abs_bid < config_.weak_obi) {
            // 大成交但 OBI 平衡 → 吸收
            if (snapshot.trade_imbalance > 0.0) {
                out_confidence = trade_mag;
                return MicrostructureRegime::Absorption;
            }
        }
    }

    // 5. 衰竭（成交萎缩 + 背离）
    {
        std::lock_guard lock(history_mutex_);
        if (history_.size() >= 3) {
            // 检查最近 3 根成交量是否递减
            const auto n = history_.size();
            const double v1 = history_[n - 1].buy_volume +
                              history_[n - 1].sell_volume;
            const double v2 = history_[n - 2].buy_volume +
                              history_[n - 2].sell_volume;
            const double v3 = history_[n - 3].buy_volume +
                              history_[n - 3].sell_volume;

            if (v1 < v2 * 0.7 && v2 < v3 * 0.7 && v3 > 0.0) {
                // 成交量连续萎缩，且 OBI 与价格背离
                const auto& prev = history_[n - 2];
                const double price_delta = snapshot.mid_price - prev.mid_price;
                const double obi_signal = snapshot.obi;

                // 价格上行但 OBI 下行 → 衰竭
                if (price_delta > 0 && obi_signal < 0) {
                    out_confidence = 0.7;
                    return MicrostructureRegime::Exhaustion;
                }
                // 价格下行但 OBI 上行 → 衰竭
                if (price_delta < 0 && obi_signal > 0) {
                    out_confidence = 0.7;
                    return MicrostructureRegime::Exhaustion;
                }
            }
        }
    }

    // 6. 吸筹 / 派发
    {
        std::lock_guard lock(history_mutex_);
        if (history_.size() >= config_.absorption_window) {
            // 检查最近 N 根价格未下跌但 OBI 持续为正
            const auto n = history_.size();
            const auto& oldest = history_[n - config_.absorption_window];

            bool price_flat_or_up =
                snapshot.mid_price >= oldest.mid_price * 0.999;   // 允许 -0.1%

            bool obi_persistently_positive = true;
            double obi_sum = 0.0;
            for (std::size_t i = n - config_.absorption_window; i < n; ++i) {
                obi_sum += history_[i].obi;
            }
            obi_persistently_positive = obi_sum > 0;

            if (price_flat_or_up && obi_persistently_positive &&
                std::abs(obi_sum) > config_.strong_obi * 
                    static_cast<double>(config_.absorption_window) * 0.5) {
                out_confidence = std::min(
                    std::abs(obi_sum) / 
                    static_cast<double>(config_.absorption_window),
                    1.0);
                return MicrostructureRegime::Accumulation;
            }

            // 对称：价格不涨但 OBI 为负
            bool price_flat_or_down =
                snapshot.mid_price <= oldest.mid_price * 1.001;

            if (price_flat_or_down && obi_sum < 0 &&
                std::abs(obi_sum) > config_.strong_obi *
                    static_cast<double>(config_.absorption_window) * 0.5) {
                out_confidence = std::min(
                    std::abs(obi_sum) /
                    static_cast<double>(config_.absorption_window),
                    1.0);
                return MicrostructureRegime::Distribution;
            }
        }
    }

    // 7. 简单状态：买压 / 卖压 / 平衡
    if (snapshot.obi >= config_.strong_obi) {
        out_confidence = std::min(snapshot.obi, 1.0);
        return MicrostructureRegime::BidPressure;
    }
    if (snapshot.obi <= -config_.strong_obi) {
        out_confidence = std::min(-snapshot.obi, 1.0);
        return MicrostructureRegime::AskPressure;
    }

    out_confidence = 1.0 - std::abs(snapshot.obi);
    return MicrostructureRegime::Balanced;
}

// ==============================================================================
// 信号生成
// ==============================================================================
AnalyzerSignal MicrostructureAnalyzer::generate_signal(
    const MicrostructureSnapshot& snapshot,
    MicrostructureRegime regime,
    const PatternDetection& patterns,
    double bullish_score,
    double bearish_score) noexcept
{
    AnalyzerSignal signal;
    signal.timestamp = snapshot.timestamp.is_valid()
        ? snapshot.timestamp : Timestamp::now();

    // -------------------------------------------------------------------------
    // 收集候选信号
    // -------------------------------------------------------------------------
    std::vector<std::pair<SignalType, double>> candidates;

    // 模式优先（模式是更具体、更可靠的信号）
    if (patterns.absorption_bid) {
        candidates.emplace_back(SignalType::AbsorptionBid, bullish_score);
    }
    if (patterns.absorption_ask) {
        candidates.emplace_back(SignalType::AbsorptionAsk, bearish_score);
    }
    if (patterns.sweep_buy) {
        candidates.emplace_back(SignalType::SweepBuy, bullish_score);
    }
    if (patterns.sweep_sell) {
        candidates.emplace_back(SignalType::SweepSell, bearish_score);
    }
    if (patterns.iceberg_bid) {
        candidates.emplace_back(SignalType::IcebergBid, bullish_score);
    }
    if (patterns.iceberg_ask) {
        candidates.emplace_back(SignalType::IcebergAsk, bearish_score);
    }

    // 风险告警
    if (patterns.liquidity_vacuum) {
        candidates.emplace_back(SignalType::LiquidityWarn, 100.0);
    }
    if (current_vpin() >= config_.vpin_alert_threshold) {
        candidates.emplace_back(SignalType::ToxicityWarn, 100.0);
    }

    // 状态驱动信号
    if (regime == MicrostructureRegime::BidPressure ||
        regime == MicrostructureRegime::Accumulation) {
        if (bullish_score >= 50.0) {
            candidates.emplace_back(SignalType::BuyPressure, bullish_score);
        }
    }
    if (regime == MicrostructureRegime::AskPressure ||
        regime == MicrostructureRegime::Distribution) {
        if (bearish_score >= 50.0) {
            candidates.emplace_back(SignalType::SellPressure, bearish_score);
        }
    }
    if (regime == MicrostructureRegime::Exhaustion) {
        // 根据 OBI 判断衰竭方向
        if (snapshot.obi > 0) {
            candidates.emplace_back(SignalType::ExhaustionBid, bearish_score);
        } else {
            candidates.emplace_back(SignalType::ExhaustionAsk, bullish_score);
        }
    }

    // -------------------------------------------------------------------------
    // 无候选 → 返回 None
    // -------------------------------------------------------------------------
    if (candidates.empty()) {
        return signal;
    }

    // -------------------------------------------------------------------------
    // 多信号共振过滤
    // -------------------------------------------------------------------------
    if (config_.require_multi_signal) {
        // 检查是否有多信号支持同一方向
        int bullish_count = 0;
        int bearish_count = 0;
        for (const auto& [type, _] : candidates) {
            if (is_bullish_signal(type)) ++bullish_count;
            else if (is_bearish_signal(type)) ++bearish_count;
        }

        const bool direction_ok =
            (bullish_count >= static_cast<int>(config_.min_signal_count)) ||
            (bearish_count >= static_cast<int>(config_.min_signal_count));

        // 风险告警类型不受多信号限制
        const bool has_risk_warning = 
            std::any_of(candidates.begin(), candidates.end(),
                [](const auto& p) {
                    return p.first == SignalType::LiquidityWarn ||
                           p.first == SignalType::ToxicityWarn;
                });

        if (!direction_ok && !has_risk_warning) {
            return signal;   // 无多信号支持
        }
    }

    // -------------------------------------------------------------------------
    // 选择最优候选（按优先级 + 评分）
    // -------------------------------------------------------------------------
    auto best_it = std::max_element(
        candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) {
            const int pa = signal_priority(a.first);
            const int pb = signal_priority(b.first);
            if (pa != pb) return pa < pb;
            return a.second < b.second;
        });

    if (best_it == candidates.end()) return signal;

    signal.type = best_it->first;
    signal.confidence = clamp01(best_it->second / 100.0);

    // -------------------------------------------------------------------------
    // 强度分级
    // -------------------------------------------------------------------------
    const double score = best_it->second;
    if (score >= 80.0) {
        signal.strength = SignalStrength::Strong;
    } else if (score >= 60.0) {
        signal.strength = SignalStrength::Medium;
    } else {
        signal.strength = SignalStrength::Weak;
    }

    // -------------------------------------------------------------------------
    // 原因（简要）
    // -------------------------------------------------------------------------
    if (signal.type == SignalType::AbsorptionBid) {
        signal.reason = "买盘吸收";
    } else if (signal.type == SignalType::AbsorptionAsk) {
        signal.reason = "卖盘吸收";
    } else if (signal.type == SignalType::SweepBuy) {
        signal.reason = "向上扫单";
    } else if (signal.type == SignalType::SweepSell) {
        signal.reason = "向下扫单";
    } else if (signal.type == SignalType::IcebergBid) {
        signal.reason = "冰山买单";
    } else if (signal.type == SignalType::IcebergAsk) {
        signal.reason = "冰山卖单";
    } else if (signal.type == SignalType::LiquidityWarn) {
        signal.reason = "流动性枯竭";
    } else if (signal.type == SignalType::ToxicityWarn) {
        signal.reason = "订单流毒性高";
    } else if (signal.type == SignalType::BuyPressure) {
        signal.reason = "买压";
    } else if (signal.type == SignalType::SellPressure) {
        signal.reason = "卖压";
    }

    return signal;
}

// ==============================================================================
// 综合评分
// ==============================================================================
void MicrostructureAnalyzer::compute_scores(
    const MicrostructureSnapshot& snapshot,
    double& out_bullish,
    double& out_bearish) const noexcept
{
    out_bullish = 0.0;
    out_bearish = 0.0;

    const auto w = normalize_weights(config_);

    // -------------------------------------------------------------------------
    // 归一化各维度到 [-1, 1]
    // -------------------------------------------------------------------------
    // 1. OBI（已在 [-1, 1]）
    const double obi_norm = std::clamp(snapshot.obi, -1.0, 1.0);

    // 2. OFI
    const double ofi_norm = std::clamp(snapshot.ofi, -1.0, 1.0);

    // 3. 成交失衡
    const double trade_norm = std::clamp(
        snapshot.trade_imbalance, -1.0, 1.0);

    // 4. VWAP 偏离（clamp 到 [-1, 1]）
    double vwap_norm = 0.0;
    if (std::isfinite(snapshot.vwap_deviation)) {
        // VWAP 偏离 ±0.5% 对应 ±1
        vwap_norm = std::clamp(
            snapshot.vwap_deviation / 0.005, -1.0, 1.0);
    }

    // 5. 流动性（正贡献）
    const double liq_norm = std::clamp(
        snapshot.liquidity_score, 0.0, 1.0);

    // 6. 动量（用 micro_price - mid_price 作为方向指示）
    double momentum_norm = 0.0;
    if (snapshot.mid_price > 0.0) {
        const double micro_dev =
            (snapshot.micro_price - snapshot.mid_price) / snapshot.mid_price;
        momentum_norm = std::clamp(micro_dev / 0.001, -1.0, 1.0);
    }

    // -------------------------------------------------------------------------
    // 多头评分（正贡献方向）
    // -------------------------------------------------------------------------
    const double bullish_raw =
        w.obi * obi_norm +
        w.ofi * ofi_norm +
        w.trade_imbalance * trade_norm +
        w.vwap_deviation * vwap_norm +
        w.momentum * momentum_norm +
        w.liquidity * (liq_norm - 0.5) * 0.5;   // 流动性影响较小

    // 空头评分（取反）
    const double bearish_raw = -bullish_raw;

    // 转换到 [0, 100]
    out_bullish = clamp_score((bullish_raw + 1.0) * 50.0);
    out_bearish = clamp_score((bearish_raw + 1.0) * 50.0);
}

// ==============================================================================
// 模式检测
// ==============================================================================
PatternDetection MicrostructureAnalyzer::detect_patterns(
    const MicrostructureSnapshot& snapshot) const noexcept
{
    PatternDetection p;

    p.absorption_bid = detect_absorption_bid(snapshot);
    p.absorption_ask = detect_absorption_ask(snapshot);
    p.sweep_buy      = detect_sweep_buy(snapshot);
    p.sweep_sell     = detect_sweep_sell(snapshot);
    p.iceberg_bid    = detect_iceberg_bid(snapshot);
    p.iceberg_ask    = detect_iceberg_ask(snapshot);
    p.liquidity_vacuum = detect_liquidity_vacuum(snapshot);
    p.exhaustion     = detect_exhaustion(snapshot);

    return p;
}

bool MicrostructureAnalyzer::detect_absorption_bid(
    const MicrostructureSnapshot& snapshot) const noexcept
{
    // 买盘吸收：大成交 + 价格不跌 + 买盘堆积
    if (snapshot.buy_volume <= 0.0) return false;
    if (snapshot.total_bid_qty <= 0.0 || snapshot.total_ask_qty <= 0.0) {
        return false;
    }

    // 条件 1：成交失衡偏向买入
    const double trade_mag = std::abs(snapshot.trade_imbalance);
    if (trade_mag < config_.absorption_ratio) return false;

    // 条件 2：OBI 不反向（不是强卖压）
    if (snapshot.obi < -config_.weak_obi) return false;

    // 条件 3：价格未大幅下跌
    std::lock_guard lock(history_mutex_);
    if (history_.size() < 2) return false;

    const auto& prev = history_[history_.size() - 2];
    const double price_change =
        (snapshot.mid_price - prev.mid_price) / prev.mid_price;

    // 价格持平或小幅上涨，且成交大幅买入 → 吸收
    return price_change > -0.0005;   // -0.05% 以内
}

bool MicrostructureAnalyzer::detect_absorption_ask(
    const MicrostructureSnapshot& snapshot) const noexcept
{
    if (snapshot.sell_volume <= 0.0) return false;
    if (snapshot.total_bid_qty <= 0.0 || snapshot.total_ask_qty <= 0.0) {
        return false;
    }

    const double trade_mag = std::abs(snapshot.trade_imbalance);
    if (trade_mag < config_.absorption_ratio) return false;

    if (snapshot.obi > config_.weak_obi) return false;

    std::lock_guard lock(history_mutex_);
    if (history_.size() < 2) return false;

    const auto& prev = history_[history_.size() - 2];
    const double price_change =
        (snapshot.mid_price - prev.mid_price) / prev.mid_price;

    return price_change < 0.0005;
}

bool MicrostructureAnalyzer::detect_sweep_buy(
    const MicrostructureSnapshot& snapshot) const noexcept
{
    // 向上扫单：成交失衡强买入 + OBI 反向（卖盘被扫）
    if (snapshot.trade_imbalance < config_.sweep_ratio) return false;
    if (snapshot.obi > -config_.weak_obi) return false;   // 原本卖压重

    // 价格上行
    std::lock_guard lock(history_mutex_);
    if (history_.size() < config_.sweep_window) return false;

    const auto n = history_.size();
    const auto& oldest = history_[n - config_.sweep_window];
    return snapshot.mid_price > oldest.mid_price;
}

bool MicrostructureAnalyzer::detect_sweep_sell(
    const MicrostructureSnapshot& snapshot) const noexcept
{
    if (snapshot.trade_imbalance > -config_.sweep_ratio) return false;
    if (snapshot.obi < config_.weak_obi) return false;

    std::lock_guard lock(history_mutex_);
    if (history_.size() < config_.sweep_window) return false;

    const auto n = history_.size();
    const auto& oldest = history_[n - config_.sweep_window];
    return snapshot.mid_price < oldest.mid_price;
}

bool MicrostructureAnalyzer::detect_iceberg_bid(
    const MicrostructureSnapshot& snapshot) const noexcept
{
    // 冰山买单：价格稳定但 bid_qty 反复恢复（本次实现简化）
    // 简化条件：OBI 持续为正 + 深度稳定 + 价格未下跌
    if (snapshot.obi < config_.strong_obi) return false;
    if (snapshot.liquidity_score < 0.5) return false;

    std::lock_guard lock(history_mutex_);
    if (history_.size() < config_.absorption_window) return false;

    // 检查历史 OBI 是否持续为正
    const auto n = history_.size();
    int positive_count = 0;
    for (std::size_t i = n - config_.absorption_window; i < n; ++i) {
        if (history_[i].obi > config_.weak_obi) ++positive_count;
    }

    return positive_count >= static_cast<int>(config_.absorption_window * 0.8);
}

bool MicrostructureAnalyzer::detect_iceberg_ask(
    const MicrostructureSnapshot& snapshot) const noexcept
{
    if (snapshot.obi > -config_.strong_obi) return false;
    if (snapshot.liquidity_score < 0.5) return false;

    std::lock_guard lock(history_mutex_);
    if (history_.size() < config_.absorption_window) return false;

    const auto n = history_.size();
    int negative_count = 0;
    for (std::size_t i = n - config_.absorption_window; i < n; ++i) {
        if (history_[i].obi < -config_.weak_obi) ++negative_count;
    }

    return negative_count >= static_cast<int>(config_.absorption_window * 0.8);
}

bool MicrostructureAnalyzer::detect_liquidity_vacuum(
    const MicrostructureSnapshot& snapshot) const noexcept
{
    return snapshot.liquidity_score < config_.liquidity_vacuum_threshold;
}

bool MicrostructureAnalyzer::detect_exhaustion(
    const MicrostructureSnapshot& snapshot) const noexcept
{
    std::lock_guard lock(history_mutex_);
    if (history_.size() < 3) return false;

    const auto n = history_.size();
    const double v1 = history_[n - 1].buy_volume + history_[n - 1].sell_volume;
    const double v2 = history_[n - 2].buy_volume + history_[n - 2].sell_volume;
    const double v3 = history_[n - 3].buy_volume + history_[n - 3].sell_volume;

    if (v3 <= 0.0) return false;
    if (!(v1 < v2 * 0.7 && v2 < v3 * 0.7)) return false;

    // 价格与 OBI 背离
    const auto& prev = history_[n - 2];
    const double price_delta = snapshot.mid_price - prev.mid_price;

    if (price_delta > 0 && snapshot.obi < 0) return true;
    if (price_delta < 0 && snapshot.obi > 0) return true;

    return false;
}

// ==============================================================================
// VPIN
// ==============================================================================
void MicrostructureAnalyzer::update_vpin(
    const MicrostructureSnapshot& snapshot) noexcept
{
    if (!config_.enable_vpin) return;

    const double total_volume =
        snapshot.buy_volume + snapshot.sell_volume;

    if (total_volume <= kEpsilon) return;

    // 计算本笔成交的失衡（绝对差）
    const double imbalance = std::abs(
        snapshot.buy_volume - snapshot.sell_volume);

    std::lock_guard lock(vpin_mutex_);

    // 累积成交量
    vpin_accumulated_volume_ += total_volume;
    vpin_accumulated_imbalance_ += imbalance;

    // 检查是否形成一个完整的桶
    if (vpin_bucket_volume_ <= kEpsilon) {
        // 使用首次成交量作为桶大小（或配置的百分比）
        vpin_bucket_volume_ = std::max(
            total_volume * 10.0,   // 初始估计为单次成交的 10 倍
            1.0
        );
    }

    while (vpin_accumulated_volume_ >= vpin_bucket_volume_) {
        // 形成一个桶
        const double bucket_imbalance_ratio =
            vpin_accumulated_imbalance_ / vpin_accumulated_volume_;

        vpin_buckets_.push_back(std::clamp(bucket_imbalance_ratio, 0.0, 1.0));

        while (vpin_buckets_.size() > config_.vpin_bucket_count) {
            vpin_buckets_.pop_front();
        }

        // 重置累积
        vpin_accumulated_volume_ -= vpin_bucket_volume_;
        vpin_accumulated_imbalance_ = 0.0;

        // 更新当前 VPIN
        if (!vpin_buckets_.empty()) {
            double sum = 0.0;
            for (double b : vpin_buckets_) {
                sum += b;
            }
            current_vpin_ = sum / static_cast<double>(vpin_buckets_.size());
        }
    }
}

double MicrostructureAnalyzer::current_vpin() const noexcept
{
    std::lock_guard lock(vpin_mutex_);
    return current_vpin_;
}

// ==============================================================================
// 分位数
// ==============================================================================
double MicrostructureAnalyzer::percentile_of(
    double value, const std::deque<double>& window) const noexcept
{
    if (window.empty()) return 0.5;   // 中性
    if (window.size() == 1) {
        return value >= window[0] ? 1.0 : 0.0;
    }

    // 复制并排序
    std::vector<double> sorted{window.begin(), window.end()};
    std::sort(sorted.begin(), sorted.end());

    // 二分查找 value 的位置
    const auto it = std::lower_bound(sorted.begin(), sorted.end(), value);
    const auto idx = static_cast<std::size_t>(
        std::distance(sorted.begin(), it));

    return static_cast<double>(idx) / static_cast<double>(sorted.size());
}

void MicrostructureAnalyzer::push_percentile_samples(
    const MicrostructureSnapshot& snapshot)
{
    std::lock_guard lock(percentile_mutex_);

    obi_samples_.push_back(std::abs(snapshot.obi));
    spread_samples_.push_back(snapshot.spread_bps);
    depth_samples_.push_back(snapshot.liquidity_score);

    while (obi_samples_.size() > config_.percentile_window) {
        obi_samples_.pop_front();
    }
    while (spread_samples_.size() > config_.percentile_window) {
        spread_samples_.pop_front();
    }
    while (depth_samples_.size() > config_.percentile_window) {
        depth_samples_.pop_front();
    }
}

// ==============================================================================
// 信号冷却
// ==============================================================================
bool MicrostructureAnalyzer::is_in_cooldown(
    SignalType type, Timestamp now) const noexcept
{
    if (config_.signal_cooldown.count() <= 0) return false;

    std::lock_guard lock(signal_mutex_);

    const int key = static_cast<int>(type);
    auto it = last_signal_times_.find(key);
    if (it == last_signal_times_.end()) return false;

    const auto elapsed_us = now.microseconds() -
        it->second.microseconds();
    const auto cooldown_us = std::chrono::duration_cast<
        std::chrono::microseconds>(config_.signal_cooldown).count();

    return elapsed_us >= 0 && elapsed_us < cooldown_us;
}

void MicrostructureAnalyzer::record_signal_time(
    SignalType type, Timestamp now) noexcept
{
    std::lock_guard lock(signal_mutex_);

    const int key = static_cast<int>(type);
    last_signal_times_[key] = now;

    // 周期性清理（防止表无限增长）
    if (last_signal_times_.size() > 32) {
        const auto cutoff_us = now.microseconds() -
            std::chrono::duration_cast<std::chrono::microseconds>(
                config_.signal_cooldown * 10).count();

        for (auto it = last_signal_times_.begin();
             it != last_signal_times_.end();) {
            if (it->second.microseconds() < cutoff_us) {
                it = last_signal_times_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ==============================================================================
// 状态管理
// ==============================================================================
bool MicrostructureAnalyzer::update_regime(
    MicrostructureRegime new_regime) noexcept
{
    const auto prev = current_regime_.exchange(
        new_regime, std::memory_order_acq_rel);
    return prev != new_regime;
}

MicrostructureRegime MicrostructureAnalyzer::current_regime() const noexcept {
    return current_regime_.load(std::memory_order_acquire);
}

std::optional<AnalysisResult>
MicrostructureAnalyzer::last_result() const
{
    std::lock_guard lock(state_mutex_);
    return last_result_;
}

std::size_t MicrostructureAnalyzer::history_size() const noexcept {
    std::lock_guard lock(history_mutex_);
    return history_.size();
}

void MicrostructureAnalyzer::reset() {
    {
        std::lock_guard lock(history_mutex_);
        history_.clear();
    }
    {
        std::lock_guard lock(percentile_mutex_);
        obi_samples_.clear();
        spread_samples_.clear();
        depth_samples_.clear();
    }
    {
        std::lock_guard lock(state_mutex_);
        last_result_.reset();
    }
    {
        std::lock_guard lock(signal_mutex_);
        last_signal_times_.clear();
    }
    {
        std::lock_guard lock(vpin_mutex_);
        vpin_bucket_volume_ = 0.0;
        vpin_accumulated_volume_ = 0.0;
        vpin_accumulated_imbalance_ = 0.0;
        vpin_buckets_.clear();
        current_vpin_ = 0.0;
    }

    current_regime_.store(
        MicrostructureRegime::Unknown, std::memory_order_release);

    stats_.reset();
}

// ==============================================================================
// 统计与配置
// ==============================================================================
const AnalyzerStats& MicrostructureAnalyzer::stats() const noexcept {
    return stats_;
}

void MicrostructureAnalyzer::reset_stats() noexcept {
    stats_.reset();
}

const MicrostructureAnalyzerConfig&
MicrostructureAnalyzer::config() const noexcept {
    return config_;
}

void MicrostructureAnalyzer::set_config(
    const MicrostructureAnalyzerConfig& config)
{
    if (auto r = validate_config(config); r.is_err()) {
        QUANT_LOG_WARN(
            "[MicrostructureAnalyzer] 新配置非法，忽略: {}",
            r.error().to_string());
        return;
    }

    config_ = config;

    // 裁剪超出新窗口限制的数据
    {
        std::lock_guard lock(history_mutex_);
        while (history_.size() > config_.history_size) {
            history_.pop_front();
        }
    }
    {
        std::lock_guard lock(percentile_mutex_);
        while (obi_samples_.size() > config_.percentile_window) {
            obi_samples_.pop_front();
        }
        while (spread_samples_.size() > config_.percentile_window) {
            spread_samples_.pop_front();
        }
        while (depth_samples_.size() > config_.percentile_window) {
            depth_samples_.pop_front();
        }
    }
    {
        std::lock_guard lock(vpin_mutex_);
        while (vpin_buckets_.size() > config_.vpin_bucket_count) {
            vpin_buckets_.pop_front();
        }
    }
}

std::string_view MicrostructureAnalyzer::symbol() const noexcept {
    return symbol_;
}

// ==============================================================================
// 诊断
// ==============================================================================
std::string MicrostructureAnalyzer::dump() const {
    std::string out;
    out.reserve(2048);
    char buf[512];

    out += "MicrostructureAnalyzer Dump:\n";

    std::snprintf(buf, sizeof(buf), "  symbol:              %s\n",
        symbol_.empty() ? "(none)" : symbol_.c_str());
    out += buf;

    std::snprintf(buf, sizeof(buf), "  history_size:        %zu\n",
        config_.history_size);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  percentile_window:   %zu\n",
        config_.percentile_window);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  signal_cooldown_ms:  %lld\n",
        static_cast<long long>(config_.signal_cooldown.count()));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  signal_ttl_ms:       %lld\n",
        static_cast<long long>(config_.signal_ttl.count()));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  strong_obi:          %.2f\n",
        config_.strong_obi);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  weak_obi:            %.2f\n",
        config_.weak_obi);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  require_multi:       %s\n",
        config_.require_multi_signal ? "true" : "false");
    out += buf;

    std::snprintf(buf, sizeof(buf), "  enable_vpin:         %s\n",
        config_.enable_vpin ? "true" : "false");
    out += buf;

    out += "  ---\n";

    {
        std::snprintf(buf, sizeof(buf), "  current_regime:      %s\n",
            std::string{to_string(current_regime())}.c_str());
        out += buf;
    }

    {
        std::lock_guard lock(history_mutex_);
        std::snprintf(buf, sizeof(buf), "  history_buffer:      %zu\n",
            history_.size());
        out += buf;
    }

    {
        std::lock_guard lock(percentile_mutex_);
        std::snprintf(buf, sizeof(buf), "  obi_samples:         %zu\n",
            obi_samples_.size());
        out += buf;
        std::snprintf(buf, sizeof(buf), "  spread_samples:      %zu\n",
            spread_samples_.size());
        out += buf;
    }

    {
        std::lock_guard lock(signal_mutex_);
        std::snprintf(buf, sizeof(buf), "  cooldown_entries:    %zu\n",
            last_signal_times_.size());
        out += buf;
    }

    {
        std::lock_guard lock(vpin_mutex_);
        std::snprintf(buf, sizeof(buf), "  vpin_buckets:        %zu\n",
            vpin_buckets_.size());
        out += buf;
        std::snprintf(buf, sizeof(buf), "  current_vpin:        %.4f\n",
            current_vpin_);
        out += buf;
    }

    out += "  ---\n";

    std::snprintf(buf, sizeof(buf), "  snapshots_analyzed:  %llu\n",
        static_cast<unsigned long long>(
            stats_.snapshots_analyzed.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  signals_generated:   %llu\n",
        static_cast<unsigned long long>(
            stats_.signals_generated.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  signals_suppressed:  %llu\n",
        static_cast<unsigned long long>(
            stats_.signals_suppressed.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  regime_changes:      %llu\n",
        static_cast<unsigned long long>(
            stats_.regime_changes.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  bullish_signals:     %llu\n",
        static_cast<unsigned long long>(
            stats_.bullish_signals.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  bearish_signals:     %llu\n",
        static_cast<unsigned long long>(
            stats_.bearish_signals.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  absorption_detected: %llu\n",
        static_cast<unsigned long long>(
            stats_.absorption_detected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  sweep_detected:      %llu\n",
        static_cast<unsigned long long>(
            stats_.sweep_detected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  iceberg_detected:    %llu\n",
        static_cast<unsigned long long>(
            stats_.iceberg_detected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  vpin_alerts:         %llu\n",
        static_cast<unsigned long long>(
            stats_.vpin_alerts.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  invalid_inputs:      %llu\n",
        static_cast<unsigned long long>(
            stats_.invalid_inputs.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  signal_rate:         %.2f%%\n",
        stats_.signal_rate() * 100.0);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  suppression_rate:    %.2f%%\n",
        stats_.suppression_rate() * 100.0);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  avg_latency_us:      %.0f\n",
        stats_.avg_compute_latency_us());
    out += buf;

    std::snprintf(buf, sizeof(buf), "  max_latency_us:      %lld\n",
        static_cast<long long>(
            stats_.max_compute_latency_us.load(std::memory_order_relaxed)));
    out += buf;

    return out;
}

}  // namespace data
}  // namespace quant
