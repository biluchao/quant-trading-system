// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 动态带宽计算（声明版）
// ==============================================================================
// @file    src/indicator/indicator.core.band.hpp
// @module  indicator
// @type    core
// @name    band
// @version 1.0.1
// @brief   基于 ATR 和波动率自适应的动态带宽、突破判定、逃顶区间
//          已修复 12 类运行时问题
//
// 设计原则:
//   - 声明与实现分离：重逻辑在 .cpp，头文件仅声明
//   - 简单 getter 保留 inline：零开销，无需跳转
//   - 量纲统一：带宽以价格为单位（ATR × k），禁止收益率单位
//   - 波动率自适应：k 随 ATR 比值调整（0.7 / 1.0 / 1.3）
//   - 趋势自适应：强趋势放宽带宽，弱趋势收紧
//   - 突破确认：突破幅度 ≥ 0.3×ATR + 成交量放大
//   - 假穿检测：3 根 K 线内回归视为假穿
//   - 逃顶区间：宽度随逃顶速度自适应
//   - 边界保护：clamp + isfinite + epsilon（实现层保证）
//   - 状态可重置：趋势切换时清理
//
// 线程安全:
//   - AdaptiveBand 实例非线程安全，每个线程独立实例
//   - 如需共享，调用方加锁
// ==============================================================================

#ifndef QUANT_INDICATOR_CORE_BAND_HPP
#define QUANT_INDICATOR_CORE_BAND_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
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

// 假穿检测窗口
inline constexpr std::size_t kFakeBreakoutWindow = 3;

// 逃顶区间宽度系数
inline constexpr double kEscapeZoneFastMult = 1.0;   // 快速逃顶
inline constexpr double kEscapeZoneSlowMult = 0.5;   // 缓慢逃顶

// 逃顶区间有效期（K 线根数）
inline constexpr std::size_t kEscapeZoneExpireBars = 30;

// K 线周期（3 分钟，微秒）
inline constexpr int64_t kBarIntervalUs = 3LL * 60 * 1000000;

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

    // ATR 分位数计算窗口（预留）
    std::size_t volatility_percentile_window{100};

    // 带宽百分比越界时是否返回警告
    bool warn_on_band_pct_out_of_range{true};

    // 快速校验（bool，无分配）
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        if (base_k < kMinBaseK || base_k > kMaxBaseK) return false;
        if (volatility_percentile_window < 20) return false;
        return true;
    }

    // 详细校验（返回错误信息，空表示合法）
    // 实现见 .cpp
    [[nodiscard]] std::string validate() const;
};

// ==============================================================================
// 波动率等级
// ==============================================================================
enum class VolatilityLevel : std::uint8_t {
    Unknown  = 0,
    Low      = 1,   // < 15% 波动率
    Normal   = 2,   // 15%~115% 波动率
    High     = 3,   // > 115% 波动率
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
    double vol_ratio{1.0};              // ATR14 / ATR50（已 clamp）
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
    double band_position{0.0};          // close 在带宽内的位置（-2 ~ 3）

    // 告警
    bool   width_too_narrow{false};
    bool   width_too_wide{false};

    // -------------------------------------------------------------------------
    // 辅助查询（inline，零开销）
    // -------------------------------------------------------------------------
    [[nodiscard]] bool is_ready() const noexcept { return !is_warmup; }

    [[nodiscard]] bool is_any_breakout() const noexcept {
        return is_breakout_up || is_breakout_down;
    }

    [[nodiscard]] bool is_confirmed_breakout() const noexcept {
        return is_confirmed_up || is_confirmed_down;
    }

    // 是否接近上轨（< 0.5 ATR）
    [[nodiscard]] bool is_near_upper() const noexcept {
        return std::abs(distance_to_upper_atr) < 0.5;
    }

    // 是否接近下轨
    [[nodiscard]] bool is_near_lower() const noexcept {
        return std::abs(distance_to_lower_atr) < 0.5;
    }

    // 是否在带宽内
    [[nodiscard]] bool is_inside_band() const noexcept {
        return band_position >= 0.0 && band_position <= 1.0;
    }

    // 序列化（实现见 .cpp）
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

    [[nodiscard]] bool is_terminal() const noexcept {
        return state == EscapeZoneState::EscapedUp
            || state == EscapeZoneState::EscapedDown
            || state == EscapeZoneState::Expired;
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

    // 假穿率（假穿 / 突破）
    [[nodiscard]] double fake_rate() const noexcept {
        const auto total = breakout_up_count + breakout_down_count;
        return total == 0
            ? 0.0
            : static_cast<double>(fake_breakouts) /
              static_cast<double>(total);
    }

    // 突破确认率
    [[nodiscard]] double confirm_rate() const noexcept {
        const auto total = breakout_up_count + breakout_down_count;
        return total == 0
            ? 0.0
            : static_cast<double>(confirmed_breakouts) /
              static_cast<double>(total);
    }
};

// ==============================================================================
// 动态带宽计算器
// ==============================================================================
class AdaptiveBand {
public:
    // -------------------------------------------------------------------------
    // 构造 / 配置
    // -------------------------------------------------------------------------
    // 构造时校验 config，非法时抛 std::invalid_argument
    explicit AdaptiveBand(BandConfig config = {});

