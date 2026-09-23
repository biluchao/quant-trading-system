// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 账户级风险监控与熔断
// ==============================================================================
// @file    src/strategy/strategy.core.risk_monitor.hpp
// @module  strategy
// @type    core
// @name    risk_monitor
// @version 1.0.1
// @brief   账户级实时风险监控，4 级熔断，渐进降级/升级，自动恢复
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 4 级风险状态机：NORMAL → CAUTION → RESTRICTED → HALTED
//   - 渐进升级：风险累积时逐级升级
//   - 渐进恢复：风险缓解时逐级恢复
//   - 冷却期：状态切换最小间隔
//   - 峰值跟踪：内部维护 peak_equity
//   - 日重置：UTC 0 点重置日内指标
//   - 多维度检查：回撤/日损/杠杆/持仓数/单笔/方向/相关性
//   - 紧急平仓：HALTED 时触发
//   - 完整追踪：历史 + 统计 + 快照
//   - 参数可配：所有阈值从 ConfigManager 注入
//
// 风险状态:
//   NORMAL     正常      所有操作允许
//   CAUTION    谨慎      降低新开仓，收紧止损
//   RESTRICTED 受限      仅允许减仓，禁止加仓
//   HALTED     熔断      仅允许平仓，触发紧急平仓
//
// 升级条件（任一）:
//   日损 > max_daily_loss_pct       → CAUTION
//   回撤 > max_drawdown_pct         → RESTRICTED
//   日损 > 2 × max_daily_loss_pct   → HALTED
//   回撤 > 2 × max_drawdown_pct     → HALTED
//   杠杆 > max_leverage             → RESTRICTED
//   连续亏损 ≥ threshold            → CAUTION
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_RISK_MONITOR_HPP
#define QUANT_STRATEGY_CORE_RISK_MONITOR_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
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
namespace risk_monitor_config {

// 默认阈值（权益占比）
inline constexpr double kDefaultMaxDailyLossPct    = 0.05;   // 5%
inline constexpr double kDefaultMaxDrawdownPct     = 0.15;   // 15%
inline constexpr double kDefaultCautionRiskPct     = 0.02;   // 2%
inline constexpr double kDefaultRestrictedRiskPct  = 0.03;   // 3%

// 杠杆
inline constexpr std::size_t kDefaultMaxLeverage   = 5;
inline constexpr std::size_t kDefaultMaxPositions  = 5;
inline constexpr double kDefaultMaxSingleRiskPct   = 0.02;

// 连续亏损
inline constexpr int kDefaultConsecutiveLossWarn   = 3;
inline constexpr int kDefaultConsecutiveLossCritical = 5;

// 状态切换冷却（秒）
inline constexpr std::int64_t kDefaultMinStateDurationSec = 60;

// HALTED 冷却（秒）
inline constexpr std::int64_t kDefaultHaltedCooldownSec = 3600;  // 1 小时

// 历史窗口
inline constexpr std::size_t kMaxHistorySize = 128;

// 浮点保护
inline constexpr double kEpsilon = 1e-9;

}  // namespace risk_monitor_config

// ==============================================================================
// 风险状态枚举
// ==============================================================================
enum class RiskState : std::uint8_t {
    Normal     = 0,   // 正常
    Caution    = 1,   // 谨慎
    Restricted = 2,   // 受限
    Halted     = 3,   // 熔断
};

[[nodiscard]] constexpr std::string_view to_string(RiskState s) noexcept {
    switch (s) {
        case RiskState::Normal:     return "Normal";
        case RiskState::Caution:    return "Caution";
        case RiskState::Restricted: return "Restricted";
        case RiskState::Halted:     return "Halted";
    }
    return "Unknown";
}

// 风险等级数值
[[nodiscard]] constexpr std::uint8_t risk_level(RiskState s) noexcept {
    return static_cast<std::uint8_t>(s);
}

// 状态对应的允许动作
[[nodiscard]] constexpr bool allows_open_position(RiskState s) noexcept {
    return s == RiskState::Normal;
}

[[nodiscard]] constexpr bool allows_add_position(RiskState s) noexcept {
    return s == RiskState::Normal;
}

[[nodiscard]] constexpr bool allows_reduce_position(RiskState s) noexcept {
    return s != RiskState::Halted;
}

[[nodiscard]] constexpr bool allows_close_position(RiskState s) noexcept {
    return true;  // 任何状态都允许平仓
}

// 状态对应的仓位缩放
[[nodiscard]] constexpr double position_scale(RiskState s) noexcept {
    switch (s) {
        case RiskState::Normal:     return 1.0;
        case RiskState::Caution:    return 0.5;
        case RiskState::Restricted: return 0.0;
        case RiskState::Halted:     return 0.0;
    }
    return 1.0;
}

// ==============================================================================
// 风险监控参数
// ==============================================================================
struct RiskMonitorParams {
    // 日损/回撤
    double max_daily_loss_pct{
        risk_monitor_config::kDefaultMaxDailyLossPct};
    double max_drawdown_pct{
        risk_monitor_config::kDefaultMaxDrawdownPct};

