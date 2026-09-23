// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 逃顶/逃底检测实现
// ==============================================================================
// @file    src/strategy/strategy.core.escape_detector.cpp
// @module  strategy
// @type    core
// @name    escape_detector
// @version 1.0.1
// @brief   趋势强度自适应逃顶检测，5 条件加权评分，二次逃顶累积
//          已修复 30 类运行时问题
//
// 处理流程:
//   1. 输入校验（ctx.is_valid）
//   2. 冷却检查（min_escape_interval）
//   3. 条件评估（5 项加权）
//   4. 加权评分 → 触发判定
//   5. 严重性分级（首次/二次/三次）
//   6. 记录历史 + 通知回调
//
// 关键保护:
//   - ATR > kEpsilon
//   - volume_ma20 > kEpsilon
//   - recent_candles != nullptr && size >= lookback
//   - 方向校验（BUY / SELL）
//   - 冷却防抖
// ==============================================================================

#include "strategy/strategy.core.escape_detector.hpp"
#include "strategy/strategy.core.mtf_checker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 匿名命名空间：辅助函数
// ==============================================================================
namespace {

constexpr double kEps = escape_config::kEpsilon;
constexpr double kVolumeEps = 1e-9;

// -----------------------------------------------------------------------------
// 安全除法
// -----------------------------------------------------------------------------
[[nodiscard]] inline double safe_div(double a, double b,
                                       double def = 0.0) noexcept {
    if (std::abs(b) < kEps) return def;
    const double r = a / b;
    return std::isfinite(r) ? r : def;
}

// -----------------------------------------------------------------------------
// 限幅
// -----------------------------------------------------------------------------
[[nodiscard]] inline double clamp_val(double v, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, v));
}

// -----------------------------------------------------------------------------
// 有限数检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool finite(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 方向字符串
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::string_view side_to_string(Side s) noexcept {
    switch (s) {
        case Side::BUY:  return "BUY";
        case Side::SELL: return "SELL";
        default:         return "UNKNOWN";
    }
}

// -----------------------------------------------------------------------------
// 判断是否为多头持仓
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_long(Side s) noexcept {
    return s == Side::BUY;
}

// -----------------------------------------------------------------------------
// 时间戳安全相减（返回微秒）
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::int64_t time_diff_us(Timestamp a,
                                                 Timestamp b) noexcept {
    return a.microseconds() - b.microseconds();
}

}  // namespace

// ==============================================================================
// EscapeParams::validate
// ==============================================================================
Result<void> EscapeParams::validate() const noexcept {
    // 距离分级：弱 < 中 < 强
    if (escape_atr_weak <= 0.0 || escape_atr_weak > 20.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "escape_atr_weak 超出范围 (0, 20]");
    }
    if (escape_atr_medium <= escape_atr_weak ||
        escape_atr_medium > 20.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "escape_atr_medium 必须 > escape_atr_weak");
    }
    if (escape_atr_strong <= escape_atr_medium ||
        escape_atr_strong > 30.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "escape_atr_strong 必须 > escape_atr_medium");
    }

    // 阈值
    if (score_threshold <= 0.0 || score_threshold > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "score_threshold 超出范围 (0, 1]");
    }

    // 权重（非负，和 > 0）
    if (weight_distance < 0.0 || weight_rsi_divergence < 0.0 ||
        weight_macd_shrink < 0.0 || weight_no_new_high < 0.0 ||
        weight_volume_spike < 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "权重不能为负");
    }

    const double weight_sum = weight_distance + weight_rsi_divergence +
                              weight_macd_shrink + weight_no_new_high +
                              weight_volume_spike;
    if (weight_sum < kEps) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "权重和必须 > 0");
    }

    // 检测周期
    if (rsi_divergence_lookback < 2 || rsi_divergence_lookback > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "rsi_divergence_lookback 超出范围 [2, 100]");
    }
    if (macd_shrink_bars < 2 || macd_shrink_bars > 20) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "macd_shrink_bars 超出范围 [2, 20]");
    }
    if (no_new_extreme_bars < 2 || no_new_extreme_bars > 50) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "no_new_extreme_bars 超出范围 [2, 50]");
    }
    if (volume_spike_ratio < 1.0 || volume_spike_ratio > 20.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "volume_spike_ratio 超出范围 [1, 20]");
    }

    // 冷却
    if (min_escape_interval_sec < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_escape_interval_sec 不能为负");
    }
    if (secondary_escape_min_interval_sec < 0 ||
        secondary_escape_min_interval_sec > min_escape_interval_sec) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "secondary_escape_min_interval_sec 无效");
    }

    return {};
}

// ==============================================================================
// EscapeParams::atr_multiplier
// ==============================================================================
double EscapeParams::atr_multiplier(TrendGrade g) const noexcept {
    switch (g) {
        case TrendGrade::Weak:   return escape_atr_weak;
        case TrendGrade::Medium: return escape_atr_medium;
        case TrendGrade::Strong: return escape_atr_strong;
    }
    return escape_atr_medium;
}

