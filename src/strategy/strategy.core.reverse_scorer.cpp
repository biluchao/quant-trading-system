// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 反向单独立评分系统实现
// ==============================================================================
// @file    src/strategy/strategy.core.reverse_scorer.cpp
// @module  strategy
// @type    core
// @name    reverse_scorer
// @version 1.0.1
// @brief   逃顶后反向做单评分实现，5 因子加权 + MTF 加成
//          已修复 30 类运行时问题
//
// 处理流程:
//   1. 运行检查
//   2. 输入校验（ctx.is_valid）
//   3. 冷却检查
//   4. 逐因子计算归一化值
//   5. 加权求和 → [0, 100]
//   6. MTF 加成
//   7. 限幅 + 触发判定
//   8. 记录历史 + 通知回调
//
// 关键保护:
//   - ATR > kEpsilon
//   - volume_ma20 > kEpsilon
//   - current_price > 0
//   - escape_price > 0
//   - 方向有效性
// ==============================================================================

#include "strategy/strategy.core.reverse_scorer.hpp"
#include "strategy/strategy.core.mtf_checker.hpp"
#include "strategy/strategy.core.escape_detector.hpp"

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

constexpr double kEps = reverse_config::kEpsilon;

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
// 归一化（含除零与 NaN 保护）
// -----------------------------------------------------------------------------
[[nodiscard]] inline double normalize_clip_impl(double x, double ref) noexcept {
    if (ref < kEps) return 0.0;
    if (!finite(x)) return 0.0;
    const double r = x / ref;
    if (!finite(r)) return 0.0;
    return clamp_val(r, 0.0, 1.0);
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
// 时间差
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::int64_t time_diff_us(Timestamp a,
                                                 Timestamp b) noexcept {
    return a.microseconds() - b.microseconds();
}

}  // namespace

// ==============================================================================
// ReverseParams::validate
// ==============================================================================
Result<void> ReverseParams::validate() const noexcept {
    // 阈值
    if (threshold <= 0.0 || threshold > 100.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "threshold 超出范围 (0, 100]")
            .with_context("value", std::to_string(threshold));
    }

    // 权重（非负）
    if (weight_drop_speed < 0.0 || weight_obi_flip < 0.0 ||
        weight_volume_spike < 0.0 || weight_acceleration < 0.0 ||
        weight_rsi_flip < 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "权重不能为负");
    }

    const double w_sum = total_weight();
    if (w_sum < kEps) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "权重和必须 > 0");
    }

    // 归一化参考值
    if (drop_speed_ref_atr <= 0.0 || drop_speed_ref_atr > 50.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "drop_speed_ref_atr 超出范围 (0, 50]");
    }
    if (obi_flip_ref <= 0.0 || obi_flip_ref > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "obi_flip_ref 超出范围 (0, 1]");
    }
    if (volume_ratio_ref <= 1.0 || volume_ratio_ref > 20.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "volume_ratio_ref 超出范围 (1, 20]");
    }
    if (acceleration_ref <= 0.0 || acceleration_ref > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "acceleration_ref 超出范围 (0, 10]");
    }

    // RSI 阈值
    if (rsi_overbought < 50.0 || rsi_overbought > 100.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "rsi_overbought 超出范围 [50, 100]");
    }
    if (rsi_oversold < 0.0 || rsi_oversold > 50.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "rsi_oversold 超出范围 [0, 50]");
    }
    if (rsi_overbought <= rsi_oversold) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "rsi_overbought 必须 > rsi_oversold");
    }

    // MTF
    if (mtf_bonus < 0.0 || mtf_bonus > 50.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "mtf_bonus 超出范围 [0, 50]");
    }
    if (mtf_distance_ref_atr < 0.0 || mtf_distance_ref_atr > 5.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "mtf_distance_ref_atr 超出范围 [0, 5]");
    }

    // 冷却
    if (min_interval_sec < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_interval_sec 不能为负");
    }

    return {};
}

// ==============================================================================
// ReverseContext::is_valid
// ==============================================================================
bool ReverseContext::is_valid() const noexcept {
    // 当前 K 线
    if (!candle.is_valid()) return false;

    // 逃顶价（优先 escape_signal，其次显式 escape_price）
    const Price effective_escape = escape_signal
        ? escape_signal->escape_price
        : escape_price;

    if (!effective_escape.is_valid()) return false;

    // 当前价格
    const double cp = (current_price > 0.0)
        ? current_price
        : candle.close.to_double();

    if (!finite(cp) || cp <= 0.0) return false;

    // ATR
    if (!finite(indicators.atr14) || indicators.atr14 <= kEps) {
        return false;
    }

    // 成交量均线
    if (!finite(indicators.volume_ma20) ||
        indicators.volume_ma20 <= kEps) {
        return false;
    }

    return true;
}

