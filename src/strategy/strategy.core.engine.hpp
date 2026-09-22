// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 策略引擎（状态机）
// ==============================================================================
// @file    src/strategy/strategy.core.engine.hpp
// @module  strategy
// @type    core
// @name    engine
// @version 1.0.1
// @brief   S0~S6 完整状态机，事件驱动，多策略兼容，可回放
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 状态机与持仓管理分离：引擎只产生决策，不直接改持仓
//   - 幂等性：同一根 K 线重复输入被忽略
//   - 无未来函数：决策基于已收盘 K 线，成交在下一根
//   - 冷却期强制：通过 CooldownManager 前置校验
//   - 依赖注入：所有子模块通过构造函数注入，便于测试
//   - 完整溯源：EngineContext 携带全部决策依据
//   - 状态历史：每次转移记录时间戳与原因
//   - 编译期状态数校验
//
// 状态定义:
//   S0  空仓观察    无持仓，同时监控多空双向候选
//   S1  顺势持仓    已按 MA 方向进场
//   S2  逃顶/逃底后 刚平仓，监控顶部/底部区间
//   S3  反向持仓    逃顶后做反向单
//   S4  趋势延续加仓 回踩确认后倒金字塔加仓
//   S5  趋势切换    连续确认穿越 MA，清空原方向
//   S6  不确定       价格贴均线、评分中性时观望
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_ENGINE_HPP
#define QUANT_STRATEGY_CORE_ENGINE_HPP

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
#include <unordered_map>
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
#include "indicator/indicator.core.scoring.hpp"
#include "indicator/indicator.core.support_resistance.hpp"

// ==============================================================================
// AI 决策（前置声明，避免循环依赖）
// ==============================================================================
namespace quant::ai {
    struct AIDecision;
}

namespace quant::strategy {

// ==============================================================================
// 常量
// ==============================================================================
namespace engine_config {

// 状态数量（编译期校验）
inline constexpr std::size_t kStateCount = 7;

// 状态历史最大保留数
inline constexpr std::size_t kMaxStateHistory = 256;

// 状态快照最大保留数
inline constexpr std::size_t kMaxSnapshots = 16;

// 最大同时持仓数
inline constexpr std::size_t kMaxConcurrentPositions = 2;

// 评分阈值
inline constexpr double kEntryScoreThreshold      = 60.0;
inline constexpr double kReverseScoreThreshold    = 80.0;
inline constexpr double kContinuationScoreThreshold = 55.0;
inline constexpr double kNeutralScoreRange        = 30.0;

// S6 进入/退出阈值
inline constexpr double kS6EnterBandRatio = 0.3;   // |价格-MA|/ATR < 0.3 进 S6
inline constexpr double kS6ExitBandRatio  = 0.5;   // |价格-MA|/ATR > 0.5 退 S6

}  // namespace engine_config

// ==============================================================================
// 状态枚举
// ==============================================================================
enum class StrategyState : std::uint8_t {
    S0_FlatObservation  = 0,   // 空仓观察
    S1_TrendPosition    = 1,   // 顺势持仓
    S2_EscapeObservation= 2,   // 逃顶/逃底后观察
    S3_ReversePosition  = 3,   // 反向持仓
    S4_ContinuationAdd  = 4,   // 趋势延续加仓
    S5_TrendSwitch      = 5,   // 趋势切换
    S6_Neutral          = 6,   // 不确定
};

[[nodiscard]] constexpr std::string_view to_string(StrategyState s) noexcept {
    switch (s) {
        case StrategyState::S0_FlatObservation:   return "S0_Flat";
        case StrategyState::S1_TrendPosition:     return "S1_Trend";
        case StrategyState::S2_EscapeObservation: return "S2_Escape";
        case StrategyState::S3_ReversePosition:   return "S3_Reverse";
        case StrategyState::S4_ContinuationAdd:   return "S4_Add";
        case StrategyState::S5_TrendSwitch:       return "S5_Switch";
        case StrategyState::S6_Neutral:           return "S6_Neutral";
    }
    return "UNKNOWN";
}

// 编译期状态数校验
static_assert(static_cast<std::size_t>(StrategyState::S6_Neutral) + 1 ==
              engine_config::kStateCount,
              "StrategyState 数量与 kStateCount 不一致，需同步更新");

// ==============================================================================
// 市场状态（与 AI 输出对齐）
// ==============================================================================
enum class MarketRegime : std::uint8_t {
    Unknown    = 0,
    TrendUp    = 1,
    TrendDown  = 2,
    Range      = 3,
    HighVol    = 4,
    LowVol     = 5,
};

// ==============================================================================
// 多时间框架上下文
// ==============================================================================
struct MTFContext {
    TrendDirection trend_5m{TrendDirection::Neutral};
    double dist_to_5m_resistance_atr{0.0};
    double dist_to_5m_support_atr{0.0};
    double trend_strength_5m{0.0};

