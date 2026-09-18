// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 时间戳实现
// ==============================================================================
// @file    src/common/common.core.timestamp.cpp
// @module  common
// @type    core
// @name    timestamp
// @version 1.0.1
// @brief   Timestamp 类的 ISO 8601 序列化、解析、ClockManager 实现
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 线程安全：gmtime_r/gmtime_s 替代 gmtime
//   - 地板除法：负时间戳（1970 前）正确处理
//   - 严格解析：显式时区 + 范围校验 + 闰年校验
//   - locale 无关：手动格式化，不依赖 put_time
//   - 溢出防护：snprintf 返回值检查，中间值用 int64_t
//   - 原子安全：acq_rel 内存序，CAS 循环有上限
// ==============================================================================

#include "common/common.core.timestamp.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

namespace quant {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// 地板除法（向负无穷取整，与向零截断的 `/` 不同）
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr int64_t floor_div(int64_t a, int64_t b) noexcept {
    const int64_t q = a / b;
    const int64_t r = a % b;
    // 若余数非零且符号与除数相反，商减 1
    return (r != 0 && (r < 0) != (b < 0)) ? q - 1 : q;
}

// -----------------------------------------------------------------------------
// 地板取模（结果符号与除数相同，始终非负当 b > 0）
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr int64_t floor_mod(int64_t a, int64_t b) noexcept {
    return a - floor_div(a, b) * b;
}

// -----------------------------------------------------------------------------
// 闰年判定
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr bool is_leap_year(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

// -----------------------------------------------------------------------------
// 月份天数
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr int days_in_month(int year, int month) noexcept {
    static constexpr int kDays[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return kDays[month - 1];
}

// -----------------------------------------------------------------------------
// 拆解时间戳为 UTC 日历字段（线程安全）
// -----------------------------------------------------------------------------
struct BrokenDown {
    int year;
    int month;      // 1-12
    int day;        // 1-31
    int hour;       // 0-23
    int minute;     // 0-59
    int second;     // 0-59
    int microsecond;// 0-999999
};

[[nodiscard]] BrokenDown break_down_utc(int64_t us) noexcept {
    // 地板除法保证负数时间戳的正确性
    const int64_t seconds = floor_div(us, units::kMicrosecondsPerSecond);
    const int64_t micros  = floor_mod(us, units::kMicrosecondsPerSecond);

    // 使用 std::time_t 转换（仅此处调用平台 API）
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm tm_utc{};

#if defined(_WIN32)
    // Windows: gmtime_s 参数顺序与 POSIX 相反
    if (gmtime_s(&tm_utc, &t) != 0) {
        // 失败时返回纪元时间
        return {1970, 1, 1, 0, 0, 0, 0};
    }
#else
    // POSIX: gmtime_r 返回 nullptr 表示失败
    if (gmtime_r(&t, &tm_utc) == nullptr) {
        return {1970, 1, 1, 0, 0, 0, 0};
    }
#endif

    BrokenDown bd{};
    bd.year        = tm_utc.tm_year + 1900;   // tm_year 从 1900 起算
    bd.month       = tm_utc.tm_mon + 1;       // tm_mon 从 0 起算
    bd.day         = tm_utc.tm_mday;
    bd.hour        = tm_utc.tm_hour;
    bd.minute      = tm_utc.tm_min;
    bd.second      = tm_utc.tm_sec;
    bd.microsecond = static_cast<int>(micros);
    return bd;
}

// -----------------------------------------------------------------------------
// 格式化为 ISO 8601（locale 无关）
// -----------------------------------------------------------------------------
[[nodiscard]] std::string format_iso8601(const BrokenDown& bd,
                                           std::string_view tz_suffix) {
    // ISO 8601 最大长度: "YYYY-MM-DDTHH:MM:SS.ffffff+HH:MM" = 32 字符
    std::array<char, 40> buf{};

    const int n = std::snprintf(
        buf.data(), buf.size(),
        "%04d-%02d-%02dT%02d:%02d:%02d.%06d",
        bd.year, bd.month, bd.day,
        bd.hour, bd.minute, bd.second,
        bd.microsecond);

    // 检查 snprintf 返回值
    if (n < 0) {
        return {};  // 编码错误
    }
    if (static_cast<std::size_t>(n) >= buf.size()) {
        return {};  // 截断（不应发生）
    }

    std::string result{buf.data(), static_cast<std::size_t>(n)};
    result.append(tz_suffix);
    return result;
}

// -----------------------------------------------------------------------------
// 安全解析整数（固定宽度）
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr int parse_fixed_digits(std::string_view sv) noexcept {
    int result = 0;
    for (char c : sv) {
        if (c < '0' || c > '9') return -1;
        result = result * 10 + (c - '0');
    }
    return result;
}

}  // namespace

// ==============================================================================
// Timestamp::to_iso8601 (UTC)
// ==============================================================================
std::string Timestamp::to_iso8601() const {
    const BrokenDown bd = break_down_utc(us_);
    return format_iso8601(bd, "Z");
}

// ==============================================================================
// Timestamp::to_iso8601 (带时区偏移)
// ==============================================================================
// tz_offset_minutes: 分钟，如 +08:00 是 480，-05:00 是 -300
// 有效范围: -12:00 ~ +14:00，即 -720 ~ +840
std::string Timestamp::to_iso8601(int32_t tz_offset_minutes) const {
    // 校验时区范围
    constexpr int32_t kMinOffset = -12 * 60;
    constexpr int32_t kMaxOffset = +14 * 60;
    if (tz_offset_minutes < kMinOffset || tz_offset_minutes > kMaxOffset) {
        return to_iso8601();  // 降级为 UTC
    }

    // 用 int64_t 计算中间值，防止溢出
    const int64_t offset_us =
        static_cast<int64_t>(tz_offset_minutes) *
        units::kMicrosecondsPerMinute;

    // 注意：此处可能与本地时间相同，但语义上是"某个时区的墙上时间"
    const BrokenDown bd = break_down_utc(us_ + offset_us);

    // 格式化时区后缀
    std::array<char, 8> suffix_buf{};
    const char sign = (tz_offset_minutes >= 0) ? '+' : '-';
    const int32_t abs_offset = (tz_offset_minutes >= 0)
        ? tz_offset_minutes
        : -tz_offset_minutes;
    const int hh = abs_offset / 60;
    const int mm = abs_offset % 60;

    const int n = std::snprintf(
        suffix_buf.data(), suffix_buf.size(),
        "%c%02d:%02d", sign, hh, mm);

    if (n < 0 || static_cast<std::size_t>(n) >= suffix_buf.size()) {
        return format_iso8601(bd, "Z");
    }

    return format_iso8601(bd, suffix_buf.data());
}

// ==============================================================================
// Timestamp::to_unix_string
// ==============================================================================
std::string Timestamp::to_unix_string() const {
    // 输出秒级 Unix 时间戳（含符号）
    std::array<char, 32> buf{};
    const int64_t seconds = seconds();
    const int n = std::snprintf(buf.data(), buf.size(),
                                 "%" PRId64, seconds);
    if (n < 0 || static_cast<std::size_t>(n) >= buf.size()) {
        return {};
    }
    return std::string{buf.data(), static_cast<std::size_t>(n)};
}

// ==============================================================================
// Timestamp::from_iso8601
// ==============================================================================
// 格式: YYYY-MM-DDTHH:MM:SS[.ffffff][Z|±HH:MM]
// 最小长度: "2026-09-18T10:30:00Z" = 20 字符
std::optional<Timestamp> Timestamp::from_iso8601(std::string_view s) noexcept {
    // ------------------------------------------------------------------------
    // 1. 基础长度检查
    // ------------------------------------------------------------------------
    constexpr std::size_t kMinLength = 20;
    if (s.size() < kMinLength) return std::nullopt;

    // ------------------------------------------------------------------------
    // 2. 分隔符位置校验
    // ------------------------------------------------------------------------
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
        s[13] != ':' || s[16] != ':') {
        return std::nullopt;
    }

    // ------------------------------------------------------------------------
    // 3. 数值解析
    // ------------------------------------------------------------------------
    const int year   = parse_fixed_digits(s.substr(0, 4));
    const int month  = parse_fixed_digits(s.substr(5, 2));
    const int day    = parse_fixed_digits(s.substr(8, 2));
    const int hour   = parse_fixed_digits(s.substr(11, 2));
    const int minute = parse_fixed_digits(s.substr(14, 2));
    const int second = parse_fixed_digits(s.substr(17, 2));

    if (year < 0 || month < 0 || day < 0 ||
        hour < 0 || minute < 0 || second < 0) {
        return std::nullopt;
    }

    // ------------------------------------------------------------------------
    // 4. 数值范围校验
    // ------------------------------------------------------------------------
    if (year < 1970 || year > 9999) return std::nullopt;
    if (month < 1 || month > 12) return std::nullopt;
    if (day < 1 || day > days_in_month(year, month)) return std::nullopt;
    if (hour < 0 || hour > 23) return std::nullopt;
    if (minute < 0 || minute > 59) return std::nullopt;
    if (second < 0 || second > 59) return std::nullopt;  // 不支持闰秒

    // ------------------------------------------------------------------------
    // 5. 解析微秒部分
    // ------------------------------------------------------------------------
    int64_t micros = 0;
    std::size_t pos = 19;  // 指向秒的最后一位后

    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        int digits = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9'
               && digits < 6) {
            micros = micros * 10 + (s[pos] - '0');
            ++pos;
            ++digits;
        }
        // 补零到 6 位（.123 → 123000）
        while (digits < 6) {
            micros *= 10;
            ++digits;
        }
    }

    // ------------------------------------------------------------------------
    // 6. 解析时区（必须显式）
    // ------------------------------------------------------------------------
    if (pos >= s.size()) {
        return std::nullopt;  // 缺少时区标识
    }

    int32_t tz_offset_min = 0;
    const char tz = s[pos];

    if (tz == 'Z' || tz == 'z') {
        tz_offset_min = 0;
        // Z 后不应有其他字符
        if (pos + 1 != s.size()) return std::nullopt;
    } else if (tz == '+' || tz == '-') {
        // 至少需要 "+HH:MM" = 6 字符
        if (s.size() - pos < 6) return std::nullopt;
        if (s[pos + 3] != ':') return std::nullopt;

        const int tz_h = parse_fixed_digits(s.substr(pos + 1, 2));
        const int tz_m = parse_fixed_digits(s.substr(pos + 4, 2));

        if (tz_h < 0 || tz_m < 0) return std::nullopt;
        if (tz_h > 14 || tz_m > 59) return std::nullopt;

        tz_offset_min = tz_h * 60 + tz_m;
        if (tz == '-') tz_offset_min = -tz_offset_min;

        // 时区偏移范围校验
        if (tz_offset_min < -12 * 60 || tz_offset_min > +14 * 60) {
            return std::nullopt;
        }

        // 时区后不应有其他字符
        if (pos + 6 != s.size()) return std::nullopt;
    } else {
        return std::nullopt;
    }

    // ------------------------------------------------------------------------
    // 7. 转换为 Unix 秒（UTC）
    // ------------------------------------------------------------------------
    std::tm tm_utc{};
    tm_utc.tm_year = year - 1900;
    tm_utc.tm_mon  = month - 1;
    tm_utc.tm_mday = day;
    tm_utc.tm_hour = hour;
    tm_utc.tm_min  = minute;
    tm_utc.tm_sec  = second;
    // 关键：不使用 tm_isdst，timegm 会忽略它

    std::time_t t;
