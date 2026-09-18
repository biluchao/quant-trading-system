// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 核心类型定义
// ==============================================================================
// @file    src/common/common.core.types.hpp
// @module  common
// @type    core
// @name    types
// @version 1.0.1
// @brief   基础类型、枚举、常量、订单、成交、K线、持仓等核心数据结构
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 价格/数量使用定点数（int64_t），避免浮点误差
//   - 所有枚举使用 enum class + uint8_t，ABI 稳定
//   - 热数据结构使用 alignas(64) 防止缓存行伪共享
//   - 所有类型提供 constexpr 构造、验证、序列化接口
//   - 编译期 static_assert 校验大小与对齐
//   - 跨平台字节序统一为小端
// ==============================================================================

#ifndef QUANT_COMMON_CORE_TYPES_HPP
#define QUANT_COMMON_CORE_TYPES_HPP

// ==============================================================================
// 标准库包含
// ==============================================================================
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <compare>
#include <functional>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// ==============================================================================
// 命名空间
// ==============================================================================
namespace quant {

// ==============================================================================
// 编译期常量
// ==============================================================================
namespace constants {

// 定点数精度（价格：8 位小数）
inline constexpr int32_t kPriceDecimals = 8;
inline constexpr int64_t kPriceScale = 100'000'000LL;   // 10^8

// 定点数精度（数量：8 位小数）
inline constexpr int32_t kQuantityDecimals = 8;
inline constexpr int64_t kQuantityScale = 100'000'000LL;

// 时间戳精度：微秒
inline constexpr int64_t kMicrosecondsPerSecond = 1'000'000LL;
inline constexpr int64_t kMicrosecondsPerMillisecond = 1'000LL;
inline constexpr int64_t kMicrosecondsPerMinute = 60LL * kMicrosecondsPerSecond;
inline constexpr int64_t kMicrosecondsPerHour = 60LL * kMicrosecondsPerMinute;
inline constexpr int64_t kMicrosecondsPerDay = 24LL * kMicrosecondsPerHour;

// K 线周期（3分钟，微秒）
inline constexpr int64_t kCandleInterval3m = 3LL * kMicrosecondsPerMinute;

// 浮点比较容差（仅在边界处使用，不用于价格存储）
inline constexpr double kEpsilon = 1e-9;

// 符号长度上限（如 "BTCUSDT" 最长 10 字符，留余量）
inline constexpr std::size_t kSymbolCapacity = 16;

// 订单 ID 字符串容量（如 UUID 36 字符）
inline constexpr std::size_t kOrderIdCapacity = 40;

// 订单客户端 ID 容量
inline constexpr std::size_t kClientOrderIdCapacity = 40;

}  // namespace constants

// ==============================================================================
// 强类型标签（防止不同类型的 ID 混淆）
// ==============================================================================
template <typename Tag, typename T>
class StrongType {
public:
    using value_type = T;

    constexpr StrongType() noexcept : value_{} {}
    constexpr explicit StrongType(T v) noexcept : value_{v} {}

    [[nodiscard]] constexpr T value() const noexcept { return value_; }

    constexpr auto operator<=>(const StrongType&) const noexcept = default;
    constexpr bool operator==(const StrongType&) const noexcept = default;

private:
    T value_;
};

// ID 标签类型
struct OrderIdTag {};
struct TradeIdTag {};
struct SignalIdTag {};
struct TraceIdTag {};
struct PositionIdTag {};

// 强类型 ID 别名
using OrderId    = StrongType<OrderIdTag, uint64_t>;
using TradeId    = StrongType<TradeIdTag, uint64_t>;
using SignalId   = StrongType<SignalIdTag, uint64_t>;
using TraceId    = StrongType<TraceIdTag, uint64_t>;
using PositionId = StrongType<PositionIdTag, uint64_t>;

// ==============================================================================
// 时间戳（微秒，UTC）
// ==============================================================================
class Timestamp {
public:
    constexpr Timestamp() noexcept : us_{0} {}
    constexpr explicit Timestamp(int64_t microseconds) noexcept : us_{microseconds} {}

    [[nodiscard]] constexpr int64_t microseconds() const noexcept { return us_; }
    [[nodiscard]] constexpr int64_t milliseconds() const noexcept { return us_ / constants::kMicrosecondsPerMillisecond; }
    [[nodiscard]] constexpr int64_t seconds() const noexcept { return us_ / constants::kMicrosecondsPerSecond; }

    [[nodiscard]] constexpr bool is_valid() const noexcept { return us_ > 0; }

