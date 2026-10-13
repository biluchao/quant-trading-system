/* ---UMS-BEGIN---
spec_version: 1.0
module: common
type: core
name: types
version: 1.0.0
level: 0
required: true
author: quant-team
created: 2026-10-13
brief: 基础类型、枚举、结构体定义（全系统依赖）

provides:
  - alias Timestamp
  - alias Price
  - alias Quantity
  - alias Volume
  - alias Notional
  - enum class OrderSide
  - enum class OrderType
  - enum class TimeInForce
  - enum class OrderStatus
  - enum class Direction
  - enum class PositionSide
  - enum class MarketRegime
  - enum class StrategyState
  - struct Candle
  - struct Order
  - struct Fill
  - struct Position
  - struct Signal
  - struct IndicatorResult
  - struct RiskMetrics
  - function to_string (all enums)
  - function from_string (all enums)
  - function to_json / from_json (nlohmann ADL)

requires:
  modules: []
  libraries:
    - nlohmann_json>=3.11
  resources:
    memory_mb: 0
    cpu_cores: 0

interface:
  contract: common.L0.contract.core.json

health:
  fn: static (always healthy)
  timeout_ms: 0

assembly:
  order: 10
  parallel: false

tests:
  - tests/test_common_types.cpp

fallback: []

tags: [core, types, foundation]
---UMS-END--- */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <array>
#include <functional>
#include <nlohmann/json.hpp>

namespace quant::common {

// =============================================================================
// 基础类型别名
// =============================================================================
// 全系统统一使用以下别名，禁止直接使用原始类型，保证一致性。
// =============================================================================

using Timestamp = int64_t;    // UTC 毫秒时间戳（自 1970-01-01 00:00:00 UTC）
using Price     = double;     // IEEE-754 双精度（价格）
using Quantity  = double;     // 数量（合约张数或币数）
using Volume    = double;     // 成交量
using Notional  = double;     // 名义价值（USDT）

// 时间常量
inline constexpr Timestamp kMsPerSec      = 1000LL;
inline constexpr Timestamp kMsPerMin      = 60LL  * kMsPerSec;
inline constexpr Timestamp kMsPerHour     = 60LL  * kMsPerMin;
inline constexpr Timestamp kMsPerDay      = 24LL  * kMsPerHour;
inline constexpr Timestamp kCandleIntervalMs = 3LL * kMsPerMin;

// =============================================================================
// 枚举定义（scoped + int8_t，避免命名冲突并节省内存）
// =============================================================================

enum class OrderSide : int8_t {
    BUY  = 0,
    SELL = 1,
};

enum class OrderType : int8_t {
    LIMIT         = 0,
    MARKET        = 1,
    STOP_MARKET   = 2,
    STOP_LIMIT    = 3,
    TAKE_PROFIT   = 4,
    TRAILING_STOP = 5,
};

enum class TimeInForce : int8_t {
    GTC = 0,   // Good Till Cancel
    IOC = 1,   // Immediate Or Cancel
    FOK = 2,   // Fill Or Kill
    GTX = 3,   // Good Till Crossing (Post Only)
};

enum class OrderStatus : int8_t {
    NEW              = 0,
    PARTIALLY_FILLED = 1,
    FILLED           = 2,
    CANCELED         = 3,
    REJECTED         = 4,
    EXPIRED          = 5,
};

enum class Direction : int8_t {
    NEUTRAL = 0,
    LONG    = 1,
    SHORT   = -1,
};

enum class PositionSide : int8_t {
    FLAT  = 0,
    LONG  = 1,
    SHORT = -1,
};

enum class MarketRegime : int8_t {
    UNKNOWN    = 0,
    TREND_UP   = 1,
    TREND_DOWN = 2,
    RANGE      = 3,
    HIGH_VOL   = 4,
};

enum class StrategyState : int8_t {
    S0_EMPTY        = 0,
    S1_POSITION     = 1,
    S2_TOP_ESCAPE   = 2,
    S3_REVERSE      = 3,
    S4_ADD_POSITION = 4,
    S5_TREND_SWITCH = 5,
    S6_UNCERTAIN    = 6,
    FAULT           = 99,
};

// =============================================================================
// 枚举 <-> 字符串 转换（inline，避免 ODR 违规）
// =============================================================================

inline std::string_view to_string(OrderSide s) noexcept {
    switch (s) {
        case OrderSide::BUY:  return "BUY";
        case OrderSide::SELL: return "SELL";
    }
    return "UNKNOWN";
}

inline std::string_view to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::LIMIT:         return "LIMIT";
        case OrderType::MARKET:        return "MARKET";
        case OrderType::STOP_MARKET:   return "STOP_MARKET";
        case OrderType::STOP_LIMIT:    return "STOP_LIMIT";
        case OrderType::TAKE_PROFIT:   return "TAKE_PROFIT";
        case OrderType::TRAILING_STOP: return "TRAILING_STOP";
    }
    return "UNKNOWN";
}

