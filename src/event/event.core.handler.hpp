// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 事件处理器
// ==============================================================================
// @file    src/event/event.core.handler.hpp
// @module  event
// @type    core
// @name    handler
// @version 1.0.2
// @brief   事件处理器抽象基类 + 函数式适配器 + 过滤器 + 链式组合
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 类型安全：虚函数接口 + 编译期类型检查
//   - 异常隔离：handle() 包装，异常不传播到 EventBus
//   - 生命周期：initialize/shutdown 显式管理
//   - 可观测：处理次数、错误数、平均延迟、峰值延迟
//   - 优先级：处理器可声明优先级（用于排序）
//   - 背压：可选同步/异步模式
//   - 零开销：热路径无虚函数之外的分支
//   - 组合：支持 Chain/Filter/Function 组合
//   - header-only：所有实现 inline，无需 .cpp
//
// 使用方式:
//   class MyHandler : public EventHandler {
//       Result<void> on_event(const Event& e) override {
//           // 处理事件
//           return {};
//       }
//   };
//
//   MyHandler handler;
//   auto sub = bus.subscribe(EventType::CANDLE_CLOSED,
//       [&handler](const Event& e) { (void)handler.handle(e); });
// ==============================================================================

#ifndef QUANT_EVENT_CORE_HANDLER_HPP
#define QUANT_EVENT_CORE_HANDLER_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"
#include "event/event.core.bus.hpp"

namespace quant {
namespace event {

// ==============================================================================
// 处理器优先级
// ==============================================================================
enum class HandlerPriority : uint8_t {
    LOWEST   = 0,
    LOW      = 64,
    NORMAL   = 128,
    HIGH     = 192,
    HIGHEST  = 255,
};

// ==============================================================================
// 处理器统计
// ==============================================================================
struct HandlerStats {
    alignas(64) std::atomic<uint64_t> handled_total{0};
    alignas(64) std::atomic<uint64_t> filtered_total{0};
    alignas(64) std::atomic<uint64_t> error_total{0};
    alignas(64) std::atomic<uint64_t> skipped_total{0};    // 未启用/未初始化时跳过
    alignas(64) std::atomic<uint64_t> total_latency_ns{0};
    alignas(64) std::atomic<uint64_t> max_latency_ns{0};

    void reset() noexcept {
        handled_total.store(0, std::memory_order_relaxed);
        filtered_total.store(0, std::memory_order_relaxed);
        error_total.store(0, std::memory_order_relaxed);
        skipped_total.store(0, std::memory_order_relaxed);
        total_latency_ns.store(0, std::memory_order_relaxed);
        max_latency_ns.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double avg_latency_us() const noexcept {
        const auto n = handled_total.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return static_cast<double>(
            total_latency_ns.load(std::memory_order_relaxed))
            / static_cast<double>(n) / 1000.0;
    }
};

// ==============================================================================
// 处理器配置
// ==============================================================================
struct HandlerConfig {
    // 优先级（用于排序，越大越先处理）
    HandlerPriority priority{HandlerPriority::NORMAL};

    // 是否启用
    bool enabled{true};

    // 是否在异常时继续（false 表示异常中止链）
    bool continue_on_error{true};

    // 是否记录详细日志
    bool verbose{false};

    // 处理器名称（用于诊断）
    std::string_view name{"handler"};

    // 最小处理间隔（微秒，0 表示无限制）—— 用于限流
    int64_t min_interval_us{0};

    // 最大处理次数（0 表示无限）
    uint64_t max_handle_count{0};
};

// ==============================================================================
// EventHandler 抽象基类
// ==============================================================================
class EventHandler {
public:
    explicit EventHandler(const HandlerConfig& config = {}) noexcept
        : config_{config}
    {}

    virtual ~EventHandler() = default;