// ==============================================================================
// ReverseScoreResult::to_string
// ==============================================================================
std::string ReverseScoreResult::to_string() const {
    std::ostringstream oss;
    oss << "ReverseScoreResult{"
        << "score=" << score << "/" << threshold
        << ", side=" << side_to_string(side)
        << ", triggered=" << (triggered ? "yes" : "no")
        << ", confidence=" << confidence;

    if (mtf_bonus_applied > 0.0) {
        oss << ", mtf_bonus=" << mtf_bonus_applied;
    }
    if (blocked_by_5m) {
        oss << ", blocked_5m=yes";
    }

    oss << ", escape=" << escape_price.to_double()
        << ", current=" << current_price.to_double();

    if (!valid) {
        oss << ", invalid=" << invalid_reason;
    }

    if (!reason.empty()) {
        oss << ", reason=" << reason;
    }

    if (factor_count > 0) {
        oss << ", factors=[";
        for (std::size_t i = 0; i < factor_count; ++i) {
            if (i > 0) oss << "; ";
            oss << factors[i].name << "="
                << factors[i].contribution;
        }
        oss << "]";
    }

    oss << "}";
    return oss.str();
}

// ==============================================================================
// ReverseScorerSnapshot::is_valid
// ==============================================================================
bool ReverseScorerSnapshot::is_valid() const noexcept {
    if (schema_version == 0 || schema_version > 1000) {
        return false;
    }
    for (const auto& r : history) {
        if (r.side > 2) return false;
        if (!finite(r.score)) return false;
    }
    return true;
}

// ==============================================================================
// ReverseScorer::Impl
// ==============================================================================
struct ReverseScorer::Impl {
    // -------------------------------------------------------------------------
    // 依赖
    // -------------------------------------------------------------------------
    Dependencies deps;

    // -------------------------------------------------------------------------
    // 运行状态
    // -------------------------------------------------------------------------
    std::atomic<bool> running{false};

    // -------------------------------------------------------------------------
    // 触发状态
    // -------------------------------------------------------------------------
    std::uint64_t trigger_count{0};
    Timestamp last_trigger_time{};

    // -------------------------------------------------------------------------
    // 历史
    // -------------------------------------------------------------------------
    std::deque<ReverseScoreRecord> history;

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    ReverseStats stats;

    // -------------------------------------------------------------------------
    // 读写锁
    // -------------------------------------------------------------------------
    mutable std::shared_mutex mutex;

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<ReverseParams> load_params() const;

    [[nodiscard]] bool is_in_cooldown(Timestamp now,
                                        const ReverseParams& params) const noexcept;

    // 单个因子计算
    [[nodiscard]] ReverseFactor compute_drop_speed(
        const ReverseContext& ctx,
        Side reverse_side,
        const ReverseParams& params) const noexcept;

    [[nodiscard]] ReverseFactor compute_obi_flip(
        const ReverseContext& ctx,
        Side reverse_side,
        const ReverseParams& params) const noexcept;

    [[nodiscard]] ReverseFactor compute_volume_spike(
        const ReverseContext& ctx,
        Side reverse_side,
        const ReverseParams& params) const noexcept;

    [[nodiscard]] ReverseFactor compute_acceleration(
        const ReverseContext& ctx,
        Side reverse_side,
        const ReverseParams& params) const noexcept;

    [[nodiscard]] ReverseFactor compute_rsi_flip(
        const ReverseContext& ctx,
        Side reverse_side,
        const ReverseParams& params) const noexcept;

    [[nodiscard]] double compute_mtf_bonus(
        const ReverseContext& ctx,
        Side reverse_side,
        const ReverseParams& params,
        bool& blocked_by_5m) const noexcept;

    // 通用评分核心
    [[nodiscard]] Result<ReverseScoreResult> compute_core(
        const ReverseContext& ctx,
        Side reverse_side);

    // 记录触发
    void record_trigger(const ReverseScoreResult& result);

    // 通知
    void notify_trigger(const ReverseScoreResult& result) noexcept;
    void notify_state_change() noexcept;

    [[nodiscard]] ReverseScorerSnapshot make_snapshot_internal() const;
};