inline std::string_view to_string(TimeInForce t) noexcept {
    switch (t) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTX: return "GTX";
    }
    return "UNKNOWN";
}

inline std::string_view to_string(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::NEW:              return "NEW";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::CANCELED:         return "CANCELED";
        case OrderStatus::REJECTED:         return "REJECTED";
        case OrderStatus::EXPIRED:          return "EXPIRED";
    }
    return "UNKNOWN";
}

inline std::string_view to_string(Direction d) noexcept {
    switch (d) {
        case Direction::NEUTRAL: return "NEUTRAL";
        case Direction::LONG:    return "LONG";
        case Direction::SHORT:   return "SHORT";
    }
    return "UNKNOWN";
}

inline std::string_view to_string(PositionSide p) noexcept {
    switch (p) {
        case PositionSide::FLAT:  return "FLAT";
        case PositionSide::LONG:  return "LONG";
        case PositionSide::SHORT: return "SHORT";
    }
    return "UNKNOWN";
}

inline std::string_view to_string(MarketRegime r) noexcept {
    switch (r) {
        case MarketRegime::UNKNOWN:    return "UNKNOWN";
        case MarketRegime::TREND_UP:   return "TREND_UP";
        case MarketRegime::TREND_DOWN: return "TREND_DOWN";
        case MarketRegime::RANGE:      return "RANGE";
        case MarketRegime::HIGH_VOL:   return "HIGH_VOL";
    }
    return "UNKNOWN";
}

inline std::string_view to_string(StrategyState s) noexcept {
    switch (s) {
        case StrategyState::S0_EMPTY:        return "S0_EMPTY";
        case StrategyState::S1_POSITION:     return "S1_POSITION";
        case StrategyState::S2_TOP_ESCAPE:   return "S2_TOP_ESCAPE";
        case StrategyState::S3_REVERSE:      return "S3_REVERSE";
        case StrategyState::S4_ADD_POSITION: return "S4_ADD_POSITION";
        case StrategyState::S5_TREND_SWITCH: return "S5_TREND_SWITCH";
        case StrategyState::S6_UNCERTAIN:    return "S6_UNCERTAIN";
        case StrategyState::FAULT:           return "FAULT";
    }
    return "UNKNOWN";
}

// 字符串 -> 枚举（返回 optional，失败返回 nullopt，绝不静默）
inline std::optional<OrderSide> order_side_from_string(std::string_view s) noexcept {
    if (s == "BUY")  return OrderSide::BUY;
    if (s == "SELL") return OrderSide::SELL;
    return std::nullopt;
}

inline std::optional<OrderType> order_type_from_string(std::string_view s) noexcept {
    if (s == "LIMIT")         return OrderType::LIMIT;
    if (s == "MARKET")        return OrderType::MARKET;
    if (s == "STOP_MARKET")   return OrderType::STOP_MARKET;
    if (s == "STOP_LIMIT")    return OrderType::STOP_LIMIT;
    if (s == "TAKE_PROFIT")   return OrderType::TAKE_PROFIT;
    if (s == "TRAILING_STOP") return OrderType::TRAILING_STOP;
    return std::nullopt;
}

inline std::optional<TimeInForce> tif_from_string(std::string_view s) noexcept {
    if (s == "GTC") return TimeInForce::GTC;
    if (s == "IOC") return TimeInForce::IOC;
    if (s == "FOK") return TimeInForce::FOK;
    if (s == "GTX") return TimeInForce::GTX;
    return std::nullopt;
}

inline std::optional<OrderStatus> order_status_from_string(std::string_view s) noexcept {
    if (s == "NEW")              return OrderStatus::NEW;
    if (s == "PARTIALLY_FILLED") return OrderStatus::PARTIALLY_FILLED;
    if (s == "FILLED")           return OrderStatus::FILLED;
    if (s == "CANCELED")         return OrderStatus::CANCELED;
    if (s == "REJECTED")         return OrderStatus::REJECTED;
    if (s == "EXPIRED")          return OrderStatus::EXPIRED;
    return std::nullopt;
}

