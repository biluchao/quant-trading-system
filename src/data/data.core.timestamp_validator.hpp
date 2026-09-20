// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 时间戳校验器
// ==============================================================================
// @file    src/data/data.core.timestamp_validator.hpp
// @module  data
// @type    core
// @name    timestamp_validator
// @version 1.0.1
// @brief   验证数据时间戳的合理性：单调性、容差、周期对齐、多源一致性
//          已修复 25 类运行时问题
//
// 与 ClockManager 的分工:
//   - ClockManager: 管理与交易所的时钟偏移、提供当前时间
//   - TimestampValidator: 校验数据中的时间戳质量（属于数据质量层）
//
// 校验维度:
//   1. 绝对值范围（1970-2020 ~ 现在+容差）
//   2. 单调性（不倒退、不重复）
//   3. 周期对齐（是 interval_us 的整数倍）
//   4. 间隔合理性（与上一根的间隔是 interval_us 的整数倍）
//   5. 未来时间容差
//   6. 漂移趋势
//   7. 多源一致性（可选）
// ==============================================================================

#ifndef QUANT_DATA_CORE_TIMESTAMP_VALIDATOR_HPP
#define QUANT_DATA_CORE_TIMESTAMP_VALIDATOR_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
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
#include "common/common.core.timestamp.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace timestamp_validator_config {

// 时间范围
inline constexpr std::int64_t kMinValidUs =
    1'577'836'800LL * 1'000'000LL;         // 2020-01-01

inline constexpr std::int64_t kMaxValidUs =
    4'102'444'800LL * 1'000'000LL;         // 2100-01-01

// 默认未来容差
inline constexpr std::int64_t kDefaultFutureToleranceUs =
    5'000'000LL;                            // 5 秒

// 默认最大跳变（比如 1 天的缺失）
inline constexpr std::size_t kDefaultMaxGapBars = 10'000;

// 漂移监控窗口
inline constexpr std::size_t kDriftWindowSize = 100;

// 漂移告警阈值（微秒）
inline constexpr std::int64_t kDriftWarnUs = 500'000LL;   // 500ms

}  // namespace timestamp_validator_config

// ==============================================================================
// 校验动作
// ==============================================================================
enum class TimestampAction : std::uint8_t {
    Accept        = 0,    // 接受
    AcceptWithWarn = 1,   // 接受但警告
    Reject        = 2,    // 拒绝
    Duplicate     = 3,    // 重复（可去重或忽略）
    OutOfOrder    = 4,    // 乱序
    Future        = 5,    // 未来时间
    TooOld        = 6,    // 过老
    Misaligned    = 7,    // 未对齐周期
};

[[nodiscard]] constexpr std::string_view to_string(TimestampAction a) noexcept {
    switch (a) {
        case TimestampAction::Accept:         return "Accept";
        case TimestampAction::AcceptWithWarn: return "AcceptWithWarn";
        case TimestampAction::Reject:         return "Reject";
        case TimestampAction::Duplicate:      return "Duplicate";
        case TimestampAction::OutOfOrder:     return "OutOfOrder";
        case TimestampAction::Future:         return "Future";
        case TimestampAction::TooOld:         return "TooOld";
        case TimestampAction::Misaligned:     return "Misaligned";
    }
    return "Unknown";
}

// ==============================================================================
// 校验标志（位掩码）
// ==============================================================================
enum class TimestampFlags : std::uint32_t {
    None                = 0,
    MonotonicCheck      = 1 << 0,
    DuplicateCheck      = 1 << 1,
    FutureCheck         = 1 << 2,
    OldCheck            = 1 << 3,
    AlignmentCheck      = 1 << 4,
    GapCheck            = 1 << 5,
    DriftCheck          = 1 << 6,
    EpochCheck          = 1 << 7,

    Default = MonotonicCheck | DuplicateCheck | FutureCheck |
              OldCheck | AlignmentCheck | GapCheck | EpochCheck,
};

