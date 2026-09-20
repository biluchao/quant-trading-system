// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 事件持久化
// ==============================================================================
// @file    src/event/event.core.persistence.hpp
// @module  event
// @type    core
// @name    persistence
// @version 1.0.2
// @brief   事件持久化：分段日志、CRC 校验、重放、保留策略
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 崩溃安全：分段 + 记录级 CRC + 尾部截断恢复
//   - 异步写入：发布路径不阻塞，后台线程批量刷盘
//   - 零拷贝：mmap 读取，writev 批量写入
//   - 分段轮转：按大小或时间切片，便于清理与归档
//   - 时间索引：每段记录起止时间，快速定位
//   - 重放：按时间范围流式重放，支持过滤
//   - 保留策略：最大磁盘用量 + 最老段优先删除
//   - 磁盘保护：空间不足时拒绝写入并告警
//   - 可观测：完整统计
// ==============================================================================

#ifndef QUANT_EVENT_CORE_PERSISTENCE_HPP
#define QUANT_EVENT_CORE_PERSISTENCE_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"
#include "event/event.core.bus.hpp"
#include "event/event.core.serializer.hpp"

namespace quant {
namespace event {

// ==============================================================================
// 持久化配置
// ==============================================================================
struct PersistConfig {
    // 存储目录
    std::filesystem::path directory{"data/event_log"};

    // 单段最大字节（默认 256 MB）
    std::size_t segment_max_bytes{256 * 1024 * 1024};

    // 单段最大时长（秒，默认 1 小时）
    std::uint32_t segment_max_seconds{3600};

    // 保留总字节（默认 10 GB）
    std::size_t retention_max_bytes{10ULL * 1024 * 1024 * 1024};

    // 保留最大天数
    std::uint32_t retention_max_days{30};

    // 异步队列容量
    std::size_t async_queue_capacity{65536};

    // 批量写入阈值（累积后刷盘）
    std::size_t batch_size{256};

    // 刷盘间隔（毫秒）
    std::uint32_t flush_interval_ms{100};

    // fsync 间隔（毫秒，0 表示不 fsync）
    std::uint32_t fsync_interval_ms{1000};

    // 是否启用压缩（zstd，需链接 libzstd）
    bool enable_compression{false};

    // 最小磁盘剩余空间（低于此值拒绝写入）
    std::size_t min_free_disk_bytes{512ULL * 1024 * 1024};  // 512 MB

    // 订阅哪些 EventType（空 = 订阅全部）
    std::vector<EventType> subscribe_types{};

    // 是否记录 CRITICAL 事件的同步写入
    bool sync_on_critical{true};

    // 序列化格式
    SerializationFormat format{SerializationFormat::MESSAGE_PACK};
};

// ==============================================================================
// 持久化统计
// ==============================================================================
struct PersistStats {
    alignas(64) std::atomic<uint64_t> written_total{0};
    alignas(64) std::atomic<uint64_t> written_bytes{0};
    alignas(64) std::atomic<uint64_t> dropped_total{0};
    alignas(64) std::atomic<uint64_t> write_error_total{0};
    alignas(64) std::atomic<uint64_t> fsync_total{0};
    alignas(64) std::atomic<uint64_t> segment_rotated_total{0};
    alignas(64) std::atomic<uint64_t> segment_cleaned_total{0};
    alignas(64) std::atomic<uint64_t> replayed_total{0};
    alignas(64) std::atomic<uint64_t> disk_full_total{0};
    alignas(64) std::atomic<uint64_t> corrupted_record_total{0};

