/* ---UMS-BEGIN---
spec_version: 1.0
module: kernel
type: core
name: types
version: 1.0.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: L0 内核基础类型定义 (Candle/Order/Fill/Signal/Position/IndicatorResult)

provides:
  - enum class Side
  - enum class OrderSide
  - enum class OrderType
  - enum class OrderStatus
  - enum class State
  - enum class MarketRegime
  - struct Candle
  - struct Order
  - struct Fill
  - struct Signal
  - struct Position
  - struct IndicatorResult

requires:
  modules: []
  libraries: [nlohmann_json]

interface:
  contract: kernel.L0.contract.types.json

health:
  fn: compile_time_check
  timeout_ms: 0

assembly:
  order: 1
  parallel: false

tests:
  - tests/test_kernel_types.cpp

tags: [kernel, types, foundation]
---UMS-END--- */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include <cmath>
#include <algorithm>
#include <functional>

#include <nlohmann/json.hpp>

namespace quant::kernel {

// ═══════════════════════════════════════════════════
// 版本控制
// ═══════════════════════════════════════════════════
constexpr int TYPES_VERSION = 1;

// ═══════════════════════════════════════════════════
// 强类型别名（防止单位混用）
// ═══════════════════════════════════════════════════
using Timestamp = int64_t;    // 毫秒级 Unix 时间戳(UTC)
using Price     = double;     // 价格
using Quantity  = double;     // 数量
using Notional  = double;     // 名义价值 = price * qty
using Symbol    = std::string;

// ═══════════════════════════════════════════════════
// 编译期常量
// ═══════════════════════════════════════════════════
constexpr Timestamp MS_PER_SEC  = 1000LL;
constexpr Timestamp MS_PER_MIN  = 60LL * MS_PER_SEC;
constexpr Timestamp MS_PER_3MIN = 3LL * MS_PER_MIN;
constexpr Timestamp MS_PER_5MIN = 5LL * MS_PER_MIN;
constexpr Timestamp MS_PER_HOUR = 60LL * MS_PER_MIN;
constexpr Timestamp MS_PER_DAY  = 24LL * MS_PER_HOUR;

constexpr double EPSILON = 1e-9;

// ═══════════════════════════════════════════════════
// 枚举（显式底层类型，跨平台一致）
// ═══════════════════════════════════════════════════

// 持仓方向
enum class Side : int8_t {
    LONG  =  1,
    SHORT = -1,
    NONE  =  0,
};

// 订单买卖方向
enum class OrderSide : uint8_t {
    BUY  = 0,
    SELL = 1,
};

// 订单类型
enum class OrderType : uint8_t {
    LIMIT       = 0,
    MARKET      = 1,
    STOP_MARKET = 2,
    STOP_LIMIT  = 3,
};

// 订单状态
enum class OrderStatus : uint8_t {
    NEW              = 0,
    PARTIALLY_FILLED = 1,
    FILLED           = 2,
    CANCELED         = 3,
    REJECTED         = 4,
    EXPIRED          = 5,
};

// 策略状态机
enum class State : uint8_t {
    S0_EMPTY         = 0,   // 空仓观察
    S1_POSITION      = 1,   // 顺势持仓
    S2_ESCAPE_WATCH  = 2,   // 逃顶/逃底后观察
    S3_REVERSE       = 3,   // 反向持仓
    S4_ADD           = 4,   // 趋势延续加仓
    S5_TREND_SWITCH  = 5,   // 趋势切换
    S6_UNCERTAIN     = 6,   // 不确定状态
    FAULT            = 255,
};

// 市场状态（由 AI 判定）
enum class MarketRegime : uint8_t {
    TREND_UP   = 0,
    TREND_DOWN = 1,
    RANGE      = 2,
    HIGH_VOL   = 3,
    UNKNOWN    = 255,
};

// ═══════════════════════════════════════════════════
// Candle（3m/5m K线）
// ═══════════════════════════════════════════════════
struct alignas(64) Candle {
    Timestamp open_time  = 0;
    Timestamp close_time = 0;