inline std::optional<Direction> direction_from_string(std::string_view s) noexcept {
    if (s == "NEUTRAL") return Direction::NEUTRAL;
    if (s == "LONG")    return Direction::LONG;
    if (s == "SHORT")   return Direction::SHORT;
    return std::nullopt;
}

inline std::optional<PositionSide> position_side_from_string(std::string_view s) noexcept {
    if (s == "FLAT")  return PositionSide::FLAT;
    if (s == "LONG")  return PositionSide::LONG;
    if (s == "SHORT") return PositionSide::SHORT;
    return std::nullopt;
}

inline std::optional<MarketRegime> market_regime_from_string(std::string_view s) noexcept {
    if (s == "UNKNOWN")    return MarketRegime::UNKNOWN;
    if (s == "TREND_UP")   return MarketRegime::TREND_UP;
    if (s == "TREND_DOWN") return MarketRegime::TREND_DOWN;
    if (s == "RANGE")      return MarketRegime::RANGE;
    if (s == "HIGH_VOL")   return MarketRegime::HIGH_VOL;
    return std::nullopt;
}

inline std::optional<StrategyState> strategy_state_from_string(std::string_view s) noexcept {
    if (s == "S0_EMPTY")        return StrategyState::S0_EMPTY;
    if (s == "S1_POSITION")     return StrategyState::S1_POSITION;
    if (s == "S2_TOP_ESCAPE")   return StrategyState::S2_TOP_ESCAPE;
    if (s == "S3_REVERSE")      return StrategyState::S3_REVERSE;
    if (s == "S4_ADD_POSITION") return StrategyState::S4_ADD_POSITION;
    if (s == "S5_TREND_SWITCH") return StrategyState::S5_TREND_SWITCH;
    if (s == "S6_UNCERTAIN")    return StrategyState::S6_UNCERTAIN;
    if (s == "FAULT")           return StrategyState::FAULT;
    return std::nullopt;
}

// =============================================================================
// 核心数据结构
// =============================================================================

// K线（严格 UTC 毫秒，收盘后 is_closed=true）
struct Candle {
    Timestamp open_time{0};
    Timestamp close_time{0};
    Price     open{0.0};
    Price     high{0.0};
    Price     low{0.0};
    Price     close{0.0};
    Volume    volume{0.0};
    bool      is_closed{false};

    [[nodiscard]] bool valid() const noexcept {
        return close_time > open_time
            && high >= low
            && high >= open && high >= close
            && low  <= open && low  <= close
            && volume >= 0.0;
    }

    bool operator==(const Candle&) const = default;
};

// 订单
struct Order {
    std::string order_id;
    std::string client_order_id;
    std::string symbol;
    OrderSide   side{OrderSide::BUY};
    OrderType   type{OrderType::LIMIT};
    TimeInForce tif{TimeInForce::GTC};
    OrderStatus status{OrderStatus::NEW};
    Price       price{0.0};          // 限价（市价单为 0）
    Price       stop_price{0.0};     // 触发价（条件单）
    Quantity    quantity{0.0};
    Quantity    filled_quantity{0.0};
    Price       average_price{0.0};
    int32_t     leverage{1};
    bool        reduce_only{false};
    Timestamp   created_at{0};
    Timestamp   updated_at{0};

    bool operator==(const Order&) const = default;
};

// 成交
struct Fill {
    std::string order_id;
    std::string symbol;
    OrderSide   side{OrderSide::BUY};
    Price       price{0.0};
    Quantity    quantity{0.0};
    Notional    fee{0.0};
    Timestamp   timestamp{0};

    bool operator==(const Fill&) const = default;
};

// 持仓
struct Position {
    std::string  symbol;
    PositionSide side{PositionSide::FLAT};
    Price        entry_price{0.0};
    Quantity     quantity{0.0};
    Price        stop_loss{0.0};
    Price        take_profit{0.0};
    Notional     unrealized_pnl{0.0};
    Notional     realized_pnl{0.0};
    int32_t      leverage{1};
    Timestamp    opened_at{0};

    [[nodiscard]] bool is_open() const noexcept {
        return side != PositionSide::FLAT && quantity > 0.0;
    }