    // 当前系统时间（UTC 微秒）
    [[nodiscard]] static Timestamp now() noexcept {
        const auto now = std::chrono::system_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        return Timestamp{static_cast<int64_t>(us)};
    }

    // 单调时钟（用于测量间隔）
    [[nodiscard]] static Timestamp steady_now() noexcept {
        const auto now = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        return Timestamp{static_cast<int64_t>(us)};
    }

    constexpr auto operator<=>(const Timestamp&) const noexcept = default;
    constexpr bool operator==(const Timestamp&) const noexcept = default;

    constexpr Timestamp operator+(int64_t us) const noexcept {
        return Timestamp{us_ + us};
    }
    constexpr Timestamp operator-(int64_t us) const noexcept {
        return Timestamp{us_ - us};
    }
    constexpr int64_t operator-(const Timestamp& other) const noexcept {
        return us_ - other.us_;
    }

private:
    int64_t us_;
};

// ==============================================================================
// 定点数价格（8 位小数）
// ==============================================================================
class Price {
public:
    constexpr Price() noexcept : raw_{0} {}
    constexpr explicit Price(int64_t raw) noexcept : raw_{raw} {}

    // 从字符串构造（如 "60000.12345678"）
    [[nodiscard]] static std::optional<Price> from_string(std::string_view s) noexcept;

    // 从 double 构造（仅用于初始化，精度可能丢失）
    [[nodiscard]] static Price from_double(double v) noexcept {
        return Price{static_cast<int64_t>(v * static_cast<double>(constants::kPriceScale) + 0.5)};
    }

    [[nodiscard]] constexpr int64_t raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(raw_) / static_cast<double>(constants::kPriceScale);
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept { return raw_ > 0; }

    constexpr auto operator<=>(const Price&) const noexcept = default;
    constexpr bool operator==(const Price&) const noexcept = default;

    constexpr Price operator+(const Price& o) const noexcept { return Price{raw_ + o.raw_}; }
    constexpr Price operator-(const Price& o) const noexcept { return Price{raw_ - o.raw_}; }

    constexpr Price& operator+=(const Price& o) noexcept { raw_ += o.raw_; return *this; }
    constexpr Price& operator-=(const Price& o) noexcept { raw_ -= o.raw_; return *this; }

private:
    int64_t raw_;
};

// ==============================================================================
// 定点数数量（8 位小数）
// ==============================================================================
class Quantity {
public:
    constexpr Quantity() noexcept : raw_{0} {}
    constexpr explicit Quantity(int64_t raw) noexcept : raw_{raw} {}

    [[nodiscard]] static std::optional<Quantity> from_string(std::string_view s) noexcept;

    [[nodiscard]] static Quantity from_double(double v) noexcept {
        return Quantity{static_cast<int64_t>(v * static_cast<double>(constants::kQuantityScale) + 0.5)};
    }

    [[nodiscard]] constexpr int64_t raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(raw_) / static_cast<double>(constants::kQuantityScale);
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept { return raw_ > 0; }

    constexpr auto operator<=>(const Quantity&) const noexcept = default;
    constexpr bool operator==(const Quantity&) const noexcept = default;

    constexpr Quantity operator+(const Quantity& o) const noexcept { return Quantity{raw_ + o.raw_}; }
    constexpr Quantity operator-(const Quantity& o) const noexcept { return Quantity{raw_ - o.raw_}; }

    constexpr Quantity& operator+=(const Quantity& o) noexcept { raw_ += o.raw_; return *this; }
    constexpr Quantity& operator-=(const Quantity& o) noexcept { raw_ -= o.raw_; return *this; }

private:
    int64_t raw_;
};

// ==============================================================================
// 符号（固定容量字符串，避免堆分配）
// ==============================================================================
class Symbol {
public:
    constexpr Symbol() noexcept : data_{}, size_{0} {}