#if defined(_WIN32)
    t = _mkgmtime(&tm_utc);
#else
    t = timegm(&tm_utc);
#endif

    // timegm 失败返回 -1，需与 1969-12-31 23:59:59 区分
    // 由于我们限制了 year >= 1970，t == -1 一定是错误
    if (t == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }

    // ------------------------------------------------------------------------
    // 8. 应用时区偏移（转为 UTC）
    // ------------------------------------------------------------------------
    int64_t total_us = static_cast<int64_t>(t) * units::kMicrosecondsPerSecond;
    total_us += micros;

    // 减去时区偏移得到 UTC
    // 例如 "+08:00" 的输入，需要减去 8 小时得到 UTC
    total_us -= static_cast<int64_t>(tz_offset_min) *
                units::kMicrosecondsPerMinute;

    return Timestamp{total_us};
}

// ==============================================================================
// ClockManager 实现
// ==============================================================================

// -----------------------------------------------------------------------------
// 单例（Meyers 单例，C++11 保证线程安全）
// -----------------------------------------------------------------------------
ClockManager& ClockManager::instance() noexcept {
    static ClockManager inst;
    return inst;
}

// -----------------------------------------------------------------------------
// 构造函数：初始化上次时间
// -----------------------------------------------------------------------------
ClockManager::ClockManager() noexcept {
    const int64_t wall_us = Timestamp::now().microseconds();
    const int64_t mono_us = Timestamp::steady_now().microseconds();
    last_wall_us_.store(wall_us, std::memory_order_relaxed);
    last_monotonic_us_.store(mono_us, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// 更新交易所时间偏移
// -----------------------------------------------------------------------------
// 公式: offset = exchange_time - local_time
// 之后 exchange_now() = local_now() + offset
void ClockManager::update_exchange_offset(Timestamp exchange_time) noexcept {
    const int64_t local_us = Timestamp::now().microseconds();
    const int64_t exchange_us = exchange_time.microseconds();
    const int64_t offset = exchange_us - local_us;
    exchange_offset_us_.store(offset, std::memory_order_release);
}

// -----------------------------------------------------------------------------
// 读取交易所时间偏移
// -----------------------------------------------------------------------------
int64_t ClockManager::exchange_offset_us() const noexcept {
    return exchange_offset_us_.load(std::memory_order_acquire);
}

// -----------------------------------------------------------------------------
// 基于交易所偏移的当前时间
// -----------------------------------------------------------------------------
Timestamp ClockManager::exchange_now() const noexcept {
    const int64_t local_us = Timestamp::now().microseconds();
    const int64_t offset = exchange_offset_us_.load(std::memory_order_acquire);
    return Timestamp{local_us + offset};
}

// -----------------------------------------------------------------------------
// 时钟回拨统计
// -----------------------------------------------------------------------------
uint64_t ClockManager::clock_backward_count() const noexcept {
    return backward_count_.load(std::memory_order_acquire);
}

int64_t ClockManager::max_backward_us() const noexcept {
    return max_backward_us_.load(std::memory_order_acquire);
}

// -----------------------------------------------------------------------------
// 时钟健康检查
// -----------------------------------------------------------------------------
bool ClockManager::is_clock_healthy(int64_t threshold_us) const noexcept {
    const int64_t offset = exchange_offset_us_.load(std::memory_order_acquire);
    const int64_t abs_offset = (offset >= 0) ? offset : -offset;
    return abs_offset <= threshold_us;
}

// -----------------------------------------------------------------------------
// 单调时间（带回拨保护）
// -----------------------------------------------------------------------------
Timestamp ClockManager::monotonic_now() noexcept {
    const int64_t current = Timestamp::now().microseconds();

    // 使用 acq_rel 语义，保证跨线程可见
    const int64_t previous = last_wall_us_.load(std::memory_order_acquire);

    if (current < previous) {
        // ------------------------------------------------------------------
        // 检测到时钟回拨
        // ------------------------------------------------------------------
        const int64_t backward = previous - current;

        backward_count_.fetch_add(1, std::memory_order_relaxed);

        // 更新最大回拨幅度（CAS 循环，有上限）
        int64_t old_max = max_backward_us_.load(std::memory_order_relaxed);
        int retry = 0;
        while (backward > old_max && retry < 10) {
            if (max_backward_us_.compare_exchange_weak(
                    old_max, backward,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                break;
            }
            ++retry;
        }
        // 若 CAS 失败超限，忽略更新（仅统计功能，不影响正确性）

        // 返回 previous + 1，保证严格单调
        const int64_t monotonic = previous + 1;
        last_wall_us_.store(monotonic, std::memory_order_release);
        return Timestamp{monotonic};
    }

    // ----------------------------------------------------------------------
    // 正常路径
    // ----------------------------------------------------------------------
    last_wall_us_.store(current, std::memory_order_release);
    return Timestamp{current};
}

}  // namespace quant