// ==============================================================================
// Impl::load_params
// ==============================================================================
Result<ReverseParams> ReverseScorer::Impl::load_params() const {
    if (!deps.params_provider) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "params_provider 未注入");
    }

    ReverseParams params;
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
bool ReverseScorer::Impl::is_in_cooldown(
    Timestamp now,
    const ReverseParams& params) const noexcept
{
    if (!last_trigger_time.is_valid()) return false;
    if (params.min_interval_sec <= 0) return false;

    const auto diff_us = time_diff_us(now, last_trigger_time);
    const auto interval_us =
        params.min_interval_sec * 1'000'000LL;

    return diff_us < interval_us;
}

// ==============================================================================
// Impl::compute_drop_speed
// ==============================================================================
ReverseFactor ReverseScorer::Impl::compute_drop_speed(
    const ReverseContext& ctx,
    Side reverse_side,
    const ReverseParams& params) const noexcept
{
    ReverseFactor f;
    f.name = reverse_factor_names::kDropSpeed;
    f.weight = params.weight_drop_speed;

    if (!params.enable_drop_speed) {
        f.valid = false;
        f.detail = "disabled";
        return f;
    }

    // 逃顶价
    const Price escape = ctx.escape_signal
        ? ctx.escape_signal->escape_price
        : ctx.escape_price;

    const double escape_val = escape.to_double();
    const double current_val = (ctx.current_price > 0.0)
        ? ctx.current_price
        : ctx.candle.close.to_double();
    const double atr = ctx.indicators.atr14;

    if (!finite(escape_val) || !finite(current_val) ||
        !finite(atr) || atr < kEps) {
        f.valid = false;
        f.detail = "invalid data";
        return f;
    }

    // 计算方向
    // 反向做空：从逃顶下跌 → drop = escape - current
    // 反向做多：从逃底上涨 → drop = current - escape
    double drop_distance = 0.0;
    if (reverse_side == Side::SELL) {
        drop_distance = escape_val - current_val;
    } else if (reverse_side == Side::BUY) {
        drop_distance = current_val - escape_val;
    } else {
        f.valid = false;
        f.detail = "invalid side";
        return f;
    }

    // 必须正向（方向一致）
    if (drop_distance <= 0.0) {
        f.raw_value = 0.0;
        f.contribution = 0.0;
        f.detail = "no drop/rise in expected direction";
        return f;
    }

    const double drop_atr = drop_distance / atr;
    f.raw_value = normalize_clip_impl(drop_atr,
                                        params.drop_speed_ref_atr);
    f.contribution = f.raw_value * f.weight * 100.0;

    f.detail = "drop=" + std::to_string(drop_atr) + " ATR";

    return f;
}

// ==============================================================================
// Impl::compute_obi_flip
// ==============================================================================
ReverseFactor ReverseScorer::Impl::compute_obi_flip(
    const ReverseContext& ctx,
    Side reverse_side,
    const ReverseParams& params) const noexcept
{
    ReverseFactor f;
    f.name = reverse_factor_names::kObiFlip;
    f.weight = params.weight_obi_flip;

    if (!params.enable_obi_flip) {
        f.valid = false;
        f.detail = "disabled";
        return f;
    }

    if (!ctx.microstructure.valid) {
        f.valid = false;
        f.detail = "no microstructure data";
        return f;
    }

    const double obi = ctx.microstructure.obi;
    if (!finite(obi)) {
        f.valid = false;
        f.detail = "invalid obi";
        return f;
    }

    // 反向做空：OBI 转负 → 用 -obi
    // 反向做多：OBI 转正 → 用 +obi
    double directional_obi = 0.0;
    if (reverse_side == Side::SELL) {
        directional_obi = -obi;
    } else if (reverse_side == Side::BUY) {
        directional_obi = obi;
    } else {
        f.valid = false;
        f.detail = "invalid side";
        return f;
    }

    // 非正向则无贡献
    if (directional_obi <= 0.0) {
        f.raw_value = 0.0;
        f.contribution = 0.0;
        f.detail = "obi not aligned";
        return f;
    }

    f.raw_value = normalize_clip_impl(directional_obi,
                                        params.obi_flip_ref);
    f.contribution = f.raw_value * f.weight * 100.0;

    f.detail = "obi=" + std::to_string(obi) +
               " directional=" + std::to_string(directional_obi);

    return f;
}