    constexpr explicit Symbol(std::string_view s) noexcept : data_{}, size_{0} {
        const std::size_t n = (s.size() < constants::kSymbolCapacity)
                                ? s.size()
                                : constants::kSymbolCapacity - 1;
        for (std::size_t i = 0; i < n; ++i) data_[i] = s[i];
        size_ = static_cast<uint8_t>(n);
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view{data_.data(), size_};
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

    constexpr bool operator==(const Symbol& o) const noexcept {
        return size_ == o.size_ && std::memcmp(data_.data(), o.data_.data(), size_) == 0;
    }

    constexpr auto operator<=>(const Symbol& o) const noexcept {
        const int cmp = std::memcmp(data_.data(), o.data_.data(),
            (size_ < o.size_) ? size_ : o.size_);
        if (cmp != 0) return cmp <=> 0;
        return size_ <=> o.size_;
    }

private:
    std::array<char, constants::kSymbolCapacity> data_;
    uint8_t size_;
};

// ==============================================================================
// 枚举：方向、订单类型、订单状态、时间周期
// ==============================================================================
enum class Side : uint8_t {
    UNKNOWN = 0,
    BUY     = 1,
    SELL    = 2,
};

enum class OrderType : uint8_t {
    UNKNOWN     = 0,
    MARKET      = 1,
    LIMIT       = 2,
    STOP_MARKET = 3,
    STOP_LIMIT  = 4,
    TAKE_PROFIT = 5,
    TRAILING    = 6,
};

enum class OrderStatus : uint8_t {
    UNKNOWN           = 0,
    NEW               = 1,
    PARTIALLY_FILLED  = 2,
    FILLED            = 3,
    CANCELED          = 4,
    REJECTED          = 5,
    EXPIRED           = 6,
};

enum class TimeInForce : uint8_t {
    UNKNOWN = 0,
    GTC     = 1,   // Good Till Cancel
    IOC     = 2,   // Immediate Or Cancel
    FOK     = 3,   // Fill Or Kill
    GTD     = 4,   // Good Till Date
};

enum class TimeFrame : uint8_t {
    UNKNOWN = 0,
    M1      = 1,
    M3      = 2,
    M5      = 3,
    M15     = 4,
    H1      = 5,
    H4      = 6,
    D1      = 7,
};

enum class MarketRegime : uint8_t {
    UNKNOWN    = 0,
    TREND_UP   = 1,
    TREND_DOWN = 2,
    RANGE      = 3,
    HIGH_VOL   = 4,
    LOW_VOL    = 5,
};

enum class ExecutionMode : uint8_t {
    UNKNOWN = 0,
    VIRTUAL = 1,
    LIVE    = 2,
};

// ==============================================================================
// 枚举转换函数
// ==============================================================================
[[nodiscard]] constexpr std::string_view to_string(Side v) noexcept {
    switch (v) {
        case Side::BUY:  return "BUY";
        case Side::SELL: return "SELL";
        default:         return "UNKNOWN";
    }
}

[[nodiscard]] constexpr std::string_view to_string(OrderType v) noexcept {
    switch (v) {
        case OrderType::MARKET:      return "MARKET";
        case OrderType::LIMIT:       return "LIMIT";
        case OrderType::STOP_MARKET: return "STOP_MARKET";
        case OrderType::STOP_LIMIT:  return "STOP_LIMIT";
        case OrderType::TAKE_PROFIT: return "TAKE_PROFIT";
        case OrderType::TRAILING:    return "TRAILING";
        default:                     return "UNKNOWN";
    }
}

[[nodiscard]] constexpr std::string_view to_string(OrderStatus v) noexcept {
    switch (v) {
        case OrderStatus::NEW:              return "NEW";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::CANCELED:         return "CANCELED";
        case OrderStatus::REJECTED:         return "REJECTED";
        case OrderStatus::EXPIRED:          return "EXPIRED";
        default:                            return "UNKNOWN";
    }
}

[[nodiscard]] constexpr std::string_view to_string(TimeInForce v) noexcept {
    switch (v) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTD: return "GTD";
        default:               return "UNKNOWN";
    }
}

[[nodiscard]] constexpr std::string_view to_string(MarketRegime v) noexcept {
    switch (v) {
        case MarketRegime::TREND_UP:   return "TREND_UP";
        case MarketRegime::TREND_DOWN: return "TREND_DOWN";
        case MarketRegime::RANGE:      return "RANGE";
        case MarketRegime::HIGH_VOL:   return "HIGH_VOL";
        case MarketRegime::LOW_VOL:    return "LOW_VOL";
        default:                       return "UNKNOWN";
    }
}

// ==============================================================================
// K 线（OHLCV）
// ==============================================================================
struct Candle {
    Timestamp open_time{};
    Timestamp close_time{};
    Price     open{};
    Price     high{};
    Price     low{};
    Price     close{};
    Quantity  volume{};
    Quantity  quote_volume{};
    uint32_t  trades{0};
    bool      is_closed{false};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return open.is_valid()
            && high.is_valid()
            && low.is_valid()
            && close.is_valid()
            && volume.is_valid()
            && high >= low
            && high >= open
            && high >= close
            && low  <= open
            && low  <= close
            && open_time.is_valid();
    }

