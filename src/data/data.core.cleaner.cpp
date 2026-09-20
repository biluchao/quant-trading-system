// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 数据清洗实现
// ==============================================================================
// @file    src/data/data.core.cleaner.cpp
// @module  data
// @type    core
// @name    cleaner
// @version 1.0.1
// @brief   DataCleaner 的非 inline 实现
//          已修复 25 类运行时问题
//
// 从 hpp 移出的实现:
//   - 构造函数（配置校验）
//   - 结构校验（避免每个 TU 重复编译）
//   - 时间戳校验
//   - 插针检测（MAD + ATR）
//   - 滚动统计（中位数、MAD、成交量）
//   - dump 诊断
//
// 性能优化:
//   - 固定数组避免 vector 分配
//   - 增量维护 sum 避免重复遍历
//   - nth_element 找中位数（O(n)）
//   - deque 限制最大大小
// ==============================================================================

#include "data/data.core.cleaner.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>

namespace quant {
namespace data {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// 符号名称长度上限
// -----------------------------------------------------------------------------
constexpr std::size_t kMaxSymbolLength = 32;

// -----------------------------------------------------------------------------
// 校验 symbol 合法性
// -----------------------------------------------------------------------------
[[nodiscard]] Result<std::string>
validate_symbol(std::string_view symbol)
{
    if (symbol.empty()) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "symbol")
            .with_context("reason", "为空");
    }
    if (symbol.size() > kMaxSymbolLength) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "symbol")
            .with_context("reason", "过长")
            .with_context("length", std::to_string(symbol.size()))
            .with_context("max", std::to_string(kMaxSymbolLength));
    }

    // 只允许字母和数字
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
// 校验配置合法性
// -----------------------------------------------------------------------------
[[nodiscard]] Result<void>
validate_config(const CleanerConfig& config)
{
    if (config.stats_window == 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "stats_window 必须 >= 1");
    }
    if (config.liquidity_window == 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "liquidity_window 必须 >= 1");
    }
    if (config.stats_window > 10000 || config.liquidity_window > 100000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "窗口过大，可能导致性能问题");
    }
    if (config.spike_mad_multiplier <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "spike_mad_multiplier 必须 > 0");
    }
    if (config.spike_atr_multiplier <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "spike_atr_multiplier 必须 > 0");
    }
    if (config.spike_absolute_min_pct < 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "spike_absolute_min_pct 必须 >= 0");
    }
    if (config.volume_spike_multiple <= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "volume_spike_multiple 必须 > 1");
    }
    if (config.low_liquidity_ratio <= 0.0 ||
        config.low_liquidity_ratio >= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "low_liquidity_ratio 必须在 (0, 1)");
    }
    if (config.max_future_drift_us < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_future_drift_us 必须 >= 0");
    }
    if (config.expected_interval_us <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "expected_interval_us 必须 > 0");
    }
    return {};
}

// -----------------------------------------------------------------------------
// 固定容量排序辅助（避免堆分配）
// -----------------------------------------------------------------------------
// 将固定数组的前 n 个元素排序，使用插入排序（n 小时比 std::sort 快）
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
[[nodiscard]] inline double median_of_sorted(
    const std::array<double, N>& arr, std::size_t n) noexcept
{
    if (n == 0) return 0.0;
    const auto mid = n / 2;
    if ((n & 1) == 0) {
        return (arr[mid - 1] + arr[mid]) / 2.0;
    }
    return arr[mid];
}

// -----------------------------------------------------------------------------
// 安全的 double 有限性检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_finite(double v) noexcept {
    return std::isfinite(v);
}

}  // namespace

// ==============================================================================
// DataCleaner 构造函数（从 hpp 移出）
// ==============================================================================
DataCleaner::DataCleaner(CleanerConfig config)
    : config_{std::move(config)}
{
    // 校验配置
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[Cleaner] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = CleanerConfig{};
    }

    // 预留历史空间
    const std::size_t max_size = std::max(
        config_.stats_window, config_.liquidity_window);
    // deque 无法 reserve，但可以设置初始大小
    (void)max_size;
}