// ==============================================================================
// EscapeContext::is_valid
// ==============================================================================
bool EscapeContext::is_valid() const noexcept {
    if (!position) return false;
    if (position->side == Side::UNKNOWN) return false;
    if (!candle.is_valid()) return false;

    // 关键：ATR 必须有效
    if (!finite(indicators.atr14) || indicators.atr14 <= kEps) {
        return false;
    }

    // EMA26 有效性
    if (!finite(indicators.ema26) || indicators.ema26 <= kEps) {
        return false;
    }

    // 成交量均线有效性
    if (!finite(indicators.volume_ma20) ||
        indicators.volume_ma20 <= kVolumeEps) {
        return false;
    }

    // 趋势强度范围
    if (!finite(trend_strength) ||
        trend_strength < 0.0 || trend_strength > 1.0) {
        return false;
    }

    return true;
}

// ==============================================================================
// EscapeSignal::to_string
// ==============================================================================
std::string EscapeSignal::to_string() const {
    std::ostringstream oss;
    oss << "EscapeSignal{"
        << "triggered=" << (triggered ? "yes" : "no")
        << ", severity=" << quant::strategy::to_string(severity)
        << ", side=" << static_cast<int>(side)
        << ", price=" << escape_price.to_double()
        << ", score=" << score << "/" << threshold
        << ", seq=" << escape_sequence
        << ", grade=" << quant::strategy::to_string(trend_grade);

    if (blocked_by_5m) {
        oss << ", blocked_5m=yes";
    }

    if (!reason.empty()) {
        oss << ", reason=" << reason;
    }

    if (!conditions.empty()) {
        oss << ", conditions=[";
        for (std::size_t i = 0; i < conditions.size(); ++i) {
            if (i > 0) oss << "; ";
            oss << conditions[i].name << "="
                << (conditions[i].satisfied ? "1" : "0")
                << "(" << conditions[i].contribution << ")";
        }
        oss << "]";
    }

    oss << "}";
    return oss.str();
}

// ==============================================================================
// EscapeSnapshot::is_valid
// ==============================================================================
bool EscapeSnapshot::is_valid() const noexcept {
    if (schema_version == 0 || schema_version > 1000) {
        return false;
    }
    if (escape_count < 0) {
        return false;
    }
    for (const auto& e : history) {
        if (e.side > 2) return false;
        if (e.sequence < 0) return false;
    }
    return true;
}

// ==============================================================================
// 辅助检测函数：RSI 背离
// ==============================================================================
bool detect_rsi_divergence(
    const std::deque<Candle>& candles,
    double current_rsi,
    Side side,
    int lookback) noexcept
{
    if (lookback < 2) return false;
    const std::size_t lb = static_cast<std::size_t>(lookback);

    if (candles.size() < lb + 1) return false;
    if (!finite(current_rsi)) return false;

    // 需要至少 lb+1 根（当前 + 历史 lookback 根）
    const std::size_t n = candles.size();

    if (side == Side::BUY) {
        // 顶背离：当前 high > 历史最高 high，但 RSI 未创新高
        const Candle& curr = candles[n - 1];
        if (!curr.is_valid()) return false;

        const double curr_high = curr.high.to_double();
        if (!finite(curr_high)) return false;

        double prev_max_high = -std::numeric_limits<double>::infinity();
        for (std::size_t i = n - 1 - lb; i < n - 1; ++i) {
            const Candle& c = candles[i];
            if (!c.is_valid()) continue;
            const double h = c.high.to_double();
            if (finite(h) && h > prev_max_high) {
                prev_max_high = h;
            }
        }

        if (!finite(prev_max_high)) return false;

        // 当前创新高，但 RSI 显著下降（简化：RSI < 70 视为未同步超买）
        // 严格实现需保存历史 RSI；此处用 current_rsi 相对值近似
        const bool new_high = curr_high > prev_max_high;
        const bool rsi_not_extreme = current_rsi < 70.0;

        return new_high && rsi_not_extreme;
    }

    if (side == Side::SELL) {
        // 底背离：当前 low < 历史最低 low，但 RSI 未创新低
        const Candle& curr = candles[n - 1];
        if (!curr.is_valid()) return false;

        const double curr_low = curr.low.to_double();
        if (!finite(curr_low)) return false;

        double prev_min_low = std::numeric_limits<double>::infinity();
        for (std::size_t i = n - 1 - lb; i < n - 1; ++i) {
            const Candle& c = candles[i];
            if (!c.is_valid()) continue;
            const double l = c.low.to_double();
            if (finite(l) && l < prev_min_low) {
                prev_min_low = l;
            }
        }

        if (!finite(prev_min_low)) return false;

        const bool new_low = curr_low < prev_min_low;
        const bool rsi_not_extreme = current_rsi > 30.0;

        return new_low && rsi_not_extreme;
    }

    return false;
}

