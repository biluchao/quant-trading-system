// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 错误处理
// ==============================================================================
// @file    src/common/common.core.error.hpp
// @module  common
// @type    core
// @name    error
// @version 1.0.1
// @brief   错误码、Result<T,E>、异常桥接、严重等级
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 热路径零开销：Result<T,E> 无动态分配，无异常
//   - 冷路径诊断：异常携带完整上下文与堆栈
//   - 分类编码：高 8 位表示类别，低 24 位表示具体错误
//   - 可重试标记：网络/限流错误自动标记为可重试
//   - 严重等级：INFO/WARN/ERROR/FATAL，映射到日志级别
//   - 错误链：context 支持多层包装
//   - 度量：错误计数器供 Prometheus 采集
// ==============================================================================

#ifndef QUANT_COMMON_CORE_ERROR_HPP
#define QUANT_COMMON_CORE_ERROR_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace quant {

// ==============================================================================
// 源码位置（编译期注入）
// ==============================================================================
struct SourceLocation {
    const char* file{""};
    std::uint32_t line{0};
    const char* function{""};

    [[nodiscard]] const char* short_file() const noexcept {
        if (!file) return "";
        const char* last = file;
        for (const char* p = file; *p; ++p) {
            if (*p == '/' || *p == '\\') last = p + 1;
        }
        return last;
    }
};

// ==============================================================================
// 错误类别（高 8 位）
// ==============================================================================
enum class ErrorCategory : std::uint8_t {
    Unknown       = 0x00,
    System        = 0x01,   // 操作系统、内存、文件
    Network       = 0x02,   // 网络、超时、DNS
    Exchange      = 0x03,   // 交易所 API、限流、签名
    Database      = 0x04,   // 数据库、SQL
    Config        = 0x05,   // 配置、参数、校验
    Strategy      = 0x06,   // 策略、信号、评分
    Order         = 0x07,   // 订单、成交、撤销
    Risk          = 0x08,   // 风控、盈亏比、熔断
    AI            = 0x09,   // 模型推理、漂移
    Internal      = 0x0A,   // 内部逻辑、不变量
    UserInput     = 0x0B,   // 用户输入、参数
};

[[nodiscard]] constexpr std::string_view to_string(ErrorCategory c) noexcept {
    switch (c) {
        case ErrorCategory::Unknown:   return "Unknown";
        case ErrorCategory::System:    return "System";
        case ErrorCategory::Network:   return "Network";
        case ErrorCategory::Exchange:  return "Exchange";
        case ErrorCategory::Database:  return "Database";
        case ErrorCategory::Config:    return "Config";
        case ErrorCategory::Strategy:  return "Strategy";
        case ErrorCategory::Order:     return "Order";
        case ErrorCategory::Risk:      return "Risk";
        case ErrorCategory::AI:        return "AI";
        case ErrorCategory::Internal:  return "Internal";
        case ErrorCategory::UserInput: return "UserInput";
    }
    return "Unknown";
}

// ==============================================================================
// 错误严重等级
// ==============================================================================
enum class ErrorSeverity : std::uint8_t {
    Info     = 0,
    Warning  = 1,
    Error    = 2,
    Critical = 3,
    Fatal    = 4,  // 记录后 abort
};

[[nodiscard]] constexpr std::string_view to_string(ErrorSeverity s) noexcept {
    switch (s) {
        case ErrorSeverity::Info:     return "INFO";
        case ErrorSeverity::Warning:  return "WARN";
        case ErrorSeverity::Error:    return "ERROR";
        case ErrorSeverity::Critical: return "CRITICAL";
        case ErrorSeverity::Fatal:    return "FATAL";
    }
    return "UNKNOWN";
}

// ==============================================================================
// 错误码（32 位：8 位类别 + 24 位具体错误）
// ==============================================================================
enum class ErrorCode : std::uint32_t {
    Ok = 0,

    // ---------- System (0x01) ----------
    SystemUnknown          = 0x0100'0001,
    SystemOutOfMemory      = 0x0100'0002,
    SystemFileNotFound     = 0x0100'0003,
    SystemPermissionDenied = 0x0100'0004,
    SystemDiskFull         = 0x0100'0005,
    SystemIoError          = 0x0100'0006,

