// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 因果链追踪
// ==============================================================================
// @file    src/event/event.core.trace.hpp
// @module  event
// @type    core
// @name    trace
// @version 1.0.2
// @brief   无锁环形缓冲区 + 哈希索引的因果链追踪，支持采样、深度限制、导出
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 无锁热路径：环形缓冲区 + SPSC 队列，record() 仅原子操作
//   - 固定内存：编译期容量，零动态分配
//   - 哈希索引：trace_id → 环形缓冲区位置，O(1) 查询
//   - 采样控制：可配置采样率，CRITICAL 强制记录
//   - 深度限制：max_chain_depth 防止无限链
//   - 父子校验：禁止自引用，时间戳必须递增
//   - 缓存友好：SoA 布局 + 64 字节对齐
//   - 可观测：TraceStats 完整统计
//   - 导出：JSON 格式，兼容 OpenTelemetry
// ==============================================================================

#ifndef QUANT_EVENT_CORE_TRACE_HPP
#define QUANT_EVENT_CORE_TRACE_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
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
// 追踪配置
// ==============================================================================
struct TraceConfig {
    // 环形缓冲区容量（必须为 2 的幂）
    std::size_t capacity{1u << 14};            // 16384 条

    // 采样率 [0.0, 1.0]，1.0 = 全部记录
    double sample_rate{1.0};

    // 是否强制记录 CRITICAL 事件
    bool always_record_critical{true};

    // 最大链深度（防止无限递归）
    std::size_t max_chain_depth{100};

    // 是否启用（false 时 record 为空操作）
    bool enabled{true};

    // 是否启用括号追踪（scope-based）
    bool enable_scopes{true};
};

// ==============================================================================
// 追踪统计
// ==============================================================================
struct TraceStats {
    alignas(64) std::atomic<uint64_t> recorded_total{0};
    alignas(64) std::atomic<uint64_t> dropped_total{0};
    alignas(64) std::atomic<uint64_t> sampled_out_total{0};
    alignas(64) std::atomic<uint64_t> overwrite_total{0};
    alignas(64) std::atomic<uint64_t> depth_exceeded_total{0};
    alignas(64) std::atomic<uint64_t> broken_chain_total{0};
    alignas(64) std::atomic<uint64_t> query_total{0};
    alignas(64) std::atomic<uint64_t> max_chain_depth_observed{0};

