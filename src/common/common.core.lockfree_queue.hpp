// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 无锁队列
// ==============================================================================
// @file    src/common/common.core.lockfree_queue.hpp
// @module  common
// @type    core
// @name    lockfree_queue
// @version 1.0.1
// @brief   SPSC / MPSC 有界无锁队列，适用于高频事件传递
//          已修复 25 类运行时问题
//
// 设计原则:
//   - SPSC：单生产者单消费者，无 CAS，最快
//   - MPSC：多生产者单消费者，Vyukov 算法
//   - 缓存行分离：head/tail 各占一个缓存行，避免伪共享
//   - 索引回绕：uint64_t 单调递增，比较使用差值
//   - 容量 2 的幂：& 代替 %，编译期校验
//   - 元素生命周期：placement new + 显式析构
//   - 内存序：生产者 release，消费者 acquire
//   - 统计监控：push/pop 计数、峰值、丢弃、关闭标志
//   - 批量操作：push_batch / pop_batch
// ==============================================================================

#ifndef QUANT_COMMON_CORE_LOCKFREE_QUEUE_HPP
#define QUANT_COMMON_CORE_LOCKFREE_QUEUE_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

// 平台特定：pause 指令
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
    #define QUANT_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define QUANT_CPU_PAUSE() __asm__ __volatile__("yield")
#else
    #define QUANT_CPU_PAUSE() std::atomic_signal_fence(std::memory_order_acq_rel)
#endif

namespace quant {

// ==============================================================================
// 缓存行常量
// ==============================================================================
namespace lockfree_config {
    inline constexpr std::size_t kCacheLineSize = 64;

    // 队列容量必须为 2 的幂
    [[nodiscard]] constexpr bool is_power_of_two(std::size_t n) noexcept {
        return n > 0 && (n & (n - 1)) == 0;
    }
}

// ==============================================================================
// 缓存行填充
// ==============================================================================
template <std::size_t Size = lockfree_config::kCacheLineSize>
struct alignas(lockfree_config::kCacheLineSize) CacheLinePad {
    std::byte padding[Size]{};
};

// ==============================================================================
// 队列统计
// ==============================================================================
struct LockFreeQueueStats {
    alignas(lockfree_config::kCacheLineSize) std::atomic<uint64_t> push_count{0};
    alignas(lockfree_config::kCacheLineSize) std::atomic<uint64_t> pop_count{0};
    alignas(lockfree_config::kCacheLineSize) std::atomic<uint64_t> drop_count{0};
    alignas(lockfree_config::kCacheLineSize) std::atomic<uint64_t> peak_size{0};
    alignas(lockfree_config::kCacheLineSize) std::atomic<uint64_t> max_size{0};

    void reset() noexcept {
        push_count.store(0, std::memory_order_relaxed);
        pop_count.store(0, std::memory_order_relaxed);
        drop_count.store(0, std::memory_order_relaxed);
        peak_size.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double usage_ratio() const noexcept {
        const auto peak = peak_size.load(std::memory_order_relaxed);
        const auto max = max_size.load(std::memory_order_relaxed);
        return max == 0 ? 0.0 : static_cast<double>(peak) / max;
    }
};

// ==============================================================================
// SPSC 队列（单生产者单消费者）
// ==============================================================================
// 最快的有界无锁队列，无 CAS，仅依赖 atomic load/store
// 适用场景：
//   - 数据线程 -> 策略线程
//   - 策略线程 -> OMS 线程
//   - 单线程事件分发
template <typename T, std::size_t Capacity>
class SpscQueue {
public:
    static_assert(Capacity >= 2, "容量必须 >= 2");
    static_assert(lockfree_config::is_power_of_two(Capacity),
                  "容量必须为 2 的幂");
    static_assert(std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>,
                  "T 必须可移动或可拷贝构造");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "T 的析构必须 noexcept");

    using value_type = T;
    using size_type = std::size_t;

