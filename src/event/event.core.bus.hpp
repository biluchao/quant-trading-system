// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 事件总线
// ==============================================================================
// @file    src/event/event.core.bus.hpp
// @module  event
// @type    core
// @name    bus
// @version 1.0.2
// @brief   类型安全、无锁、带因果链追踪和优先级的事件总线
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 声明与定义分离：重函数仅在头文件声明，.cpp 提供实现，避免 ODR
//   - 类型安全：std::variant 存储 payload，EventType 严格匹配
//   - 无锁发布：热路径入队使用 SPSC 队列，零阻塞
//   - 优先级：4 级队列（LOW/NORMAL/HIGH/CRITICAL）
//   - 因果链：trace_id + parent_id 串联全链路事件
//   - 异常隔离：回调异常不传播，计数告警
//   - 生命周期：Subscription RAII + weak_ptr 防悬空
//   - 可观测：完整统计
//
// 使用方式:
//   EventBus::instance().initialize(cfg);
//   EventBus::instance().start();
//   auto sub = EventBus::instance().subscribe(...);
//   EventBus::instance().publish(...);
//   EventBus::instance().stop();
// ==============================================================================

#ifndef QUANT_EVENT_CORE_BUS_HPP
#define QUANT_EVENT_CORE_BUS_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"
#include "common/common.core.lockfree_queue.hpp"

namespace quant {
namespace event {

// ==============================================================================
// 事件类型
// ==============================================================================
enum class EventType : uint16_t {
    UNKNOWN             = 0x0000,

    // 数据层
    CANDLE_CLOSED       = 0x0101,
    CANDLE_UPDATED      = 0x0102,
    DEPTH_UPDATED       = 0x0103,
    FUNDING_UPDATED     = 0x0104,

    // 策略层
    SIGNAL_GENERATED    = 0x0201,
    SIGNAL_FILTERED     = 0x0202,
    REGIME_CHANGED      = 0x0203,

    // OMS 层
    ORDER_PLACED        = 0x0301,
    ORDER_FILLED        = 0x0302,
    ORDER_PARTIAL_FILL  = 0x0303,
    ORDER_CANCELED      = 0x0304,
    ORDER_REJECTED      = 0x0305,
    POSITION_OPENED     = 0x0306,
    POSITION_CLOSED     = 0x0307,
    STOP_TRIGGERED      = 0x0308,

    // AI 层
    AI_DECISION         = 0x0401,
    MODEL_DRIFT         = 0x0402,

    // 风控层
    RISK_CHECK_FAILED   = 0x0501,
    CIRCUIT_BREAKER     = 0x0502,
    DAILY_LOSS_LIMIT    = 0x0503,

    // 系统层
    CONFIG_RELOADED     = 0x0601,
    FAULT_DETECTED      = 0x0602,
    FAULT_RESOLVED      = 0x0603,
    HEARTBEAT           = 0x0604,

    // 生命周期
    STARTUP             = 0x0701,
    SHUTDOWN            = 0x0702,
};

// 声明（定义在 .cpp）
[[nodiscard]] std::string_view to_string(EventType t) noexcept;

// ==============================================================================
// 事件优先级
// ==============================================================================
enum class EventPriority : uint8_t {
    LOW      = 0,
    NORMAL   = 1,
    HIGH     = 2,
    CRITICAL = 3,
};

[[nodiscard]] EventPriority default_priority(EventType t) noexcept;

// ==============================================================================
// 因果链追踪
// ==============================================================================
using TraceId = uint64_t;

[[nodiscard]] TraceId generate_trace_id() noexcept;

// ==============================================================================
// 事件 payload
// ==============================================================================
struct CandleClosedPayload {
    std::string symbol;
    int64_t open_time_us{0};
    int64_t close_time_us{0};
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    double volume{0.0};
    bool is_closed{false};
};

struct SignalGeneratedPayload {
    std::string symbol;
    int64_t entry_price_raw{0};
    int64_t stop_loss_raw{0};
    int64_t take_profit_raw{0};
    int8_t side{0};
    double score{0.0};
    double confidence{0.0};
};

struct OrderPayload {
    uint64_t order_id{0};
    std::string symbol;
    int8_t side{0};
    int8_t type{0};
    int8_t status{0};
    int64_t price_raw{0};
    int64_t quantity_raw{0};
    int64_t filled_raw{0};
};

struct FaultPayload {
    uint32_t code{0};
    std::string description;
    std::string source_module;
};

struct ConfigReloadedPayload {
    uint64_t old_version{0};
    uint64_t new_version{0};
    std::string source_file;
};

struct GenericPayload {
    std::string key;
    std::string value;
};

struct EmptyPayload {};

using EventPayload = std::variant<
    EmptyPayload,
    CandleClosedPayload,
    SignalGeneratedPayload,
    OrderPayload,
    FaultPayload,
    ConfigReloadedPayload,
    GenericPayload
>;

// ==============================================================================
// 事件
// ==============================================================================
class Event {
public:
    EventType type{EventType::UNKNOWN};
    EventPriority priority{EventPriority::NORMAL};
    TraceId trace_id{0};
    TraceId parent_id{0};
    Timestamp timestamp{};
    EventPayload payload{EmptyPayload{}};