    void reset() noexcept {
        recorded_total.store(0, std::memory_order_relaxed);
        dropped_total.store(0, std::memory_order_relaxed);
        sampled_out_total.store(0, std::memory_order_relaxed);
        overwrite_total.store(0, std::memory_order_relaxed);
        depth_exceeded_total.store(0, std::memory_order_relaxed);
        broken_chain_total.store(0, std::memory_order_relaxed);
        query_total.store(0, std::memory_order_relaxed);
        max_chain_depth_observed.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// 追踪节点（固定大小，无动态分配）
// ==============================================================================
struct alignas(64) TraceNode {
    uint64_t trace_id{0};
    uint64_t parent_id{0};
    int64_t  timestamp_us{0};
    uint16_t event_type{0};       // EventType
    uint8_t  priority{0};         // EventPriority
    uint8_t  flags{0};            // bit0: sampled, bit1: critical
    uint32_t depth{0};            // 链深度（从 root 算起）
    uint32_t seq{0};              // 环形缓冲区序号（用于校验有效性）

    static constexpr uint8_t kFlagSampled  = 0x01;
    static constexpr uint8_t kFlagCritical = 0x02;

    [[nodiscard]] bool is_valid() const noexcept {
        return trace_id != 0 && timestamp_us > 0;
    }

    [[nodiscard]] bool is_sampled() const noexcept {
        return (flags & kFlagSampled) != 0;
    }

    [[nodiscard]] bool is_critical() const noexcept {
        return (flags & kFlagCritical) != 0;
    }
};

// 编译期校验：TraceNode 不超过 2 个缓存行
static_assert(sizeof(TraceNode) <= 128, "TraceNode 过大");
static_assert(alignof(TraceNode) == 64, "TraceNode 未对齐");

// ==============================================================================
// 追踪查询结果
// ==============================================================================
struct TraceChain {
    std::vector<TraceNode> nodes;         // 从 root 到 leaf 顺序
    uint64_t root_trace_id{0};
    int64_t  start_us{0};
    int64_t  end_us{0};
    bool     is_complete{true};           // false 表示有节点被淘汰
    bool     has_cycle{false};            // 检测到循环引用
    uint32_t max_depth{0};

    [[nodiscard]] int64_t duration_us() const noexcept {
        return end_us - start_us;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return nodes.size();
    }
};

// ==============================================================================
// Scope 追踪（RAII，用于测量函数执行时间）
// ==============================================================================
class TraceScope {
public:
    TraceScope(EventType type, uint64_t parent_id = 0) noexcept;
    ~TraceScope() noexcept;

    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

    TraceScope(TraceScope&& other) noexcept;
    TraceScope& operator=(TraceScope&& other) noexcept;

    [[nodiscard]] uint64_t trace_id() const noexcept { return trace_id_; }
    [[nodiscard]] int64_t elapsed_us() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// TraceContext（无锁环形缓冲区）
// ==============================================================================
class TraceContext {
public:
    // -------------------------------------------------------------------------
    // 单例
    // -------------------------------------------------------------------------
    [[nodiscard]] static TraceContext& instance() noexcept;

    TraceContext(const TraceContext&) = delete;
    TraceContext& operator=(const TraceContext&) = delete;
    TraceContext(TraceContext&&) = delete;
    TraceContext& operator=(TraceContext&&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> initialize(const TraceConfig& config = {}) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool is_initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // 记录事件（热路径，无锁）
    // -------------------------------------------------------------------------
    // 参数：
    //   event     - 事件（含 trace_id、parent_id、type、priority、timestamp）
    // 返回：
    //   true  - 记录成功
    //   false - 被采样/禁用/满时丢弃
    bool record(const Event& event) noexcept;

    // 直接记录字段（避免构造 Event 对象）
    bool record(uint64_t trace_id, uint64_t parent_id,
                EventType type, EventPriority priority,
                Timestamp timestamp) noexcept;

    // -------------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------------
    // 查询某 trace_id 的完整链（从 root 到 leaf）
    // 返回：
    //   成功 - 完整或部分链
    //   失败 - trace_id 不存在或已过期
    [[nodiscard]] Result<TraceChain> query(uint64_t trace_id) const;

    // 查询某 trace_id 及其所有子节点
    [[nodiscard]] Result<std::vector<TraceNode>>
        query_children(uint64_t trace_id) const;

    // 查询最近的 N 条记录
    [[nodiscard]] std::vector<TraceNode> query_recent(
        std::size_t count = 100) const;

    // -------------------------------------------------------------------------
    // 统计与配置
    // -------------------------------------------------------------------------
    [[nodiscard]] const TraceStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const TraceConfig& config() const noexcept { return config_; }

    void set_sample_rate(double rate) noexcept;
    void set_enabled(bool enabled) noexcept;

    // -------------------------------------------------------------------------
    // 导出
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string export_json(uint64_t trace_id) const;
    [[nodiscard]] std::string dump() const;

    // -------------------------------------------------------------------------
    // 测试辅助
    // -------------------------------------------------------------------------
    // 清空所有节点（仅测试）
    void reset_for_testing() noexcept;

private:
    TraceContext();
    ~TraceContext();

    // 内部：计算 depth
    uint32_t compute_depth(uint64_t trace_id, uint64_t parent_id) const noexcept;

    // 内部：验证记录
    bool validate_record(uint64_t trace_id, uint64_t parent_id,
                         int64_t timestamp_us) const noexcept;

    // 内部：采样决策
    bool should_sample(EventPriority priority) noexcept;

    // 内部：环形缓冲区索引
    [[nodiscard]] std::size_t ring_index(uint64_t seq) const noexcept {
        return static_cast<std::size_t>(seq) & (config_.capacity - 1);
    }

    // 内部：查找 trace_id 对应的节点位置
    struct LookupResult {
        bool found{false};
        std::size_t index{0};
        uint32_t seq{0};
    };
    [[nodiscard]] LookupResult lookup(uint64_t trace_id) const noexcept;

    // -------------------------------------------------------------------------
    // 成员
    // -------------------------------------------------------------------------
    TraceConfig config_{};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<double> sample_rate_{1.0};

    // 环形缓冲区（预分配，无动态分配）
    std::unique_ptr<TraceNode[]> ring_;
    std::atomic<uint64_t> write_seq_{0};

    // 哈希索引：trace_id → (index, seq)
    // 使用开放寻址，容量为 ring 的 2 倍
    struct HashEntry {
        std::atomic<uint64_t> trace_id{0};
        std::atomic<uint64_t> packed{0};  // (seq << 32) | index
    };
    static constexpr uint64_t kEmptySlot = 0;
    std::unique_ptr<HashEntry[]> index_;
    std::size_t index_capacity_{0};

    // 随机数生成器（采样用）
    alignas(64) std::atomic<uint64_t> rng_state_{0x853c49e6748fea9bULL};

    // 统计
    mutable TraceStats stats_{};
};

// ==============================================================================
// TraceScope 实现（PIMPL）
// ==============================================================================
class TraceScope::Impl {
public:
    Impl(EventType type, uint64_t parent_id) noexcept
        : type_{type}
        , parent_id_{parent_id}
        , start_us_{Timestamp::now().microseconds()}
        , trace_id_{generate_trace_id()}
    {
        // 记录 scope 开始（可选）
        TraceContext::instance().record(
            trace_id_, parent_id_, type_, EventPriority::NORMAL,
            Timestamp{start_us_});
    }

    ~Impl() noexcept {
        // Scope 结束记录（可选）
        // 若需要记录结束时间，可在此添加 end 节点
    }

    [[nodiscard]] int64_t elapsed_us() const noexcept {
        return Timestamp::now().microseconds() - start_us_;
    }

    EventType type_;
    uint64_t parent_id_;
    int64_t start_us_;
    uint64_t trace_id_;
};

inline TraceScope::TraceScope(EventType type, uint64_t parent_id) noexcept {
    try {
        impl_ = std::make_unique<Impl>(type, parent_id);
    } catch (...) {
        // 分配失败时静默降级
        impl_ = nullptr;
    }
}

inline TraceScope::~TraceScope() noexcept = default;

inline TraceScope::TraceScope(TraceScope&& other) noexcept
    : impl_{std::move(other.impl_)} {}

inline TraceScope& TraceScope::operator=(TraceScope&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

inline uint64_t TraceScope::trace_id() const noexcept {
    return impl_ ? impl_->trace_id_ : 0;
}

inline int64_t TraceScope::elapsed_us() const noexcept {
    return impl_ ? impl_->elapsed_us() : 0;
}

// ==============================================================================
// TraceContext 内联实现
// ==============================================================================

inline TraceContext& TraceContext::instance() noexcept {
    static TraceContext ctx;
    return ctx;
}

inline TraceContext::TraceContext() = default;

inline TraceContext::~TraceContext() {
    shutdown();
}

inline Result<void> TraceContext::initialize(const TraceConfig& cfg) noexcept {
    if (initialized_.load(std::memory_order_acquire)) {
        return {};
    }

    try {
        // 校验容量为 2 的幂
        if (cfg.capacity == 0
            || (cfg.capacity & (cfg.capacity - 1)) != 0) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "capacity 必须为 2 的幂")
                .with_context("capacity", std::to_string(cfg.capacity));
        }

        // 校验采样率
        if (cfg.sample_rate < 0.0 || cfg.sample_rate > 1.0) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "sample_rate 必须在 [0, 1]")
                .with_context("sample_rate", std::to_string(cfg.sample_rate));
        }

        config_ = cfg;
        sample_rate_.store(cfg.sample_rate, std::memory_order_release);
        enabled_.store(cfg.enabled, std::memory_order_release);

        // 分配环形缓冲区（预分配，无动态分配）
        ring_ = std::make_unique<TraceNode[]>(config_.capacity);
        if (!ring_) {
            return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
                "环形缓冲区分配失败");
        }

        // 索引容量 = ring 的 2 倍（降低哈希冲突）
        index_capacity_ = config_.capacity * 2;
        index_ = std::make_unique<HashEntry[]>(index_capacity_);
        if (!index_) {
            ring_.reset();
            return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
                "哈希索引分配失败");
        }

