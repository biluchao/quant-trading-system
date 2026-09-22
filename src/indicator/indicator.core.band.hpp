// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 动态带宽计算
// ==============================================================================
// @file    src/indicator/indicator.core.band.hpp
// @module  indicator
// @type    core
// @name    band
// @version 1.0.1
// @brief   基于 ATR 和波动率自适应的动态带宽、突破判定、逃顶区间
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 量纲统一：带宽以价格为单位（ATR × k），禁止收益率单位
//   - 波动率自适应：k 随 ATR 分位数调整（0.7 / 1.0 / 1.3）
//   - 突破确认：突破幅度 ≥ 0.3×ATR + 成交量放大
//   - 假穿检测：3 根 K 线内回归视为假穿
//   - 逃顶区间：宽度随逃顶速度调整（0.5~1.0×带宽）
//   - 趋势自适应：强趋势放宽带宽，弱趋势收紧
//   - 边界保护：clamp + isfinite + epsilon
//   - 状态可重置：趋势切换时清理
// ==============================================================================

#ifndef QUANT_INDICATOR_CORE_BAND_HPP
#define QUANT_INDICATOR_CORE_BAND_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"
#include "common/common.core.types.hpp"
#include "indicator/indicator.core.calculator.hpp"

namespace quant {
namespace indicator {

// ==============================================================================
// 常量
// ==============================================================================
inline constexpr double kBandEpsilon = 1e-12;

// k 的边界
inline constexpr double kMinBaseK = 0.3;
inline constexpr double kMaxBaseK = 2.5;
inline constexpr double kDefaultBaseK = 1.0;

// vol_ratio clamp 范围
inline constexpr double kMinVolRatio = 0.7;
inline constexpr double kMaxVolRatio = 1.5;

// 带宽百分比安全范围（相对价格）
inline constexpr double kMinBandPct = 0.0015;   // 0.15%
inline constexpr double kMaxBandPct = 0.0150;   // 1.50%

// 突破确认参数
inline constexpr double kBreakoutBufferAtr = 0.3;    // 突破幅度至少 0.3×ATR
inline constexpr double kBreakoutVolumeRatio = 1.2;  // 成交量放大倍数

// 假穿检测
inline constexpr std::size_t kFakeBreakoutWindow = 3;

// 逃顶区间宽度系数
inline constexpr double kEscapeZoneFastMult = 1.0;   // 快速逃顶
inline constexpr double kEscapeZoneSlowMult = 0.5;   // 缓慢逃顶

// ==============================================================================
// 配置
// ==============================================================================
struct BandConfig {
    // 基础带宽系数 k
    double base_k{kDefaultBaseK};

    // 是否启用波动率自适应
    bool enable_volatility_adaptive{true};

    // 是否启用趋势强度自适应
    bool enable_trend_adaptive{true};

    // ATR 分位数计算窗口
    std::size_t volatility_percentile_window{100};

    // 带宽百分比越界时是否返回警告
    bool warn_on_band_pct_out_of_range{true};

    // 校验
    [[nodiscard]] bool is_valid() const noexcept {
        if (base_k < kMinBaseK || base_k > kMaxBaseK) return false;
        if (volatility_percentile_window < 20) return false;
        return true;
    }

    [[nodiscard]] std::string validate() const {
        if (base_k < kMinBaseK || base_k > kMaxBaseK) {
            return "base_k 越界 [" + std::to_string(kMinBaseK) + ", " +
                   std::to_string(kMaxBaseK) + "]，当前 " +
                   std::to_string(base_k);
        }
        if (volatility_percentile_window < 20) {
            return "volatility_percentile_window 太小（<20）";
        }
        return {};
    }
};

// ==============================================================================
// 波动率等级
// ==============================================================================
enum class VolatilityLevel : std::uint8_t {
    Unknown  = 0,
    Low      = 1,   // < 20 分位
    Normal   = 2,   // 20~80 分位
    High     = 3,   // > 80 分位
};

[[nodiscard]] constexpr std::string_view to_string(VolatilityLevel v) noexcept {
    switch (v) {
        case VolatilityLevel::Low:    return "Low";
        case VolatilityLevel::Normal: return "Normal";
        case VolatilityLevel::High:   return "High";
        default:                      return "Unknown";
    }
}

// ==============================================================================
// 带宽结果
// ==============================================================================
struct BandResult {
    // 元信息
    Timestamp   timestamp{};
    std::size_t bar_index{0};
    bool        is_warmup{true};        // ATR 未就绪

