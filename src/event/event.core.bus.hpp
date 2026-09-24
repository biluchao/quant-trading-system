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
//   - 类型安全：Event 使用 variant 存储具体 payload，避免字符串拷贝
//   - 无锁发布：热路径入队使用 MPSC 队列，零阻塞（多生产者）
//   - 优先级：CRITICAL/HIGH/NORMAL/LOW 四级队列，风控优先
//   - 因果链：trace_id + parent_id 串联全链路事件
//   - 异常隔离：每个回调独立 try-catch，异常不传播
//   - 订阅生命周期：RAII Subscription，析构自动取消
//   - 背压策略：有界队列，满时按优先级丢弃或阻塞
//   - 优雅关闭：stop() 排空队列，广播 SHUTDOWN 事件
//   - 可观测：完整统计（发布/分发/丢弃/延迟）
//
// 实现说明:
//   - 本头文件仅声明接口，具体实现在 event.core.bus.cpp
//   - 轻量 inline 函数（模板、简单转发）保留在此文件
//   - 每优先级队列为单消费者模式，消费者数由 dispatch_threads 控制
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
// 项目依赖
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
    UNKNOWN             = 0,

    // 数据层 (0x01xx)
    CANDLE_CLOSED       = 0x0101,
    CANDLE_UPDATED      = 0x0102,
    DEPTH_UPDATED       = 0x0103,
    FUNDING_UPDATED     = 0x0104,

    // 策略层 (0x02xx)
    SIGNAL_GENERATED    = 0x0201,
    SIGNAL_FILTERED     = 0x0202,
    REGIME_CHANGED      = 0x0203,

    // OMS 层 (0x03xx)
    ORDER_PLACED        = 0x0301,
    ORDER_FILLED        = 0x0302,
    ORDER_PARTIAL_FILL  = 0x0303,
    ORDER_CANCELED      = 0x0304,
    ORDER_REJECTED      = 0x0305,
    POSITION_OPENED     = 0x0306,
    POSITION_CLOSED     = 0x0307,
    STOP_TRIGGERED      = 0x0308,

    // AI 层 (0x04xx)
    AI_DECISION         = 0x0401,
    MODEL_DRIFT         = 0x0402,

    // 风控层 (0x05xx)
    RISK_CHECK_FAILED   = 0x0501,
    CIRCUIT_BREAKER     = 0x0502,
    DAILY_LOSS_LIMIT    = 0x0503,

    // 系统层 (0x06xx)
    CONFIG_RELOADED     = 0x0601,
    FAULT_DETECTED      = 0x0602,
    FAULT_RESOLVED      = 0x0603,
    HEARTBEAT           = 0x0604,

    // 生命周期 (0x07xx)
    STARTUP             = 0x0701,
    SHUTDOWN            = 0x0702,
};

[[nodiscard]] constexpr std::string_view to_string(EventType t) noexcept {
    switch (t) {
        case EventType::UNKNOWN:            return "UNKNOWN";
        case EventType::CANDLE_CLOSED:      return "CANDLE_CLOSED";
        case EventType::CANDLE_UPDATED:     return "CANDLE_UPDATED";
        case EventType::DEPTH_UPDATED:      return "DEPTH_UPDATED";
        case EventType::FUNDING_UPDATED:    return "FUNDING_UPDATED";
        case EventType::SIGNAL_GENERATED:   return "SIGNAL_GENERATED";
        case EventType::SIGNAL_FILTERED:    return "SIGNAL_FILTERED";
        case EventType::REGIME_CHANGED:     return "REGIME_CHANGED";
        case EventType::ORDER_PLACED:       return "ORDER_PLACED";
        case EventType::ORDER_FILLED:       return "ORDER_FILLED";
        case EventType::ORDER_PARTIAL_FILL: return "ORDER_PARTIAL_FILL";
        case EventType::ORDER_CANCELED:     return "ORDER_CANCELED";
        case EventType::ORDER_REJECTED:     return "ORDER_REJECTED";
        case EventType::POSITION_OPENED:    return "POSITION_OPENED";
        case EventType::POSITION_CLOSED:    return "POSITION_CLOSED";
        case EventType::STOP_TRIGGERED:     return "STOP_TRIGGERED";
        case EventType::AI_DECISION:        return "AI_DECISION";
        case EventType::MODEL_DRIFT:        return "MODEL_DRIFT";
        case EventType::RISK_CHECK_FAILED:  return "RISK_CHECK_FAILED";
        case EventType::CIRCUIT_BREAKER:    return "CIRCUIT_BREAKER";
        case EventType::DAILY_LOSS_LIMIT:   return "DAILY_LOSS_LIMIT";
        case EventType::CONFIG_RELOADED:    return "CONFIG_RELOADED";
        case EventType::FAULT_DETECTED:     return "FAULT_DETECTED";
        case EventType::FAULT_RESOLVED:     return "FAULT_RESOLVED";
        case EventType::HEARTBEAT:          return "HEARTBEAT";
        case EventType::STARTUP:            return "STARTUP";
        case EventType::SHUTDOWN:           return "SHUTDOWN";
    }
    return "UNKNOWN";
}

