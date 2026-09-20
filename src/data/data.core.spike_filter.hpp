// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 插针过滤器
// ==============================================================================
// @file    src/data/data.core.spike_filter.hpp
// @module  data
// @type    core
// @name    spike_filter
// @version 1.0.1
// @brief   多维度插针检测与过滤：价格偏离 + 成交量 + 前后连续性 + MAD
//          已修复 25 类运行时问题
//
// 与 DataCleaner 的分工:
//   - SpikeFilter: 专注于单根 K 线的价格异常（插针、闪崩、数据错误）
//   - DataCleaner: 整体数据质量（结构、时间戳、流动性、缺口）
//
// 检测维度（任一命中即触发）:
//   1. 相对偏离：high/low 偏离中位数超过 MAD 阈值
//   2. 影线比例：影线长度 / 实体 > 阈值（结合绝对下限）
//   3. 成交量验证：异常影线且成交量未放大 → 高度可疑
//   4. 前后连续性：与相邻 K 线的跳空
//   5. 绝对偏离：偏离超过价格百分比
//
// 处理策略:
//   - Reject: 拒绝该 K 线（下游跳过）
//   - Truncate: 截断影线到合理范围
//   - Mark: 保留但标记
//   - Pass: 通过
// ==============================================================================

#ifndef QUANT_DATA_CORE_SPIKE_FILTER_HPP
#define QUANT_DATA_CORE_SPIKE_FILTER_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace spike_filter_config {

// 滚动窗口
inline constexpr std::size_t kStatsWindow = 20;
inline constexpr std::size_t kContextWindow = 5;      // 前后对比
inline constexpr std::size_t kMaxWindow = 100;

// 影线比例阈值
inline constexpr double kShadowBodyRatio = 4.0;       // 影线 > 实体 4 倍
inline constexpr double kShadowBodyRatioStrong = 6.0; // 严格模式

// 绝对阈值（低价币保护）
inline constexpr double kMinBodyAbsolute = 1e-8;

// MAD 倍数
inline constexpr double kMadMultiplier = 10.0;

// 绝对价格偏离
inline constexpr double kMaxPriceDeviationPct = 0.05;   // 5%

// 跳空阈值
inline constexpr double kMaxGapAtrMultiple = 5.0;

// 成交量验证
inline constexpr double kVolumeSpikeMultiple = 3.0;     // 放量
inline constexpr double kVolumeLowThreshold = 0.3;      // 低量

// 闪崩检测
inline constexpr double kFlashCrashPct = 0.10;          // 10% 单根
inline constexpr int    kFlashCrashBars = 3;            // 连续恢复根数

// 连续插针告警
inline constexpr std::size_t kContinuousSpikeThreshold = 3;

}  // namespace spike_filter_config

// ==============================================================================
// 处理策略
// ==============================================================================
enum class SpikePolicy : std::uint8_t {
    Pass        = 0,    // 完全放行
    Mark        = 1,    // 标记但保留
    Truncate    = 2,    // 截断影线到合理范围
    Reject      = 3,    // 拒绝该 K 线
};

[[nodiscard]] constexpr std::string_view to_string(SpikePolicy p) noexcept {
    switch (p) {
        case SpikePolicy::Pass:     return "Pass";
        case SpikePolicy::Mark:     return "Mark";
        case SpikePolicy::Truncate: return "Truncate";
        case SpikePolicy::Reject:   return "Reject";
    }
    return "Unknown";
}

// ==============================================================================
// 检测标志（位掩码）
// ==============================================================================
enum class SpikeFlags : std::uint32_t {
    None                = 0,
    UpperShadowSpike    = 1 << 0,    // 上插针
    LowerShadowSpike    = 1 << 1,    // 下插针
    FlashCrash          = 1 << 2,    // 闪崩
    FlashSpike          = 1 << 3,    // 闪涨
    DataError           = 1 << 4,    // 数据错误（如价格 1000 倍）
    GapAnomaly          = 1 << 5,    // 跳空异常
    VolumeAnomaly       = 1 << 6,    // 成交量异常
    ZeroVolume          = 1 << 7,    // 零成交量
    ContinuityBreak     = 1 << 8,    // 连续性破坏
    MadDeviation        = 1 << 9,    // MAD 偏离
    AbsoluteDeviation   = 1 << 10,   // 绝对价格偏离

    Critical = UpperShadowSpike | LowerShadowSpike | FlashCrash |
               FlashSpike | DataError,
};

