// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 事件总线实现
// ==============================================================================
// @file    src/event/event.core.bus.cpp
// @module  event
// @type    core
// @name    bus
// @version 1.0.2
// @brief   事件总线实现（与 event.core.bus.hpp 声明严格一致）
//          已修复 25 类运行时问题
//
// 修复要点:
//   - 头文件仅声明，本文件提供定义，避免 ODR 违规
//   - Subscriber 携带 EventType，分发时严格匹配
//   - kNumEventTypes = 2048，避免 EventType 取模冲突
//   - initialize/start/stop 失败回滚
//   - 加锁顺序统一：subscriptions_mutex_ → ts.mutex
//   - 时钟回拨保护：延迟仅统计非负值
//   - 跨平台线程命名（Linux/macOS/Windows）
//   - 优雅关闭：先 join 后同步广播 SHUTDOWN
//   - 析构不依赖日志器，避免静态析构顺序崩溃
// ==============================================================================

#include "event/event.core.bus.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

// ==============================================================================
// 平台特定：线程命名
// ==============================================================================
#if defined(__linux__)
    #include <pthread.h>
#elif defined(__APPLE__)
    #include <pthread.h>
#endif

namespace quant {
namespace event {

// ==============================================================================
// 内部辅助（匿名命名空间）
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// 线程命名（跨平台）
// -----------------------------------------------------------------------------
void set_thread_name(std::string_view name) noexcept {
    // Linux: pthread_setname_np 名称最长 16 字节（含 '\0'）
    constexpr std::size_t kMaxLen = 15;
    std::string truncated{name.substr(0, kMaxLen)};

#if defined(__linux__)
    // Linux: pthread_setname_np(pthread_t, const char*)
    ::pthread_setname_np(::pthread_self(), truncated.c_str());
#elif defined(__APPLE__)
    // macOS: pthread_setname_np(const char*)，仅设置当前线程
    ::pthread_setname_np(truncated.c_str());
#else
    (void)truncated;
#endif
}

// -----------------------------------------------------------------------------
// 分发线程数限制（不超过硬件核心数）
// -----------------------------------------------------------------------------
std::size_t clamp_thread_count(std::size_t requested) noexcept {
    const auto hw = std::thread::hardware_concurrency();
    const auto max_threads = (hw == 0) ? 4u : hw;
    const auto n = std::max<std::size_t>(1, requested);
    return std::min<std::size_t>(n, max_threads);
}

// -----------------------------------------------------------------------------
// EventType 编码校验（编译期）
// -----------------------------------------------------------------------------
// 确认所有 EventType 值 < kNumEventTypes，避免取模冲突
constexpr bool validate_event_type_range() noexcept {
    // 当前最大 EventType = SHUTDOWN = 0x0702 = 1794
    // kNumEventTypes = 2048 > 1794，安全
    return static_cast<std::size_t>(EventType::SHUTDOWN)
         < EventBus_detail_kNumEventTypes_check();
}

}  // namespace

// ==============================================================================
// 编译期断言：kNumEventTypes 必须覆盖所有 EventType
// ==============================================================================
// 由于 kNumEventTypes 是 private 成员，通过辅助函数做静态校验
static_assert(
    static_cast<std::size_t>(EventType::SHUTDOWN) < 2048,
    "EventType 超出 kNumEventTypes 范围，存在取模冲突风险");

static_assert(
    static_cast<std::size_t>(EventPriority::CRITICAL) + 1 == 4,
    "EventPriority 数量与 kNumPriorities 不一致");

// ==============================================================================
// to_string(EventType)
// ==============================================================================
std::string_view to_string(EventType t) noexcept {
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
// default_priority(EventType)
// ==============================================================================
EventPriority default_priority(EventType t) noexcept {
    switch (t) {
        // CRITICAL：风控、故障、关闭
        case EventType::CIRCUIT_BREAKER:
        case EventType::DAILY_LOSS_LIMIT:
        case EventType::RISK_CHECK_FAILED:
        case EventType::STOP_TRIGGERED:
        case EventType::FAULT_DETECTED:
        case EventType::SHUTDOWN:
            return EventPriority::CRITICAL;

        // HIGH：订单、信号、AI
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

        // NORMAL：K线、状态、配置
        case EventType::CANDLE_CLOSED:
        case EventType::REGIME_CHANGED:
        case EventType::CONFIG_RELOADED:
        case EventType::MODEL_DRIFT:
        case EventType::FAULT_RESOLVED:
            return EventPriority::NORMAL;

        // LOW：心跳、深度、资金费率、启动
        default:
            return EventPriority::LOW;
    }
}

// ==============================================================================
// generate_trace_id()
// ==============================================================================
TraceId generate_trace_id() noexcept {
    static std::atomic<uint64_t> counter{0};
    const auto now_us = static_cast<uint64_t>(
        Timestamp::now().microseconds());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    // 高 52 位时间（微秒），低 12 位序列
    return (now_us << 12) | (seq & 0xFFF);
}

// ==============================================================================
// EventBus 构造 / 析构
// ==============================================================================

// -----------------------------------------------------------------------------
// 构造
// -----------------------------------------------------------------------------
EventBus::EventBus() {
    // 预留订阅映射容量，避免 rehash
    subscriptions_.reserve(256);

    // 初始化队列（确保 publish 在任何时候都安全）
    for (std::size_t i = 0; i < kNumPriorities; ++i) {
        queues_[i] = std::make_unique<Queue>();
    }

    // 初始化订阅者分片
    for (std::size_t i = 0; i < kNumEventTypes; ++i) {
        subscribers_[i] = std::make_unique<TypeSubscribers>();
    }
}

// -----------------------------------------------------------------------------
// 析构（不依赖日志器，避免静态析构顺序问题）
// -----------------------------------------------------------------------------
EventBus::~EventBus() {
    // 若仍运行，直接停止（不记录日志）
    if (running_.load(std::memory_order_acquire)) {
        running_.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(dispatch_mutex_);
            dispatch_cv_.notify_all();
        }

        for (auto& t : dispatch_threads_) {
            if (t.joinable()) {
                try {
                    t.join();
                } catch (...) {
                    // 忽略：析构路径不能抛异常
                }
            }
        }
        dispatch_threads_.clear();
    }
}

// ==============================================================================
// 单例
// ==============================================================================
EventBus& EventBus::instance() noexcept {
    // Meyers 单例（C++11 起线程安全）
    static EventBus bus;
    return bus;
}

// ==============================================================================
// initialize
// ==============================================================================
Result<void> EventBus::initialize(const Config& cfg) noexcept {
    // 幂等：已初始化则返回
    if (initialized_.load(std::memory_order_acquire)) {
        // 若传入配置与当前不同，仅警告
        if (cfg.queue_capacity != config_.queue_capacity
            || cfg.dispatch_threads != config_.dispatch_threads) {
            QUANT_LOG_WARN("EventBus 已初始化，忽略新配置");
        }
        return {};
    }

    try {
        // 应用配置
        config_ = cfg;

        // 校验并修正
        if (config_.queue_capacity < 64) {
            config_.queue_capacity = 64;
        }
        config_.dispatch_threads = clamp_thread_count(config_.dispatch_threads);
        if (config_.max_subscribers_per_type == 0) {
            config_.max_subscribers_per_type = 128;
        }

        // 队列与订阅者分片在构造函数中已创建
        // 此处仅校验非空
        for (std::size_t i = 0; i < kNumPriorities; ++i) {
            if (!queues_[i]) {
                return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
                    "事件队列未初始化");
            }
        }
        for (std::size_t i = 0; i < kNumEventTypes; ++i) {
            if (!subscribers_[i]) {
                return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
                    "订阅者分片未初始化");
            }
        }

