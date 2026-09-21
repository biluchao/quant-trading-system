// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 数据持久化存储
// ==============================================================================
// @file    src/data/data.core.storage.hpp
// @module  data
// @type    core
// @name    storage
// @version 1.0.1
// @brief   基于 PostgreSQL/TimescaleDB 的 K 线/订单/成交持久化
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 连接池：多连接复用，避免频繁建立
//   - 参数化查询：防止 SQL 注入
//   - 事务批量：COPY 协议，速度提升 10~100 倍
//   - 异步写入：写队列 + 后台线程，不阻塞主路径
//   - 自动重连：指数退避 + 健康检查
//   - TLS 加密：sslmode=require
//   - 预编译缓存：避免重复解析 SQL
//   - 错误映射：PostgreSQL 错误码 → 项目 ErrorCode
//   - 完整可观测：StorageStats + dump()
// ==============================================================================

#ifndef QUANT_DATA_CORE_STORAGE_HPP
#define QUANT_DATA_CORE_STORAGE_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace storage_config {

// 连接池
inline constexpr std::size_t kDefaultPoolSize = 8;
inline constexpr std::size_t kMinPoolSize = 1;
inline constexpr std::size_t kMaxPoolSize = 64;

// 连接超时
inline constexpr std::chrono::seconds kDefaultConnectTimeout{10};
inline constexpr std::chrono::seconds kDefaultQueryTimeout{30};

// 重试
inline constexpr int kDefaultMaxRetries = 3;
inline constexpr std::chrono::milliseconds kRetryInitialDelay{500};
inline constexpr std::chrono::milliseconds kRetryMaxDelay{10'000};

// 批量
inline constexpr std::size_t kDefaultBatchSize = 1000;
inline constexpr std::size_t kMaxBatchSize = 10'000;

// 写队列
inline constexpr std::size_t kDefaultWriteQueueSize = 100'000;

// 健康检查
inline constexpr std::chrono::seconds kDefaultHealthCheckInterval{30};

// 保留策略
inline constexpr std::chrono::days kDefaultRetentionDays{90};

}  // namespace storage_config

// ==============================================================================
// 事务隔离级别
// ==============================================================================
enum class IsolationLevel : std::uint8_t {
    ReadCommitted   = 0,
    RepeatableRead  = 1,
    Serializable    = 2,
};

[[nodiscard]] constexpr std::string_view to_string(IsolationLevel l) noexcept {
    switch (l) {
        case IsolationLevel::ReadCommitted:  return "READ COMMITTED";
        case IsolationLevel::RepeatableRead: return "REPEATABLE READ";
        case IsolationLevel::Serializable:   return "SERIALIZABLE";
    }
    return "READ COMMITTED";
}

// ==============================================================================
// 存储状态
// ==============================================================================
enum class StorageState : std::uint8_t {
    Disconnected = 0,
    Connecting   = 1,
    Connected    = 2,
    Degraded     = 3,   // 部分连接可用
    Failed       = 4,
};

[[nodiscard]] constexpr std::string_view to_string(StorageState s) noexcept {
    switch (s) {
        case StorageState::Disconnected: return "Disconnected";
        case StorageState::Connecting:   return "Connecting";
        case StorageState::Connected:    return "Connected";
        case StorageState::Degraded:     return "Degraded";
        case StorageState::Failed:       return "Failed";
    }
    return "Unknown";
}

// ==============================================================================
// 写入操作类型
// ==============================================================================
enum class WriteOp : std::uint8_t {
    InsertCandle   = 0,
    UpdateCandle   = 1,
    UpsertCandle   = 2,
    InsertOrder    = 3,
    InsertFill     = 4,
    InsertTrade    = 5,
    InsertSignal   = 6,
    Custom         = 7,
};

[[nodiscard]] constexpr std::string_view to_string(WriteOp op) noexcept {
    switch (op) {
        case WriteOp::InsertCandle: return "InsertCandle";
        case WriteOp::UpdateCandle: return "UpdateCandle";
        case WriteOp::UpsertCandle: return "UpsertCandle";
        case WriteOp::InsertOrder:  return "InsertOrder";
        case WriteOp::InsertFill:   return "InsertFill";
        case WriteOp::InsertTrade:  return "InsertTrade";
        case WriteOp::InsertSignal: return "InsertSignal";
        case WriteOp::Custom:       return "Custom";
    }
    return "Unknown";
}

// ==============================================================================
// 统计
// ==============================================================================
struct StorageStats {
    alignas(64) std::atomic<std::uint64_t> queries_total{0};
    alignas(64) std::atomic<std::uint64_t> queries_succeeded{0};
    alignas(64) std::atomic<std::uint64_t> queries_failed{0};
    alignas(64) std::atomic<std::uint64_t> queries_retried{0};
    alignas(64) std::atomic<std::uint64_t> queries_timeout{0};