    // 分级阈值
    double caution_risk_pct{
        risk_monitor_config::kDefaultCautionRiskPct};
    double restricted_risk_pct{
        risk_monitor_config::kDefaultRestrictedRiskPct};

    // 杠杆
    std::size_t max_leverage{
        risk_monitor_config::kDefaultMaxLeverage};
    std::size_t max_positions{
        risk_monitor_config::kDefaultMaxPositions};
    double max_single_risk_pct{
        risk_monitor_config::kDefaultMaxSingleRiskPct};

    // 连续亏损
    int consecutive_loss_warn{
        risk_monitor_config::kDefaultConsecutiveLossWarn};
    int consecutive_loss_critical{
        risk_monitor_config::kDefaultConsecutiveLossCritical};

    // 冷却
    std::int64_t min_state_duration_sec{
        risk_monitor_config::kDefaultMinStateDurationSec};
    std::int64_t halted_cooldown_sec{
        risk_monitor_config::kDefaultHaltedCooldownSec};

    // 启用各项检查
    bool enable_daily_loss_check{true};
    bool enable_drawdown_check{true};
    bool enable_leverage_check{true};
    bool enable_position_count_check{true};
    bool enable_single_position_check{true};
    bool enable_consecutive_loss_check{true};
    bool enable_direction_bias_check{true};

    // 方向偏置系数
    double same_direction_limit{1.5};

    [[nodiscard]] Result<void> validate() const noexcept;

    [[nodiscard]] static RiskMonitorParams defaults() noexcept {
        return RiskMonitorParams{};
    }
};

// ==============================================================================
// 账户状态（供风险监控）
// ==============================================================================
struct AccountSnapshot {
    // 权益
    double equity{0.0};
    double peak_equity{0.0};
    double daily_start_equity{0.0};

    // 日盈亏
    double daily_pnl{0.0};

    // 风险
    double current_risk_amount{0.0};      // 当前总风险（USDT）
    double max_single_risk_amount{0.0};   // 单个持仓最大风险

    // 持仓
    std::size_t open_position_count{0};
    std::size_t long_position_count{0};
    std::size_t short_position_count{0};
    double long_risk_amount{0.0};
    double short_risk_amount{0.0};

    // 杠杆
    std::size_t current_leverage{1};

    // 连续亏损
    int consecutive_losses{0};

    [[nodiscard]] bool is_valid() const noexcept {
        return std::isfinite(equity) && equity >= 0.0 &&
               std::isfinite(peak_equity) && peak_equity >= 0.0;
    }
};

// ==============================================================================
// 风险状态快照（供下游消费）
// ==============================================================================
struct RiskStatus {
    // 当前状态
    RiskState state{RiskState::Normal};

    // 关键指标
    double equity{0.0};
    double peak_equity{0.0};
    double daily_loss_pct{0.0};
    double drawdown_pct{0.0};
    double current_risk_pct{0.0};
    std::size_t leverage{1};
    std::size_t position_count{0};
    int consecutive_losses{0};

    // 建议动作
    bool allow_open{true};
    bool allow_add{true};
    bool allow_reduce{true};
    bool allow_close{true};
    double position_scale{1.0};

    // 熔断
    bool circuit_breaker_active{false};
    bool emergency_liquidate_required{false};

    // 原因
    std::vector<std::string> violations;

    // 时间
    Timestamp evaluated_at{};
    Timestamp state_since{};

    [[nodiscard]] bool is_healthy() const noexcept {
        return state == RiskState::Normal;
    }

    [[nodiscard]] bool is_halted() const noexcept {
        return state == RiskState::Halted;
    }

    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 风险状态转换记录
// ==============================================================================
struct RiskStateChange {
    Timestamp timestamp{};
    RiskState from{RiskState::Normal};
    RiskState to{RiskState::Normal};
    std::vector<std::string> reasons;
};

// ==============================================================================
// 风险快照（持久化）
// ==============================================================================
struct RiskMonitorSnapshot {
    std::uint32_t schema_version{1};
    Timestamp snapshot_time{};

    // 状态
    std::uint8_t state{0};
    std::int64_t state_since_us{0};

    // 账户跟踪
    double peak_equity{0.0};
    double daily_start_equity{0.0};
    std::int64_t current_day_start_us{0};

    // 计数
    int consecutive_losses{0};
    std::uint64_t state_change_count{0};
    std::uint64_t halt_count{0};

    // 历史
    std::vector<RiskStateChange> history;

    [[nodiscard]] bool is_valid() const noexcept {
        return schema_version > 0 && schema_version <= 1000;
    }
};

// ==============================================================================
// 风险监控统计
// ==============================================================================
struct RiskMonitorStats {
    std::atomic<std::uint64_t> evaluate_count{0};
    std::atomic<std::uint64_t> state_change_count{0};
    std::atomic<std::uint64_t> caution_count{0};
    std::atomic<std::uint64_t> restricted_count{0};
    std::atomic<std::uint64_t> halted_count{0};
    std::atomic<std::uint64_t> recovered_count{0};
    std::atomic<std::uint64_t> cooldown_blocked_count{0};
    std::atomic<std::uint64_t> day_rollover_count{0};
    std::atomic<std::uint64_t> emergency_liquidate_count{0};
    std::atomic<std::uint64_t> rejected_open_count{0};
    std::atomic<std::uint64_t> rejected_add_count{0};