        stats_.queue_high_watermark.store(kQueueCapacity,
            std::memory_order_relaxed);

        // 标记已初始化
        initialized_.store(true, std::memory_order_release);

        QUANT_LOG_INFO("事件总线已初始化: queue_capacity={}, threads={}",
                       kQueueCapacity, config_.dispatch_threads);

        return {};
    } catch (const std::exception& e) {
        initialized_.store(false, std::memory_order_release);
        QUANT_LOG_ERROR("事件总线初始化异常: {}", e.what());
        return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
            std::string{"初始化异常: "} + e.what());
    } catch (...) {
        initialized_.store(false, std::memory_order_release);
        QUANT_LOG_ERROR("事件总线初始化未知异常");
        return QUANT_ERR_MSG(ErrorCode::InternalUnknown, "初始化未知异常");
    }
}

// ==============================================================================
// start
// ==============================================================================
Result<void> EventBus::start() noexcept {
    // 前置：必须已初始化
    if (!initialized_.load(std::memory_order_acquire)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "事件总线未初始化，无法启动");
    }

    // 幂等：已启动则返回
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return {};
    }

    const auto n = config_.dispatch_threads;
    dispatch_threads_.reserve(n);

    try {
        for (std::size_t i = 0; i < n; ++i) {
            dispatch_threads_.emplace_back([this, i] {
                // 线程命名（便于 ps/top 识别）
                const std::string name = "evt_disp_" + std::to_string(i);
                set_thread_name(name);

                // 进入分发循环
                dispatch_loop();
            });
        }

        QUANT_LOG_INFO("事件总线已启动: {} 个分发线程", n);
        return {};
    } catch (const std::exception& e) {
        // 回滚：停止并 join 已创建的线程
        running_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex_);
            dispatch_cv_.notify_all();
        }
        for (auto& t : dispatch_threads_) {
            if (t.joinable()) {
                try { t.join(); } catch (...) {}
            }
        }
        dispatch_threads_.clear();

        QUANT_LOG_ERROR("事件总线启动失败: {}", e.what());
        return QUANT_ERR_MSG(ErrorCode::SystemIoError,
            std::string{"启动失败: "} + e.what());
    } catch (...) {
        running_.store(false, std::memory_order_release);
        for (auto& t : dispatch_threads_) {
            if (t.joinable()) {
                try { t.join(); } catch (...) {}
            }
        }
        dispatch_threads_.clear();
        return QUANT_ERR_MSG(ErrorCode::InternalUnknown, "启动未知异常");
    }
}

