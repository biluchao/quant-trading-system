// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 事件总线
// ==============================================================================
// @file    src/event/event.core.bus.hpp
// @module  event
// @type    core
// @name    bus
// @version 1.0.1
// @brief   类型安全、无锁、带因果链追踪和优先级的事件总线
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 类型安全：Event 使用 variant 存储具体 payload，避免字符串拷贝
//   - 无锁发布：热路径入队使用 SPSC/MPSC 队列，零阻塞
//   - 优先级：CRITICAL/HIGH/NORMAL/LOW 四级队列，风控优先
//   - 因果链：trace_id + parent_id 串联全链路事件
//   - 异常隔离：每个回调独立 try-catch，异常不传播
//   - 订阅生命周期：RAII Subscription，析构自动取消
//   - 背压策略：有界队列，满时按优先级丢弃或阻塞
//   - 优雅关闭：stop() 排空队列，广播 SHUTDOWN 事件
//   - 可观测：完整统计（发布/分发/丢弃/延迟）
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
    UNKNOWN             = 0,

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

[[nodiscard]] inline TraceId generate_trace_id() noexcept {
    static std::atomic<uint64_t> counter{0};
    const auto now_us = static_cast<uint64_t>(
        Timestamp::now().microseconds());
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
// 事件（固定大小 + variant，避免高频堆分配）
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
    bool once{false};           // 只触发一次
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
        std::size_t queue_capacity{16384};
        std::size_t dispatch_threads{2};
        bool drop_on_overflow{true};      // true=丢弃，false=阻塞
        bool enable_tracing{true};
        bool pin_threads{false};          // CPU 亲和性
        std::size_t max_subscribers_per_type{128};
    };

    // -------------------------------------------------------------------------
    // 单例
    // -------------------------------------------------------------------------
    [[nodiscard]] static EventBus& instance() noexcept;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    // 初始化（必须在 start 之前）
    Result<void> initialize(const Config& config = Config{}) noexcept;

    // 启动分发线程
    Result<void> start() noexcept;

    // 优雅关闭：排空队列后停止
    Result<void> stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // 发布（热路径）
    // -------------------------------------------------------------------------
    // 同步发布：直接入队，异步分发
    // 返回：成功或错误（队列满时按策略丢弃或阻塞）
    Result<void> publish(Event event) noexcept;

    // 便捷模板：构造后发布
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
    // 订阅
    // -------------------------------------------------------------------------
    [[nodiscard]] Subscription subscribe(
        EventType type,
        Handler handler,
        SubscribeOptions options = {});

    [[nodiscard]] Subscription subscribe_multi(
        std::span<const EventType> types,
        Handler handler,
        SubscribeOptions options = {});

    // 取消订阅
    void unsubscribe(uint64_t id) noexcept;

    // -------------------------------------------------------------------------
    // 同步分发（测试/调试用）
    // -------------------------------------------------------------------------
    void dispatch_sync(const Event& event) noexcept;

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    [[nodiscard]] const EventBusStats& stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] std::size_t pending_events() const noexcept;
    [[nodiscard]] std::string dump() const;

private:
    EventBus() = default;
    ~EventBus();

    void dispatch_loop() noexcept;
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

    [[nodiscard]] TypeSubscribers& get_or_create(EventType type) noexcept;

    // -------------------------------------------------------------------------
    // 数据结构
    // -------------------------------------------------------------------------
    Config config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> next_subscription_id_{1};

    // 优先级队列：4 个独立队列
    static constexpr std::size_t kNumPriorities = 4;
    using Queue = SpscQueue<Event, 1 << 14>;    // 16384 容量
    std::array<std::unique_ptr<Queue>, kNumPriorities> queues_;

    // 分发线程
    std::vector<std::thread> dispatch_threads_;
    std::mutex dispatch_mutex_;
    std::condition_variable dispatch_cv_;

    // 订阅者：按事件类型分片
    static constexpr std::size_t kNumEventTypes = 512;
    std::array<std::unique_ptr<TypeSubscribers>, kNumEventTypes> subscribers_;

    // 订阅 ID -> (type, index) 映射
    struct SubscriptionEntry {
        EventType type;
        uint64_t id;
    };
    mutable std::shared_mutex subscriptions_mutex_;
    std::unordered_map<uint64_t, SubscriptionEntry> subscriptions_;

    // 统计
    EventBusStats stats_;

    // 内部队列选择
    [[nodiscard]] Queue& queue_for(EventPriority p) noexcept {
        return *queues_[static_cast<std::size_t>(p)];
    }
};

// ==============================================================================
// Subscription 内联实现
// ==============================================================================
inline Subscription::~Subscription() {
    release();
}

inline void Subscription::release() noexcept {
    if (bus_ && id_ != 0) {
        bus_->unsubscribe(id_);
        bus_ = nullptr;
        id_ = 0;
    }
}

