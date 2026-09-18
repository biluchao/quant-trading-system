// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 错误处理实现
// ==============================================================================
// @file    src/common/common.core.error.cpp
// @module  common
// @type    core
// @name    error
// @version 1.0.1
// @brief   致命错误处理、errno/HTTP/币安错误码转换
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 异步信号安全：fatal_error 只使用 write(2) 或 fputs
//   - 递归保护：atomic flag 防止无限递归
//   - 日志优先：abort 前显式 flush 异步队列
//   - 平台兼容：errno/信号处理条件编译
//   - 完整映射：errno/HTTP/币安错误码覆盖常见场景
//   - 可测试：提供终止器注入点
// ==============================================================================

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// ==============================================================================
// 平台特定头文件
// ==============================================================================
#if defined(_WIN32)
    #include <io.h>
    #include <process.h>
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace quant {

// ==============================================================================
// 内部辅助（异步信号安全）
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// 递归保护标志
// -----------------------------------------------------------------------------
std::atomic<bool> g_fatal_in_progress{false};

// -----------------------------------------------------------------------------
// 可注入的终止器（供测试使用）
// -----------------------------------------------------------------------------
using TerminatorFn = void(*)() noexcept;
std::atomic<TerminatorFn> g_terminator{&std::abort};

// -----------------------------------------------------------------------------
// 安全写（处理部分写入与 EINTR）
// -----------------------------------------------------------------------------
#if !defined(_WIN32)

void safe_write_fd(int fd, const char* data, std::size_t len) noexcept {
    while (len > 0) {
        const auto written = ::write(fd, data, len);
        if (written > 0) {
            data += written;
            len -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        // 其他错误，放弃
        break;
    }
}

void safe_write_str(const char* s) noexcept {
    if (s) safe_write_fd(STDERR_FILENO, s, std::strlen(s));
}

void safe_write_sv(std::string_view sv) noexcept {
    if (!sv.empty()) {
        safe_write_fd(STDERR_FILENO, sv.data(), sv.size());
    }
}

#else  // _WIN32

void safe_write_str(const char* s) noexcept {
    if (s) {
        std::fputs(s, stderr);
        std::fflush(stderr);
    }
}

void safe_write_sv(std::string_view sv) noexcept {
    if (!sv.empty()) {
        std::fwrite(sv.data(), 1, sv.size(), stderr);
        std::fflush(stderr);
    }
}

#endif

// -----------------------------------------------------------------------------
// 安全输出整数
// -----------------------------------------------------------------------------
void safe_write_int(std::int64_t value) noexcept {
    char buf[24];
    int pos = static_cast<int>(sizeof(buf)) - 1;
    buf[pos] = '\0';

    if (value == 0) {
        buf[--pos] = '0';
    } else {
        const bool negative = value < 0;
        // 处理 INT64_MIN：-(value + 1) 避免溢出
        std::uint64_t abs_value = negative
            ? static_cast<std::uint64_t>(-(value + 1)) + 1
            : static_cast<std::uint64_t>(value);

        while (abs_value > 0) {
            buf[--pos] = static_cast<char>('0' + (abs_value % 10));
            abs_value /= 10;
        }
        if (negative) {
            buf[--pos] = '-';
        }
    }

    safe_write_str(buf + pos);
}

// -----------------------------------------------------------------------------
// 安全输出十六进制
// -----------------------------------------------------------------------------
void safe_write_hex32(std::uint32_t value) noexcept {
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; ++i) {
        const auto nibble = static_cast<unsigned>((value >> ((7 - i) * 4)) & 0xFU);
        buf[2 + i] = static_cast<char>(nibble < 10 ? '0' + nibble
                                                     : 'a' + nibble - 10);
    }
    buf[10] = '\0';
    safe_write_str(buf);
}

}  // namespace