    // ---------- Network (0x02) ----------
    NetworkUnknown         = 0x0200'0001,
    NetworkTimeout         = 0x0200'0002,
    NetworkDisconnected    = 0x0200'0003,
    NetworkDnsFailure      = 0x0200'0004,
    NetworkSslError        = 0x0200'0005,

    // ---------- Exchange (0x03) ----------
    ExchangeUnknown        = 0x0300'0001,
    ExchangeRateLimit      = 0x0300'0002,  // 可重试
    ExchangeApiError       = 0x0300'0003,
    ExchangeInvalidSymbol  = 0x0300'0004,
    ExchangeInvalidParam   = 0x0300'0005,
    ExchangeInsufficient   = 0x0300'0006,
    ExchangeMaintenance    = 0x0300'0007,
    ExchangeSignatureError = 0x0300'0008,
    ExchangeTimestampError = 0x0300'0009,

    // ---------- Database (0x04) ----------
    DatabaseUnknown        = 0x0400'0001,
    DatabaseConnectionLost = 0x0400'0002,
    DatabaseQueryFailed    = 0x0400'0003,
    DatabaseDeadlock       = 0x0400'0004,
    DatabaseConstraint     = 0x0400'0005,

    // ---------- Config (0x05) ----------
    ConfigUnknown          = 0x0500'0001,
    ConfigNotFound         = 0x0500'0002,
    ConfigParseFailed      = 0x0500'0003,
    ConfigInvalidValue     = 0x0500'0004,
    ConfigSchemaMismatch   = 0x0500'0005,

    // ---------- Strategy (0x06) ----------
    StrategyUnknown        = 0x0600'0001,
    StrategyInvalidSignal  = 0x0600'0002,
    StrategyScoreTooLow    = 0x0600'0003,
    StrategyRiskRewardFail = 0x0600'0004,
    StrategyStateInvalid   = 0x0600'0005,

    // ---------- Order (0x07) ----------
    OrderUnknown           = 0x0700'0001,
    OrderRejected          = 0x0700'0002,
    OrderNotFound          = 0x0700'0003,
    OrderDuplicate         = 0x0700'0004,
    OrderPartialFill       = 0x0700'0005,
    OrderCancelFailed      = 0x0700'0006,
    OrderStopLossMissing   = 0x0700'0007,

    // ---------- Risk (0x08) ----------
    RiskUnknown            = 0x0800'0001,
    RiskPositionLimit      = 0x0800'0002,
    RiskDailyLossLimit     = 0x0800'0003,
    RiskDrawdownLimit      = 0x0800'0004,
    RiskLeverageLimit      = 0x0800'0005,
    RiskLiquidityLow       = 0x0800'0006,
    RiskCircuitBreaker     = 0x0800'0007,

    // ---------- AI (0x09) ----------
    AiUnknown              = 0x0900'0001,
    AiInferenceTimeout     = 0x0900'0002,
    AiModelDrift           = 0x0900'0003,
    AiFeatureInvalid       = 0x0900'0004,
    AiModelLoadFailed      = 0x0900'0005,

    // ---------- Internal (0x0A) ----------
    InternalUnknown        = 0x0A00'0001,
    InternalInvariant      = 0x0A00'0002,
    InternalNotImplemented = 0x0A00'0003,
    InternalAssertion      = 0x0A00'0004,

    // ---------- UserInput (0x0B) ----------
    UserInputUnknown       = 0x0B00'0001,
    UserInputInvalid       = 0x0B00'0002,
    UserInputOutOfRange    = 0x0B00'0003,
};

// -----------------------------------------------------------------------------
// 错误码辅助
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr bool is_ok(ErrorCode c) noexcept {
    return c == ErrorCode::Ok;
}

[[nodiscard]] constexpr ErrorCategory category_of(ErrorCode c) noexcept {
    const auto v = static_cast<std::uint32_t>(c);
    return static_cast<ErrorCategory>((v >> 24) & 0xFF);
}

[[nodiscard]] constexpr std::uint32_t raw_code(ErrorCode c) noexcept {
    return static_cast<std::uint32_t>(c);
}