    [[nodiscard]] bool has_conflict(Side dir_3m) const noexcept {
        if (dir_3m == Side::BUY && trend_5m == TrendDirection::Down) return true;
        if (dir_3m == Side::SELL && trend_5m == TrendDirection::Up) return true;
        if (dir_3m == Side::BUY && dist_to_5m_resistance_atr < 0.5) return true;
        if (dir_3m == Side::SELL && dist_to_5m_support_atr < 0.5) return true;
        return false;
    }
};

// ==============================================================================
// 引擎上下文（决策依据，用于溯源）
// ==============================================================================
struct EngineContext {
    // 输入快照
    Timestamp candle_time{};
    Candle candle{};
    IndicatorResult indicators{};
    BandResult band{};
    ScoringResult scoring{};
    MTFContext mtf{};
    MarketRegime regime{MarketRegime::Unknown};
    double ai_trend_probability{0.0};

    // 账户状态
    double equity{0.0};
    std::size_t open_position_count{0};

    // 决策过程
    double effective_score{0.0};
    std::string decision_reason;

    [[nodiscard]] std::string to_string() const {
        std::string s;
        s += "ctx{time=" + std::to_string(candle_time.microseconds());
        s += ", state_score=" + std::to_string(effective_score);
        s += ", regime=" + std::to_string(static_cast<int>(regime));
        s += ", mtf_conflict=" + std::to_string(mtf.has_conflict(Side::BUY));
        s += "}";
        return s;
    }
};

// ==============================================================================
// 三级确认（进场时机）
// ==============================================================================
struct EntryConfirmation {
    // 第一级：区间突破
    bool level1_breakout{false};
    double breakout_distance_atr{0.0};
    double breakout_volume_ratio{0.0};

    // 第二级：结构确认（回踩不破或连续确认）
    bool level2_structure{false};
    int consecutive_outside_count{0};

    // 第三级：动能与 AI 共振
    bool level3_momentum{false};
    bool level3_ai_confirmed{false};

    [[nodiscard]] bool all_passed() const noexcept {
        return level1_breakout && level2_structure && level3_momentum;
    }

    [[nodiscard]] int passed_level() const noexcept {
        if (!level1_breakout) return 0;
        if (!level2_structure) return 1;
        if (!level3_momentum) return 2;
        return 3;
    }
};

// ==============================================================================
// 引擎决策（输出，不直接改持仓）
// ==============================================================================
struct EngineDecision {
    enum class Action : std::uint8_t {
        None         = 0,   // 无动作
        OpenLong     = 1,   // 开多
        OpenShort    = 2,   // 开空
        Close        = 3,   // 平仓
        AddPosition  = 4,   // 加仓
        ReducePosition = 5, // 减仓
        MoveStopLoss = 6,   // 移动止损
    };

    // 决策动作
    Action action{Action::None};

    // 下单参数（开仓/加仓时有效）
    Side side{Side::UNKNOWN};
    Price entry_price{};
    Price stop_loss{};
    Price take_profit_1{};
    Price take_profit_2{};
    Quantity suggested_quantity{};

    // 评分与置信度
    double score{0.0};
    double confidence{0.0};

    // 执行时机（下一根 K 线开盘价）
    Timestamp execute_at{};

    // 决策原因（供溯源）
    std::string reason;

    // 溯源 ID
    TraceId trace_id{};