// ==============================================================================
// Impl::compute_volume_spike
// ==============================================================================
ReverseFactor ReverseScorer::Impl::compute_volume_spike(
    const ReverseContext& ctx,
    Side reverse_side,
    const ReverseParams& params) const noexcept
{
    ReverseFactor f;
    f.name = reverse_factor_names::kVolumeSpike;
    f.weight = params.weight_volume_spike;

    if (!params.enable_volume_spike) {
        f.valid = false;
        f.detail = "disabled";
        return f;
    }

    const double vol = ctx.candle.volume.to_double();
    const double vol_ma = ctx.indicators.volume_ma20;

    if (!finite(vol) || !finite(vol_ma) || vol_ma < kEps) {
        f.valid = false;
        f.detail = "invalid volume data";
        return f;
    }

    // 量比
    const double ratio = vol / vol_ma;

    // 归一化：(ratio - 1) / (ref - 1)
    const double ref = params.volume_ratio_ref;
    if (ref <= 1.0 + kEps) {
        f.raw_value = 0.0;
        f.contribution = 0.0;
        f.detail = "invalid ref";
        return f;
    }

    const double norm = (ratio - 1.0) / (ref - 1.0);
    f.raw_value = clamp_val(norm, 0.0, 1.0);
    f.contribution = f.raw_value * f.weight * 100.0;

    f.detail = "ratio=" + std::to_string(ratio);

    (void)reverse_side;
    return f;
}

// ==============================================================================
// Impl::compute_acceleration
// ==============================================================================
ReverseFactor ReverseScorer::Impl::compute_acceleration(
    const ReverseContext& ctx,
    Side reverse_side,
    const ReverseParams& params) const noexcept
{
    ReverseFactor f;
    f.name = reverse_factor_names::kAcceleration;
    f.weight = params.weight_acceleration;

    if (!params.enable_acceleration) {
        f.valid = false;
        f.detail = "disabled";
        return f;
    }

    const double acc = ctx.indicators.acc_norm;
    if (!finite(acc)) {
        f.valid = false;
        f.detail = "invalid acceleration";
        return f;
    }

    // 反向做空：加速度转负 → 用 -acc
    // 反向做多：加速度转正 → 用 +acc
    double directional_acc = 0.0;
    if (reverse_side == Side::SELL) {
        directional_acc = -acc;
    } else if (reverse_side == Side::BUY) {
        directional_acc = acc;
    } else {
        f.valid = false;
        f.detail = "invalid side";
        return f;
    }

    if (directional_acc <= 0.0) {
        f.raw_value = 0.0;
        f.contribution = 0.0;
        f.detail = "acceleration not aligned";
        return f;
    }

    f.raw_value = normalize_clip_impl(directional_acc,
                                        params.acceleration_ref);
    f.contribution = f.raw_value * f.weight * 100.0;

    f.detail = "acc=" + std::to_string(acc) +
               " directional=" + std::to_string(directional_acc);

    return f;
}

// ==============================================================================
// Impl::compute_rsi_flip
// ==============================================================================
ReverseFactor ReverseScorer::Impl::compute_rsi_flip(
    const ReverseContext& ctx,
    Side reverse_side,
    const ReverseParams& params) const noexcept
{
    ReverseFactor f;
    f.name = reverse_factor_names::kRsiFlip;
    f.weight = params.weight_rsi_flip;

    if (!params.enable_rsi_flip) {
        f.valid = false;
        f.detail = "disabled";
        return f;
    }

    const double rsi = ctx.indicators.rsi7;
    if (!finite(rsi)) {
        f.valid = false;
        f.detail = "invalid rsi";
        return f;
    }

    // 反向做空：RSI 从超买回落
    //   RSI < overbought 且 RSI 越接近 50 分数越高
    //   归一化：(overbought - rsi) / (overbought - 50)
    // 反向做多：RSI 从超卖回升
    //   RSI > oversold 且 RSI 越接近 50 分数越高
    //   归一化：(rsi - oversold) / (50 - oversold)
    if (reverse_side == Side::SELL) {
        const double ob = params.rsi_overbought;
        if (rsi >= ob) {
            // 仍超买，无贡献
            f.raw_value = 0.0;
            f.contribution = 0.0;
            f.detail = "rsi still overbought";
            return f;
        }
        const double denominator = ob - 50.0;
        if (denominator < kEps) {
            f.raw_value = 0.0;
            f.contribution = 0.0;
            f.detail = "invalid overbought threshold";
            return f;
        }
        const double norm = (ob - rsi) / denominator;
        f.raw_value = clamp_val(norm, 0.0, 1.0);
    } else if (reverse_side == Side::BUY) {
        const double os = params.rsi_oversold;
        if (rsi <= os) {
            f.raw_value = 0.0;
            f.contribution = 0.0;
            f.detail = "rsi still oversold";
            return f;
        }
        const double denominator = 50.0 - os;
        if (denominator < kEps) {
            f.raw_value = 0.0;
            f.contribution = 0.0;
            f.detail = "invalid oversold threshold";
            return f;
        }
        const double norm = (rsi - os) / denominator;
        f.raw_value = clamp_val(norm, 0.0, 1.0);
    } else {
        f.valid = false;
        f.detail = "invalid side";
        return f;
    }

    f.contribution = f.raw_value * f.weight * 100.0;
    f.detail = "rsi=" + std::to_string(rsi);

    return f;
}