[[nodiscard]] constexpr std::string_view to_string(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::Ok:                     return "OK";

        case ErrorCode::SystemUnknown:          return "SystemUnknown";
        case ErrorCode::SystemOutOfMemory:      return "SystemOutOfMemory";
        case ErrorCode::SystemFileNotFound:     return "SystemFileNotFound";
        case ErrorCode::SystemPermissionDenied: return "SystemPermissionDenied";
        case ErrorCode::SystemDiskFull:         return "SystemDiskFull";
        case ErrorCode::SystemIoError:          return "SystemIoError";

        case ErrorCode::NetworkUnknown:         return "NetworkUnknown";
        case ErrorCode::NetworkTimeout:         return "NetworkTimeout";
        case ErrorCode::NetworkDisconnected:    return "NetworkDisconnected";
        case ErrorCode::NetworkDnsFailure:      return "NetworkDnsFailure";
        case ErrorCode::NetworkSslError:        return "NetworkSslError";

        case ErrorCode::ExchangeUnknown:        return "ExchangeUnknown";
        case ErrorCode::ExchangeRateLimit:      return "ExchangeRateLimit";
        case ErrorCode::ExchangeApiError:       return "ExchangeApiError";
        case ErrorCode::ExchangeInvalidSymbol:  return "ExchangeInvalidSymbol";
        case ErrorCode::ExchangeInvalidParam:   return "ExchangeInvalidParam";
        case ErrorCode::ExchangeInsufficient:   return "ExchangeInsufficient";
        case ErrorCode::ExchangeMaintenance:    return "ExchangeMaintenance";
        case ErrorCode::ExchangeSignatureError: return "ExchangeSignatureError";
        case ErrorCode::ExchangeTimestampError: return "ExchangeTimestampError";

        case ErrorCode::DatabaseUnknown:        return "DatabaseUnknown";
        case ErrorCode::DatabaseConnectionLost: return "DatabaseConnectionLost";
        case ErrorCode::DatabaseQueryFailed:    return "DatabaseQueryFailed";
        case ErrorCode::DatabaseDeadlock:       return "DatabaseDeadlock";
        case ErrorCode::DatabaseConstraint:     return "DatabaseConstraint";

        case ErrorCode::ConfigUnknown:          return "ConfigUnknown";
        case ErrorCode::ConfigNotFound:         return "ConfigNotFound";
        case ErrorCode::ConfigParseFailed:      return "ConfigParseFailed";
        case ErrorCode::ConfigInvalidValue:     return "ConfigInvalidValue";
        case ErrorCode::ConfigSchemaMismatch:   return "ConfigSchemaMismatch";

        case ErrorCode::StrategyUnknown:        return "StrategyUnknown";
        case ErrorCode::StrategyInvalidSignal:  return "StrategyInvalidSignal";
        case ErrorCode::StrategyScoreTooLow:    return "StrategyScoreTooLow";
        case ErrorCode::StrategyRiskRewardFail: return "StrategyRiskRewardFail";
        case ErrorCode::StrategyStateInvalid:   return "StrategyStateInvalid";

        case ErrorCode::OrderUnknown:           return "OrderUnknown";
        case ErrorCode::OrderRejected:          return "OrderRejected";
        case ErrorCode::OrderNotFound:          return "OrderNotFound";
        case ErrorCode::OrderDuplicate:         return "OrderDuplicate";
        case ErrorCode::OrderPartialFill:       return "OrderPartialFill";
        case ErrorCode::OrderCancelFailed:      return "OrderCancelFailed";
        case ErrorCode::OrderStopLossMissing:   return "OrderStopLossMissing";

        case ErrorCode::RiskUnknown:            return "RiskUnknown";
        case ErrorCode::RiskPositionLimit:      return "RiskPositionLimit";
        case ErrorCode::RiskDailyLossLimit:     return "RiskDailyLossLimit";
        case ErrorCode::RiskDrawdownLimit:      return "RiskDrawdownLimit";
        case ErrorCode::RiskLeverageLimit:      return "RiskLeverageLimit";
        case ErrorCode::RiskLiquidityLow:       return "RiskLiquidityLow";
        case ErrorCode::RiskCircuitBreaker:     return "RiskCircuitBreaker";

        case ErrorCode::AiUnknown:              return "AiUnknown";
        case ErrorCode::AiInferenceTimeout:     return "AiInferenceTimeout";
        case ErrorCode::AiModelDrift:           return "AiModelDrift";
        case ErrorCode::AiFeatureInvalid:       return "AiFeatureInvalid";
        case ErrorCode::AiModelLoadFailed:      return "AiModelLoadFailed";

        case ErrorCode::InternalUnknown:        return "InternalUnknown";
        case ErrorCode::InternalInvariant:      return "InternalInvariant";
        case ErrorCode::InternalNotImplemented: return "InternalNotImplemented";
        case ErrorCode::InternalAssertion:      return "InternalAssertion";

        case ErrorCode::UserInputUnknown:       return "UserInputUnknown";
        case ErrorCode::UserInputInvalid:       return "UserInputInvalid";
        case ErrorCode::UserInputOutOfRange:    return "UserInputOutOfRange";
    }
    return "Unknown";
}