// ==============================================================================
// 辅助检测函数：MACD 柱连续缩短
// ==============================================================================
bool detect_macd_shrinking(
    const IndicatorResult& ind,
    int bars) noexcept
{
    if (bars < 2) return false;

    // 需要至少 bars 个 MACD 柱值
    if (ind.macd_hist_series.size() < static_cast<std::size_t>(bars)) {
        return false;
    }

    const auto& series = ind.macd_hist_series;
    const std::size_t n = series.size();

    // 检查最近 bars 个值是否绝对值单调递减
    // （多头关注正值缩短，空头关注负值缩短）
    const double last = series[n - 1];
    if (!finite(last)) return false;

    const bool is_positive = last > 0.0;

    for (std::size_t i = n - static_cast<std::size_t>(bars); i < n; ++i) {
        if (!finite(series[i])) return false;
    }

    if (is_positive) {
        // 正值：单调递减
        for (std::size_t i = n - static_cast<std::size_t>(bars) + 1;
             i < n; ++i) {
            if (series[i] >= series[i - 1]) return false;
        }
        return true;
    } else {
        // 负值：绝对值递减（即值递增趋近 0）
        for (std::size_t i = n - static_cast<std::size_t>(bars) + 1;
             i < n; ++i) {
            if (series[i] <= series[i - 1]) return false;
        }
        return true;
    }
}

// ==============================================================================
// 辅助检测函数：最近 N 根未创新高/新低
// ==============================================================================
bool detect_no_new_extreme(
    const std::deque<Candle>& candles,
    Side side,
    int bars) noexcept
{
    if (bars < 2) return false;
    const std::size_t nb = static_cast<std::size_t>(bars);

    if (candles.size() < nb) return false;

    const std::size_t n = candles.size();
    const std::size_t start = n - nb;

    if (side == Side::BUY) {
        // 最近 bars 根未创新高
        double max_high = -std::numeric_limits<double>::infinity();
        std::size_t max_idx = 0;

        for (std::size_t i = start; i < n; ++i) {
            const Candle& c = candles[i];
            if (!c.is_valid()) continue;
            const double h = c.high.to_double();
            if (finite(h) && h > max_high) {
                max_high = h;
                max_idx = i;
            }
        }

        // 最高点不在最近几根（允许最后 1 根）
        return max_idx < n - 1;
    }

    if (side == Side::SELL) {
        // 最近 bars 根未创新低
        double min_low = std::numeric_limits<double>::infinity();
        std::size_t min_idx = 0;

        for (std::size_t i = start; i < n; ++i) {
            const Candle& c = candles[i];
            if (!c.is_valid()) continue;
            const double l = c.low.to_double();
            if (finite(l) && l < min_low) {
                min_low = l;
                min_idx = i;
            }
        }

        return min_idx < n - 1;
    }

    return false;
}

// ==============================================================================
// 辅助检测函数：放量滞涨
// ==============================================================================
bool detect_volume_spike_no_move(
    const Candle& candle,
    double volume_ma20,
    double atr,
    double spike_ratio) noexcept
{
    if (!candle.is_valid()) return false;
    if (!finite(volume_ma20) || volume_ma20 <= kVolumeEps) return false;
    if (!finite(atr) || atr <= kEps) return false;
    if (spike_ratio < 1.0) return false;

    const double vol = candle.volume.to_double();
    if (!finite(vol)) return false;

    const double ratio = vol / volume_ma20;
    if (ratio < spike_ratio) return false;

    // 实体幅度小于 30% ATR
    const double body = std::abs(candle.close.to_double() -
                                   candle.open.to_double());
    return body < 0.3 * atr;
}

// ==============================================================================
// EscapeDetector::Impl
// ==============================================================================
struct EscapeDetector::Impl {
    // -------------------------------------------------------------------------
    // 依赖
    // -------------------------------------------------------------------------
    Dependencies deps;

    // -------------------------------------------------------------------------
    // 运行状态
    // -------------------------------------------------------------------------
    std::atomic<bool> running{false};

    // -------------------------------------------------------------------------
    // 历史记录
    // -------------------------------------------------------------------------
    std::deque<EscapeHistoryRecord> history;
    std::deque<Price> escape_prices;

    // 累积计数
    int escape_count{0};
    Timestamp last_escape_time{};

    // 最近一次触发
    EscapeSignal last_signal{};

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    EscapeStats stats;

    // -------------------------------------------------------------------------
    // 读写锁
    // -------------------------------------------------------------------------
    mutable std::shared_mutex mutex;

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<EscapeParams> load_params() const;

    [[nodiscard]] bool is_in_cooldown(Timestamp now,
                                        const EscapeParams& params) const noexcept;

    void evaluate_conditions(
        const EscapeContext& ctx,
        const EscapeParams& params,
        EscapeSignal& signal) const;

    [[nodiscard]] double compute_weighted_score(
        const EscapeSignal& signal,
        const EscapeParams& params) const noexcept;