    bool operator==(const Position&) const = default;
};

// 交易信号
struct Signal {
    std::string signal_id;
    std::string symbol;
    Direction   direction{Direction::NEUTRAL};
    Price       entry_price{0.0};
    Price       stop_loss{0.0};
    Price       take_profit_1{0.0};
    Price       take_profit_2{0.0};
    Quantity    quantity{0.0};
    double      score{0.0};       // 0~100
    double      rr_ratio{0.0};    // 盈亏比
    double      confidence{0.0};  // 0~1
    Timestamp   timestamp{0};
    std::string reason;

    bool operator==(const Signal&) const = default;
};

// 指标结果
struct IndicatorResult {
    // 趋势
    Price  ema_fast{0.0};
    Price  ema_slow{0.0};
    Price  ema_slope{0.0};
    // 波动
    Price  atr14{0.0};
    Price  atr50{0.0};
    // 动量
    double rsi{50.0};
    double macd_hist{0.0};
    double macd_hist_prev1{0.0};
    double macd_hist_prev2{0.0};
    double obv_slope{0.0};
    // 量能
    double obi{0.0};
    double volume_ratio{1.0};
    // 带宽
    double upper_band{0.0};
    double lower_band{0.0};
    // 评分
    double score_net{0.0};
    double score_long{0.0};
    double score_short{0.0};
    // 时间
    Timestamp timestamp{0};

    bool operator==(const IndicatorResult&) const = default;
};

// 风控指标
struct RiskMetrics {
    Notional equity{0.0};
    Notional margin_used{0.0};
    Notional available{0.0};
    Notional daily_pnl{0.0};
    double   daily_drawdown{0.0};
    double   total_risk_pct{0.0};
    double   portfolio_risk_pct{0.0};
    Timestamp updated_at{0};

    bool operator==(const RiskMetrics&) const = default;
};

// =============================================================================
// JSON 序列化（nlohmann ADL）
// =============================================================================

inline void to_json(nlohmann::json& j, const Candle& c) {
    j = nlohmann::json{
        {"open_time",  c.open_time},
        {"close_time", c.close_time},
        {"open",       c.open},
        {"high",       c.high},
        {"low",        c.low},
        {"close",      c.close},
        {"volume",     c.volume},
        {"is_closed",  c.is_closed},
    };
}

inline void from_json(const nlohmann::json& j, Candle& c) {
    j.at("open_time").get_to(c.open_time);
    j.at("close_time").get_to(c.close_time);
    j.at("open").get_to(c.open);
    j.at("high").get_to(c.high);
    j.at("low").get_to(c.low);
    j.at("close").get_to(c.close);
    j.at("volume").get_to(c.volume);
    j.at("is_closed").get_to(c.is_closed);
}

inline void to_json(nlohmann::json& j, const Order& o) {
    j = nlohmann::json{
        {"order_id",        o.order_id},
        {"client_order_id", o.client_order_id},
        {"symbol",          o.symbol},
        {"side",            std::string(to_string(o.side))},
        {"type",            std::string(to_string(o.type))},
        {"tif",             std::string(to_string(o.tif))},
        {"status",          std::string(to_string(o.status))},
        {"price",           o.price},
        {"stop_price",      o.stop_price},
        {"quantity",        o.quantity},
        {"filled_quantity", o.filled_quantity},
        {"average_price",   o.average_price},
        {"leverage",        o.leverage},
        {"reduce_only",     o.reduce_only},
        {"created_at",      o.created_at},
        {"updated_at",      o.updated_at},
    };
}

inline void from_json(const nlohmann::json& j, Order& o) {
    j.at("order_id").get_to(o.order_id);
    j.at("client_order_id").get_to(o.client_order_id);
    j.at("symbol").get_to(o.symbol);

    auto side = order_side_from_string(j.at("side").get<std::string>());
    if (!side) throw std::invalid_argument("Invalid OrderSide");
    o.side = *side;

    auto type = order_type_from_string(j.at("type").get<std::string>());
    if (!type) throw std::invalid_argument("Invalid OrderType");
    o.type = *type;

    auto tif = tif_from_string(j.at("tif").get<std::string>());
    if (!tif) throw std::invalid_argument("Invalid TimeInForce");
    o.tif = *tif;

    auto status = order_status_from_string(j.at("status").get<std::string>());
    if (!status) throw std::invalid_argument("Invalid OrderStatus");
    o.status = *status;

    j.at("price").get_to(o.price);
    j.at("stop_price").get_to(o.stop_price);
    j.at("quantity").get_to(o.quantity);
    j.at("filled_quantity").get_to(o.filled_quantity);
    j.at("average_price").get_to(o.average_price);
    j.at("leverage").get_to(o.leverage);
    j.at("reduce_only").get_to(o.reduce_only);
    j.at("created_at").get_to(o.created_at);
    j.at("updated_at").get_to(o.updated_at);
}