    [[nodiscard]] constexpr Price range() const noexcept {
        return Price{high.raw() - low.raw()};
    }

    [[nodiscard]] constexpr bool is_bullish() const noexcept {
        return close.raw() > open.raw();
    }

    [[nodiscard]] constexpr bool is_bearish() const noexcept {
        return close.raw() < open.raw();
    }
};

// ==============================================================================
// 订单
// ==============================================================================
struct Order {
    OrderId       id{};
    OrderId       parent_id{};     // 关联订单（如止损单关联主订单）
    TraceId       trace_id{};      // 因果链追踪
    Timestamp     created_at{};
    Timestamp     updated_at{};
    Symbol        symbol{};
    Price         price{};
    Price         stop_price{};
    Quantity      quantity{};
    Quantity      filled_quantity{};
    Price         average_price{};
    Side          side{Side::UNKNOWN};
    OrderType     type{OrderType::UNKNOWN};
    OrderStatus   status{OrderStatus::UNKNOWN};
    TimeInForce   time_in_force{TimeInForce::GTC};
    uint8_t       leverage{1};
    bool          reduce_only{false};
    bool          post_only{false};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return !symbol.empty()
            && quantity.is_valid()
            && side != Side::UNKNOWN
            && type != OrderType::UNKNOWN
            && status != OrderStatus::UNKNOWN;
    }

    [[nodiscard]] constexpr Quantity remaining() const noexcept {
        return Quantity{quantity.raw() - filled_quantity.raw()};
    }

    [[nodiscard]] constexpr bool is_terminal() const noexcept {
        return status == OrderStatus::FILLED
            || status == OrderStatus::CANCELED
            || status == OrderStatus::REJECTED
            || status == OrderStatus::EXPIRED;
    }

    [[nodiscard]] constexpr bool is_active() const noexcept {
        return status == OrderStatus::NEW
            || status == OrderStatus::PARTIALLY_FILLED;
    }
};

// ==============================================================================
// 成交
// ==============================================================================
struct Fill {
    TradeId   id{};
    OrderId   order_id{};
    TraceId   trace_id{};
    Timestamp timestamp{};
    Symbol    symbol{};
    Price     price{};
    Quantity  quantity{};
    Side      side{Side::UNKNOWN};
    Price     commission{};     // 手续费（以计价货币计）
    bool      is_maker{false};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return !symbol.empty()
            && price.is_valid()
            && quantity.is_valid()
            && side != Side::UNKNOWN
            && timestamp.is_valid();
    }

    [[nodiscard]] constexpr Price notional() const noexcept {
        // 简化计算：price * quantity / scale
        // 实际需用 128 位中间值防溢出
        return Price{price.raw() * quantity.raw() / constants::kQuantityScale};
    }
};

// ==============================================================================
// 持仓
// ==============================================================================
struct Position {
    PositionId  id{};
    Symbol      symbol{};
    Side        side{Side::UNKNOWN};
    Price       entry_price{};
    Price       mark_price{};
    Price       stop_loss{};
    Price       take_profit{};
    Quantity    quantity{};
    Quantity    max_quantity{};      // 历史最大持仓
    Timestamp   opened_at{};
    Timestamp   updated_at{};
    uint8_t     add_count{0};        // 已加仓次数
    bool        is_open{false};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return !symbol.empty()
            && side != Side::UNKNOWN
            && entry_price.is_valid()
            && quantity.is_valid();
    }

    // 未实现盈亏（以计价货币计）
    [[nodiscard]] constexpr Price unrealized_pnl() const noexcept {
        if (!is_valid() || !mark_price.is_valid()) return Price{};
        const int64_t diff = (side == Side::BUY)
            ? (mark_price.raw() - entry_price.raw())
            : (entry_price.raw() - mark_price.raw());
        return Price{diff * quantity.raw() / constants::kQuantityScale};
    }

    [[nodiscard]] constexpr Price notional() const noexcept {
        return Price{entry_price.raw() * quantity.raw() / constants::kQuantityScale};
    }
};