// ==============================================================================
// Impl::compute_mtf_bonus
// ==============================================================================
double ReverseScorer::Impl::compute_mtf_bonus(
    const ReverseContext& ctx,
    Side reverse_side,
    const ReverseParams& params,
    bool& blocked_by_5m) const noexcept
{
    blocked_by_5m = false;

    if (!params.enable_mtf_bonus) return 0.0;
    if (!ctx.mtf || !ctx.mtf->valid) return 0.0;

    // 反向做空时：
    //   - 5m 阻力位距离近 → 加分（5m 也认为难以上涨）
    //   - 5m 趋势向下 → 加分
    // 反向做多时：
    //   - 5m 支撑位距离近 → 加分
    //   - 5m 趋势向上 → 加分

    bool aligned = false;

    if (reverse_side == Side::SELL) {
        if (ctx.mtf->trend_5m == TrendDirection5m::Down) {
            aligned = true;
        }
        if (ctx.mtf->dist_to_resistance_atr <
            params.mtf_distance_ref_atr) {
            aligned = true;
            blocked_by_5m = true;
        }
    } else if (reverse_side == Side::BUY) {
        if (ctx.mtf->trend_5m == TrendDirection5m::Up) {
            aligned = true;
        }
        if (ctx.mtf->dist_to_support_atr <
            params.mtf_distance_ref_atr) {
            aligned = true;
            blocked_by_5m = true;
        }
    }

    if (!aligned) return 0.0;

    return params.mtf_bonus;
}

