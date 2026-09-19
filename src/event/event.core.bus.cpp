// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 事件总线实现
// ==============================================================================
// @file    src/event/event.core.bus.cpp
// @module  event
// @type    core
// @name    bus
// @version 1.0.1
// @brief   事件总线实现（发布/订阅、分发、统计、优雅关闭）
//          已修复 25 类运行时问题
//
// 说明:
//   本文件提供 event.core.bus.hpp 中重函数的实现。
//   头文件中应将这些函数改为"仅声明"，避免 ODR 违规。
//
// 修复要点:
//   - 幂等初始化：失败时重置 initialized_
//   - 线程创建失败回滚
//   - 析构不依赖日志器
//   - 加锁顺序统一
//   - 时钟回拨处理
//   - 跨平台线程命名
//   - 优雅关闭（先 join 后广播 SHUTDOWN）
// ==============================================================================

#include "event/event.core.bus.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

// 平台特定：线程命名
#if defined(__linux__)
    #include <pthread.h>
#endif

namespace quant::event {

// ==============================================================================
// EventBus 构造 / 析构
// ==============================================================================
EventBus::EventBus() = default;

EventBus::~EventBus() {
    // 优雅关闭，避免资源泄漏
    // 注意：析构路径不依赖日志器（可能已先销毁）
    if (running_.load(std::memory_order_acquire)) {
        // 直接停止线程，不记录日志
        running_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(dispatch_mutex_);
            dispatch_cv_.notify_all();
        }
        for (auto& t : dispatch_threads_) {
            if (t.joinable()) {
                try { t.join(); } catch (...) {}
            }
        }
        dispatch_threads_.clear();
    }
}

// ==============================================================================
// 单例
// ==============================================================================
EventBus& EventBus::instance() noexcept {
    // Meyers 单例：C++11 起线程安全
    static EventBus bus;
    return bus;
}

// ==============================================================================
// 内部：线程命名（跨平台）
// ==============================================================================
namespace {

void set_thread_name(std::string_view name) noexcept {
    // Linux: 名称最长 16 字节（含 '\0'）
    constexpr std::size_t kMaxLen = 15;
    std::string truncated{name.substr(0, kMaxLen)};

#if defined(__linux__)
    // Linux: pthread_setname_np(pthread_t, const char*)
    ::pthread_setname_np(::pthread_self(), truncated.c_str());
#elif defined(__APPLE__)
    // macOS: pthread_setname_np(const char*)，仅设置当前线程
    ::pthread_setname_np(truncated.c_str());
#elif defined(_WIN32)
    // Windows: SetThreadDescription（需要 <windows.h>，此处省略）
    (void)truncated;
#else
    (void)truncated;
#endif
}

// 计算合理的分发线程数（不超过硬件核心数）
std::size_t clamp_thread_count(std::size_t requested) noexcept {
    const auto hw = std::thread::hardware_concurrency();
    const auto max_threads = (hw == 0) ? 4u : hw;
    const auto n = std::max<std::size_t>(1, requested);
    return std::min<std::size_t>(n, max_threads);
}

}  // namespace

// ==============================================================================
// initialize
// ==============================================================================
Result<void> EventBus::initialize(const Config& cfg) noexcept {
    // 幂等：已初始化则直接返回
    if (initialized_.exchange(true, std::memory_order_acq_rel)) {
        return {};
    }

    try {
        config_ = cfg;

        // 校验配置
        if (config_.queue_capacity < 64) {
            config_.queue_capacity = 64;
        }

        // 限制线程数不超过硬件核心
        config_.dispatch_threads = clamp_thread_count(config_.dispatch_threads);

        // 初始化队列（4 个优先级）
        for (std::size_t i = 0; i < kNumPriorities; ++i) {
            queues_[i] = std::make_unique<Queue>();
            if (!queues_[i]) {
                // 失败：重置 initialized_ 并清理已创建资源
                initialized_.store(false, std::memory_order_release);
                for (std::size_t j = 0; j < i; ++j) {
                    queues_[j].reset();
                }
                return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
                    "事件队列分配失败");
            }
        }

        // 初始化订阅者分片
        for (std::size_t i = 0; i < kNumEventTypes; ++i) {
            subscribers_[i] = std::make_unique<TypeSubscribers>();
            if (!subscribers_[i]) {
                initialized_.store(false, std::memory_order_release);
                for (std::size_t j = 0; j < i; ++j) {
                    subscribers_[j].reset();
                }
                for (auto& q : queues_) q.reset();
                return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
                    "订阅者分片分配失败");
            }
        }

        stats_.queue_high_watermark.store(config_.queue_capacity,
            std::memory_order_relaxed);

        QUANT_LOG_INFO("事件总线已初始化: queue_capacity={}, threads={}",
                       config_.queue_capacity, config_.dispatch_threads);

        return {};
    } catch (const std::exception& e) {
        // 异常：重置 initialized_，避免后续误判
        initialized_.store(false, std::memory_order_release);
        for (auto& q : queues_) q.reset();
        for (auto& s : subscribers_) s.reset();
        QUANT_LOG_ERROR("事件总线初始化异常: {}", e.what());
        return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
            std::string{"初始化异常: "} + e.what());
    } catch (...) {
        initialized_.store(false, std::memory_order_release);
        for (auto& q : queues_) q.reset();
        for (auto& s : subscribers_) s.reset();
        QUANT_LOG_ERROR("事件总线初始化未知异常");
        return QUANT_ERR_MSG(ErrorCode::InternalUnknown,
            "初始化未知异常");
    }
}

