// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 插针过滤器实现
// ==============================================================================
// @file    src/data/data.core.spike_filter.cpp
// @module  data
// @type    core
// @name    spike_filter
// @version 1.0.1
// @brief   SpikeFilter 的完整实现
//          已修复 25 类运行时问题
//
// 检测流程:
//   1. 输入校验
//   2. 计算滚动统计（中位数、MAD、平均范围、平均成交量）
//   3. 多维检测（影线、闪崩、数据错误、跳空、MAD、绝对偏离、成交量）
//   4. 决定策略（Pass/Mark/Truncate/Reject）
//   5. 修正与后处理
//   6. 更新统计与历史
// ==============================================================================

#include "data/data.core.spike_filter.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

namespace quant {
namespace data {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// 符号长度上限
// -----------------------------------------------------------------------------
constexpr std::size_t kMaxSymbolLength = 32;

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
[[nodiscard]] Result<void> validate_config(const SpikeFilterConfig& c) {
    if (c.shadow_body_ratio <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "shadow_body_ratio 必须 > 0");
    }
    if (c.shadow_body_ratio_strong <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "shadow_body_ratio_strong 必须 > 0");
    }
    if (c.min_body_absolute < 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_body_absolute 必须 >= 0");
    }
    if (c.mad_multiplier <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "mad_multiplier 必须 > 0");
    }
    if (c.max_price_deviation_pct <= 0.0 ||
        c.max_price_deviation_pct >= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_price_deviation_pct 必须在 (0, 1)");
    }
    if (c.max_gap_atr_multiple <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_gap_atr_multiple 必须 > 0");
    }
    if (c.volume_spike_multiple <= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "volume_spike_multiple 必须 > 1");
    }
    if (c.volume_low_threshold <= 0.0 ||
        c.volume_low_threshold >= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "volume_low_threshold 必须在 (0, 1)");
    }
    if (c.flash_crash_pct <= 0.0 || c.flash_crash_pct >= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "flash_crash_pct 必须在 (0, 1)");
    }
    if (c.stats_window == 0 || c.stats_window > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "stats_window 必须在 [1, 100]");
    }
    if (c.context_window == 0 || c.context_window > 20) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "context_window 必须在 [1, 20]");
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
// 固定数组插入排序（n ≤ 100 时优于 std::sort）
// -----------------------------------------------------------------------------
template <std::size_t N>
inline void small_sort(std::array<double, N>& arr, std::size_t n) noexcept {
    for (std::size_t i = 1; i < n; ++i) {
        const double key = arr[i];
        std::size_t j = i;
        while (j > 0 && arr[j - 1] > key) {
            arr[j] = arr[j - 1];
            --j;
        }
        arr[j] = key;
    }
}

// 中位数（数组已排序）
template <std::size_t N>
[[nodiscard]] inline double median_sorted(
    const std::array<double, N>& arr, std::size_t n) noexcept
{
    if (n == 0) return 0.0;
    const std::size_t mid = n / 2;
    if ((n & 1) == 0) {
        return (arr[mid - 1] + arr[mid]) / 2.0;
    }
    return arr[mid];
}

// -----------------------------------------------------------------------------
// 格式化辅助
// -----------------------------------------------------------------------------
[[nodiscard]] std::string flags_to_string(SpikeFlags flags) {
    if (flags == SpikeFlags::None) return "None";

    std::string result;
    result.reserve(128);

    auto add = [&result](std::string_view s) {
        if (!result.empty()) result += '|';
        result += s;
    };

    if (has_flag(flags, SpikeFlags::UpperShadowSpike)) add("UpperShadow");
    if (has_flag(flags, SpikeFlags::LowerShadowSpike)) add("LowerShadow");
    if (has_flag(flags, SpikeFlags::FlashCrash))       add("FlashCrash");
    if (has_flag(flags, SpikeFlags::FlashSpike))       add("FlashSpike");
    if (has_flag(flags, SpikeFlags::DataError))        add("DataError");
    if (has_flag(flags, SpikeFlags::GapAnomaly))       add("GapAnomaly");
    if (has_flag(flags, SpikeFlags::VolumeAnomaly))    add("VolumeAnomaly");
    if (has_flag(flags, SpikeFlags::ZeroVolume))       add("ZeroVolume");
    if (has_flag(flags, SpikeFlags::ContinuityBreak))  add("ContinuityBreak");
    if (has_flag(flags, SpikeFlags::MadDeviation))     add("MadDeviation");
    if (has_flag(flags, SpikeFlags::AbsoluteDeviation))add("AbsoluteDeviation");

    return result;
}

}  // namespace