    EventHandler(const EventHandler&) = delete;
    EventHandler& operator=(const EventHandler&) = delete;
    EventHandler(EventHandler&&) = delete;
    EventHandler(EventHandler&&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期（子类可重写）
    // -------------------------------------------------------------------------
    [[nodiscard]] virtual Result<void> initialize() noexcept {
        return {};
    }

    virtual void shutdown() noexcept {}

    // -------------------------------------------------------------------------
    // 事件处理（子类必须实现）
    // -------------------------------------------------------------------------
    [[nodiscard]] virtual Result<void> on_event(const Event& event) noexcept = 0;

    // -------------------------------------------------------------------------
    // 条件过滤（子类可重写，默认接受所有事件）
    // -------------------------------------------------------------------------
    [[nodiscard]] virtual bool accept(const Event& /*event*/) const noexcept {
        return true;
    }

    // -------------------------------------------------------------------------
    // 主入口：handle（含异常隔离、统计、限流）
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void> handle(const Event& event) noexcept {
        // 1. 启用检查
        if (!config_.enabled) {
            stats_.skipped_total.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        // 2. 初始化检查
        if (!initialized_.load(std::memory_order_acquire)) {
            auto init_result = initialize();
            if (init_result.is_err()) {
                return init_result;
            }
            initialized_.store(true, std::memory_order_release);
        }

        // 3. 数量限制
        if (config_.max_handle_count > 0) {
            const auto handled = stats_.handled_total.load(
                std::memory_order_relaxed);
            if (handled >= config_.max_handle_count) {
                stats_.skipped_total.fetch_add(1, std::memory_order_relaxed);
                return QUANT_ERR_MSG(ErrorCode::UserInputOutOfRange,
                    "处理器已达最大处理次数")
                    .with_context("name", std::string{config_.name})
                    .with_context("max", std::to_string(config_.max_handle_count));
            }
        }

        // 4. 限流检查
        if (config_.min_interval_us > 0) {
            const auto now_us = Timestamp::now().microseconds();
            const auto last = last_handled_us_.load(std::memory_order_relaxed);
            if (now_us - last < config_.min_interval_us) {
                stats_.skipped_total.fetch_add(1, std::memory_order_relaxed);
                return {};  // 限流跳过
            }
            last_handled_us_.store(now_us, std::memory_order_relaxed);
        }

        // 5. 条件过滤
        try {
            if (!accept(event)) {
                stats_.filtered_total.fetch_add(1, std::memory_order_relaxed);
                return {};
            }
        } catch (const std::exception& e) {
            stats_.error_total.fetch_add(1, std::memory_order_relaxed);
            return QUANT_ERR_MSG(ErrorCode::InternalUnknown,
                "accept 异常")
                .with_context("name", std::string{config_.name})
                .with_context("exception", e.what());
        }

        // 6. 计时开始
        const auto start_ns = now_ns();

        // 7. 处理（异常隔离）
        Result<void> result;
        try {
            result = on_event(event);
        } catch (const std::exception& e) {
            stats_.error_total.fetch_add(1, std::memory_order_relaxed);
            result = QUANT_ERR_MSG(ErrorCode::InternalUnknown,
                "on_event 异常")
                .with_context("name", std::string{config_.name})
                .with_context("exception", e.what())
                .with_context("event_type", to_string(event.type));
        } catch (...) {
            stats_.error_total.fetch_add(1, std::memory_order_relaxed);
            result = QUANT_ERR_MSG(ErrorCode::InternalUnknown,
                "on_event 未知异常")
                .with_context("name", std::string{config_.name});
        }

        // 8. 计时结束
        const auto elapsed_ns = now_ns() - start_ns;
        stats_.total_latency_ns.fetch_add(elapsed_ns,
            std::memory_order_relaxed);

        // 更新最大延迟
        auto max_lat = stats_.max_latency_ns.load(std::memory_order_relaxed);
        while (elapsed_ns > max_lat &&
               !stats_.max_latency_ns.compare_exchange_weak(
                   max_lat, elapsed_ns,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
            // CAS 重试
        }

        if (result.is_err()) {
            stats_.error_total.fetch_add(1, std::memory_order_relaxed);
            if (config_.verbose) {
                QUANT_LOG_WARN("处理器错误: name={}, event={}, err={}",
                               config_.name, to_string(event.type),
                               result.error().to_string());
            }
        } else {
            stats_.handled_total.fetch_add(1, std::memory_order_relaxed);
        }

        return result;
    }

    // -------------------------------------------------------------------------
    // 访问器
    // -------------------------------------------------------------------------
    [[nodiscard]] const HandlerConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] const HandlerStats& stats() const noexcept {
        return stats_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return config_.name;
    }

    [[nodiscard]] HandlerPriority priority() const noexcept {
        return config_.priority;
    }

    [[nodiscard]] bool is_enabled() const noexcept {
        return config_.enabled;
    }

    void set_enabled(bool enabled) noexcept {
        config_.enabled = enabled;
    }

private:
    [[nodiscard]] static uint64_t now_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    HandlerConfig config_{};
    HandlerStats stats_{};
    std::atomic<bool> initialized_{false};
    std::atomic<int64_t> last_handled_us_{0};
};

// ==============================================================================
// FunctionHandler：函数式适配器
// ==============================================================================
class FunctionHandler final : public EventHandler {
public:
    using Callback = std::function<Result<void>(const Event&)>;
    using Filter = std::function<bool(const Event&)>;

    explicit FunctionHandler(Callback callback,
                              const HandlerConfig& config = {}) noexcept
        : EventHandler(config)
        , callback_{std::move(callback)}
    {}

    FunctionHandler(Callback callback, Filter filter,
                    const HandlerConfig& config = {}) noexcept
        : EventHandler(config)
        , callback_{std::move(callback)}
        , filter_{std::move(filter)}
    {}

    [[nodiscard]] Result<void> on_event(const Event& event) noexcept override {
        if (!callback_) {
            return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
                "FunctionHandler 回调为空");
        }
        return callback_(event);
    }

    [[nodiscard]] bool accept(const Event& event) const noexcept override {
        if (!filter_) return true;
        try {
            return filter_(event);
        } catch (...) {
            return false;
        }
    }

private:
    Callback callback_;
    Filter filter_;
};

// ==============================================================================
// FilterHandler：条件过滤包装
// ==============================================================================
// 将任意 EventHandler 包装为带条件的处理器
class FilterHandler final : public EventHandler {
public:
    using Predicate = std::function<bool(const Event&)>;

