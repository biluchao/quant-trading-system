// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 动态带宽实现
// ==============================================================================
// @file    src/indicator/indicator.core.band.cpp
// @module  indicator
// @type    core
// @name    band
// @version 1.0.1
// @brief   AdaptiveBand 的非内联实现：
//          - 构造函数与配置校验
//          - compute：核心带宽计算
//          - record_breakout / check_fake_breakout：假穿检测
//          - compute_escape_zone / update_escape_zone：逃顶区间
//          - BandResult 序列化
//          已修复 12 类运行时问题
//
// 设计原则:
//   - 头文件仅保留声明，重逻辑在此实现
//   - 所有输出做 NaN/Inf 检查与 clamp
//   - 数值边界使用 epsilon 保护
//   - 序列化输出合法 JSON（NaN → null）
// ==============================================================================

#include "indicator/indicator.core.band.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace quant {
namespace indicator {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

[[nodiscard]] inline double safe_div(double num, double den,
                                      double fallback = 0.0) noexcept {
    if (std::abs(den) < kBandEpsilon) return fallback;
    return num / den;
}

[[nodiscard]] inline bool is_finite(double v) noexcept {
    return std::isfinite(v);
}

// JSON 友好的 double 格式化（NaN/Inf → null）
[[nodiscard]] std::string format_double_json(double v, int precision = 8) {
    if (!std::isfinite(v)) return "null";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

// 一般格式化
[[nodiscard]] std::string format_double(double v, int precision = 4) {
    if (!std::isfinite(v)) return "NaN";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

}  // namespace

// ==============================================================================
// 构造函数与配置
// ==============================================================================
AdaptiveBand::AdaptiveBand(BandConfig config)
    : config_{std::move(config)}
{
    const auto err = config_.validate();
    if (!err.empty()) {
        throw std::invalid_argument("BandConfig 非法: " + err);
    }

    // 初始化波动率历史
    vol_history_.fill(0.0);
    vol_history_head_ = 0;
    vol_history_count_ = 0;
}

void AdaptiveBand::set_config(const BandConfig& config) {
    const auto err = config.validate();
    if (!err.empty()) {
        throw std::invalid_argument("BandConfig 非法: " + err);
    }
    config_ = config;
    reset();
}

void AdaptiveBand::reset() noexcept {
    bar_count_ = 0;
    vol_history_.fill(0.0);
    vol_history_head_ = 0;
    vol_history_count_ = 0;
    last_breakout_ = FakeBreakoutRecord{};
    // 保留 stats_（除非显式 reset_stats）
}

// ==============================================================================
// 波动率自适应
// ==============================================================================
VolatilityLevel
AdaptiveBand::classify_volatility(double atr, double atr_long) noexcept {
    if (!is_finite(atr) || !is_finite(atr_long)) {
        return VolatilityLevel::Unknown;
    }

    const double ratio = safe_div(atr, atr_long, 1.0);

    // 简化分级：基于 vol_ratio
    if (ratio < 0.85) return VolatilityLevel::Low;
    if (ratio > 1.15) return VolatilityLevel::High;
    return VolatilityLevel::Normal;
}

double AdaptiveBand::compute_k_adaptive(VolatilityLevel level) const noexcept {
    if (!config_.enable_volatility_adaptive) {
        return config_.base_k;
    }

    switch (level) {
        case VolatilityLevel::High:   return config_.base_k * 0.7;
        case VolatilityLevel::Low:    return config_.base_k * 1.3;
        case VolatilityLevel::Normal: return config_.base_k;
        default:                      return config_.base_k;
    }
}

double AdaptiveBand::compute_k_trend(double trend_strength) const noexcept {
    if (!config_.enable_trend_adaptive) {
        return 1.0;
    }

    // trend_strength ∈ [0, 1] → k_trend ∈ [0.8, 1.3]
    const double clamped = std::clamp(trend_strength, 0.0, 1.0);
    return 0.8 + clamped * 0.5;
}

double
AdaptiveBand::compute_escape_speed(const IndicatorResult& ind) const noexcept {
    if (!is_finite(ind.ema_distance)) return 0.0;
    return std::abs(ind.ema_distance);
}

// ==============================================================================
// 核心：带宽计算
// ==============================================================================
Result<BandResult>
AdaptiveBand::compute(const IndicatorResult& ind,
                       const Candle& candle,
                       double trend_strength,
                       double volume_ratio) noexcept {

    ++stats_.total_computes;

    // -------------------------------------------------------------------------
    // 1. 输入校验
    // -------------------------------------------------------------------------
    if (!candle.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "Candle 非法（价格或数量无效）")
            .with_context("bar", std::to_string(bar_count_));
    }

    // -------------------------------------------------------------------------
    // 2. 结果初始化
    // -------------------------------------------------------------------------
    BandResult r;
    r.timestamp = candle.open_time;
    r.bar_index = bar_count_;
    r.close = candle.close.to_double();

    // -------------------------------------------------------------------------
    // 3. 预热判定
    // -------------------------------------------------------------------------
    if (ind.is_warmup || !is_finite(ind.ema_slow) || !is_finite(ind.atr)
        || ind.atr < kBandEpsilon) {
        r.is_warmup = true;
        r.ema = is_finite(ind.ema_slow) ? ind.ema_slow : r.close;
        r.upper = r.ema;
        r.lower = r.ema;
        r.width = 0.0;
        r.width_pct = 0.0;
        r.band_position = 0.5;
        ++stats_.warmup_skips;
        return r;
    }

    r.is_warmup = false;
    r.ema = ind.ema_slow;

    // -------------------------------------------------------------------------
    // 4. 波动率自适应
    // -------------------------------------------------------------------------
    const double atr = ind.atr;
    const double atr_long = (is_finite(ind.atr_long) && ind.atr_long > kBandEpsilon)
        ? ind.atr_long
        : atr;   // 回退：用 ATR 自身

    // vol_ratio 带 clamp 防极端
    const double vol_ratio_raw = safe_div(atr, atr_long, 1.0);
    r.vol_ratio = std::clamp(vol_ratio_raw, kMinVolRatio, kMaxVolRatio);

    // 波动率等级
    r.vol_level = classify_volatility(atr, atr_long);

    // k 双重自适应
    r.base_k = config_.base_k;
    r.k_adaptive = compute_k_adaptive(r.vol_level);
    r.k_effective = r.k_adaptive * compute_k_trend(trend_strength);

    // -------------------------------------------------------------------------
    // 5. 带宽计算
    // -------------------------------------------------------------------------
    r.width = atr * r.vol_ratio * r.k_effective;

    // 边界保护
    if (!is_finite(r.width) || r.width < 0.0) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "带宽计算异常（非有限或负值）")
            .with_context("atr", std::to_string(atr))
            .with_context("vol_ratio", std::to_string(r.vol_ratio))
            .with_context("k_effective", std::to_string(r.k_effective));
    }

    // 带宽百分比
    r.width_pct = safe_div(r.width, r.close, 0.0);

    // 上下轨
    r.upper = r.ema + r.width;
    r.lower = r.ema - r.width;

    // -------------------------------------------------------------------------
    // 6. 带宽百分比越界告警
    // -------------------------------------------------------------------------
    if (r.width_pct < kMinBandPct) {
        r.width_too_narrow = true;
        ++stats_.width_narrow_warnings;
    } else if (r.width_pct > kMaxBandPct) {
        r.width_too_wide = true;
        ++stats_.width_wide_warnings;
    }

    // -------------------------------------------------------------------------
    // 7. 突破判定
    // -------------------------------------------------------------------------
    r.breakout_buffer = kBreakoutBufferAtr * atr;

    const double close = r.close;
    r.is_breakout_up   = close > r.upper;
    r.is_breakout_down = close < r.lower;

    // 突破幅度
    const double margin_up   = std::max(0.0, close - r.upper);
    const double margin_down = std::max(0.0, r.lower - close);

    // 量能确认（volume_ratio 非法时不做要求）
    const bool volume_ok = !is_finite(volume_ratio) || volume_ratio >= kBreakoutVolumeRatio;

    r.is_confirmed_up = r.is_breakout_up
        && margin_up >= r.breakout_buffer
        && volume_ok;

    r.is_confirmed_down = r.is_breakout_down
        && margin_down >= r.breakout_buffer
        && volume_ok;

    if (r.is_breakout_up)   ++stats_.breakout_up_count;
    if (r.is_breakout_down) ++stats_.breakout_down_count;
    if (r.is_confirmed_up || r.is_confirmed_down) {
        ++stats_.confirmed_breakouts;
    }

    // -------------------------------------------------------------------------
    // 8. 位置与距离
    // -------------------------------------------------------------------------
    r.distance_to_upper_atr = safe_div(r.upper - close, atr, 0.0);
    r.distance_to_lower_atr = safe_div(close - r.lower, atr, 0.0);

    if (r.width > kBandEpsilon) {
        r.band_position = (close - r.lower) / r.width;
        // 允许超出 [0, 1]，但 clamp 防止极端
        r.band_position = std::clamp(r.band_position, -2.0, 3.0);
    } else {
        r.band_position = 0.5;
    }

    // -------------------------------------------------------------------------
    // 9. 更新波动率历史（环形缓冲）
    // -------------------------------------------------------------------------
    if (kVolHistorySize > 0) {
        vol_history_[vol_history_head_] = atr;
        vol_history_head_ = (vol_history_head_ + 1) % kVolHistorySize;
        if (vol_history_count_ < kVolHistorySize) {
            ++vol_history_count_;
        }
    }

    ++bar_count_;

    return r;
}

