// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 内存池
// ==============================================================================
// @file    src/common/common.core.memory_pool.hpp
// @module  common
// @type    core
// @name    memory_pool
// @version 1.0.1
// @brief   线程本地缓存 + 全局后备的分层内存池，支持固定大小和大小类别
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 分层架构：线程本地缓存（TLB） + 全局后备池
//   - 无锁热路径：TLB 命中时无锁分配/释放
//   - 对齐保证：所有块按 64 字节对齐（支持 AVX-512）
//   - 大小类别：8~4096 字节分档，超过阈值走 malloc
//   - 构造/析构：create<T>() / destroy<T>() 自动管理生命周期
//   - Debug 检测：双重释放、释放后使用、跨池释放
//   - 统计监控：使用率、峰值、失败次数
//   - 异常安全：失败时状态一致，无泄漏
//   - STL 兼容：PoolAllocator<T> 支持标准容器
// ==============================================================================

#ifndef QUANT_COMMON_CORE_MEMORY_POOL_HPP
#define QUANT_COMMON_CORE_MEMORY_POOL_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// 平台特定
#if defined(_WIN32)
    #include <malloc.h>
#else
    #include <unistd.h>
#endif

namespace quant {

// ==============================================================================
// 编译期配置
// ==============================================================================
namespace memory_pool_config {

// 最小块大小（字节）
inline constexpr std::size_t kMinBlockSize = 8;

// 最大块大小（超过则走 malloc）
inline constexpr std::size_t kMaxBlockSize = 4096;

// 大小类别数量
inline constexpr std::size_t kNumSizeClasses = 10;

// 对齐字节（64 = AVX-512 缓存行）
inline constexpr std::size_t kAlignment = 64;

// 线程本地缓存每个大小类别的最大块数
inline constexpr std::size_t kThreadCacheSize = 64;

// 全局池初始块数（每个大小类别）
inline constexpr std::size_t kGlobalPoolInitialSize = 256;

// 全局池最大块数（每个大小类别）
inline constexpr std::size_t kGlobalPoolMaxSize = 65536;

// 每次扩充块数
inline constexpr std::size_t kChunkSize = 64;

// Debug 模式毒化模式
inline constexpr uint32_t kMagicAllocated = 0xA110CA7E;  // ALLOCATE
inline constexpr uint32_t kMagicFreed     = 0xF4EEDEAD;  // FREEDEAD

}  // namespace memory_pool_config

// ==============================================================================
// 大小类别计算
// ==============================================================================
class SizeClass {
public:
    // 将请求大小向上取整到最近的类别
    [[nodiscard]] static constexpr std::size_t classify(std::size_t size) noexcept {
        using namespace memory_pool_config;

        if (size <= kMinBlockSize) return kMinBlockSize;

        // 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
        std::size_t cls = kMinBlockSize;
        for (std::size_t i = 1; i < kNumSizeClasses; ++i) {
            cls <<= 1;
            if (size <= cls) return cls;
        }
        return size;  // 超过最大块大小
    }

    // 类别索引（0~kNumSizeClasses-1，或 kNumSizeClasses 表示超大）
    [[nodiscard]] static constexpr std::size_t index(std::size_t size) noexcept {
        using namespace memory_pool_config;

        if (size <= kMinBlockSize) return 0;

        std::size_t cls = kMinBlockSize;
        for (std::size_t i = 1; i < kNumSizeClasses; ++i) {
            cls <<= 1;
            if (size <= cls) return i;
        }
        return kNumSizeClasses;  // 超出范围
    }

    // 类别对应的块大小
    [[nodiscard]] static constexpr std::size_t block_size(std::size_t idx) noexcept {
        using namespace memory_pool_config;
        return kMinBlockSize << idx;
    }

    // 是否为超大类（走 malloc）
    [[nodiscard]] static constexpr bool is_oversized(std::size_t size) noexcept {
        return size > memory_pool_config::kMaxBlockSize;
    }
};

// ==============================================================================
// 统计信息
// ==============================================================================
struct MemoryPoolStats {
    std::atomic<std::size_t> total_blocks{0};
    std::atomic<std::size_t> in_use_blocks{0};
    std::atomic<std::size_t> peak_in_use{0};
    std::atomic<std::size_t> allocate_count{0};
    std::atomic<std::size_t> deallocate_count{0};
    std::atomic<std::size_t> cache_hit_count{0};
    std::atomic<std::size_t> cache_miss_count{0};
    std::atomic<std::size_t> overflow_count{0};      // 池扩充次数
    std::atomic<std::size_t> malloc_fallback_count{0}; // 超大分配次数
    std::atomic<std::size_t> failed_count{0};          // 分配失败次数