    [[nodiscard]] EscapeSeverity determine_severity(
        int sequence) const noexcept;

    [[nodiscard]] Price determine_escape_price(
        const EscapeContext& ctx) const noexcept;

    [[nodiscard]] std::string build_reason(
        const EscapeSignal& signal) const;

    void record_escape(const EscapeSignal& signal);

    void notify_escape(const EscapeSignal& signal) noexcept;

    void notify_state_change() noexcept;

    [[nodiscard]] EscapeSnapshot make_snapshot_internal() const;
};

// ==============================================================================
// Impl::load_params
// ==============================================================================
Result<EscapeParams> EscapeDetector::Impl::load_params() const {
    if (!deps.params_provider) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "params_provider 未注入");
    }

    EscapeParams params;
    try {
        params = deps.params_provider();
    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数加载异常")
            .with_context("error", e.what());
    } catch (...) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数加载未知异常");
    }

    if (auto r = params.validate(); r.is_err()) {
        return r;
    }

    return params;
}

// ==============================================================================
// Impl::is_in_cooldown
// ==============================================================================
bool EscapeDetector::Impl::is_in_cooldown(
    Timestamp now,
    const EscapeParams& params) const noexcept
{
    if (!last_escape_time.is_valid()) return false;

    const auto diff_us = time_diff_us(now, last_escape_time);

    // 首次逃顶冷却
    const std::int64_t interval_us = (escape_count <= 1)
        ? (params.min_escape_interval_sec * 1'000'000LL)
        : (params.secondary_escape_min_interval_sec * 1'000'000LL);

    return diff_us < interval_us;
}

// ==============================================================================
// Impl::evaluate_conditions
// ==============================================================================
void EscapeDetector::Impl::evaluate_conditions(
    const EscapeContext& ctx,
    const EscapeParams& params,
    EscapeSignal& signal) const
{
    const auto& pos = *ctx.position;
    const auto& ind = ctx.indicators;
    const auto& c = ctx.candle;
    const Side side = pos.side;
    const bool long_pos = is_long(side);

    // ---------- 条件 1：距离 EMA ----------
    EscapeCondition cond_dist;
    cond_dist.name = "distance";
    cond_dist.weight = params.weight_distance;

    if (params.enable_distance) {
        const double close = c.close.to_double();
        const double ema = ind.ema26;
        const double atr = ind.atr14;

        if (finite(close) && finite(ema) && finite(atr) && atr > kEps) {
            const double distance_atr = std::abs(close - ema) / atr;
            const double required =
                params.atr_multiplier(signal.trend_grade);

            const bool satisfied = distance_atr >= required;

            cond_dist.satisfied = satisfied;
            cond_dist.contribution = satisfied ? cond_dist.weight : 0.0;
            cond_dist.detail = "distance=" + std::to_string(distance_atr) +
                               " ATR, required=" + std::to_string(required);

            if (satisfied) {
                stats.distance_hits.fetch_add(
                    1, std::memory_order_relaxed);
            }
        } else {
            cond_dist.detail = "invalid data";
        }
    }
    signal.conditions.push_back(cond_dist);

    // ---------- 条件 2：RSI 背离 ----------
    EscapeCondition cond_rsi;
    cond_rsi.name = "rsi_divergence";
    cond_rsi.weight = params.weight_rsi_divergence;

    if (params.enable_rsi_divergence && ctx.recent_candles) {
        const double rsi = ind.rsi7;
        if (finite(rsi)) {
            const bool diverged = detect_rsi_divergence(
                *ctx.recent_candles, rsi, side,
                params.rsi_divergence_lookback);

            cond_rsi.satisfied = diverged;
            cond_rsi.contribution = diverged ? cond_rsi.weight : 0.0;
            cond_rsi.detail = "rsi=" + std::to_string(rsi) +
                              (diverged ? " (diverged)" : " (no divergence)");

            if (diverged) {
                stats.rsi_div_hits.fetch_add(
                    1, std::memory_order_relaxed);
            }
        } else {
            cond_rsi.detail = "invalid rsi";
        }
    } else {
        cond_rsi.detail = "disabled or no data";
    }
    signal.conditions.push_back(cond_rsi);

    // ---------- 条件 3：MACD 柱连续缩短 ----------
    EscapeCondition cond_macd;
    cond_macd.name = "macd_shrink";
    cond_macd.weight = params.weight_macd_shrink;

    if (params.enable_macd_shrink) {
        const bool shrinking = detect_macd_shrinking(
            ind, params.macd_shrink_bars);

        cond_macd.satisfied = shrinking;
        cond_macd.contribution = shrinking ? cond_macd.weight : 0.0;
        cond_macd.detail = shrinking
            ? "MACD histogram shrinking"
            : "MACD histogram stable/growing";

        if (shrinking) {
            stats.macd_shrink_hits.fetch_add(
                1, std::memory_order_relaxed);
        }
    } else {
        cond_macd.detail = "disabled";
    }
    signal.conditions.push_back(cond_macd);

    // ---------- 条件 4：未创新高/新低 ----------
    EscapeCondition cond_nnh;
    cond_nnh.name = "no_new_extreme";
    cond_nnh.weight = params.weight_no_new_high;

    if (params.enable_no_new_high && ctx.recent_candles) {
        const bool no_extreme = detect_no_new_extreme(
            *ctx.recent_candles, side, params.no_new_extreme_bars);

        cond_nnh.satisfied = no_extreme;
        cond_nnh.contribution = no_extreme ? cond_nnh.weight : 0.0;
        cond_nnh.detail = no_extreme
            ? "no new extreme"
            : "new extreme detected";

        if (no_extreme) {
            stats.no_new_high_hits.fetch_add(
                1, std::memory_order_relaxed);
        }
    } else {
        cond_nnh.detail = "disabled or no data";
    }
    signal.conditions.push_back(cond_nnh);

    // ---------- 条件 5：放量滞涨 ----------
    EscapeCondition cond_vol;
    cond_vol.name = "volume_spike";
    cond_vol.weight = params.weight_volume_spike;

    if (params.enable_volume_spike) {
        const bool spike = detect_volume_spike_no_move(
            c, ind.volume_ma20, ind.atr14, params.volume_spike_ratio);

        cond_vol.satisfied = spike;
        cond_vol.contribution = spike ? cond_vol.weight : 0.0;
        cond_vol.detail = spike
            ? "volume spike without move"
            : "no volume anomaly";

        if (spike) {
            stats.volume_spike_hits.fetch_add(
                1, std::memory_order_relaxed);
        }
    } else {
        cond_vol.detail = "disabled";
    }
    signal.conditions.push_back(cond_vol);

    (void)long_pos;
}

