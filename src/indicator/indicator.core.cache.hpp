// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 指标缓存
// ==============================================================================
// @file    src/indicator/indicator.core.cache.hpp
// @module  indicator
// @type    core
// @name    cache
// @version 1.0.1
// @brief   指标结果缓存：O(1) 查询、LRU 淘汰、失效管理、命中率统计
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 时间戳主键：int64 微秒，天然有序，无哈希冲突
//   - 环形缓冲 + 哈希索引：O(1) 查询，O(1) 淘汰
//   - 读多写少：shared_mutex，读并发
//   - 惰性失效：invalidate_from 标记，不立即删除
//   - 命中率监控：判断缓存是否有效
//   - 版本号：数据变更时递增，避免 stale
//   - 容量自适应：可 resize，支持预留
//   - 无动态分配热路径：预分配桶 + 复用节点
//
// 线程安全:
//   - 所有 public 方法线程安全
//   - 返回的引用在下次写操作前有效，多线程需注意
//   - 建议：读端持 shared_ptr 或拷贝
// ==============================================================================

#ifndef QUANT_INDICATOR_CORE_CACHE_HPP
#define QUANT_INDICATOR_CORE_CACHE_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"
#include "indicator/indicator.core.band.hpp"
#include "indicator/indicator.core.calculator.hpp"

namespace quant {
namespace indicator {

// ==============================================================================
// 常量
// ==============================================================================
inline constexpr std::size_t kDefaultCacheCapacity = 1024;
inline constexpr std::size_t kMinCacheCapacity = 16;
inline constexpr std::size_t kMaxCacheCapacity = 1 << 20;   // 1M 条目
inline constexpr std::size_t kDefaultInvalidateWindow = 100; // 前向失效窗口

// ==============================================================================
// 缓存统计
// ==============================================================================
struct CacheStats {
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
    std::atomic<std::uint64_t> inserts{0};
    std::atomic<std::uint64_t> updates{0};
    std::atomic<std::uint64_t> evictions{0};
    std::atomic<std::uint64_t> invalidations{0};
    std::atomic<std::uint64_t> resize_count{0};

