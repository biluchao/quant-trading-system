// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 冷却期管理
// ==============================================================================
// @file    src/strategy/strategy.core.cooldown.hpp
// @module  strategy
// @type    core
// @name    cooldown
// @version 1.0.1
// @brief   多类型冷却期管理，按 K 线计数，支持持久化
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 多类型独立：5 类冷却互不干扰，叠加时取最大截止
//   - K 线计数：避免时钟漂移影响
//   - 时间戳保护：与 K 线计数双重判定
//   - 单调时间：Timestamp::now() 不受 NTP 回拨影响
//   - 类型安全：CooldownType 枚举
//   - 完整追踪：历史 + 统计 + 诊断
//   - 持久化：to_snapshot / from_snapshot 支持恢复
//   - 回调通知：冷却结束时通知下游
//
// 冷却类型:
//   PositionClosed   平仓后冷却（默认 3 根 K 线）
//   StopLoss         止损后冷却（默认 5 根 K 线）
//   ReverseClosed    反向单平仓后冷却（默认 8 根 K 线）
//   ConsecutiveLoss  连续亏损冷却（默认 10 根 K 线）
//   TrendSwitch      趋势切换冷却（默认 10 根 K 线）
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_COOLDOWN_HPP
#define QUANT_STRATEGY_CORE_COOLDOWN_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
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
// 常量
// ==============================================================================
namespace cooldown_config {

// K 线间隔（3 分钟，微秒）
inline constexpr std::int64_t kCandleIntervalUs =
    3LL * 60LL * 1000LL * 1000LL;

// 各类型默认冷却 K 线数
inline constexpr int kDefaultPositionClosedBars  = 3;
inline constexpr int kDefaultStopLossBars        = 5;
inline constexpr int kDefaultReverseClosedBars   = 8;
inline constexpr int kDefaultConsecutiveLossBars = 10;
inline constexpr int kDefaultTrendSwitchBars     = 10;

// 冷却最大时长（防止误配永久锁死）
inline constexpr int kMaxCooldownBars = 200;

// 冷却历史保留条数
inline constexpr std::size_t kMaxHistorySize = 64;

// 连续亏损触发升级冷却的阈值
inline constexpr int kConsecutiveLossThreshold = 3;

}  // namespace cooldown_config

// ==============================================================================
// 冷却类型枚举
// ==============================================================================
enum class CooldownType : std::uint8_t {
    None             = 0,
    PositionClosed   = 1,   // 平仓后
    StopLoss         = 2,   // 止损后
    ReverseClosed    = 3,   // 反向单平仓后
    ConsecutiveLoss  = 4,   // 连续亏损
    TrendSwitch      = 5,   // 趋势切换
};

[[nodiscard]] constexpr std::string_view to_string(CooldownType t) noexcept {
    switch (t) {
        case CooldownType::None:            return "None";
        case CooldownType::PositionClosed:  return "PositionClosed";
        case CooldownType::StopLoss:        return "StopLoss";
        case CooldownType::ReverseClosed:   return "ReverseClosed";
        case CooldownType::ConsecutiveLoss: return "ConsecutiveLoss";
        case CooldownType::TrendSwitch:     return "TrendSwitch";
    }
    return "Unknown";
}

// 编译期校验
inline constexpr std::size_t kCooldownTypeCount = 6;
static_assert(static_cast<std::size_t>(CooldownType::TrendSwitch) + 1 ==
              kCooldownTypeCount,
              "CooldownType 数量与 kCooldownTypeCount 不一致");

// ==============================================================================
// 冷却参数（从 ConfigManager 加载）
// ==============================================================================
struct CooldownParams {
    int position_closed_bars{cooldown_config::kDefaultPositionClosedBars};
    int stop_loss_bars{cooldown_config::kDefaultStopLossBars};
    int reverse_closed_bars{cooldown_config::kDefaultReverseClosedBars};
    int consecutive_loss_bars{cooldown_config::kDefaultConsecutiveLossBars};
    int trend_switch_bars{cooldown_config::kDefaultTrendSwitchBars};

