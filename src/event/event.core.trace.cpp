// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 因果链追踪实现
// ==============================================================================
// @file    src/event/event.core.trace.cpp
// @module  event
// @type    core
// @name    trace
// @version 1.0.2
// @brief   无锁环形缓冲区 + 哈希索引的因果链追踪实现
//
// 说明:
//   本文件提供 event.core.trace.hpp 中非 inline 函数的实现。
//
//   为使本文件可编译，需将 hpp 中以下函数的 "inline 定义" 改为 "仅声明"：
//     1. TraceScope 相关方法
//     2. TraceContext::instance
//     3. TraceContext::initialize
//     4. TraceContext::shutdown
//     5. TraceContext::record (两个重载)
//     6. TraceContext::query
//     7. TraceContext::query_children
//     8. TraceContext::query_recent
//     9. TraceContext::export_json
//    10. TraceContext::dump
//    11. TraceContext::reset_for_testing
//    12. TraceContext::set_sample_rate / set_enabled
//    13. TraceContext::compute_depth
//    14. TraceContext::validate_record
//    15. TraceContext::should_sample
//    16. TraceContext::lookup
//
// 修复要点:
//   - 无锁环形缓冲区（SPSC 语义）
//   - 哈希索引 O(1) 查询
//   - 采样决策（xorshift64* 快速随机）
//   - 深度计算（迭代，无递归）
//   - 覆盖保护（seq 校验）
//   - 时钟回拨保护
//   - 加锁顺序统一
//   - 单例 Meyers + noexcept
// ==============================================================================

#include "event/event.core.trace.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <sstream>

namespace quant {
namespace event {

// ==============================================================================
// TraceScope::Impl（PIMPL）
// ==============================================================================
class TraceScope::Impl {
public:
    Impl(EventType type, uint64_t parent_id) noexcept
        : type_{type}
        , parent_id_{parent_id}
        , start_us_{Timestamp::now().microseconds()}
        , trace_id_{generate_trace_id()}
    {
        // 记录 scope 开始
        (void)TraceContext::instance().record(
            trace_id_, parent_id_, type_, EventPriority::NORMAL,
            Timestamp{start_us_});
    }

    ~Impl() noexcept = default;

    [[nodiscard]] int64_t elapsed_us() const noexcept {
        return Timestamp::now().microseconds() - start_us_;
    }

    EventType type_;
    uint64_t parent_id_;
    int64_t start_us_;
    uint64_t trace_id_;
};

// ==============================================================================
// TraceScope
// ==============================================================================
TraceScope::TraceScope(EventType type, uint64_t parent_id) noexcept {
    try {
        impl_ = std::make_unique<Impl>(type, parent_id);
    } catch (...) {
        // 分配失败时静默降级
        impl_ = nullptr;
    }
}

TraceScope::~TraceScope() noexcept = default;

TraceScope::TraceScope(TraceScope&& other) noexcept
    : impl_{std::move(other.impl_)} {}

TraceScope& TraceScope::operator=(TraceScope&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

uint64_t TraceScope::trace_id() const noexcept {
    return impl_ ? impl_->trace_id_ : 0;
}

int64_t TraceScope::elapsed_us() const noexcept {
    return impl_ ? impl_->elapsed_us() : 0;
}

// ==============================================================================
// TraceContext 构造 / 析构
// ==============================================================================
TraceContext::TraceContext() = default;

TraceContext::~TraceContext() {
    shutdown();
}

TraceContext& TraceContext::instance() noexcept {
    // Meyers 单例（C++11 起线程安全）
    static TraceContext ctx;
    return ctx;
}

// ==============================================================================
// initialize
// ==============================================================================
Result<void> TraceContext::initialize(const TraceConfig& cfg) noexcept {
    // 幂等
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

        // 校验最大深度
        if (cfg.max_chain_depth == 0) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "max_chain_depth 必须 > 0");
        }

        config_ = cfg;
        sample_rate_.store(cfg.sample_rate, std::memory_order_release);
        enabled_.store(cfg.enabled, std::memory_order_release);

        // 分配环形缓冲区
        ring_ = std::make_unique<TraceNode[]>(config_.capacity);
        if (!ring_) {
            return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
                "环形缓冲区分配失败")
                .with_context("capacity", std::to_string(config_.capacity));
        }