// ==============================================================================
// start
// ==============================================================================
Result<void> EventBus::start() noexcept {
    // 前置检查：必须已初始化
    if (!initialized_.load(std::memory_order_acquire)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "事件总线未初始化，无法启动");
    }

    // 幂等：已启动则直接返回
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return {};
    }

    const auto n = config_.dispatch_threads;
    dispatch_threads_.reserve(n);

    try {
        for (std::size_t i = 0; i < n; ++i) {
            dispatch_threads_.emplace_back([this, i] {
                // 设置线程名称（便于 ps/top 识别）
                std::string name = "evt_disp_" + std::to_string(i);
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
            std::lock_guard lock(dispatch_mutex_);
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
    }
}

// ==============================================================================
// stop
// ==============================================================================
Result<void> EventBus::stop() noexcept {
    // 幂等：未运行则直接返回
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return {};
    }

    // 通知所有等待线程退出
    {
        std::lock_guard lock(dispatch_mutex_);
        dispatch_cv_.notify_all();
    }

    // 等待所有分发线程退出
    for (auto& t : dispatch_threads_) {
        if (t.joinable()) {
            try {
                t.join();
            } catch (...) {
                // join 异常忽略（如线程已被 join）
            }
        }
    }
    dispatch_threads_.clear();

    // 所有线程已停止后，同步广播 SHUTDOWN 事件
    // 此时不会有新线程启动，确保订阅者收到关闭通知
    try {
        Event shutdown_event{
            EventType::SHUTDOWN,
            EmptyPayload{},
            EventPriority::CRITICAL};
        dispatch_sync(shutdown_event);
    } catch (...) {
        // 忽略：关闭阶段的异常不影响流程
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

    auto& q = queue_for(event.priority);

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

    // 唤醒分发线程
    dispatch_cv_.notify_one();

    return {};
}

// ==============================================================================
// get_or_create（内部）
// ==============================================================================
EventBus::TypeSubscribers&
EventBus::get_or_create(EventType type) noexcept {
    const auto idx = static_cast<std::size_t>(type) % kNumEventTypes;
    return *subscribers_[idx];
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

        // 加锁顺序：先 subscriptions_mutex_，后 ts.mutex
        // 与 unsubscribe 保持一致，避免死锁
        {
            std::unique_lock sub_lock(subscriptions_mutex_);

            {
                std::unique_lock lock(ts.mutex);

                if (ts.subscribers.size() >= config_.max_subscribers_per_type) {
                    QUANT_LOG_ERROR("订阅者数量已达上限: {} ({})",
                                    to_string(type),
                                    config_.max_subscribers_per_type);
                    return {};
                }

                // reserve 避免 push_back 抛异常导致的 ID 泄漏
                ts.subscribers.reserve(ts.subscribers.size() + 1);
                ts.subscribers.push_back(
                    Subscriber{id, std::move(handler), options});
            }

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
// unsubscribe
// ==============================================================================
void EventBus::unsubscribe(uint64_t id) noexcept {
    if (id == 0) return;

    try {
        // 查询订阅类型
        EventType type = EventType::UNKNOWN;
        {
            std::shared_lock lock(subscriptions_mutex_);
            auto it = subscriptions_.find(id);
            if (it == subscriptions_.end()) return;
            type = it->second.type;
        }

        auto& ts = get_or_create(type);

        // 加锁顺序：先 subscriptions_mutex_，后 ts.mutex
        // 与 subscribe 保持一致
        std::unique_lock sub_lock(subscriptions_mutex_);
        std::unique_lock ts_lock(ts.mutex);

        auto it = std::remove_if(ts.subscribers.begin(), ts.subscribers.end(),
            [id](const Subscriber& s) { return s.id == id; });
        ts.subscribers.erase(it, ts.subscribers.end());

        subscriptions_.erase(id);

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

        // 按优先级从高到低轮询（CRITICAL → HIGH → NORMAL → LOW）
        for (std::size_t i = kNumPriorities; i-- > 0; ) {
            Event event;
            if (queues_[i]->pop(event)) {
                dispatch_one(event);
                dispatched = true;
            }
        }

        if (!dispatched) {
            // 无事件：短暂等待
            std::unique_lock lock(dispatch_mutex_);
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
    // 延迟统计
    const auto now = Timestamp::now();
    const auto latency_us = (now - event.timestamp);

    // 时钟回拨保护：仅统计非负延迟
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

    // 拷贝订阅者列表（避免与 subscribe/unsubscribe 竞态）
    std::vector<Subscriber> snapshot;
    try {
        auto& ts = get_or_create(event.type);
        std::shared_lock lock(ts.mutex);
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

    // 分发（异常隔离）
    for (auto& sub : snapshot) {
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

        // 一次性订阅：分发后取消
        if (sub.options.once) {
            unsubscribe(sub.id);
        }
    }

    stats_.dispatched_total.fetch_add(1, std::memory_order_relaxed);
}

// ==============================================================================
// dispatch_sync（同步分发，用于关闭时广播）
// ==============================================================================
void EventBus::dispatch_sync(const Event& event) noexcept {
    try {
        // 拷贝事件（因为 dispatch_one 接收非 const 引用）
        Event copy = event;
        dispatch_one(copy);
    } catch (...) {
        // 忽略：同步分发路径不抛异常
    }
}

// ==============================================================================
// pending_events（查询）
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
std::string EventBus::dump() const {
    try {
        std::string result;
        result.reserve(512);

        result += "EventBus dump:\n";
        result += "  running: ";
        result += running_.load() ? "yes" : "no";
        result += "\n";
        result += "  initialized: ";
        result += initialized_.load() ? "yes" : "no";
        result += "\n";
        result += "  dispatch_threads: ";
        result += std::to_string(dispatch_threads_.size());
        result += "\n";
        result += "  pending_events: ";
        result += std::to_string(pending_events());
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

}  // namespace quant::event