// -----------------------------------------------------------------------------
// 可重试判定
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr bool is_retryable(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::NetworkTimeout:
        case ErrorCode::NetworkDisconnected:
        case ErrorCode::NetworkDnsFailure:
        case ErrorCode::ExchangeRateLimit:
        case ErrorCode::ExchangeMaintenance:
        case ErrorCode::DatabaseConnectionLost:
        case ErrorCode::DatabaseDeadlock:
        case ErrorCode::AiInferenceTimeout:
            return true;
        default:
            return false;
    }
}

// -----------------------------------------------------------------------------
// 严重等级判定
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr ErrorSeverity severity_of(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::Ok:
            return ErrorSeverity::Info;

        // 致命错误
        case ErrorCode::SystemOutOfMemory:
        case ErrorCode::SystemDiskFull:
        case ErrorCode::InternalInvariant:
        case ErrorCode::InternalAssertion:
            return ErrorSeverity::Fatal;

        // 严重错误
        case ErrorCode::ExchangeSignatureError:
        case ErrorCode::ExchangeTimestampError:
        case ErrorCode::RiskDailyLossLimit:
        case ErrorCode::RiskDrawdownLimit:
        case ErrorCode::RiskCircuitBreaker:
        case ErrorCode::AiModelDrift:
        case ErrorCode::OrderStopLossMissing:
            return ErrorSeverity::Critical;

        // 错误
        case ErrorCode::NetworkUnknown:
        case ErrorCode::ExchangeUnknown:
        case ErrorCode::ExchangeApiError:
        case ErrorCode::DatabaseQueryFailed:
        case ErrorCode::ConfigParseFailed:
        case ErrorCode::StrategyInvalidSignal:
        case ErrorCode::OrderRejected:
        case ErrorCode::RiskPositionLimit:
            return ErrorSeverity::Error;

        // 警告
        case ErrorCode::NetworkTimeout:
        case ErrorCode::NetworkDisconnected:
        case ErrorCode::ExchangeRateLimit:
        case ErrorCode::OrderPartialFill:
        case ErrorCode::AiInferenceTimeout:
            return ErrorSeverity::Warning;

        default:
            return ErrorSeverity::Error;
    }
}

// ==============================================================================
// 上下文（key-value 对，用于错误诊断）
// ==============================================================================
class ErrorContext {
public:
    static constexpr std::size_t kMaxFields = 8;
    static constexpr std::size_t kKeyMaxLen = 24;
    static constexpr std::size_t kValueMaxLen = 64;

    struct Field {
        std::array<char, kKeyMaxLen> key{};
        std::array<char, kValueMaxLen> value{};
    };

    ErrorContext() noexcept = default;

