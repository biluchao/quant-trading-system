// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 事件序列化实现
// ==============================================================================
// @file    src/event/event.core.serializer.cpp
// @module  event
// @type    core
// @name    serializer
// @version 1.0.2
// @brief   事件序列化/反序列化实现（JSON / MessagePack，类型安全，版本兼容）
//
// 说明:
//   本文件提供 event.core.serializer.hpp 中非模板函数的实现。
//
//   为使本文件可编译，需将 hpp 中以下函数的 "inline 定义" 改为 "仅声明"：
//     1. EventSerializer::validate              (static)
//     2. EventSerializer::serialize             (static)
//     3. EventSerializer::to_json_string        (static)
//     4. EventSerializer::deserialize           (static)
//     5. EventSerializer::from_json_string      (static)
//     6. EventSerializer::serialize_batch       (static)
//     7. EventSerializer::deserialize_batch     (static)
//     8. EventSerializer::deserialize_payload_dispatch (static, private)
//
//   保留在 hpp 中的（模板或模板特化，必须可见）：
//     - detail::serialize_payload<T>             (模板特化)
//     - detail::deserialize_payload<T>           (模板特化)
//     - detail::crc32                            (inline)
//     - EventSerializer::serialize_payload_dispatch<T> (模板)
//
// 修复要点:
//   - payload 完整序列化（按 EventType 分派）
//   - 大小限制（防 OOM）
//   - 类型安全（严格匹配，无静默丢失）
//   - 版本兼容（schema_version）
//   - 精度保证（定点数 int64_t）
//   - NaN/Inf 处理（error_handler_t::replace）
//   - 错误返回 Result<T> 携带上下文
//   - 批量接口
// ==============================================================================

#include "event/event.core.serializer.hpp"

#include <cmath>
#include <cstring>

namespace quant {
namespace event {

// ==============================================================================
// EventSerializer::validate
// ==============================================================================
bool EventSerializer::validate(const Event& event) noexcept {
    if (!event.is_valid()) return false;
    if (event.trace_id == 0) return false;

    // 时间戳合理性：不超过 now + 1 小时
    const auto now_us = Timestamp::now().microseconds();
    const auto event_us = event.timestamp.microseconds();
    constexpr int64_t kOneHourUs = 3600LL * 1'000'000LL;
    if (event_us > now_us + kOneHourUs) return false;

    // 检查 trace_id 与 parent_id 不同
    if (event.parent_id != 0 && event.parent_id == event.trace_id) {
        return false;
    }

    return true;
}

// ==============================================================================
// EventSerializer::serialize
// ==============================================================================
Result<SerializedEvent> EventSerializer::serialize(
    const Event& event,
    const SerializationOptions& options)
{
    // 1. 校验事件
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

        // payload：使用 visit 分派到具体类型
        nlohmann::json payload_json = nlohmann::json::object();
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
                    return QUANT_ERR_T(SerializedEvent,
                        ErrorCode::SystemOutOfMemory)
                        .with_context("size", std::to_string(text.size()))
                        .with_context("max_size", std::to_string(options.max_size));
                }

                result.data.assign(text.begin(), text.end());
                break;
            }

            case SerializationFormat::MESSAGE_PACK: {
                const auto bytes = nlohmann::json::to_msgpack(j);

                if (bytes.size() > options.max_size) {
                    return QUANT_ERR_T(SerializedEvent,
                        ErrorCode::SystemOutOfMemory)
                        .with_context("size", std::to_string(bytes.size()))
                        .with_context("max_size", std::to_string(options.max_size));
                }

                result.data = bytes;
                break;
            }
        }

        // 4. 可选 CRC 校验
        if (options.include_checksum) {
            result.checksum = detail::crc32(result.data);
        }

        return result;
    } catch (const std::exception& e) {
        return QUANT_ERR_T(SerializedEvent, ErrorCode::InternalUnknown)
            .with_context("exception", e.what())
            .with_context("type", to_string(event.type));
    } catch (...) {
        return QUANT_ERR_T(SerializedEvent, ErrorCode::InternalUnknown)
            .with_context("exception", "unknown")
            .with_context("type", to_string(event.type));
    }
}