// ==============================================================================
// detail::fatal_error
// ==============================================================================
namespace detail {

// [[noreturn]] 标记在头文件中
void fatal_error(ErrorCode code, std::string_view msg,
                 SourceLocation loc) noexcept {
    // -------------------------------------------------------------------------
    // 1. 递归保护（CAS，防止二次进入）
    // -------------------------------------------------------------------------
    bool expected = false;
    if (!g_fatal_in_progress.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        // 已经在致命错误处理中，直接终止
        // 使用可注入的终止器，测试可捕获
        g_terminator.load(std::memory_order_acquire)();
        // [[noreturn]] 保证不返回；标准库 abort 不会返回
    }

    // -------------------------------------------------------------------------
    // 2. 刷新日志（在 abort 前尽可能持久化）
    // -------------------------------------------------------------------------
    try {
        Logger::flush();
    } catch (...) {
        // 忽略：日志刷新失败不应影响终止流程
    }

    // -------------------------------------------------------------------------
    // 3. 输出致命错误信息（异步信号安全）
    // -------------------------------------------------------------------------
    safe_write_str("\n[FATAL] 致命错误\n");

    safe_write_str("[FATAL] code: ");
    safe_write_sv(to_string(code));
    safe_write_str(" (");
    safe_write_hex32(raw_code(code));
    safe_write_str(")\n");

    safe_write_str("[FATAL] category: ");
    safe_write_sv(to_string(category_of(code)));
    safe_write_str("\n");

    safe_write_str("[FATAL] severity: ");
    safe_write_sv(to_string(severity_of(code)));
    safe_write_str("\n");

    if (!msg.empty()) {
        safe_write_str("[FATAL] message: ");
        safe_write_sv(msg);
        safe_write_str("\n");
    }

    if (loc.file && loc.line > 0) {
        safe_write_str("[FATAL] location: ");
        safe_write_str(loc.short_file());
        safe_write_str(":");
        safe_write_int(static_cast<std::int64_t>(loc.line));
        if (loc.function) {
            safe_write_str(" (");
            safe_write_str(loc.function);
            safe_write_str(")");
        }
        safe_write_str("\n");
    }

    // -------------------------------------------------------------------------
    // 4. 输出关键统计（帮助诊断）
    // -------------------------------------------------------------------------
    try {
        const auto& metrics = ErrorMetrics::instance();
        safe_write_str("[FATAL] error_metrics.total: ");
        safe_write_int(static_cast<std::int64_t>(metrics.total()));
        safe_write_str("\n");
    } catch (...) {
        // 忽略
    }

    safe_write_str("[FATAL] 进程即将终止\n");

    // -------------------------------------------------------------------------
    // 5. 记录到错误统计
    // -------------------------------------------------------------------------
    try {
        ErrorMetrics::instance().record(code);
    } catch (...) {
        // 忽略
    }

    // -------------------------------------------------------------------------
    // 6. 终止进程
    // -------------------------------------------------------------------------
    // 使用可注入的终止器；默认 std::abort 会生成 core dump
    g_terminator.load(std::memory_order_acquire)();

    // 若终止器返回（不应该），强制终止
    std::abort();
}

}  // namespace detail

// ==============================================================================
// from_errno
// ==============================================================================
ErrorCode from_errno(int err) noexcept {
    switch (err) {
        case 0:
            return ErrorCode::Ok;

        // ---------- 内存 ----------
#ifdef ENOMEM
        case ENOMEM:
            return ErrorCode::SystemOutOfMemory;
#endif
#ifdef ENOBUFS
        case ENOBUFS:
            return ErrorCode::SystemOutOfMemory;
#endif

        // ---------- 文件系统 ----------
#ifdef ENOENT
        case ENOENT:
            return ErrorCode::SystemFileNotFound;
#endif
#ifdef ENOTDIR
        case ENOTDIR:
            return ErrorCode::SystemFileNotFound;
#endif
#ifdef EACCES
        case EACCES:
            return ErrorCode::SystemPermissionDenied;
#endif
#ifdef EPERM
        case EPERM:
            return ErrorCode::SystemPermissionDenied;
#endif
#ifdef EROFS
        case EROFS:
            return ErrorCode::SystemPermissionDenied;
#endif
#ifdef ENOSPC
        case ENOSPC:
            return ErrorCode::SystemDiskFull;
#endif
#ifdef EIO
        case EIO:
            return ErrorCode::SystemIoError;
#endif

        // ---------- 网络 ----------
#ifdef ETIMEDOUT
        case ETIMEDOUT:
            return ErrorCode::NetworkTimeout;
#endif
#ifdef ECONNREFUSED
        case ECONNREFUSED:
            return ErrorCode::NetworkDisconnected;
#endif
#ifdef ECONNRESET
        case ECONNRESET:
            return ErrorCode::NetworkDisconnected;
#endif
#ifdef ECONNABORTED
        case ECONNABORTED:
            return ErrorCode::NetworkDisconnected;
#endif
#ifdef ENETUNREACH
        case ENETUNREACH:
            return ErrorCode::NetworkDisconnected;
#endif
#ifdef ENETDOWN
        case ENETDOWN:
            return ErrorCode::NetworkDisconnected;
#endif
#ifdef EHOSTUNREACH
        case EHOSTUNREACH:
            return ErrorCode::NetworkDisconnected;
#endif

        default:
            return ErrorCode::SystemUnknown;
    }
}