    // 添加字段（不分配）
    ErrorContext& add(std::string_view key, std::string_view value) noexcept {
        if (count_ >= kMaxFields) return *this;
        auto& f = fields_[count_++];
        copy_truncated(f.key, key);
        copy_truncated(f.value, value);
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    [[nodiscard]] std::string to_string() const {
        std::string result;
        for (std::size_t i = 0; i < count_; ++i) {
            if (i > 0) result += ", ";
            result += fields_[i].key.data();
            result += '=';
            result += fields_[i].value.data();
        }
        return result;
    }

private:
    template <std::size_t N>
    static void copy_truncated(std::array<char, N>& dst, std::string_view src) noexcept {
        const auto n = (src.size() < N - 1) ? src.size() : N - 1;
        std::memcpy(dst.data(), src.data(), n);
        dst[n] = '\0';
    }

    std::array<Field, kMaxFields> fields_{};
    std::size_t count_{0};
};

// ==============================================================================
// 错误信息（携带 code + message + context + location）
// ==============================================================================
struct ErrorInfo {
    ErrorCode code{ErrorCode::Ok};
    std::string_view message{};      // 静态或调用方持有的字符串
    SourceLocation location{};
    ErrorContext context{};

    ErrorInfo() noexcept = default;

    ErrorInfo(ErrorCode c, std::string_view msg) noexcept
        : code{c}, message{msg} {}

    ErrorInfo(ErrorCode c, std::string_view msg, SourceLocation loc) noexcept
        : code{c}, message{msg}, location{loc} {}

    [[nodiscard]] bool is_ok() const noexcept { return code == ErrorCode::Ok; }

    [[nodiscard]] std::string to_string() const {
        std::ostringstream oss;
        oss << "[" << quant::to_string(code) << "] "
            << "(" << quant::to_string(category_of(code)) << ") "
            << message;
        if (context.size() > 0) {
            oss << " {" << context.to_string() << "}";
        }
        if (location.line > 0) {
            oss << " at " << location.short_file() << ":" << location.line;
        }
        return oss.str();
    }
};

// ==============================================================================
// Result<T, E>（热路径无异常）
// ==============================================================================
template <typename T>
class Result {
public:
    // 成功构造
    Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : storage_{std::in_place_index<0>, std::move(value)}
    {}

    // 失败构造（从 ErrorCode）
    Result(ErrorCode code, std::string_view msg = "") noexcept
        : storage_{std::in_place_index<1>, ErrorInfo{code, msg}}
    {}

    // 失败构造（从 ErrorInfo）
    Result(ErrorInfo info) noexcept
        : storage_{std::in_place_index<1>, std::move(info)}
    {}

    // 拷贝与移动
    Result(const Result&) = default;
    Result(Result&&) noexcept = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) noexcept = default;

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] bool is_ok() const noexcept {
        return storage_.index() == 0;
    }

    [[nodiscard]] bool is_err() const noexcept {
        return storage_.index() == 1;
    }

    explicit operator bool() const noexcept { return is_ok(); }

    // -------------------------------------------------------------------------
    // 访问
    // -------------------------------------------------------------------------
    [[nodiscard]] T& value() & {
        if (is_err()) throw_error();
        return std::get<0>(storage_);
    }

    [[nodiscard]] const T& value() const& {
        if (is_err()) throw_error();
        return std::get<0>(storage_);
    }

    [[nodiscard]] T&& value() && {
        if (is_err()) throw_error();
        return std::move(std::get<0>(storage_));
    }

    [[nodiscard]] const ErrorInfo& error() const noexcept {
        static const ErrorInfo kOkError{ErrorCode::Ok, "ok"};
        return is_err() ? std::get<1>(storage_) : kOkError;
    }

    [[nodiscard]] ErrorCode error_code() const noexcept {
        return is_err() ? std::get<1>(storage_).code : ErrorCode::Ok;
    }

    // 获取或默认值
    template <typename U>
    [[nodiscard]] T value_or(U&& default_value) const& {
        return is_ok() ? std::get<0>(storage_)
                       : static_cast<T>(std::forward<U>(default_value));
    }

    template <typename U>
    [[nodiscard]] T value_or(U&& default_value) && {
        return is_ok() ? std::move(std::get<0>(storage_))
                       : static_cast<T>(std::forward<U>(default_value));
    }

    // -------------------------------------------------------------------------
    // 单子操作（链式）
    // -------------------------------------------------------------------------
    template <typename F>
    auto map(F&& f) const& -> Result<std::invoke_result_t<F, const T&>> {
        using U = std::invoke_result_t<F, const T&>;
        if (is_ok()) {
            return Result<U>{std::forward<F>(f)(std::get<0>(storage_))};
        }
        return Result<U>{std::get<1>(storage_)};
    }

    template <typename F>
    auto and_then(F&& f) const&
        -> std::invoke_result_t<F, const T&>
    {
        if (is_ok()) {
            return std::forward<F>(f)(std::get<0>(storage_));
        }
        using U = typename std::invoke_result_t<F, const T&>::value_type;
        return std::invoke_result_t<F, const T&>{
            std::get<1>(storage_)};
    }

    template <typename F>
    auto map_err(F&& f) const -> Result<T> {
        if (is_err()) {
            return Result<T>{std::forward<F>(f)(std::get<1>(storage_))};
        }
        return *this;
    }

    // 添加上下文
    Result& with_context(std::string_view key, std::string_view value) noexcept {
        if (is_err()) {
            std::get<1>(storage_).context.add(key, value);
        }
        return *this;
    }

    // 附加源码位置
    Result& with_location(SourceLocation loc) noexcept {
        if (is_err()) {
            std::get<1>(storage_).location = loc;
        }
        return *this;
    }