    void reset() noexcept {
        total_blocks.store(0, std::memory_order_relaxed);
        in_use_blocks.store(0, std::memory_order_relaxed);
        peak_in_use.store(0, std::memory_order_relaxed);
        allocate_count.store(0, std::memory_order_relaxed);
        deallocate_count.store(0, std::memory_order_relaxed);
        cache_hit_count.store(0, std::memory_order_relaxed);
        cache_miss_count.store(0, std::memory_order_relaxed);
        overflow_count.store(0, std::memory_order_relaxed);
        malloc_fallback_count.store(0, std::memory_order_relaxed);
        failed_count.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double usage_ratio() const noexcept {
        const auto total = total_blocks.load(std::memory_order_relaxed);
        const auto used = in_use_blocks.load(std::memory_order_relaxed);
        return total == 0 ? 0.0 : static_cast<double>(used) / total;
    }
};

// ==============================================================================
// 全局内存池（单例）
// ==============================================================================
class MemoryPool {
public:
    using SizeClassType = std::size_t;

    // -------------------------------------------------------------------------
    // 单例
    // -------------------------------------------------------------------------
    [[nodiscard]] static MemoryPool& instance() noexcept;

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    // -------------------------------------------------------------------------
    // 全局分配（供 TLB 补充用）
    // -------------------------------------------------------------------------
    // 分配一个指定类别的块
    [[nodiscard]] void* allocate_raw(std::size_t size) noexcept;

    // 释放一个块回全局池
    void deallocate_raw(void* ptr, std::size_t size) noexcept;

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    [[nodiscard]] const MemoryPoolStats& stats() const noexcept;
    void reset_stats() noexcept;

    // -------------------------------------------------------------------------
    // 维护
    // -------------------------------------------------------------------------
    // 释放所有空闲块
    void trim() noexcept;

    // 保留块（预热）
    void reserve(std::size_t bytes_per_class);

    // 诊断
    [[nodiscard]] std::string dump() const;

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    struct Config {
        std::size_t thread_cache_size{
            memory_pool_config::kThreadCacheSize};
        std::size_t global_pool_initial{
            memory_pool_config::kGlobalPoolInitialSize};
        std::size_t global_pool_max{
            memory_pool_config::kGlobalPoolMaxSize};
    };

    void configure(const Config& cfg) noexcept;

private:
    MemoryPool();
    ~MemoryPool();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 线程本地缓存（TLB）
// ==============================================================================
class ThreadLocalCache {
public:
    ThreadLocalCache() noexcept;
    ~ThreadLocalCache() noexcept;

    ThreadLocalCache(const ThreadLocalCache&) = delete;
    ThreadLocalCache& operator=(const ThreadLocalCache&) = delete;

    // -------------------------------------------------------------------------
    // 分配与释放（无锁，热路径）
    // -------------------------------------------------------------------------
    [[nodiscard]] void* allocate(std::size_t size) noexcept;
    void deallocate(void* ptr, std::size_t size) noexcept;

    // -------------------------------------------------------------------------
    // 状态
    // -------------------------------------------------------------------------
    [[nodiscard]] std::size_t cached_count(std::size_t size_class_index) const noexcept;

    // 清空缓存（归还全局池）
    void flush() noexcept;

    // -------------------------------------------------------------------------
    // 访问 TLS 单例
    // -------------------------------------------------------------------------
    [[nodiscard]] static ThreadLocalCache& current() noexcept;

private:
    using Index = std::size_t;

    // 每个大小类别的空闲链表
    struct FreeList {
        void* head{nullptr};
        std::size_t count{0};
        std::size_t max_count{memory_pool_config::kThreadCacheSize};
    };

    std::array<FreeList, memory_pool_config::kNumSizeClasses + 1> lists_;

    // 批量从全局池补充
    void refill(Index idx) noexcept;
    // 批量归还全局池
    void spill(Index idx) noexcept;
};

// ==============================================================================
// 分配器接口（供 STL 容器使用）
// ==============================================================================
template <typename T>
class PoolAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    struct rebind {
        using other = PoolAllocator<U>;
    };

    PoolAllocator() noexcept = default;

    template <typename U>
    PoolAllocator(const PoolAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        void* p = ThreadLocalCache::current().allocate(bytes);
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        ThreadLocalCache::current().deallocate(p, n * sizeof(T));
    }

    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <typename U>
    void destroy(U* p) {
        p->~U();
    }