    [[nodiscard]] bool has_action() const noexcept {
        return action != Action::None;
    }
};

// ==============================================================================
// 状态转移记录
// ==============================================================================
struct StateTransition {
    Timestamp timestamp{};
    StrategyState from{StrategyState::S0_FlatObservation};
    StrategyState to{StrategyState::S0_FlatObservation};
    std::string reason;
    TraceId trace_id{};
};

// ==============================================================================
// 状态快照（供前端/持久化）
// ==============================================================================
struct StateSnapshot {
    Timestamp timestamp{};
    StrategyState state{StrategyState::S0_FlatObservation};
    Side current_direction{Side::UNKNOWN};
    double effective_score{0.0};
    double trend_strength{0.0};
    int cooldown_remaining_bars{0};
    int consecutive_losses{0};
    int escape_count{0};
    bool has_position{false};
    std::string position_summary;
};

// ==============================================================================
// 策略引擎接口
// ==============================================================================
class StrategyEngine {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 指标计算器（必需）
        IndicatorCalculator* indicator{nullptr};

        // 带宽计算（必需）
        AdaptiveBand* band{nullptr};

        // 评分系统（必需）
        ScoringSystem* scoring{nullptr};

        // 支撑阻力（可选，nullptr 时跳过 MTF 检查）
        SupportResistance* sr{nullptr};

        // 参数提供者（必需，从 ConfigManager 读取）
        std::function<struct StrategyParams()> params_provider;

        // 回调：状态变更时通知（用于持久化）
        std::function<void(const StateTransition&)> on_state_change;

        // 回调：决策产生时通知（用于溯源、日志）
        std::function<void(const EngineDecision&, const EngineContext&)>
            on_decision;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit StrategyEngine(Dependencies deps);
    ~StrategyEngine();

    // 禁止拷贝（含回调与状态）
    StrategyEngine(const StrategyEngine&) = delete;
    StrategyEngine& operator=(const StrategyEngine&) = delete;

