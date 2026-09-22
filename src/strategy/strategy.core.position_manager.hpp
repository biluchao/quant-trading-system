// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 持仓管理
// ==============================================================================
// @file    src/strategy/strategy.core.position_manager.hpp
// @module  strategy
// @type    core
// @name    position_manager
// @version 1.0.1
// @brief   持仓生命周期管理：移动止损、加仓、分批止盈、时间止损、结构破坏
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 只产生动作：不直接下单，由 OMS 执行
//   - 方向对称：多空逻辑严格对称
//   - 只上移/下移：移动止损严格单向
//   - 保本优先：1R 后止损至少保本
//   - 阶梯止盈：2R/3R 分批减仓，剩余移动止损
//   - 加仓同步：加仓后重挂止损覆盖全部持仓
//   - 风控内置：加仓前校验总风险 ≤ 2%
//   - 时间止损：长期不盈利主动离场
//   - 结构破坏：跌破 EMA + 斜率转负 = 离场
//   - 假穿容忍：3 根确认 + 距离阈值
//
// 移动止损阶梯:
//   浮盈 1R  → 止损移至保本
//   浮盈 2R  → 止损移至 1R
//   浮盈 3R  → 止损移至 2R
//   浮盈 4R  → 止损移至 3R
//   浮盈 5R+ → 止损移至 4R
//
// 分批止盈:
//   2R → 平仓 30%
//   3R → 平仓 30%
//   剩余 40% → 移动止损跟跑
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_POSITION_MANAGER_HPP
#define QUANT_STRATEGY_CORE_POSITION_MANAGER_HPP

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
#include "indicator/indicator.core.calculator.hpp"
#include "indicator/indicator.core.band.hpp"

namespace quant::strategy {

// ==============================================================================
// 常量
// ==============================================================================
namespace position_config {

// 止损距离
inline constexpr double kInitialStopAtr       = 1.5;   // 初始止损 1.5×ATR
inline constexpr double kMinStopAtr           = 0.5;   // 最小止损 0.5×ATR
inline constexpr double kSwingBufferAtr       = 0.2;   // 摆动点外侧缓冲
inline constexpr double kTrailingBufferAtr    = 0.5;   // 移动止损缓冲
inline constexpr double kStrongTrendBufferAtr = 0.8;   // 强趋势放宽

// 保本触发
inline constexpr double kBreakevenR           = 1.0;   // 1R 后移至保本

// 阶梯移动止损（R 倍数 → 止损位置）
inline constexpr std::size_t kRStopLevels     = 6;
inline constexpr std::array<double, kRStopLevels> kRStopTrigger = {
    1.0, 2.0, 3.0, 4.0, 5.0, 6.0
};
inline constexpr std::array<double, kRStopLevels> kRStopPosition = {
    0.0, 1.0, 2.0, 3.0, 4.0, 5.0
};

// 分批止盈
inline constexpr std::size_t kTakeProfitStages = 3;
inline constexpr std::array<double, kTakeProfitStages> kTpTriggersR = {
    2.0, 3.0, 4.0
};
inline constexpr std::array<double, kTakeProfitStages> kTpFractions = {
    0.30, 0.30, 0.30
};
// 剩余 10% 移动止损跟跑

// 时间止损
inline constexpr int kTimeStopBars            = 6;     // 6 根 K 线
inline constexpr double kTimeStopMinR         = 0.5;   // 未达 0.5R 则平仓
inline constexpr int kStrongTrendTimeStopBars = 20;    // 强趋势放宽

// 结构破坏
inline constexpr int kStructureBreakConfirmBars = 3;   // 连续 3 根确认
inline constexpr double kStructureBreakMinAtr   = 0.5; // 距离阈值

// 加仓
inline constexpr std::size_t kMaxAddCount     = 3;     // 最多加仓 3 次
inline constexpr double kFirstAddTriggerR     = 1.5;   // 首次加仓 1.5R
inline constexpr double kSecondAddTriggerR    = 3.0;   // 二次加仓 3.0R
inline constexpr double kAddTrendMinStrength  = 0.6;   // 加仓趋势强度阈值
inline constexpr int kAddCooldownBars         = 5;     // 加仓间隔 K 线

// 假穿容忍
inline constexpr int kFalseBreakoutConfirmBars = 3;    // 3 根确认
inline constexpr double kFalseBreakoutMinAtr   = 0.3;  // 距离阈值

// 总风险预算
inline constexpr double kTotalRiskBudget      = 0.02;  // 2%

// 浮点比较 epsilon
inline constexpr double kPriceEpsilon         = 1e-9;

// 历史窗口
inline constexpr std::size_t kMaxSwingPoints  = 32;

}  // namespace position_config

// ==============================================================================
// 持仓动作类型
// ==============================================================================
enum class PositionActionType : std::uint8_t {
    None          = 0,   // 无动作
    Hold          = 1,   // 保持
    MoveStopLoss  = 2,   // 移动止损
    AddPosition   = 3,   // 加仓
    ReducePosition= 4,   // 减仓（止盈）
    ClosePosition = 5,   // 全部平仓
};

[[nodiscard]] constexpr std::string_view to_string(PositionActionType t) noexcept {
    switch (t) {
        case PositionActionType::None:           return "None";
        case PositionActionType::Hold:           return "Hold";
        case PositionActionType::MoveStopLoss:   return "MoveStopLoss";
        case PositionActionType::AddPosition:    return "AddPosition";
        case PositionActionType::ReducePosition: return "ReducePosition";
        case PositionActionType::ClosePosition:  return "ClosePosition";
    }
    return "Unknown";
}

// ==============================================================================
// 持仓动作
// ==============================================================================
struct PositionAction {
    PositionActionType type{PositionActionType::None};