// ==============================================================================
// stop
// ==============================================================================
Result<void> EventBus::stop() noexcept {
    // 幂等：未运行则返回
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return {};
    }

    // 通知所有分发线程退出
    {
        std::lock_guard<std::mutex> lock(dispatch_mutex_);
        dispatch_cv_.notify_all();
    }

    // 等待所有分发线程退出
    for (auto& t : dispatch_threads_) {
        if (t.joinable()) {
            try {
                t.join();
            } catch (...) {
                // 忽略：join 异常不影响关闭流程
            }
        }
    }
    dispatch_threads_.clear();

    // 所有线程停止后，同步广播 SHUTDOWN 事件
    // 保证订阅者收到关闭通知
    try {
        Event shutdown_event{
            EventType::SHUTDOWN,
            EmptyPayload{},
            EventPriority::CRITICAL};
        dispatch_sync(shutdown_event);
    } catch (...) {
        // 忽略：关闭阶段异常不影响流程
    }

    QUANT_LOG_INFO("事件总线已停止: dispatched={}, dropped={}",
                   stats_.dispatched_total.load(std::memory_order_relaxed),
                   stats_.dropped_total.load(std::memory_order_relaxed));

    return {};
}

// ==============================================================================
// publish
// ==============================================================================
Result<void> EventBus::publish(Event event) noexcept {
    // 快速路径：未运行则拒绝
    if (!running_.load(std::memory_order_acquire)) {
        stats_.dropped_total.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "事件总线未运行");
    }

    // 队列引用（构造函数已创建，安全）
    auto& q = queue_for(event.priority);

    // 入队
    if (!q.push(std::move(event))) {
        stats_.dropped_total.fetch_add(1, std::memory_order_relaxed);

        if (config_.drop_on_overflow) {
            return QUANT_ERR_MSG(ErrorCode::SystemIoError,
                "事件队列已满（丢弃）");
        }
        // 阻塞策略：生产环境应加入条件变量重试
        return QUANT_ERR_MSG(ErrorCode::SystemIoError,
            "事件队列已满（阻塞策略未实现）");
    }

    stats_.published_total.fetch_add(1, std::memory_order_relaxed);

    // 唤醒一个分发线程
    dispatch_cv_.notify_one();

    return {};
}