    void reset() noexcept {
        evaluate_count.store(0, std::memory_order_relaxed);
        state_change_count.store(0, std::memory_order_relaxed);
        caution_count.store(0, std::memory_order_relaxed);
        restricted_count.store(0, std::memory_order_relaxed);
        halted_count.store(0, std::memory_order_relaxed);
        recovered_count.store(0, std::memory_order_relaxed);
        cooldown_blocked_count.store(0, std::memory_order_relaxed);
        day_rollover_count.store(0, std::memory_order_relaxed);
        emergency_liquidate_count.store(0, std::memory_order_relaxed);
        rejected_open_count.store(0, std::memory_order_relaxed);
        rejected_add_count.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// 风险监控器
// ==============================================================================
class RiskMonitor {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 参数提供者（必需）
        std::function<RiskMonitorParams()> params_provider;

        // 状态变更回调（可选）
        std::function<void(const RiskStateChange&)> on_state_change;

        // 熔断触发回调（可选）
        std::function<void(const RiskStatus&)> on_halt;

        // 恢复回调（可选）
        std::function<void(const RiskStatus&)> on_recover;

        // 紧急平仓回调（可选）
        std::function<void(const RiskStatus&)> on_emergency_liquidate;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit RiskMonitor(Dependencies deps);
    ~RiskMonitor();

    RiskMonitor(const RiskMonitor&) = delete;
    RiskMonitor& operator=(const RiskMonitor&) = delete;
    RiskMonitor(RiskMonitor&&) noexcept;
    RiskMonitor& operator=(RiskMonitor&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 核心评估
    // -------------------------------------------------------------------------
    // 评估账户状态，返回风险状态
    // 幂等性：同一时间戳重复评估返回缓存结果
    [[nodiscard]] Result<RiskStatus> evaluate(
        const AccountSnapshot& account);

    // -------------------------------------------------------------------------
    // 快速检查
    // -------------------------------------------------------------------------
    // 检查是否允许开仓
    [[nodiscard]] Result<bool> can_open_position(
        const AccountSnapshot& account) const;

    // 检查是否允许加仓
    [[nodiscard]] Result<bool> can_add_position(
        const AccountSnapshot& account) const;

    // 检查是否允许平仓
    [[nodiscard]] Result<bool> can_close_position(
        const AccountSnapshot& account) const;

    // -------------------------------------------------------------------------
    // 事件通知
    // -------------------------------------------------------------------------
    // 交易结束通知（用于更新连续亏损计数）
    void on_trade_closed(double pnl) noexcept;

    // 手动触发紧急平仓
    void trigger_emergency_liquidate(std::string_view reason) noexcept;

    // 日切换（UTC 0 点调用）
    void on_day_rollover(double new_equity) noexcept;

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] RiskState current_state() const noexcept;
    [[nodiscard]] RiskStatus current_status() const;
    [[nodiscard]] std::optional<Timestamp> state_since() const noexcept;
    [[nodiscard]] std::vector<RiskStateChange> history() const;

    // -------------------------------------------------------------------------
    // 手动控制
    // -------------------------------------------------------------------------
    // 手动重置熔断（谨慎使用）
    Result<void> clear_halt(std::string_view reason);

    // 清空历史
    void clear_history() noexcept;

    // -------------------------------------------------------------------------
    // 持久化
    // -------------------------------------------------------------------------
    [[nodiscard]] RiskMonitorSnapshot to_snapshot() const;
    Result<void> from_snapshot(const RiskMonitorSnapshot& snapshot);

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const RiskMonitorStats& stats() const noexcept;
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

// 计算回撤百分比
[[nodiscard]] inline double compute_drawdown_pct(
    double equity, double peak_equity) noexcept
{
    if (peak_equity < risk_monitor_config::kEpsilon) return 0.0;
    const double dd = (peak_equity - equity) / peak_equity;
    if (!std::isfinite(dd)) return 0.0;
    return std::max(0.0, dd);
}

// 计算日亏损百分比
[[nodiscard]] inline double compute_daily_loss_pct(
    double daily_start_equity, double current_equity) noexcept
{
    if (daily_start_equity < risk_monitor_config::kEpsilon) return 0.0;
    const double loss = (daily_start_equity - current_equity) /
                         daily_start_equity;
    if (!std::isfinite(loss)) return 0.0;
    return std::max(0.0, loss);
}

// 风险状态升级
[[nodiscard]] inline RiskState escalate(RiskState current,
                                          RiskState target) noexcept {
    return (risk_level(target) > risk_level(current)) ? target : current;
}

// 风险状态降级
[[nodiscard]] inline RiskState deescalate(RiskState current,
                                            RiskState target) noexcept {
    return (risk_level(target) < risk_level(current)) ? target : current;
}

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_RISK_MONITOR_HPP