    // 允许移动（转交所有权）
    StrategyEngine(StrategyEngine&&) noexcept;
    StrategyEngine& operator=(StrategyEngine&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    // 启动引擎（初始化状态、订阅配置）
    Result<void> start();

    // 停止引擎（清空状态、取消订阅）
    void stop() noexcept;

    // 重置引擎（清空所有状态，用于测试）
    void reset() noexcept;

    // 是否已启动
    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 事件输入（热路径）
    // -------------------------------------------------------------------------
    // 每根 K 线收盘时调用
    // 幂等性：同一根 K 线重复调用被忽略（按时间戳去重）
    // 线程安全：非线程安全，调用方保证单线程
    Result<EngineDecision> on_candle_close(
        const Candle& candle,
        const IndicatorResult& ind,
        const BandResult& band,
        const MTFContext& mtf,
        const ai::AIDecision* ai_decision,
        double equity,
        std::size_t open_position_count);

    // 持仓更新（OrderFilled / PositionClosed 事件后调用）
    void on_position_opened(const struct Position& pos) noexcept;
    void on_position_closed(const struct Position& pos,
                             std::string_view reason) noexcept;
    void on_position_updated(const struct Position& pos) noexcept;

    // 冷却事件
    void on_stop_loss_triggered() noexcept;
    void on_consecutive_losses(int count) noexcept;

    // -------------------------------------------------------------------------
    // 状态查询（任意时刻，返回一致快照）
    // -------------------------------------------------------------------------
    [[nodiscard]] StateSnapshot state_snapshot() const;
    [[nodiscard]] StrategyState current_state() const noexcept;
    [[nodiscard]] bool has_position() const noexcept;
    [[nodiscard]] Side current_direction() const noexcept;

    // -------------------------------------------------------------------------
    // 状态历史（用于诊断）
    // -------------------------------------------------------------------------
    [[nodiscard]] std::vector<StateTransition> state_history() const;
    [[nodiscard]] std::optional<StateTransition> last_transition() const;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

    // -------------------------------------------------------------------------
    // 策略切换（热切换）
    // -------------------------------------------------------------------------
    // 切换时若当前有持仓，返回失败，要求先平仓
    Result<void> set_strategy_type(std::string_view type_name);

    // -------------------------------------------------------------------------
    // 配置更新
    // -------------------------------------------------------------------------
    // 参数变更时调用，重新加载参数
    void on_params_changed();

private:
    // =========================================================================
    // 状态处理函数
    // =========================================================================
    EngineDecision handle_s0(const EngineContext& ctx);
    EngineDecision handle_s1(const EngineContext& ctx);
    EngineDecision handle_s2(const EngineContext& ctx);
    EngineDecision handle_s3(const EngineContext& ctx);
    EngineDecision handle_s4(const EngineContext& ctx);
    EngineDecision handle_s5(const EngineContext& ctx);
    EngineDecision handle_s6(const EngineContext& ctx);

    // =========================================================================
    // 状态转移
    // =========================================================================
    bool is_transition_allowed(StrategyState from, StrategyState to) const noexcept;
    void transition_to(StrategyState new_state, std::string_view reason,
                       TraceId trace_id);

    // =========================================================================
    // 决策辅助
    // =========================================================================
    // 检查是否可开新仓（冷却、持仓数、风控）
    Result<void> can_open_position(const EngineContext& ctx, Side side) const;

    // 构建入场确认
    EntryConfirmation build_entry_confirmation(const EngineContext& ctx) const;

    // 计算趋势强度
    double compute_trend_strength(const EngineContext& ctx) const;

    // 计算有效评分（融合 AI）
    double compute_effective_score(const EngineContext& ctx) const;

    // 计算止损价格
    Price compute_stop_loss(const EngineContext& ctx, Side side) const;

    // 计算止盈价格
    std::pair<Price, Price> compute_take_profits(
        const EngineContext& ctx, Side side, Price entry, Price stop) const;

    // 检查逃顶信号
    bool is_escape_signal(const EngineContext& ctx) const;

    // 检查趋势切换
    bool is_trend_switch(const EngineContext& ctx) const;

    // 构建最终决策
    EngineDecision make_decision(EngineDecision::Action action,
                                  const EngineContext& ctx,
                                  Side side = Side::UNKNOWN);

    // =========================================================================
    // 内部状态
    // =========================================================================
    Dependencies deps_;

    // 状态
    StrategyState state_{StrategyState::S0_FlatObservation};
    Side current_direction_{Side::UNKNOWN};
    TraceId current_trace_id_{};

    // 幂等性：最后处理的 K 线时间戳
    Timestamp last_processed_time_{};

    // 冷却期截止时间
    Timestamp cooldown_until_{};
    std::string cooldown_reason_;

    // 连续亏损计数
    int consecutive_losses_{0};

    // 逃顶记录
    int escape_count_{0};
    std::deque<Price> escape_prices_;

    // 当前评分
    double current_score_{0.0};
    double current_trend_strength_{0.0};

    // S6 待退出标记
    bool s6_was_neutral_{false};

    // 状态历史
    std::deque<StateTransition> state_history_;

    // 最近决策
    EngineDecision last_decision_{};

    // 运行标志
    std::atomic<bool> running_{false};

    // 策略类型
    std::string strategy_type_{"trend_following"};

    // 诊断计数器
    std::atomic<std::uint64_t> processed_candle_count_{0};
    std::atomic<std::uint64_t> rejected_signal_count_{0};
    std::atomic<std::uint64_t> state_transition_count_{0};
    std::atomic<std::uint64_t> illegal_transition_count_{0};
};

// ==============================================================================
// 辅助函数
// ==============================================================================
[[nodiscard]] inline const char* state_name(StrategyState s) noexcept {
    return to_string(s).data();
}

// 判断是否为持仓状态
[[nodiscard]] inline bool is_position_state(StrategyState s) noexcept {
    return s == StrategyState::S1_TrendPosition
        || s == StrategyState::S3_ReversePosition
        || s == StrategyState::S4_ContinuationAdd;
}

// 判断是否为观察状态
[[nodiscard]] inline bool is_observation_state(StrategyState s) noexcept {
    return s == StrategyState::S0_FlatObservation
        || s == StrategyState::S2_EscapeObservation
        || s == StrategyState::S6_Neutral;
}

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_ENGINE_HPP