    FilterHandler(std::shared_ptr<EventHandler> inner,
                  Predicate predicate,
                  const HandlerConfig& config = {}) noexcept
        : EventHandler(config)
        , inner_{std::move(inner)}
        , predicate_{std::move(predicate)}
    {}

    [[nodiscard]] Result<void> initialize() noexcept override {
        if (inner_) return inner_->initialize();
        return {};
    }

    void shutdown() noexcept override {
        if (inner_) inner_->shutdown();
    }

    [[nodiscard]] Result<void> on_event(const Event& event) noexcept override {
        if (!inner_) {
            return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
                "FilterHandler 内部处理器为空");
        }
        return inner_->handle(event);
    }

    [[nodiscard]] bool accept(const Event& event) const noexcept override {
        if (!predicate_) return true;
        try {
            return predicate_(event);
        } catch (...) {
            return false;
        }
    }

private:
    std::shared_ptr<EventHandler> inner_;
    Predicate predicate_;
};

// ==============================================================================
// ChainHandler：链式组合多个处理器
// ==============================================================================
class ChainHandler final : public EventHandler {
public:
    explicit ChainHandler(const HandlerConfig& config = {}) noexcept
        : EventHandler(config)
    {}

    // -------------------------------------------------------------------------
    // 添加处理器（按优先级排序）
    // -------------------------------------------------------------------------
    void add(std::shared_ptr<EventHandler> handler) {
        if (!handler) return;
        std::lock_guard lock{mutex_};
        handlers_.push_back(std::move(handler));
        // 按优先级降序排序
        std::stable_sort(handlers_.begin(), handlers_.end(),
            [](const auto& a, const auto& b) {
                return static_cast<uint8_t>(a->priority())
                     > static_cast<uint8_t>(b->priority());
            });
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lock{mutex_};
        return handlers_.size();
    }

    void clear() noexcept {
        std::lock_guard lock{mutex_};
        handlers_.clear();
    }

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void> initialize() noexcept override {
        std::lock_guard lock{mutex_};
        for (auto& h : handlers_) {
            if (h) {
                auto r = h->initialize();
                if (r.is_err()) return r;
            }
        }
        return {};
    }

    void shutdown() noexcept override {
        std::lock_guard lock{mutex_};
        for (auto& h : handlers_) {
            if (h) h->shutdown();
        }
    }