    alignas(64) std::atomic<std::uint64_t> candles_written{0};
    alignas(64) std::atomic<std::uint64_t> candles_read{0};
    alignas(64) std::atomic<std::uint64_t> candles_upserted{0};
    alignas(64) std::atomic<std::uint64_t> orders_written{0};
    alignas(64) std::atomic<std::uint64_t> fills_written{0};

    alignas(64) std::atomic<std::uint64_t> bytes_written{0};
    alignas(64) std::atomic<std::uint64_t> bytes_read{0};

    alignas(64) std::atomic<std::uint64_t> transactions_committed{0};
    alignas(64) std::atomic<std::uint64_t> transactions_rolled_back{0};

    alignas(64) std::atomic<std::uint64_t> reconnects{0};
    alignas(64) std::atomic<std::uint64_t> health_check_failures{0};

    alignas(64) std::atomic<std::uint64_t> write_queue_drops{0};
    alignas(64) std::atomic<std::uint64_t> write_queue_size{0};
    alignas(64) std::atomic<std::uint64_t> write_queue_peak{0};

    alignas(64) std::atomic<std::int64_t> last_query_latency_us{0};
    alignas(64) std::atomic<std::int64_t> max_query_latency_us{0};
    alignas(64) std::atomic<std::int64_t> total_query_latency_us{0};

    void reset() noexcept {
        queries_total.store(0, std::memory_order_relaxed);
        queries_succeeded.store(0, std::memory_order_relaxed);
        queries_failed.store(0, std::memory_order_relaxed);
        queries_retried.store(0, std::memory_order_relaxed);
        queries_timeout.store(0, std::memory_order_relaxed);
        candles_written.store(0, std::memory_order_relaxed);
        candles_read.store(0, std::memory_order_relaxed);
        candles_upserted.store(0, std::memory_order_relaxed);
        orders_written.store(0, std::memory_order_relaxed);
        fills_written.store(0, std::memory_order_relaxed);
        bytes_written.store(0, std::memory_order_relaxed);
        bytes_read.store(0, std::memory_order_relaxed);
        transactions_committed.store(0, std::memory_order_relaxed);
        transactions_rolled_back.store(0, std::memory_order_relaxed);
        reconnects.store(0, std::memory_order_relaxed);
        health_check_failures.store(0, std::memory_order_relaxed);
        write_queue_drops.store(0, std::memory_order_relaxed);
        write_queue_size.store(0, std::memory_order_relaxed);
        write_queue_peak.store(0, std::memory_order_relaxed);
        last_query_latency_us.store(0, std::memory_order_relaxed);
        max_query_latency_us.store(0, std::memory_order_relaxed);
        total_query_latency_us.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double success_rate() const noexcept {
        const auto total = queries_total.load(std::memory_order_relaxed);
        if (total == 0) return 1.0;
        return static_cast<double>(
            queries_succeeded.load(std::memory_order_relaxed)) / total;
    }

    [[nodiscard]] double avg_latency_us() const noexcept {
        const auto total = queries_total.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            total_query_latency_us.load(std::memory_order_relaxed)) / total;
    }
};

// ==============================================================================
// 配置
// ==============================================================================
struct StorageConfig {
    // 连接
    std::string host{"localhost"};
    std::uint16_t port{5432};
    std::string database{"quant"};
    std::string user{"quant"};
    std::string password;                     // 从 Vault/环境变量读取
    std::string application_name{"quant-trading-system"};
    std::string ssl_mode{"require"};          // disable/allow/prefer/require

    // 连接池
    std::size_t pool_size{storage_config::kDefaultPoolSize};
    std::size_t min_idle{2};

    // 超时
    std::chrono::seconds connect_timeout{
        storage_config::kDefaultConnectTimeout};
    std::chrono::seconds query_timeout{
        storage_config::kDefaultQueryTimeout};

    // 重试
    int max_retries{storage_config::kDefaultMaxRetries};
    std::chrono::milliseconds retry_initial_delay{
        storage_config::kRetryInitialDelay};
    std::chrono::milliseconds retry_max_delay{
        storage_config::kRetryMaxDelay};

    // 批量
    std::size_t batch_size{storage_config::kDefaultBatchSize};

    // 写队列
    std::size_t write_queue_size{storage_config::kDefaultWriteQueueSize};
    bool enable_async_write{true};

    // 健康检查
    std::chrono::seconds health_check_interval{
        storage_config::kDefaultHealthCheckInterval};
    bool enable_health_check{true};

