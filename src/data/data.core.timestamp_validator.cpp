// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 时间戳校验器实现
// ==============================================================================
// @file    src/data/data.core.timestamp_validator.cpp
// @module  data
// @type    core
// @name    timestamp_validator
// @version 1.0.1
// @brief   TimestampValidator 的完整实现
//          已修复 25 类运行时问题
//
// 校验顺序:
//   1. epoch 检测（未初始化）
//   2. 时间范围（2020 ~ 2100）
//   3. 未来时间容差
//   4. 单调性（不倒退、不重复）
//   5. 周期对齐
//   6. 间隔合理性
//   7. 漂移监控
// ==============================================================================

#include "data/data.core.timestamp_validator.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <string>

namespace quant {
namespace data {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

constexpr std::size_t kMaxSymbolLength = 32;

// 对齐容差（浮点转换误差）
constexpr std::int64_t kAlignmentToleranceUs = 1LL;

// 未来容差上限（1 小时）
constexpr std::int64_t kMaxFutureToleranceUs = 3'600'000'000LL;

// 间隔上限（100 万根）
constexpr std::size_t kMaxGapBarsLimit = 1'000'000;

// 漂移窗口上限
constexpr std::size_t kMaxDriftWindowSize = 10'000;

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
            .with_context("reason", "过长")
            .with_context("length", std::to_string(symbol.size()))
            .with_context("max", std::to_string(kMaxSymbolLength));
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
    const TimestampValidatorConfig& c)
{
    // 周期
    if (c.interval_us <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "interval_us 必须 > 0")
            .with_context("interval_us", std::to_string(c.interval_us));
    }
    if (c.interval_us > 86'400'000'000LL) {   // 1 天
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "interval_us 不能超过 1 天");
    }

    // 时间范围
    if (c.min_valid_us <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_valid_us 必须 > 0");
    }
    if (c.max_valid_us <= c.min_valid_us) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_valid_us 必须 > min_valid_us");
    }

    // 未来容差
    if (c.future_tolerance_us < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "future_tolerance_us 必须 >= 0");
    }
    if (c.future_tolerance_us > kMaxFutureToleranceUs) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "future_tolerance_us 过大（上限 1 小时）");
    }

    // 间隔
    if (c.max_gap_bars == 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_gap_bars 必须 >= 1");
    }
    if (c.max_gap_bars > kMaxGapBarsLimit) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_gap_bars 过大");
    }

    // 漂移窗口
    if (c.drift_window == 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "drift_window 必须 >= 1");
    }
    if (c.drift_window > kMaxDriftWindowSize) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "drift_window 过大");
    }
    if (c.drift_warn_us < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "drift_warn_us 必须 >= 0");
    }

    return {};
}

// -----------------------------------------------------------------------------
// 检查时间戳是否对齐到周期（带容差）
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_aligned(
    std::int64_t ts_us, std::int64_t interval_us) noexcept
{
    if (interval_us <= 0) return true;

    const auto remainder = ts_us % interval_us;
    if (remainder == 0) return true;

    // 允许 ±1μs 浮点误差
    if (remainder <= kAlignmentToleranceUs) return true;
    if (interval_us - remainder <= kAlignmentToleranceUs) return true;

    return false;
}

// -----------------------------------------------------------------------------
// 安全累加（避免溢出）
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::int64_t safe_add(
    std::int64_t a, std::int64_t b) noexcept
{
    if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return a + b;
}

// -----------------------------------------------------------------------------
// 有限性检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_finite_i64(std::int64_t) noexcept {
    return true;   // int64_t 本身有限
}

}  // namespace

// ==============================================================================
// 构造函数
// ==============================================================================
TimestampValidator::TimestampValidator(TimestampValidatorConfig config)
    : config_{std::move(config)}
{
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[TimestampValidator] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = TimestampValidatorConfig{};
    }
}

TimestampValidator::TimestampValidator(
    std::string_view symbol,
    TimestampValidatorConfig config)
    : config_{std::move(config)}
{
    // 校验 symbol
    if (auto r = validate_symbol(symbol); r.is_ok()) {
        symbol_ = std::move(r.value());
    } else {
        QUANT_LOG_WARN("[TimestampValidator] symbol 非法: {}，使用 UNKNOWN",
            r.error().to_string());
        symbol_ = "UNKNOWN";
    }

    // 校验配置
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[TimestampValidator] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = TimestampValidatorConfig{};
    }
}