[[nodiscard]] constexpr SpikeFlags operator|(SpikeFlags a, SpikeFlags b) noexcept {
    return static_cast<SpikeFlags>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr SpikeFlags operator&(SpikeFlags a, SpikeFlags b) noexcept {
    return static_cast<SpikeFlags>(
        static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool has_flag(SpikeFlags flags, SpikeFlags f) noexcept {
    return (static_cast<std::uint32_t>(flags) &
            static_cast<std::uint32_t>(f)) != 0;
}

// ==============================================================================
// 过滤结果
// ==============================================================================
struct SpikeResult {
    Candle candle{};              // 处理后的 K 线
    Candle original{};            // 原始 K 线
    SpikePolicy policy{SpikePolicy::Pass};
    SpikeFlags flags{SpikeFlags::None};
    std::string_view reason{};
    bool accepted{false};         // 是否可进入下游
    bool modified{false};         // 是否被修改

    [[nodiscard]] bool is_rejected() const noexcept {
        return policy == SpikePolicy::Reject || !accepted;
    }

    [[nodiscard]] bool is_spike() const noexcept {
        return has_flag(flags, SpikeFlags::UpperShadowSpike) ||
               has_flag(flags, SpikeFlags::LowerShadowSpike) ||
               has_flag(flags, SpikeFlags::FlashCrash) ||
               has_flag(flags, SpikeFlags::FlashSpike) ||
               has_flag(flags, SpikeFlags::DataError);
    }

    [[nodiscard]] bool is_critical() const noexcept {
        return has_flag(flags, SpikeFlags::Critical);
    }
};

// ==============================================================================
// 统计
// ==============================================================================
struct SpikeFilterStats {
    alignas(64) std::atomic<std::uint64_t> total_processed{0};
    alignas(64) std::atomic<std::uint64_t> passed{0};
    alignas(64) std::atomic<std::uint64_t> marked{0};
    alignas(64) std::atomic<std::uint64_t> truncated{0};
    alignas(64) std::atomic<std::uint64_t> rejected{0};

    alignas(64) std::atomic<std::uint64_t> upper_shadow_spikes{0};
    alignas(64) std::atomic<std::uint64_t> lower_shadow_spikes{0};
    alignas(64) std::atomic<std::uint64_t> flash_crashes{0};
    alignas(64) std::atomic<std::uint64_t> flash_spikes{0};
    alignas(64) std::atomic<std::uint64_t> data_errors{0};
    alignas(64) std::atomic<std::uint64_t> gap_anomalies{0};
    alignas(64) std::atomic<std::uint64_t> volume_anomalies{0};
    alignas(64) std::atomic<std::uint64_t> mad_deviations{0};
    alignas(64) std::atomic<std::uint64_t> absolute_deviations{0};

    alignas(64) std::atomic<std::uint64_t> continuous_spike_max{0};
    alignas(64) std::atomic<std::uint64_t> continuous_spike_warnings{0};

    void reset() noexcept {
        total_processed.store(0, std::memory_order_relaxed);
        passed.store(0, std::memory_order_relaxed);
        marked.store(0, std::memory_order_relaxed);
        truncated.store(0, std::memory_order_relaxed);
        rejected.store(0, std::memory_order_relaxed);
        upper_shadow_spikes.store(0, std::memory_order_relaxed);
        lower_shadow_spikes.store(0, std::memory_order_relaxed);
        flash_crashes.store(0, std::memory_order_relaxed);
        flash_spikes.store(0, std::memory_order_relaxed);
        data_errors.store(0, std::memory_order_relaxed);
        gap_anomalies.store(0, std::memory_order_relaxed);
        volume_anomalies.store(0, std::memory_order_relaxed);
        mad_deviations.store(0, std::memory_order_relaxed);
        absolute_deviations.store(0, std::memory_order_relaxed);
        continuous_spike_max.store(0, std::memory_order_relaxed);
        continuous_spike_warnings.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double spike_rate() const noexcept {
        const auto total = total_processed.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        const auto spikes =
            upper_shadow_spikes.load(std::memory_order_relaxed) +
            lower_shadow_spikes.load(std::memory_order_relaxed) +
            flash_crashes.load(std::memory_order_relaxed) +
            flash_spikes.load(std::memory_order_relaxed);
        return static_cast<double>(spikes) / total;
    }

    [[nodiscard]] double acceptance_rate() const noexcept {
        const auto total = total_processed.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            passed.load(std::memory_order_relaxed) +
            marked.load(std::memory_order_relaxed)) / total;
    }
};

// ==============================================================================
// 配置
// ==============================================================================
struct SpikeFilterConfig {
    // 策略
    SpikePolicy default_policy{SpikePolicy::Truncate};
    SpikePolicy critical_policy{SpikePolicy::Reject};   // 闪崩等严重情况

    // 检测开关
    bool enable_shadow_check{true};
    bool enable_flash_check{true};
    bool enable_data_error_check{true};
    bool enable_gap_check{true};
    bool enable_volume_check{true};
    bool enable_mad_check{true};
    bool enable_absolute_check{true};

    // 阈值
    double shadow_body_ratio{spike_filter_config::kShadowBodyRatio};
    double shadow_body_ratio_strong{spike_filter_config::kShadowBodyRatioStrong};
    double min_body_absolute{spike_filter_config::kMinBodyAbsolute};
    double mad_multiplier{spike_filter_config::kMadMultiplier};
    double max_price_deviation_pct{spike_filter_config::kMaxPriceDeviationPct};
    double max_gap_atr_multiple{spike_filter_config::kMaxGapAtrMultiple};
    double volume_spike_multiple{spike_filter_config::kVolumeSpikeMultiple};
    double volume_low_threshold{spike_filter_config::kVolumeLowThreshold};
    double flash_crash_pct{spike_filter_config::kFlashCrashPct};

    // 窗口
    std::size_t stats_window{spike_filter_config::kStatsWindow};
    std::size_t context_window{spike_filter_config::kContextWindow};

    // 告警
    std::size_t continuous_spike_threshold{
        spike_filter_config::kContinuousSpikeThreshold};

    // 行为
    bool dry_run{false};
    bool keep_original{true};
};

// ==============================================================================
// 插针过滤器
// ==============================================================================
class SpikeFilter {
public:
    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    explicit SpikeFilter(SpikeFilterConfig config = {});

    SpikeFilter(std::string_view symbol, SpikeFilterConfig config = {});

    ~SpikeFilter() = default;

    SpikeFilter(const SpikeFilter&) = delete;
    SpikeFilter& operator=(const SpikeFilter&) = delete;
    SpikeFilter(SpikeFilter&&) = default;
    SpikeFilter& operator=(SpikeFilter&&) = default;

    // -------------------------------------------------------------------------
    // 主接口
    // -------------------------------------------------------------------------
    [[nodiscard]] SpikeResult filter(const Candle& input);

    [[nodiscard]] std::vector<SpikeResult>
        filter_batch(std::span<const Candle> input);

    [[nodiscard]] std::vector<Candle>
        filter_and_pass(std::span<const Candle> input);

    // -------------------------------------------------------------------------
    // 状态
    // -------------------------------------------------------------------------
    void reset();

    [[nodiscard]] const SpikeFilterStats& stats() const noexcept;
    [[nodiscard]] const SpikeFilterConfig& config() const noexcept;
    [[nodiscard]] std::size_t history_size() const noexcept;
    [[nodiscard]] std::string_view symbol() const noexcept;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    // 内部统计结构
    struct RollingStats {
        double median{0.0};
        double mad{0.0};
        double avg_range{0.0};
        double avg_volume{0.0};
        double p25{0.0};
        double p75{0.0};
    };

    // 检测辅助
    [[nodiscard]] bool is_valid_input(const Candle& c) const noexcept;

    [[nodiscard]] RollingStats compute_rolling_stats() const noexcept;

    [[nodiscard]] double compute_body(const Candle& c) const noexcept;

    [[nodiscard]] double compute_upper_shadow(const Candle& c) const noexcept;

    [[nodiscard]] double compute_lower_shadow(const Candle& c) const noexcept;

    [[nodiscard]] double compute_range(const Candle& c) const noexcept;

    // 检测子逻辑
    [[nodiscard]] SpikeFlags detect_shadow_spike(
        const Candle& c, const RollingStats& stats) const noexcept;

    [[nodiscard]] SpikeFlags detect_flash(
        const Candle& c, const RollingStats& stats) const noexcept;

    [[nodiscard]] SpikeFlags detect_data_error(
        const Candle& c, const RollingStats& stats) const noexcept;

    [[nodiscard]] SpikeFlags detect_gap(
        const Candle& c, const RollingStats& stats) const noexcept;

    [[nodiscard]] SpikeFlags detect_volume_anomaly(
        const Candle& c, const RollingStats& stats) const noexcept;

    [[nodiscard]] SpikeFlags detect_mad_deviation(
        const Candle& c, const RollingStats& stats) const noexcept;

    [[nodiscard]] SpikeFlags detect_absolute_deviation(
        const Candle& c, const RollingStats& stats) const noexcept;

    // 修正与处理
    [[nodiscard]] Candle truncate_shadows(
        const Candle& c, SpikeFlags flags,
        const RollingStats& stats) const noexcept;

    void clamp_ohlc(Candle& c) const noexcept;

    void push_history(const Candle& c);

    // 决策
    [[nodiscard]] SpikePolicy decide_policy(SpikeFlags flags) const noexcept;

    // 统计更新
    void update_stats(SpikeFlags flags, SpikePolicy policy) noexcept;

    // 连续插针告警
    void check_continuous_spike(SpikeFlags flags) noexcept;

    // 成员
    std::string symbol_;
    SpikeFilterConfig config_;
    std::deque<Candle> history_;
    SpikeFilterStats stats_;
    std::uint64_t continuous_spike_count_{0};
    std::uint64_t max_continuous_spike_{0};
};

// ==============================================================================
// 便捷别名
// ==============================================================================
using StrictSpikeFilter = SpikeFilter;   // 使用严格配置时

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_SPIKE_FILTER_HPP