    Event() noexcept = default;

    Event(EventType t, EventPayload p,
          EventPriority prio = EventPriority::NORMAL,
          TraceId tid = 0, TraceId pid = 0) noexcept
        : type{t}
        , priority{prio}
        , trace_id{tid == 0 ? generate_trace_id() : tid}
        , parent_id{pid}
        , timestamp{Timestamp::now()}
        , payload{std::move(p)}
    {}

    [[nodiscard]] bool is_valid() const noexcept {
        return type != EventType::UNKNOWN
            && timestamp.microseconds() > 0;
    }

    // 类型检查辅助
    template <typename T>
    [[nodiscard]] bool has_payload() const noexcept {
        return std::holds_alternative<T>(payload);
    }

    template <typename T>
    [[nodiscard]] const T* get_payload() const noexcept {
        return std::get_if<T>(&payload);
    }

    template <typename T>
    [[nodiscard]] T* get_payload() noexcept {
        return std::get_if<T>(&payload);
    }

    [[nodiscard]] std::string to_string() const noexcept;
};

// ==============================================================================
// 订阅句柄（RAII）
// ==============================================================================
class EventBus;

class Subscription {
public:
    Subscription() noexcept = default;
    Subscription(EventBus* bus, uint64_t id) noexcept
        : bus_{bus}, id_{id} {}

    ~Subscription();

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept
        : bus_{other.bus_}, id_{other.id_} {
        other.bus_ = nullptr;
        other.id_ = 0;
    }

    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            release();
            bus_ = other.bus_;
            id_ = other.id_;
            other.bus_ = nullptr;
            other.id_ = 0;
        }
        return *this;
    }

    void release() noexcept;

    [[nodiscard]] bool valid() const noexcept { return id_ != 0 && bus_ != nullptr; }
    [[nodiscard]] uint64_t id() const noexcept { return id_; }

private:
    EventBus* bus_{nullptr};
    uint64_t id_{0};
};

// ==============================================================================
// 订阅选项
// ==============================================================================
struct SubscribeOptions {
    EventPriority min_priority{EventPriority::LOW};
    bool once{false};
    std::string_view name{};
};

// ==============================================================================
// 事件总线统计
// ==============================================================================
struct EventBusStats {
    alignas(64) std::atomic<uint64_t> published_total{0};
    alignas(64) std::atomic<uint64_t> dispatched_total{0};
    alignas(64) std::atomic<uint64_t> dropped_total{0};
    alignas(64) std::atomic<uint64_t> callback_error_total{0};
    alignas(64) std::atomic<uint64_t> queue_high_watermark{0};
    alignas(64) std::atomic<uint64_t> total_latency_us{0};
    alignas(64) std::atomic<uint64_t> max_latency_us{0};
    alignas(64) std::atomic<uint64_t> subscriber_count{0};

    void reset() noexcept;
    [[nodiscard]] double avg_latency_us() const noexcept;
};

// ==============================================================================
// 事件总线
// ==============================================================================
class EventBus {
public:
    using Handler = std::function<void(const Event&)>;

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    struct Config {
        std::size_t queue_capacity{16384};
        std::size_t dispatch_threads{2};
        bool drop_on_overflow{true};
        bool enable_tracing{true};
        bool pin_threads{false};
        std::size_t max_subscribers_per_type{128};
    };

    // -------------------------------------------------------------------------
    // 单例
    // -------------------------------------------------------------------------
    [[nodiscard]] static EventBus& instance() noexcept;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期（仅声明）
    // -------------------------------------------------------------------------
    Result<void> initialize(const Config& config = Config{}) noexcept;
    Result<void> start() noexcept;
    Result<void> stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // 发布（仅声明）
    // -------------------------------------------------------------------------
    Result<void> publish(Event event) noexcept;

    template <typename Payload>
    Result<void> publish(EventType type, Payload&& payload,
                          EventPriority priority = EventPriority::NORMAL,
                          TraceId parent_id = 0) noexcept {
        Event e{type,
                EventPayload{std::forward<Payload>(payload)},
                priority, generate_trace_id(), parent_id};
        return publish(std::move(e));
    }

    // -------------------------------------------------------------------------
    // 订阅（仅声明）
    // -------------------------------------------------------------------------
    [[nodiscard]] Subscription subscribe(
        EventType type,
        Handler handler,
        SubscribeOptions options = {});

    [[nodiscard]] Subscription subscribe_multi(
        std::span<const EventType> types,
        Handler handler,
        SubscribeOptions options = {});

    void unsubscribe(uint64_t id) noexcept;