    Price    open   = 0.0;
    Price    high   = 0.0;
    Price    low    = 0.0;
    Price    close  = 0.0;
    Quantity volume = 0.0;

    bool     is_closed = false;   // true 表示已收盘(策略只处理已收盘)

    // 校验
    [[nodiscard]] bool is_valid() const noexcept {
        if (open_time <= 0 || close_time <= open_time) return false;
        if (!std::isfinite(open) || !std::isfinite(high) ||
            !std::isfinite(low)  || !std::isfinite(close) ||
            !std::isfinite(volume)) return false;
        if (high < low) return false;
        if (open > high + EPSILON || open < low - EPSILON) return false;
        if (close > high + EPSILON || close < low - EPSILON) return false;
        if (volume < 0.0) return false;
        return true;
    }

    [[nodiscard]] Price range() const noexcept { return high - low; }
    [[nodiscard]] Price change() const noexcept { return close - open; }
    [[nodiscard]] double change_pct() const noexcept {
        return open > EPSILON ? (close - open) / open : 0.0;
    }
    [[nodiscard]] int direction() const noexcept {
        if (close > open + EPSILON) return 1;
        if (close < open - EPSILON) return -1;
        return 0;
    }
};

// ═══════════════════════════════════════════════════
// Order（订单）
// ═══════════════════════════════════════════════════
struct alignas(64) Order {
    std::string order_id;         // 系统内唯一ID
    std::string client_order_id;  // 幂等键
    Symbol      symbol;

    OrderSide   side   = OrderSide::BUY;
    OrderType   type   = OrderType::LIMIT;
    OrderStatus status = OrderStatus::NEW;

    Price    price      = 0.0;    // 限价单价格
    Price    stop_price = 0.0;    // 条件单触发价
    Quantity quantity   = 0.0;    // 委托数量
    Quantity filled_qty = 0.0;    // 已成交
    Price    avg_price  = 0.0;    // 成交均价

    int      leverage    = 1;
    bool     reduce_only = false; // 只减仓（止损单必用）

    Timestamp created_at = 0;
    Timestamp updated_at = 0;

    [[nodiscard]] bool is_valid() const noexcept {
        if (symbol.empty()) return false;
        if (!std::isfinite(quantity) || quantity <= 0.0) return false;
        if (!std::isfinite(filled_qty) || filled_qty < 0.0) return false;
        if (filled_qty > quantity + EPSILON) return false;
        if (leverage < 1 || leverage > 125) return false;
        if (type == OrderType::LIMIT && price <= 0.0) return false;
        if ((type == OrderType::STOP_MARKET || type == OrderType::STOP_LIMIT)
            && stop_price <= 0.0) return false;
        return true;
    }

    [[nodiscard]] Quantity remaining() const noexcept {
        return std::max(0.0, quantity - filled_qty);
    }
};

// ═══════════════════════════════════════════════════
// Fill（成交记录）
// ═══════════════════════════════════════════════════
struct Fill {
    std::string order_id;
    Symbol      symbol;
    OrderSide   side     = OrderSide::BUY;
    Price       price    = 0.0;
    Quantity    quantity = 0.0;
    Notional    fee      = 0.0;
    Timestamp   timestamp = 0;

    [[nodiscard]] bool is_valid() const noexcept {
        return !symbol.empty() && price > 0.0 && quantity > 0.0 &&
               std::isfinite(fee) && timestamp > 0;
    }
};

// ═══════════════════════════════════════════════════
// Signal（策略信号）
// ═══════════════════════════════════════════════════
struct Signal {
    std::string signal_id;
    Symbol      symbol;
    Side        side = Side::NONE;

    Price    entry_price = 0.0;
    Price    stop_loss   = 0.0;
    Quantity quantity    = 0.0;

