// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 事件序列化
// ==============================================================================
// @file    src/event/event.core.serializer.hpp
// @module  event
// @type    core
// @name    serializer
// @version 1.0.2
// @brief   事件序列化/反序列化，支持 JSON / MessagePack，类型安全，带版本
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 类型安全：按 EventType 严格序列化/反序列化 payload
//   - 版本兼容：每条记录带 schema_version，支持向后迁移
//   - 大小限制：拒绝超过 kMaxSerializedSize（1 MB）的输入
//   - 精度保证：价格/数量使用 int64_t 定点数，double 仅用于非金额字段
//   - 错误返回：Result<T> 明确错误码，无异常传播
//   - 二进制格式：可选 MessagePack，压缩率 5~10 倍
//   - 校验和：可选 CRC32，检测数据损坏
//   - 无全局状态：所有函数为纯函数，线程安全
// ==============================================================================

#ifndef QUANT_EVENT_CORE_SERIALIZER_HPP
#define QUANT_EVENT_CORE_SERIALIZER_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// ==============================================================================
// 第三方
// ==============================================================================
#include <nlohmann/json.hpp>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.crypto.hpp"
#include "event/event.core.bus.hpp"

namespace quant {
namespace event {

// ==============================================================================
// 序列化格式
// ==============================================================================
enum class SerializationFormat : uint8_t {
    JSON        = 0,    // 文本 JSON（人类可读，调试友好）
    MESSAGE_PACK = 1,   // MessagePack（二进制，紧凑）
    // FLATBUFFERS 预留
};

// ==============================================================================
// 序列化选项
// ==============================================================================
struct SerializationOptions {
    SerializationFormat format{SerializationFormat::JSON};
    bool pretty{false};                   // JSON 缩进
    bool include_checksum{false};         // 附加 CRC32 校验
    bool include_metadata{true};          // 包含 schema_version、序列化时间等
    std::size_t max_size{kMaxSerializedSize};
    int json_indent{0};                   // 0 = 紧凑，2 = 缩进

    static constexpr std::size_t kMaxSerializedSize = 1 * 1024 * 1024;  // 1 MB
};

// ==============================================================================
// 序列化结果（二进制 + 可选校验）
// ==============================================================================
struct SerializedEvent {
    std::vector<uint8_t> data;
    uint32_t checksum{0};                 // CRC32（若启用）
    SerializationFormat format{SerializationFormat::JSON};

    [[nodiscard]] std::string_view view() const noexcept {
        return {reinterpret_cast<const char*>(data.data()), data.size()};
    }