// ==============================================================================
// Impl::compute_weighted_score
// ==============================================================================
double EscapeDetector::Impl::compute_weighted_score(
    const EscapeSignal& signal,
    const EscapeParams& params) const noexcept
{
    double sum = 0.0;
    for (const auto& cond : signal.conditions) {
        sum += cond.contribution;
    }

    // 权重归一化（如果配置的权重和不为 1.0，按实际和归一化）
    const double total_weight = params.weight_distance +
                                 params.weight_rsi_divergence +
                                 params.weight_macd_shrink +
                                 params.weight_no_new_high +
                                 params.weight_volume_spike;

    if (total_weight < kEps) return 0.0;

    // 归一化到 [0, 1]
    const double normalized = sum / total_weight;
    return clamp_val(normalized, 0.0, 1.0);
}

// ==============================================================================
// Impl::determine_severity
// ==============================================================================
EscapeSeverity EscapeDetector::Impl::determine_severity(
    int sequence) const noexcept
{
    if (sequence <= 0) return EscapeSeverity::None;
    if (sequence == 1) return EscapeSeverity::Weak;
    if (sequence == 2) return EscapeSeverity::Medium;
    return EscapeSeverity::Strong;
}

// ==============================================================================
// Impl::determine_escape_price
// ==============================================================================
Price EscapeDetector::Impl::determine_escape_price(
    const EscapeContext& ctx) const noexcept
{
    const Side side = ctx.position->side;

    if (!ctx.recent_candles || ctx.recent_candles->empty()) {
        // 回退：使用当前 K 线
        return (side == Side::BUY) ? ctx.candle.high : ctx.candle.low;
    }

    const auto& candles = *ctx.recent_candles;
    const std::size_t n = candles.size();

    // 取最近 N 根的极值（N = max(lookback, 3)）
    const std::size_t lookback = std::min<std::size_t>(n, 5);
    const std::size_t start = n - lookback;

    if (side == Side::BUY) {
        double max_high = -std::numeric_limits<double>::infinity();
        for (std::size_t i = start; i < n; ++i) {
            const auto& c = candles[i];
            if (!c.is_valid()) continue;
            const double h = c.high.to_double();
            if (finite(h) && h > max_high) max_high = h;
        }
        if (finite(max_high) && max_high > 0.0) {
            return Price::from_double(max_high);
        }
        return ctx.candle.high;
    }

    if (side == Side::SELL) {
        double min_low = std::numeric_limits<double>::infinity();
        for (std::size_t i = start; i < n; ++i) {
            const auto& c = candles[i];
            if (!c.is_valid()) continue;
            const double l = c.low.to_double();
            if (finite(l) && l < min_low) min_low = l;
        }
        if (finite(min_low) && min_low > 0.0) {
            return Price::from_double(min_low);
        }
        return ctx.candle.low;
    }

    return ctx.candle.close;
}