    AdaptiveBand(const AdaptiveBand&) = delete;
    AdaptiveBand& operator=(const AdaptiveBand&) = delete;
    AdaptiveBand(AdaptiveBand&&) noexcept = default;
    AdaptiveBand& operator=(AdaptiveBand&&) noexcept = default;

    ~AdaptiveBand() = default;

    [[nodiscard]] const BandConfig& config() const noexcept { return config_; }

    // 更新配置（会重置状态）
    void set_config(const BandConfig& config);

    // -------------------------------------------------------------------------
    // 核心：计算带宽
    // -------------------------------------------------------------------------
    // 输入：
    //   ind            - 指标结果（含 EMA、ATR、ATR_Long、成交量、均线斜率）
    //   candle         - 当前 K 线
    //   trend_strength - 趋势强度 [0, 1]（0=无趋势，1=强趋势）
    //   volume_ratio   - 成交量比（可选，用于突破确认）
    // 返回：Result<BandResult>
    [[nodiscard]] Result<BandResult>
        compute(const IndicatorResult& ind,
                const Candle& candle,
                double trend_strength = 0.5,
                double volume_ratio = 1.0) noexcept;

    // 便捷接口：失败时返回预热空结果
    [[nodiscard]] BandResult
        compute_unchecked(const IndicatorResult& ind,
                          const Candle& candle,
                          double trend_strength = 0.5,
                          double volume_ratio = 1.0) noexcept;

    // -------------------------------------------------------------------------
    // 假穿检测
    // -------------------------------------------------------------------------
    // 记录一次突破事件，后续 K 线检测是否回归
    void record_breakout(bool is_up, double price, Timestamp t) noexcept;

    // 检查是否假穿（在窗口内回归）
    // 每次调用自动推进 bars_since 计数
    [[nodiscard]] bool check_fake_breakout(const Candle& c) noexcept;

    // 是否处于待确认的突破中
    [[nodiscard]] bool has_pending_breakout() const noexcept {
        return last_breakout_.active;
    }

    // -------------------------------------------------------------------------
    // 逃顶区间
    // -------------------------------------------------------------------------
    // 价格远离均线 ≥ 3 ATR 时计算逃顶区间
    [[nodiscard]] std::optional<EscapeZone>
        compute_escape_zone(const IndicatorResult& ind,
                            const Candle& candle) noexcept;

    // 更新逃顶区间状态（根据最新价格）
    void update_escape_zone(EscapeZone& zone,
                            const Candle& candle,
                            const IndicatorResult& ind) noexcept;

    // 清理
    void clear_escape_zone(EscapeZone& zone) noexcept;

    // -------------------------------------------------------------------------
    // 状态管理
    // -------------------------------------------------------------------------
    // 重置所有状态（不清空 stats）
    void reset() noexcept;

    // 只重置 stats
    void reset_stats() noexcept { stats_.reset(); }

    // 重置所有（含 stats）
    void reset_all() noexcept {
        reset();
        stats_.reset();
    }

    // -------------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------------
    [[nodiscard]] const BandStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t bar_count() const noexcept { return bar_count_; }

    // 波动率历史（用于诊断）
    [[nodiscard]] std::size_t volatility_history_count() const noexcept {
        return vol_history_count_;
    }

private:
    // -------------------------------------------------------------------------
    // 内部方法（实现见 .cpp）
    // -------------------------------------------------------------------------
    [[nodiscard]] VolatilityLevel
        classify_volatility(double atr, double atr_long) noexcept;

    [[nodiscard]] double
        compute_k_adaptive(VolatilityLevel level) const noexcept;

    [[nodiscard]] double
        compute_k_trend(double trend_strength) const noexcept;

    [[nodiscard]] double
        compute_escape_speed(const IndicatorResult& ind) const noexcept;

    // -------------------------------------------------------------------------
    // 成员
    // -------------------------------------------------------------------------
    BandConfig config_;
    BandStats  stats_{};
    std::size_t bar_count_{0};

    // 波动率历史（环形缓冲，用于未来分位数）
    static constexpr std::size_t kVolHistorySize = 256;
    std::array<double, kVolHistorySize> vol_history_{};   // 零初始化
    std::size_t vol_history_head_{0};
    std::size_t vol_history_count_{0};

    // 假穿检测状态
    struct FakeBreakoutRecord {
        bool        is_up{false};
        double      breakout_price{0.0};
        Timestamp   breakout_time{};
        std::size_t bars_since{0};
        bool        active{false};
    };
    FakeBreakoutRecord last_breakout_{};
};

}  // namespace indicator
}  // namespace quant

#endif  // QUANT_INDICATOR_CORE_BAND_HPP