    // 移动止损时：新的止损价
    Price new_stop_loss{};

    // 加仓/减仓时：数量
    Quantity quantity{};

    // 加仓/减仓时：触发价格
    Price trigger_price{};

    // 原因（供溯源）
    std::string reason;

    // 触发时机
    Timestamp execute_at{};

    [[nodiscard]] bool has_action() const noexcept {
        return type != PositionActionType::None &&
               type != PositionActionType::Hold;
    }
};

// ==============================================================================
// 持仓快照（供前端/持久化）
// ==============================================================================
struct PositionSnapshot {
    bool has_position{false};
    Side side{Side::UNKNOWN};
    Symbol symbol;

    Price entry_price{};
    Price current_price{};
    Price stop_loss{};
    Price take_profit_1{};
    Price take_profit_2{};

    Quantity quantity{};
    Quantity initial_quantity{};

    double stop_distance{0.0};       // 初始止损距离
    double unrealized_pnl{0.0};      // 未实现盈亏
    double unrealized_r{0.0};        // 浮盈 R 倍数

    int bars_held{0};
    int add_count{0};
    int tp_stage{0};                 // 已完成的止盈阶段
    bool breakeven_set{false};

    std::string to_string() const;
};

// ==============================================================================
// 持仓统计
// ==============================================================================
struct PositionStats {
    std::atomic<std::uint64_t> position_opened{0};
    std::atomic<std::uint64_t> stop_loss_moved{0};
    std::atomic<std::uint64_t> position_added{0};
    std::atomic<std::uint64_t> position_reduced{0};
    std::atomic<std::uint64_t> position_closed{0};
    std::atomic<std::uint64_t> time_stop_triggered{0};
    std::atomic<std::uint64_t> structure_break_triggered{0};
    std::atomic<std::uint64_t> take_profit_triggered{0};
    std::atomic<std::uint64_t> breakeven_triggered{0};
    std::atomic<std::uint64_t> false_breakout_detected{0};
    std::atomic<std::uint64_t> risk_check_failed{0};

    void reset() noexcept {
        position_opened.store(0);
        stop_loss_moved.store(0);
        position_added.store(0);
        position_reduced.store(0);
        position_closed.store(0);
        time_stop_triggered.store(0);
        structure_break_triggered.store(0);
        take_profit_triggered.store(0);
        breakeven_triggered.store(0);
        false_breakout_detected.store(0);
        risk_check_failed.store(0);
    }
};

// ==============================================================================
// 持仓参数（从 ConfigManager 加载）
// ==============================================================================
struct PositionParams {
    // 止损
    double initial_stop_atr{kInitialStopAtr};
    double min_stop_atr{kMinStopAtr};
    double swing_buffer_atr{kSwingBufferAtr};
    double trailing_buffer_atr{kTrailingBufferAtr};
    double strong_trend_buffer_atr{kStrongTrendBufferAtr};

    // 保本
    double breakeven_r{kBreakevenR};

    // 时间止损
    int time_stop_bars{kTimeStopBars};
    double time_stop_min_r{kTimeStopMinR};

    // 结构破坏
    int structure_break_confirm_bars{kStructureBreakConfirmBars};
    double structure_break_min_atr{kStructureBreakMinAtr};

    // 加仓
    std::size_t max_add_count{kMaxAddCount};
    double first_add_trigger_r{kFirstAddTriggerR};
    double second_add_trigger_r{kSecondAddTriggerR};
    double add_trend_min_strength{kAddTrendMinStrength};
    int add_cooldown_bars{kAddCooldownBars};
    std::array<double, 3> add_scales{1.0, 0.8, 0.4};  // 首仓/一次加/二次加