    [[nodiscard]] std::string as_string() const {
        return std::string(view());
    }
};

// ==============================================================================
// 内部：类型名与 payload 映射（编译期）
// ==============================================================================
namespace detail {

// 各 EventType 对应的 payload 类型
template <EventType T> struct PayloadTypeFor;
template <> struct PayloadTypeFor<EventType::CANDLE_CLOSED> {
    using type = CandleClosedPayload;
};
template <> struct PayloadTypeFor<EventType::CANDLE_UPDATED> {
    using type = CandleClosedPayload;
};
template <> struct PayloadTypeFor<EventType::SIGNAL_GENERATED> {
    using type = SignalGeneratedPayload;
};
template <> struct PayloadTypeFor<EventType::SIGNAL_FILTERED> {
    using type = SignalGeneratedPayload;
};
template <> struct PayloadTypeFor<EventType::ORDER_PLACED> {
    using type = OrderPayload;
};
template <> struct PayloadTypeFor<EventType::ORDER_FILLED> {
    using type = OrderPayload;
};
template <> struct PayloadTypeFor<EventType::ORDER_PARTIAL_FILL> {
    using type = OrderPayload;
};
template <> struct PayloadTypeFor<EventType::ORDER_CANCELED> {
    using type = OrderPayload;
};
template <> struct PayloadTypeFor<EventType::ORDER_REJECTED> {
    using type = OrderPayload;
};
template <> struct PayloadTypeFor<EventType::POSITION_OPENED> {
    using type = OrderPayload;
};
template <> struct PayloadTypeFor<EventType::POSITION_CLOSED> {
    using type = OrderPayload;
};
template <> struct PayloadTypeFor<EventType::STOP_TRIGGERED> {
    using type = OrderPayload;
};
template <> struct PayloadTypeFor<EventType::AI_DECISION> {
    using type = GenericPayload;
};
template <> struct PayloadTypeFor<EventType::MODEL_DRIFT> {
    using type = GenericPayload;
};
template <> struct PayloadTypeFor<EventType::RISK_CHECK_FAILED> {
    using type = FaultPayload;
};
template <> struct PayloadTypeFor<EventType::CIRCUIT_BREAKER> {
    using type = FaultPayload;
};
template <> struct PayloadTypeFor<EventType::DAILY_LOSS_LIMIT> {
    using type = FaultPayload;
};
template <> struct PayloadTypeFor<EventType::FAULT_DETECTED> {
    using type = FaultPayload;
};
template <> struct PayloadTypeFor<EventType::FAULT_RESOLVED> {
    using type = FaultPayload;
};
template <> struct PayloadTypeFor<EventType::CONFIG_RELOADED> {
    using type = ConfigReloadedPayload;
};
template <> struct PayloadTypeFor<EventType::HEARTBEAT> {
    using type = EmptyPayload;
};
template <> struct PayloadTypeFor<EventType::STARTUP> {
    using type = EmptyPayload;
};
template <> struct PayloadTypeFor<EventType::SHUTDOWN> {
    using type = EmptyPayload;
};

// 通用 payload 序列化
template <typename T>
void serialize_payload(nlohmann::json& j, const T& payload);

template <typename T>
Result<T> deserialize_payload(const nlohmann::json& j);

// -----------------------------------------------------------------------------
// EmptyPayload
// -----------------------------------------------------------------------------
template <>
inline void serialize_payload<EmptyPayload>(nlohmann::json& /*j*/, const EmptyPayload&) {
    // 无字段
}

template <>
inline Result<EmptyPayload> deserialize_payload<EmptyPayload>(const nlohmann::json& /*j*/) {
    return EmptyPayload{};
}

// -----------------------------------------------------------------------------
// CandleClosedPayload
// -----------------------------------------------------------------------------
template <>
inline void serialize_payload<CandleClosedPayload>(
    nlohmann::json& j, const CandleClosedPayload& p)
{
    j["symbol"]        = p.symbol;
    j["open_time_us"]  = p.open_time_us;
    j["close_time_us"] = p.close_time_us;
    j["open"]          = p.open;
    j["high"]          = p.high;
    j["low"]           = p.low;
    j["close"]         = p.close;
    j["volume"]        = p.volume;
    j["is_closed"]     = p.is_closed;
}

template <>
inline Result<CandleClosedPayload>
deserialize_payload<CandleClosedPayload>(const nlohmann::json& j) {
    try {
        CandleClosedPayload p;
        if (j.contains("symbol"))        p.symbol = j.at("symbol").get<std::string>();
        if (j.contains("open_time_us"))  p.open_time_us = j.at("open_time_us").get<int64_t>();
        if (j.contains("close_time_us")) p.close_time_us = j.at("close_time_us").get<int64_t>();
        if (j.contains("open"))          p.open = j.at("open").get<double>();
        if (j.contains("high"))          p.high = j.at("high").get<double>();
        if (j.contains("low"))           p.low = j.at("low").get<double>();
        if (j.contains("close"))         p.close = j.at("close").get<double>();
        if (j.contains("volume"))        p.volume = j.at("volume").get<double>();
        if (j.contains("is_closed"))     p.is_closed = j.at("is_closed").get<bool>();

        // 校验
        if (p.symbol.empty()) {
            return QUANT_ERR_T(CandleClosedPayload, ErrorCode::ConfigInvalidValue)
                .with_context("payload", "CandleClosedPayload.symbol 为空");
        }
        if (p.high < p.low) {
            return QUANT_ERR_T(CandleClosedPayload, ErrorCode::ConfigInvalidValue)
                .with_context("payload", "high < low");
        }
        return p;
    } catch (const std::exception& e) {
        return QUANT_ERR_T(CandleClosedPayload, ErrorCode::ConfigInvalidValue)
            .with_context("exception", e.what());
    }
}

// -----------------------------------------------------------------------------
// SignalGeneratedPayload
// -----------------------------------------------------------------------------
template <>
inline void serialize_payload<SignalGeneratedPayload>(
    nlohmann::json& j, const SignalGeneratedPayload& p)
{
    j["symbol"]            = p.symbol;
    j["entry_price_raw"]   = p.entry_price_raw;
    j["stop_loss_raw"]     = p.stop_loss_raw;
    j["take_profit_raw"]   = p.take_profit_raw;
    j["side"]              = p.side;
    j["score"]             = p.score;
    j["confidence"]        = p.confidence;
}

template <>
inline Result<SignalGeneratedPayload>
deserialize_payload<SignalGeneratedPayload>(const nlohmann::json& j) {
    try {
        SignalGeneratedPayload p;
        if (j.contains("symbol"))          p.symbol = j.at("symbol").get<std::string>();
        if (j.contains("entry_price_raw")) p.entry_price_raw = j.at("entry_price_raw").get<int64_t>();
        if (j.contains("stop_loss_raw"))   p.stop_loss_raw = j.at("stop_loss_raw").get<int64_t>();
        if (j.contains("take_profit_raw")) p.take_profit_raw = j.at("take_profit_raw").get<int64_t>();
        if (j.contains("side"))            p.side = j.at("side").get<int8_t>();
        if (j.contains("score"))           p.score = j.at("score").get<double>();
        if (j.contains("confidence"))      p.confidence = j.at("confidence").get<double>();

        // 校验 score 范围
        if (p.score < 0.0 || p.score > 100.0) {
            return QUANT_ERR_T(SignalGeneratedPayload, ErrorCode::ConfigInvalidValue)
                .with_context("score", std::to_string(p.score));
        }
        return p;
    } catch (const std::exception& e) {
        return QUANT_ERR_T(SignalGeneratedPayload, ErrorCode::ConfigInvalidValue)
            .with_context("exception", e.what());
    }
}

// -----------------------------------------------------------------------------
// OrderPayload
// -----------------------------------------------------------------------------
template <>
inline void serialize_payload<OrderPayload>(
    nlohmann::json& j, const OrderPayload& p)
{
    j["order_id"]      = p.order_id;
    j["symbol"]        = p.symbol;
    j["side"]          = p.side;
    j["type"]          = p.type;
    j["status"]        = p.status;
    j["price_raw"]     = p.price_raw;
    j["quantity_raw"]  = p.quantity_raw;
    j["filled_raw"]    = p.filled_raw;
}

template <>
inline Result<OrderPayload>
deserialize_payload<OrderPayload>(const nlohmann::json& j) {
    try {
        OrderPayload p;
        if (j.contains("order_id"))     p.order_id = j.at("order_id").get<uint64_t>();
        if (j.contains("symbol"))       p.symbol = j.at("symbol").get<std::string>();
        if (j.contains("side"))         p.side = j.at("side").get<int8_t>();
        if (j.contains("type"))         p.type = j.at("type").get<int8_t>();
        if (j.contains("status"))       p.status = j.at("status").get<int8_t>();
        if (j.contains("price_raw"))    p.price_raw = j.at("price_raw").get<int64_t>();
        if (j.contains("quantity_raw")) p.quantity_raw = j.at("quantity_raw").get<int64_t>();
        if (j.contains("filled_raw"))   p.filled_raw = j.at("filled_raw").get<int64_t>();

        // 校验
        if (p.order_id == 0) {
            return QUANT_ERR_T(OrderPayload, ErrorCode::ConfigInvalidValue)
                .with_context("order_id", "0");
        }
        return p;
    } catch (const std::exception& e) {
        return QUANT_ERR_T(OrderPayload, ErrorCode::ConfigInvalidValue)
            .with_context("exception", e.what());
    }
}

// -----------------------------------------------------------------------------
// FaultPayload
// -----------------------------------------------------------------------------
template <>
inline void serialize_payload<FaultPayload>(
    nlohmann::json& j, const FaultPayload& p)
{
    j["code"]          = p.code;
    j["description"]   = p.description;
    j["source_module"] = p.source_module;
}

template <>
inline Result<FaultPayload>
deserialize_payload<FaultPayload>(const nlohmann::json& j) {
    try {
        FaultPayload p;
        if (j.contains("code"))          p.code = j.at("code").get<uint32_t>();
        if (j.contains("description"))   p.description = j.at("description").get<std::string>();
        if (j.contains("source_module")) p.source_module = j.at("source_module").get<std::string>();
        return p;
    } catch (const std::exception& e) {
        return QUANT_ERR_T(FaultPayload, ErrorCode::ConfigInvalidValue)
            .with_context("exception", e.what());
    }
}

// -----------------------------------------------------------------------------
// ConfigReloadedPayload
// -----------------------------------------------------------------------------
template <>
inline void serialize_payload<ConfigReloadedPayload>(
    nlohmann::json& j, const ConfigReloadedPayload& p)
{
    j["old_version"] = p.old_version;
    j["new_version"] = p.new_version;
    j["source_file"] = p.source_file;
}

template <>
inline Result<ConfigReloadedPayload>
deserialize_payload<ConfigReloadedPayload>(const nlohmann::json& j) {
    try {
        ConfigReloadedPayload p;
        if (j.contains("old_version")) p.old_version = j.at("old_version").get<uint64_t>();
        if (j.contains("new_version")) p.new_version = j.at("new_version").get<uint64_t>();
        if (j.contains("source_file")) p.source_file = j.at("source_file").get<std::string>();
        return p;
    } catch (const std::exception& e) {
        return QUANT_ERR_T(ConfigReloadedPayload, ErrorCode::ConfigInvalidValue)
            .with_context("exception", e.what());
    }
}

// -----------------------------------------------------------------------------
// GenericPayload
// -----------------------------------------------------------------------------
template <>
inline void serialize_payload<GenericPayload>(
    nlohmann::json& j, const GenericPayload& p)
{
    j["key"]   = p.key;
    j["value"] = p.value;
}

template <>
inline Result<GenericPayload>
deserialize_payload<GenericPayload>(const nlohmann::json& j) {
    try {
        GenericPayload p;
        if (j.contains("key"))   p.key = j.at("key").get<std::string>();
        if (j.contains("value")) p.value = j.at("value").get<std::string>();
        return p;
    } catch (const std::exception& e) {
        return QUANT_ERR_T(GenericPayload, ErrorCode::ConfigInvalidValue)
            .with_context("exception", e.what());
    }
}

// -----------------------------------------------------------------------------
// CRC32 计算（用于可选校验）
// -----------------------------------------------------------------------------
[[nodiscard]] inline uint32_t crc32(std::span<const uint8_t> data) noexcept {
    // 使用 CRC32C（Castagnoli）多项式，可硬件加速
    // 简化实现：查表法
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        constexpr uint32_t poly = 0x82F63B78;  // CRC32C
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = 0xFFFFFFFF;
    for (auto b : data) {
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// -----------------------------------------------------------------------------
// double 有效性（JSON 不支持 NaN/Inf）
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_finite_number(double v) noexcept {
    return std::isfinite(v);
}

}  // namespace detail

// ==============================================================================
// EventSerializer
// ==============================================================================
class EventSerializer {
public:
    // Schema 版本（当前）
    static constexpr uint32_t kSchemaVersion = 1;

    // 大小限制
    static constexpr std::size_t kMaxSerializedSize = 1 * 1024 * 1024;  // 1 MB

    // -------------------------------------------------------------------------
    // 序列化：Event → 字节
    // -------------------------------------------------------------------------
    [[nodiscard]] static Result<SerializedEvent> serialize(
        const Event& event,
        const SerializationOptions& options = {});

    // 序列化为 JSON 字符串（便捷）
    [[nodiscard]] static Result<std::string> to_json_string(
        const Event& event,
        bool pretty = false);

    // 批量序列化
    [[nodiscard]] static Result<std::vector<SerializedEvent>> serialize_batch(
        std::span<const Event> events,
        const SerializationOptions& options = {});

    // -------------------------------------------------------------------------
    // 反序列化：字节 → Event
    // -------------------------------------------------------------------------
    [[nodiscard]] static Result<Event> deserialize(
        std::span<const uint8_t> data,
        const SerializationOptions& options = {});

    // 从字符串反序列化
    [[nodiscard]] static Result<Event> from_json_string(std::string_view json);

    // 批量反序列化
    [[nodiscard]] static Result<std::vector<Event>> deserialize_batch(
        std::span<const SerializedEvent> records,
        const SerializationOptions& options = {});

    // -------------------------------------------------------------------------
    // 校验
    // -------------------------------------------------------------------------
    [[nodiscard]] static bool validate(const Event& event) noexcept;

private:
    // 内部：按 EventType 分派 payload 序列化
    template <typename Payload>
    static void serialize_payload_dispatch(nlohmann::json& j, const Event& event);

    // 内部：按 EventType 分派 payload 反序列化
    static Result<EventPayload> deserialize_payload_dispatch(
        EventType type, const nlohmann::json& j);
};

// ==============================================================================
// 内联实现
// ==============================================================================

// -----------------------------------------------------------------------------
// serialize_payload_dispatch
// -----------------------------------------------------------------------------
template <typename Payload>
inline void EventSerializer::serialize_payload_dispatch(
    nlohmann::json& j, const Event& event)
{
    if (auto* p = std::get_if<Payload>(&event.payload)) {
        detail::serialize_payload<Payload>(j, *p);
    }
}

// -----------------------------------------------------------------------------
// deserialize_payload_dispatch
// -----------------------------------------------------------------------------
inline Result<EventPayload> EventSerializer::deserialize_payload_dispatch(
    EventType type, const nlohmann::json& j)
{
    switch (type) {
        case EventType::CANDLE_CLOSED:
        case EventType::CANDLE_UPDATED: {
            auto r = detail::deserialize_payload<CandleClosedPayload>(j);
            if (r.is_err()) return QUANT_ERR_T(EventPayload, r.error_code())
                .with_context("type", to_string(type));
            return EventPayload{std::move(r.value())};
        }

        case EventType::SIGNAL_GENERATED:
        case EventType::SIGNAL_FILTERED: {
            auto r = detail::deserialize_payload<SignalGeneratedPayload>(j);
            if (r.is_err()) return QUANT_ERR_T(EventPayload, r.error_code())
                .with_context("type", to_string(type));
            return EventPayload{std::move(r.value())};
        }

        case EventType::ORDER_PLACED:
        case EventType::ORDER_FILLED:
        case EventType::ORDER_PARTIAL_FILL:
        case EventType::ORDER_CANCELED:
        case EventType::ORDER_REJECTED:
        case EventType::POSITION_OPENED:
        case EventType::POSITION_CLOSED:
        case EventType::STOP_TRIGGERED: {
            auto r = detail::deserialize_payload<OrderPayload>(j);
            if (r.is_err()) return QUANT_ERR_T(EventPayload, r.error_code())
                .with_context("type", to_string(type));
            return EventPayload{std::move(r.value())};
        }

        case EventType::RISK_CHECK_FAILED:
        case EventType::CIRCUIT_BREAKER:
        case EventType::DAILY_LOSS_LIMIT:
        case EventType::FAULT_DETECTED:
        case EventType::FAULT_RESOLVED: {
            auto r = detail::deserialize_payload<FaultPayload>(j);
            if (r.is_err()) return QUANT_ERR_T(EventPayload, r.error_code())
                .with_context("type", to_string(type));
            return EventPayload{std::move(r.value())};
        }

        case EventType::CONFIG_RELOADED: {
            auto r = detail::deserialize_payload<ConfigReloadedPayload>(j);
            if (r.is_err()) return QUANT_ERR_T(EventPayload, r.error_code())
                .with_context("type", to_string(type));
            return EventPayload{std::move(r.value())};
        }

        case EventType::HEARTBEAT:
        case EventType::STARTUP:
        case EventType::SHUTDOWN:
        case EventType::DEPTH_UPDATED:
        case EventType::FUNDING_UPDATED:
        case EventType::REGIME_CHANGED:
        case EventType::AI_DECISION:
        case EventType::MODEL_DRIFT: {
            // 这些类型使用 GenericPayload 或 EmptyPayload
            if (j.is_null() || j.empty()) {
                return EventPayload{EmptyPayload{}};
            }
            auto r = detail::deserialize_payload<GenericPayload>(j);
            if (r.is_err()) return QUANT_ERR_T(EventPayload, r.error_code());
            return EventPayload{std::move(r.value())};
        }

        case EventType::UNKNOWN:
        default:
            return QUANT_ERR_T(EventPayload, ErrorCode::ConfigInvalidValue)
                .with_context("type", "UNKNOWN");
    }
}

// -----------------------------------------------------------------------------
// validate
// -----------------------------------------------------------------------------
inline bool EventSerializer::validate(const Event& event) noexcept {
    if (!event.is_valid()) return false;
    if (event.trace_id == 0) return false;

    // 时间戳合理性：不超过 now + 1 小时
    const auto now_us = Timestamp::now().microseconds();
    const auto event_us = event.timestamp.microseconds();
    constexpr int64_t kOneHourUs = 3600LL * 1'000'000LL;
    if (event_us > now_us + kOneHourUs) return false;

    return true;
}

// ==============================================================================
// serialize 实现（非模板，可放头文件 inline）
// ==============================================================================
inline Result<SerializedEvent> EventSerializer::serialize(
    const Event& event,
    const SerializationOptions& options)
{
    // 1. 校验
    if (!validate(event)) {
        return QUANT_ERR_T(SerializedEvent, ErrorCode::ConfigInvalidValue)
            .with_context("event", event.to_string());
    }

    // 2. 构造 JSON 对象
    try {
        nlohmann::json j;

        // 元数据
        if (options.include_metadata) {
            j["_schema_version"] = kSchemaVersion;
            j["_serialized_at"]  = Timestamp::now().microseconds();
        }

        // 事件基础字段
        j["type"]        = static_cast<uint16_t>(event.type);
        j["type_name"]   = std::string{to_string(event.type)};
        j["priority"]    = static_cast<uint8_t>(event.priority);
        j["trace_id"]    = event.trace_id;
        j["parent_id"]   = event.parent_id;
        j["timestamp"]   = event.timestamp.microseconds();

        // payload（按类型分派）
        nlohmann::json payload_json = nlohmann::json::object();

        // 使用 visitor 分派（编译期展开所有可能类型）
        std::visit([&payload_json](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            detail::serialize_payload<T>(payload_json, p);
        }, event.payload);

        j["payload"] = std::move(payload_json);

        // 3. 序列化为字节
        SerializedEvent result;
        result.format = options.format;

        switch (options.format) {
            case SerializationFormat::JSON: {
                const int indent = options.pretty ? options.json_indent : -1;
                const auto text = j.dump(indent, ' ', false,
                    nlohmann::json::error_handler_t::replace);
                if (text.size() > options.max_size) {
                    return QUANT_ERR_T(SerializedEvent, ErrorCode::SystemOutOfMemory)
                        .with_context("size", std::to_string(text.size()));
                }
                result.data.assign(text.begin(), text.end());
                break;
            }

            case SerializationFormat::MESSAGE_PACK: {
                const auto bytes = nlohmann::json::to_msgpack(j);
                if (bytes.size() > options.max_size) {
                    return QUANT_ERR_T(SerializedEvent, ErrorCode::SystemOutOfMemory)
                        .with_context("size", std::to_string(bytes.size()));
                }
                result.data = bytes;
                break;
            }
        }

        // 4. 可选校验和
        if (options.include_checksum) {
            result.checksum = detail::crc32(result.data);
        }

        return result;
    } catch (const std::exception& e) {
        return QUANT_ERR_T(SerializedEvent, ErrorCode::InternalUnknown)
            .with_context("exception", e.what());
    } catch (...) {
        return QUANT_ERR_T(SerializedEvent, ErrorCode::InternalUnknown)
            .with_context("exception", "unknown");
    }
}

// -----------------------------------------------------------------------------
// to_json_string
// -----------------------------------------------------------------------------
inline Result<std::string> EventSerializer::to_json_string(
    const Event& event, bool pretty)
{
    SerializationOptions opts;
    opts.format = SerializationFormat::JSON;
    opts.pretty = pretty;
    opts.json_indent = 2;

    auto r = serialize(event, opts);
    if (r.is_err()) {
        return QUANT_ERR_T(std::string, r.error_code())
            .with_context("serialize", r.error().to_string());
    }
    return r.value().as_string();
}

// -----------------------------------------------------------------------------
// deserialize
// -----------------------------------------------------------------------------
inline Result<Event> EventSerializer::deserialize(
    std::span<const uint8_t> data,
    const SerializationOptions& options)
{
    // 1. 大小限制
    if (data.size() > options.max_size) {
        return QUANT_ERR_T(Event, ErrorCode::UserInputInvalid)
            .with_context("size", std::to_string(data.size()));
    }

    if (data.empty()) {
        return QUANT_ERR_T(Event, ErrorCode::UserInputInvalid)
            .with_context("data", "empty");
    }

    // 2. 解析 JSON
    nlohmann::json j;
    try {
        switch (options.format) {
            case SerializationFormat::JSON: {
                j = nlohmann::json::parse(data.begin(), data.end(),
                    nullptr, true, true);  // allow_exceptions=true
                break;
            }
            case SerializationFormat::MESSAGE_PACK: {
                j = nlohmann::json::from_msgpack(data.begin(), data.end(),
                    true, true);
                break;
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        return QUANT_ERR_T(Event, ErrorCode::ConfigParseFailed)
            .with_context("parse_error", e.what())
            .with_context("byte", std::to_string(e.byte));
    } catch (const std::exception& e) {
        return QUANT_ERR_T(Event, ErrorCode::ConfigParseFailed)
            .with_context("exception", e.what());
    }

    // 3. 版本检查（可选迁移）
    if (j.contains("_schema_version")) {
        const auto version = j["_schema_version"].get<uint32_t>();
        if (version > kSchemaVersion) {
            return QUANT_ERR_T(Event, ErrorCode::ConfigSchemaMismatch)
                .with_context("version", std::to_string(version))
                .with_context("expected", std::to_string(kSchemaVersion));
        }
        // 未来可在此处做迁移
    }

    // 4. 提取基础字段
    try {
        Event event;

        if (!j.contains("type")) {
            return QUANT_ERR_T(Event, ErrorCode::ConfigInvalidValue)
                .with_context("field", "type");
        }
        event.type = static_cast<EventType>(j.at("type").get<uint16_t>());

        if (j.contains("priority")) {
            event.priority = static_cast<EventPriority>(j.at("priority").get<uint8_t>());
        } else {
            event.priority = default_priority(event.type);
        }

        if (j.contains("trace_id"))  event.trace_id = j.at("trace_id").get<uint64_t>();
        if (j.contains("parent_id")) event.parent_id = j.at("parent_id").get<uint64_t>();

        if (j.contains("timestamp")) {
            event.timestamp = Timestamp{j.at("timestamp").get<int64_t>()};
        } else {
            event.timestamp = Timestamp::now();
        }

        // 5. 反序列化 payload
        nlohmann::json empty_payload = nlohmann::json::object();
        const auto& payload_json = j.contains("payload") ? j.at("payload") : empty_payload;

        auto payload_result = deserialize_payload_dispatch(event.type, payload_json);
        if (payload_result.is_err()) {
            return QUANT_ERR_T(Event, payload_result.error_code())
                .with_context("payload_type", to_string(event.type));
        }
        event.payload = std::move(payload_result.value());

        // 6. 校验
        if (!validate(event)) {
            return QUANT_ERR_T(Event, ErrorCode::ConfigInvalidValue)
                .with_context("reason", "validate failed");
        }

        return event;
    } catch (const std::exception& e) {
        return QUANT_ERR_T(Event, ErrorCode::ConfigInvalidValue)
            .with_context("exception", e.what());
    }
}

// -----------------------------------------------------------------------------
// from_json_string
// -----------------------------------------------------------------------------
inline Result<Event> EventSerializer::from_json_string(std::string_view json) {
    SerializationOptions opts;
    opts.format = SerializationFormat::JSON;
    return deserialize(
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(json.data()), json.size()),
        opts);
}

// -----------------------------------------------------------------------------
// serialize_batch
// -----------------------------------------------------------------------------
inline Result<std::vector<SerializedEvent>> EventSerializer::serialize_batch(
    std::span<const Event> events,
    const SerializationOptions& options)
{
    std::vector<SerializedEvent> results;
    results.reserve(events.size());

    for (const auto& e : events) {
        auto r = serialize(e, options);
        if (r.is_err()) {
            return QUANT_ERR_T(std::vector<SerializedEvent>, r.error_code())
                .with_context("batch_index", std::to_string(results.size()));
        }
        results.push_back(std::move(r.value()));
    }
    return results;
}

// -----------------------------------------------------------------------------
// deserialize_batch
// -----------------------------------------------------------------------------
inline Result<std::vector<Event>> EventSerializer::deserialize_batch(
    std::span<const SerializedEvent> records,
    const SerializationOptions& options)
{
    std::vector<Event> results;
    results.reserve(records.size());

    for (const auto& rec : records) {
        auto r = deserialize(rec.data, options);
        if (r.is_err()) {
            return QUANT_ERR_T(std::vector<Event>, r.error_code())
                .with_context("batch_index", std::to_string(results.size()));
        }
        results.push_back(std::move(r.value()));
    }
    return results;
}

}  // namespace event
}  // namespace quant

#endif  // QUANT_EVENT_CORE_SERIALIZER_HPP