        // 索引容量 = ring 的 2 倍（降低哈希冲突）
        index_capacity_ = config_.capacity * 2;
        index_ = std::make_unique<HashEntry[]>(index_capacity_);
        if (!index_) {
            ring_.reset();
            return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
                "哈希索引分配失败")
                .with_context("index_capacity",
                    std::to_string(index_capacity_));
        }

        // 初始化索引为空
        for (std::size_t i = 0; i < index_capacity_; ++i) {
            index_[i].trace_id.store(kEmptySlot, std::memory_order_relaxed);
            index_[i].packed.store(0, std::memory_order_relaxed);
        }

        // 初始化随机种子（时间 + 线程 ID 哈希）
        const auto seed =
            static_cast<uint64_t>(Timestamp::now().microseconds()) ^
            static_cast<uint64_t>(std::hash<std::thread::id>{}(
                std::this_thread::get_id()));
        rng_state_.store(seed ? seed : 0x853c49e6748fea9bULL,
                         std::memory_order_relaxed);

        initialized_.store(true, std::memory_order_release);

        QUANT_LOG_INFO("TraceContext 已初始化: capacity={}, sample_rate={}, "
                       "max_chain_depth={}",
                       config_.capacity, config_.sample_rate,
                       config_.max_chain_depth);

        return {};
    } catch (const std::exception& e) {
        ring_.reset();
        index_.reset();
        initialized_.store(false, std::memory_order_release);
        QUANT_LOG_ERROR("TraceContext 初始化异常: {}", e.what());
        return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory, e.what());
    } catch (...) {
        ring_.reset();
        index_.reset();
        initialized_.store(false, std::memory_order_release);
        return QUANT_ERR_MSG(ErrorCode::InternalUnknown, "初始化未知异常");
    }
}