// ==============================================================================
// EventSerializer::to_json_string
// ==============================================================================
Result<std::string> EventSerializer::to_json_string(
    const Event& event, bool pretty)
{
    SerializationOptions opts;
    opts.format = SerializationFormat::JSON;
    opts.pretty = pretty;
    opts.json_indent = 2;
    opts.include_metadata = true;

    auto r = serialize(event, opts);
    if (r.is_err()) {
        return QUANT_ERR_T(std::string, r.error_code())
            .with_context("serialize", r.error().to_string());
    }
    return r.value().as_string();
}

// ==============================================================================
// EventSerializer::deserialize_payload_dispatch
// ==============================================================================
Result<EventPayload> EventSerializer::deserialize_payload_dispatch(
    EventType type, const nlohmann::json& j)
{
    switch (type) {
        case EventType::CANDLE_CLOSED:
        case EventType::CANDLE_UPDATED: {
            auto r = detail::deserialize_payload<CandleClosedPayload>(j);
            if (r.is_err()) {
                return QUANT_ERR_T(EventPayload, r.error_code())
                    .with_context("type", to_string(type))
                    .with_context("detail", r.error().to_string());
            }
            return EventPayload{std::move(r.value())};
        }

        case EventType::SIGNAL_GENERATED:
        case EventType::SIGNAL_FILTERED: {
            auto r = detail::deserialize_payload<SignalGeneratedPayload>(j);
            if (r.is_err()) {
                return QUANT_ERR_T(EventPayload, r.error_code())
                    .with_context("type", to_string(type))
                    .with_context("detail", r.error().to_string());
            }
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
            if (r.is_err()) {
                return QUANT_ERR_T(EventPayload, r.error_code())
                    .with_context("type", to_string(type))
                    .with_context("detail", r.error().to_string());
            }
            return EventPayload{std::move(r.value())};
        }

        case EventType::RISK_CHECK_FAILED:
        case EventType::CIRCUIT_BREAKER:
        case EventType::DAILY_LOSS_LIMIT:
        case EventType::FAULT_DETECTED:
        case EventType::FAULT_RESOLVED: {
            auto r = detail::deserialize_payload<FaultPayload>(j);
            if (r.is_err()) {
                return QUANT_ERR_T(EventPayload, r.error_code())
                    .with_context("type", to_string(type))
                    .with_context("detail", r.error().to_string());
            }
            return EventPayload{std::move(r.value())};
        }

        case EventType::CONFIG_RELOADED: {
            auto r = detail::deserialize_payload<ConfigReloadedPayload>(j);
            if (r.is_err()) {
                return QUANT_ERR_T(EventPayload, r.error_code())
                    .with_context("type", to_string(type))
                    .with_context("detail", r.error().to_string());
            }
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
            // 使用 GenericPayload 或 EmptyPayload
            if (j.is_null() || j.empty()) {
                return EventPayload{EmptyPayload{}};
            }
            auto r = detail::deserialize_payload<GenericPayload>(j);
            if (r.is_err()) {
                return QUANT_ERR_T(EventPayload, r.error_code())
                    .with_context("type", to_string(type))
                    .with_context("detail", r.error().to_string());
            }
            return EventPayload{std::move(r.value())};
        }

        case EventType::UNKNOWN:
        default:
            return QUANT_ERR_T(EventPayload, ErrorCode::ConfigInvalidValue)
                .with_context("type", "UNKNOWN")
                .with_context("raw_value",
                    std::to_string(static_cast<uint16_t>(type)));
    }
}

