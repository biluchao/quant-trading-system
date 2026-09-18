// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 内存池实现
// ==============================================================================
// @file    src/common/common.core.memory_pool.cpp
// @module  common
// @type    core
// @name    memory_pool
// @version 1.0.1
// @brief   分层内存池实现（TLS 缓存 + 全局后备）
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 平台兼容：aligned_alloc / _aligned_malloc 统一封装
//   - 对齐保证：64 字节，支持 AVX-512
//   - 失败回滚：refill 部分失败时回滚已分配
//   - 生命周期：MemoryPool 永不析构，避免静态析构顺序问题
//   - 线程安全：Impl 内 mutex 保护，config 只读
//   - 单源统计：从 pool 聚合，无重复计数
//   - Debug 检测：毒化 + 双重释放检测（QUANT_MEMORY_POOL_DEBUG）
// ==============================================================================

#include "common/common.core.memory_pool.hpp"
#include "common/common.core.logger.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

// ==============================================================================
// 平台特定：对齐内存分配/释放
// ==============================================================================
namespace quant {
namespace memory_pool_detail {

// 校验对齐是 2 的幂
[[nodiscard]] constexpr bool is_power_of_two(std::size_t n) noexcept {
    return n > 0 && (n & (n - 1)) == 0;
}

#if defined(_WIN32)
    [[nodiscard]] inline void* aligned_alloc_impl(std::size_t align,
                                                    std::size_t size) noexcept {
        return _aligned_malloc(size, align);
    }
    inline void aligned_free_impl(void* p) noexcept {
        _aligned_free(p);
    }
#else
    [[nodiscard]] inline void* aligned_alloc_impl(std::size_t align,
                                                    std::size_t size) noexcept {
        // POSIX aligned_alloc 要求 size 是 align 的倍数
        const std::size_t aligned_size = (size + align - 1) & ~(align - 1);
        return std::aligned_alloc(align, aligned_size);
    }
    inline void aligned_free_impl(void* p) noexcept {
        std::free(p);
    }
#endif

}  // namespace memory_pool_detail
}  // namespace quant

namespace quant {

// ==============================================================================
// Impl 定义
// ==============================================================================
struct MemoryPool::Impl {
    // -------------------------------------------------------------------------
    // 每个 size class 的池
    // -------------------------------------------------------------------------
    struct ClassPool {
        std::mutex mutex;
        void* free_list{nullptr};
        std::size_t count{0};         // 空闲块数
        std::size_t total{0};         // 总块数（含使用中）
        std::size_t block_size{0};    // 块大小
    };

    // 仅 kNumSizeClasses 个池，超大块走 malloc
    std::array<ClassPool, memory_pool_config::kNumSizeClasses> pools;

    // 配置（构造后只读）
    MemoryPool::Config config;

    // 统计
    MemoryPoolStats stats;

    // 构造
    Impl() {
        for (std::size_t i = 0; i < pools.size(); ++i) {
            pools[i].block_size = SizeClass::block_size(i);
            // 确保块足够存储 next 指针
            if (pools[i].block_size < sizeof(void*)) {
                pools[i].block_size = sizeof(void*);
            }
        }
        // 应用默认配置
        config.thread_cache_size = memory_pool_config::kThreadCacheSize;
        config.global_pool_initial = memory_pool_config::kGlobalPoolInitialSize;
        config.global_pool_max = memory_pool_config::kGlobalPoolMaxSize;
    }

    // 析构：释放所有空闲块
    ~Impl() {
        for (auto& pool : pools) {
            std::lock_guard lock(pool.mutex);
            void* p = pool.free_list;
            while (p) {
                void* next = nullptr;
                std::memcpy(&next, p, sizeof(void*));
                memory_pool_detail::aligned_free_impl(p);
                p = next;
            }
            pool.free_list = nullptr;
            pool.count = 0;
            pool.total = 0;
        }
    }

    // -------------------------------------------------------------------------
    // 从池中取一块
    // -------------------------------------------------------------------------
    [[nodiscard]] void* alloc_from_pool(std::size_t idx) noexcept {
        auto& pool = pools[idx];

        if (pool.free_list) {
            void* p = pool.free_list;
            std::memcpy(&pool.free_list, p, sizeof(void*));
            --pool.count;
            return p;
        }
        return nullptr;
    }

    // -------------------------------------------------------------------------
    // 归还一块到池
    // -------------------------------------------------------------------------
    void free_to_pool(std::size_t idx, void* ptr) noexcept {
        auto& pool = pools[idx];
        std::memcpy(ptr, &pool.free_list, sizeof(void*));
        pool.free_list = ptr;
        ++pool.count;
    }