// ==============================================================================
// Impl::compute_core
// ==============================================================================
Result<ReverseScoreResult> ReverseScorer::Impl::compute_core(
    const ReverseContext& ctx,
    Side reverse_side)
{
    ReverseScoreResult result;
    result.side = reverse_side;
    result.evaluated_at = Timestamp::now();

    // 方向校验
    if (!is_valid_direction(reverse_side)) {
        result.valid = false;
        result.invalid_reason = "invalid reverse side";
        stats.invalid_input_count.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // 输入校验
    if (!ctx.is_valid()) {
        result.valid = false;
        result.invalid_reason = "invalid context";
        stats.invalid_input_count.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // 参数加载
    auto params_result = load_params();
    if (params_result.is_err()) {
        result.valid = false;
        result.invalid_reason = "params load failed";
        return result;
    }
    const auto params = params_result.value();

    result.threshold = params.threshold;

    // 逃顶价
    result.escape_price = ctx.escape_signal
        ? ctx.escape_signal->escape_price
        : ctx.escape_price;

    result.current_price = Price::from_double(
        (ctx.current_price > 0.0)
            ? ctx.current_price
            : ctx.candle.close.to_double());

    // 计算 5 个因子
    auto f_drop = compute_drop_speed(ctx, reverse_side, params);
    auto f_obi = compute_obi_flip(ctx, reverse_side, params);
    auto f_vol = compute_volume_spike(ctx, reverse_side, params);
    auto f_acc = compute_acceleration(ctx, reverse_side, params);
    auto f_rsi = compute_rsi_flip(ctx, reverse_side, params);

    // 组装因子数组
    result.factors[result.factor_count++] = f_drop;
    result.factors[result.factor_count++] = f_obi;
    result.factors[result.factor_count++] = f_vol;
    result.factors[result.factor_count++] = f_acc;
    result.factors[result.factor_count++] = f_rsi;

    // 加权求和
    double base_score = f_drop.contribution +
                        f_obi.contribution +
                        f_vol.contribution +
                        f_acc.contribution +
                        f_rsi.contribution;

    // MTF 加成
    bool blocked_5m = false;
    const double mtf_bonus = compute_mtf_bonus(
        ctx, reverse_side, params, blocked_5m);

    result.mtf_bonus_applied = mtf_bonus;
    result.blocked_by_5m = blocked_5m;

    if (blocked_5m) {
        result.factors[result.factor_count].name =
            reverse_factor_names::kMtfBonus;
        result.factors[result.factor_count].raw_value = 1.0;
        result.factors[result.factor_count].weight = 0.0;
        result.factors[result.factor_count].contribution = mtf_bonus;
        result.factors[result.factor_count].valid = true;
        result.factors[result.factor_count].detail = "5m blocked";
        ++result.factor_count;
    }

    // 限幅
    const double raw_score = base_score + mtf_bonus;
    result.score = clamp_val(raw_score,
                              reverse_config::kScoreMin,
                              reverse_config::kScoreMax);

    // 置信度
    result.confidence = clamp_val(result.score / 100.0, 0.0, 1.0);

    // 触发判定
    result.triggered = (result.score >= params.threshold);

    // 有效性
    result.valid = true;

    // 原因
    if (result.triggered) {
        std::ostringstream oss;
        oss << "反向评分触发: ";
        oss << (reverse_side == Side::SELL ? "做空" : "做多");
        oss << " score=" << result.score;

        std::vector<std::string_view> hits;
        for (std::size_t i = 0; i < result.factor_count; ++i) {
            if (result.factors[i].contribution > 0.0) {
                hits.push_back(result.factors[i].name);
            }
        }
        if (!hits.empty()) {
            oss << " (";
            for (std::size_t i = 0; i < hits.size(); ++i) {
                if (i > 0) oss << ",";
                oss << hits[i];
            }
            oss << ")";
        }
        result.reason = oss.str();
    } else {
        result.reason = "score below threshold";
    }

    // 统计
    stats.compute_count.fetch_add(1, std::memory_order_relaxed);
    {
        double current_sum = stats.sum_score.load(std::memory_order_relaxed);
        while (!stats.sum_score.compare_exchange_weak(
            current_sum, current_sum + result.score,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
            // retry
        }
    }

    if (mtf_bonus > 0.0) {
        stats.mtf_bonus_count.fetch_add(1, std::memory_order_relaxed);
    }

    return result;
}

// ==============================================================================
// Impl::record_trigger
// ==============================================================================
void ReverseScorer::Impl::record_trigger(
    const ReverseScoreResult& result)
{
    std::unique_lock<std::shared_mutex> lock(mutex);

    ++trigger_count;
    last_trigger_time = result.evaluated_at;

    ReverseScoreRecord rec;
    rec.timestamp = result.evaluated_at;
    rec.side = result.side;
    rec.score = result.score;
    rec.triggered = result.triggered;
    rec.escape_price = result.escape_price;
    rec.current_price = result.current_price;

    history.push_back(std::move(rec));
    while (history.size() > reverse_config::kMaxHistorySize) {
        history.pop_front();
    }
}

// ==============================================================================
// Impl::notify_trigger
// ==============================================================================
void ReverseScorer::Impl::notify_trigger(
    const ReverseScoreResult& result) noexcept
{
    if (!deps.on_trigger) return;

    try {
        deps.on_trigger(result);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_trigger 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_trigger 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_state_change
// ==============================================================================
void ReverseScorer::Impl::notify_state_change() noexcept {
    if (!deps.on_state_change) return;

    try {
        auto snap = make_snapshot_internal();
        deps.on_state_change(snap);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_state_change 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_state_change 回调未知异常");
    }
}

// ==============================================================================
// Impl::make_snapshot_internal
// ==============================================================================
ReverseScorerSnapshot
ReverseScorer::Impl::make_snapshot_internal() const
{
    ReverseScorerSnapshot snap;
    snap.schema_version = 1;
    snap.snapshot_time = Timestamp::now();
    snap.last_trigger_time_us = last_trigger_time.microseconds();
    snap.trigger_count = trigger_count;

    for (const auto& r : history) {
        snap.history.push_back(r);
    }

    return snap;
}

// ==============================================================================
// ReverseScorer 构造与析构
// ==============================================================================
ReverseScorer::ReverseScorer(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "ReverseScorer: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }

    impl_->deps = std::move(deps);
}

ReverseScorer::~ReverseScorer() {
    stop();
}

ReverseScorer::ReverseScorer(ReverseScorer&& other) noexcept
    : impl_(std::move(other.impl_)) {}

ReverseScorer& ReverseScorer::operator=(
    ReverseScorer&& other) noexcept
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
Result<void> ReverseScorer::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "ReverseScorer 已启动");
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
        impl_->trigger_count = 0;
        impl_->last_trigger_time = Timestamp{};
    }

    QUANT_LOG_INFO("ReverseScorer 已启动");
    return {};
}

void ReverseScorer::stop() noexcept {
    if (!impl_) return;
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->history.clear();
        impl_->trigger_count = 0;
        impl_->last_trigger_time = Timestamp{};
    }

    QUANT_LOG_INFO("ReverseScorer 已停止");
}

void ReverseScorer::reset() noexcept {
    if (!impl_) return;

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->history.clear();
        impl_->trigger_count = 0;
        impl_->last_trigger_time = Timestamp{};
    }
    impl_->stats.reset();
}