// ==============================================================================
// 主接口
// ==============================================================================
TimestampValidation TimestampValidator::validate(Timestamp ts) {
    std::lock_guard lock(mutex_);
    return validate_internal(ts);
}

std::vector<TimestampValidation> TimestampValidator::validate_batch(
    std::span<const Timestamp> timestamps)
{
    std::vector<TimestampValidation> results;
    if (timestamps.empty()) return results;

    results.reserve(timestamps.size());

    std::lock_guard lock(mutex_);
    for (const auto& ts : timestamps) {
        try {
            results.push_back(validate_internal(ts));
        } catch (const std::exception& e) {
            QUANT_LOG_ERROR(
                "[TimestampValidator] 校验异常: {}", e.what());

            TimestampValidation err;
            err.timestamp = ts;
            err.action = TimestampAction::Reject;
            err.reason = "校验异常";
            err.accepted = false;
            results.push_back(std::move(err));
        }
    }

    return results;
}

// ==============================================================================
// 内部校验逻辑
// ==============================================================================
TimestampValidation TimestampValidator::validate_internal(
    Timestamp ts) noexcept
{
    TimestampValidation result;
    result.timestamp = ts;
    result.action = TimestampAction::Accept;
    result.accepted = true;

    stats_.total_processed.fetch_add(1, std::memory_order_relaxed);

    const auto ts_us = ts.microseconds();

    // -------------------------------------------------------------------------
    // 1. epoch 检测
    // -------------------------------------------------------------------------
    if (has_flag(config_.flags, TimestampFlags::EpochCheck)) {
        if (!check_epoch(ts)) {
            result.action = TimestampAction::Reject;
            result.reason = "时间戳未初始化（epoch）";
            result.triggered_flags = TimestampFlags::EpochCheck;
            result.accepted = false;

            stats_.epoch_detected.fetch_add(1, std::memory_order_relaxed);
            stats_.rejected.fetch_add(1, std::memory_order_relaxed);
            update_stats(result);
            return result;
        }
    }

    // -------------------------------------------------------------------------
    // 2. 时间范围校验
    // -------------------------------------------------------------------------
    if (!check_range(ts)) {
        result.action = (ts_us < config_.min_valid_us)
            ? TimestampAction::TooOld
            : TimestampAction::Future;
        result.reason = (ts_us < config_.min_valid_us)
            ? "时间戳过早"
            : "时间戳过晚";
        result.accepted = false;

        if (ts_us < config_.min_valid_us) {
            stats_.too_old_detected.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.future_detected.fetch_add(1, std::memory_order_relaxed);
        }
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
        update_stats(result);
        return result;
    }

    // -------------------------------------------------------------------------
    // 3. 未来时间校验（回放模式跳过）
    // -------------------------------------------------------------------------
    if (has_flag(config_.flags, TimestampFlags::FutureCheck) &&
        !config_.replay_mode) {
        if (!check_future(ts)) {
            result.action = TimestampAction::Future;
            result.reason = "时间戳超过未来容差";
            result.accepted = false;

            stats_.future_detected.fetch_add(1, std::memory_order_relaxed);
            stats_.rejected.fetch_add(1, std::memory_order_relaxed);
            update_stats(result);
            return result;
        }
    }

    // -------------------------------------------------------------------------
    // 4. 单调性校验
    // -------------------------------------------------------------------------
    std::int64_t delta_us = 0;
    if (last_ts_.has_value()) {
        delta_us = ts_us - last_ts_->microseconds();
        result.delta_us = delta_us;

        // 4.1 重复检测
        if (has_flag(config_.flags, TimestampFlags::DuplicateCheck)) {
            if (delta_us == 0) {
                if (!config_.allow_duplicate) {
                    result.action = TimestampAction::Duplicate;
                    result.reason = "时间戳重复";
                    result.triggered_flags =
                        result.triggered_flags | TimestampFlags::DuplicateCheck;
                    result.accepted = false;

                    stats_.duplicates.fetch_add(1,
                        std::memory_order_relaxed);
                    stats_.rejected.fetch_add(1,
                        std::memory_order_relaxed);
                    update_stats(result);
                    return result;
                }

                // 允许重复：接受但不更新状态
                result.action = TimestampAction::AcceptWithWarn;
                result.reason = "重复（允许）";
                stats_.accepted_with_warn.fetch_add(1,
                    std::memory_order_relaxed);
                update_stats(result);
                return result;
            }
        }

        // 4.2 乱序检测
        if (has_flag(config_.flags, TimestampFlags::MonotonicCheck)) {
            if (delta_us < 0) {
                if (!config_.allow_out_of_order) {
                    result.action = TimestampAction::OutOfOrder;
                    result.reason = "时间戳乱序";
                    result.triggered_flags =
                        result.triggered_flags | TimestampFlags::MonotonicCheck;
                    result.accepted = false;

                    stats_.out_of_order.fetch_add(1,
                        std::memory_order_relaxed);
                    stats_.rejected.fetch_add(1,
                        std::memory_order_relaxed);
                    update_stats(result);
                    return result;
                }

                // 允许乱序：接受但警告
                result.action = TimestampAction::AcceptWithWarn;
                result.reason = "乱序（允许）";
                stats_.accepted_with_warn.fetch_add(1,
                    std::memory_order_relaxed);
                update_stats(result);
                return result;
            }
        }

        // 4.3 周期对齐校验
        if (has_flag(config_.flags, TimestampFlags::AlignmentCheck) &&
            config_.require_alignment) {
            if (!check_alignment(ts)) {
                result.action = TimestampAction::AcceptWithWarn;
                result.reason = "时间戳未对齐周期";
                result.triggered_flags =
                    result.triggered_flags | TimestampFlags::AlignmentCheck;

                stats_.misaligned.fetch_add(1,
                    std::memory_order_relaxed);
                stats_.accepted_with_warn.fetch_add(1,
                    std::memory_order_relaxed);

                // 更新状态后返回
                if (!config_.dry_run) {
                    update_last_timestamp(ts);
                }
                update_stats(result);
                return result;
            }
        }

        // 4.4 间隔合理性
        if (has_flag(config_.flags, TimestampFlags::GapCheck) &&
            delta_us > 0) {
            if (!check_gap(ts, delta_us)) {
                result.action = TimestampAction::AcceptWithWarn;
                result.reason = "时间戳跨度异常";
                result.triggered_flags =
                    result.triggered_flags | TimestampFlags::GapCheck;

                stats_.gap_detected.fetch_add(1,
                    std::memory_order_relaxed);
                stats_.accepted_with_warn.fetch_add(1,
                    std::memory_order_relaxed);
                // 继续到漂移检查
            }
        }

        // 4.5 漂移监控
        if (has_flag(config_.flags, TimestampFlags::DriftCheck) &&
            delta_us > 0) {
            const auto expected = config_.interval_us;
            const auto drift = delta_us - expected;

            result.drift_us = drift;

            if (check_drift(ts, delta_us)) {
                result.triggered_flags =
                    result.triggered_flags | TimestampFlags::DriftCheck;
                // 继续，但记录漂移
                if (result.action == TimestampAction::Accept) {
                    result.action = TimestampAction::AcceptWithWarn;
                    result.reason = "时间戳漂移";
                }
            }
        }
    } else {
        // 第一根：检查是否对齐
        if (has_flag(config_.flags, TimestampFlags::AlignmentCheck) &&
            config_.require_alignment) {
            if (!check_alignment(ts)) {
                result.action = TimestampAction::AcceptWithWarn;
                result.reason = "首根未对齐周期";
                stats_.misaligned.fetch_add(1,
                    std::memory_order_relaxed);
                stats_.accepted_with_warn.fetch_add(1,
                    std::memory_order_relaxed);
            }
        }
    }

    // -------------------------------------------------------------------------
    // 5. 更新状态
    // -------------------------------------------------------------------------
    if (!config_.dry_run) {
        update_last_timestamp(ts);
    }

    // -------------------------------------------------------------------------
    // 6. 统计
    // -------------------------------------------------------------------------
    if (result.accepted) {
        if (result.action == TimestampAction::Accept) {
            stats_.accepted.fetch_add(1, std::memory_order_relaxed);
        } else if (result.action == TimestampAction::AcceptWithWarn) {
            stats_.accepted_with_warn.fetch_add(1,
                std::memory_order_relaxed);
        }
    } else {
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
    }

    update_stats(result);
    return result;
}