    double    score = 0.0;       // [-100, 100] 带符号
    Timestamp timestamp = 0;
    std::string reason;

    [[nodiscard]] bool is_valid() const noexcept {
        if (side == Side::NONE) return false;
        if (!std::isfinite(entry_price) || entry_price <= 0.0) return false;
        if (!std::isfinite(stop_loss)   || stop_loss   <= 0.0) return false;
        if (!std::isfinite(quantity)    || quantity    <= 0.0) return false;
        // 多头止损必须低于入场价
        if (side == Side::LONG  && stop_loss >= entry_price) return false;
        // 空头止损必须高于入场价
        if (side == Side::SHORT && stop_loss <= entry_price) return false;
        return true;
    }
};

// ═══════════════════════════════════════════════════
// Position（持仓）
// ═══════════════════════════════════════════════════
struct Position {
    Symbol      symbol;
    Side        side      = Side::NONE;
    Price       avg_cost  = 0.0;
    Quantity    qty       = 0.0;
    Price       stop_loss = 0.0;

    Timestamp   entry_time = 0;
    int         add_count  = 0;

    [[nodiscard]] bool is_open() const noexcept {
        return side != Side::NONE && qty > EPSILON;
    }

    [[nodiscard]] double unrealized_pnl(Price current) const noexcept {
        if (!is_open()) return 0.0;
        if (side == Side::LONG)  return (current - avg_cost) * qty;
        if (side == Side::SHORT) return (avg_cost - current) * qty;
        return 0.0;
    }
};

// ═══════════════════════════════════════════════════
// IndicatorResult（指标结果）
// ═══════════════════════════════════════════════════
struct IndicatorResult {
    Timestamp timestamp = 0;
    Symbol    symbol;

    Price    close       = 0.0;
    double   ema_fast    = 0.0;
    double   ema_slow    = 0.0;
    double   ema_slope   = 0.0;
    double   atr14       = 0.0;
    double   atr50       = 0.0;
    double   rsi         = 0.0;
    double   macd_hist   = 0.0;
    double   volume_ratio = 1.0;
    double   obi         = 0.0;
    double   upper_band  = 0.0;
    double   lower_band  = 0.0;
    double   band_width  = 0.0;
    double   score       = 0.0;   // [-100, 100]