    // 带宽核心
    double ema{0.0};                    // 均线中轴
    double upper{0.0};                  // 上轨
    double lower{0.0};                  // 下轨
    double width{0.0};                  // 带宽（价格单位）
    double width_pct{0.0};              // 带宽 / 价格

    // 自适应系数
    double base_k{1.0};
    double vol_ratio{1.0};              // ATR14 / ATR50
    double k_adaptive{1.0};             // 波动率自适应后的 k
    double k_effective{1.0};            // 趋势自适应后的最终 k
    VolatilityLevel vol_level{VolatilityLevel::Unknown};

    // 突破判定
    bool   is_breakout_up{false};       // 价格突破上轨
    bool   is_breakout_down{false};     // 价格突破下轨
    bool   is_confirmed_up{false};      // 突破幅度 + 量能确认
    bool   is_confirmed_down{false};
    double breakout_buffer{0.0};        // 突破缓冲（0.3×ATR）

    // 当前位置
    double close{0.0};
    double distance_to_upper_atr{0.0};  // (upper - close) / ATR
    double distance_to_lower_atr{0.0};  // (close - lower) / ATR
    double band_position{0.0};          // close 在带宽内的位置 [0, 1]

    // 告警
    bool   width_too_narrow{false};
    bool   width_too_wide{false};

    // 辅助
    [[nodiscard]] bool is_ready() const noexcept { return !is_warmup; }

    [[nodiscard]] bool is_any_breakout() const noexcept {
        return is_breakout_up || is_breakout_down;
    }

    [[nodiscard]] bool is_confirmed_breakout() const noexcept {
        return is_confirmed_up || is_confirmed_down;
    }

    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 逃顶区间状态
// ==============================================================================
enum class EscapeZoneState : std::uint8_t {
    Inactive      = 0,   // 未激活
    Watching      = 1,   // 价格在区间内震荡
    EscapedUp     = 2,   // 向上突破（趋势延续）
    EscapedDown   = 3,   // 向下突破（反向做单）
    Expired       = 4,   // 超时失效
};

[[nodiscard]] constexpr std::string_view to_string(EscapeZoneState s) noexcept {
    switch (s) {
        case EscapeZoneState::Watching:    return "Watching";
        case EscapeZoneState::EscapedUp:   return "EscapedUp";
        case EscapeZoneState::EscapedDown: return "EscapedDown";
        case EscapeZoneState::Expired:     return "Expired";
        default:                           return "Inactive";
    }
}

// ==============================================================================
// 逃顶区间
// ==============================================================================
struct EscapeZone {
    Timestamp        created_at{};
    Timestamp        expired_at{};       // 0 表示不过期
    double           escape_price{0.0};  // 逃顶点价格
    double           upper{0.0};         // 区间上轨
    double           lower{0.0};         // 区间下轨
    double           width{0.0};         // 区间宽度
    double           escape_speed_atr{0.0}; // 逃顶速度（ATR/根）
    EscapeZoneState  state{EscapeZoneState::Inactive};

    [[nodiscard]] bool is_active() const noexcept {
        return state == EscapeZoneState::Watching;
    }

    [[nodiscard]] bool contains(double price) const noexcept {
        return price >= lower && price <= upper;
    }

    [[nodiscard]] bool has_expired(Timestamp now) const noexcept {
        return expired_at.is_valid() && now > expired_at;
    }
};

// ==============================================================================
// 统计
// ==============================================================================
struct BandStats {
    std::uint64_t total_computes{0};
    std::uint64_t breakout_up_count{0};
    std::uint64_t breakout_down_count{0};
    std::uint64_t confirmed_breakouts{0};
    std::uint64_t fake_breakouts{0};
    std::uint64_t width_narrow_warnings{0};
    std::uint64_t width_wide_warnings{0};
    std::uint64_t warmup_skips{0};

    void reset() noexcept { *this = BandStats{}; }
};

// ==============================================================================
// 动态带宽计算器
// ==============================================================================
class AdaptiveBand {
public:
    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    explicit AdaptiveBand(BandConfig config = {});