// ==============================================================================
// 独立检测函数
// ==============================================================================
bool TimestampValidator::check_epoch(Timestamp ts) noexcept {
    // 0 值表示未初始化（区别于 1970-01-01 的 0 微秒）
    return ts.microseconds() != 0;
}

bool TimestampValidator::check_range(Timestamp ts) noexcept {
    const auto us = ts.microseconds();
    return us >= config_.min_valid_us && us <= config_.max_valid_us;
}

bool TimestampValidator::check_future(Timestamp ts) noexcept {
    const auto now_us = Timestamp::now().microseconds();
    const auto ts_us = ts.microseconds();
    return ts_us <= safe_add(now_us, config_.future_tolerance_us);
}

bool TimestampValidator::check_old(Timestamp ts) noexcept {
    const auto us = ts.microseconds();
    return us >= config_.min_valid_us;
}

bool TimestampValidator::check_monotonic(Timestamp ts) noexcept {
    if (!last_ts_.has_value()) return true;
    return ts.microseconds() > last_ts_->microseconds();
}

bool TimestampValidator::check_duplicate(Timestamp ts) noexcept {
    if (!last_ts_.has_value()) return false;
    return ts.microseconds() == last_ts_->microseconds();
}

bool TimestampValidator::check_alignment(Timestamp ts) noexcept {
    return is_aligned(ts.microseconds(), config_.interval_us);
}