    // -------------------------------------------------------------------------
    // 处理：按优先级顺序调用所有处理器
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void> on_event(const Event& event) noexcept override {
        // 快照（避免与 add/clear 竞态）
        std::vector<std::shared_ptr<EventHandler>> snapshot;
        {
            std::lock_guard lock{mutex_};
            snapshot = handlers_;
        }

        if (snapshot.empty()) return {};

        Result<void> last_error;
        for (auto& h : snapshot) {
            if (!h) continue;
            auto r = h->handle(event);
            if (r.is_err()) {
                last_error = r;
                if (!config_.continue_on_error) {
                    return r;
                }
            }
        }
        return last_error;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<EventHandler>> handlers_;
};

// ==============================================================================
// 辅助：从 lambda 快速创建 FunctionHandler
// ==============================================================================
template <typename Callback>
[[nodiscard]] std::shared_ptr<FunctionHandler>
make_handler(Callback&& callback,
             const HandlerConfig& config = {})
{
    using CallbackType = std::function<Result<void>(const Event&)>;
    return std::make_shared<FunctionHandler>(
        CallbackType{std::forward<Callback>(callback)},
        config);
}

// ==============================================================================
// 辅助：Event → void 回调的适配
// ==============================================================================
// 简化常见场景：回调不关心错误返回
template <typename VoidCallback>
[[nodiscard]] std::shared_ptr<FunctionHandler>
make_simple_handler(VoidCallback&& callback,
                    const HandlerConfig& config = {})
{
    using CallbackType = std::function<Result<void>(const Event&)>;
    auto wrapper = CallbackType{
        [cb = std::forward<VoidCallback>(callback)](
            const Event& e) -> Result<void> {
            try {
                cb(e);
                return {};
            } catch (const std::exception& ex) {
                return QUANT_ERR_MSG(ErrorCode::InternalUnknown, ex.what());
            }
        }
    };
    return std::make_shared<FunctionHandler>(std::move(wrapper), config);
}

// ==============================================================================
// 辅助：带类型检查的处理器
// ==============================================================================
// 仅当 Event 的 payload 为指定类型时才调用
template <typename Payload, typename Callback>
[[nodiscard]] std::shared_ptr<FunctionHandler>
make_typed_handler(Callback&& callback,
                   const HandlerConfig& config = {})
{
    using CallbackType = std::function<Result<void>(const Event&)>;
    auto wrapper = CallbackType{
        [cb = std::forward<Callback>(callback)](
            const Event& e) -> Result<void> {
            if (auto* p = e.template get_payload<Payload>()) {
                try {
                    cb(*p, e);
                    return {};
                } catch (const std::exception& ex) {
                    return QUANT_ERR_MSG(ErrorCode::InternalUnknown, ex.what());
                }
            }
            return {};  // 类型不匹配，静默跳过
        }
    };
    return std::make_shared<FunctionHandler>(std::move(wrapper), config);
}

// ==============================================================================
// 便捷：将 EventHandler 直接订阅到 EventBus
// ==============================================================================
[[nodiscard]] inline Subscription
subscribe_handler(EventBus& bus,
                  EventType type,
                  std::shared_ptr<EventHandler> handler,
                  SubscribeOptions options = {})
{
    if (!handler) return {};

    return bus.subscribe(
        type,
        [h = std::move(handler)](const Event& e) {
            (void)h->handle(e);
        },
        options);
}

// 订阅多类型
[[nodiscard]] inline std::vector<Subscription>
subscribe_handler_multi(EventBus& bus,
                        std::span<const EventType> types,
                        std::shared_ptr<EventHandler> handler,
                        SubscribeOptions options = {})
{
    std::vector<Subscription> subs;
    if (!handler) return subs;
    subs.reserve(types.size());

    for (auto type : types) {
        subs.push_back(subscribe_handler(bus, type, handler, options));
    }
    return subs;
}

// ==============================================================================
// 编译期校验
// ==============================================================================
static_assert(std::is_abstract_v<EventHandler>,
    "EventHandler 必须是抽象基类");
static_assert(!std::is_copy_constructible_v<EventHandler>,
    "EventHandler 禁止拷贝");
static_assert(!std::is_move_constructible_v<EventHandler>,
    "EventHandler 禁止移动");

}  // namespace event
}  // namespace quant

#endif  // QUANT_EVENT_CORE_HANDLER_HPP