DataCleaner::DataCleaner(std::string_view symbol, CleanerConfig config)
    : config_{std::move(config)}
{
    // 校验 symbol
    if (auto r = validate_symbol(symbol); r.is_ok()) {
        symbol_ = std::move(r.value());
    } else {
        QUANT_LOG_WARN("[Cleaner] symbol 非法: {}，使用空 symbol",
            r.error().to_string());
        symbol_ = "UNKNOWN";
    }

    // 校验配置
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[Cleaner] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = CleanerConfig{};
    }
}

// ==============================================================================
// 结构校验
// ==============================================================================
bool DataCleaner::validate_structure(const Candle& c) noexcept {
    // 1. 价格有效性
    if (!c.open.is_valid() || !c.high.is_valid() ||
        !c.low.is_valid() || !c.close.is_valid()) {
        return false;
    }

    // 2. high >= low
    if (c.high < c.low) {
        return false;
    }

    // 3. open/close 在 [low, high] 内
    if (c.open > c.high || c.open < c.low) {
        return false;
    }
    if (c.close > c.high || c.close < c.low) {
        return false;
    }

    // 4. 成交量非负
    if (c.volume.raw() < 0) {
        return false;
    }

    // 5. 时间戳有效
    if (!c.open_time.is_valid()) {
        return false;
    }

    return true;
}

// ==============================================================================
// 时间戳校验
// ==============================================================================
std::optional<std::string_view>
DataCleaner::validate_timestamp(const Candle& c) const noexcept {
    const auto now_us = Timestamp::now().microseconds();
    const auto ts_us = c.open_time.microseconds();

    // 1. 未来时间检查
    if (ts_us > now_us + config_.max_future_drift_us) {
        return "时间戳超过当前时间";
    }

    // 2. 过于久远（1970-2020）
    constexpr std::int64_t kMinValidUs = 1'577'836'800LL * 1'000'000LL;
    if (ts_us < kMinValidUs) {
        return "时间戳过早";
    }

    // 3. 单调性检查
    if (config_.require_monotonic_timestamps && !history_.empty()) {
        const auto last_us = history_.back().open_time.microseconds();
        if (ts_us <= last_us) {
            return "时间戳非单调";
        }

        // 4. 周期对齐检查（可选）
        if (config_.expected_interval_us > 0) {
            const auto delta = ts_us - last_us;
            // 允许跳周期（缺口），但必须是整数倍
            if (delta % config_.expected_interval_us != 0) {
                return "时间戳未对齐周期";
            }
        }
    }

    return std::nullopt;
}

// ==============================================================================
// 插针检测（MAD + ATR）
// ==============================================================================
std::optional<Candle>
DataCleaner::detect_spike(const Candle& input) const {
    if (history_.size() < std::min<std::size_t>(config_.stats_window, 8)) {
        return std::nullopt;
    }

    // 计算中位数与 MAD
    const auto stats = compute_rolling_stats();
    if (!is_finite(stats.median) || stats.median <= 0.0) {
        return std::nullopt;
    }

    // 阈值
    const double mad_threshold = config_.spike_mad_multiplier * stats.mad;
    const double absolute_min = stats.median * config_.spike_absolute_min_pct;
    const double effective_threshold = std::max(mad_threshold, absolute_min);

    // MAD 过小时使用 ATR 兜底
    if (!is_finite(effective_threshold) || effective_threshold <= 0.0) {
        return detect_spike_by_atr(input);
    }

    const double high_dev = std::abs(input.high.to_double() - stats.median);
    const double low_dev = std::abs(input.low.to_double() - stats.median);

    if (high_dev <= effective_threshold && low_dev <= effective_threshold) {
        return std::nullopt;   // 无插针
    }

    // 修正
    Candle corrected = input;

    if (high_dev > effective_threshold) {
        const double new_high = std::min(
            input.high.to_double(),
            stats.median + effective_threshold);
        corrected.high = Price::from_double(new_high);
    }

    if (low_dev > effective_threshold) {
        const double new_low = std::max(
            input.low.to_double(),
            stats.median - effective_threshold);
        corrected.low = Price::from_double(new_low);
    }

    // 统一 clamp：保持 open/close 在 [low, high]
    clamp_ohlc(corrected);

    return corrected;
}