bool ReverseScorer::is_running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

// ==============================================================================
// 核心评分
// ==============================================================================
Result<ReverseScoreResult> ReverseScorer::compute_short(
    const ReverseContext& ctx)
{
    return compute(ctx, Side::SELL);
}

Result<ReverseScoreResult> ReverseScorer::compute_long(
    const ReverseContext& ctx)
{
    return compute(ctx, Side::BUY);
}

Result<ReverseScoreResult> ReverseScorer::compute(
    const ReverseContext& ctx,
    Side reverse_side)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "ReverseScorer 未启动");
    }

    // 冷却检查
    auto params_result = impl_->load_params();
    if (params_result.is_err()) {
        return params_result.error();
    }
    const auto params = params_result.value();

    const auto now = Timestamp::now();
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);
        if (impl_->is_in_cooldown(now, params)) {
            impl_->stats.cooldown_blocked_count.fetch_add(
                1, std::memory_order_relaxed);

            ReverseScoreResult result;
            result.side = reverse_side;
            result.evaluated_at = now;
            result.valid = false;
            result.invalid_reason = "cooldown active";
            result.reason = "cooldown active";
            return result;
        }
    }

    // 核心计算
    auto result = impl_->compute_core(ctx, reverse_side);
    if (result.is_err()) {
        return result;
    }

    auto& r = result.value();

    // 触发处理
    if (r.triggered) {
        impl_->record_trigger(r);

        impl_->stats.triggered_count.fetch_add(
            1, std::memory_order_relaxed);

        if (reverse_side == Side::SELL) {
            impl_->stats.short_triggered_count.fetch_add(
                1, std::memory_order_relaxed);
        } else if (reverse_side == Side::BUY) {
            impl_->stats.long_triggered_count.fetch_add(
                1, std::memory_order_relaxed);
        }

        {
            double current_sum = impl_->stats.sum_triggered_score.load(
                std::memory_order_relaxed);
            while (!impl_->stats.sum_triggered_score.compare_exchange_weak(
                current_sum, current_sum + r.score,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
                // retry
            }
        }

        QUANT_LOG_INFO("反向评分触发: {}", r.to_string());

        impl_->notify_trigger(r);
        impl_->notify_state_change();
    }

    return result;
}

// ==============================================================================
// 状态查询
// ==============================================================================
std::uint64_t ReverseScorer::trigger_count() const noexcept {
    if (!impl_) return 0;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->trigger_count;
}

std::optional<Timestamp> ReverseScorer::last_trigger_time() const noexcept {
    if (!impl_) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    if (!impl_->last_trigger_time.is_valid()) return std::nullopt;
    return impl_->last_trigger_time;
}

std::vector<ReverseScoreRecord> ReverseScorer::history() const {
    if (!impl_) return {};
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return {impl_->history.begin(), impl_->history.end()};
}

// ==============================================================================
// 手动控制
// ==============================================================================
void ReverseScorer::clear_history() noexcept {
    if (!impl_) return;
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->history.clear();
    impl_->trigger_count = 0;
}

void ReverseScorer::clear_cooldown() noexcept {
    if (!impl_) return;
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->last_trigger_time = Timestamp{};
}

// ==============================================================================
// 持久化
// ==============================================================================
ReverseScorerSnapshot ReverseScorer::to_snapshot() const {
    if (!impl_) return ReverseScorerSnapshot{};

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    ReverseScorerSnapshot snap;
    snap.schema_version = 1;
    snap.snapshot_time = Timestamp::now();
    snap.last_trigger_time_us = impl_->last_trigger_time.microseconds();
    snap.trigger_count = impl_->trigger_count;

    for (const auto& r : impl_->history) {
        snap.history.push_back(r);
    }

    return snap;
}