    // 总风险预算
    double total_risk_budget{kTotalRiskBudget};

    // 止盈
    double tp1_trigger_r{2.0};
    double tp2_trigger_r{3.0};
    double tp1_fraction{0.30};
    double tp2_fraction{0.30};
};

// ==============================================================================
// 持仓管理器
// ==============================================================================
class PositionManager {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 参数提供者（必需）
        std::function<PositionParams()> params_provider;

        // 权益提供者（必需，用于风险计算）
        std::function<double()> equity_provider;

        // 状态变更回调（可选，用于持久化）
        std::function<void(std::string_view event,
                            const PositionSnapshot&)> on_state_change;

        // 动作产生回调（可选，用于溯源）
        std::function<void(const PositionAction&,
                            const PositionSnapshot&)> on_action;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit PositionManager(Dependencies deps);
    ~PositionManager();

    PositionManager(const PositionManager&) = delete;
    PositionManager& operator=(const PositionManager&) = delete;
    PositionManager(PositionManager&&) noexcept;
    PositionManager& operator=(PositionManager&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 持仓事件
    // -------------------------------------------------------------------------
    // 持仓开仓（由 engine 或 OMS 触发）
    Result<void> on_position_opened(const Position& pos,
                                      const Signal& signal);

    // 持仓更新（部分成交、均价变化）
    void on_position_updated(const Position& pos) noexcept;

    // 持仓平仓（由 engine 或 OMS 触发）
    void on_position_closed(std::string_view reason) noexcept;

    // 部分成交
    void on_partial_fill(const Fill& fill) noexcept;

    // -------------------------------------------------------------------------
    // 每根 K 线处理（热路径）
    // -------------------------------------------------------------------------
    // 返回持仓动作
    [[nodiscard]] Result<PositionAction> on_candle_close(
        const Candle& candle,
        const IndicatorResult& ind,
        const BandResult& band,
        const struct MTFContext* mtf,
        double trend_strength);

    // -------------------------------------------------------------------------
    // 加仓请求（由外部触发，如趋势强度足够）
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<PositionAction> request_add_position(
        const Candle& candle,
        const IndicatorResult& ind,
        double trend_strength);

    // -------------------------------------------------------------------------
    // 减仓请求（风控触发）
    [[nodiscard]] Result<PositionAction> request_reduce_position(
        double fraction,
        std::string_view reason);

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] PositionSnapshot snapshot() const;
    [[nodiscard]] bool has_position() const noexcept;
    [[nodiscard]] Side current_side() const noexcept;
    [[nodiscard]] double unrealized_r() const noexcept;
    [[nodiscard]] int bars_held() const noexcept;
    [[nodiscard]] int add_count() const noexcept;

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const PositionStats& stats() const noexcept;
    [[nodiscard]] std::string dump() const;

private:
    // =========================================================================
    // 内部状态
    // =========================================================================
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 辅助函数
// ==============================================================================

// 计算浮盈 R 倍数
[[nodiscard]] inline double compute_r_multiple(
    const Position& pos,
    double current_price) noexcept
{
    if (pos.entry_price.raw() == 0) return 0.0;
    const double entry = pos.entry_price.to_double();
    const double stop_dist = std::abs(entry - pos.stop_loss.to_double());
    if (stop_dist < position_config::kPriceEpsilon) return 0.0;

    const double pnl = (pos.side == Side::BUY)
        ? (current_price - entry)
        : (entry - current_price);
    return pnl / stop_dist;
}

// 计算止盈价
[[nodiscard]] inline Price compute_take_profit(
    const Position& pos,
    double r_multiple) noexcept
{
    const double entry = pos.entry_price.to_double();
    const double stop_dist = std::abs(entry - pos.stop_loss.to_double());
    const double tp = (pos.side == Side::BUY)
        ? entry + r_multiple * stop_dist
        : entry - r_multiple * stop_dist;
    return Price::from_double(tp);
}

// ==============================================================================
// 便捷：动作类型判断
// ==============================================================================
[[nodiscard]] inline bool is_close_action(const PositionAction& a) noexcept {
    return a.type == PositionActionType::ClosePosition;
}

[[nodiscard]] inline bool is_add_action(const PositionAction& a) noexcept {
    return a.type == PositionActionType::AddPosition;
}

[[nodiscard]] inline bool is_reduce_action(const PositionAction& a) noexcept {
    return a.type == PositionActionType::ReducePosition;
}

[[nodiscard]] inline bool is_move_stop_action(const PositionAction& a) noexcept {
    return a.type == PositionActionType::MoveStopLoss;
}

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_POSITION_MANAGER_HPP