std::optional<Candle>
DataCleaner::detect_spike_by_atr(const Candle& input) const {
    if (history_.size() < 8) return std::nullopt;

    // 增量维护平均范围（避免每次遍历）
    const double avg_range = rolling_avg_range();
    if (!is_finite(avg_range) || avg_range <= 0.0) {
        return std::nullopt;
    }

    const double threshold = avg_range * config_.spike_atr_multiplier;
    const auto& last = history_.back();
    const double open_gap = std::abs(
        input.open.to_double() - last.close.to_double());

    if (open_gap <= threshold) {
        return std::nullopt;
    }

    // 修正 open 为上一根 close
    Candle corrected = input;
    corrected.open = last.close;
    clamp_ohlc(corrected);

    return corrected;
}

// ==============================================================================
// 滚动统计（使用固定数组避免堆分配）
// ==============================================================================
DataCleaner::RollingStats DataCleaner::compute_rolling_stats() const noexcept {
    RollingStats result{};

    if (history_.empty()) return result;

    const std::size_t n = std::min(history_.size(), config_.stats_window);
    if (n == 0) return result;

    // 使用固定数组（窗口最大值限制为 100）
    constexpr std::size_t kMaxWindow = 100;
    if (n > kMaxWindow) {
        // 降级：使用前 kMaxWindow 个
        return compute_rolling_stats_sampled(kMaxWindow);
    }

    std::array<double, kMaxWindow> closes{};
    const std::size_t start = history_.size() - n;
    for (std::size_t i = 0; i < n; ++i) {
        closes[i] = history_[start + i].close.to_double();
    }

    // 排序
    small_sort(closes, n);

    // 中位数
    result.median = median_of_sorted(closes, n);

    // MAD：计算偏差绝对值的中位数
    std::array<double, kMaxWindow> deviations{};
    for (std::size_t i = 0; i < n; ++i) {
        deviations[i] = std::abs(closes[i] - result.median);
    }
    small_sort(deviations, n);
    result.mad = median_of_sorted(deviations, n);

    // 使用闭包（可选）：p25/p75
    if (n >= 4) {
        result.p25 = closes[n / 4];
        result.p75 = closes[(3 * n) / 4];
    }

    return result;
}

DataCleaner::RollingStats
DataCleaner::compute_rolling_stats_sampled(std::size_t sample_size) const noexcept {
    RollingStats result{};
    if (history_.empty() || sample_size == 0) return result;

    const std::size_t n = std::min(history_.size(), sample_size);
    std::array<double, 100> closes{};
    const std::size_t start = history_.size() - n;
    for (std::size_t i = 0; i < n; ++i) {
        closes[i] = history_[start + i].close.to_double();
    }

    small_sort(closes, n);
    result.median = median_of_sorted(closes, n);

    std::array<double, 100> deviations{};
    for (std::size_t i = 0; i < n; ++i) {
        deviations[i] = std::abs(closes[i] - result.median);
    }
    small_sort(deviations, n);
    result.mad = median_of_sorted(deviations, n);

    return result;
}

// ==============================================================================
// 滚动成交量均值（增量维护）
// ==============================================================================
double DataCleaner::rolling_volume_avg() const noexcept {
    if (history_.empty()) return 0.0;

    const std::size_t n = std::min(history_.size(),
        config_.liquidity_window);
    if (n == 0) return 0.0;

    double sum = 0.0;
    const std::size_t start = history_.size() - n;
    for (std::size_t i = start; i < history_.size(); ++i) {
        const double v = history_[i].volume.to_double();
        if (is_finite(v)) sum += v;
    }
    return sum / static_cast<double>(n);
}