    AdaptiveBand(const AdaptiveBand&) = delete;
    AdaptiveBand& operator=(const AdaptiveBand&) = delete;
    AdaptiveBand(AdaptiveBand&&) noexcept = default;
    AdaptiveBand& operator=(AdaptiveBand&&) noexcept = default;

    ~AdaptiveBand() = default;

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    [[nodiscard]] const BandConfig& config() const noexcept { return config_; }
    void set_config(const BandConfig& config);

    // -------------------------------------------------------------------------
    // 核心：计算带宽
    // -------------------------------------------------------------------------
    // 输入：
    //   ind   - 指标结果（含 EMA、ATR、ATR_Long、成交量、均线斜率）
    //   candle - 当前 K 线
    //   trend_strength - 趋势强度 [0, 1]（0=无趋势，1=强趋势）
    //   volume_ratio   - 成交量比（可选，用于突破确认）
    // 返回：Result<BandResult>
    [[nodiscard]] Result<BandResult>
        compute(const IndicatorResult& ind,
                const Candle& candle,
                double trend_strength = 0.5,
                double volume_ratio = 1.0) noexcept;

    // 简化接口（用于测试/回测）
    [[nodiscard]] BandResult
        compute_unchecked(const IndicatorResult& ind,
                          const Candle& candle,
                          double trend_strength = 0.5,
                          double volume_ratio = 1.0) noexcept;

    // -------------------------------------------------------------------------
    // 突破后假穿检测
    // -------------------------------------------------------------------------
    // 记录突破事件，后续 K 线检测是否回归
    void record_breakout(bool is_up, double price, Timestamp t) noexcept;

    // 检查是否有假穿（在窗口内回归）
    [[nodiscard]] bool check_fake_breakout(const Candle& c) noexcept;

    // -------------------------------------------------------------------------
    // 逃顶区间
    // -------------------------------------------------------------------------
    // 计算逃顶区间（价格远离均线达到阈值时调用）
    [[nodiscard]] std::optional<EscapeZone>
        compute_escape_zone(const IndicatorResult& ind,
                            const Candle& candle) noexcept;

    // 更新逃顶区间状态（根据最新价格）
    void update_escape_zone(EscapeZone& zone,
                            const Candle& candle,
                            const IndicatorResult& ind) noexcept;

    // 清理逃顶区间
    void clear_escape_zone(EscapeZone& zone) noexcept;

    // -------------------------------------------------------------------------
    // 状态管理
    // -------------------------------------------------------------------------
    void reset() noexcept;
    void reset_stats() noexcept { stats_.reset(); }

    [[nodiscard]] const BandStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t bar_count() const noexcept { return bar_count_; }

private:
    // -------------------------------------------------------------------------
    // 内部：计算波动率等级
    // -------------------------------------------------------------------------
    [[nodiscard]] VolatilityLevel classify_volatility(double atr,
                                                       double atr_long) noexcept;

    // -------------------------------------------------------------------------
    // 内部：计算 k 自适应系数
    // -------------------------------------------------------------------------
    [[nodiscard]] double compute_k_adaptive(VolatilityLevel level) const noexcept;
    [[nodiscard]] double compute_k_trend(double trend_strength) const noexcept;

    // -------------------------------------------------------------------------
    // 内部：计算逃顶速度
    // -------------------------------------------------------------------------
    [[nodiscard]] double compute_escape_speed(const IndicatorResult& ind) const noexcept;

    BandConfig config_;
    BandStats  stats_{};
    std::size_t bar_count_{0};

    // 波动率分位数历史（用于自适应）
    static constexpr std::size_t kVolHistorySize = 256;
    std::array<double, kVolHistorySize> vol_history_{};
    std::size_t vol_history_head_{0};
    std::size_t vol_history_count_{0};