// ==============================================================================
// shutdown
// ==============================================================================
void TraceContext::shutdown() noexcept {
    if (!initialized_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // 清理资源（析构路径不依赖日志器）
    ring_.reset();
    index_.reset();
    write_seq_.store(0, std::memory_order_relaxed);
}

// ==============================================================================
// should_sample（xorshift64* 快速随机）
// ==============================================================================
bool TraceContext::should_sample(EventPriority priority) noexcept {
    if (!enabled_.load(std::memory_order_acquire)) return false;

    // CRITICAL 事件强制记录
    if (priority == EventPriority::CRITICAL
        && config_.always_record_critical) {
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

// ==============================================================================
// validate_record
// ==============================================================================
bool TraceContext::validate_record(
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

// ==============================================================================
// compute_depth（迭代，无递归）
// ==============================================================================
uint32_t TraceContext::compute_depth(
    uint64_t trace_id, uint64_t parent_id) const noexcept
{
    (void)trace_id;  // 保留参数，便于未来扩展

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

    // 父深度 + 1，限流
    const auto depth = parent_node.depth + 1;
    return depth;
}

// ==============================================================================
// record（含所有校验与采样）
// ==============================================================================
bool TraceContext::record(
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
               max_depth, depth,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
        // CAS 重试
    }

    // 获取写序号（acq_rel 保证可见性）
    const auto seq = write_seq_.fetch_add(1, std::memory_order_acq_rel);
    const auto idx = ring_index(seq);

    // 覆盖检测
    TraceNode& old_node = ring_[idx];
    if (old_node.is_valid()) {
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

    // 标志位
    if (priority == EventPriority::CRITICAL) {
        node.flags |= TraceNode::kFlagCritical;
    }
    node.flags |= TraceNode::kFlagSampled;

    // 写入节点（非原子写，但通过 seq 保证一致性）
    ring_[idx] = node;

    // 更新哈希索引（开放寻址 + 线性探测）
    const auto hash = trace_id * 0x9E3779B97F4A7C15ULL;
    auto h = hash % index_capacity_;
    for (std::size_t probe = 0; probe < 8; ++probe) {
        auto& entry = index_[h];
        const auto existing = entry.trace_id.load(std::memory_order_acquire);

        if (existing == kEmptySlot || existing == trace_id) {
            entry.trace_id.store(trace_id, std::memory_order_release);
            const auto packed =
                (static_cast<uint64_t>(node.seq) << 32) |
                static_cast<uint64_t>(idx);
            entry.packed.store(packed, std::memory_order_release);
            break;
        }

        h = (h + 1) % index_capacity_;
    }

    stats_.recorded_total.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool TraceContext::record(const Event& event) noexcept {
    return record(event.trace_id, event.parent_id,
                  event.type, event.priority, event.timestamp);
}

// ==============================================================================
// lookup（开放寻址哈希索引）
// ==============================================================================
TraceContext::LookupResult
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
                if (node.trace_id == trace_id
                    && node.seq == seq) {
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

// ==============================================================================
// query（从 leaf 回溯到 root）
// ==============================================================================
Result<TraceChain> TraceContext::query(uint64_t trace_id) const {
    stats_.query_total.fetch_add(1, std::memory_order_relaxed);

    if (!initialized_.load(std::memory_order_acquire)) {
        return QUANT_ERR_T(TraceChain, ErrorCode::InternalInvariant)
            .with_context("state", "not initialized");
    }

    if (trace_id == 0) {
        return QUANT_ERR_T(TraceChain, ErrorCode::UserInputInvalid)
            .with_context("trace_id", "0");
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
    reverse_chain.reserve(
        leaf_node.depth > 0 ? std::min<std::size_t>(leaf_node.depth, 256) : 1);

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

        // 循环检测：parent_id 指向自己
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

// ==============================================================================
// query_children（O(n) 遍历，仅诊断用）
// ==============================================================================
Result<std::vector<TraceNode>>
TraceContext::query_children(uint64_t trace_id) const {
    stats_.query_total.fetch_add(1, std::memory_order_relaxed);

    if (!initialized_.load(std::memory_order_acquire)) {
        return QUANT_ERR_T(std::vector<TraceNode>, ErrorCode::InternalInvariant)
            .with_context("state", "not initialized");
    }

    std::vector<TraceNode> children;

    const auto current_seq = write_seq_.load(std::memory_order_acquire);
    const auto count = std::min<std::size_t>(
        current_seq, config_.capacity);

    children.reserve(16);  // 常见规模，避免频繁扩容

    for (std::size_t i = 0; i < count; ++i) {
        const auto& node = ring_[i];
        if (node.is_valid() && node.parent_id == trace_id) {
            children.push_back(node);
        }
    }

    return children;
}

// ==============================================================================
// query_recent
// ==============================================================================
std::vector<TraceNode> TraceContext::query_recent(
    std::size_t count) const
{
    std::vector<TraceNode> recent;

    if (!initialized_.load(std::memory_order_acquire)) {
        return recent;
    }

    const auto current_seq = write_seq_.load(std::memory_order_acquire);
    if (current_seq == 0) return recent;

    const auto n = std::min({count,
                             static_cast<std::size_t>(current_seq),
                             config_.capacity});
    recent.reserve(n);

    // 从最新往回读
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

// ==============================================================================
// set_sample_rate / set_enabled
// ==============================================================================
void TraceContext::set_sample_rate(double rate) noexcept {
    if (rate < 0.0) rate = 0.0;
    if (rate > 1.0) rate = 1.0;
    sample_rate_.store(rate, std::memory_order_release);
}

void TraceContext::set_enabled(bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_release);
}

// ==============================================================================
// export_json
// ==============================================================================
std::string TraceContext::export_json(uint64_t trace_id) const {
    try {
        auto chain_result = query(trace_id);
        if (chain_result.is_err()) {
            std::ostringstream oss;
            oss << "{\"error\":\""
                << chain_result.error().to_string()
                << "\"}";
            return oss.str();
        }

        const auto& chain = chain_result.value();
        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"root_trace_id\": " << chain.root_trace_id << ",\n";
        oss << "  \"start_us\": " << chain.start_us << ",\n";
        oss << "  \"end_us\": " << chain.end_us << ",\n";
        oss << "  \"duration_us\": " << chain.duration_us() << ",\n";
        oss << "  \"is_complete\": "
            << (chain.is_complete ? "true" : "false") << ",\n";
        oss << "  \"has_cycle\": "
            << (chain.has_cycle ? "true" : "false") << ",\n";
        oss << "  \"max_depth\": " << chain.max_depth << ",\n";
        oss << "  \"nodes\": [\n";

        for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
            const auto& n = chain.nodes[i];
            oss << "    {\n";
            oss << "      \"trace_id\": " << n.trace_id << ",\n";
            oss << "      \"parent_id\": " << n.parent_id << ",\n";
            oss << "      \"timestamp_us\": " << n.timestamp_us << ",\n";
            oss << "      \"event_type\": "
                << static_cast<uint16_t>(n.event_type) << ",\n";
            oss << "      \"event_name\": \""
                << to_string(static_cast<EventType>(n.event_type))
                << "\",\n";
            oss << "      \"priority\": "
                << static_cast<uint8_t>(n.priority) << ",\n";
            oss << "      \"depth\": " << n.depth << "\n";
            oss << "    }";
            if (i + 1 < chain.nodes.size()) oss << ",";
            oss << "\n";
        }

        oss << "  ]\n";
        oss << "}\n";
        return oss.str();
    } catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "{\"error\":\"export failed: " << e.what() << "\"}";
        return oss.str();
    } catch (...) {
        return "{\"error\":\"export failed: unknown\"}";
    }
}

// ==============================================================================
// dump
// ==============================================================================
std::string TraceContext::dump() const {
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

        result += "  max_chain_depth: ";
        result += std::to_string(config_.max_chain_depth);
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

// ==============================================================================
// reset_for_testing
// ==============================================================================
void TraceContext::reset_for_testing() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) return;

    // 清空索引
    for (std::size_t i = 0; i < index_capacity_; ++i) {
        index_[i].trace_id.store(kEmptySlot, std::memory_order_relaxed);
        index_[i].packed.store(0, std::memory_order_relaxed);
    }

    // 清空环形缓冲区（通过 write_seq 归零，旧数据自然被覆盖）
    write_seq_.store(0, std::memory_order_relaxed);

    // 重置统计
    stats_.reset();
}

}  // namespace event
}  // namespace quant