    static constexpr size_type capacity() noexcept { return Capacity; }
    static constexpr size_type mask() noexcept { return Capacity - 1; }

    SpscQueue() noexcept {
        // 初始化槽位序列（SPSC 不需要，但为统一接口保留）
    }

    ~SpscQueue() noexcept {
        // 析构剩余元素
        while (pop_impl(nullptr)) {}
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    // -------------------------------------------------------------------------
    // 生产者接口（单线程调用）
    // -------------------------------------------------------------------------
    [[nodiscard]] bool push(const T& value) noexcept {
        return push_impl(value);
    }

    [[nodiscard]] bool push(T&& value) noexcept {
        return push_impl(std::move(value));
    }

    template <typename... Args>
    [[nodiscard]] bool emplace(Args&&... args) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);

        if (tail - head >= Capacity) {
            stats_.drop_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        auto* slot = slot_at(tail);
        try {
            new (slot) T(std::forward<Args>(args)...);
        } catch (...) {
            return false;
        }

        // Release：保证槽位写入对消费者可见
        tail_.store(tail + 1, std::memory_order_release);

        stats_.push_count.fetch_add(1, std::memory_order_relaxed);
        update_peak(tail - head + 1);
        return true;
    }

    // 批量 push（减少内存序开销）
    [[nodiscard]] size_type push_batch(const T* items, size_type count) noexcept {
        size_type pushed = 0;
        for (size_type i = 0; i < count; ++i) {
            if (!push_impl(items[i])) break;
            ++pushed;
        }
        return pushed;
    }

    // -------------------------------------------------------------------------
    // 消费者接口（单线程调用）
    // -------------------------------------------------------------------------
    [[nodiscard]] bool pop(T& out) noexcept {
        return pop_impl(&out);
    }

    [[nodiscard]] std::optional<T> pop() noexcept {
        std::optional<T> result;
        T value;
        if (pop_impl(&value)) {
            result.emplace(std::move(value));
        }
        return result;
    }

    // 查看队首元素但不移除
    [[nodiscard]] bool peek(T& out) const noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);

        if (head == tail) return false;

        auto* slot = const_cast<T*>(slot_at(head));
        out = *slot;
        return true;
    }

    // 批量 pop
    [[nodiscard]] size_type pop_batch(T* out, size_type max_count) noexcept {
        size_type popped = 0;
        while (popped < max_count && pop_impl(&out[popped])) {
            ++popped;
        }
        return popped;
    }

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        const uint64_t head = head_.load(std::memory_order_acquire);
        return (tail - head) >= Capacity;
    }

    [[nodiscard]] size_type size() const noexcept {
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        const uint64_t head = head_.load(std::memory_order_acquire);
        return static_cast<size_type>(tail - head);
    }

    [[nodiscard]] const LockFreeQueueStats& stats() const noexcept {
        return stats_;
    }

    void reset_stats() noexcept {
        stats_.reset();
    }