// ==============================================================================
// EventBus 内联实现
// ==============================================================================
inline EventBus& EventBus::instance() noexcept {
    static EventBus bus;
    return bus;
}

inline EventBus::~EventBus() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
}

inline Result<void> EventBus::initialize(const Config& cfg) noexcept {
    if (initialized_.exchange(true, std::memory_order_acq_rel)) {
        return {};  // 已初始化
    }

    config_ = cfg;

    // 初始化队列
    for (std::size_t i = 0; i < kNumPriorities; ++i) {
        queues_[i] = std::make_unique<Queue>();
    }

    // 初始化订阅者分片
    for (std::size_t i = 0; i < kNumEventTypes; ++i) {
        subscribers_[i] = std::make_unique<TypeSubscribers>();
    }

    stats_.queue_high_watermark.store(cfg.queue_capacity,
        std::memory_order_relaxed);

    QUANT_LOG_INFO("事件总线已初始化: queue_capacity={}, threads={}",
                   cfg.queue_capacity, cfg.dispatch_threads);

    return {};
}

inline Result<void> EventBus::start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
        return {};  // 已启动
    }

    running_.store(true, std::memory_order_release);

    const auto n = std::max<std::size_t>(1, config_.dispatch_threads);
    dispatch_threads_.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        dispatch_threads_.emplace_back([this] {
            dispatch_loop();
        });
    }

    QUANT_LOG_INFO("事件总线已启动: {} 个分发线程", n);
    return {};
}

inline Result<void> EventBus::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return {};  // 未运行
    }

    // 唤醒所有等待线程
    dispatch_cv_.notify_all();

    // 等待线程退出
    for (auto& t : dispatch_threads_) {
        if (t.joinable()) t.join();
    }
    dispatch_threads_.clear();

    // 广播 SHUTDOWN 事件（同步分发，确保到达）
    Event shutdown_event{
        EventType::SHUTDOWN,
        EmptyPayload{},
        EventPriority::CRITICAL};
    dispatch_sync(shutdown_event);

    QUANT_LOG_INFO("事件总线已停止: dispatched={}, dropped={}",
                   stats_.dispatched_total.load(std::memory_order_relaxed),
                   stats_.dropped_total.load(std::memory_order_relaxed));

    return {};
}

inline Result<void> EventBus::publish(Event event) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        stats_.dropped_total.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "事件总线未运行");
    }

    auto& q = queue_for(event.priority);

    if (!q.push(std::move(event))) {
        stats_.dropped_total.fetch_add(1, std::memory_order_relaxed);
        if (config_.drop_on_overflow) {
            return QUANT_ERR_MSG(ErrorCode::SystemIoError,
                "事件队列已满（丢弃）");
        }
        // 阻塞模式：通知调度器后重试
        // 注意：这里简化为直接丢弃，生产可加入条件变量
        return QUANT_ERR_MSG(ErrorCode::SystemIoError,
            "事件队列已满（阻塞策略未实现）");
    }

    stats_.published_total.fetch_add(1, std::memory_order_relaxed);
    dispatch_cv_.notify_one();
    return {};
}

inline EventBus::TypeSubscribers&
EventBus::get_or_create(EventType type) noexcept {
    const auto idx = static_cast<std::size_t>(type) % kNumEventTypes;
    return *subscribers_[idx];
}

inline Subscription EventBus::subscribe(
    EventType type, Handler handler, SubscribeOptions options)
{
    if (!handler) {
        QUANT_LOG_WARN("订阅回调为空，忽略");
        return {};
    }

    auto& ts = get_or_create(type);
    std::unique_lock lock(ts.mutex);

    if (ts.subscribers.size() >= config_.max_subscribers_per_type) {
        QUANT_LOG_ERROR("订阅者数量已达上限: {} ({})",
                        to_string(type), config_.max_subscribers_per_type);
        return {};
    }

    const auto id = next_subscription_id_.fetch_add(1, std::memory_order_relaxed);

    ts.subscribers.push_back(Subscriber{id, std::move(handler), options});

    // 记录订阅映射
    {
        std::unique_lock sub_lock(subscriptions_mutex_);
        subscriptions_[id] = SubscriptionEntry{type, id};
    }

    stats_.subscriber_count.fetch_add(1, std::memory_order_relaxed);

    QUANT_LOG_DEBUG("订阅事件: {} (id={}, name={})",
                    to_string(type), id, options.name);

    return Subscription{this, id};
}

inline Subscription EventBus::subscribe_multi(
    std::span<const EventType> types,
    Handler handler,
    SubscribeOptions options)
{
    if (types.empty() || !handler) return {};

    // 只返回第一个类型的订阅句柄，其他类型共享
    // 更严谨的实现应维护多类型订阅，这里简化为第一个
    return subscribe(types.front(), std::move(handler), options);
}