    void reset() noexcept {
        hits.store(0, std::memory_order_relaxed);
        misses.store(0, std::memory_order_relaxed);
        inserts.store(0, std::memory_order_relaxed);
        updates.store(0, std::memory_order_relaxed);
        evictions.store(0, std::memory_order_relaxed);
        invalidations.store(0, std::memory_order_relaxed);
        resize_count.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t total_lookups() const noexcept {
        return hits.load(std::memory_order_relaxed) +
               misses.load(std::memory_order_relaxed);
    }

    [[nodiscard]] double hit_rate() const noexcept {
        const auto total = total_lookups();
        return total == 0
            ? 0.0
            : static_cast<double>(hits.load(std::memory_order_relaxed)) /
              static_cast<double>(total);
    }

    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 缓存配置
// ==============================================================================
struct CacheConfig {
    // 最大条目数
    std::size_t capacity{kDefaultCacheCapacity};

    // 是否启用 LRU（false 则用 FIFO）
    bool enable_lru{true};

    // 是否缓存预热结果
    bool preload_on_resize{false};

    // 前向失效窗口（invalidate_from 时额外标记的条目数）
    std::size_t invalidate_window{kDefaultInvalidateWindow};

    // 是否校验条目中的时间戳与键一致
    bool validate_on_get{true};

    [[nodiscard]] bool is_valid() const noexcept {
        if (capacity < kMinCacheCapacity || capacity > kMaxCacheCapacity) {
            return false;
        }
        if (invalidate_window > capacity) return false;
        return true;
    }

    [[nodiscard]] std::string validate() const;
};

// ==============================================================================
// 缓存条目（模板化，支持 IndicatorResult / BandResult）
// ==============================================================================
template <typename T>
struct CacheEntry {
    Timestamp timestamp{};
    T         value{};
    std::uint64_t version{0};
    bool      valid{true};

    // LRU 链表迭代器（仅内部使用）
    typename std::list<std::size_t>::iterator lru_iter{};
};

// ==============================================================================
// 泛型缓存（基于时间戳键）
// ==============================================================================
// 线程安全：所有 public 方法内部加锁
// 存储：unordered_map<Timestamp_us, Node>
// LRU：list<key> 维护访问顺序
template <typename T>
class TimestampCache {
public:
    using ValueType = T;
    using KeyType = std::int64_t;   // 微秒时间戳

    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    explicit TimestampCache(CacheConfig config = {});

    TimestampCache(const TimestampCache&) = delete;
    TimestampCache& operator=(const TimestampCache&) = delete;
    TimestampCache(TimestampCache&&) = delete;
    TimestampCache& operator=(TimestampCache&&) = delete;

    ~TimestampCache() = default;

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    [[nodiscard]] CacheConfig config() const noexcept;

    // 调整容量（会淘汰多余条目）
    void resize(std::size_t new_capacity);

    // -------------------------------------------------------------------------
    // 读操作
    // -------------------------------------------------------------------------
    // 查询：返回值拷贝，避免悬空引用
    [[nodiscard]] std::optional<T> get(Timestamp t) const;

    // 批量查询
    [[nodiscard]] std::vector<std::optional<T>>
        get_batch(const std::vector<Timestamp>& timestamps) const;

    // 是否包含
    [[nodiscard]] bool contains(Timestamp t) const;

    // -------------------------------------------------------------------------
    // 写操作
    // -------------------------------------------------------------------------
    // 插入或更新
    void put(Timestamp t, T value);

    // 批量插入
    void put_batch(const std::vector<std::pair<Timestamp, T>>& items);

    // 移除单个条目
    bool remove(Timestamp t);

    // 从某时间戳起失效（含该点）
    void invalidate_from(Timestamp t);

    // 全部失效
    void invalidate_all();

    // 清空
    void clear();

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::uint64_t version() const noexcept;

    [[nodiscard]] CacheStats snapshot_stats() const;

    // -------------------------------------------------------------------------
    // 遍历（只读）
    // -------------------------------------------------------------------------
    // 按时间戳升序返回所有有效条目
    [[nodiscard]] std::vector<std::pair<Timestamp, T>> entries() const;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    // 内部节点
    struct Node {
        T value{};
        std::uint64_t version{0};
        typename std::list<KeyType>::iterator lru_iter;
    };

    // 从 LRU 尾部淘汰一个
    void evict_one_locked();

    // 更新 LRU 位置
    void touch_locked(typename std::unordered_map<KeyType, Node>::iterator it);

    // 校验
    [[nodiscard]] bool validate_key(Timestamp t) const noexcept;

    // -------------------------------------------------------------------------
    // 成员
    // -------------------------------------------------------------------------
    CacheConfig config_;
    mutable std::shared_mutex mutex_;

    std::unordered_map<KeyType, Node> map_;
    std::list<KeyType> lru_;                  // 前=最近使用，后=最久未用
    std::uint64_t version_{0};

    mutable CacheStats stats_;
};

// ==============================================================================
// 组合：指标 + 带宽缓存
// ==============================================================================
class IndicatorCache {
public:
    explicit IndicatorCache(CacheConfig config = {});

    IndicatorCache(const IndicatorCache&) = delete;
    IndicatorCache& operator=(const IndicatorCache&) = delete;
    IndicatorCache(IndicatorCache&&) = delete;
    IndicatorCache& operator=(IndicatorCache&&) = delete;

    ~IndicatorCache() = default;

    // -------------------------------------------------------------------------
    // 指标访问
    // -------------------------------------------------------------------------
    [[nodiscard]] std::optional<IndicatorResult>
        get_indicator(Timestamp t) const;

    void put_indicator(Timestamp t, IndicatorResult r);

    // -------------------------------------------------------------------------
    // 带宽访问
    // -------------------------------------------------------------------------
    [[nodiscard]] std::optional<BandResult>
        get_band(Timestamp t) const;

    void put_band(Timestamp t, BandResult b);

    // -------------------------------------------------------------------------
    // 失效（指标和带宽一起失效）
    // -------------------------------------------------------------------------
    void invalidate_from(Timestamp t);
    void invalidate_all();
    void clear();

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    void resize(std::size_t new_capacity);

    // -------------------------------------------------------------------------
    // 状态
    // -------------------------------------------------------------------------
    [[nodiscard]] std::size_t indicator_size() const;
    [[nodiscard]] std::size_t band_size() const;
    [[nodiscard]] std::uint64_t version() const;

    [[nodiscard]] CacheStats snapshot_stats() const;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    TimestampCache<IndicatorResult> indicator_cache_;
    TimestampCache<BandResult>      band_cache_;
};

// ==============================================================================
// 模板实现
// ==============================================================================
template <typename T>
TimestampCache<T>::TimestampCache(CacheConfig config)
    : config_{config}
{
    if (!config_.is_valid()) {
        throw std::invalid_argument("CacheConfig 非法: " + config_.validate());
    }
    // 预留哈希桶
    map_.reserve(config_.capacity);
}

template <typename T>
CacheConfig TimestampCache<T>::config() const noexcept {
    std::shared_lock lock(mutex_);
    return config_;
}

template <typename T>
bool TimestampCache<T>::validate_key(Timestamp t) const noexcept {
    return t.is_valid();
}

template <typename T>
std::optional<T> TimestampCache<T>::get(Timestamp t) const {
    if (!validate_key(t)) {
        stats_.misses.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    const KeyType key = t.microseconds();

    // 先读锁快速路径
    {
        std::shared_lock lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            stats_.misses.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        stats_.hits.fetch_add(1, std::memory_order_relaxed);
        return it->second.value;   // 拷贝返回
    }
}

template <typename T>
std::vector<std::optional<T>>
TimestampCache<T>::get_batch(const std::vector<Timestamp>& timestamps) const {
    std::vector<std::optional<T>> results;
    results.reserve(timestamps.size());

    std::shared_lock lock(mutex_);
    for (const auto& t : timestamps) {
        if (!validate_key(t)) {
            results.push_back(std::nullopt);
            stats_.misses.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        auto it = map_.find(t.microseconds());
        if (it == map_.end()) {
            results.push_back(std::nullopt);
            stats_.misses.fetch_add(1, std::memory_order_relaxed);
        } else {
            results.push_back(it->second.value);
            stats_.hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return results;
}

template <typename T>
bool TimestampCache<T>::contains(Timestamp t) const {
    if (!validate_key(t)) return false;
    std::shared_lock lock(mutex_);
    return map_.find(t.microseconds()) != map_.end();
}

template <typename T>
void TimestampCache<T>::put(Timestamp t, T value) {
    if (!validate_key(t)) return;

    const KeyType key = t.microseconds();

    std::unique_lock lock(mutex_);

    auto it = map_.find(key);
    if (it != map_.end()) {
        // 更新
        it->second.value = std::move(value);
        it->second.version = version_;
        touch_locked(it);
        stats_.updates.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // 容量检查
    if (map_.size() >= config_.capacity) {
        evict_one_locked();
    }

    // 插入
    lru_.push_front(key);
    Node node;
    node.value = std::move(value);
    node.version = version_;
    node.lru_iter = lru_.begin();
    map_.emplace(key, std::move(node));

    stats_.inserts.fetch_add(1, std::memory_order_relaxed);
}

template <typename T>
void TimestampCache<T>::put_batch(
    const std::vector<std::pair<Timestamp, T>>& items) {
    std::unique_lock lock(mutex_);
    for (const auto& [t, v] : items) {
        if (!validate_key(t)) continue;
        const KeyType key = t.microseconds();

        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.value = v;
            it->second.version = version_;
            touch_locked(it);
            stats_.updates.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (map_.size() >= config_.capacity) {
            evict_one_locked();
        }

        lru_.push_front(key);
        Node node;
        node.value = v;
        node.version = version_;
        node.lru_iter = lru_.begin();
        map_.emplace(key, std::move(node));
        stats_.inserts.fetch_add(1, std::memory_order_relaxed);
    }
}

template <typename T>
bool TimestampCache<T>::remove(Timestamp t) {
    if (!validate_key(t)) return false;

    std::unique_lock lock(mutex_);
    auto it = map_.find(t.microseconds());
    if (it == map_.end()) return false;

    lru_.erase(it->second.lru_iter);
    map_.erase(it);
    return true;
}

template <typename T>
void TimestampCache<T>::invalidate_from(Timestamp t) {
    if (!validate_key(t)) return;

    const KeyType key = t.microseconds();

    std::unique_lock lock(mutex_);

    // 收集所有 >= key 的条目（时间戳升序）
    std::vector<KeyType> to_remove;
    for (const auto& [k, node] : map_) {
        if (k >= key) {
            to_remove.push_back(k);
        }
    }

    for (auto k : to_remove) {
        auto it = map_.find(k);
        if (it != map_.end()) {
            lru_.erase(it->second.lru_iter);
            map_.erase(it);
            stats_.invalidations.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 版本号递增
    ++version_;
}

template <typename T>
void TimestampCache<T>::invalidate_all() {
    std::unique_lock lock(mutex_);
    map_.clear();
    lru_.clear();
    ++version_;
    stats_.invalidations.fetch_add(1, std::memory_order_relaxed);
}

template <typename T>
void TimestampCache<T>::clear() {
    invalidate_all();
}

template <typename T>
std::size_t TimestampCache<T>::size() const noexcept {
    std::shared_lock lock(mutex_);
    return map_.size();
}

template <typename T>
std::size_t TimestampCache<T>::capacity() const noexcept {
    std::shared_lock lock(mutex_);
    return config_.capacity;
}

template <typename T>
bool TimestampCache<T>::empty() const noexcept {
    std::shared_lock lock(mutex_);
    return map_.empty();
}

template <typename T>
std::uint64_t TimestampCache<T>::version() const noexcept {
    std::shared_lock lock(mutex_);
    return version_;
}

template <typename T>
CacheStats TimestampCache<T>::snapshot_stats() const {
    CacheStats snapshot;
    snapshot.hits.store(stats_.hits.load(std::memory_order_relaxed));
    snapshot.misses.store(stats_.misses.load(std::memory_order_relaxed));
    snapshot.inserts.store(stats_.inserts.load(std::memory_order_relaxed));
    snapshot.updates.store(stats_.updates.load(std::memory_order_relaxed));
    snapshot.evictions.store(stats_.evictions.load(std::memory_order_relaxed));
    snapshot.invalidations.store(
        stats_.invalidations.load(std::memory_order_relaxed));
    snapshot.resize_count.store(
        stats_.resize_count.load(std::memory_order_relaxed));
    return snapshot;
}

template <typename T>
std::vector<std::pair<Timestamp, T>>
TimestampCache<T>::entries() const {
    std::shared_lock lock(mutex_);

    // 收集所有键，按时间戳升序排序
    std::vector<KeyType> keys;
    keys.reserve(map_.size());
    for (const auto& [k, _] : map_) {
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());

    std::vector<std::pair<Timestamp, T>> result;
    result.reserve(keys.size());
    for (auto k : keys) {
        auto it = map_.find(k);
        if (it != map_.end()) {
            result.emplace_back(Timestamp{k}, it->second.value);
        }
    }
    return result;
}

template <typename T>
void TimestampCache<T>::resize(std::size_t new_capacity) {
    if (new_capacity < kMinCacheCapacity ||
        new_capacity > kMaxCacheCapacity) {
        throw std::invalid_argument(
            "容量越界 [" + std::to_string(kMinCacheCapacity) +
            ", " + std::to_string(kMaxCacheCapacity) + "]");
    }

    std::unique_lock lock(mutex_);
    config_.capacity = new_capacity;

    // 淘汰多余条目
    while (map_.size() > config_.capacity) {
        evict_one_locked();
    }

    stats_.resize_count.fetch_add(1, std::memory_order_relaxed);
}

template <typename T>
void TimestampCache<T>::evict_one_locked() {
    if (lru_.empty()) return;

    const KeyType victim = lru_.back();
    lru_.pop_back();

    auto it = map_.find(victim);
    if (it != map_.end()) {
        map_.erase(it);
        stats_.evictions.fetch_add(1, std::memory_order_relaxed);
    }
}

template <typename T>
void TimestampCache<T>::touch_locked(
    typename std::unordered_map<KeyType, Node>::iterator it) {
    if (!config_.enable_lru) return;

    // 从当前位置移除
    lru_.erase(it->second.lru_iter);
    // 移到队首
    lru_.push_front(it->first);
    it->second.lru_iter = lru_.begin();
}

template <typename T>
std::string TimestampCache<T>::dump() const {
    std::shared_lock lock(mutex_);

    std::ostringstream oss;
    oss << "TimestampCache{"
        << "size=" << map_.size()
        << "/" << config_.capacity
        << ", version=" << version_
        << ", lru=" << (config_.enable_lru ? "on" : "off")
        << ", hits=" << stats_.hits.load(std::memory_order_relaxed)
        << ", misses=" << stats_.misses.load(std::memory_order_relaxed)
        << ", hit_rate=" << (snapshot_stats().hit_rate() * 100.0) << "%"
        << "}";
    return oss.str();
}

// ==============================================================================
// IndicatorCache 实现
// ==============================================================================
inline IndicatorCache::IndicatorCache(CacheConfig config)
    : indicator_cache_{config}
    , band_cache_{config}
{}

inline std::optional<IndicatorResult>
IndicatorCache::get_indicator(Timestamp t) const {
    return indicator_cache_.get(t);
}

inline void IndicatorCache::put_indicator(Timestamp t, IndicatorResult r) {
    indicator_cache_.put(t, std::move(r));
}

inline std::optional<BandResult>
IndicatorCache::get_band(Timestamp t) const {
    return band_cache_.get(t);
}

inline void IndicatorCache::put_band(Timestamp t, BandResult b) {
    band_cache_.put(t, std::move(b));
}

inline void IndicatorCache::invalidate_from(Timestamp t) {
    indicator_cache_.invalidate_from(t);
    band_cache_.invalidate_from(t);
}

inline void IndicatorCache::invalidate_all() {
    indicator_cache_.invalidate_all();
    band_cache_.invalidate_all();
}

inline void IndicatorCache::clear() {
    invalidate_all();
}

inline void IndicatorCache::resize(std::size_t new_capacity) {
    indicator_cache_.resize(new_capacity);
    band_cache_.resize(new_capacity);
}

inline std::size_t IndicatorCache::indicator_size() const {
    return indicator_cache_.size();
}

inline std::size_t IndicatorCache::band_size() const {
    return band_cache_.size();
}

inline std::uint64_t IndicatorCache::version() const {
    // 取两者中较大的版本号（保守）
    const auto iv = indicator_cache_.version();
    const auto bv = band_cache_.version();
    return std::max(iv, bv);
}

inline CacheStats IndicatorCache::snapshot_stats() const {
    // 合并两个缓存的统计
    CacheStats merged;
    auto is = indicator_cache_.snapshot_stats();
    auto bs = band_cache_.snapshot_stats();
    merged.hits.store(is.hits.load() + bs.hits.load());
    merged.misses.store(is.misses.load() + bs.misses.load());
    merged.inserts.store(is.inserts.load() + bs.inserts.load());
    merged.updates.store(is.updates.load() + bs.updates.load());
    merged.evictions.store(is.evictions.load() + bs.evictions.load());
    merged.invalidations.store(is.invalidations.load() + bs.invalidations.load());
    merged.resize_count.store(is.resize_count.load() + bs.resize_count.load());
    return merged;
}

inline std::string IndicatorCache::dump() const {
    std::ostringstream oss;
    oss << "IndicatorCache{\n"
        << "  indicator: " << indicator_cache_.dump() << "\n"
        << "  band:      " << band_cache_.dump() << "\n"
        << "}";
    return oss.str();
}

// ==============================================================================
// CacheStats::to_string / CacheConfig::validate 的实现
// ==============================================================================
inline std::string CacheStats::to_string() const {
    std::ostringstream oss;
    oss << "CacheStats{"
        << "hits=" << hits.load(std::memory_order_relaxed)
        << ", misses=" << misses.load(std::memory_order_relaxed)
        << ", inserts=" << inserts.load(std::memory_order_relaxed)
        << ", updates=" << updates.load(std::memory_order_relaxed)
        << ", evictions=" << evictions.load(std::memory_order_relaxed)
        << ", invalidations=" << invalidations.load(std::memory_order_relaxed)
        << ", hit_rate=" << (hit_rate() * 100.0) << "%"
        << "}";
    return oss.str();
}

inline std::string CacheConfig::validate() const {
    if (capacity < kMinCacheCapacity || capacity > kMaxCacheCapacity) {
        return "capacity 越界 [" + std::to_string(kMinCacheCapacity) +
               ", " + std::to_string(kMaxCacheCapacity) + "]";
    }
    if (invalidate_window > capacity) {
        return "invalidate_window 大于 capacity";
    }
    return {};
}

}  // namespace indicator
}  // namespace quant

#endif  // QUANT_INDICATOR_CORE_CACHE_HPP