    [[nodiscard]] bool is_valid() const noexcept {
        return std::isfinite(ema_slow) &&
               std::isfinite(atr14) && atr14 > 0.0 &&
               std::isfinite(rsi) && rsi >= 0.0 && rsi <= 100.0 &&
               std::isfinite(score) && score >= -100.0 && score <= 100.0;
    }
};

// ═══════════════════════════════════════════════════
// 枚举转字符串（日志友好）
// ═══════════════════════════════════════════════════
[[nodiscard]] inline const char* to_string(Side s) noexcept {
    switch (s) {
        case Side::LONG:  return "LONG";
        case Side::SHORT: return "SHORT";
        default:          return "NONE";
    }
}

[[nodiscard]] inline const char* to_string(OrderStatus s) noexcept {
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

[[nodiscard]] inline const char* to_string(State s) noexcept {
    switch (s) {
        case State::S0_EMPTY:        return "S0_EMPTY";
        case State::S1_POSITION:     return "S1_POSITION";
        case State::S2_ESCAPE_WATCH: return "S2_ESCAPE_WATCH";
        case State::S3_REVERSE:      return "S3_REVERSE";
        case State::S4_ADD:          return "S4_ADD";
        case State::S5_TREND_SWITCH: return "S5_TREND_SWITCH";
        case State::S6_UNCERTAIN:    return "S6_UNCERTAIN";
        case State::FAULT:           return "FAULT";
    }
    return "UNKNOWN";
}

[[nodiscard]] inline const char* to_string(MarketRegime r) noexcept {
    switch (r) {
        case MarketRegime::TREND_UP:   return "TREND_UP";
        case MarketRegime::TREND_DOWN: return "TREND_DOWN";
        case MarketRegime::RANGE:      return "RANGE";
        case MarketRegime::HIGH_VOL:   return "HIGH_VOL";
        default:                       return "UNKNOWN";
    }
}

// ═══════════════════════════════════════════════════
// JSON 序列化（与前端通信）
// ═══════════════════════════════════════════════════
inline void to_json(nlohmann::json& j, const Candle& c) {
    j = nlohmann::json{
        {"open_time", c.open_time}, {"close_time", c.close_time},
        {"open", c.open}, {"high", c.high},
        {"low", c.low},   {"close", c.close},
        {"volume", c.volume}, {"is_closed", c.is_closed}
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
        {"order_id", o.order_id},
        {"client_order_id", o.client_order_id},
        {"symbol", o.symbol},
        {"side",   static_cast<int>(o.side)},
        {"type",   static_cast<int>(o.type)},
        {"status", static_cast<int>(o.status)},
        {"price", o.price}, {"stop_price", o.stop_price},
        {"quantity", o.quantity}, {"filled_qty", o.filled_qty},
        {"avg_price", o.avg_price},
        {"leverage", o.leverage}, {"reduce_only", o.reduce_only},
        {"created_at", o.created_at}, {"updated_at", o.updated_at}
    };
}

inline void from_json(const nlohmann::json& j, Order& o) {
    j.at("order_id").get_to(o.order_id);
    j.at("client_order_id").get_to(o.client_order_id);
    j.at("symbol").get_to(o.symbol);
    o.side   = static_cast<OrderSide>(j.at("side").get<int>());
    o.type   = static_cast<OrderType>(j.at("type").get<int>());
    o.status = static_cast<OrderStatus>(j.at("status").get<int>());
    j.at("price").get_to(o.price);
    j.at("stop_price").get_to(o.stop_price);
    j.at("quantity").get_to(o.quantity);
    j.at("filled_qty").get_to(o.filled_qty);
    j.at("avg_price").get_to(o.avg_price);
    j.at("leverage").get_to(o.leverage);
    j.at("reduce_only").get_to(o.reduce_only);
    j.at("created_at").get_to(o.created_at);
    j.at("updated_at").get_to(o.updated_at);
}

inline void to_json(nlohmann::json& j, const Signal& s) {
    j = nlohmann::json{
        {"signal_id", s.signal_id},
        {"symbol", s.symbol},
        {"side", static_cast<int>(s.side)},
        {"entry_price", s.entry_price},
        {"stop_loss", s.stop_loss},
        {"quantity", s.quantity},
        {"score", s.score},
        {"timestamp", s.timestamp},
        {"reason", s.reason}
    };
}

inline void to_json(nlohmann::json& j, const IndicatorResult& r) {
    j = nlohmann::json{
        {"timestamp", r.timestamp}, {"symbol", r.symbol},
        {"close", r.close},
        {"ema_fast", r.ema_fast}, {"ema_slow", r.ema_slow},
        {"ema_slope", r.ema_slope},
        {"atr14", r.atr14}, {"atr50", r.atr50},
        {"rsi", r.rsi}, {"macd_hist", r.macd_hist},
        {"volume_ratio", r.volume_ratio}, {"obi", r.obi},
        {"upper_band", r.upper_band}, {"lower_band", r.lower_band},
        {"band_width", r.band_width}, {"score", r.score}
    };
}

} // namespace quant::kernel

// ═══════════════════════════════════════════════════
// std::hash 特化（用于容器键）
// ═══════════════════════════════════════════════════
namespace std {
    template<> struct hash<quant::kernel::Candle> {
        size_t operator()(const quant::kernel::Candle& c) const noexcept {
            return std::hash<int64_t>{}(c.open_time) ^
                   (std::hash<double>{}(c.close) << 1);
        }
    };
}
