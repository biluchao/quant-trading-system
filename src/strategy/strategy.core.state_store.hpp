// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 策略状态持久化
// ==============================================================================
// @file    src/strategy/strategy.core.state_store.hpp
// @module  strategy
// @type    core
// @name    state_store
// @version 1.0.1
// @brief   策略状态持久化与恢复，支持 Redis + 本地文件双写，与交易所对账
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 双写：Redis（快速）+ 本地文件（持久）
//   - 原子性：临时文件 + rename，Redis 事务
//   - 版本化：schema_version 字段，支持迁移
//   - 完整性：SHA-256 校验，防篡改
//   - 降级恢复：Redis → 本地文件 → 交易所对账
//   - 加密：敏感字段可选 AES-GCM
//   - TTL：自动过期，避免内存堆积
//   - 定时快照：崩溃时最多丢失 N 分钟
//   - 完整诊断：dump() + 统计计数器
//
// 恢复优先级:
//   1. Redis 快照（若未过期）
//   2. 本地文件快照（若未过期）
//   3. 交易所对账（重建状态）
//   4. 空状态（冷启动）
//
// 持久化内容:
//   - 状态机状态（S0~S6）
//   - 当前持仓（symbol/side/entry/qty/stop）
//   - 加仓次数
//   - 逃顶历史（价格序列）
//   - 冷却期截止
//   - 连续亏损次数
//   - 信号溯源 ID
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_STATE_STORE_HPP
#define QUANT_STRATEGY_CORE_STATE_STORE_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ==============================================================================
// 项目内部
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.timestamp.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 前置声明
// ==============================================================================
enum class StrategyState : std::uint8_t;

// ==============================================================================
// 常量
// ==============================================================================
namespace state_store_config {

// Schema 版本（数据格式变更时递增）
inline constexpr std::uint32_t kSchemaVersion = 1;

// 快照过期时间（秒）
inline constexpr std::int64_t kSnapshotTtlSeconds = 3600;   // 1 小时

// 自动快照间隔（秒）
inline constexpr std::int64_t kAutoSnapshotIntervalSec = 60;

// 本地文件大小限制（字节）
inline constexpr std::size_t kMaxLocalFileSize = 1 * 1024 * 1024;   // 1 MB

// Redis 键前缀
inline constexpr std::string_view kRedisKeyPrefix = "quant:strategy:state:";

// 本地文件默认路径
inline constexpr std::string_view kDefaultLocalPath =
    "data/snapshots/strategy_state.json";

// 校验和算法
inline constexpr std::string_view kChecksumAlgorithm = "sha256";

}  // namespace state_store_config

// ==============================================================================
// 策略状态快照（持久化对象）
// ==============================================================================
struct StrategyStateSnapshot {
    // Schema 版本
    std::uint32_t schema_version{state_store_config::kSchemaVersion};

    // 元信息
    std::string strategy_id{"trend_following"};
    std::string symbol;

    // 快照时间
    Timestamp snapshot_time{};
    Timestamp strategy_started_at{};

    // 状态机
    std::uint8_t state{0};              // S0~S6
    std::uint8_t direction{0};          // 0=UNKNOWN, 1=BUY, 2=SELL
    TraceId trace_id{};

    // 持仓
    bool has_position{false};
    Price entry_price{};
    Price stop_loss{};
    Price take_profit_1{};
    Price take_profit_2{};
    Quantity quantity{};
    Quantity initial_quantity{};
    double stop_distance{0.0};
    Timestamp position_opened_at{};

    // 计数
    int add_count{0};
    int tp_stage{0};
    int bars_held{0};
    bool breakeven_set{false};
    int consecutive_losses{0};

    // 逃顶历史
    std::vector<Price> escape_prices;
    int escape_count{0};

    // 冷却
    Timestamp cooldown_until{};

    // 校验和（存储时不包含自身）
    std::string checksum;

    // -------------------------------------------------------------------------
    // 校验
    // -------------------------------------------------------------------------
    [[nodiscard]] bool is_valid() const noexcept;

    // 是否已过期
    [[nodiscard]] bool is_stale(std::int64_t ttl_seconds) const noexcept;

    // 序列化（返回 JSON 字符串）
    [[nodiscard]] std::string to_json() const;

    // 反序列化
    [[nodiscard]] static Result<StrategyStateSnapshot> from_json(
        std::string_view json);

    // 诊断
    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 状态存储统计
// ==============================================================================
struct StateStoreStats {
    std::atomic<std::uint64_t> persist_count{0};
    std::atomic<std::uint64_t> persist_failed_count{0};
    std::atomic<std::uint64_t> restore_count{0};
    std::atomic<std::uint64_t> restore_failed_count{0};
    std::atomic<std::uint64_t> redis_write_count{0};
    std::atomic<std::uint64_t> redis_read_count{0};
    std::atomic<std::uint64_t> local_write_count{0};
    std::atomic<std::uint64_t> local_read_count{0};
    std::atomic<std::uint64_t> checksum_mismatch_count{0};
    std::atomic<std::uint64_t> stale_snapshot_count{0};
    std::atomic<std::uint64_t> reconcile_count{0};
    std::atomic<std::uint64_t> schema_migrate_count{0};