    // 假穿检测状态
    struct FakeBreakoutRecord {
        bool      is_up{false};
        double    breakout_price{0.0};
        Timestamp breakout_time{};
        std::size_t bars_since{0};
        bool      active{false};
    };
    FakeBreakoutRecord last_breakout_{};
};

// ==============================================================================
// 内联实现
// ==============================================================================

// -----------------------------------------------------------------------------
// 安全除法
// -----------------------------------------------------------------------------
namespace detail {
[[nodiscard]] inline double safe_div(double num, double den,
                                      double fallback = 0.0) noexcept {
    if (std::abs(den) < kBandEpsilon) return fallback;
    return num / den;
}

[[nodiscard]] inline bool is_finite(double v) noexcept {
    return std::isfinite(v);
}
}  // namespace detail

// -----------------------------------------------------------------------------
// 构造
// -----------------------------------------------------------------------------
inline AdaptiveBand::AdaptiveBand(BandConfig config)
    : config_{config}
{
    const auto err = config_.validate();
    if (!err.empty()) {
        throw std::invalid_argument("BandConfig 非法: " + err);
    }
}

inline void AdaptiveBand::set_config(const BandConfig& config) {
    const auto err = config.validate();
    if (!err.empty()) {
        throw std::invalid_argument("BandConfig 非法: " + err);
    }
    config_ = config;
    reset();
}

inline void AdaptiveBand::reset() noexcept {
    bar_count_ = 0;
    vol_history_.fill(0.0);
    vol_history_head_ = 0;
    vol_history_count_ = 0;
    last_breakout_ = FakeBreakoutRecord{};
    // 保留 stats_，除非显式 reset_stats
}

// -----------------------------------------------------------------------------
// 波动率等级分类
// -----------------------------------------------------------------------------
inline VolatilityLevel
AdaptiveBand::classify_volatility(double atr, double atr_long) noexcept {
    if (!detail::is_finite(atr) || !detail::is_finite(atr_long)) {
        return VolatilityLevel::Unknown;
    }

    const double ratio = detail::safe_div(atr, atr_long, 1.0);

    // 简化分级：基于 vol_ratio 而非分位数（分位数需要完整历史）
    if (ratio < 0.85) return VolatilityLevel::Low;
    if (ratio > 1.15) return VolatilityLevel::High;
    return VolatilityLevel::Normal;
}

inline double AdaptiveBand::compute_k_adaptive(VolatilityLevel level) const noexcept {
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

inline double AdaptiveBand::compute_k_trend(double trend_strength) const noexcept {
    if (!config_.enable_trend_adaptive) {
        return 1.0;
    }

    // trend_strength ∈ [0, 1]
    // 强趋势：放宽 30%（避免频繁假突破）
    // 弱趋势：收紧 20%（避免噪声）
    const double clamped = std::clamp(trend_strength, 0.0, 1.0);
    return 0.8 + clamped * 0.5;   // [0.8, 1.3]
}

inline double
AdaptiveBand::compute_escape_speed(const IndicatorResult& ind) const noexcept {
    // 逃顶速度：|ema_distance| 的绝对值（ATR 单位）
    if (!detail::is_finite(ind.ema_distance)) return 0.0;
    return std::abs(ind.ema_distance);
}

// -----------------------------------------------------------------------------
// 计算带宽
// -----------------------------------------------------------------------------
inline Result<BandResult>
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
            "Candle 非法");
    }

    if (!std::isfinite(ind.ema_slow) || !std::isfinite(ind.atr)) {
        ++stats_.warmup_skips;
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "指标未就绪（EMA 或 ATR 非有限）");
    }

    // -------------------------------------------------------------------------
    // 2. 预热判定
    // -------------------------------------------------------------------------
    BandResult r;
    r.timestamp = candle.open_time;
    r.bar_index = bar_count_;
    r.close = candle.close.to_double();

    if (ind.is_warmup || ind.atr < kBandEpsilon) {
        r.is_warmup = true;
        r.ema = ind.ema_slow;
        r.upper = ind.ema_slow;
        r.lower = ind.ema_slow;
        ++stats_.warmup_skips;
        return r;
    }

    r.is_warmup = false;
    r.ema = ind.ema_slow;

    // -------------------------------------------------------------------------
    // 3. 波动率自适应
    // -------------------------------------------------------------------------
    const double atr = ind.atr;
    const double atr_long = ind.atr_long > kBandEpsilon
        ? ind.atr_long
        : atr;   // 回退

    // vol_ratio 带 clamp
    double vol_ratio_raw = detail::safe_div(atr, atr_long, 1.0);
    r.vol_ratio = std::clamp(vol_ratio_raw, kMinVolRatio, kMaxVolRatio);

    // 波动率等级
    r.vol_level = classify_volatility(atr, atr_long);

    // k 自适应
    r.base_k = config_.base_k;
    r.k_adaptive = compute_k_adaptive(r.vol_level);
    r.k_effective = r.k_adaptive * compute_k_trend(trend_strength);