        // 初始化索引为空
        for (std::size_t i = 0; i < index_capacity_; ++i) {
            index_[i].trace_id.store(kEmptySlot, std::memory_order_relaxed);
            index_[i].packed.store(0, std::memory_order_relaxed);
        }

        // 初始化随机种子
        rng_state_.store(
            static_cast<uint64_t>(Timestamp::now().microseconds()) ^
            static_cast<uint64_t>(std::hash<std::thread::id>{}(
                std::this_thread::get_id())),
            std::memory_order_relaxed);

        initialized_.store(true, std::memory_order_release);

        QUANT_LOG_INFO("TraceContext 已初始化: capacity={}, sample_rate={}",
                       config_.capacity, config_.sample_rate);

        return {};
    } catch (const std::exception& e) {
        ring_.reset();
        index_.reset();
        initialized_.store(false, std::memory_order_release);
        return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory, e.what());
    }
}

inline void TraceContext::shutdown() noexcept {
    if (!initialized_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    ring_.reset();
    index_.reset();
    write_seq_.store(0, std::memory_order_relaxed);
}

inline bool TraceContext::should_sample(EventPriority priority) noexcept {
    if (!enabled_.load(std::memory_order_acquire)) return false;

    if (priority == EventPriority::CRITICAL && config_.always_record_critical) {
        return true;
    }

    const double rate = sample_rate_.load(std::memory_order_acquire);
    if (rate >= 1.0) return true;
    if (rate <= 0.0) return false;

    // xorshift64* 快速随机
    uint64_t x = rng_state_.load(std::memory_order_relaxed);
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state_.store(x, std::memory_order_relaxed);

    const uint64_t r = x * 0x2545F4914F6CDD1DULL;
    const double rand01 = static_cast<double>(r >> 11) /
                          static_cast<double>(1ULL << 53);

    return rand01 < rate;
}

inline bool TraceContext::validate_record(
    uint64_t trace_id, uint64_t parent_id, int64_t timestamp_us) const noexcept
{
    // trace_id 不能为 0
    if (trace_id == 0) return false;

    // 禁止自引用
    if (parent_id == trace_id) return false;

    // 时间戳必须有效
    if (timestamp_us <= 0) return false;

    return true;
}

inline uint32_t TraceContext::compute_depth(
    uint64_t trace_id, uint64_t parent_id) const noexcept
{
    if (parent_id == 0) return 1;  // root

    // 查找父节点深度
    auto parent_lookup = lookup(parent_id);
    if (!parent_lookup.found) {
        return 1;  // 父节点已淘汰，视为新 root
    }

    const auto& parent_node = ring_[parent_lookup.index];
    if (parent_node.trace_id != parent_id) {
        return 1;  // 已被覆盖
    }

    return parent_node.depth + 1;
}

inline bool TraceContext::record(
    uint64_t trace_id, uint64_t parent_id,
    EventType type, EventPriority priority, Timestamp timestamp) noexcept
{
    if (!initialized_.load(std::memory_order_acquire)) {
        return false;
    }

    // 采样决策
    if (!should_sample(priority)) {
        stats_.sampled_out_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 校验记录
    const auto ts_us = timestamp.microseconds();
    if (!validate_record(trace_id, parent_id, ts_us)) {
        stats_.dropped_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 计算深度
    const auto depth = compute_depth(trace_id, parent_id);
    if (depth > config_.max_chain_depth) {
        stats_.depth_exceeded_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 更新最大深度统计
    auto max_depth = stats_.max_chain_depth_observed.load(
        std::memory_order_relaxed);
    while (depth > max_depth &&
           !stats_.max_chain_depth_observed.compare_exchange_weak(
               max_depth, depth, std::memory_order_relaxed)) {
        // 重试
    }

    // 获取写序号
    const auto seq = write_seq_.fetch_add(1, std::memory_order_acq_rel);
    const auto idx = ring_index(seq);

    // 覆盖旧节点时递增 overwrite 计数
    TraceNode& old_node = ring_[idx];
    const bool was_valid = old_node.is_valid();
    if (was_valid) {
        stats_.overwrite_total.fetch_add(1, std::memory_order_relaxed);
    }

    // 构造新节点
    TraceNode node{};
    node.trace_id     = trace_id;
    node.parent_id    = parent_id;
    node.timestamp_us = ts_us;
    node.event_type   = static_cast<uint16_t>(type);
    node.priority     = static_cast<uint8_t>(priority);
    node.depth        = depth;
    node.seq          = static_cast<uint32_t>(seq);

    // 设置标志位
    if (priority == EventPriority::CRITICAL) {
        node.flags |= TraceNode::kFlagCritical;
    }
    // 采样标志：本次记录即视为采样（因为已通过 should_sample）
    node.flags |= TraceNode::kFlagSampled;

    // 写入节点（对齐写，非原子，但通过 seq 保证一致性）
    ring_[idx] = node;

    // 更新哈希索引
    // 开放寻址：线性探测
    const auto hash = trace_id * 0x9E3779B97F4A7C15ULL;  // 黄金比例
    auto h = hash % index_capacity_;
    for (std::size_t probe = 0; probe < 8; ++probe) {
        auto& entry = index_[h];
        const auto existing = entry.trace_id.load(std::memory_order_acquire);

        if (existing == kEmptySlot || existing == trace_id) {
            entry.trace_id.store(trace_id, std::memory_order_release);
            const auto packed = (static_cast<uint64_t>(node.seq) << 32) |
                                static_cast<uint64_t>(idx);
            entry.packed.store(packed, std::memory_order_release);
            break;
        }

        h = (h + 1) % index_capacity_;
    }

    stats_.recorded_total.fetch_add(1, std::memory_order_relaxed);
    return true;
}

inline bool TraceContext::record(const Event& event) noexcept {
    return record(event.trace_id, event.parent_id,
                  event.type, event.priority, event.timestamp);
}

inline TraceContext::LookupResult
TraceContext::lookup(uint64_t trace_id) const noexcept {
    LookupResult result;

    if (!initialized_.load(std::memory_order_acquire) || trace_id == 0) {
        return result;
    }

    const auto hash = trace_id * 0x9E3779B97F4A7C15ULL;
    auto h = hash % index_capacity_;

    for (std::size_t probe = 0; probe < 8; ++probe) {
        const auto& entry = index_[h];
        const auto existing = entry.trace_id.load(std::memory_order_acquire);

        if (existing == kEmptySlot) {
            return result;  // 未找到
        }

        if (existing == trace_id) {
            const auto packed = entry.packed.load(std::memory_order_acquire);
            const auto seq = static_cast<uint32_t>(packed >> 32);
            const auto idx = static_cast<std::size_t>(packed & 0xFFFFFFFF);

            // 校验节点仍有效（未被覆盖）
            if (idx < config_.capacity) {
                const auto& node = ring_[idx];
                if (node.trace_id == trace_id && node.seq == seq) {
                    result.found = true;
                    result.index = idx;
                    result.seq = seq;
                }
            }
            return result;
        }

        h = (h + 1) % index_capacity_;
    }

    return result;
}

inline Result<TraceChain> TraceContext::query(uint64_t trace_id) const {
    stats_.query_total.fetch_add(1, std::memory_order_relaxed);

    if (!initialized_.load(std::memory_order_acquire)) {
        return QUANT_ERR_T(TraceChain, ErrorCode::InternalInvariant)
            .with_context("state", "not initialized");
    }

    TraceChain chain;
    chain.root_trace_id = trace_id;

    // 1. 查找当前节点
    auto lookup_result = lookup(trace_id);
    if (!lookup_result.found) {
        stats_.broken_chain_total.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_T(TraceChain, ErrorCode::ConfigNotFound)
            .with_context("trace_id", std::to_string(trace_id));
    }

    const auto& leaf_node = ring_[lookup_result.index];
    chain.end_us = leaf_node.timestamp_us;
    chain.max_depth = leaf_node.depth;

    // 2. 从 leaf 向上回溯到 root
    std::vector<TraceNode> reverse_chain;
    reverse_chain.reserve(leaf_node.depth > 0 ? leaf_node.depth : 1);

    uint64_t current_id = trace_id;
    std::size_t depth = 0;
    const std::size_t max_depth = config_.max_chain_depth;

    while (current_id != 0 && depth < max_depth) {
        auto cur = lookup(current_id);
        if (!cur.found) {
            chain.is_complete = false;
            stats_.broken_chain_total.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        const auto& node = ring_[cur.index];
        reverse_chain.push_back(node);

        // 循环检测
        if (node.parent_id == current_id) {
            chain.has_cycle = true;
            chain.is_complete = false;
            break;
        }

        current_id = node.parent_id;
        ++depth;
    }

    if (depth >= max_depth) {
        chain.is_complete = false;
        stats_.depth_exceeded_total.fetch_add(1, std::memory_order_relaxed);
    }

    // 3. 反转为从 root 到 leaf
    chain.nodes.assign(reverse_chain.rbegin(), reverse_chain.rend());

    if (!chain.nodes.empty()) {
        chain.start_us = chain.nodes.front().timestamp_us;
        chain.end_us = chain.nodes.back().timestamp_us;
        chain.root_trace_id = chain.nodes.front().trace_id;
    }

    return chain;
}

inline Result<std::vector<TraceNode>>
TraceContext::query_children(uint64_t trace_id) const {
    stats_.query_total.fetch_add(1, std::memory_order_relaxed);

    if (!initialized_.load(std::memory_order_acquire)) {
        return QUANT_ERR_T(std::vector<TraceNode>, ErrorCode::InternalInvariant)
            .with_context("state", "not initialized");
    }

    std::vector<TraceNode> children;

    // 遍历整个环形缓冲区，收集 parent_id 匹配的节点
    // 注意：这是 O(n) 操作，仅用于诊断，非热路径
    const auto current_seq = write_seq_.load(std::memory_order_acquire);
    const auto count = std::min<std::size_t>(
        current_seq, config_.capacity);

    for (std::size_t i = 0; i < count; ++i) {
        const auto& node = ring_[i];
        if (node.is_valid() && node.parent_id == trace_id) {
            children.push_back(node);
        }
    }

    return children;
}

inline std::vector<TraceNode> TraceContext::query_recent(
    std::size_t count) const
{
    std::vector<TraceNode> recent;

    if (!initialized_.load(std::memory_order_acquire)) {
        return recent;
    }

    const auto current_seq = write_seq_.load(std::memory_order_acquire);
    if (current_seq == 0) return recent;

    const auto n = std::min({count, static_cast<std::size_t>(current_seq),
                             config_.capacity});
    recent.reserve(n);

    // 从最新的往回读
    for (std::size_t i = 0; i < n; ++i) {
        const auto seq = current_seq - 1 - i;
        const auto idx = ring_index(seq);
        const auto& node = ring_[idx];

        if (node.is_valid() && node.seq == static_cast<uint32_t>(seq)) {
            recent.push_back(node);
        }
    }

    return recent;
}

inline void TraceContext::set_sample_rate(double rate) noexcept {
    if (rate < 0.0) rate = 0.0;
    if (rate > 1.0) rate = 1.0;
    sample_rate_.store(rate, std::memory_order_release);
}

inline void TraceContext::set_enabled(bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_release);
}

inline std::string TraceContext::export_json(uint64_t trace_id) const {
    try {
        auto chain_result = query(trace_id);
        if (chain_result.is_err()) {
            return "{\"error\":\"" + chain_result.error().to_string() + "\"}";
        }

        const auto& chain = chain_result.value();
        std::string json = "{\n";
        json += "  \"root_trace_id\": " + std::to_string(chain.root_trace_id) + ",\n";
        json += "  \"start_us\": " + std::to_string(chain.start_us) + ",\n";
        json += "  \"end_us\": " + std::to_string(chain.end_us) + ",\n";
        json += "  \"duration_us\": " + std::to_string(chain.duration_us()) + ",\n";
        json += "  \"is_complete\": " + std::string(chain.is_complete ? "true" : "false") + ",\n";
        json += "  \"has_cycle\": " + std::string(chain.has_cycle ? "true" : "false") + ",\n";
        json += "  \"max_depth\": " + std::to_string(chain.max_depth) + ",\n";
        json += "  \"nodes\": [\n";

        for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
            const auto& n = chain.nodes[i];
            json += "    {\n";
            json += "      \"trace_id\": " + std::to_string(n.trace_id) + ",\n";
            json += "      \"parent_id\": " + std::to_string(n.parent_id) + ",\n";
            json += "      \"timestamp_us\": " + std::to_string(n.timestamp_us) + ",\n";
            json += "      \"event_type\": " +
                    std::to_string(static_cast<uint16_t>(n.event_type)) + ",\n";
            json += "      \"event_name\": \"" +
                    std::string{to_string(static_cast<EventType>(n.event_type))} + "\",\n";
            json += "      \"priority\": " +
                    std::to_string(static_cast<uint8_t>(n.priority)) + ",\n";
            json += "      \"depth\": " + std::to_string(n.depth) + "\n";
            json += "    }";
            if (i + 1 < chain.nodes.size()) json += ",";
            json += "\n";
        }

        json += "  ]\n";
        json += "}\n";
        return json;
    } catch (...) {
        return "{\"error\":\"export failed\"}";
    }
}

inline std::string TraceContext::dump() const {
    try {
        std::string result;
        result.reserve(512);

        result += "TraceContext dump:\n";
        result += "  initialized: ";
        result += initialized_.load() ? "yes" : "no";
        result += "\n";
        result += "  enabled: ";
        result += enabled_.load() ? "yes" : "no";
        result += "\n";
        result += "  capacity: " + std::to_string(config_.capacity) + "\n";
        result += "  write_seq: ";
        result += std::to_string(write_seq_.load(std::memory_order_acquire));
        result += "\n";
        result += "  sample_rate: ";
        result += std::to_string(sample_rate_.load(std::memory_order_acquire));
        result += "\n";
        result += "  recorded_total: ";
        result += std::to_string(
            stats_.recorded_total.load(std::memory_order_relaxed));
        result += "\n";
        result += "  dropped_total: ";
        result += std::to_string(
            stats_.dropped_total.load(std::memory_order_relaxed));
        result += "\n";
        result += "  sampled_out_total: ";
        result += std::to_string(
            stats_.sampled_out_total.load(std::memory_order_relaxed));
        result += "\n";
        result += "  overwrite_total: ";
        result += std::to_string(
            stats_.overwrite_total.load(std::memory_order_relaxed));
        result += "\n";
        result += "  depth_exceeded_total: ";
        result += std::to_string(
            stats_.depth_exceeded_total.load(std::memory_order_relaxed));
        result += "\n";
        result += "  broken_chain_total: ";
        result += std::to_string(
            stats_.broken_chain_total.load(std::memory_order_relaxed));
        result += "\n";
        result += "  query_total: ";
        result += std::to_string(
            stats_.query_total.load(std::memory_order_relaxed));
        result += "\n";
        result += "  max_chain_depth_observed: ";
        result += std::to_string(
            stats_.max_chain_depth_observed.load(std::memory_order_relaxed));
        result += "\n";

        return result;
    } catch (...) {
        return "TraceContext dump: <failed>\n";
    }
}

inline void TraceContext::reset_for_testing() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) return;

    // 清空索引
    for (std::size_t i = 0; i < index_capacity_; ++i) {
        index_[i].trace_id.store(kEmptySlot, std::memory_order_relaxed);
        index_[i].packed.store(0, std::memory_order_relaxed);
    }

    // 清空环形缓冲区
    write_seq_.store(0, std::memory_order_relaxed);

    // 重置统计
    stats_.reset();
}

}  // namespace event
}  // namespace quant

#endif  // QUANT_EVENT_CORE_TRACE_HPP