    void reset() noexcept {
        persist_count.store(0, std::memory_order_relaxed);
        persist_failed_count.store(0, std::memory_order_relaxed);
        restore_count.store(0, std::memory_order_relaxed);
        restore_failed_count.store(0, std::memory_order_relaxed);
        redis_write_count.store(0, std::memory_order_relaxed);
        redis_read_count.store(0, std::memory_order_relaxed);
        local_write_count.store(0, std::memory_order_relaxed);
        local_read_count.store(0, std::memory_order_relaxed);
        checksum_mismatch_count.store(0, std::memory_order_relaxed);
        stale_snapshot_count.store(0, std::memory_order_relaxed);
        reconcile_count.store(0, std::memory_order_relaxed);
        schema_migrate_count.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// 交易所状态（对账用）
// ==============================================================================
struct ExchangeState {
    bool has_position{false};
    Side side{Side::UNKNOWN};
    Symbol symbol;
    Price entry_price{};
    Price mark_price{};
    Quantity quantity{};
    Price stop_loss{};
    Timestamp updated_at{};

    [[nodiscard]] bool is_valid() const noexcept {
        return has_position ? (!symbol.empty() &&
                                quantity.is_valid() &&
                                entry_price.is_valid())
                             : true;
    }
};

// ==============================================================================
// 状态存储配置
// ==============================================================================
struct StateStoreConfig {
    // Redis
    bool redis_enabled{true};
    std::string redis_key_prefix{
        std::string{state_store_config::kRedisKeyPrefix}};
    std::int64_t redis_ttl_seconds{state_store_config::kSnapshotTtlSeconds};

    // 本地文件
    bool local_enabled{true};
    std::filesystem::path local_path{
        std::string{state_store_config::kDefaultLocalPath}};

    // 自动快照
    bool auto_snapshot_enabled{true};
    std::int64_t auto_snapshot_interval_sec{
        state_store_config::kAutoSnapshotIntervalSec};

    // 快照 TTL
    std::int64_t snapshot_ttl_seconds{
        state_store_config::kSnapshotTtlSeconds};

    // 加密（可选）
    bool encryption_enabled{false};
    std::array<std::uint8_t, 32> encryption_key{};

    // 压缩（可选）
    bool compression_enabled{false};

    // 校验和
    bool checksum_enabled{true};

    [[nodiscard]] Result<void> validate() const noexcept;
};

// ==============================================================================
// 状态存储接口
// ==============================================================================
class StateStore {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 配置提供者（必需）
        std::function<StateStoreConfig()> config_provider;

        // Redis 客户端接口（可选）
        struct RedisClient {
            std::function<bool(std::string_view key,
                               std::string_view value,
                               std::int64_t ttl_seconds)> set_ex;
            std::function<std::optional<std::string>(
                std::string_view key)> get;
            std::function<bool(std::string_view key)> del;
            std::function<bool(std::string_view key)> exists;
        };
        RedisClient redis;

        // 交易所状态获取（可选，用于对账）
        std::function<Result<ExchangeState>(const Symbol&)>
            exchange_state_provider;

        // 快照成功回调（可选）
        std::function<void(const StrategyStateSnapshot&)> on_snapshot;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit StateStore(Dependencies deps);
    ~StateStore();

    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;
    StateStore(StateStore&&) noexcept;
    StateStore& operator=(StateStore&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 持久化
    // -------------------------------------------------------------------------
    // 保存快照到 Redis + 本地文件
    Result<void> persist(const StrategyStateSnapshot& snapshot);

    // 手动触发自动快照
    Result<void> snapshot_now();

    // -------------------------------------------------------------------------
    // 恢复
    // -------------------------------------------------------------------------
    // 按优先级恢复：Redis → 本地文件 → 交易所对账 → 空
    Result<StrategyStateSnapshot> restore(const std::string& symbol);

    // 仅从 Redis 恢复
    Result<StrategyStateSnapshot> restore_from_redis(const std::string& symbol);

    // 仅从本地文件恢复
    Result<StrategyStateSnapshot> restore_from_local();

    // 从交易所对账重建
    Result<StrategyStateSnapshot> restore_from_exchange(const std::string& symbol);

    // -------------------------------------------------------------------------
    // 清除
    // -------------------------------------------------------------------------
    // 清除指定 symbol 的快照
    Result<void> clear(const std::string& symbol);

    // 清除所有快照
    Result<void> clear_all();

    // -------------------------------------------------------------------------
    // 存在性检查
    // -------------------------------------------------------------------------
    [[nodiscard]] bool has_snapshot(const std::string& symbol) const;

    // 快照时间查询（用于判断新鲜度）
    [[nodiscard]] std::optional<Timestamp> snapshot_time(
        const std::string& symbol) const;

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const StateStoreStats& stats() const noexcept;
    [[nodiscard]] std::string dump() const;

private:
    // =========================================================================
    // 内部实现
    // =========================================================================
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 辅助：从各模块状态构造快照
// ==============================================================================
// 便捷构造函数（避免每次手写）
[[nodiscard]] StrategyStateSnapshot make_snapshot(
    const std::string& strategy_id,
    const std::string& symbol,
    std::uint8_t state,
    Side direction,
    bool has_position,
    const Position* pos = nullptr);

// ==============================================================================
// 校验和计算（SHA-256）
// ==============================================================================
[[nodiscard]] std::string compute_checksum(std::string_view data);

// ==============================================================================
// Schema 迁移
// ==============================================================================
// 从旧版本迁移到当前版本
[[nodiscard]] Result<StrategyStateSnapshot> migrate_snapshot(
    const StrategyStateSnapshot& old_snapshot);

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_STATE_STORE_HPP