    // 连续亏损触发阈值
    int consecutive_loss_threshold{
        cooldown_config::kConsecutiveLossThreshold};

    // 是否启用各类冷却
    bool enable_position_closed{true};
    bool enable_stop_loss{true};
    bool enable_reverse_closed{true};
    bool enable_consecutive_loss{true};
    bool enable_trend_switch{true};

    [[nodiscard]] Result<void> validate() const noexcept;

    // 根据类型获取 K 线数
    [[nodiscard]] int bars_for(CooldownType type) const noexcept;

    // 是否启用某类型
    [[nodiscard]] bool is_enabled(CooldownType type) const noexcept;
};

// ==============================================================================
// 单个冷却记录
// ==============================================================================
struct CooldownEntry {
    CooldownType type{CooldownType::None};
    Timestamp started_at{};         // 开始时间
    Timestamp ends_at{};            // 结束时间（墙钟）
    int total_bars{0};              // 冷却总 K 线数
    int remaining_bars{0};          // 剩余 K 线数
    std::string reason;             // 原因描述
    Side side{Side::UNKNOWN};       // 触发方向（可选）

    [[nodiscard]] bool is_active() const noexcept {
        return type != CooldownType::None && remaining_bars > 0;
    }
};

// ==============================================================================
// 冷却统计
// ==============================================================================
struct CooldownStats {
    std::atomic<std::uint64_t> total_cooldowns{0};
    std::atomic<std::uint64_t> position_closed_count{0};
    std::atomic<std::uint64_t> stop_loss_count{0};
    std::atomic<std::uint64_t> reverse_closed_count{0};
    std::atomic<std::uint64_t> consecutive_loss_count{0};
    std::atomic<std::uint64_t> trend_switch_count{0};
    std::atomic<std::uint64_t> total_cooldown_bars{0};
    std::atomic<std::uint64_t> blocked_signal_count{0};
    std::atomic<std::uint64_t> early_clear_count{0};

    void reset() noexcept {
        total_cooldowns.store(0, std::memory_order_relaxed);
        position_closed_count.store(0, std::memory_order_relaxed);
        stop_loss_count.store(0, std::memory_order_relaxed);
        reverse_closed_count.store(0, std::memory_order_relaxed);
        consecutive_loss_count.store(0, std::memory_order_relaxed);
        trend_switch_count.store(0, std::memory_order_relaxed);
        total_cooldown_bars.store(0, std::memory_order_relaxed);
        blocked_signal_count.store(0, std::memory_order_relaxed);
        early_clear_count.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double avg_cooldown_bars() const noexcept {
        const auto total = total_cooldowns.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        const auto bars = total_cooldown_bars.load(std::memory_order_relaxed);
        return static_cast<double>(bars) / static_cast<double>(total);
    }
};

// ==============================================================================
// 冷却期快照（持久化用）
// ==============================================================================
struct CooldownSnapshot {
    std::uint32_t schema_version{1};
    Timestamp snapshot_time{};

    struct Entry {
        std::uint8_t type{0};
        std::int64_t started_at_us{0};
        std::int64_t ends_at_us{0};
        int total_bars{0};
        int remaining_bars{0};
        std::string reason;
        std::uint8_t side{0};
    };
    std::vector<Entry> active_cooldowns;

    int consecutive_losses{0};
    std::int64_t last_trigger_time_us{0};

    [[nodiscard]] bool is_valid() const noexcept;
};

// ==============================================================================
// 冷却管理器
// ==============================================================================
class CooldownManager {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 参数提供者（必需）
        std::function<CooldownParams()> params_provider;

        // 冷却结束回调（可选）
        std::function<void(CooldownType, Timestamp)> on_cooldown_end;

        // 冷却开始回调（可选）
        std::function<void(const CooldownEntry&)> on_cooldown_start;

        // 冷却状态变更回调（可选，用于持久化）
        std::function<void(const CooldownSnapshot&)> on_state_change;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit CooldownManager(Dependencies deps);
    ~CooldownManager();