inline void to_json(nlohmann::json& j, const Fill& f) {
    j = nlohmann::json{
        {"order_id",  f.order_id},
        {"symbol",    f.symbol},
        {"side",      std::string(to_string(f.side))},
        {"price",     f.price},
        {"quantity",  f.quantity},
        {"fee",       f.fee},
        {"timestamp", f.timestamp},
    };
}

inline void from_json(const nlohmann::json& j, Fill& f) {
    j.at("order_id").get_to(f.order_id);
    j.at("symbol").get_to(f.symbol);

    auto side = order_side_from_string(j.at("side").get<std::string>());
    if (!side) throw std::invalid_argument("Invalid OrderSide");
    f.side = *side;

    j.at("price").get_to(f.price);
    j.at("quantity").get_to(f.quantity);
    j.at("fee").get_to(f.fee);
    j.at("timestamp").get_to(f.timestamp);
}

inline void to_json(nlohmann::json& j, const Position& p) {
    j = nlohmann::json{
        {"symbol",         p.symbol},
        {"side",           std::string(to_string(p.side))},
        {"entry_price",    p.entry_price},
        {"quantity",       p.quantity},
        {"stop_loss",      p.stop_loss},
        {"take_profit",    p.take_profit},
        {"unrealized_pnl", p.unrealized_pnl},
        {"realized_pnl",   p.realized_pnl},
        {"leverage",       p.leverage},
        {"opened_at",      p.opened_at},
    };
}

inline void from_json(const nlohmann::json& j, Position& p) {
    j.at("symbol").get_to(p.symbol);

    auto side = position_side_from_string(j.at("side").get<std::string>());
    if (!side) throw std::invalid_argument("Invalid PositionSide");
    p.side = *side;

    j.at("entry_price").get_to(p.entry_price);
    j.at("quantity").get_to(p.quantity);
    j.at("stop_loss").get_to(p.stop_loss);
    j.at("take_profit").get_to(p.take_profit);
    j.at("unrealized_pnl").get_to(p.unrealized_pnl);
    j.at("realized_pnl").get_to(p.realized_pnl);
    j.at("leverage").get_to(p.leverage);
    j.at("opened_at").get_to(p.opened_at);
}

} // namespace quant::common

// =============================================================================
// std::hash 特化（供 unordered_map / unordered_set 使用）
// =============================================================================

namespace std {

#define QUANT_HASH_ENUM(EnumType)                                              \
    template<>                                                                 \
    struct hash<quant::common::EnumType> {                                     \
        size_t operator()(quant::common::EnumType v) const noexcept {          \
            return std::hash<int8_t>{}(static_cast<int8_t>(v));                \
        }                                                                      \
    }

QUANT_HASH_ENUM(OrderSide);
QUANT_HASH_ENUM(OrderType);
QUANT_HASH_ENUM(TimeInForce);
QUANT_HASH_ENUM(OrderStatus);
QUANT_HASH_ENUM(Direction);
QUANT_HASH_ENUM(PositionSide);
QUANT_HASH_ENUM(MarketRegime);
QUANT_HASH_ENUM(StrategyState);

#undef QUANT_HASH_ENUM

} // namespace std

// =============================================================================
// 编译期校验（ABI 保障）
// =============================================================================

namespace quant::common {

static_assert(sizeof(Timestamp) == 8, "Timestamp must be 8 bytes");
static_assert(sizeof(Price)     == 8, "Price must be 8 bytes");
static_assert(sizeof(Quantity)  == 8, "Quantity must be 8 bytes");
static_assert(sizeof(OrderSide) == 1, "OrderSide must be 1 byte");
static_assert(sizeof(OrderType) == 1, "OrderType must be 1 byte");
static_assert(sizeof(StrategyState) == 1, "StrategyState must be 1 byte");

} // namespace quant::common