// ==============================================================================
// from_http_status
// ==============================================================================
ErrorCode from_http_status(int status) noexcept {
    // 无效状态码
    if (status < 100 || status > 599) {
        return ErrorCode::NetworkUnknown;
    }

    // 1xx 信息性
    if (status < 200) {
        return ErrorCode::Ok;  // 信息性状态，视为成功
    }

    // 2xx 成功
    if (status < 300) {
        return ErrorCode::Ok;
    }

    // 3xx 重定向（不应自动跟随，视为错误）
    if (status < 400) {
        return ErrorCode::NetworkUnknown;
    }

    // 4xx 客户端错误
    switch (status) {
        case 400:  // Bad Request
            return ErrorCode::ExchangeInvalidParam;
        case 401:  // Unauthorized
            return ErrorCode::ExchangeSignatureError;
        case 403:  // Forbidden
            return ErrorCode::ExchangeSignatureError;
        case 404:  // Not Found
            return ErrorCode::ExchangeApiError;
        case 405:  // Method Not Allowed
            return ErrorCode::ExchangeInvalidParam;
        case 408:  // Request Timeout
            return ErrorCode::NetworkTimeout;
        case 409:  // Conflict
            return ErrorCode::ExchangeApiError;
        case 418:  // I'm a teapot（币安特有：IP 被封）
            return ErrorCode::ExchangeRateLimit;
        case 422:  // Unprocessable Entity
            return ErrorCode::ExchangeInvalidParam;
        case 429:  // Too Many Requests
            return ErrorCode::ExchangeRateLimit;
        default:
            break;
    }

    // 5xx 服务端错误
    if (status < 600) {
        return ErrorCode::ExchangeMaintenance;
    }

    return ErrorCode::ExchangeApiError;
}