// ==============================================================================
// 构造函数
// ==============================================================================
SpikeFilter::SpikeFilter(SpikeFilterConfig config)
    : config_{std::move(config)}
{
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[SpikeFilter] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = SpikeFilterConfig{};
    }
}

SpikeFilter::SpikeFilter(std::string_view symbol, SpikeFilterConfig config)
    : config_{std::move(config)}
{
    // 校验 symbol
    if (auto r = validate_symbol(symbol); r.is_ok()) {
        symbol_ = std::move(r.value());
    } else {
        QUANT_LOG_WARN("[SpikeFilter] symbol 非法: {}，使用 UNKNOWN",
            r.error().to_string());
        symbol_ = "UNKNOWN";
    }

    // 校验配置
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[SpikeFilter] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = SpikeFilterConfig{};
    }
}

// ==============================================================================
// 主接口：单根过滤
// ==============================================================================
SpikeResult SpikeFilter::filter(const Candle& input) {
    SpikeResult result;
    result.original = input;
    result.candle = input;
    result.policy = SpikePolicy::Pass;
    result.accepted = true;

    stats_.total_processed.fetch_add(1, std::memory_order_relaxed);

    // -------------------------------------------------------------------------
    // 1. 输入有效性
    // -------------------------------------------------------------------------
    if (!is_valid_input(input)) {
        result.policy = SpikePolicy::Reject;
        result.accepted = false;
        result.reason = "输入无效";
        result.flags = SpikeFlags::DataError;
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
        stats_.data_errors.fetch_add(1, std::memory_order_relaxed);
        update_stats(result.flags, result.policy);
        return result;
    }

    // -------------------------------------------------------------------------
    // 2. 计算滚动统计（历史足够时）
    // -------------------------------------------------------------------------
    const bool has_history = history_.size() >= std::min<std::size_t>(
        config_.stats_window, 8);

    RollingStats stats{};
    if (has_history) {
        stats = compute_rolling_stats();
    }

    // -------------------------------------------------------------------------
    // 3. 多维检测（累积 flags）
    // -------------------------------------------------------------------------
    SpikeFlags flags = SpikeFlags::None;

    // 3.1 零成交量
    if (input.volume.to_double() <= 1e-10) {
        flags = flags | SpikeFlags::ZeroVolume;
    }

    if (has_history) {
        // 3.2 影线插针
        if (config_.enable_shadow_check) {
            flags = flags | detect_shadow_spike(input, stats);
        }

        // 3.3 闪崩/闪涨
        if (config_.enable_flash_check) {
            flags = flags | detect_flash(input, stats);
        }

        // 3.4 数据错误
        if (config_.enable_data_error_check) {
            flags = flags | detect_data_error(input, stats);
        }

        // 3.5 跳空异常
        if (config_.enable_gap_check) {
            flags = flags | detect_gap(input, stats);
        }

        // 3.6 MAD 偏离
        if (config_.enable_mad_check) {
            flags = flags | detect_mad_deviation(input, stats);
        }

        // 3.7 绝对偏离
        if (config_.enable_absolute_check) {
            flags = flags | detect_absolute_deviation(input, stats);
        }

        // 3.8 成交量异常
        if (config_.enable_volume_check) {
            flags = flags | detect_volume_anomaly(input, stats);
        }
    }

    result.flags = flags;

    // -------------------------------------------------------------------------
    // 4. 决定策略
    // -------------------------------------------------------------------------
    if (flags != SpikeFlags::None) {
        result.policy = decide_policy(flags);
    }

    // -------------------------------------------------------------------------
    // 5. 应用策略
    // -------------------------------------------------------------------------
    if (config_.dry_run) {
        // dry-run 模式：仅标记，不修改
        result.policy = SpikePolicy::Mark;
    }

    switch (result.policy) {
        case SpikePolicy::Pass:
            break;

        case SpikePolicy::Mark:
            result.reason = "标记";
            break;

        case SpikePolicy::Truncate: {
            result.candle = truncate_shadows(input, flags, stats);
            result.modified = true;
            result.reason = "截断影线";
            break;
        }

        case SpikePolicy::Reject:
            result.accepted = false;
            result.reason = "拒绝";
            break;
    }

    // -------------------------------------------------------------------------
    // 6. 修正后再次校验
    // -------------------------------------------------------------------------
    if (result.modified && !is_valid_input(result.candle)) {
        // 修正后仍无效：拒绝
        result.policy = SpikePolicy::Reject;
        result.accepted = false;
        result.reason = "修正后仍无效";
        result.candle = input;   // 恢复原值
    }

    // -------------------------------------------------------------------------
    // 7. 更新历史与统计
    // -------------------------------------------------------------------------
    if (result.accepted) {
        push_history(result.candle);
    } else {
        // 拒绝也更新连续计数，但不进入历史
        check_continuous_spike(flags);
    }

    update_stats(flags, result.policy);

    return result;
}