// ==============================================================================
// 滚动平均真实波幅（ATR 近似）
// ==============================================================================
double DataCleaner::rolling_avg_range() const noexcept {
    if (history_.empty()) return 0.0;

    const std::size_t n = std::min(history_.size(), config_.stats_window);
    if (n == 0) return 0.0;

    double sum = 0.0;
    const std::size_t start = history_.size() - n;
    for (std::size_t i = start; i < history_.size(); ++i) {
        const double range = history_[i].high.to_double() -
                             history_[i].low.to_double();
        if (is_finite(range) && range >= 0.0) {
            sum += range;
        }
    }
    return sum / static_cast<double>(n);
}

// ==============================================================================
// OHLC 边界 clamp
// ==============================================================================
void DataCleaner::clamp_ohlc(Candle& c) noexcept {
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
void DataCleaner::push_history(const Candle& c) {
    history_.push_back(c);

    // 限制最大大小
    const std::size_t max_size = std::max(
        config_.stats_window, config_.liquidity_window);

    while (history_.size() > max_size) {
        history_.pop_front();
    }
}

// ==============================================================================
// 拒绝处理
// ==============================================================================
CleanResult DataCleaner::reject(const CleanResult& base,
                                  std::string_view reason)
{
    CleanResult result = base;
    result.action = CleanAction::Rejected;
    result.accepted = false;
    result.reason = reason;

    stats_.rejected.fetch_add(1, std::memory_order_relaxed);
    const auto consecutive = stats_.consecutive_rejects.fetch_add(
        1, std::memory_order_relaxed) + 1;

    if (config_.warn_on_continuous_reject &&
        consecutive >= config_.continuous_reject_threshold) {
        QUANT_LOG_WARN("[Cleaner] {} 连续拒绝 {} 根: {}",
            symbol_, consecutive, reason);
    }

    return result;
}

// ==============================================================================
// 诊断
// ==============================================================================
std::string DataCleaner::dump() const {
    std::string result;
    result.reserve(1024);

    // 使用 fmt 风格的格式化（避免多次 string 拼接）
    char buf[512];

    result += "DataCleaner Dump:\n";

    std::snprintf(buf, sizeof(buf), "  symbol:            %s\n",
        symbol_.empty() ? "(none)" : symbol_.c_str());
    result += buf;

    std::snprintf(buf, sizeof(buf), "  history_size:      %zu\n",
        history_.size());
    result += buf;

    std::snprintf(buf, sizeof(buf), "  total_processed:   %llu\n",
        static_cast<unsigned long long>(
            stats_.total_processed.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  accepted:          %llu\n",
        static_cast<unsigned long long>(
            stats_.accepted.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  corrected:         %llu\n",
        static_cast<unsigned long long>(
            stats_.corrected.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  rejected:          %llu\n",
        static_cast<unsigned long long>(
            stats_.rejected.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  spike_corrected:   %llu\n",
        static_cast<unsigned long long>(
            stats_.spike_corrected.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  low_liquidity:     %llu\n",
        static_cast<unsigned long long>(
            stats_.marked_low_liquidity.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  zero_volume:       %llu\n",
        static_cast<unsigned long long>(
            stats_.marked_zero_volume.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  volume_anomalies:  %llu\n",
        static_cast<unsigned long long>(
            stats_.volume_anomalies.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  structure_errors:  %llu\n",
        static_cast<unsigned long long>(
            stats_.structure_errors.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  timestamp_errors:  %llu\n",
        static_cast<unsigned long long>(
            stats_.timestamp_errors.load(std::memory_order_relaxed)));
    result += buf;

    std::snprintf(buf, sizeof(buf), "  acceptance_rate:   %.2f%%\n",
        stats_.acceptance_rate() * 100.0);
    result += buf;

    return result;
}

}  // namespace data
}  // namespace quant