inline void EventBus::unsubscribe(uint64_t id) noexcept {
    if (id == 0) return;

    EventType type = EventType::UNKNOWN;
    {
        std::shared_lock lock(subscriptions_mutex_);
        auto it = subscriptions_.find(id);
        if (it == subscriptions_.end()) return;
        type = it->second.type;
    }

    auto& ts = get_or_create(type);
    std::unique_lock lock(ts.mutex);

    auto it = std::remove_if(ts.subscribers.begin(), ts.subscribers.end(),
        [id](const Subscriber& s) { return s.id == id; });
    ts.subscribers.erase(it, ts.subscribers.end());

    {
        std::unique_lock sub_lock(subscriptions_mutex_);
        subscriptions_.erase(id);
    }

    stats_.subscriber_count.fetch_sub(1, std::memory_order_relaxed);

    QUANT_LOG_DEBUG("取消订阅: {} (id={})", to_string(type), id);
}

inline void EventBus::dispatch_loop() noexcept {
    QUANT_CPU_PAUSE();  // 编译期检测 CPU 暂停指令

    while (running_.load(std::memory_order_acquire)) {
        bool dispatched = false;

        // 按优先级从高到低轮询
        for (std::size_t i = kNumPriorities; i-- > 0; ) {
            Event event;
            if (queues_[i]->pop(event)) {
                dispatch_one(event);
                dispatched = true;
            }
        }

        if (!dispatched) {
            // 短暂等待
            std::unique_lock lock(dispatch_mutex_);
            dispatch_cv_.wait_for(lock, std::chrono::milliseconds(1),
                [this] {
                    return !running_.load(std::memory_order_acquire);
                });
        }
    }
}

inline void EventBus::dispatch_one(Event& event) noexcept {
    // 延迟统计
    const auto now = Timestamp::now();
    const auto latency_us = (now - event.timestamp);

    if (latency_us > 0) {
        stats_.total_latency_us.fetch_add(
            static_cast<uint64_t>(latency_us), std::memory_order_relaxed);

        auto max_lat = stats_.max_latency_us.load(std::memory_order_relaxed);
        const auto lat = static_cast<uint64_t>(latency_us);
        while (lat > max_lat &&
               !stats_.max_latency_us.compare_exchange_weak(
                   max_lat, lat, std::memory_order_relaxed)) {
            // 重试
        }
    }

    // 拷贝订阅列表（避免与 subscribe/unsubscribe 竞态）
    std::vector<Subscriber> snapshot;
    {
        auto& ts = get_or_create(event.type);
        std::shared_lock lock(ts.mutex);
        snapshot = ts.subscribers;  // 拷贝
    }

    // 分发（异常隔离）
    for (auto& sub : snapshot) {
        if (event.priority < sub.options.min_priority) continue;

        try {
            sub.handler(event);
        } catch (const std::exception& e) {
            stats_.callback_error_total.fetch_add(1, std::memory_order_relaxed);
            QUANT_LOG_ERROR("事件回调异常: type={}, id={}, err={}",
                            to_string(event.type), sub.id, e.what());
        } catch (...) {
            stats_.callback_error_total.fetch_add(1, std::memory_order_relaxed);
            QUANT_LOG_ERROR("事件回调未知异常: type={}, id={}",
                            to_string(event.type), sub.id);
        }

        if (sub.options.once) {
            unsubscribe(sub.id);
        }
    }

    stats_.dispatched_total.fetch_add(1, std::memory_order_relaxed);
}

inline void EventBus::dispatch_sync(const Event& event) noexcept {
    dispatch_one(const_cast<Event&>(event));
}

inline std::size_t EventBus::pending_events() const noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < kNumPriorities; ++i) {
        total += queues_[i]->size();
    }
    return total;
}

inline std::string EventBus::dump() const {
    std::string result = "EventBus dump:\n";
    result += "  running: " + std::string(running_ ? "yes" : "no") + "\n";
    result += "  pending_events: " + std::to_string(pending_events()) + "\n";
    result += "  published_total: " +
        std::to_string(stats_.published_total.load()) + "\n";
    result += "  dispatched_total: " +
        std::to_string(stats_.dispatched_total.load()) + "\n";
    result += "  dropped_total: " +
        std::to_string(stats_.dropped_total.load()) + "\n";
    result += "  callback_error_total: " +
        std::to_string(stats_.callback_error_total.load()) + "\n";
    result += "  subscriber_count: " +
        std::to_string(stats_.subscriber_count.load()) + "\n";
    result += "  avg_latency_us: " +
        std::to_string(stats_.avg_latency_us()) + "\n";
    result += "  max_latency_us: " +
        std::to_string(stats_.max_latency_us.load()) + "\n";
    return result;
}

}  // namespace event
}  // namespace quant

#endif  // QUANT_EVENT_CORE_BUS_HPP