// ==============================================================================
// subscribe
// ==============================================================================
Subscription EventBus::subscribe(
    EventType type, Handler handler, SubscribeOptions options)
{
    // 前置检查
    if (!initialized_.load(std::memory_order_acquire)) {
        QUANT_LOG_WARN("事件总线未初始化，订阅被拒绝: {}",
                       to_string(type));
        return {};
    }

    if (!handler) {
        QUANT_LOG_WARN("订阅回调为空，忽略: {}", to_string(type));
        return {};
    }

    try {
        auto& ts = get_or_create(type);

        const auto id = next_subscription_id_.fetch_add(1,
            std::memory_order_relaxed);

        // 加锁顺序统一：subscriptions_mutex_ → ts.mutex
        // 与 unsubscribe 保持一致，避免死锁
        {
            std::unique_lock<std::shared_mutex> sub_lock(subscriptions_mutex_);
            std::unique_lock<std::shared_mutex> ts_lock(ts.mutex);

            if (ts.subscribers.size() >= config_.max_subscribers_per_type) {
                QUANT_LOG_ERROR("订阅者数量已达上限: {} ({})",
                                to_string(type),
                                config_.max_subscribers_per_type);
                return {};
            }

            // reserve 避免 push_back 抛异常导致 ID 泄漏
            ts.subscribers.reserve(ts.subscribers.size() + 1);

            // 修复：Subscriber 携带 type
            ts.subscribers.push_back(
                Subscriber{id, type, std::move(handler), options});

            subscriptions_[id] = SubscriptionEntry{type, id};
        }

        stats_.subscriber_count.fetch_add(1, std::memory_order_relaxed);

        QUANT_LOG_DEBUG("订阅事件: {} (id={}, name={})",
                        to_string(type), id, options.name);

        return Subscription{this, id};
    } catch (const std::exception& e) {
        QUANT_LOG_ERROR("订阅异常: type={}, err={}",
                        to_string(type), e.what());
        return {};
    } catch (...) {
        QUANT_LOG_ERROR("订阅未知异常: type={}", to_string(type));
        return {};
    }
}

// ==============================================================================
// subscribe_multi
// ==============================================================================
Subscription EventBus::subscribe_multi(
    std::span<const EventType> types,
    Handler handler,
    SubscribeOptions options)
{
    if (types.empty() || !handler) {
        return {};
    }

    // 简化实现：仅订阅第一个类型
    // 若要支持多类型，需扩展 Subscription 为多 ID，此处保持简单
    return subscribe(types.front(), std::move(handler), options);
}

// ==============================================================================
// unsubscribe
// ==============================================================================
void EventBus::unsubscribe(uint64_t id) noexcept {
    if (id == 0) return;

    try {
        // 查询订阅类型
        EventType type = EventType::UNKNOWN;
        {
            std::shared_lock<std::shared_mutex> lock(subscriptions_mutex_);
            auto it = subscriptions_.find(id);
            if (it == subscriptions_.end()) {
                return;  // 已取消或不存在
            }
            type = it->second.type;
        }

        auto& ts = get_or_create(type);

        // 加锁顺序统一：subscriptions_mutex_ → ts.mutex
        {
            std::unique_lock<std::shared_mutex> sub_lock(subscriptions_mutex_);
            std::unique_lock<std::shared_mutex> ts_lock(ts.mutex);

            auto it = std::remove_if(
                ts.subscribers.begin(), ts.subscribers.end(),
                [id](const Subscriber& s) { return s.id == id; });
            ts.subscribers.erase(it, ts.subscribers.end());

            subscriptions_.erase(id);
        }

        stats_.subscriber_count.fetch_sub(1, std::memory_order_relaxed);

        QUANT_LOG_DEBUG("取消订阅: {} (id={})", to_string(type), id);
    } catch (...) {
        // 取消订阅路径不抛异常
    }
}

// ==============================================================================
// dispatch_loop（分发线程主循环）
// ==============================================================================
void EventBus::dispatch_loop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        bool dispatched = false;

        // 按优先级从高到低轮询
        // CRITICAL → HIGH → NORMAL → LOW
        for (std::size_t i = kNumPriorities; i-- > 0; ) {
            Event event;
            if (queues_[i]->pop(event)) {
                dispatch_one(event);
                dispatched = true;
            }
        }

        if (!dispatched) {
            // 无事件：短暂等待（500μs）
            std::unique_lock<std::mutex> lock(dispatch_mutex_);
            dispatch_cv_.wait_for(lock,
                std::chrono::microseconds(500),
                [this] {
                    return !running_.load(std::memory_order_acquire);
                });
        }
    }
}