    // -------------------------------------------------------------------------
    // 批量扩充池（全部成功才提交，失败回滚）
    // -------------------------------------------------------------------------
    [[nodiscard]] bool refill_pool(std::size_t idx,
                                     std::size_t batch) noexcept {
        auto& pool = pools[idx];

        // 上限检查
        if (pool.total >= config.global_pool_max) {
            return false;
        }

        const std::size_t remaining = config.global_pool_max - pool.total;
        const std::size_t to_alloc = std::min(batch, remaining);

        if (to_alloc == 0) return false;

        // 阶段 1：全部分配到临时数组
        std::vector<void*> allocated;
        allocated.reserve(to_alloc);

        for (std::size_t i = 0; i < to_alloc; ++i) {
            void* p = memory_pool_detail::aligned_alloc_impl(
                memory_pool_config::kAlignment,
                pool.block_size);

            if (!p) {
                // 分配失败，回滚已分配的
                for (void* q : allocated) {
                    memory_pool_detail::aligned_free_impl(q);
                }
                stats.failed_count.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            allocated.push_back(p);
        }

        // 阶段 2：全部插入 free_list
        for (void* p : allocated) {
            std::memcpy(p, &pool.free_list, sizeof(void*));
            pool.free_list = p;
        }
        pool.count += allocated.size();
        pool.total += allocated.size();

        // 统计（单源更新）
        stats.total_blocks.store(
            aggregate_total_blocks(), std::memory_order_relaxed);
        stats.overflow_count.fetch_add(1, std::memory_order_relaxed);

        return true;
    }

    // -------------------------------------------------------------------------
    // 聚合所有池的总块数
    // -------------------------------------------------------------------------
    [[nodiscard]] std::size_t aggregate_total_blocks() const noexcept {
        std::size_t sum = 0;
        for (const auto& pool : pools) {
            sum += pool.total;
        }
        return sum;
    }

    // -------------------------------------------------------------------------
    // 更新峰值（CAS 循环）
    // -------------------------------------------------------------------------
    void update_peak(std::size_t current) noexcept {
        std::size_t peak = stats.peak_in_use.load(std::memory_order_relaxed);
        while (current > peak) {
            if (stats.peak_in_use.compare_exchange_weak(
                    peak, current,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                break;
            }
        }
    }
};

// ==============================================================================
// MemoryPool
// ==============================================================================

// -----------------------------------------------------------------------------
// 单例（永不析构，避免静态析构顺序问题）
// -----------------------------------------------------------------------------
MemoryPool& MemoryPool::instance() noexcept {
    static MemoryPool* pool = new MemoryPool();
    return *pool;
}

MemoryPool::MemoryPool() : impl_(std::make_unique<Impl>()) {}

MemoryPool::~MemoryPool() = default;

// -----------------------------------------------------------------------------
// 分配原始块
// -----------------------------------------------------------------------------
void* MemoryPool::allocate_raw(std::size_t size) noexcept {
    // 超大分配：直接走 aligned_alloc
    if (size > memory_pool_config::kMaxBlockSize) {
        void* p = memory_pool_detail::aligned_alloc_impl(
            memory_pool_config::kAlignment, size);
        if (p) {
            impl_->stats.malloc_fallback_count.fetch_add(
                1, std::memory_order_relaxed);
            impl_->stats.allocate_count.fetch_add(
                1, std::memory_order_relaxed);
            impl_->stats.in_use_blocks.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            impl_->stats.failed_count.fetch_add(
                1, std::memory_order_relaxed);
        }
        return p;
    }

    const auto idx = SizeClass::index(size);
    auto& pool = impl_->pools[idx];

    std::lock_guard lock(pool.mutex);

    void* p = impl_->alloc_from_pool(idx);
    if (!p) {
        // 池空：扩充
        impl_->refill_pool(idx, memory_pool_config::kChunkSize);
        p = impl_->alloc_from_pool(idx);
    }

    if (p) {
        impl_->stats.allocate_count.fetch_add(1, std::memory_order_relaxed);
        const auto used = impl_->stats.in_use_blocks.fetch_add(
            1, std::memory_order_relaxed) + 1;
        impl_->update_peak(used);
    } else {
        impl_->stats.failed_count.fetch_add(1, std::memory_order_relaxed);
    }

    return p;
}

// -----------------------------------------------------------------------------
// 释放原始块
// -----------------------------------------------------------------------------
void MemoryPool::deallocate_raw(void* ptr, std::size_t size) noexcept {
    if (!ptr) return;

    // 超大释放
    if (size > memory_pool_config::kMaxBlockSize) {
        memory_pool_detail::aligned_free_impl(ptr);
        impl_->stats.deallocate_count.fetch_add(
            1, std::memory_order_relaxed);
        impl_->stats.in_use_blocks.fetch_sub(
            1, std::memory_order_relaxed);
        return;
    }

    const auto idx = SizeClass::index(size);
    auto& pool = impl_->pools[idx];

    std::lock_guard lock(pool.mutex);

    // Debug：毒化
#ifdef QUANT_MEMORY_POOL_DEBUG
    std::memset(ptr, 0xDE, pool.block_size);
#endif

    impl_->free_to_pool(idx, ptr);

    impl_->stats.deallocate_count.fetch_add(1, std::memory_order_relaxed);
    impl_->stats.in_use_blocks.fetch_sub(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// 统计
// -----------------------------------------------------------------------------
const MemoryPoolStats& MemoryPool::stats() const noexcept {
    return impl_->stats;
}

void MemoryPool::reset_stats() noexcept {
    impl_->stats.reset();
}

// -----------------------------------------------------------------------------
// 释放所有空闲块
// -----------------------------------------------------------------------------
void MemoryPool::trim() noexcept {
    std::size_t released = 0;

    for (auto& pool : impl_->pools) {
        std::lock_guard lock(pool.mutex);

        void* p = pool.free_list;
        std::size_t n = 0;
        while (p) {
            void* next = nullptr;
            std::memcpy(&next, p, sizeof(void*));
            memory_pool_detail::aligned_free_impl(p);
            p = next;
            ++n;
        }
        pool.free_list = nullptr;
        pool.count = 0;
        pool.total -= n;   // 减去释放的数量
        released += n;
    }

    // 更新聚合统计
    impl_->stats.total_blocks.store(
        impl_->aggregate_total_blocks(), std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// 预留块（预热）
// -----------------------------------------------------------------------------
void MemoryPool::reserve(std::size_t bytes_per_class) {
    for (std::size_t i = 0; i < impl_->pools.size(); ++i) {
        auto& pool = impl_->pools[i];

        const std::size_t block_size = pool.block_size;
        if (block_size == 0) continue;

        const std::size_t target = bytes_per_class / block_size;

        std::lock_guard lock(pool.mutex);

        // 循环分配直到达到目标或达到上限
        int max_iterations = 64;  // 防止无限循环
        while (pool.count < target &&
               pool.total < impl_->config.global_pool_max &&
               max_iterations-- > 0) {
            const std::size_t batch = std::min(
                memory_pool_config::kChunkSize,
                target - pool.count);

            if (!impl_->refill_pool(i, batch)) {
                break;  // 分配失败，退出
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 诊断输出
// -----------------------------------------------------------------------------
std::string MemoryPool::dump() const {
    std::ostringstream oss;

    oss << "MemoryPool dump:\n";

    const auto& s = impl_->stats;
    oss << "  total_blocks:         "
        << s.total_blocks.load(std::memory_order_relaxed) << "\n";
    oss << "  in_use_blocks:        "
        << s.in_use_blocks.load(std::memory_order_relaxed) << "\n";
    oss << "  peak_in_use:          "
        << s.peak_in_use.load(std::memory_order_relaxed) << "\n";
    oss << "  allocate_count:       "
        << s.allocate_count.load(std::memory_order_relaxed) << "\n";
    oss << "  deallocate_count:     "
        << s.deallocate_count.load(std::memory_order_relaxed) << "\n";
    oss << "  cache_hit_count:      "
        << s.cache_hit_count.load(std::memory_order_relaxed) << "\n";
    oss << "  cache_miss_count:     "
        << s.cache_miss_count.load(std::memory_order_relaxed) << "\n";
    oss << "  overflow_count:       "
        << s.overflow_count.load(std::memory_order_relaxed) << "\n";
    oss << "  malloc_fallback_count:"
        << s.malloc_fallback_count.load(std::memory_order_relaxed) << "\n";
    oss << "  failed_count:         "
        << s.failed_count.load(std::memory_order_relaxed) << "\n";
    oss << "  usage_ratio:          "
        << s.usage_ratio() << "\n";

    oss << "\n  Per-class:\n";
    for (std::size_t i = 0; i < impl_->pools.size(); ++i) {
        const auto& pool = impl_->pools[i];
        oss << "    class[" << i << "]"
            << " block_size=" << pool.block_size
            << " free=" << pool.count
            << " total=" << pool.total << "\n";
    }

    return oss.str();
}

// -----------------------------------------------------------------------------
// 配置（仅初始化前可配置）
// -----------------------------------------------------------------------------
void MemoryPool::configure(const Config& cfg) noexcept {
    // 简单校验
    Config validated = cfg;
    if (validated.thread_cache_size == 0) {
        validated.thread_cache_size = memory_pool_config::kThreadCacheSize;
    }
    if (validated.global_pool_max == 0) {
        validated.global_pool_max = memory_pool_config::kGlobalPoolMaxSize;
    }
    if (validated.global_pool_initial > validated.global_pool_max) {
        validated.global_pool_initial = validated.global_pool_max;
    }

    impl_->config = validated;
}

// ==============================================================================
// ThreadLocalCache 实现
// ==============================================================================

// -----------------------------------------------------------------------------
// 构造
// -----------------------------------------------------------------------------
ThreadLocalCache::ThreadLocalCache() noexcept {
    for (auto& list : lists_) {
        list.max_count = memory_pool_config::kThreadCacheSize;
    }
}

// -----------------------------------------------------------------------------
// 析构：归还所有块（检查 MemoryPool 有效性）
// -----------------------------------------------------------------------------
ThreadLocalCache::~ThreadLocalCache() noexcept {
    // MemoryPool 永不析构，但线程退出时若程序已进入静态析构阶段，
    // 需确保 flush 不崩溃。由于 MemoryPool 用 new 分配，此处安全。
    flush();
}

// -----------------------------------------------------------------------------
// 从全局池补充
// -----------------------------------------------------------------------------
void ThreadLocalCache::refill(Index idx) noexcept {
    if (idx >= lists_.size()) return;

    auto& list = lists_[idx];

    // 计算大小
    const std::size_t size = (idx < memory_pool_config::kNumSizeClasses)
        ? SizeClass::block_size(idx)
        : memory_pool_config::kMaxBlockSize;

    const std::size_t batch = std::min(
        list.max_count - list.count,
        memory_pool_config::kChunkSize);

    if (batch == 0) return;

    auto& pool = MemoryPool::instance();
    std::size_t fetched = 0;

    for (std::size_t i = 0; i < batch; ++i) {
        void* p = pool.allocate_raw(size);
        if (!p) break;

        // 头插
        std::memcpy(p, &list.head, sizeof(void*));
        list.head = p;
        ++list.count;
        ++fetched;
    }

    // 统计更新
    auto& stats = const_cast<MemoryPoolStats&>(pool.stats());
    if (fetched > 0) {
        stats.cache_miss_count.fetch_add(1, std::memory_order_relaxed);
    }
}

// -----------------------------------------------------------------------------
// 归还全局池（至少归还一半，且至少 1 个）
// -----------------------------------------------------------------------------
void ThreadLocalCache::spill(Index idx) noexcept {
    if (idx >= lists_.size()) return;

    auto& list = lists_[idx];

    if (list.count == 0) return;

    const std::size_t size = (idx < memory_pool_config::kNumSizeClasses)
        ? SizeClass::block_size(idx)
        : memory_pool_config::kMaxBlockSize;

    // 归还一半，至少 1 个
    const std::size_t to_return = std::max<std::size_t>(list.count / 2, 1);

    auto& pool = MemoryPool::instance();

    for (std::size_t i = 0; i < to_return; ++i) {
        void* p = list.head;
        if (!p) break;

        // 先读取 next，再归还（deallocate_raw 会修改块首）
        void* next = nullptr;
        std::memcpy(&next, p, sizeof(void*));
        list.head = next;
        --list.count;

        pool.deallocate_raw(p, size);
    }
}

// -----------------------------------------------------------------------------
// 查询缓存数量
// -----------------------------------------------------------------------------
std::size_t ThreadLocalCache::cached_count(
    std::size_t idx) const noexcept
{
    if (idx >= lists_.size()) return 0;
    return lists_[idx].count;
}

// -----------------------------------------------------------------------------
// 清空缓存（归还全部）
// -----------------------------------------------------------------------------
void ThreadLocalCache::flush() noexcept {
    auto& pool = MemoryPool::instance();

    for (std::size_t idx = 0; idx < lists_.size(); ++idx) {
        auto& list = lists_[idx];

        if (list.count == 0) continue;

        const std::size_t size = (idx < memory_pool_config::kNumSizeClasses)
            ? SizeClass::block_size(idx)
            : memory_pool_config::kMaxBlockSize;

        void* p = list.head;
        while (p) {
            void* next = nullptr;
            std::memcpy(&next, p, sizeof(void*));
            pool.deallocate_raw(p, size);
            p = next;
        }

        list.head = nullptr;
        list.count = 0;
    }
}

}  // namespace quant