private:
    void throw_error() const {
        const auto& info = std::get<1>(storage_);
        throw std::runtime_error{info.to_string()};
    }

    std::variant<T, ErrorInfo> storage_;
};

// -----------------------------------------------------------------------------
// 特化：Result<void>
// -----------------------------------------------------------------------------
template <>
class Result<void> {
public:
    Result() noexcept = default;

    Result(ErrorCode code, std::string_view msg = "") noexcept
        : error_{ErrorInfo{code, msg}}, has_error_{true} {}

    Result(ErrorInfo info) noexcept
        : error_{std::move(info)}, has_error_{true} {}

    [[nodiscard]] bool is_ok() const noexcept { return !has_error_; }
    [[nodiscard]] bool is_err() const noexcept { return has_error_; }
    explicit operator bool() const noexcept { return is_ok(); }

    [[nodiscard]] const ErrorInfo& error() const noexcept { return error_; }
    [[nodiscard]] ErrorCode error_code() const noexcept {
        return has_error_ ? error_.code : ErrorCode::Ok;
    }

    Result& with_context(std::string_view key, std::string_view value) noexcept {
        if (has_error_) error_.context.add(key, value);
        return *this;
    }

    Result& with_location(SourceLocation loc) noexcept {
        if (has_error_) error_.location = loc;
        return *this;
    }

    void value() const {
        if (has_error_) {
            throw std::runtime_error{error_.to_string()};
        }
    }

private:
    ErrorInfo error_{};
    bool has_error_{false};
};

// ==============================================================================
// 便捷宏：源码位置注入
// ==============================================================================
#define QUANT_CURRENT_LOCATION \
    ::quant::SourceLocation{__FILE__, static_cast<std::uint32_t>(__LINE__), __func__}

