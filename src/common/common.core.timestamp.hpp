// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 时间戳类型
// ==============================================================================
// @file    src/common/common.core.timestamp.hpp
// @module  common
// @type    core
// @name    timestamp
// @version 1.0.1
// @brief   统一时间戳（微秒精度，UTC），支持墙上时钟与单调时钟
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 所有时间戳为 int64_t 微秒，UTC
//   - 区分墙上时钟（now）和单调时钟（steady_now）
//   - 支持交易所时间偏移（币安签名要求 ±1s）
//   - 检测时钟回拨，保证单调性
//   - 提供 ISO 8601 序列化
//   - 线程安全
//   - 禁止 double 存时间戳
// ==============================================================================

#ifndef QUANT_COMMON_CORE_TIMESTAMP_HPP
#define QUANT_COMMON_CORE_TIMESTAMP_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <ctime>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>

// ==============================================================================
// 命名空间
// ==============================================================================
namespace quant {

// ==============================================================================
// 时间单位常量
// ==============================================================================
namespace units {

inline constexpr int64_t kNanosecondsPerMicrosecond = 1'000LL;
inline constexpr int64_t kMicrosecondsPerMillisecond = 1'000LL;
inline constexpr int64_t kMicrosecondsPerSecond      = 1'000'000LL;
inline constexpr int64_t kMicrosecondsPerMinute      = 60LL * kMicrosecondsPerSecond;
inline constexpr int64_t kMicrosecondsPerHour        = 60LL * kMicrosecondsPerMinute;
inline constexpr int64_t kMicrosecondsPerDay         = 24LL * kMicrosecondsPerHour;

// 转换辅助
[[nodiscard]] constexpr int64_t seconds_to_us(int64_t s) noexcept {
    return s * kMicrosecondsPerSecond;
}
[[nodiscard]] constexpr int64_t ms_to_us(int64_t ms) noexcept {
    return ms * kMicrosecondsPerMillisecond;
}
[[nodiscard]] constexpr int64_t us_to_ms(int64_t us) noexcept {
    return us / kMicrosecondsPerMillisecond;   // 截断
}
[[nodiscard]] constexpr int64_t us_to_seconds(int64_t us) noexcept {
    return us / kMicrosecondsPerSecond;         // 截断
}
[[nodiscard]] constexpr int64_t us_to_ms_round(int64_t us) noexcept {
    return (us + kMicrosecondsPerMillisecond / 2) / kMicrosecondsPerMillisecond;
}

}  // namespace units

// ==============================================================================
// Timestamp 类
// ==============================================================================
class Timestamp {
public:
    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    constexpr Timestamp() noexcept : us_{0} {}
    constexpr explicit Timestamp(int64_t microseconds) noexcept : us_{microseconds} {}

    // -------------------------------------------------------------------------
    // 工厂函数：墙上时钟（UTC）
    // -------------------------------------------------------------------------
    // 注意：system_clock 非单调，NTP 同步可能回拨。测量间隔请用 steady_now()。
    [[nodiscard]] static Timestamp now() noexcept {
        const auto now = std::chrono::system_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        return Timestamp{static_cast<int64_t>(us)};
    }

    // -------------------------------------------------------------------------
    // 工厂函数：单调时钟（测量间隔）
    // -------------------------------------------------------------------------
    // 单调时钟保证不会回拨，但无绝对意义，仅用于测量时间差。
    [[nodiscard]] static Timestamp steady_now() noexcept {
        const auto now = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        return Timestamp{static_cast<int64_t>(us)};
    }

    // -------------------------------------------------------------------------
    // 工厂函数：从各精度构造
    // -------------------------------------------------------------------------
    [[nodiscard]] static constexpr Timestamp from_seconds(int64_t s) noexcept {
        return Timestamp{units::seconds_to_us(s)};
    }
    [[nodiscard]] static constexpr Timestamp from_milliseconds(int64_t ms) noexcept {
        return Timestamp{units::ms_to_us(ms)};
    }
    [[nodiscard]] static constexpr Timestamp from_microseconds(int64_t us) noexcept {
        return Timestamp{us};
    }

    // 从 std::chrono::time_point 构造
    template <typename Clock, typename Duration>
    [[nodiscard]] static Timestamp from_time_point(
        const std::chrono::time_point<Clock, Duration>& tp) noexcept {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            tp.time_since_epoch()).count();
        return Timestamp{static_cast<int64_t>(us)};
    }