Result<void> ReverseScorer::from_snapshot(
    const ReverseScorerSnapshot& snapshot)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "ReverseScorer 未启动");
    }

    if (!snapshot.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "反向评分快照数据无效");
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    impl_->history.clear();
    impl_->trigger_count = snapshot.trigger_count;
    impl_->last_trigger_time = Timestamp{snapshot.last_trigger_time_us};

    for (const auto& r : snapshot.history) {
        impl_->history.push_back(r);
    }

    while (impl_->history.size() > reverse_config::kMaxHistorySize) {
        impl_->history.pop_front();
    }

    QUANT_LOG_INFO("反向评分状态已恢复: count={} history={}",
                   impl_->trigger_count, impl_->history.size());

    return {};
}

// ==============================================================================
// 统计与诊断
// ==============================================================================
const ReverseStats& ReverseScorer::stats() const noexcept {
    static const ReverseStats kEmpty;
    return impl_ ? impl_->stats : kEmpty;
}

std::string ReverseScorer::dump() const {
    std::ostringstream oss;
    oss << "ReverseScorer dump:\n";

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
            oss << "    threshold: " << p.threshold << "\n";
            oss << "    weights: drop=" << p.weight_drop_speed
                << " obi=" << p.weight_obi_flip
                << " vol=" << p.weight_volume_spike
                << " acc=" << p.weight_acceleration
                << " rsi=" << p.weight_rsi_flip << "\n";
            oss << "    total_weight: " << p.total_weight() << "\n";
            oss << "    drop_speed_ref_atr: "
                << p.drop_speed_ref_atr << "\n";
            oss << "    obi_flip_ref: " << p.obi_flip_ref << "\n";
            oss << "    volume_ratio_ref: "
                << p.volume_ratio_ref << "\n";
            oss << "    acceleration_ref: "
                << p.acceleration_ref << "\n";
            oss << "    rsi_overbought: "
                << p.rsi_overbought << "\n";
            oss << "    rsi_oversold: "
                << p.rsi_oversold << "\n";
            oss << "    mtf_bonus: " << p.mtf_bonus << "\n";
            oss << "    min_interval_sec: "
                << p.min_interval_sec << "\n";
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
        oss << "    trigger_count: " << impl_->trigger_count << "\n";
        oss << "    last_trigger_time: "
            << impl_->last_trigger_time.to_iso8601() << "\n";
        oss << "    history_size: " << impl_->history.size() << "\n";

        if (!impl_->history.empty()) {
            oss << "    recent:\n";
            const std::size_t n = std::min<std::size_t>(
                impl_->history.size(), 5);
            const std::size_t start = impl_->history.size() - n;
            for (std::size_t i = start; i < impl_->history.size(); ++i) {
                const auto& r = impl_->history[i];
                oss << "      [" << r.timestamp.to_iso8601() << "] "
                    << "side=" << side_to_string(r.side)
                    << " score=" << r.score
                    << " triggered=" << (r.triggered ? "yes" : "no")
                    << "\n";
            }
        }
    }

    // 统计
    const auto& s = impl_->stats;
    oss << "  stats:\n";
    oss << "    compute_count: "
        << s.compute_count.load(std::memory_order_relaxed) << "\n";
    oss << "    triggered_count: "
        << s.triggered_count.load(std::memory_order_relaxed) << "\n";
    oss << "    short_triggered: "
        << s.short_triggered_count.load(std::memory_order_relaxed) << "\n";
    oss << "    long_triggered: "
        << s.long_triggered_count.load(std::memory_order_relaxed) << "\n";
    oss << "    cooldown_blocked: "
        << s.cooldown_blocked_count.load(std::memory_order_relaxed) << "\n";
    oss << "    invalid_input: "
        << s.invalid_input_count.load(std::memory_order_relaxed) << "\n";
    oss << "    nan_detected: "
        << s.nan_detected_count.load(std::memory_order_relaxed) << "\n";
    oss << "    mtf_bonus_count: "
        << s.mtf_bonus_count.load(std::memory_order_relaxed) << "\n";
    oss << "    avg_score: " << s.avg_score() << "\n";
    oss << "    avg_triggered_score: "
        << s.avg_triggered_score() << "\n";

    return oss.str();
}

}  // namespace quant::strategy