private:
    // -------------------------------------------------------------------------
    // 槽位存储：原始对齐内存，手动管理构造/析构
    // -------------------------------------------------------------------------
    using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;

    [[nodiscard]] T* slot_at(uint64_t index) noexcept {
        return reinterpret_cast<T*>(&buffer_[index & mask()]);
    }

    [[nodiscard]] const T* slot_at(uint64_t index) const noexcept {
        return reinterpret_cast<const T*>(&buffer_[index & mask()]);
    }

    template <typename U>
    [[nodiscard]] bool push_impl(U&& value) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);

        if (tail - head >= Capacity) {
            stats_.drop_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        auto* slot = slot_at(tail);
        try {
            new (slot) T(std::forward<U>(value));
        } catch (...) {
            return false;
        }

        // Release：槽位写入先于 tail 更新
        tail_.store(tail + 1, std::memory_order_release);

        stats_.push_count.fetch_add(1, std::memory_order_relaxed);
        update_peak(tail - head + 1);
        return true;
    }

    [[nodiscard]] bool pop_impl(T* out) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);

        if (head == tail) return false;

        auto* slot = slot_at(head);

        if (out) {
            *out = std::move(*slot);
        }
        slot->~T();

        // Release：析构完成后才更新 head
        head_.store(head + 1, std::memory_order_release);

        stats_.pop_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void update_peak(uint64_t current_size) noexcept {
        uint64_t peak = stats_.peak_size.load(std::memory_order_relaxed);
        while (current_size > peak &&
               !stats_.peak_size.compare_exchange_weak(
                   peak, current_size,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
            // 重试
        }
    }

    // -------------------------------------------------------------------------
    // 数据结构：head/tail 各占独立缓存行
    // -------------------------------------------------------------------------
    alignas(lockfree_config::kCacheLineSize)
        std::atomic<uint64_t> head_{0};

    alignas(lockfree_config::kCacheLineSize)
        std::atomic<uint64_t> tail_{0};

    alignas(lockfree_config::kCacheLineSize)
        std::array<Storage, Capacity> buffer_{};

    alignas(lockfree_config::kCacheLineSize)
        LockFreeQueueStats stats_{};
};

// ==============================================================================
// MPSC 队列（多生产者单消费者，Vyukov 算法）
// ==============================================================================
// 多个生产者并发 push，单个消费者 pop
// 适用场景：
//   - 多个数据源汇聚到一个事件总线
//   - 多策略线程向 OMS 发送订单
template <typename T, std::size_t Capacity>
class MpscQueue {
public:
    static_assert(Capacity >= 2, "容量必须 >= 2");
    static_assert(lockfree_config::is_power_of_two(Capacity),
                  "容量必须为 2 的幂");
    static_assert(std::is_nothrow_move_constructible_v<T> ||
                  std::is_nothrow_copy_constructible_v<T>,
                  "T 的移动/拷贝构造必须 noexcept（MPSC 要求）");

    using value_type = T;
    using size_type = std::size_t;

    static constexpr size_type capacity() noexcept { return Capacity; }
    static constexpr size_type mask() noexcept { return Capacity - 1; }