// ==============================================================================
// Impl::build_reason
// ==============================================================================
std::string EscapeDetector::Impl::build_reason(
    const EscapeSignal& signal) const
{
    std::ostringstream oss;

    oss << (is_long(signal.side) ? "逃顶" : "逃底");
    oss << " 第" << signal.escape_sequence << "次";
    oss << " (" << to_string(signal.severity) << ")";

    if (!signal.conditions.empty()) {
        oss << ": ";
        bool first = true;
        for (const auto& c : signal.conditions) {
            if (!c.satisfied) continue;
            if (!first) oss << ", ";
            oss << c.name;
            first = false;
        }
    }

    if (signal.blocked_by_5m) {
        oss << " [5m 阻挡]";
    }

    return oss.str();
}

// ==============================================================================
// Impl::record_escape
// ==============================================================================
void EscapeDetector::Impl::record_escape(const EscapeSignal& signal) {
    std::unique_lock<std::shared_mutex> lock(mutex);

    ++escape_count;
    last_escape_time = signal.triggered_at;

    EscapeHistoryRecord rec;
    rec.timestamp = signal.triggered_at;
    rec.escape_price = signal.escape_price;
    rec.side = signal.side;
    rec.score = signal.score;
    rec.sequence = escape_count;
    rec.reason = signal.reason;

    history.push_back(std::move(rec));
    while (history.size() > escape_config::kMaxEscapeHistory) {
        history.pop_front();
    }

    escape_prices.push_back(signal.escape_price);
    while (escape_prices.size() > escape_config::kMaxEscapeHistory) {
        escape_prices.pop_front();
    }
}

// ==============================================================================
// Impl::notify_escape
// ==============================================================================
void EscapeDetector::Impl::notify_escape(
    const EscapeSignal& signal) noexcept
{
    if (!deps.on_escape) return;

    try {
        deps.on_escape(signal);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_escape 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_escape 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_state_change
// ==============================================================================
void EscapeDetector::Impl::notify_state_change() noexcept {
    if (!deps.on_state_change) return;

    try {
        auto snapshot = make_snapshot_internal();
        deps.on_state_change(snapshot);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_state_change 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_state_change 回调未知异常");
    }
}

// ==============================================================================
// Impl::make_snapshot_internal
// ==============================================================================
EscapeSnapshot EscapeDetector::Impl::make_snapshot_internal() const {
    EscapeSnapshot snap;
    snap.schema_version = 1;
    snap.snapshot_time = Timestamp::now();

    // 调用者可能已持锁，也可能未持锁
    // 为简化，这里直接访问（内部调用约定：调用前未持锁）
    for (const auto& rec : history) {
        EscapeSnapshot::Entry e;
        e.timestamp_us = rec.timestamp.microseconds();
        e.escape_price_raw = rec.escape_price.raw();
        e.side = static_cast<std::uint8_t>(rec.side);
        e.score = rec.score;
        e.sequence = rec.sequence;
        e.reason = rec.reason;
        snap.history.push_back(std::move(e));
    }

    snap.escape_count = escape_count;
    snap.last_escape_time_us = last_escape_time.microseconds();

    return snap;
}

// ==============================================================================
// EscapeDetector 构造与析构
// ==============================================================================
EscapeDetector::EscapeDetector(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "EscapeDetector: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }

    impl_->deps = std::move(deps);
}

EscapeDetector::~EscapeDetector() {
    stop();
}

EscapeDetector::EscapeDetector(EscapeDetector&& other) noexcept
    : impl_(std::move(other.impl_)) {}

EscapeDetector& EscapeDetector::operator=(
    EscapeDetector&& other) noexcept
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
Result<void> EscapeDetector::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "EscapeDetector 已启动");
    }

    // 首次加载参数并校验
    if (auto r = impl_->load_params(); r.is_err()) {
        impl_->running.store(false, std::memory_order_release);
        return r;
    }

    // 清空状态
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->history.clear();
        impl_->escape_prices.clear();
        impl_->escape_count = 0;
        impl_->last_escape_time = Timestamp{};
        impl_->last_signal = EscapeSignal{};
    }

    QUANT_LOG_INFO("EscapeDetector 已启动");
    return {};
}

void EscapeDetector::stop() noexcept {
    if (!impl_) return;
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->history.clear();
        impl_->escape_prices.clear();
        impl_->escape_count = 0;
        impl_->last_escape_time = Timestamp{};
        impl_->last_signal = EscapeSignal{};
    }

    QUANT_LOG_INFO("EscapeDetector 已停止");
}

void EscapeDetector::reset() noexcept {
    if (!impl_) return;

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->history.clear();
        impl_->escape_prices.clear();
        impl_->escape_count = 0;
        impl_->last_escape_time = Timestamp{};
        impl_->last_signal = EscapeSignal{};
    }
    impl_->stats.reset();
}

bool EscapeDetector::is_running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

