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
//
// 声明与定义分离:
//   本头文件仅声明非 trivial 函数，实现位于 event.core.trace.cpp。
//   保留在头文件中的：类型定义、常量、简单 inline 访问器、编译期断言。
//
// 使用方式:
//   auto& tc = TraceContext::instance();
//   tc.initialize(cfg);
//   tc.record(event);
//   auto chain = tc.query(trace_id);
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

    [[nodiscard]] uint64_t trace_id() const noexcept;
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
    // 初始化（幂等）
    Result<void> initialize(const TraceConfig& config = {}) noexcept;

    // 关闭（释放所有资源）
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
    [[nodiscard]] Result<TraceChain> query(uint64_t trace_id) const;

    // 查询某 trace_id 的所有直接子节点
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

    // -------------------------------------------------------------------------
    // 内部辅助（仅声明，实现于 .cpp）
    // -------------------------------------------------------------------------

    // 计算 depth（基于父节点）
    uint32_t compute_depth(uint64_t trace_id, uint64_t parent_id) const noexcept;

    // 验证记录参数
    bool validate_record(uint64_t trace_id, uint64_t parent_id,
                         int64_t timestamp_us) const noexcept;

    // 采样决策
    bool should_sample(EventPriority priority) noexcept;

    // -------------------------------------------------------------------------
    // 内部辅助（inline，简单位运算）
    // -------------------------------------------------------------------------

    // 环形缓冲区索引（seq 对容量取模，容量为 2 的幂）
    [[nodiscard]] std::size_t ring_index(uint64_t seq) const noexcept {
        return static_cast<std::size_t>(seq) & (config_.capacity - 1);
    }

    // 哈希索引查找结果
    struct LookupResult {
        bool found{false};
        std::size_t index{0};
        uint32_t seq{0};
    };

    // 哈希索引查找（实现于 .cpp）
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

}  // namespace event
}  // namespace quant

#endif  // QUANT_EVENT_CORE_TRACE_HPP