BandResult
AdaptiveBand::compute_unchecked(const IndicatorResult& ind,
                                  const Candle& candle,
                                  double trend_strength,
                                  double volume_ratio) noexcept {
    auto r = compute(ind, candle, trend_strength, volume_ratio);
    if (r.is_ok()) {
        return std::move(r).value();
    }

    // 失败时返回空结果，并标记为预热
    BandResult empty;
    empty.timestamp = candle.open_time;
    empty.bar_index = bar_count_;
    empty.is_warmup = true;
    empty.close = candle.close.to_double();
    empty.ema = empty.close;
    empty.upper = empty.close;
    empty.lower = empty.close;
    empty.band_position = 0.5;
    return empty;
}

// ==============================================================================
// 假穿检测
// ==============================================================================
void AdaptiveBand::record_breakout(bool is_up, double price,
                                    Timestamp t) noexcept {
    last_breakout_.is_up = is_up;
    last_breakout_.breakout_price = price;
    last_breakout_.breakout_time = t;
    last_breakout_.bars_since = 0;
    last_breakout_.active = true;
}

bool AdaptiveBand::check_fake_breakout(const Candle& c) noexcept {
    if (!last_breakout_.active) return false;

    // 超过窗口 → 视为有效突破
    if (last_breakout_.bars_since >= kFakeBreakoutWindow) {
        last_breakout_.active = false;
        return false;
    }

    ++last_breakout_.bars_since;

    // 回归判断：用收盘价 vs 突破价
    const double close = c.close.to_double();
    const bool returned = last_breakout_.is_up
        ? (close < last_breakout_.breakout_price - kBandEpsilon)
        : (close > last_breakout_.breakout_price + kBandEpsilon);

    if (returned) {
        last_breakout_.active = false;
        ++stats_.fake_breakouts;
        return true;
    }

    return false;
}