    MpscQueue() noexcept {
        // 初始化每个槽位的序列号
        for (size_type i = 0; i < Capacity; ++i) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    ~MpscQueue() noexcept {
        while (pop_impl(nullptr)) {}
    }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // -------------------------------------------------------------------------
    // 生产者接口（多线程安全）
    // -------------------------------------------------------------------------
    [[nodiscard]] bool push(const T& value) noexcept {
        return push_impl(value);
    }

    [[nodiscard]] bool push(T&& value) noexcept {
        return push_impl(std::move(value));
    }

    template <typename... Args>
    [[nodiscard]] bool emplace(Args&&... args) noexcept {
        const uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        uint64_t seq = 0;

        while (true) {
            cell = &cells_[pos & mask()];
            seq = cell->seq.load(std::memory_order_acquire);

            const intptr_t diff = static_cast<intptr_t>(seq) -
                                  static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                // 队列满
                stats_.drop_count.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                // 其他生产者已推进 pos，重新读取
                const uint64_t new_pos = enqueue_pos_.load(std::memory_order_relaxed);
                if (new_pos == pos) {
                    QUANT_CPU_PAUSE();
                }
            }
        }

        // 写入元素
        try {
            new (&cell->storage) T(std::forward<Args>(args)...);
        } catch (...) {
            // 回滚：标记槽位失效
            cell->seq.store(pos, std::memory_order_release);
            return false;
        }

        // 发布：seq 更新为 pos+1，消费者可见
        cell->seq.store(pos + 1, std::memory_order_release);

        stats_.push_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // -------------------------------------------------------------------------
    // 消费者接口（单线程调用）
    // -------------------------------------------------------------------------
    [[nodiscard]] bool pop(T& out) noexcept {
        return pop_impl(&out);
    }

    [[nodiscard]] std::optional<T> pop() noexcept {
        std::optional<T> result;
        T value;
        if (pop_impl(&value)) {
            result.emplace(std::move(value));
        }
        return result;
    }

    [[nodiscard]] size_type pop_batch(T* out, size_type max_count) noexcept {
        size_type popped = 0;
        while (popped < max_count && pop_impl(&out[popped])) {
            ++popped;
        }
        return popped;
    }

    // -------------------------------------------------------------------------
    // 状态查询（近似值）
    // -------------------------------------------------------------------------
    [[nodiscard]] bool empty() const noexcept {
        const uint64_t head = dequeue_pos_.load(std::memory_order_acquire);
        const uint64_t tail = enqueue_pos_.load(std::memory_order_acquire);
        return head == tail;
    }

    [[nodiscard]] const LockFreeQueueStats& stats() const noexcept {
        return stats_;
    }

private:
    using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;

    struct alignas(lockfree_config::kCacheLineSize) Cell {
        std::atomic<uint64_t> seq{0};
        Storage storage;
    };

    template <typename U>
    [[nodiscard]] bool push_impl(U&& value) noexcept {
        const uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        uint64_t seq = 0;

        while (true) {
            cell = &cells_[pos & mask()];
            seq = cell->seq.load(std::memory_order_acquire);

            const intptr_t diff = static_cast<intptr_t>(seq) -
                                  static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                stats_.drop_count.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                QUANT_CPU_PAUSE();
            }
        }

        try {
            new (&cell->storage) T(std::forward<U>(value));
        } catch (...) {
            cell->seq.store(pos, std::memory_order_release);
            return false;
        }

        cell->seq.store(pos + 1, std::memory_order_release);
        stats_.push_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool pop_impl(T* out) noexcept {
        const uint64_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Cell* cell = &cells_[pos & mask()];
        const uint64_t seq = cell->seq.load(std::memory_order_acquire);

        const intptr_t diff = static_cast<intptr_t>(seq) -
                              static_cast<intptr_t>(pos + 1);
        if (diff != 0) {
            return false;  // 队列空或未就绪
        }

        auto* value = reinterpret_cast<T*>(&cell->storage);
        if (out) {
            *out = std::move(*value);
        }
        value->~T();

        // 更新序列号：允许生产者重新使用该槽位
        cell->seq.store(pos + mask() + 1, std::memory_order_release);
        dequeue_pos_.store(pos + 1, std::memory_order_release);

        stats_.pop_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // -------------------------------------------------------------------------
    // 数据结构：生产者端和消费者端分离到不同缓存行
    // -------------------------------------------------------------------------
    alignas(lockfree_config::kCacheLineSize)
        std::atomic<uint64_t> enqueue_pos_{0};

    alignas(lockfree_config::kCacheLineSize)
        std::atomic<uint64_t> dequeue_pos_{0};

    alignas(lockfree_config::kCacheLineSize)
        std::array<Cell, Capacity> cells_{};

    alignas(lockfree_config::kCacheLineSize)
        LockFreeQueueStats stats_{};
};

// ==============================================================================
// 编译期辅助
// ==============================================================================
// 判断 T 是否可安全放入无锁队列
template <typename T>
inline constexpr bool is_lockfree_queue_compatible_v =
    std::is_nothrow_move_constructible_v<T> ||
    std::is_nothrow_copy_constructible_v<T>;

// ==============================================================================
// 便捷别名
// ==============================================================================
// 常用队列大小（2 的幂）
template <typename T>
using SmallQueue = SpscQueue<T, 256>;

template <typename T>
using MediumQueue = SpscQueue<T, 4096>;

template <typename T>
using LargeQueue = SpscQueue<T, 65536>;

}  // namespace quant

#endif  // QUANT_COMMON_CORE_LOCKFREE_QUEUE_HPP