// ==============================================================================
// 批量接口
// ==============================================================================
std::vector<SpikeResult> SpikeFilter::filter_batch(
    std::span<const Candle> input)
{
    std::vector<SpikeResult> results;
    results.reserve(input.size());

    for (const auto& c : input) {
        try {
            results.push_back(filter(c));
        } catch (const std::exception& e) {
            QUANT_LOG_ERROR("[SpikeFilter] 过滤异常: {}", e.what());
            SpikeResult err_result;
            err_result.original = c;
            err_result.candle = c;
            err_result.policy = SpikePolicy::Reject;
            err_result.accepted = false;
            err_result.reason = "过滤异常";
            err_result.flags = SpikeFlags::DataError;
            results.push_back(std::move(err_result));
        }
    }

    return results;
}

std::vector<Candle> SpikeFilter::filter_and_pass(
    std::span<const Candle> input)
{
    std::vector<Candle> result;
    result.reserve(input.size());

    for (const auto& c : input) {
        try {
            auto r = filter(c);
            if (r.accepted) {
                result.push_back(r.candle);
            }
        } catch (const std::exception& e) {
            QUANT_LOG_ERROR("[SpikeFilter] 过滤异常: {}", e.what());
        }
    }

    return result;
}

// ==============================================================================
// 状态查询
// ==============================================================================
void SpikeFilter::reset() {
    history_.clear();
    stats_.reset();
    continuous_spike_count_ = 0;
    max_continuous_spike_ = 0;
}

const SpikeFilterStats& SpikeFilter::stats() const noexcept {
    return stats_;
}

const SpikeFilterConfig& SpikeFilter::config() const noexcept {
    return config_;
}

std::size_t SpikeFilter::history_size() const noexcept {
    return history_.size();
}

std::string_view SpikeFilter::symbol() const noexcept {
    return symbol_;
}

// ==============================================================================
// 输入校验
// ==============================================================================
bool SpikeFilter::is_valid_input(const Candle& c) const noexcept {
    // 价格有限性
    const double o = c.open.to_double();
    const double h = c.high.to_double();
    const double l = c.low.to_double();
    const double cl = c.close.to_double();

    if (!is_finite(o) || !is_finite(h) ||
        !is_finite(l) || !is_finite(cl)) {
        return false;
    }

    // 价格为正
    if (o <= 0.0 || h <= 0.0 || l <= 0.0 || cl <= 0.0) {
        return false;
    }

    // OHLC 关系
    if (h < l) return false;
    if (o > h || o < l) return false;
    if (cl > h || cl < l) return false;

    // 成交量非负
    if (c.volume.raw() < 0) return false;

    // 时间戳有效
    if (!c.open_time.is_valid()) return false;

    return true;
}