    // 隔离级别
    IsolationLevel isolation_level{IsolationLevel::ReadCommitted};

    // 保留
    std::chrono::days retention_days{storage_config::kDefaultRetentionDays};

    // 日志
    LogLevel log_level{LogLevel::INFO};

    // 构建连接字符串
    [[nodiscard]] std::string build_connection_string() const;
};

// ==============================================================================
// 事务句柄（RAII）
// ==============================================================================
class Transaction {
public:
    ~Transaction() noexcept;

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&& other) noexcept;

    // 提交
    Result<void> commit();

    // 回滚
    void rollback() noexcept;

    [[nodiscard]] bool is_active() const noexcept { return active_; }

private:
    friend class Storage;

    struct Impl;
    explicit Transaction(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
    bool active_{true};
    bool committed_{false};
};

// ==============================================================================
// 存储
// ==============================================================================
class Storage {
public:
    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit Storage(StorageConfig config);
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    Storage(Storage&&) = delete;
    Storage& operator=(Storage&&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> connect();
    void disconnect() noexcept;
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] StorageState state() const noexcept;

    // -------------------------------------------------------------------------
    // Schema 管理
    // -------------------------------------------------------------------------
    Result<void> ensure_schema();
    [[nodiscard]] Result<int32_t> schema_version() const;
    Result<void> migrate_to(int32_t target_version);

    // -------------------------------------------------------------------------
    // K 线写入
    // -------------------------------------------------------------------------
    // 单根插入
    Result<void> insert_candle(std::string_view symbol,
                                 const Candle& candle);

    // 批量插入（事务 + COPY）
    Result<void> insert_candles(std::string_view symbol,
                                  std::span<const Candle> candles);

    // Upsert（存在则更新）
    Result<void> upsert_candles(std::string_view symbol,
                                  std::span<const Candle> candles);

    // -------------------------------------------------------------------------
    // K 线读取
    // -------------------------------------------------------------------------
    Result<std::vector<Candle>>
        load_candles(std::string_view symbol,
                      Timestamp start,
                      Timestamp end,
                      std::optional<std::size_t> limit = std::nullopt);

    Result<std::vector<Candle>>
        load_latest_candles(std::string_view symbol,
                             std::size_t count);

    [[nodiscard]] Result<std::size_t>
        count_candles(std::string_view symbol,
                       Timestamp start,
                       Timestamp end) const;

    // -------------------------------------------------------------------------
    // 订单与成交
    // -------------------------------------------------------------------------
    Result<void> insert_order(const Order& order);
    Result<void> insert_orders(std::span<const Order> orders);

    Result<void> insert_fill(const Fill& fill);
    Result<void> insert_fills(std::span<const Fill> fills);

    Result<std::vector<Order>>
        load_orders(std::string_view symbol,
                     Timestamp start,
                     Timestamp end);

    // -------------------------------------------------------------------------
    // 事务
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<std::unique_ptr<Transaction>> begin_transaction();
    [[nodiscard]] Result<std::unique_ptr<Transaction>>
        begin_transaction(IsolationLevel level);

    // -------------------------------------------------------------------------
    // 原始查询（高级用法）
    // -------------------------------------------------------------------------
    // 参数化查询
    Result<std::vector<std::vector<std::string>>>
        execute(std::string_view sql,
                 std::span<const std::string> params = {});

    // -------------------------------------------------------------------------
    // 维护
    // -------------------------------------------------------------------------
    // 清理过期数据
    Result<std::size_t> cleanup_old_data(
        std::chrono::days older_than);

    // Vacuum（TimescaleDB）
    Result<void> vacuum();

    // 数据库大小
    [[nodiscard]] Result<std::uint64_t> database_size_bytes() const;

    // 表大小
    [[nodiscard]] Result<std::uint64_t>
        table_size_bytes(std::string_view table_name) const;

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    [[nodiscard]] const StorageStats& stats() const noexcept;
    void reset_stats() noexcept;

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    [[nodiscard]] const StorageConfig& config() const noexcept;
    void set_config(const StorageConfig& config);

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

    // -------------------------------------------------------------------------
    // 工具函数
    // -------------------------------------------------------------------------
    // 周期字符串 → 表名后缀
    [[nodiscard]] static std::string
        interval_to_table_suffix(std::string_view interval);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 错误映射
// ==============================================================================
[[nodiscard]] ErrorCode from_pg_error(std::string_view sqlstate) noexcept;

// ==============================================================================
// 工具函数
// ==============================================================================
[[nodiscard]] std::string
format_timestamp_pg(Timestamp ts);

[[nodiscard]] std::optional<Timestamp>
parse_timestamp_pg(std::string_view str);

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_STORAGE_HPP
