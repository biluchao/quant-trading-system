// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 指标计算器实现
// ==============================================================================
// @file    src/indicator/indicator.core.calculator.cpp
// @module  indicator
// @type    core
// @name    calculator
// @version 1.0.1
// @brief   IndicatorCalculator 的非内联实现：
//          - build_result: 从内部状态组装 IndicatorResult
//          - compute_batch: 批量计算（回测）
//          - 辅助函数：RSI、结果格式化
//          已修复 12 类运行时问题
//
// 设计原则:
//   - 与头文件严格互补，不重复定义 inline 函数（避免 ODR 违规）
//   - 所有计算结果做 NaN/Inf 检查
//   - 预热期明确标记，避免下游误用
//   - 结果可序列化，支持日志与持久化
// ==============================================================================

#include "indicator/indicator.core.calculator.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace quant {
namespace indicator {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// 从平均涨/跌幅计算 RSI
// -----------------------------------------------------------------------------
[[nodiscard]] double rsi_from_averages(double avg_gain, double avg_loss) noexcept {
    // 两者都为 0：市场完全持平
    if (avg_gain < kEpsilon && avg_loss < kEpsilon) {
        return 50.0;
    }
    // 无下跌：RSI = 100
    if (avg_loss < kEpsilon) {
        return 100.0;
    }
    // 无上涨：RSI = 0
    if (avg_gain < kEpsilon) {
        return 0.0;
    }

    const double rs = avg_gain / avg_loss;
    const double rsi = 100.0 - 100.0 / (1.0 + rs);

    // 边界保护：理论上 rs >= 0，rsi ∈ [0, 100]
    return std::clamp(rsi, 0.0, 100.0);
}

// -----------------------------------------------------------------------------
// 安全除法（分母过小时返回 fallback）
// -----------------------------------------------------------------------------
[[nodiscard]] double safe_div(double num, double den, double fallback = 0.0) noexcept {
    if (std::abs(den) < kEpsilon) return fallback;
    return num / den;
}

// -----------------------------------------------------------------------------
// 浮点有限性检查（NaN/Inf）
// -----------------------------------------------------------------------------
[[nodiscard]] bool is_finite(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 格式化 double（JSON 友好，避免 NaN/Inf 导致 JSON 非法）
// -----------------------------------------------------------------------------
[[nodiscard]] std::string format_double(double v, int precision = 6) {
    if (!std::isfinite(v)) {
        return "null";   // JSON 中 null 表示无效
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

// -----------------------------------------------------------------------------
// 判断指标是否全部就绪
// -----------------------------------------------------------------------------
[[nodiscard]] bool is_ready(const IndicatorState& state,
                             const IndicatorConfig& config) noexcept {
    return state.ema_slow_count    >= config.ema_slow_period
        && state.atr_count         >= config.atr_period
        && state.atr_long_count    >= config.atr_long_period
        && state.rsi_short_count   >= config.rsi_short_period
        && state.rsi_long_count    >= config.rsi_long_period
        && state.macd_slow_count   >= config.macd_slow
        && state.macd_dea_count    >= config.macd_signal
        && state.volume_count      > 0;
}

}  // namespace

// ==============================================================================
// build_result：从内部状态组装结果
// ==============================================================================
IndicatorResult
IndicatorCalculator::build_result(const Candle& c) const noexcept {
    IndicatorResult r;

    // -------------------------------------------------------------------------
    // 1. 元信息
    // -------------------------------------------------------------------------
    r.timestamp = c.open_time;
    r.bar_index = state_.bar_count;

    // -------------------------------------------------------------------------
    // 2. 原始价格
    // -------------------------------------------------------------------------
    r.open   = c.open.to_double();
    r.high   = c.high.to_double();
    r.low    = c.low.to_double();
    r.close  = c.close.to_double();
    r.volume = c.volume.to_double();

    // -------------------------------------------------------------------------
    // 3. 预热与缺口状态
    // -------------------------------------------------------------------------
    const std::size_t warmup = config_.compute_warmup();
    r.is_warmup = (state_.bar_count < warmup);
    r.warmup_bars_remaining = r.is_warmup
        ? (warmup - state_.bar_count)
        : 0;
    r.has_data_gap = has_gap_;

    // -------------------------------------------------------------------------
    // 4. EMA
    // -------------------------------------------------------------------------
    r.ema_fast = state_.ema_fast;
    r.ema_slow = state_.ema_slow;

    // EMA 斜率：最近 5 根的变化量
    if (state_.history_filled >= 5) {
        r.ema_slope = state_.ema_slow_history[0] - state_.ema_slow_history[4];
    } else {
        r.ema_slope = 0.0;
    }

    // -------------------------------------------------------------------------
    // 5. ATR 与波动率
    // -------------------------------------------------------------------------
    r.atr      = state_.atr;
    r.atr_long = state_.atr_long;

    // atr_ratio = ATR14 / ATR50，用于波动率自适应
    r.atr_ratio = safe_div(state_.atr, state_.atr_long, 1.0);

    // clamp 到合理区间，防止极端值污染下游
    r.atr_ratio = std::clamp(r.atr_ratio, 0.1, 10.0);

    // EMA 距离（以 ATR 为单位）
    r.ema_distance = safe_div(r.close - r.ema_slow, state_.atr, 0.0);

    // -------------------------------------------------------------------------
    // 6. RSI
    // -------------------------------------------------------------------------
    if (state_.rsi_short_count >= config_.rsi_short_period) {
        r.rsi_short = rsi_from_averages(
            state_.rsi_short_avg_gain,
            state_.rsi_short_avg_loss);
    } else {
        r.rsi_short = 50.0;   // 未就绪时中性
    }

    if (state_.rsi_long_count >= config_.rsi_long_period) {
        r.rsi_long = rsi_from_averages(
            state_.rsi_long_avg_gain,
            state_.rsi_long_avg_loss);
    } else {
        r.rsi_long = 50.0;
    }

    // -------------------------------------------------------------------------
    // 7. MACD
    // -------------------------------------------------------------------------
    r.macd_dif = state_.macd_dif;
    r.macd_dea = state_.macd_dea;

    const double hist_raw = state_.macd_dif - state_.macd_dea;
    r.macd_hist = config_.macd_hist_times_two
        ? (2.0 * hist_raw)
        : hist_raw;

    // -------------------------------------------------------------------------
    // 8. 成交量
    // -------------------------------------------------------------------------
    if (state_.volume_count > 0) {
        r.volume_ma = state_.volume_sum /
                      static_cast<double>(state_.volume_count);
    } else {
        r.volume_ma = 0.0;
    }

    r.volume_ratio = safe_div(r.volume, r.volume_ma, 1.0);

    // -------------------------------------------------------------------------
    // 9. OBV
    // -------------------------------------------------------------------------
    r.obv = state_.obv;

    if (state_.history_filled >= IndicatorState::kSlopeWindow) {
        r.obv_slope = state_.obv_history[0] -
                      state_.obv_history[IndicatorState::kSlopeWindow - 1];
    } else {
        r.obv_slope = 0.0;
    }

    // -------------------------------------------------------------------------
    // 10. VWAP
    // -------------------------------------------------------------------------
    if (state_.vwap_v_sum > kEpsilon) {
        r.vwap = state_.vwap_pv_sum / state_.vwap_v_sum;
        r.vwap_deviation = safe_div(r.close - r.vwap, r.vwap, 0.0);
    } else {
        r.vwap = r.close;
        r.vwap_deviation = 0.0;
    }

    // -------------------------------------------------------------------------
    // 11. 有效性判定
    // -------------------------------------------------------------------------
    // 条件 1：所有周期计数达标
    const bool counts_ok = is_ready(state_, config_);

    // 条件 2：关键指标有限
    const bool finite_ok = is_finite(r.ema_fast)
                        && is_finite(r.ema_slow)
                        && is_finite(r.atr)
                        && is_finite(r.rsi_short)
                        && is_finite(r.rsi_long)
                        && is_finite(r.macd_dif)
                        && is_finite(r.macd_hist);

    // 条件 3：非预热
    const bool not_warmup = !r.is_warmup;

    r.is_valid = counts_ok && finite_ok && not_warmup;

    // -------------------------------------------------------------------------
    // 12. 兜底：若指标非法，重置为中性值防止污染
    // -------------------------------------------------------------------------
    if (!finite_ok) {
        if (!is_finite(r.ema_fast))  r.ema_fast = r.close;
        if (!is_finite(r.ema_slow))  r.ema_slow = r.close;
        if (!is_finite(r.atr))       r.atr = 0.0;
        if (!is_finite(r.rsi_short)) r.rsi_short = 50.0;
        if (!is_finite(r.rsi_long))  r.rsi_long = 50.0;
        if (!is_finite(r.macd_dif))  r.macd_dif = 0.0;
        if (!is_finite(r.macd_dea))  r.macd_dea = 0.0;
        if (!is_finite(r.macd_hist)) r.macd_hist = 0.0;
        if (!is_finite(r.obv))       r.obv = 0.0;
    }

    return r;
}

// ==============================================================================
// compute_batch：批量计算（回测）
// ==============================================================================
Result<std::vector<IndicatorResult>>
IndicatorCalculator::compute_batch(const std::vector<Candle>& candles) noexcept {
    // 空输入：返回空结果
    if (candles.empty()) {
        return std::vector<IndicatorResult>{};
    }

    // 预分配
    std::vector<IndicatorResult> results;
    results.reserve(candles.size());

    for (std::size_t i = 0; i < candles.size(); ++i) {
        auto r = update(candles[i]);

        if (r.is_err()) {
            // 携带足够上下文便于排查
            return QUANT_ERR_MSG(r.error_code(),
                "批量计算在第 " + std::to_string(i) + " 根 K 线失败")
                .with_context("bar", std::to_string(i))
                .with_context("reason", r.error().to_string())
                .with_context("prev_ts",
                    std::to_string(state_.prev_timestamp.microseconds()))
                .with_context("curr_ts",
                    std::to_string(candles[i].open_time.microseconds()));
        }

        results.push_back(std::move(r).value());
    }

    return results;
}

// ==============================================================================
// to_json / to_string 的实现（如果头文件未内联，此处提供）
// ==============================================================================
// 注意：头文件中已内联 to_json / to_string，此处不重复定义（避免 ODR 违规）
// 如需将实现移到 .cpp，请在头文件中删除 inline 定义

}  // namespace indicator
}  // namespace quant