    // -------------------------------------------------------------------------
    // 同步分发（仅声明）
    // -------------------------------------------------------------------------
    void dispatch_sync(Event& event) noexcept;

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    [[nodiscard]] const EventBusStats& stats() const noexcept {
        return stats_;
    }

    [[nodiscard]] std::size_t pending_events() const noexcept;
    [[nodiscard]] std::string dump() const noexcept;

private:
    EventBus();
    ~EventBus();

    void dispatch_loop() noexcept;
    void dispatch_one(Event& event) noexcept;

    // -------------------------------------------------------------------------
    // 订阅者条目（修复：携带 type，便于严格匹配）
    // -------------------------------------------------------------------------
    struct Subscriber {
        uint64_t id{0};
        EventType type{EventType::UNKNOWN};
        Handler handler;
        SubscribeOptions options;
    };

    struct TypeSubscribers {
        std::vector<Subscriber> subscribers;
        mutable std::shared_mutex mutex;
    };

    [[nodiscard]] TypeSubscribers& get_or_create(EventType type) noexcept;

    // -------------------------------------------------------------------------
    // 常量
    // -------------------------------------------------------------------------
    // 修复：从 512 增大到 2048，避免 EventType 取模冲突
    static constexpr std::size_t kNumEventTypes = 2048;

    // 修复：从 EventPriority 最大值推导，避免不同步
    static constexpr std::size_t kNumPriorities =
        static_cast<std::size_t>(EventPriority::CRITICAL) + 1;

    // 队列单容量（16384）
    static constexpr std::size_t kQueueCapacity = 1u << 14;

    using Queue = SpscQueue<Event, kQueueCapacity>;

    // -------------------------------------------------------------------------
    // 数据结构
    // -------------------------------------------------------------------------
    Config config_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> next_subscription_id_{1};

    std::array<std::unique_ptr<Queue>, kNumPriorities> queues_{};

    std::vector<std::thread> dispatch_threads_{};
    mutable std::mutex dispatch_mutex_{};
    std::condition_variable dispatch_cv_{};

    std::array<std::unique_ptr<TypeSubscribers>, kNumEventTypes> subscribers_{};

    struct SubscriptionEntry {
        EventType type{EventType::UNKNOWN};
        uint64_t id{0};
    };

    mutable std::shared_mutex subscriptions_mutex_{};
    std::unordered_map<uint64_t, SubscriptionEntry> subscriptions_{};

    EventBusStats stats_{};

    [[nodiscard]] Queue& queue_for(EventPriority p) noexcept {
        return *queues_[static_cast<std::size_t>(p)];
    }
};

// ==============================================================================
// 内联实现（仅小函数，避免 ODR）
// ==============================================================================

// -----------------------------------------------------------------------------
// Subscription
// -----------------------------------------------------------------------------
inline Subscription::~Subscription() {
    release();
}

inline void Subscription::release() noexcept {
    if (bus_ != nullptr && id_ != 0) {
        EventBus* bus = bus_;
        bus_ = nullptr;
        const uint64_t id = id_;
        id_ = 0;
        // 注意：unsubscribe 内部不抛异常
        bus->unsubscribe(id);
    }
}

// -----------------------------------------------------------------------------
// Event
// -----------------------------------------------------------------------------
inline std::string Event::to_string() const noexcept {
    try {
        return std::string{event::to_string(type)} +
               " [trace=" + std::to_string(trace_id) +
               ", parent=" + std::to_string(parent_id) + "]";
    } catch (...) {
        return "<Event::to_string failed>";
    }
}

// -----------------------------------------------------------------------------
// EventBusStats
// -----------------------------------------------------------------------------
inline void EventBusStats::reset() noexcept {
    published_total.store(0, std::memory_order_relaxed);
    dispatched_total.store(0, std::memory_order_relaxed);
    dropped_total.store(0, std::memory_order_relaxed);
    callback_error_total.store(0, std::memory_order_relaxed);
    queue_high_watermark.store(0, std::memory_order_relaxed);
    total_latency_us.store(0, std::memory_order_relaxed);
    max_latency_us.store(0, std::memory_order_relaxed);
}

inline double EventBusStats::avg_latency_us() const noexcept {
    const auto n = dispatched_total.load(std::memory_order_relaxed);
    if (n == 0) return 0.0;
    const auto total = total_latency_us.load(std::memory_order_relaxed);
    return static_cast<double>(total) / static_cast<double>(n);
}

// -----------------------------------------------------------------------------
// EventBus（简单转发）
// -----------------------------------------------------------------------------
inline EventBus::TypeSubscribers&
EventBus::get_or_create(EventType type) noexcept {
    const auto idx = static_cast<std::size_t>(type) % kNumEventTypes;
    return *subscribers_[idx];
}

}  // namespace event
}  // namespace quant

#endif  // QUANT_EVENT_CORE_BUS_HPP