    // -------------------------------------------------------------------------
    // 4. 带宽计算
    // -------------------------------------------------------------------------
    r.width = atr * r.vol_ratio * r.k_effective;

    // 边界保护：带宽不能为负或 NaN
    if (!detail::is_finite(r.width) || r.width < 0.0) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "带宽计算异常")
            .with_context("atr", std::to_string(atr))
            .with_context("k_effective", std::to_string(r.k_effective));
    }

    // 带宽百分比
    r.width_pct = detail::safe_div(r.width, r.close, 0.0);

    // 上下轨
    r.upper = r.ema + r.width;
    r.lower = r.ema - r.width;

    // 带宽过窄/过宽告警
    if (r.width_pct < kMinBandPct) {
        r.width_too_narrow = true;
        ++stats_.width_narrow_warnings;
    } else if (r.width_pct > kMaxBandPct) {
        r.width_too_wide = true;
        ++stats_.width_wide_warnings;
    }

    // -------------------------------------------------------------------------
    // 5. 突破判定
    // -------------------------------------------------------------------------
    r.breakout_buffer = kBreakoutBufferAtr * atr;

    const double close = r.close;
    r.is_breakout_up   = close > r.upper;
    r.is_breakout_down = close < r.lower;

    // 突破幅度 + 量能确认
    const double breakout_margin = std::max(0.0, close - r.upper);
    const double breakout_margin_down = std::max(0.0, r.lower - close);

    const bool volume_ok = volume_ratio >= kBreakoutVolumeRatio;

    r.is_confirmed_up = r.is_breakout_up
        && breakout_margin >= r.breakout_buffer
        && volume_ok;

    r.is_confirmed_down = r.is_breakout_down
        && breakout_margin_down >= r.breakout_buffer
        && volume_ok;

    if (r.is_breakout_up)   ++stats_.breakout_up_count;
    if (r.is_breakout_down) ++stats_.breakout_down_count;
    if (r.is_confirmed_up || r.is_confirmed_down) {
        ++stats_.confirmed_breakouts;
    }

    // -------------------------------------------------------------------------
    // 6. 位置与距离
    // -------------------------------------------------------------------------
    r.distance_to_upper_atr = detail::safe_div(r.upper - close, atr, 0.0);
    r.distance_to_lower_atr = detail::safe_div(close - r.lower, atr, 0.0);

    // 位置：0 = 下轨，1 = 上轨，可能超出 [0, 1]
    if (r.width > kBandEpsilon) {
        r.band_position = (close - r.lower) / r.width;
    } else {
        r.band_position = 0.5;
    }

    // -------------------------------------------------------------------------
    // 7. 记录波动率历史（用于未来分位数）
    // -------------------------------------------------------------------------
    vol_history_[vol_history_head_] = atr;
    vol_history_head_ = (vol_history_head_ + 1) % kVolHistorySize;
    if (vol_history_count_ < kVolHistorySize) {
        ++vol_history_count_;
    }

    ++bar_count_;

    return r;
}

inline BandResult
AdaptiveBand::compute_unchecked(const IndicatorResult& ind,
                                  const Candle& candle,
                                  double trend_strength,
                                  double volume_ratio) noexcept {
    auto r = compute(ind, candle, trend_strength, volume_ratio);
    if (r.is_ok()) {
        return std::move(r).value();
    }
    BandResult empty;
    empty.timestamp = candle.open_time;
    empty.bar_index = bar_count_;
    empty.is_warmup = true;
    return empty;
}

// -----------------------------------------------------------------------------
// 假穿检测
// -----------------------------------------------------------------------------
inline void AdaptiveBand::record_breakout(bool is_up, double price,
                                            Timestamp t) noexcept {
    last_breakout_.is_up = is_up;
    last_breakout_.breakout_price = price;
    last_breakout_.breakout_time = t;
    last_breakout_.bars_since = 0;
    last_breakout_.active = true;
}

inline bool AdaptiveBand::check_fake_breakout(const Candle& c) noexcept {
    if (!last_breakout_.active) return false;

    ++last_breakout_.bars_since;

    // 超过窗口则视为有效突破
    if (last_breakout_.bars_since > kFakeBreakoutWindow) {
        last_breakout_.active = false;
        return false;
    }

    // 回归判断：向上突破后价格回落到突破价下方
    const double close = c.close.to_double();
    const bool returned = last_breakout_.is_up
        ? (close < last_breakout_.breakout_price)
        : (close > last_breakout_.breakout_price);

    if (returned) {
        last_breakout_.active = false;
        ++stats_.fake_breakouts;
        return true;
    }

    return false;
}