// 错误返回
#define QUANT_ERR(code) \
    (::quant::Result<void>{::quant::ErrorInfo{code, #code, QUANT_CURRENT_LOCATION}})

#define QUANT_ERR_MSG(code, msg) \
    (::quant::Result<void>{::quant::ErrorInfo{code, msg, QUANT_CURRENT_LOCATION}})

#define QUANT_ERR_T(T, code) \
    (::quant::Result<T>{::quant::ErrorInfo{code, #code, QUANT_CURRENT_LOCATION}})

#define QUANT_ERR_T_MSG(T, code, msg) \
    (::quant::Result<T>{::quant::ErrorInfo{code, msg, QUANT_CURRENT_LOCATION}})

// 不变量断言
#define QUANT_ENSURE(cond, code)                       \
    do {                                               \
        if (!(cond)) [[unlikely]] {                    \
            return QUANT_ERR(code);                    \
        }                                              \
    } while (0)

// 致命错误：记录后终止
#define QUANT_FATAL(code, msg)                                          \
    do {                                                                \
        ::quant::detail::fatal_error(                                   \
            code, msg, QUANT_CURRENT_LOCATION);                         \
    } while (0)

// ==============================================================================
// 错误度量（供 Prometheus 采集）
// ==============================================================================
class ErrorMetrics {
public:
    [[nodiscard]] static ErrorMetrics& instance() noexcept {
        static ErrorMetrics m;
        return m;
    }

    void record(ErrorCode code) noexcept {
        const auto idx = static_cast<std::size_t>(raw_code(code) & 0xFF);
        if (idx < kMaxCodes) {
            counters_[idx].fetch_add(1, std::memory_order_relaxed);
        }
        total_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t count(ErrorCode code) const noexcept {
        const auto idx = static_cast<std::size_t>(raw_code(code) & 0xFF);
        return idx < kMaxCodes
            ? counters_[idx].load(std::memory_order_relaxed)
            : 0;
    }

    [[nodiscard]] std::uint64_t total() const noexcept {
        return total_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        for (auto& c : counters_) c.store(0, std::memory_order_relaxed);
        total_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kMaxCodes = 256;
    std::array<std::atomic<std::uint64_t>, kMaxCodes> counters_{};
    std::atomic<std::uint64_t> total_{0};

    ErrorMetrics() noexcept {
        for (auto& c : counters_) c.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// detail 实现
// ==============================================================================
namespace detail {

// 致命错误处理：记录日志后 abort
[[noreturn]] void fatal_error(ErrorCode code, std::string_view msg,
                              SourceLocation loc) noexcept;

}  // namespace detail

// ==============================================================================
// 异常桥接（冷路径）
// ==============================================================================
class QuantException : public std::exception {
public:
    QuantException(ErrorCode code, std::string_view msg,
                   SourceLocation loc = {}) noexcept
        : info_{code, msg, loc}
    {}

    QuantException(ErrorInfo info) noexcept : info_{std::move(info)} {}

    [[nodiscard]] const char* what() const noexcept override {
        // 缓存 what() 字符串（仅在首次调用时构造）
        if (what_cache_.empty()) {
            const_cast<QuantException*>(this)->what_cache_ = info_.to_string();
        }
        return what_cache_.c_str();
    }

    [[nodiscard]] ErrorCode code() const noexcept { return info_.code; }
    [[nodiscard]] const ErrorInfo& info() const noexcept { return info_; }

private:
    ErrorInfo info_;
    mutable std::string what_cache_;
};

// 宏：throw 带源码位置
#define QUANT_THROW(code, msg) \
    throw ::quant::QuantException{code, msg, QUANT_CURRENT_LOCATION}

#define QUANT_THROW_INFO(info) \
    throw ::quant::QuantException{info}

// ==============================================================================
// 辅助：从系统 errno 转换
// ==============================================================================
[[nodiscard]] ErrorCode from_errno(int err) noexcept;

// 辅助：从 HTTP 状态码转换
[[nodiscard]] ErrorCode from_http_status(int status) noexcept;

// 辅助：从币安错误码转换
[[nodiscard]] ErrorCode from_binance_error(int code) noexcept;

// ==============================================================================
// 断言宏（Debug 与 Release 语义不同）
// ==============================================================================
#ifdef NDEBUG
    #define QUANT_ASSERT(cond) ((void)0)
#else
    #define QUANT_ASSERT(cond)                                     \
        do {                                                       \
            if (!(cond)) [[unlikely]] {                            \
                ::quant::detail::fatal_error(                      \
                    ::quant::ErrorCode::InternalAssertion,         \
                    "Assertion failed: " #cond,                    \
                    QUANT_CURRENT_LOCATION);                       \
            }                                                      \
        } while (0)
#endif

// ==============================================================================
// 忽略错误（显式标记 + 计数）
// ==============================================================================
#define QUANT_IGNORE_ERROR(expr)                     \
    do {                                             \
        auto _quant_result = (expr);                 \
        if (_quant_result.is_err()) [[unlikely]] {   \
            ::quant::ErrorMetrics::instance().record( \
                _quant_result.error_code());         \
        }                                            \
    } while (0)

}  // namespace quant

#endif  // QUANT_COMMON_CORE_ERROR_HPP