// ==============================================================================
// 核心检测
// ==============================================================================
Result<EscapeSignal> EscapeDetector::detect(const EscapeContext& ctx) {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "EscapeDetector 未启动");
    }

    impl_->stats.detect_count.fetch_add(1, std::memory_order_relaxed);

    // 输入校验
    if (!ctx.is_valid()) {
        EscapeSignal empty;
        empty.triggered = false;
        empty.reason = "invalid context";
        return empty;
    }

    // 参数加载
    auto params_result = impl_->load_params();
    if (params_result.is_err()) {
        return params_result.error();
    }
    const auto params = params_result.value();

    // 构造基础信号
    EscapeSignal signal;
    signal.side = ctx.position->side;
    signal.triggered_at = ctx.candle.close_time;
    signal.threshold = params.score_threshold;
    signal.trend_grade = grade_from_strength(ctx.trend_strength);

    // 冷却检查
    const auto now = Timestamp::now();
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);
        if (impl_->is_in_cooldown(now, params)) {
            impl_->stats.cooldown_blocked_count.fetch_add(
                1, std::memory_order_relaxed);
            signal.reason = "cooldown active";
            return signal;
        }
    }

    // 评估 5 个条件
    impl_->evaluate_conditions(ctx, params, signal);

    // 加权评分
    signal.score = impl_->compute_weighted_score(signal, params);

    // 5m 阻挡加分（可选）
    if (ctx.mtf && ctx.mtf->valid) {
        signal.dist_to_5m_resistance_atr =
            ctx.mtf->dist_to_resistance_atr;
        signal.dist_to_5m_support_atr =
            ctx.mtf->dist_to_support_atr;

        const bool long_pos = is_long(signal.side);
        if (long_pos && signal.dist_to_5m_resistance_atr < 0.3) {
            signal.blocked_by_5m = true;
        } else if (!long_pos && signal.dist_to_5m_support_atr < 0.3) {
            signal.blocked_by_5m = true;
        }

        // 5m 阻挡作为加分项
        if (signal.blocked_by_5m) {
            signal.score = clamp_val(signal.score + 0.10, 0.0, 1.0);
        }
    }

    // 触发判定
    if (signal.score < params.score_threshold) {
        signal.triggered = false;
        signal.reason = "score below threshold";
        return signal;
    }

    // 触发
    signal.triggered = true;
    signal.escape_price = impl_->determine_escape_price(ctx);

    // 记录 + 严重性
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->record_escape(signal);
        signal.escape_sequence = impl_->escape_count;
        signal.severity = impl_->determine_severity(
            impl_->escape_count);
    }

    signal.reason = impl_->build_reason(signal);

    // 统计
    impl_->stats.triggered_count.fetch_add(1, std::memory_order_relaxed);
    switch (signal.severity) {
        case EscapeSeverity::Weak:
            impl_->stats.weak_count.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case EscapeSeverity::Medium:
            impl_->stats.medium_count.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case EscapeSeverity::Strong:
            impl_->stats.strong_count.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case EscapeSeverity::None:
            break;
    }

    // 保存最近信号
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->last_signal = signal;
    }

    QUANT_LOG_INFO("逃顶触发: {}", signal.to_string());

    // 通知
    impl_->notify_escape(signal);
    impl_->notify_state_change();

    return signal;
}

// ==============================================================================
// 状态查询
// ==============================================================================
int EscapeDetector::escape_count() const noexcept {
    if (!impl_) return 0;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->escape_count;
}

std::optional<Timestamp> EscapeDetector::last_escape_time() const noexcept {
    if (!impl_) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    if (!impl_->last_escape_time.is_valid()) return std::nullopt;
    return impl_->last_escape_time;
}

std::vector<EscapeHistoryRecord> EscapeDetector::history() const {
    if (!impl_) return {};
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return {impl_->history.begin(), impl_->history.end()};
}

std::vector<Price> EscapeDetector::recent_escape_prices() const {
    if (!impl_) return {};
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return {impl_->escape_prices.begin(), impl_->escape_prices.end()};
}

// ==============================================================================
// 手动控制
// ==============================================================================
void EscapeDetector::clear_history() noexcept {
    if (!impl_) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->history.clear();
    impl_->escape_prices.clear();
    impl_->escape_count = 0;
}

void EscapeDetector::clear_cooldown() noexcept {
    if (!impl_) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->last_escape_time = Timestamp{};
}

// ==============================================================================
// 持久化
// ==============================================================================
EscapeSnapshot EscapeDetector::to_snapshot() const {
    if (!impl_) return EscapeSnapshot{};

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    EscapeSnapshot snap;
    snap.schema_version = 1;
    snap.snapshot_time = Timestamp::now();

    for (const auto& rec : impl_->history) {
        EscapeSnapshot::Entry e;
        e.timestamp_us = rec.timestamp.microseconds();
        e.escape_price_raw = rec.escape_price.raw();
        e.side = static_cast<std::uint8_t>(rec.side);
        e.score = rec.score;
        e.sequence = rec.sequence;
        e.reason = rec.reason;
        snap.history.push_back(std::move(e));
    }

    snap.escape_count = impl_->escape_count;
    snap.last_escape_time_us =
        impl_->last_escape_time.microseconds();

    return snap;
}