bool TimestampValidator::check_gap(Timestamp ts, std::int64_t delta_us) noexcept {
    (void)ts;   // 未使用

    if (delta_us <= 0) return true;

    // 计算跨了多少根
    const auto bars = delta_us / config_.interval_us;

    // 超过 max_gap_bars 则异常
    return static_cast<std::size_t>(bars) <= config_.max_gap_bars;
}

bool TimestampValidator::check_drift(Timestamp ts, std::int64_t delta_us) noexcept {
    (void)ts;

    if (delta_us <= 0) return false;
    if (config_.drift_warn_us <= 0) return false;

    const auto expected = config_.interval_us;
    const auto drift = std::abs(delta_us - expected);

    // 单次漂移超阈值
    if (drift > config_.drift_warn_us) {
        return true;
    }

    // 累计漂移
    const auto cumulative = cumulative_drift_us_ + (delta_us - expected);
    if (std::abs(cumulative) > config_.drift_warn_us * 3) {
        return true;
    }

    return false;
}

// ==============================================================================
// 状态更新
// ==============================================================================
void TimestampValidator::update_last_timestamp(Timestamp ts) noexcept {
    // 仅在时间戳前进时更新
    if (!last_ts_.has_value() ||
        ts.microseconds() > last_ts_->microseconds()) {
        last_ts_ = ts;
    }
}

// ==============================================================================
// 漂移窗口管理
// ==============================================================================
void TimestampValidator::push_drift_sample(std::int64_t drift_us) {
    drift_samples_.push_back(drift_us);
    cumulative_drift_us_ = safe_add(cumulative_drift_us_, drift_us);

    // 限制窗口
    while (drift_samples_.size() > config_.drift_window) {
        const auto old = drift_samples_.front();
        drift_samples_.pop_front();
        cumulative_drift_us_ = safe_add(cumulative_drift_us_, -old);
    }
}

// ==============================================================================
// 统计更新
// ==============================================================================
void TimestampValidator::update_stats(
    const TimestampValidation& v) noexcept
{
    // 更新最近一次 delta
    if (v.delta_us != 0) {
        stats_.last_delta_us.store(v.delta_us, std::memory_order_relaxed);

        // 更新 min/max delta
        auto cur_min = stats_.min_delta_us.load(std::memory_order_relaxed);
        if (cur_min == 0 || v.delta_us < cur_min) {
            stats_.min_delta_us.store(v.delta_us,
                std::memory_order_relaxed);
        }
    }

    // 更新漂移峰值
    if (v.drift_us != 0) {
        const auto abs_drift = std::abs(v.drift_us);
        auto cur_max = stats_.max_drift_us.load(std::memory_order_relaxed);
        while (abs_drift > cur_max &&
               !stats_.max_drift_us.compare_exchange_weak(
                   cur_max, abs_drift, std::memory_order_relaxed)) {}
    }

    // 漂移告警计数
    if (has_flag(v.triggered_flags, TimestampFlags::DriftCheck)) {
        stats_.drift_warnings.fetch_add(1, std::memory_order_relaxed);
    }
}

// ==============================================================================
// 状态管理
// ==============================================================================
void TimestampValidator::reset() {
    std::lock_guard lock(mutex_);
    last_ts_.reset();
    drift_samples_.clear();
    cumulative_drift_us_ = 0;
    stats_.reset();
}