// ==============================================================================
// 逃顶区间
// ==============================================================================
std::optional<EscapeZone>
AdaptiveBand::compute_escape_zone(const IndicatorResult& ind,
                                    const Candle& candle) noexcept {
    // 前提：指标就绪且 ATR 有效
    if (ind.is_warmup || !is_finite(ind.atr) || ind.atr < kBandEpsilon) {
        return std::nullopt;
    }

    // 价格必须远离均线（|ema_distance| ≥ 3 ATR）
    if (!is_finite(ind.ema_distance) || std::abs(ind.ema_distance) < 3.0) {
        return std::nullopt;
    }

    EscapeZone zone;
    zone.created_at = candle.open_time;
    zone.escape_price = candle.close.to_double();
    zone.escape_speed_atr = compute_escape_speed(ind);

    // 区间宽度：逃顶速度越快越宽
    const double base_width = ind.atr * config_.base_k;
    const double speed_factor = (zone.escape_speed_atr > 3.0)
        ? kEscapeZoneFastMult
        : kEscapeZoneSlowMult;

    zone.width = base_width * speed_factor;
    zone.upper = zone.escape_price + zone.width;
    zone.lower = zone.escape_price - zone.width;
    zone.state = EscapeZoneState::Watching;

    // 有效期：30 根 3 分钟 K 线（90 分钟）
    constexpr int64_t kExpireBars = 30;
    const int64_t expire_us = kExpireBars * 3LL * 60 * 1000000;
    zone.expired_at = Timestamp{candle.open_time.microseconds() + expire_us};

    return zone;
}