    // -------------------------------------------------------------------------
    // 访问器
    // -------------------------------------------------------------------------
    [[nodiscard]] constexpr int64_t microseconds() const noexcept { return us_; }
    [[nodiscard]] constexpr int64_t milliseconds() const noexcept {
        return units::us_to_ms(us_);
    }
    [[nodiscard]] constexpr int64_t seconds() const noexcept {
        return units::us_to_seconds(us_);
    }

    // 微秒的余数（用于格式化）
    [[nodiscard]] constexpr int32_t sub_millisecond_us() const noexcept {
        return static_cast<int32_t>(us_ % units::kMicrosecondsPerMillisecond);
    }

    // -------------------------------------------------------------------------
    // 有效性
    // -------------------------------------------------------------------------
    // Unix 纪元起点（1970-01-01）
    [[nodiscard]] constexpr bool is_epoch() const noexcept { return us_ == 0; }

    // 最小合法时间：2020-01-01T00:00:00Z = 1577836800 秒
    [[nodiscard]] constexpr bool is_synchronized() const noexcept {
        constexpr int64_t kMinSynchronized = 1'577'836'800LL * units::kMicrosecondsPerSecond;
        return us_ >= kMinSynchronized;
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return us_ > 0;
    }

    // -------------------------------------------------------------------------
    // 算术
    // -------------------------------------------------------------------------
    constexpr Timestamp operator+(int64_t us) const noexcept {
        return Timestamp{us_ + us};
    }
    constexpr Timestamp operator-(int64_t us) const noexcept {
        return Timestamp{us_ - us};
    }
    // 仅同类时钟的时间戳相减有意义
    constexpr int64_t operator-(const Timestamp& other) const noexcept {
        return us_ - other.us_;
    }

    constexpr Timestamp& operator+=(int64_t us) noexcept {
        us_ += us;
        return *this;
    }
    constexpr Timestamp& operator-=(int64_t us) noexcept {
        us_ -= us;
        return *this;
    }

    // 饱和加法（防止溢出）
    [[nodiscard]] constexpr Timestamp saturating_add(int64_t us) const noexcept {
        constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
        if (us > 0 && us_ > kMax - us) return Timestamp{kMax};
        if (us < 0 && us_ < std::numeric_limits<int64_t>::min() - us)
            return Timestamp{std::numeric_limits<int64_t>::min()};
        return Timestamp{us_ + us};
    }

    // -------------------------------------------------------------------------
    // 比较
    // -------------------------------------------------------------------------
    constexpr auto operator<=>(const Timestamp&) const noexcept = default;
    constexpr bool operator==(const Timestamp&) const noexcept = default;

    // -------------------------------------------------------------------------
    // 与 std::chrono 互操作
    // -------------------------------------------------------------------------
    [[nodiscard]] std::chrono::system_clock::time_point to_time_point() const noexcept {
        return std::chrono::system_clock::time_point{
            std::chrono::microseconds{us_}};
    }

    // -------------------------------------------------------------------------
    // 序列化
    // -------------------------------------------------------------------------
    // ISO 8601 UTC 格式： "2026-09-18T10:30:00.123456Z"
    [[nodiscard]] std::string to_iso8601() const;

    // ISO 8601 带时区偏移： "2026-09-18T18:30:00.123456+08:00"
    [[nodiscard]] std::string to_iso8601(int32_t tz_offset_minutes) const;

    // Unix 秒（截断）
    [[nodiscard]] std::string to_unix_string() const;

    // 从 ISO 8601 解析（要求显式时区）
    [[nodiscard]] static std::optional<Timestamp> from_iso8601(std::string_view s) noexcept;

    // -------------------------------------------------------------------------
    // 边界值
    // -------------------------------------------------------------------------
    [[nodiscard]] static constexpr Timestamp min() noexcept {
        return Timestamp{std::numeric_limits<int64_t>::min()};
    }
    [[nodiscard]] static constexpr Timestamp max() noexcept {
        return Timestamp{std::numeric_limits<int64_t>::max()};
    }
    [[nodiscard]] static constexpr Timestamp zero() noexcept {
        return Timestamp{0};
    }

private:
    int64_t us_;
};

// ==============================================================================
// 时钟管理器（线程安全）
// ==============================================================================
// 负责：
//   1. 维护与交易所的时间偏移
//   2. 检测本地时钟回拨
//   3. 提供统一的 "当前时间"
class ClockManager {
public:
    // -------------------------------------------------------------------------
    // 单例访问
    // -------------------------------------------------------------------------
    [[nodiscard]] static ClockManager& instance() noexcept;