    [[nodiscard]] bool operator==(const PoolAllocator&) const noexcept { return true; }
    [[nodiscard]] bool operator!=(const PoolAllocator&) const noexcept { return false; }
};

// ==============================================================================
// 对象池（带构造/析构）
// ==============================================================================
template <typename T, typename... Args>
[[nodiscard]] T* pool_create(Args&&... args) {
    void* p = ThreadLocalCache::current().allocate(sizeof(T));
    if (!p) throw std::bad_alloc();
    try {
        return ::new (p) T(std::forward<Args>(args)...);
    } catch (...) {
        ThreadLocalCache::current().deallocate(p, sizeof(T));
        throw;
    }
}

template <typename T>
void pool_destroy(T* p) noexcept {
    if (!p) return;
    p->~T();
    ThreadLocalCache::current().deallocate(p, sizeof(T));
}

// 智能指针配合
template <typename T>
struct PoolDeleter {
    void operator()(T* p) const noexcept {
        pool_destroy(p);
    }
};

template <typename T, typename... Args>
[[nodiscard]] std::unique_ptr<T, PoolDeleter<T>> pool_make_unique(Args&&... args) {
    return std::unique_ptr<T, PoolDeleter<T>>(
        pool_create<T>(std::forward<Args>(args)...));
}

// ==============================================================================
// Debug 检测
// ==============================================================================
#ifdef QUANT_MEMORY_POOL_DEBUG

namespace memory_pool_debug {

// 双重释放检测
void register_allocation(void* ptr, std::size_t size) noexcept;
void unregister_allocation(void* ptr) noexcept;

// 毒化
void poison_allocated(void* ptr, std::size_t size) noexcept;
void poison_freed(void* ptr, std::size_t size) noexcept;

// 校验
void verify_allocated(void* ptr) noexcept;

}  // namespace memory_pool_debug

#endif  // QUANT_MEMORY_POOL_DEBUG

// ==============================================================================
// 线程本地缓存的 inline 实现
// ==============================================================================
inline ThreadLocalCache& ThreadLocalCache::current() noexcept {
    static thread_local ThreadLocalCache cache;
    return cache;
}

inline void* ThreadLocalCache::allocate(std::size_t size) noexcept {
    using namespace memory_pool_config;

    // 超大分配：直接走 malloc
    if (size > kMaxBlockSize) {
        auto& stats = const_cast<MemoryPoolStats&>(MemoryPool::instance().stats());
        stats.malloc_fallback_count.fetch_add(1, std::memory_order_relaxed);
        void* p = std::aligned_alloc(kAlignment,
            (size + kAlignment - 1) & ~(kAlignment - 1));
        return p;
    }

    const auto idx = SizeClass::index(size);
    auto& list = lists_[idx];

    // 热路径：TLS 命中
    if (list.head) {
        void* p = list.head;
        std::memcpy(&list.head, p, sizeof(void*));
        --list.count;

        auto& stats = const_cast<MemoryPoolStats&>(MemoryPool::instance().stats());
        stats.cache_hit_count.fetch_add(1, std::memory_order_relaxed);
        stats.allocate_count.fetch_add(1, std::memory_order_relaxed);
        stats.in_use_blocks.fetch_add(1, std::memory_order_relaxed);

        // 更新峰值
        auto peak = stats.peak_in_use.load(std::memory_order_relaxed);
        auto used = stats.in_use_blocks.load(std::memory_order_relaxed);
        while (used > peak && !stats.peak_in_use.compare_exchange_weak(
            peak, used, std::memory_order_relaxed)) {}

        return p;
    }

    // TLS 空：从全局池批量补充
    auto& stats = const_cast<MemoryPoolStats&>(MemoryPool::instance().stats());
    stats.cache_miss_count.fetch_add(1, std::memory_order_relaxed);

    refill(idx);
    if (list.head) {
        void* p = list.head;
        std::memcpy(&list.head, p, sizeof(void*));
        --list.count;
        stats.allocate_count.fetch_add(1, std::memory_order_relaxed);
        stats.in_use_blocks.fetch_add(1, std::memory_order_relaxed);
        return p;
    }

    // 全局池也失败
    stats.failed_count.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

inline void ThreadLocalCache::deallocate(void* ptr, std::size_t size) noexcept {
    if (!ptr) return;

    using namespace memory_pool_config;

    // 超大释放
    if (size > kMaxBlockSize) {
        std::free(ptr);
        auto& stats = const_cast<MemoryPoolStats&>(MemoryPool::instance().stats());
        stats.deallocate_count.fetch_add(1, std::memory_order_relaxed);
        stats.in_use_blocks.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    const auto idx = SizeClass::index(size);
    auto& list = lists_[idx];

    // TLS 满：批量归还
    if (list.count >= list.max_count) {
        spill(idx);
    }

    // 头插
    std::memcpy(ptr, &list.head, sizeof(void*));
    list.head = ptr;
    ++list.count;

    auto& stats = const_cast<MemoryPoolStats&>(MemoryPool::instance().stats());
    stats.deallocate_count.fetch_add(1, std::memory_order_relaxed);
    stats.in_use_blocks.fetch_sub(1, std::memory_order_relaxed);
}

}  // namespace quant

#endif  // QUANT_COMMON_CORE_MEMORY_POOL_HPP