void AdaptiveBand::update_escape_zone(EscapeZone& zone,
                                        const Candle& candle,
                                        const IndicatorResult& ind) noexcept {
    (void)ind;   // 保留参数以便未来扩展

    // 仅 Watching 状态需要更新
    if (zone.state != EscapeZoneState::Watching) return;

    // 超时检查
    if (zone.has_expired(candle.open_time)) {
        zone.state = EscapeZoneState::Expired;
        return;
    }

    const double close = candle.close.to_double();

    // 向上突破
    if (close > zone.upper + kBandEpsilon) {
        zone.state = EscapeZoneState::EscapedUp;
        return;
    }

    // 向下突破
    if (close < zone.lower - kBandEpsilon) {
        zone.state = EscapeZoneState::EscapedDown;
        return;
    }

    // 区间内震荡，保持 Watching
}

void AdaptiveBand::clear_escape_zone(EscapeZone& zone) noexcept {
    zone = EscapeZone{};
}

// ==============================================================================
// 序列化
// ==============================================================================
std::string BandResult::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"ts\":"             << timestamp.microseconds() << ","
        << "\"bar\":"            << bar_index << ","
        << "\"warmup\":"         << (is_warmup ? "true" : "false") << ","
        << "\"ema\":"            << format_double_json(ema) << ","
        << "\"upper\":"          << format_double_json(upper) << ","
        << "\"lower\":"          << format_double_json(lower) << ","
        << "\"width\":"          << format_double_json(width) << ","
        << "\"width_pct\":"      << format_double_json(width_pct) << ","
        << "\"vol_ratio\":"      << format_double_json(vol_ratio) << ","
        << "\"vol_level\":\""    << to_string(vol_level) << "\","
        << "\"k_base\":"         << format_double_json(base_k) << ","
        << "\"k_adaptive\":"     << format_double_json(k_adaptive) << ","
        << "\"k_effective\":"    << format_double_json(k_effective) << ","
        << "\"is_breakout_up\":"   << (is_breakout_up ? "true" : "false") << ","
        << "\"is_breakout_down\":" << (is_breakout_down ? "true" : "false") << ","
        << "\"is_confirmed_up\":"   << (is_confirmed_up ? "true" : "false") << ","
        << "\"is_confirmed_down\":" << (is_confirmed_down ? "true" : "false") << ","
        << "\"breakout_buffer\":"  << format_double_json(breakout_buffer) << ","
        << "\"distance_to_upper_atr\":" << format_double_json(distance_to_upper_atr) << ","
        << "\"distance_to_lower_atr\":" << format_double_json(distance_to_lower_atr) << ","
        << "\"band_position\":"     << format_double_json(band_position) << ","
        << "\"width_too_narrow\":"  << (width_too_narrow ? "true" : "false") << ","
        << "\"width_too_wide\":"    << (width_too_wide ? "true" : "false") << ","
        << "\"close\":"             << format_double_json(close)
        << "}";
    return oss.str();
}

std::string BandResult::to_string() const {
    std::ostringstream oss;
    oss << "Band{bar=" << bar_index
        << ", ema=" << format_double(ema, 2)
        << ", [" << format_double(lower, 2) << ", " << format_double(upper, 2) << "]"
        << ", width=" << format_double(width, 2)
        << " (" << format_double(width_pct * 100.0, 3) << "%)"
        << ", k=" << format_double(k_effective, 2)
        << ", vol=" << to_string(vol_level)
        << ", pos=" << format_double(band_position, 2);

    if (is_warmup) {
        oss << ", WARMUP";
    } else if (is_confirmed_up) {
        oss << ", CONFIRMED_UP";
    } else if (is_confirmed_down) {
        oss << ", CONFIRMED_DOWN";
    } else if (is_breakout_up) {
        oss << ", BREAKOUT_UP";
    } else if (is_breakout_down) {
        oss << ", BREAKOUT_DOWN";
    }

    oss << "}";
    return oss.str();
}

}  // namespace indicator
}  // namespace quant