// ==============================================================================
// 事件优先级
// ==============================================================================
enum class EventPriority : uint8_t {
    LOW      = 0,
    NORMAL   = 1,
    HIGH     = 2,
    CRITICAL = 3,
};

[[nodiscard]] constexpr EventPriority default_priority(EventType t) noexcept {
    switch (t) {
        case EventType::CIRCUIT_BREAKER:
        case EventType::DAILY_LOSS_LIMIT:
        case EventType::RISK_CHECK_FAILED:
        case EventType::STOP_TRIGGERED:
        case EventType::FAULT_DETECTED:
        case EventType::SHUTDOWN:
            return EventPriority::CRITICAL;

        case EventType::ORDER_PLACED:
        case EventType::ORDER_FILLED:
        case EventType::ORDER_REJECTED:
        case EventType::ORDER_CANCELED:
        case EventType::ORDER_PARTIAL_FILL:
        case EventType::POSITION_OPENED:
        case EventType::POSITION_CLOSED:
        case EventType::SIGNAL_GENERATED:
        case EventType::AI_DECISION:
            return EventPriority::HIGH;

        case EventType::CANDLE_CLOSED:
        case EventType::REGIME_CHANGED:
        case EventType::CONFIG_RELOADED:
        case EventType::MODEL_DRIFT:
        case EventType::FAULT_RESOLVED:
            return EventPriority::NORMAL;

        default:
            return EventPriority::LOW;
    }
}

// ==============================================================================
// 因果链追踪
// ==============================================================================
using TraceId = uint64_t;

// 生成全局唯一 trace_id（时间戳 + 序列号）
[[nodiscard]] inline TraceId generate_trace_id() noexcept {
    static std::atomic<uint64_t> counter{0};
    const auto now_us = static_cast<uint64_t>(Timestamp::now().microseconds());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    return (now_us << 12) | (seq & 0xFFF);
}

// ==============================================================================
// 事件 payload（类型安全）
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
    int8_t side{0};       // 1=BUY, 2=SELL
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

// 通用 payload：未定义类型时使用
struct GenericPayload {
    std::string key;
    std::string value;
};

// 空 payload
struct EmptyPayload {};

// 所有 payload 的 variant
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
struct Event {
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
        return type != EventType::UNKNOWN && timestamp.is_valid();
    }

    [[nodiscard]] std::string to_string() const {
        return std::string{event::to_string(type)} +
               " [trace=" + std::to_string(trace_id) +
               ", parent=" + std::to_string(parent_id) + "]";
    }
};

// ==============================================================================
// 前向声明
// ==============================================================================
class EventBus;