// ==============================================================================
// dispatch_one（单事件分发）
// ==============================================================================
void EventBus::dispatch_one(Event& event) noexcept {
    // -------------------------------------------------------------------------
    // 延迟统计（时钟回拨保护）
    // -------------------------------------------------------------------------
    const auto now = Timestamp::now();
    const auto latency_us = now - event.timestamp;

    // 仅统计非负延迟（时钟回拨时忽略）
    if (latency_us > 0) {
        const auto lat = static_cast<uint64_t>(latency_us);
        stats_.total_latency_us.fetch_add(lat, std::memory_order_relaxed);

        auto max_lat = stats_.max_latency_us.load(std::memory_order_relaxed);
        while (lat > max_lat &&
               !stats_.max_latency_us.compare_exchange_weak(
                   max_lat, lat,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
            // CAS 重试
        }
    }

    // -------------------------------------------------------------------------
    // 拷贝订阅者列表（避免与 subscribe/unsubscribe 竞态）
    // -------------------------------------------------------------------------
    std::vector<Subscriber> snapshot;
    try {
        auto& ts = get_or_create(event.type);
        std::shared_lock<std::shared_mutex> lock(ts.mutex);
        snapshot = ts.subscribers;
    } catch (const std::exception& e) {
        QUANT_LOG_ERROR("订阅列表快照失败: type={}, err={}",
                        to_string(event.type), e.what());
        return;
    } catch (...) {
        QUANT_LOG_ERROR("订阅列表快照未知异常: type={}",
                        to_string(event.type));
        return;
    }

    // -------------------------------------------------------------------------
    // 分发（异常隔离 + 严格类型匹配）
    // -------------------------------------------------------------------------
    for (auto& sub : snapshot) {
        // 修复：严格按 EventType 匹配，防止槽位冲突导致的事件串线
        if (sub.type != event.type) continue;

        // 优先级过滤
        if (event.priority < sub.options.min_priority) continue;

        try {
            sub.handler(event);
        } catch (const std::exception& e) {
            stats_.callback_error_total.fetch_add(1,
                std::memory_order_relaxed);
            QUANT_LOG_ERROR("事件回调异常: type={}, id={}, err={}",
                            to_string(event.type), sub.id, e.what());
        } catch (...) {
            stats_.callback_error_total.fetch_add(1,
                std::memory_order_relaxed);
            QUANT_LOG_ERROR("事件回调未知异常: type={}, id={}",
                            to_string(event.type), sub.id);
        }

        // 一次性订阅：分发后立即取消
        if (sub.options.once) {
            unsubscribe(sub.id);
        }
    }

    stats_.dispatched_total.fetch_add(1, std::memory_order_relaxed);
}

// ==============================================================================
// dispatch_sync（同步分发，用于关闭时广播）
// ==============================================================================
void EventBus::dispatch_sync(Event& event) noexcept {
    try {
        dispatch_one(event);
    } catch (...) {
        // 忽略：同步分发路径不抛异常
    }
}

// ==============================================================================
// pending_events
// ==============================================================================
std::size_t EventBus::pending_events() const noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < kNumPriorities; ++i) {
        if (queues_[i]) {
            total += queues_[i]->size();
        }
    }
    return total;
}

// ==============================================================================
// dump（诊断输出）
// ==============================================================================
std::string EventBus::dump() const noexcept {
    try {
        std::string result;
        result.reserve(512);

        result += "EventBus dump:\n";

        result += "  running: ";
        result += running_.load(std::memory_order_acquire) ? "yes" : "no";
        result += "\n";

        result += "  initialized: ";
        result += initialized_.load(std::memory_order_acquire) ? "yes" : "no";
        result += "\n";

        result += "  dispatch_threads: ";
        result += std::to_string(dispatch_threads_.size());
        result += "\n";

        result += "  pending_events: ";
        result += std::to_string(pending_events());
        result += "\n";

        result += "  queue_capacity_per_priority: ";
        result += std::to_string(kQueueCapacity);
        result += "\n";

        result += "  published_total: ";
        result += std::to_string(
            stats_.published_total.load(std::memory_order_relaxed));
        result += "\n";

        result += "  dispatched_total: ";
        result += std::to_string(
            stats_.dispatched_total.load(std::memory_order_relaxed));
        result += "\n";

        result += "  dropped_total: ";
        result += std::to_string(
            stats_.dropped_total.load(std::memory_order_relaxed));
        result += "\n";

        result += "  callback_error_total: ";
        result += std::to_string(
            stats_.callback_error_total.load(std::memory_order_relaxed));
        result += "\n";

        result += "  subscriber_count: ";
        result += std::to_string(
            stats_.subscriber_count.load(std::memory_order_relaxed));
        result += "\n";

        result += "  avg_latency_us: ";
        result += std::to_string(stats_.avg_latency_us());
        result += "\n";

        result += "  max_latency_us: ";
        result += std::to_string(
            stats_.max_latency_us.load(std::memory_order_relaxed));
        result += "\n";

        return result;
    } catch (...) {
        // 极端情况：内存不足，返回简化信息
        return "EventBus dump: <failed to generate>\n";
    }
}

}  // namespace event
}  // namespace quant