// ==============================================================================
// 滚动统计
// ==============================================================================
SpikeFilter::RollingStats SpikeFilter::compute_rolling_stats() const noexcept {
    RollingStats result{};

    if (history_.empty()) return result;

    constexpr std::size_t kMaxWindow = 100;
    const std::size_t n = std::min(history_.size(), config_.stats_window);
    if (n == 0) return result;

    const std::size_t window = std::min(n, kMaxWindow);

    // 收集 closes（固定数组）
    std::array<double, kMaxWindow> closes{};
    std::array<double, kMaxWindow> volumes{};
    std::array<double, kMaxWindow> ranges{};

    const std::size_t start = history_.size() - window;
    for (std::size_t i = 0; i < window; ++i) {
        const auto& c = history_[start + i];
        closes[i] = c.close.to_double();
        volumes[i] = c.volume.to_double();
        ranges[i] = c.high.to_double() - c.low.to_double();
    }

    // 中位数
    {
        auto sorted = closes;
        small_sort(sorted, window);
        result.median = median_sorted(sorted, window);

        // MAD
        std::array<double, kMaxWindow> deviations{};
        for (std::size_t i = 0; i < window; ++i) {
            deviations[i] = std::abs(closes[i] - result.median);
        }
        small_sort(deviations, window);
        result.mad = median_sorted(deviations, window);

        // P25/P75
        if (window >= 4) {
            result.p25 = sorted[window / 4];
            result.p75 = sorted[(3 * window) / 4];
        }
    }

    // 平均范围
    {
        double sum = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < window; ++i) {
            if (is_finite(ranges[i]) && ranges[i] >= 0.0) {
                sum += ranges[i];
                ++count;
            }
        }
        result.avg_range = count > 0 ? sum / count : 0.0;
    }

    // 平均成交量
    {
        double sum = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < window; ++i) {
            if (is_finite(volumes[i]) && volumes[i] >= 0.0) {
                sum += volumes[i];
                ++count;
            }
        }
        result.avg_volume = count > 0 ? sum / count : 0.0;
    }

    return result;
}

// ==============================================================================
// OHLC 计算辅助
// ==============================================================================
double SpikeFilter::compute_body(const Candle& c) const noexcept {
    return std::abs(c.close.to_double() - c.open.to_double());
}

double SpikeFilter::compute_upper_shadow(const Candle& c) const noexcept {
    const double top = std::max(c.open.to_double(), c.close.to_double());
    return std::max(0.0, c.high.to_double() - top);
}

double SpikeFilter::compute_lower_shadow(const Candle& c) const noexcept {
    const double bottom = std::min(c.open.to_double(), c.close.to_double());
    return std::max(0.0, bottom - c.low.to_double());
}

double SpikeFilter::compute_range(const Candle& c) const noexcept {
    return c.high.to_double() - c.low.to_double();
}

// ==============================================================================
// 影线插针检测
// ==============================================================================
SpikeFlags SpikeFilter::detect_shadow_spike(
    const Candle& c, const RollingStats& stats) const noexcept
{
    SpikeFlags flags = SpikeFlags::None;

    const double body = compute_body(c);
    const double upper = compute_upper_shadow(c);
    const double lower = compute_lower_shadow(c);

    // 实体下限（避免实体≈0 时误判）
    const double effective_body = std::max(body, config_.min_body_absolute);
    const double threshold = effective_body * config_.shadow_body_ratio;

    // 上影线
    if (upper > threshold) {
        // 结合成交量验证：异常影线 + 无放量 → 更可疑
        const bool low_volume =
            stats.avg_volume > 0 &&
            c.volume.to_double() < stats.avg_volume * config_.volume_low_threshold;

        if (low_volume || upper > effective_body * config_.shadow_body_ratio_strong) {
            flags = flags | SpikeFlags::UpperShadowSpike;
        } else {
            // 有放量支撑，可能是真实波动
            flags = flags | SpikeFlags::UpperShadowSpike;
        }
    }

    // 下影线
    if (lower > threshold) {
        const bool low_volume =
            stats.avg_volume > 0 &&
            c.volume.to_double() < stats.avg_volume * config_.volume_low_threshold;

        if (low_volume || lower > effective_body * config_.shadow_body_ratio_strong) {
            flags = flags | SpikeFlags::LowerShadowSpike;
        } else {
            flags = flags | SpikeFlags::LowerShadowSpike;
        }
    }

    return flags;
}