// ==============================================================================
// 订阅句柄（RAII，析构自动取消）
// ==============================================================================
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

    // 主动取消订阅（幂等）
    void release() noexcept;

    [[nodiscard]] bool valid() const noexcept { return id_ != 0; }
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
    bool async{false};          // 保留：是否异步分发
    bool once{false};           // 只触发一次，分发后自动取消
    std::string_view name{};    // 订阅者名称（用于日志）
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

    void reset() noexcept {
        published_total.store(0, std::memory_order_relaxed);
        dispatched_total.store(0, std::memory_order_relaxed);
        dropped_total.store(0, std::memory_order_relaxed);
        callback_error_total.store(0, std::memory_order_relaxed);
        queue_high_watermark.store(0, std::memory_order_relaxed);
        total_latency_us.store(0, std::memory_order_relaxed);
        max_latency_us.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double avg_latency_us() const noexcept {
        const auto n = dispatched_total.load(std::memory_order_relaxed);
        return n == 0 ? 0.0
            : static_cast<double>(total_latency_us.load(std::memory_order_relaxed))
              / static_cast<double>(n);
    }
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
        // 队列容量（用于统计基准；实际容量由编译期常量决定）
        std::size_t queue_capacity{16384};

        // 分发线程数（1~4；每优先级队列单消费者，超过 4 无效）
        std::size_t dispatch_threads{2};

        // 队列满时策略：true=丢弃，false=返回错误
        bool drop_on_overflow{true};

        // 是否启用因果链追踪
        bool enable_tracing{true};

        // 是否绑定 CPU 核心（需要平台支持）
        bool pin_threads{false};

        // 每类事件最大订阅者数
        std::size_t max_subscribers_per_type{128};
    };

    // -------------------------------------------------------------------------
    // 常量
    // -------------------------------------------------------------------------
    static constexpr std::size_t kNumPriorities = 4;

    // 每优先级队列容量（编译期固定，2 的幂）
    static constexpr std::size_t kQueueCapacityPerPriority = 1 << 14;  // 16384

    // 事件类型分片数
    static constexpr std::size_t kNumEventTypes = 512;

    // -------------------------------------------------------------------------
    // 单例
    // -------------------------------------------------------------------------
    [[nodiscard]] static EventBus& instance() noexcept;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------

    // 初始化（幂等；必须在 start 之前）
    Result<void> initialize(const Config& config = Config{}) noexcept;

    // 启动分发线程（幂等）
    Result<void> start() noexcept;

    // 优雅关闭（幂等；先停止分发线程，再同步广播 SHUTDOWN）
    Result<void> stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // 发布
    // -------------------------------------------------------------------------

    // 同步发布：入队后返回，异步分发
    Result<void> publish(Event event) noexcept;

    // 便捷模板：构造后发布
    template <typename Payload>
    Result<void> publish(EventType type, Payload&& payload,
                          EventPriority priority = EventPriority::NORMAL,
                          TraceId parent_id = 0) noexcept {
        Event e{type,
                EventPayload{std::forward<Payload>(payload)},
                priority,
                generate_trace_id(),
                parent_id};
        return publish(std::move(e));
    }

    // -------------------------------------------------------------------------
    // 订阅
    // -------------------------------------------------------------------------

    // 订阅单类型事件
    [[nodiscard]] Subscription subscribe(
        EventType type,
        Handler handler,
        SubscribeOptions options = {});

    // 订阅多类型事件（返回第一个类型的句柄，实际多类型共享取消）
    [[nodiscard]] Subscription subscribe_multi(
        std::span<const EventType> types,
        Handler handler,
        SubscribeOptions options = {});

    // 取消订阅（幂等）
    void unsubscribe(uint64_t id) noexcept;

    // -------------------------------------------------------------------------
    // 同步分发（仅用于关闭时广播，或测试）
    // -------------------------------------------------------------------------
    void dispatch_sync(const Event& event) noexcept;

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const EventBusStats& stats() const noexcept {
        return stats_;
    }

    [[nodiscard]] std::size_t pending_events() const noexcept;

    [[nodiscard]] std::string dump() const;

private:
    EventBus();
    ~EventBus();

    // 分发线程主循环
    void dispatch_loop() noexcept;

    // 单事件分发
    void dispatch_one(Event& event) noexcept;

    // 内部：订阅者条目
    struct Subscriber {
        uint64_t id{0};
        Handler handler;
        SubscribeOptions options;
    };

    // 内部：按类型的订阅者列表
    struct TypeSubscribers {
        std::vector<Subscriber> subscribers;
        mutable std::shared_mutex mutex;
    };

    // 获取或创建类型分片
    [[nodiscard]] TypeSubscribers& get_or_create(EventType type) noexcept;

    // 队列类型：多生产者单消费者
    using Queue = MpscQueue<Event, kQueueCapacityPerPriority>;

    // 按优先级选择队列
    [[nodiscard]] Queue& queue_for(EventPriority p) noexcept {
        return *queues_[static_cast<std::size_t>(p)];
    }

    // -------------------------------------------------------------------------
    // 数据成员
    // -------------------------------------------------------------------------
    Config config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> next_subscription_id_{1};

    // 4 个优先级队列（每个独立 MPSC）
    std::array<std::unique_ptr<Queue>, kNumPriorities> queues_;

    // 分发线程
    std::vector<std::thread> dispatch_threads_;
    std::mutex dispatch_mutex_;
    std::condition_variable dispatch_cv_;

    // 订阅者：按事件类型分片
    std::array<std::unique_ptr<TypeSubscribers>, kNumEventTypes> subscribers_;

    // 订阅 ID -> (type, id) 映射
    struct SubscriptionEntry {
        EventType type;
        uint64_t id;
    };
    mutable std::shared_mutex subscriptions_mutex_;
    std::unordered_map<uint64_t, SubscriptionEntry> subscriptions_;

    // 统计
    EventBusStats stats_;
};

// ==============================================================================
// 便捷宏
// ==============================================================================
#define QUANT_EVENT_BUS ::quant::event::EventBus::instance()

#define QUANT_PUBLISH(type, payload) \
    (::quant::event::EventBus::instance().publish(type, payload))

#define QUANT_SUBSCRIBE(type, handler) \
    (::quant::event::EventBus::instance().subscribe(type, handler))

// ==============================================================================
// 编译期校验
// ==============================================================================
static_assert(sizeof(EventType) == 2, "EventType 必须为 2 字节");
static_assert(sizeof(EventPriority) == 1, "EventPriority 必须为 1 字节");
static_assert(std::is_trivially_copyable_v<TraceId>,
              "TraceId 必须可平凡复制");

}  // namespace event
}  // namespace quant

#endif  // QUANT_EVENT_CORE_BUS_HPP