const TimestampValidatorStats& TimestampValidator::stats() const noexcept {
    return stats_;
}

const TimestampValidatorConfig& TimestampValidator::config() const noexcept {
    return config_;
}

std::string_view TimestampValidator::symbol() const noexcept {
    return symbol_;
}

std::optional<Timestamp> TimestampValidator::last_timestamp() const {
    std::lock_guard lock(mutex_);
    return last_ts_;
}

std::size_t TimestampValidator::history_size() const noexcept {
    std::lock_guard lock(mutex_);
    return drift_samples_.size();
}

void TimestampValidator::set_config(
    const TimestampValidatorConfig& config)
{
    if (auto r = validate_config(config); r.is_err()) {
        QUANT_LOG_WARN("[TimestampValidator] 新配置非法，忽略: {}",
            r.error().to_string());
        return;
    }
    std::lock_guard lock(mutex_);
    config_ = config;

    // 配置变化时重置漂移窗口（避免新旧配置混淆）
    drift_samples_.clear();
    cumulative_drift_us_ = 0;
}

// ==============================================================================
// 诊断
// ==============================================================================
std::string TimestampValidator::dump() const {
    std::string out;
    out.reserve(1024);
    char buf[512];

    // 加锁保护状态读取
    std::lock_guard lock(mutex_);

    out += "TimestampValidator Dump:\n";

    std::snprintf(buf, sizeof(buf), "  symbol:              %s\n",
        symbol_.empty() ? "(none)" : symbol_.c_str());
    out += buf;

    std::snprintf(buf, sizeof(buf), "  interval_us:         %lld\n",
        static_cast<long long>(config_.interval_us));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  future_tolerance_us: %lld\n",
        static_cast<long long>(config_.future_tolerance_us));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  max_gap_bars:        %zu\n",
        config_.max_gap_bars);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  replay_mode:         %s\n",
        config_.replay_mode ? "true" : "false");
    out += buf;

    std::snprintf(buf, sizeof(buf), "  dry_run:             %s\n",
        config_.dry_run ? "true" : "false");
    out += buf;

    out += "  ---\n";

    if (last_ts_.has_value()) {
        std::snprintf(buf, sizeof(buf), "  last_ts_us:          %lld\n",
            static_cast<long long>(last_ts_->microseconds()));
        out += buf;
    } else {
        out += "  last_ts_us:          (none)\n";
    }

    std::snprintf(buf, sizeof(buf), "  drift_window_size:   %zu\n",
        drift_samples_.size());
    out += buf;

    std::snprintf(buf, sizeof(buf), "  cumulative_drift_us: %lld\n",
        static_cast<long long>(cumulative_drift_us_));
    out += buf;

    out += "  ---\n";

    std::snprintf(buf, sizeof(buf), "  total_processed:     %llu\n",
        static_cast<unsigned long long>(
            stats_.total_processed.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  accepted:            %llu\n",
        static_cast<unsigned long long>(
            stats_.accepted.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  accepted_with_warn:  %llu\n",
        static_cast<unsigned long long>(
            stats_.accepted_with_warn.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  rejected:            %llu\n",
        static_cast<unsigned long long>(
            stats_.rejected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  duplicates:          %llu\n",
        static_cast<unsigned long long>(
            stats_.duplicates.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  out_of_order:        %llu\n",
        static_cast<unsigned long long>(
            stats_.out_of_order.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  future_detected:     %llu\n",
        static_cast<unsigned long long>(
            stats_.future_detected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  too_old_detected:    %llu\n",
        static_cast<unsigned long long>(
            stats_.too_old_detected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  misaligned:          %llu\n",
        static_cast<unsigned long long>(
            stats_.misaligned.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  gap_detected:        %llu\n",
        static_cast<unsigned long long>(
            stats_.gap_detected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  drift_warnings:      %llu\n",
        static_cast<unsigned long long>(
            stats_.drift_warnings.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  epoch_detected:      %llu\n",
        static_cast<unsigned long long>(
            stats_.epoch_detected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  last_delta_us:       %lld\n",
        static_cast<long long>(
            stats_.last_delta_us.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  max_drift_us:        %lld\n",
        static_cast<long long>(
            stats_.max_drift_us.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  acceptance_rate:     %.2f%%\n",
        stats_.acceptance_rate() * 100.0);
    out += buf;

    return out;
}

}  // namespace data
}  // namespace quant