    void reset() noexcept {
        written_total.store(0, std::memory_order_relaxed);
        written_bytes.store(0, std::memory_order_relaxed);
        dropped_total.store(0, std::memory_order_relaxed);
        write_error_total.store(0, std::memory_order_relaxed);
        fsync_total.store(0, std::memory_order_relaxed);
        segment_rotated_total.store(0, std::memory_order_relaxed);
        segment_cleaned_total.store(0, std::memory_order_relaxed);
        replayed_total.store(0, std::memory_order_relaxed);
        disk_full_total.store(0, std::memory_order_relaxed);
        corrupted_record_total.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// 段信息
// ==============================================================================
struct SegmentInfo {
    std::filesystem::path path;
    uint64_t sequence{0};
    int64_t start_time_us{0};
    int64_t end_time_us{0};
    std::size_t bytes{0};
    std::size_t record_count{0};
};

// ==============================================================================
// 重放选项
// ==============================================================================
struct ReplayOptions {
    int64_t start_time_us{0};        // 0 = 最早
    int64_t end_time_us{0};          // 0 = 最新
    std::vector<EventType> filter{}; // 空 = 全部
    std::size_t max_records{0};      // 0 = 无限
    bool continue_on_error{true};
};

// ==============================================================================
// 重放回调（返回 false 表示停止重放）
// ==============================================================================
using ReplayCallback = std::function<bool(const Event&)>;

// ==============================================================================
// EventPersistence（事件持久化）
// ==============================================================================
class EventPersistence {
public:
    // -------------------------------------------------------------------------
    // 单例
    // -------------------------------------------------------------------------
    [[nodiscard]] static EventPersistence& instance() noexcept;

    EventPersistence(const EventPersistence&) = delete;
    EventPersistence& operator=(const EventPersistence&) = delete;
    EventPersistence(EventPersistence&&) = delete;
    EventPersistence& operator=(EventPersistence&&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> initialize(const PersistConfig& config = {}) noexcept;
    Result<void> start() noexcept;
    Result<void> stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // 写入（异步，热路径无阻塞）
    // -------------------------------------------------------------------------
    // 追加一条事件到持久化队列
    Result<void> append(const Event& event) noexcept;

    // 批量追加
    Result<void> append_batch(std::span<const Event> events) noexcept;

    // 强制刷盘（等待队列清空）
    Result<void> flush() noexcept;

    // -------------------------------------------------------------------------
    // 重放
    // -------------------------------------------------------------------------
    Result<std::size_t> replay(const ReplayOptions& options,
                               ReplayCallback callback) const;

    // 列出所有段
    [[nodiscard]] std::vector<SegmentInfo> list_segments() const;

    // 清理旧段（按保留策略）
    Result<std::size_t> cleanup() noexcept;

    // -------------------------------------------------------------------------
    // 统计与配置
    // -------------------------------------------------------------------------
    [[nodiscard]] const PersistStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const PersistConfig& config() const noexcept { return config_; }

    [[nodiscard]] std::size_t queue_size() const noexcept;
    [[nodiscard]] std::size_t current_segment_bytes() const noexcept;
    [[nodiscard]] std::size_t total_disk_bytes() const;

    [[nodiscard]] std::string dump() const;

private:
    EventPersistence();
    ~EventPersistence();

    // 内部
    void writer_loop() noexcept;
    void cleanup_loop() noexcept;

    Result<void> ensure_directory() const;
    Result<void> open_new_segment() noexcept;
    Result<void> close_current_segment() noexcept;
    Result<void> write_record(const Event& event) noexcept;
    Result<void> rotate_if_needed() noexcept;
    Result<void> fsync_if_needed() noexcept;

    [[nodiscard]] std::size_t check_free_disk_space() const;

    // -------------------------------------------------------------------------
    // 成员
    // -------------------------------------------------------------------------
    PersistConfig config_{};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};

    // 异步写入队列（SPSC）
    class AsyncQueue;
    std::unique_ptr<AsyncQueue> queue_;

    // 写入线程
    std::thread writer_thread_;
    std::thread cleanup_thread_;
    mutable std::mutex writer_mutex_;
    std::condition_variable writer_cv_;

    // 当前段（writer 线程独占）
    struct SegmentState {
        std::filesystem::path path;
        int fd{-1};
        std::size_t bytes{0};
        std::size_t record_count{0};
        uint64_t sequence{0};
        int64_t start_time_us{0};
        int64_t end_time_us{0};
        int64_t segment_start_us{0};
    };
    SegmentState current_;

    // 段列表（读写锁保护）
    mutable std::shared_mutex segments_mutex_;
    std::vector<SegmentInfo> segments_;

    // fsync 节流
    std::atomic<int64_t> last_fsync_us_{0};

    // 统计
    PersistStats stats_{};
};

// ==============================================================================
// 便捷：自动订阅 EventBus 并持久化
// ==============================================================================
class EventBusPersister {
public:
    explicit EventBusPersister(const PersistConfig& config = {});
    ~EventBusPersister();

    EventBusPersister(const EventBusPersister&) = delete;
    EventBusPersister& operator=(const EventBusPersister&) = delete;

    [[nodiscard]] Result<void> start();
    [[nodiscard]] Result<void> stop();

private:
    std::vector<Subscription> subscriptions_;
    bool started_{false};
};

}  // namespace event
}  // namespace quant

#endif  // QUANT_EVENT_CORE_PERSISTENCE_HPP