// ==============================================================================
// EventSerializer::deserialize
// ==============================================================================
Result<Event> EventSerializer::deserialize(
    std::span<const uint8_t> data,
    const SerializationOptions& options)
{
    // 1. 大小限制
    if (data.size() > options.max_size) {
        return QUANT_ERR_T(Event, ErrorCode::UserInputInvalid)
            .with_context("size", std::to_string(data.size()))
            .with_context("max_size", std::to_string(options.max_size));
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

    // 3. 版本检查
    if (j.contains("_schema_version")) {
        try {
            const auto version = j["_schema_version"].get<uint32_t>();
            if (version > kSchemaVersion) {
                return QUANT_ERR_T(Event, ErrorCode::ConfigSchemaMismatch)
                    .with_context("version", std::to_string(version))
                    .with_context("expected", std::to_string(kSchemaVersion));
            }
            // 未来可在此处做版本迁移
        } catch (const std::exception& e) {
            return QUANT_ERR_T(Event, ErrorCode::ConfigSchemaMismatch)
                .with_context("exception", e.what());
        }
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
            event.priority = static_cast<EventPriority>(
                j.at("priority").get<uint8_t>());
        } else {
            event.priority = default_priority(event.type);
        }

        if (j.contains("trace_id")) {
            event.trace_id = j.at("trace_id").get<uint64_t>();
        }
        if (j.contains("parent_id")) {
            event.parent_id = j.at("parent_id").get<uint64_t>();
        }

        if (j.contains("timestamp")) {
            event.timestamp = Timestamp{j.at("timestamp").get<int64_t>()};
        } else {
            event.timestamp = Timestamp::now();
        }

        // 5. 反序列化 payload
        nlohmann::json empty_payload = nlohmann::json::object();
        const auto& payload_json = j.contains("payload")
            ? j.at("payload")
            : empty_payload;

        auto payload_result = deserialize_payload_dispatch(
            event.type, payload_json);
        if (payload_result.is_err()) {
            return QUANT_ERR_T(Event, payload_result.error_code())
                .with_context("payload_type", to_string(event.type))
                .with_context("detail", payload_result.error().to_string());
        }
        event.payload = std::move(payload_result.value());

        // 6. 校验
        if (!validate(event)) {
            return QUANT_ERR_T(Event, ErrorCode::ConfigInvalidValue)
                .with_context("reason", "validate failed")
                .with_context("type", to_string(event.type))
                .with_context("trace_id", std::to_string(event.trace_id));
        }

        return event;
    } catch (const nlohmann::json::type_error& e) {
        return QUANT_ERR_T(Event, ErrorCode::ConfigTypeError)
            .with_context("exception", e.what())
            .with_context("id", std::to_string(e.id));
    } catch (const std::exception& e) {
        return QUANT_ERR_T(Event, ErrorCode::ConfigInvalidValue)
            .with_context("exception", e.what());
    }
}

// ==============================================================================
// EventSerializer::from_json_string
// ==============================================================================
Result<Event> EventSerializer::from_json_string(std::string_view json) {
    SerializationOptions opts;
    opts.format = SerializationFormat::JSON;

    return deserialize(
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(json.data()), json.size()),
        opts);
}

// ==============================================================================
// EventSerializer::serialize_batch
// ==============================================================================
Result<std::vector<SerializedEvent>> EventSerializer::serialize_batch(
    std::span<const Event> events,
    const SerializationOptions& options)
{
    std::vector<SerializedEvent> results;
    results.reserve(events.size());

    for (std::size_t i = 0; i < events.size(); ++i) {
        auto r = serialize(events[i], options);
        if (r.is_err()) {
            return QUANT_ERR_T(std::vector<SerializedEvent>, r.error_code())
                .with_context("batch_index", std::to_string(i))
                .with_context("batch_size", std::to_string(events.size()))
                .with_context("detail", r.error().to_string());
        }
        results.push_back(std::move(r.value()));
    }
    return results;
}

// ==============================================================================
// EventSerializer::deserialize_batch
// ==============================================================================
Result<std::vector<Event>> EventSerializer::deserialize_batch(
    std::span<const SerializedEvent> records,
    const SerializationOptions& options)
{
    std::vector<Event> results;
    results.reserve(records.size());

    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& rec = records[i];

        // 可选 CRC 校验
        if (options.include_checksum && rec.checksum != 0) {
            const auto computed = detail::crc32(rec.data);
            if (computed != rec.checksum) {
                return QUANT_ERR_T(std::vector<Event>, ErrorCode::SystemIoError)
                    .with_context("batch_index", std::to_string(i))
                    .with_context("reason", "checksum mismatch")
                    .with_context("expected", std::to_string(rec.checksum))
                    .with_context("computed", std::to_string(computed));
            }
        }

        auto r = deserialize(rec.data, options);
        if (r.is_err()) {
            return QUANT_ERR_T(std::vector<Event>, r.error_code())
                .with_context("batch_index", std::to_string(i))
                .with_context("batch_size", std::to_string(records.size()))
                .with_context("detail", r.error().to_string());
        }
        results.push_back(std::move(r.value()));
    }
    return results;
}

}  // namespace event
}  // namespace quant