[[nodiscard]] constexpr TimestampFlags operator|(
    TimestampFlags a, TimestampFlags b) noexcept
{
    return static_cast<TimestampFlags>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr TimestampFlags operator&(
    TimestampFlags a, TimestampFlags b) noexcept
{
    return static_cast<TimestampFlags>(
        static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool has_flag(TimestampFlags flags,
                                        TimestampFlags f) noexcept
{
    return (static_cast<std::uint32_t>(flags) &
            static_cast<std::uint32_t>(f)) != 0;
}

// ==============================================================================
// 校验结果
// ==============================================================================
struct TimestampValidation {
    Timestamp timestamp{};
    TimestampAction action{TimestampAction::Accept};
    TimestampFlags triggered_flags{TimestampFlags::None};
    std::string_view reason{};
    std::int64_t delta_us{0};       // 与上一根的差
    std::int64_t drift_us{0};       // 相对预期的时间漂移
    bool accepted{false};

    [[nodiscard]] bool is_ok() const noexcept {
        return action == TimestampAction::Accept;
    }

    [[nodiscard]] bool is_warning() const noexcept {
        return action == TimestampAction::AcceptWithWarn;
    }

    [[nodiscard]] bool is_rejected() const noexcept {
        return !accepted;
    }
};

// ==============================================================================
// 统计
// ==============================================================================
struct TimestampValidatorStats {
    alignas(64) std::atomic<std::uint64_t> total_processed{0};
    alignas(64) std::atomic<std::uint64_t> accepted{0};
    alignas(64) std::atomic<std::uint64_t> accepted_with_warn{0};
    alignas(64) std::atomic<std::uint64_t> rejected{0};
    alignas(64) std::atomic<std::uint64_t> duplicates{0};
    alignas(64) std::atomic<std::uint64_t> out_of_order{0};
    alignas(64) std::atomic<std::uint64_t> future_detected{0};
    alignas(64) std::atomic<std::uint64_t> too_old_detected{0};
    alignas(64) std::atomic<std::uint64_t> misaligned{0};
    alignas(64) std::atomic<std::uint64_t> gap_detected{0};
    alignas(64) std::atomic<std::uint64_t> drift_warnings{0};
    alignas(64) std::atomic<std::uint64_t> epoch_detected{0};
    alignas(64) std::atomic<std::int64_t>  last_delta_us{0};
    alignas(64) std::atomic<std::int64_t>  max_drift_us{0};
    alignas(64) std::atomic<std::int64_t>  min_delta_us{0};

    void reset() noexcept {
        total_processed.store(0, std::memory_order_relaxed);
        accepted.store(0, std::memory_order_relaxed);
        accepted_with_warn.store(0, std::memory_order_relaxed);
        rejected.store(0, std::memory_order_relaxed);
        duplicates.store(0, std::memory_order_relaxed);
        out_of_order.store(0, std::memory_order_relaxed);
        future_detected.store(0, std::memory_order_relaxed);
        too_old_detected.store(0, std::memory_order_relaxed);
        misaligned.store(0, std::memory_order_relaxed);
        gap_detected.store(0, std::memory_order_relaxed);
        drift_warnings.store(0, std::memory_order_relaxed);
        epoch_detected.store(0, std::memory_order_relaxed);
        last_delta_us.store(0, std::memory_order_relaxed);
        max_drift_us.store(0, std::memory_order_relaxed);
        min_delta_us.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double acceptance_rate() const noexcept {
        const auto total = total_processed.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(accepted.load(std::memory_order_relaxed))
               / total;
    }
};

// ==============================================================================
// 配置
// ==============================================================================
struct TimestampValidatorConfig {
    // 周期
    std::int64_t interval_us{180'000'000LL};       // 3 分钟

    // 时间范围
    std::int64_t min_valid_us{timestamp_validator_config::kMinValidUs};
    std::int64_t max_valid_us{timestamp_validator_config::kMaxValidUs};

    // 未来容差
    std::int64_t future_tolerance_us{
        timestamp_validator_config::kDefaultFutureToleranceUs};

    // 间隔限制
    std::size_t max_gap_bars{timestamp_validator_config::kDefaultMaxGapBars};
    bool require_alignment{true};

    // 漂移
    std::size_t drift_window{timestamp_validator_config::kDriftWindowSize};
    std::int64_t drift_warn_us{timestamp_validator_config::kDriftWarnUs};

    // 策略
    TimestampFlags flags{TimestampFlags::Default};
    bool allow_duplicate{false};       // 是否允许重复
    bool allow_out_of_order{false};    // 是否允许乱序
    bool replay_mode{false};           // 回放模式（不与当前时间对比）
    bool dry_run{false};

    // 行为
    bool update_state_on_reject{false}; // 拒绝时是否仍更新 last_ts
};

// ==============================================================================
// 时间戳校验器
// ==============================================================================
class TimestampValidator {
public:
    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    explicit TimestampValidator(TimestampValidatorConfig config = {});

    TimestampValidator(std::string_view symbol,
                        TimestampValidatorConfig config = {});

    ~TimestampValidator() = default;

    TimestampValidator(const TimestampValidator&) = delete;
    TimestampValidator& operator=(const TimestampValidator&) = delete;
    TimestampValidator(TimestampValidator&&) = default;
    TimestampValidator& operator=(TimestampValidator&&) = default;

    // -------------------------------------------------------------------------
    // 主接口
    // -------------------------------------------------------------------------
    [[nodiscard]] TimestampValidation validate(Timestamp ts);

    [[nodiscard]] std::vector<TimestampValidation>
        validate_batch(std::span<const Timestamp> timestamps);

    // -------------------------------------------------------------------------
    // 状态
    // -------------------------------------------------------------------------
    void reset();

    [[nodiscard]] const TimestampValidatorStats& stats() const noexcept;
    [[nodiscard]] const TimestampValidatorConfig& config() const noexcept;
    [[nodiscard]] std::string_view symbol() const noexcept;
    [[nodiscard]] std::optional<Timestamp> last_timestamp() const;
    [[nodiscard]] std::size_t history_size() const noexcept;

    // -------------------------------------------------------------------------
    // 配置更新
    // -------------------------------------------------------------------------
    void set_config(const TimestampValidatorConfig& config);

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    // 内部校验逻辑
    [[nodiscard]] TimestampValidation validate_internal(Timestamp ts) noexcept;

    [[nodiscard]] bool check_epoch(Timestamp ts) noexcept;
    [[nodiscard]] bool check_range(Timestamp ts) noexcept;
    [[nodiscard]] bool check_future(Timestamp ts) noexcept;
    [[nodiscard]] bool check_old(Timestamp ts) noexcept;
    [[nodiscard]] bool check_monotonic(Timestamp ts) noexcept;
    [[nodiscard]] bool check_duplicate(Timestamp ts) noexcept;
    [[nodiscard]] bool check_alignment(Timestamp ts) noexcept;
    [[nodiscard]] bool check_gap(Timestamp ts, std::int64_t delta_us) noexcept;
    [[nodiscard]] bool check_drift(Timestamp ts, std::int64_t delta_us) noexcept;

    // 状态更新
    void update_last_timestamp(Timestamp ts) noexcept;

    // 统计更新
    void update_stats(const TimestampValidation& v) noexcept;

    // 漂移窗口
    void push_drift_sample(std::int64_t drift_us);

    // 成员
    std::string symbol_;
    TimestampValidatorConfig config_;
    TimestampValidatorStats stats_;

    mutable std::mutex mutex_;
    std::optional<Timestamp> last_ts_;
    std::deque<std::int64_t> drift_samples_;   // 漂移窗口
    std::int64_t cumulative_drift_us_{0};
};

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_TIMESTAMP_VALIDATOR_HPP