    // -------------------------------------------------------------------------
    // 交易所时间偏移
    // -------------------------------------------------------------------------
    // 由 BinanceClient 定期调用，传入交易所返回的时间戳
    void update_exchange_offset(Timestamp exchange_time) noexcept;

    // 返回最近的交易所时间偏移（微秒）
    [[nodiscard]] int64_t exchange_offset_us() const noexcept;

    // 基于交易所偏移的当前时间
    [[nodiscard]] Timestamp exchange_now() const noexcept;

    // -------------------------------------------------------------------------
    // 时钟回拨检测
    // -------------------------------------------------------------------------
    // 返回自启动以来检测到的回拨次数
    [[nodiscard]] uint64_t clock_backward_count() const noexcept;

    // 返回最大回拨幅度（微秒）
    [[nodiscard]] int64_t max_backward_us() const noexcept;

    // 检查系统时钟是否同步（相对交易所）
    [[nodiscard]] bool is_clock_healthy(int64_t threshold_us = 1'000'000LL) const noexcept;

    // -------------------------------------------------------------------------
    // 单调时间（带保护）
    // -------------------------------------------------------------------------
    // 返回保证单调递增的时间戳
    [[nodiscard]] Timestamp monotonic_now() noexcept;

private:
    ClockManager() noexcept;
    ~ClockManager() = default;
    ClockManager(const ClockManager&) = delete;
    ClockManager& operator=(const ClockManager&) = delete;

    std::atomic<int64_t> exchange_offset_us_{0};
    std::atomic<int64_t> last_wall_us_{0};
    std::atomic<int64_t> last_monotonic_us_{0};
    std::atomic<uint64_t> backward_count_{0};
    std::atomic<int64_t> max_backward_us_{0};
};

// ==============================================================================
// 时间戳区间
// ==============================================================================
struct TimeRange {
    Timestamp start{};
    Timestamp end{};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return start.is_valid() && end.is_valid() && start <= end;
    }

    [[nodiscard]] constexpr int64_t duration_us() const noexcept {
        return end - start;
    }

    [[nodiscard]] constexpr bool contains(const Timestamp& t) const noexcept {
        return t >= start && t <= end;
    }

    [[nodiscard]] constexpr bool overlaps(const TimeRange& other) const noexcept {
        return start <= other.end && other.start <= end;
    }
};

// ==============================================================================
// 哈希支持
// ==============================================================================
struct TimestampHash {
    [[nodiscard]] std::size_t operator()(const Timestamp& t) const noexcept {
        return std::hash<int64_t>{}(t.microseconds());
    }
};

// ==============================================================================
// 流输出
// ==============================================================================
inline std::ostream& operator<<(std::ostream& os, const Timestamp& t) {
    os << t.to_iso8601();
    return os;
}

// ==============================================================================
// 便捷函数
// ==============================================================================
[[nodiscard]] inline Timestamp now() noexcept {
    return Timestamp::now();
}

[[nodiscard]] inline Timestamp steady_now() noexcept {
    return Timestamp::steady_now();
}

// 测量代码块执行时间
class ScopedTimer {
public:
    ScopedTimer() noexcept : start_{Timestamp::steady_now()} {}

    [[nodiscard]] int64_t elapsed_us() const noexcept {
        return Timestamp::steady_now() - start_;
    }

    [[nodiscard]] double elapsed_ms() const noexcept {
        return static_cast<double>(elapsed_us()) / 1000.0;
    }

private:
    Timestamp start_;
};

}  // namespace quant

// ==============================================================================
// std::hash 特化
// ==============================================================================
namespace std {

template <>
struct hash<quant::Timestamp> {
    size_t operator()(const quant::Timestamp& t) const noexcept {
        return hash<int64_t>{}(t.microseconds());
    }
};

}  // namespace std

// ==============================================================================
// 编译期校验
// ==============================================================================
static_assert(sizeof(quant::Timestamp) == 8, "Timestamp 必须为 8 字节");
static_assert(std::is_trivially_copyable_v<quant::Timestamp>,
    "Timestamp 必须可平凡复制");
static_assert(std::is_nothrow_move_constructible_v<quant::Timestamp>,
    "Timestamp 移动构造必须 noexcept");

#endif  // QUANT_COMMON_CORE_TIMESTAMP_HPP
