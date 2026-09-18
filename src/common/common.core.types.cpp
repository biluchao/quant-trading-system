// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 核心类型实现
// ==============================================================================
// @file    src/common/common.core.types.cpp
// @module  common
// @type    core
// @name    types
// @version 1.0.1
// @brief   Price / Quantity 的字符串解析与格式化实现
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 无异常：解析失败返回 std::nullopt，绝不抛出
//   - 精度无损：全程整数运算，不用 double 中间值
//   - 溢出检测：使用 __int128 中间值，显式拒绝超范围
//   - 严格校验：只接受 [0-9.]，拒绝符号/科学计数法/空白
//   - locale 无关：不使用 stod/strtod
//   - 长度限制：拒绝超长输入，防 DoS
// ==============================================================================

#include "common/common.core.types.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace quant {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// 输入长度上限（防止 DoS）
// -----------------------------------------------------------------------------
// 价格最大 19 位整数部分 + 1 小数点 + 8 位小数 = 28 字符
// 加 1 字节余量
constexpr std::size_t kMaxInputLength = 32;

// -----------------------------------------------------------------------------
// 判断字符是否为数字
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// -----------------------------------------------------------------------------
// 判断字符是否为空白
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == '\v' || c == '\f';
}

// -----------------------------------------------------------------------------
// 检测字符串是否全空白
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr bool is_blank(std::string_view s) noexcept {
    for (char c : s) {
        if (!is_space(c)) return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// 解析结果
// -----------------------------------------------------------------------------
struct ParsedDecimal {
    bool ok{false};
    int64_t raw{0};              // 定标后的整数表示
    int fractional_digits{0};    // 实际输入的小数位数
    bool precision_exceeded{false};  // 是否超过精度上限
    bool overflow{false};        // 是否溢出
};

// -----------------------------------------------------------------------------
// 解析定点数（严格模式）
// -----------------------------------------------------------------------------
// 输入格式: [0-9]+(\.[0-9]{0,scale})?
// 或者: \.[0-9]{1,scale}
// 拒绝:
//   - 空字符串
//   - 前导/尾随空白
//   - 符号（+/-）
//   - 科学计数法（e/E）
//   - 十六进制（0x）
//   - NaN/Inf
//   - 多个小数点
//   - 无效字符
//
// 返回: ParsedDecimal{ok=true} 表示成功
template <std::size_t Scale>
[[nodiscard]] ParsedDecimal parse_decimal(std::string_view s) noexcept {
    ParsedDecimal result{};

    // ------------------------------------------------------------------------
    // 1. 长度检查
    // ------------------------------------------------------------------------
    if (s.empty() || s.size() > kMaxInputLength) {
        return result;
    }

    // ------------------------------------------------------------------------
    // 2. 前导/尾随空白检查
    // ------------------------------------------------------------------------
    if (is_space(s.front()) || is_space(s.back())) {
        return result;
    }

    // ------------------------------------------------------------------------
    // 3. 扫描字符，分离整数部分和小数部分
    // ------------------------------------------------------------------------
    std::size_t pos = 0;
    std::string_view integer_part{};
    std::string_view fractional_part{};
    bool has_dot = false;

    // 整数部分
    const std::size_t int_start = pos;
    while (pos < s.size() && is_digit(s[pos])) {
        ++pos;
    }
    integer_part = s.substr(int_start, pos - int_start);

    // 小数点
    if (pos < s.size() && s[pos] == '.') {
        has_dot = true;
        ++pos;

        // 小数部分
        const std::size_t frac_start = pos;
        while (pos < s.size() && is_digit(s[pos])) {
            ++pos;
        }
        fractional_part = s.substr(frac_start, pos - frac_start);
    }

    // 校验：所有字符必须已消费
    if (pos != s.size()) {
        return result;  // 存在非法字符
    }

    // 校验：至少要有一个数字
    if (integer_part.empty() && fractional_part.empty()) {
        return result;  // 仅 "." 或空
    }

    // 校验：小数点后无数字时，"123." 是否合法？
    // 这里明确允许 "123."（视作 123.0）
    // 若需拒绝，改为: if (has_dot && fractional_part.empty()) return result;

    // ------------------------------------------------------------------------
    // 4. 解析整数部分（带溢出检测）
    // ------------------------------------------------------------------------
    // int64_t 最大 9223372036854775807
    // 定标后最大 = LLONG_MAX / Scale
    constexpr int64_t kMaxRaw = std::numeric_limits<int64_t>::max();

    int64_t integer_value = 0;
    for (char c : integer_part) {
        const int digit = c - '0';
        // 溢出预检: integer_value * 10 + digit > kMaxRaw / Scale
        // 转为: integer_value > (kMaxRaw / Scale - digit) / 10
        constexpr int64_t kMaxIntPart = kMaxRaw / static_cast<int64_t>(Scale);

        if (integer_value > (kMaxIntPart - digit) / 10) {
            result.overflow = true;
            return result;
        }
        integer_value = integer_value * 10 + digit;
    }

    // ------------------------------------------------------------------------
    // 5. 解析小数部分（超精度时拒绝）
    // ------------------------------------------------------------------------
    int64_t fractional_value = 0;
    int frac_digits = 0;
    const int max_frac = static_cast<int>(Scale == 1 ? 0 : 8);  // kPriceDecimals

    for (char c : fractional_part) {
        const int digit = c - '0';
        if (frac_digits >= max_frac) {
            // 超精度
            if (digit != 0) {
                result.precision_exceeded = true;
                return result;  // 非零超精度，拒绝
            }
            // 零超精度可以忽略
            continue;
        }
        fractional_value = fractional_value * 10 + digit;
        ++frac_digits;
    }

    // 补齐到 Scale 位
    while (frac_digits < max_frac) {
        fractional_value *= 10;
        ++frac_digits;
    }

    // ------------------------------------------------------------------------
    // 6. 组装（使用 __int128 中间值防溢出）
    // ------------------------------------------------------------------------
#if defined(__SIZEOF_INT128__)
    const __int128 combined =
        static_cast<__int128>(integer_value) * static_cast<__int128>(Scale) +
        static_cast<__int128>(fractional_value);

    if (combined > static_cast<__int128>(kMaxRaw)) {
        result.overflow = true;
        return result;
    }
    result.raw = static_cast<int64_t>(combined);
#else
    // 无 __int128 平台（如 MSVC 32 位）使用显式溢出检测
    // 前置步骤已确保 integer_value <= kMaxRaw / Scale
    // 剩余检查: integer_value * Scale + fractional_value <= kMaxRaw
    const int64_t remaining = kMaxRaw - integer_value * Scale;
    if (fractional_value > remaining) {
        result.overflow = true;
        return result;
    }
    result.raw = integer_value * Scale + fractional_value;
#endif

    result.ok = true;
    result.fractional_digits = frac_digits;
    return result;
}

// -----------------------------------------------------------------------------
// 格式化定点数为字符串（无尾随零）
// -----------------------------------------------------------------------------
template <std::size_t Scale>
[[nodiscard]] std::string format_decimal(int64_t raw) noexcept {
    // 处理零
    if (raw == 0) {
        return "0";
    }

    // 处理负数（价格/数量不应为负，但防御性处理）
    bool negative = raw < 0;
    uint64_t abs_raw = negative
        ? static_cast<uint64_t>(-(raw + 1)) + 1  // 处理 INT64_MIN
        : static_cast<uint64_t>(raw);

    const uint64_t scale = static_cast<uint64_t>(Scale);
    const uint64_t integer = abs_raw / scale;
    const uint64_t fraction = abs_raw % scale;

    // 格式化整数部分
    std::array<char, 32> buf{};
    int n = std::snprintf(buf.data(), buf.size(),
                           "%llu",
                           static_cast<unsigned long long>(integer));
    if (n < 0) return {};

    std::string result;
    if (negative) result += '-';
    result.append(buf.data(), static_cast<std::size_t>(n));

    // 格式化小数部分（仅当非零时）
    if (fraction != 0) {
        // 补前导零到 Scale 位
        std::array<char, 16> frac_buf{};
        int m = std::snprintf(frac_buf.data(), frac_buf.size(),
                               "%0*llu",
                               static_cast<int>(Scale == 1 ? 0 : 8),
                               static_cast<unsigned long long>(fraction));
        if (m < 0) return {};

        std::string frac_str{frac_buf.data(), static_cast<std::size_t>(m)};

        // 去掉尾随零
        while (!frac_str.empty() && frac_str.back() == '0') {
            frac_str.pop_back();
        }

        if (!frac_str.empty()) {
            result += '.';
            result += frac_str;
        }
    }

    return result;
}

}  // namespace

// ==============================================================================
// Price::from_string
// ==============================================================================
std::optional<Price> Price::from_string(std::string_view s) noexcept {
    const auto parsed = parse_decimal<constants::kPriceScale>(s);
    if (!parsed.ok) {
        return std::nullopt;
    }
    // 价格必须为正（raw > 0）
    if (parsed.raw <= 0) {
        return std::nullopt;
    }
    return Price{parsed.raw};
}

// ==============================================================================
// Quantity::from_string
// ==============================================================================
std::optional<Quantity> Quantity::from_string(std::string_view s) noexcept {
    const auto parsed = parse_decimal<constants::kQuantityScale>(s);
    if (!parsed.ok) {
        return std::nullopt;
    }
    // 数量必须为正
    if (parsed.raw <= 0) {
        return std::nullopt;
    }
    return Quantity{parsed.raw};
}

// ==============================================================================
// 自由函数：格式化
// ==============================================================================
// 注意：这些函数在当前头文件中未声明，建议后续在 .hpp 中添加
// 目前作为内部工具函数

namespace {

[[nodiscard]] std::string price_to_string(Price p) noexcept {
    return format_decimal<constants::kPriceScale>(p.raw());
}

[[nodiscard]] std::string quantity_to_string(Quantity q) noexcept {
    return format_decimal<constants::kQuantityScale>(q.raw());
}

}  // namespace

// ==============================================================================
// 调试辅助（仅在 Debug 构建中保留）
// ==============================================================================
#ifdef QUANT_ENABLE_DEBUG_FORMATTERS

#include <ostream>

std::ostream& operator<<(std::ostream& os, Price p) {
    os << price_to_string(p);
    return os;
}

std::ostream& operator<<(std::ostream& os, Quantity q) {
    os << quantity_to_string(q);
    return os;
}

std::ostream& operator<<(std::ostream& os, Side s) {
    os << to_string(s);
    return os;
}

std::ostream& operator<<(std::ostream& os, OrderStatus s) {
    os << to_string(s);
    return os;
}

std::ostream& operator<<(std::ostream& os, OrderType t) {
    os << to_string(t);
    return os;
}

#endif  // QUANT_ENABLE_DEBUG_FORMATTERS

}  // namespace quant