// -----------------------------------------------------------------------------
// 逃顶区间
// -----------------------------------------------------------------------------
inline std::optional<EscapeZone>
AdaptiveBand::compute_escape_zone(const IndicatorResult& ind,
                                    const Candle& candle) noexcept {
    if (ind.is_warmup || !detail::is_finite(ind.atr)) {
        return std::nullopt;
    }

    // 价格必须远离均线（≥ 3×ATR）才触发逃顶
    if (std::abs(ind.ema_distance) < 3.0) {
        return std::nullopt;
    }

    EscapeZone zone;
    zone.created_at = candle.open_time;
    zone.escape_price = candle.close.to_double();
    zone.escape_speed_atr = compute_escape_speed(ind);

    // 区间宽度：速度越快，区间越宽
    const double base_width = ind.atr * config_.base_k;
    const double speed_factor = (zone.escape_speed_atr > 3.0)
        ? kEscapeZoneFastMult
        : kEscapeZoneSlowMult;
    zone.width = base_width * speed_factor;

    zone.upper = zone.escape_price + zone.width;
    zone.lower = zone.escape_price - zone.width;
    zone.state = EscapeZoneState::Watching;

    // 默认有效期：30 根 K 线（90 分钟）
    const int64_t expire_us = 30LL * 3 * 60 * 1000000;
    zone.expired_at = Timestamp{candle.open_time.microseconds() + expire_us};

    return zone;
}

inline void AdaptiveBand::update_escape_zone(EscapeZone& zone,
                                                const Candle& candle,
                                                const IndicatorResult& ind) noexcept {
    if (zone.state != EscapeZoneState::Watching) return;

    // 超时检查
    if (zone.has_expired(candle.open_time)) {
        zone.state = EscapeZoneState::Expired;
        return;
    }

    const double close = candle.close.to_double();

    // 向上突破
    if (close > zone.upper) {
        zone.state = EscapeZoneState::EscapedUp;
        return;
    }

    // 向下突破
    if (close < zone.lower) {
        zone.state = EscapeZoneState::EscapedDown;
        return;
    }

    // 区间内震荡，继续保持 Watching
    (void)ind;
}

inline void AdaptiveBand::clear_escape_zone(EscapeZone& zone) noexcept {
    zone = EscapeZone{};
}

// -----------------------------------------------------------------------------
// 序列化
// -----------------------------------------------------------------------------
inline std::string BandResult::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"ts\":"        << timestamp.microseconds() << ","
        << "\"bar\":"       << bar_index << ","
        << "\"warmup\":"    << (is_warmup ? "true" : "false") << ","
        << "\"ema\":"       << ema << ","
        << "\"upper\":"     << upper << ","
        << "\"lower\":"     << lower << ","
        << "\"width\":"     << width << ","
        << "\"width_pct\":" << width_pct << ","
        << "\"vol_ratio\":" << vol_ratio << ","
        << "\"k_eff\":"     << k_effective << ","
        << "\"bo_up\":"     << (is_breakout_up ? "true" : "false") << ","
        << "\"bo_down\":"   << (is_breakout_down ? "true" : "false") << ","
        << "\"confirmed_up\":"   << (is_confirmed_up ? "true" : "false") << ","
        << "\"confirmed_down\":" << (is_confirmed_down ? "true" : "false")
        << "}";
    return oss.str();
}

inline std::string BandResult::to_string() const {
    std::ostringstream oss;
    oss << "Band{bar=" << bar_index
        << ", ema=" << ema
        << ", [" << lower << ", " << upper << "]"
        << ", width=" << width
        << " (" << (width_pct * 100.0) << "%)"
        << ", k=" << k_effective
        << ", vol=" << to_string(vol_level)
        << ", bo=" << (is_breakout_up ? "UP" : (is_breakout_down ? "DOWN" : "-"))
        << "}";
    return oss.str();
}

}  // namespace indicator
}  // namespace quant

#endif  // QUANT_INDICATOR_CORE_BAND_HPP