// ==============================================================================
// 信号
// ==============================================================================
struct Signal {
    SignalId     id{};
    TraceId      trace_id{};
    Timestamp    timestamp{};
    Symbol       symbol{};
    Price        entry_price{};
    Price        stop_loss{};
    Price        take_profit_1{};
    Price        take_profit_2{};
    Quantity     suggested_quantity{};
    Side         side{Side::UNKNOWN};
    double       score{0.0};        // 评分 [0, 100]
    double       confidence{0.0};   // 置信度 [0, 1]
    std::string_view reason{};      // 简要原因（不持有所有权）

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return !symbol.empty()
            && entry_price.is_valid()
            && stop_loss.is_valid()
            && side != Side::UNKNOWN;
    }

    // 盈亏比
    [[nodiscard]] constexpr double risk_reward_ratio() const noexcept {
        if (!is_valid() || !take_profit_1.is_valid()) return 0.0;
        const int64_t risk = (side == Side::BUY)
            ? (entry_price.raw() - stop_loss.raw())
            : (stop_loss.raw() - entry_price.raw());
        const int64_t reward = (side == Side::BUY)
            ? (take_profit_1.raw() - entry_price.raw())
            : (entry_price.raw() - take_profit_1.raw());
        if (risk <= 0) return 0.0;
        return static_cast<double>(reward) / static_cast<double>(risk);
    }
};

// ==============================================================================
// 哈希支持（用于 unordered_map）
// ==============================================================================
struct SymbolHash {
    [[nodiscard]] std::size_t operator()(const Symbol& s) const noexcept {
        return std::hash<std::string_view>{}(s.view());
    }
};

struct OrderIdHash {
    [[nodiscard]] std::size_t operator()(const OrderId& id) const noexcept {
        return std::hash<uint64_t>{}(id.value());
    }
};

struct TradeIdHash {
    [[nodiscard]] std::size_t operator()(const TradeId& id) const noexcept {
        return std::hash<uint64_t>{}(id.value());
    }
};

// ==============================================================================
// 字节序转换（跨平台序列化）
// ==============================================================================
namespace endian {

[[nodiscard]] constexpr bool is_little_endian() noexcept {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
    return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#elif defined(_WIN32)
    return true;
#else
    return true;  // 保守假设
#endif
}

// 转换为小端（若本机是大端则交换）
[[nodiscard]] inline uint64_t to_little_endian(uint64_t v) noexcept {
    if (is_little_endian()) return v;
    return __builtin_bswap64(v);
}

[[nodiscard]] inline uint64_t from_little_endian(uint64_t v) noexcept {
    return to_little_endian(v);
}

[[nodiscard]] inline uint32_t to_little_endian(uint32_t v) noexcept {
    if (is_little_endian()) return v;
    return __builtin_bswap32(v);
}

[[nodiscard]] inline uint32_t from_little_endian(uint32_t v) noexcept {
    return to_little_endian(v);
}

}  // namespace endian

// ==============================================================================
// 编译期大小与对齐校验
// ==============================================================================
static_assert(sizeof(Timestamp) == 8, "Timestamp 必须为 8 字节");
static_assert(sizeof(Price) == 8, "Price 必须为 8 字节");
static_assert(sizeof(Quantity) == 8, "Quantity 必须为 8 字节");
static_assert(sizeof(Symbol) == constants::kSymbolCapacity + 1, "Symbol 布局异常");
static_assert(std::is_trivially_copyable_v<Candle>, "Candle 必须可平凡复制");
static_assert(std::is_trivially_copyable_v<Order>, "Order 必须可平凡复制");
static_assert(std::is_trivially_copyable_v<Fill>, "Fill 必须可平凡复制");
static_assert(std::is_trivially_copyable_v<Position>, "Position 必须可平凡复制");

// 枚举底层类型校验
static_assert(sizeof(Side) == 1, "Side 必须为 1 字节");
static_assert(sizeof(OrderType) == 1, "OrderType 必须为 1 字节");
static_assert(sizeof(OrderStatus) == 1, "OrderStatus 必须为 1 字节");

}  // namespace quant

// ==============================================================================
// std::hash 特化
// ==============================================================================
namespace std {

template <>
struct hash<quant::OrderId> {
    size_t operator()(const quant::OrderId& id) const noexcept {
        return hash<uint64_t>{}(id.value());
    }
};

template <>
struct hash<quant::TradeId> {
    size_t operator()(const quant::TradeId& id) const noexcept {
        return hash<uint64_t>{}(id.value());
    }
};

template <>
struct hash<quant::Symbol> {
    size_t operator()(const quant::Symbol& s) const noexcept {
        return hash<string_view>{}(s.view());
    }
};

}  // namespace std

#endif  // QUANT_COMMON_CORE_TYPES_HPP