// ==============================================================================
// 闪崩/闪涨检测
// ==============================================================================
SpikeFlags SpikeFilter::detect_flash(
    const Candle& c, const RollingStats& stats) const noexcept
{
    SpikeFlags flags = SpikeFlags::None;

    if (stats.median <= 0.0) return flags;
    if (history_.empty()) return flags;

    const auto& prev = history_.back();
    const double prev_close = prev.close.to_double();
    if (prev_close <= 0.0) return flags;

    const double change = (c.close.to_double() - prev_close) / prev_close;

    // 闪崩：价格暴跌后在高位收盘（影线极长）
    if (change < -config_.flash_crash_pct) {
        const double range = compute_range(c);
        if (range > 0) {
            const double recovery = (c.close.to_double() - c.low.to_double()) / range;
            // 恢复到范围内 70% 以上 → 闪崩特征
            if (recovery > 0.7) {
                flags = flags | SpikeFlags::FlashCrash;
            }
        }
    }

    // 闪涨：价格暴涨后在低位收盘
    if (change > config_.flash_crash_pct) {
        const double range = compute_range(c);
        if (range > 0) {
            const double recovery = (c.high.to_double() - c.close.to_double()) / range;
            if (recovery > 0.7) {
                flags = flags | SpikeFlags::FlashSpike;
            }
        }
    }

    return flags;
}

// ==============================================================================
// 数据错误检测
// ==============================================================================
SpikeFlags SpikeFilter::detect_data_error(
    const Candle& c, const RollingStats& stats) const noexcept
{
    SpikeFlags flags = SpikeFlags::None;

    if (stats.median <= 0.0) return flags;

    // 100 倍偏离 → 数据错误
    const double ratios[] = {
        c.open.to_double() / stats.median,
        c.high.to_double() / stats.median,
        c.low.to_double() / stats.median,
        c.close.to_double() / stats.median,
    };

    for (double r : ratios) {
        if (r > 100.0 || r < 0.01) {
            flags = flags | SpikeFlags::DataError;
            break;
        }
    }

    return flags;
}

// ==============================================================================
// 跳空异常检测
// ==============================================================================
SpikeFlags SpikeFilter::detect_gap(
    const Candle& c, const RollingStats& stats) const noexcept
{
    SpikeFlags flags = SpikeFlags::None;

    if (history_.empty()) return flags;
    if (stats.avg_range <= 0.0) return flags;

    const auto& prev = history_.back();
    const double gap = std::abs(
        c.open.to_double() - prev.close.to_double());

    const double threshold = stats.avg_range * config_.max_gap_atr_multiple;

    if (gap > threshold) {
        flags = flags | SpikeFlags::GapAnomaly;
    }

    return flags;
}

// ==============================================================================
// MAD 偏离检测
// ==============================================================================
SpikeFlags SpikeFilter::detect_mad_deviation(
    const Candle& c, const RollingStats& stats) const noexcept
{
    SpikeFlags flags = SpikeFlags::None;

    if (stats.mad <= 0.0 || stats.median <= 0.0) return flags;

    const double threshold = config_.mad_multiplier * stats.mad;

    if (std::abs(c.close.to_double() - stats.median) > threshold) {
        flags = flags | SpikeFlags::MadDeviation;
    }

    return flags;
}

// ==============================================================================
// 绝对偏离检测
// ==============================================================================
SpikeFlags SpikeFilter::detect_absolute_deviation(
    const Candle& c, const RollingStats& stats) const noexcept
{
    SpikeFlags flags = SpikeFlags::None;

    if (stats.median <= 0.0) return flags;

    const double dev_pct = std::abs(
        c.close.to_double() - stats.median) / stats.median;

    if (dev_pct > config_.max_price_deviation_pct) {
        flags = flags | SpikeFlags::AbsoluteDeviation;
    }

    return flags;
}

// ==============================================================================
// 成交量异常检测
// ==============================================================================
SpikeFlags SpikeFilter::detect_volume_anomaly(
    const Candle& c, const RollingStats& stats) const noexcept
{
    SpikeFlags flags = SpikeFlags::None;

    if (stats.avg_volume <= 0.0) return flags;

    const double ratio = c.volume.to_double() / stats.avg_volume;

    if (ratio > config_.volume_spike_multiple) {
        flags = flags | SpikeFlags::VolumeAnomaly;
    }

    return flags;
}