    CooldownManager(const CooldownManager&) = delete;
    CooldownManager& operator=(const CooldownManager&) = delete;
    CooldownManager(CooldownManager&&) noexcept;
    CooldownManager& operator=(CooldownManager&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // K 线推进（核心：冷却按 K 线递减）
    // -------------------------------------------------------------------------
    // 每根 K 线收盘时调用
    // 幂等性：同一根 K 线重复输入被忽略
    Result<void> on_candle_close(const Candle& candle);

    // -------------------------------------------------------------------------
    // 冷却触发
    // -------------------------------------------------------------------------
    // 平仓后冷却
    void on_position_closed(Timestamp t,
                             Side side = Side::UNKNOWN,
                             std::string_view reason = "");

    // 止损后冷却
    void on_stop_loss(Timestamp t,
                       Side side = Side::UNKNOWN,
                       std::string_view reason = "");

    // 反向单平仓后冷却
    void on_reverse_closed(Timestamp t,
                            Side side = Side::UNKNOWN,
                            std::string_view reason = "");

    // 连续亏损（由外部调用，传入当前连续亏损次数）
    void on_consecutive_losses(Timestamp t, int count);

    // 趋势切换
    void on_trend_switch(Timestamp t,
                          Side side = Side::UNKNOWN,
                          std::string_view reason = "");

    // -------------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------------
    // 是否处于任意冷却期
    [[nodiscard]] bool is_cooling_down() const noexcept;

    // 指定类型是否冷却中
    [[nodiscard]] bool is_cooling_down(CooldownType type) const noexcept;

    // 冷却剩余 K 线数（所有类型取最大）
    [[nodiscard]] int remaining_bars() const noexcept;

    // 指定类型的剩余 K 线数
    [[nodiscard]] int remaining_bars(CooldownType type) const noexcept;

    // 剩余冷却时间（墙钟）
    [[nodiscard]] std::optional<Timestamp> cooldown_until() const noexcept;

    // 冷却原因（所有类型合并）
    [[nodiscard]] std::string cooldown_reason() const;

    // 所有活跃的冷却
    [[nodiscard]] std::vector<CooldownEntry> active_cooldowns() const;

    // 最大冷却截止时间
    [[nodiscard]] Timestamp max_cooldown_until() const noexcept;

    // -------------------------------------------------------------------------
    // 手动清除
    // -------------------------------------------------------------------------
    // 清除指定类型
    void clear(CooldownType type) noexcept;

    // 清除所有
    void clear_all() noexcept;

    // -------------------------------------------------------------------------
    // 连续亏损计数
    // -------------------------------------------------------------------------
    [[nodiscard]] int consecutive_losses() const noexcept;
    void reset_consecutive_losses() noexcept;

    // -------------------------------------------------------------------------
    // 历史
    // -------------------------------------------------------------------------
    [[nodiscard]] std::vector<CooldownEntry> history() const;

    // -------------------------------------------------------------------------
    // 持久化
    // -------------------------------------------------------------------------
    [[nodiscard]] CooldownSnapshot to_snapshot() const;
    Result<void> from_snapshot(const CooldownSnapshot& snapshot);

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const CooldownStats& stats() const noexcept;
    [[nodiscard]] std::string dump() const;

private:
    // =========================================================================
    // 内部实现
    // =========================================================================
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 辅助函数
// ==============================================================================

// 判断某类型是否需要触发冷却
[[nodiscard]] inline bool should_trigger_cooldown(
    CooldownType type,
    const CooldownParams& params) noexcept
{
    return params.is_enabled(type) && params.bars_for(type) > 0;
}

// 计算冷却结束时间（用于日志和 UI）
[[nodiscard]] inline Timestamp compute_cooldown_end(
    Timestamp start,
    int bars) noexcept
{
    if (bars <= 0) return start;
    const auto delta_us = static_cast<std::int64_t>(bars) *
                          cooldown_config::kCandleIntervalUs;
    return start.saturating_add(delta_us);
}

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_COOLDOWN_HPP