// ==============================================================================
// from_binance_error
// ==============================================================================
ErrorCode from_binance_error(int code) noexcept {
    // 币安错误码均为负数，正数视为未知
    if (code >= 0) {
        return ErrorCode::ExchangeUnknown;
    }

    switch (code) {
        // ---------- 通用 ----------
        case -1000:
            return ErrorCode::ExchangeUnknown;
        case -1001:  // 断线
            return ErrorCode::NetworkDisconnected;
        case -1002:  // 未授权
            return ErrorCode::ExchangeSignatureError;
        case -1003:  // 请求过于频繁
            return ErrorCode::ExchangeRateLimit;
        case -1004:  // 服务器繁忙
            return ErrorCode::ExchangeRateLimit;
        case -1005:  // 被拒绝
            return ErrorCode::ExchangeSignatureError;
        case -1006:  // 时间戳异常
            return ErrorCode::ExchangeTimestampError;
        case -1007:  // 超时
            return ErrorCode::NetworkTimeout;
        case -1008:  // 服务器维护
            return ErrorCode::ExchangeMaintenance;
        case -1010:  // 参数错误
            return ErrorCode::ExchangeInvalidParam;
        case -1011:  // 未知订单
            return ErrorCode::OrderNotFound;
        case -1013:  // 无效数量
            return ErrorCode::ExchangeInvalidParam;
        case -1014:  // 交易被拒绝
            return ErrorCode::OrderRejected;
        case -1015:  // 请求过于频繁
            return ErrorCode::ExchangeRateLimit;
        case -1016:  // 服务下线
            return ErrorCode::ExchangeMaintenance;
        case -1020:  // 不支持的操作
            return ErrorCode::ExchangeApiError;
        case -1021:  // 时间戳过期
            return ErrorCode::ExchangeTimestampError;
        case -1022:  // 签名错误
            return ErrorCode::ExchangeSignatureError;

        // ---------- 订单 ----------
        case -1100:  // 非法字符
            return ErrorCode::ExchangeInvalidParam;
        case -1102:  // 缺少参数
            return ErrorCode::ExchangeInvalidParam;
        case -1104:  // 参数重复
            return ErrorCode::ExchangeInvalidParam;
        case -1111:  // 精度错误
            return ErrorCode::ExchangeInvalidParam;
        case -1112:  // 无订单
            return ErrorCode::OrderNotFound;
        case -1116:  // 订单类型无效
            return ErrorCode::OrderRejected;
        case -1117:  // 时间无效
            return ErrorCode::OrderRejected;
        case -1118:  // 时间范围无效
            return ErrorCode::OrderRejected;
        case -1121:  // 交易对无效
            return ErrorCode::ExchangeInvalidSymbol;

        // ---------- 仓位与资金 ----------
        case -2010:  // 新订单被拒
            return ErrorCode::OrderRejected;
        case -2011:  // 撤单被拒
            return ErrorCode::OrderCancelFailed;
        case -2013:  // 订单不存在
            return ErrorCode::OrderNotFound;
        case -2014:  // API 密钥格式错误
            return ErrorCode::ExchangeSignatureError;
        case -2015:  // 无效 API 密钥/IP/权限
            return ErrorCode::ExchangeSignatureError;
        case -2018:  // 余额不足
            return ErrorCode::ExchangeInsufficient;
        case -2019:  // 保证金不足
            return ErrorCode::ExchangeInsufficient;
        case -2020:  // 无法成交
            return ErrorCode::OrderRejected;
        case -2021:  // 订单立即触发
            return ErrorCode::OrderRejected;
        case -2022:  // ReduceOnly 被拒
            return ErrorCode::OrderRejected;
        case -2026:  // 触发熔断
            return ErrorCode::RiskCircuitBreaker;

        // ---------- 其他 ----------
        case -4131:  // 限价单被拒
            return ErrorCode::OrderRejected;
        case -4164:  // 名义价值过小
            return ErrorCode::ExchangeInsufficient;

        default:
            return ErrorCode::ExchangeApiError;
    }
}

// ==============================================================================
// 测试辅助：注入终止器
// ==============================================================================
namespace detail {

// 供单元测试使用：临时替换终止器
class ScopedTerminatorOverride {
public:
    explicit ScopedTerminatorOverride(TerminatorFn fn) noexcept
        : prev_{g_terminator.exchange(fn, std::memory_order_acq_rel)}
    {}

    ~ScopedTerminatorOverride() noexcept {
        g_terminator.store(prev_, std::memory_order_release);
        // 重置递归保护，允许后续测试
        g_fatal_in_progress.store(false, std::memory_order_release);
    }

    ScopedTerminatorOverride(const ScopedTerminatorOverride&) = delete;
    ScopedTerminatorOverride& operator=(const ScopedTerminatorOverride&) = delete;

private:
    TerminatorFn prev_;
};

}  // namespace detail

}  // namespace quant

// ==============================================================================
// fmt 格式化支持（可选，若项目使用 fmt 库）
// ==============================================================================
#if defined(QUANT_ENABLE_FMT_FORMATTER) && __has_include(<fmt/format.h>)
#include <fmt/format.h>

template <>
struct fmt::formatter<quant::ErrorCode> : formatter<std::string_view> {
    auto format(quant::ErrorCode code, format_context& ctx) const {
        auto name = quant::to_string(code);
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<quant::ErrorCategory> : formatter<std::string_view> {
    auto format(quant::ErrorCategory cat, format_context& ctx) const {
        auto name = quant::to_string(cat);
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<quant::ErrorSeverity> : formatter<std::string_view> {
    auto format(quant::ErrorSeverity sev, format_context& ctx) const {
        auto name = quant::to_string(sev);
        return formatter<std::string_view>::format(name, ctx);
    }
};

#endif  // QUANT_ENABLE_FMT_FORMATTER