// ==============================================================================
// 截断影线
// ==============================================================================
Candle SpikeFilter::truncate_shadows(
    const Candle& c, SpikeFlags flags,
    const RollingStats& stats) const noexcept
{
    Candle corrected = c;

    const double body = compute_body(c);
    const double effective_body = std::max(body, config_.min_body_absolute);
    const double threshold = effective_body * config_.shadow_body_ratio;

    const double top = std::max(c.open.to_double(), c.close.to_double());
    const double bottom = std::min(c.open.to_double(), c.close.to_double());

    // 截断上影线
    if (has_flag(flags, SpikeFlags::UpperShadowSpike)) {
        const double max_high = top + threshold;
        if (c.high.to_double() > max_high) {
            corrected.high = Price::from_double(max_high);
        }
    }

    // 截断下影线
    if (has_flag(flags, SpikeFlags::LowerShadowSpike)) {
        const double min_low = bottom - threshold;
        if (c.low.to_double() < min_low) {
            corrected.low = Price::from_double(min_low);
        }
    }

    // 跳空修正：open 拉回上一根 close 附近
    if (has_flag(flags, SpikeFlags::GapAnomaly) && !history_.empty()) {
        const double prev_close = history_.back().close.to_double();
        corrected.open = Price::from_double(prev_close);
    }

    // 统一边界处理
    clamp_ohlc(corrected);

    return corrected;
}

// ==============================================================================
// OHLC 边界 clamp
// ==============================================================================
void SpikeFilter::clamp_ohlc(Candle& c) const noexcept {
    // 确保 high >= low
    if (c.high < c.low) {
        std::swap(c.high, c.low);
    }

    // open 在范围内
    if (c.open > c.high) c.open = c.high;
    if (c.open < c.low)  c.open = c.low;

    // close 在范围内
    if (c.close > c.high) c.close = c.high;
    if (c.close < c.low)  c.close = c.low;
}

// ==============================================================================
// 历史管理
// ==============================================================================
void SpikeFilter::push_history(const Candle& c) {
    history_.push_back(c);

    // 限制历史大小
    const std::size_t max_size = std::max(
        config_.stats_window, config_.context_window);

    while (history_.size() > max_size) {
        history_.pop_front();
    }
}

// ==============================================================================
// 策略决策
// ==============================================================================
SpikePolicy SpikeFilter::decide_policy(SpikeFlags flags) const noexcept {
    // 严重错误：直接拒绝
    if (has_flag(flags, SpikeFlags::Critical)) {
        return config_.critical_policy;
    }

    // 其他情况：按默认策略处理
    return config_.default_policy;
}

// ==============================================================================
// 统计更新
// ==============================================================================
void SpikeFilter::update_stats(SpikeFlags flags, SpikePolicy policy) noexcept {
    // 策略计数
    switch (policy) {
        case SpikePolicy::Pass:
            stats_.passed.fetch_add(1, std::memory_order_relaxed);
            break;
        case SpikePolicy::Mark:
            stats_.marked.fetch_add(1, std::memory_order_relaxed);
            break;
        case SpikePolicy::Truncate:
            stats_.truncated.fetch_add(1, std::memory_order_relaxed);
            break;
        case SpikePolicy::Reject:
            stats_.rejected.fetch_add(1, std::memory_order_relaxed);
            break;
    }

    // 标志计数
    if (has_flag(flags, SpikeFlags::UpperShadowSpike)) {
        stats_.upper_shadow_spikes.fetch_add(1, std::memory_order_relaxed);
    }
    if (has_flag(flags, SpikeFlags::LowerShadowSpike)) {
        stats_.lower_shadow_spikes.fetch_add(1, std::memory_order_relaxed);
    }
    if (has_flag(flags, SpikeFlags::FlashCrash)) {
        stats_.flash_crashes.fetch_add(1, std::memory_order_relaxed);
    }
    if (has_flag(flags, SpikeFlags::FlashSpike)) {
        stats_.flash_spikes.fetch_add(1, std::memory_order_relaxed);
    }
    if (has_flag(flags, SpikeFlags::DataError)) {
        stats_.data_errors.fetch_add(1, std::memory_order_relaxed);
    }
    if (has_flag(flags, SpikeFlags::GapAnomaly)) {
        stats_.gap_anomalies.fetch_add(1, std::memory_order_relaxed);
    }
    if (has_flag(flags, SpikeFlags::VolumeAnomaly)) {
        stats_.volume_anomalies.fetch_add(1, std::memory_order_relaxed);
    }
    if (has_flag(flags, SpikeFlags::MadDeviation)) {
        stats_.mad_deviations.fetch_add(1, std::memory_order_relaxed);
    }
    if (has_flag(flags, SpikeFlags::AbsoluteDeviation)) {
        stats_.absolute_deviations.fetch_add(1, std::memory_order_relaxed);
    }
}