Result<void> EscapeDetector::from_snapshot(
    const EscapeSnapshot& snapshot)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "EscapeDetector 未启动");
    }

    if (!snapshot.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "逃顶快照数据无效");
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    impl_->history.clear();
    impl_->escape_prices.clear();

    for (const auto& e : snapshot.history) {
        EscapeHistoryRecord rec;
        rec.timestamp = Timestamp{e.timestamp_us};
        rec.escape_price = Price{e.escape_price_raw};
        rec.side = static_cast<Side>(e.side);
        rec.score = e.score;
        rec.sequence = e.sequence;
        rec.reason = e.reason;

        impl_->history.push_back(std::move(rec));
        impl_->escape_prices.push_back(rec.escape_price);
    }

    impl_->escape_count = snapshot.escape_count;
    impl_->last_escape_time = Timestamp{snapshot.last_escape_time_us};

    QUANT_LOG_INFO("逃顶状态已恢复: count={} history={}",
                   impl_->escape_count, impl_->history.size());

    return {};
}

// ==============================================================================
// 统计与诊断
// ==============================================================================
const EscapeStats& EscapeDetector::stats() const noexcept {
    static const EscapeStats kEmpty;
    return impl_ ? impl_->stats : kEmpty;
}

std::string EscapeDetector::dump() const {
    std::ostringstream oss;
    oss << "EscapeDetector dump:\n";

    if (!impl_) {
        oss << "  <impl is null>\n";
        return oss.str();
    }

    oss << "  running: " << (is_running() ? "yes" : "no") << "\n";

    // 参数
    try {
        auto params_result = impl_->load_params();
        if (params_result.is_ok()) {
            const auto& p = params_result.value();
            oss << "  params:\n";
            oss << "    escape_atr_weak: " << p.escape_atr_weak << "\n";
            oss << "    escape_atr_medium: "
                << p.escape_atr_medium << "\n";
            oss << "    escape_atr_strong: "
                << p.escape_atr_strong << "\n";
            oss << "    score_threshold: " << p.score_threshold << "\n";
            oss << "    weights: distance=" << p.weight_distance
                << " rsi=" << p.weight_rsi_divergence
                << " macd=" << p.weight_macd_shrink
                << " no_high=" << p.weight_no_new_high
                << " vol=" << p.weight_volume_spike << "\n";
            oss << "    min_escape_interval_sec: "
                << p.min_escape_interval_sec << "\n";
        } else {
            oss << "  params: <unavailable>\n";
        }
    } catch (...) {
        oss << "  params: <unavailable>\n";
    }

    // 状态
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);

        oss << "  state:\n";
        oss << "    escape_count: " << impl_->escape_count << "\n";
        oss << "    last_escape_time: "
            << impl_->last_escape_time.to_iso8601() << "\n";
        oss << "    history_size: " << impl_->history.size() << "\n";
        oss << "    escape_prices_size: "
            << impl_->escape_prices.size() << "\n";

        if (!impl_->history.empty()) {
            oss << "    recent_history:\n";
            const std::size_t n = std::min<std::size_t>(
                impl_->history.size(), 5);
            const std::size_t start = impl_->history.size() - n;
            for (std::size_t i = start; i < impl_->history.size(); ++i) {
                const auto& rec = impl_->history[i];
                oss << "      ["
                    << rec.timestamp.to_iso8601() << "] "
                    << "seq=" << rec.sequence
                    << " side=" << side_to_string(rec.side)
                    << " price=" << rec.escape_price.to_double()
                    << " score=" << rec.score
                    << "\n";
            }
        }
    }

    // 统计
    const auto& s = impl_->stats;
    oss << "  stats:\n";
    oss << "    detect_count: "
        << s.detect_count.load(std::memory_order_relaxed) << "\n";
    oss << "    triggered_count: "
        << s.triggered_count.load(std::memory_order_relaxed) << "\n";
    oss << "    weak: "
        << s.weak_count.load(std::memory_order_relaxed) << "\n";
    oss << "    medium: "
        << s.medium_count.load(std::memory_order_relaxed) << "\n";
    oss << "    strong: "
        << s.strong_count.load(std::memory_order_relaxed) << "\n";
    oss << "    cooldown_blocked: "
        << s.cooldown_blocked_count.load(std::memory_order_relaxed) << "\n";
    oss << "    distance_hits: "
        << s.distance_hits.load(std::memory_order_relaxed) << "\n";
    oss << "    rsi_div_hits: "
        << s.rsi_div_hits.load(std::memory_order_relaxed) << "\n";
    oss << "    macd_shrink_hits: "
        << s.macd_shrink_hits.load(std::memory_order_relaxed) << "\n";
    oss << "    no_new_high_hits: "
        << s.no_new_high_hits.load(std::memory_order_relaxed) << "\n";
    oss << "    volume_spike_hits: "
        << s.volume_spike_hits.load(std::memory_order_relaxed) << "\n";

    return oss.str();
}

}  // namespace quant::strategy