// ==============================================================================
// 连续插针告警
// ==============================================================================
void SpikeFilter::check_continuous_spike(SpikeFlags flags) noexcept {
    const bool is_spike =
        has_flag(flags, SpikeFlags::UpperShadowSpike) ||
        has_flag(flags, SpikeFlags::LowerShadowSpike) ||
        has_flag(flags, SpikeFlags::FlashCrash) ||
        has_flag(flags, SpikeFlags::FlashSpike) ||
        has_flag(flags, SpikeFlags::DataError);

    if (!is_spike) {
        continuous_spike_count_ = 0;
        return;
    }

    ++continuous_spike_count_;

    // 更新最大连续
    if (continuous_spike_count_ > max_continuous_spike_) {
        max_continuous_spike_ = continuous_spike_count_;
        auto peak = stats_.continuous_spike_max.load(std::memory_order_relaxed);
        while (max_continuous_spike_ > peak &&
               !stats_.continuous_spike_max.compare_exchange_weak(
                   peak, max_continuous_spike_,
                   std::memory_order_relaxed)) {}
    }

    // 超过阈值告警
    if (continuous_spike_count_ == config_.continuous_spike_threshold) {
        stats_.continuous_spike_warnings.fetch_add(1,
            std::memory_order_relaxed);
        QUANT_LOG_WARN(
            "[SpikeFilter] {} 连续 {} 根插针，可能有数据源异常",
            symbol_, continuous_spike_count_);
    }
}

// ==============================================================================
// 诊断
// ==============================================================================
std::string SpikeFilter::dump() const {
    std::string result;
    result.reserve(1024);
    char buf[512];

    result += "SpikeFilter Dump:\n";

    std::snprintf(buf, sizeof(buf), "  symbol:              %s\n",
        symbol_.empty() ? "(none)" : symbol_.c_str());
    result += buf;

    std::snprintf(buf, sizeof(buf), "  history_size:        %zu\n",
        history_.size());
    result += buf;

    std::snprintf(buf, sizeof(buf), "  total_processed:     %llu\n",
        static_cast<unsigned long long>(
            stats_.total_processed.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  passed:              %llu\n",
        static_cast<unsigned long long>(
            stats_.passed.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  marked:              %llu\n",
        static_cast<unsigned long long>(
            stats_.marked.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  truncated:           %llu\n",
        static_cast<unsigned long long>(
            stats_.truncated.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  rejected:            %llu\n",
        static_cast<unsigned long long>(
            stats_.rejected.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  upper_shadow_spikes: %llu\n",
        static_cast<unsigned long long>(
            stats_.upper_shadow_spikes.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  lower_shadow_spikes: %llu\n",
        static_cast<unsigned long long>(
            stats_.lower_shadow_spikes.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  flash_crashes:       %llu\n",
        static_cast<unsigned long long>(
            stats_.flash_crashes.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  flash_spikes:        %llu\n",
        static_cast<unsigned long long>(
            stats_.flash_spikes.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  data_errors:         %llu\n",
        static_cast<unsigned long long>(
            stats_.data_errors.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  gap_anomalies:       %llu\n",
        static_cast<unsigned long long>(
            stats_.gap_anomalies.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  mad_deviations:      %llu\n",
        static_cast<unsigned long long>(
            stats_.mad_deviations.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  absolute_deviations: %llu\n",
        static_cast<unsigned long long>(
            stats_.absolute_deviations.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  spike_rate:          %.2f%%\n",
        stats_.spike_rate() * 100.0);
    result += buf;

    std::snprintf(buf, sizeof(buf), "  acceptance_rate:     %.2f%%\n",
        stats_.acceptance_rate() * 100.0);
    result += buf;

    std::snprintf(buf, sizeof(buf), "  continuous_spike:    %llu (max: %llu)\n",
        static_cast<unsigned long long>(continuous_spike_count_),
        static_cast<unsigned long long>(max_continuous_spike_));
    result += buf;

    return result;
}

}  // namespace data
}  // namespace quant
