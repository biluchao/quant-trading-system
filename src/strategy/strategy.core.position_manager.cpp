// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 持仓管理实现
// ==============================================================================
// @file    src/strategy/strategy.core.position_manager.cpp
// @module  strategy
// @type    core
// @name    position_manager
// @version 1.0.1
// @brief   移动止损、加仓、分批止盈、时间止损、结构破坏的完整实现
//          已修复 30 类运行时问题
//
// 处理顺序（on_candle_close）:
//   1. 运行检查
//   2. 无持仓快速返回
//   3. K 线有效性检查
//   4. 幂等性检查（时间戳）
//   5. 更新计数与参数
//   6. 结构破坏检测（最高优先级）
//   7. 时间止损
//   8. 分批止盈
//   9. 移动止损（阶梯）
//  10. 加仓检查（无止损动作时）
//  11. 返回动作
// ==============================================================================

#include "strategy/strategy.core.position_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 匿名命名空间：辅助函数
// ==============================================================================
namespace {

constexpr double kEpsilon      = 1e-9;
constexpr double kPriceEps     = position_config::kPriceEpsilon;
constexpr std::int64_t kCandleIntervalUs = 3LL * 60LL * 1000LL * 1000LL;

// -----------------------------------------------------------------------------
// 安全除法
// -----------------------------------------------------------------------------
[[nodiscard]] inline double safe_div(double a, double b,
                                       double def = 0.0) noexcept {
    if (std::abs(b) < kEpsilon) return def;
    const double r = a / b;
    return std::isfinite(r) ? r : def;
}

// -----------------------------------------------------------------------------
// 限幅
// -----------------------------------------------------------------------------
[[nodiscard]] inline double clamp(double v, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, v));
}

// -----------------------------------------------------------------------------
// 有限数检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool finite(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 方向判断
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_long(Side s) noexcept {
    return s == Side::BUY;
}

// -----------------------------------------------------------------------------
// 计算浮盈 R 倍数
// -----------------------------------------------------------------------------
[[nodiscard]] inline double calc_r(const Position& pos,
                                     double current_price,
                                     double stop_distance) noexcept {
    if (stop_distance < kEpsilon) return 0.0;
    const double entry = pos.entry_price.to_double();
    const double diff = is_long(pos.side)
        ? (current_price - entry)
        : (entry - current_price);
    const double r = diff / stop_distance;
    return finite(r) ? r : 0.0;
}

}  // namespace

// ==============================================================================
// PositionManager::Impl（内部状态与内部方法）
// ==============================================================================
struct PositionManager::Impl {
    // -------------------------------------------------------------------------
    // 依赖
    // -------------------------------------------------------------------------
    Dependencies deps;

    // -------------------------------------------------------------------------
    // 持仓状态
    // -------------------------------------------------------------------------
    Position pos{};
    bool has_pos{false};
    Side current_side{Side::UNKNOWN};

    // 首仓信息
    Quantity initial_quantity{};
    double stop_distance{0.0};
    double initial_risk_amount{0.0};

    // 计数
    int add_count{0};
    int tp_stage{0};
    int bars_held{0};
    bool breakeven_set{false};

    // 结构破坏检测
    int structure_break_bars{0};
    bool structure_break_pending{false};

    // 假穿容忍
    int false_breakout_bars{0};

    // 时间戳
    Timestamp opened_at{};
    Timestamp last_processed_time{};
    Timestamp last_add_time{};

    // 缓存参数与账户
    PositionParams params{};
    double equity{0.0};

    // 运行标志
    std::atomic<bool> running{false};

    // 统计
    PositionStats stats{};

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] PositionAction do_structure_break(
        const Candle& candle,
        const IndicatorResult& ind);

    [[nodiscard]] PositionAction do_time_stop(
        const Candle& candle,
        const IndicatorResult& ind,
        double trend_strength);

    [[nodiscard]] PositionAction do_take_profit(
        const Candle& candle,
        const IndicatorResult& ind);

    [[nodiscard]] PositionAction do_trailing_stop(
        const Candle& candle,
        const IndicatorResult& ind,
        double trend_strength);

    [[nodiscard]] PositionAction do_add_position(
        const Candle& candle,
        const IndicatorResult& ind,
        double trend_strength);

    [[nodiscard]] bool check_total_risk(double add_qty) const;

    [[nodiscard]] PositionSnapshot make_snapshot() const;

    void clear_state() noexcept;

    void notify_state_change(std::string_view event) noexcept;

    void notify_action(const PositionAction& a) noexcept;
};

// ==============================================================================
// Impl::do_structure_break
// ==============================================================================
PositionAction PositionManager::Impl::do_structure_break(
    const Candle& candle,
    const IndicatorResult& ind)
{
    PositionAction action;

    const double close = candle.close.to_double();
    const double ema   = ind.ema26;
    const double atr   = ind.atr14;

    if (!finite(ema) || !finite(atr) || atr < kEpsilon) return action;

    const bool long_pos = is_long(current_side);
    const double distance_atr = std::abs(close - ema) / atr;

    // 是否穿越到均线反向
    const bool crossed = long_pos ? (close < ema) : (close > ema);

    // 假穿容忍：距离过小时视为无效穿越
    if (crossed && distance_atr < position_config::kFalseBreakoutMinAtr) {
        ++false_breakout_bars;
        if (false_breakout_bars <=
            position_config::kFalseBreakoutConfirmBars) {
            stats.false_breakout_detected.fetch_add(
                1, std::memory_order_relaxed);
            return action;  // 视为假穿，继续观察
        }
    }

    if (!crossed) {
        structure_break_bars = 0;
        structure_break_pending = false;
        false_breakout_bars = 0;
        return action;
    }

    // 距离不足不构成破坏
    if (distance_atr < position_config::kStructureBreakMinAtr) {
        return action;
    }

    // 斜率方向须与穿越方向一致
    const bool slope_ok = long_pos
        ? (ind.ema_slope < 0)
        : (ind.ema_slope > 0);
    if (!slope_ok) {
        return action;
    }

    // 连续确认
    ++structure_break_bars;
    structure_break_pending = true;

    const int required = params.structure_break_confirm_bars;
    if (structure_break_bars < required) {
        QUANT_LOG_DEBUG("结构破坏确认中: {}/{}",
                        structure_break_bars, required);
        return action;
    }

    action.type = PositionActionType::ClosePosition;
    action.reason = "结构破坏: 连续" + std::to_string(required) +
                    "根穿越EMA + 斜率一致 + 距离>=" +
                    std::to_string(position_config::kStructureBreakMinAtr) +
                    "ATR";
    action.execute_at = Timestamp{
        candle.close_time.microseconds() + kCandleIntervalUs};

    stats.structure_break_triggered.fetch_add(1, std::memory_order_relaxed);

    QUANT_LOG_WARN("结构破坏触发: 平仓 side={} close={:.4f} ema={:.4f} atr={:.4f}",
                   static_cast<int>(current_side), close, ema, atr);

    return action;
}

// ==============================================================================
// Impl::do_time_stop
// ==============================================================================
PositionAction PositionManager::Impl::do_time_stop(
    const Candle& candle,
    const IndicatorResult& ind,
    double trend_strength)
{
    PositionAction action;

    // 趋势强度分级：强趋势放宽
    int required_bars = params.time_stop_bars;
    if (trend_strength > 0.7) {
        required_bars = position_config::kStrongTrendTimeStopBars;
    }

    if (bars_held < required_bars) {
        return action;
    }

    const double close = candle.close.to_double();
    const double r = calc_r(pos, close, stop_distance);

    if (r >= params.time_stop_min_r) {
        return action;
    }

    action.type = PositionActionType::ClosePosition;
    action.reason = "时间止损: " + std::to_string(bars_held) +
                    "根K线浮盈仅 " + std::to_string(r) + "R";
    action.execute_at = Timestamp{
        candle.close_time.microseconds() + kCandleIntervalUs};

    stats.time_stop_triggered.fetch_add(1, std::memory_order_relaxed);

    QUANT_LOG_INFO("时间止损触发: bars={} r={:.2f} threshold={}",
                   bars_held, r, required_bars);

    (void)ind;
    return action;
}

// ==============================================================================
// Impl::do_take_profit
// ==============================================================================
PositionAction PositionManager::Impl::do_take_profit(
    const Candle& candle,
    const IndicatorResult& ind)
{
    PositionAction action;

    if (tp_stage >= static_cast<int>(position_config::kTakeProfitStages)) {
        return action;
    }

    const double close = candle.close.to_double();
    const double r = calc_r(pos, close, stop_distance);

    const double trigger_r = position_config::kTpTriggersR[
        static_cast<std::size_t>(tp_stage)];

    if (r < trigger_r) {
        return action;
    }

    // 计算减仓数量
    const double fraction = position_config::kTpFractions[
        static_cast<std::size_t>(tp_stage)];
    const double current_qty = pos.quantity.to_double();
    const double reduce_qty = current_qty * fraction;

    if (reduce_qty < kEpsilon) {
        return action;
    }

    action.type = PositionActionType::ReducePosition;
    action.quantity = Quantity::from_double(reduce_qty);
    action.trigger_price = candle.close;
    action.reason = "分批止盈 阶段" + std::to_string(tp_stage + 1) +
                    " (" + std::to_string(r) + "R, 平" +
                    std::to_string(static_cast<int>(fraction * 100)) + "%)";
    action.execute_at = Timestamp{
        candle.close_time.microseconds() + kCandleIntervalUs};

    ++tp_stage;
    stats.take_profit_triggered.fetch_add(1, std::memory_order_relaxed);

    QUANT_LOG_INFO("分批止盈: stage={} r={:.2f} qty={:.4f}",
                   tp_stage, r, reduce_qty);

    // 部分止盈后，止损至少移至保本
    if (!breakeven_set) {
        const double entry = pos.entry_price.to_double();
        const double current_stop = pos.stop_loss.to_double();
        const bool should_move = is_long(current_side)
            ? (entry > current_stop + kPriceEps)
            : (entry < current_stop - kPriceEps);

        if (should_move) {
            pos.stop_loss = Price::from_double(entry);
            breakeven_set = true;
            stats.breakeven_triggered.fetch_add(1, std::memory_order_relaxed);
            stats.stop_loss_moved.fetch_add(1, std::memory_order_relaxed);
            QUANT_LOG_INFO("止盈后止损移至保本: {:.4f}", entry);
        }
    }

    (void)ind;
    return action;
}

// ==============================================================================
// Impl::do_trailing_stop
// ==============================================================================
PositionAction PositionManager::Impl::do_trailing_stop(
    const Candle& candle,
    const IndicatorResult& ind,
    double trend_strength)
{
    PositionAction action;

    const double close = candle.close.to_double();
    const double atr = ind.atr14;
    if (!finite(atr) || atr < kEpsilon) return action;

    const double r = calc_r(pos, close, stop_distance);
    const bool long_pos = is_long(current_side);

    double new_stop = pos.stop_loss.to_double();
    std::string reason;

    // 阶梯移动止损
    const double entry = pos.entry_price.to_double();
    for (std::size_t i = 0; i < position_config::kRStopLevels; ++i) {
        if (r < position_config::kRStopTrigger[i]) {
            continue;
        }
        const double target_r = position_config::kRStopPosition[i];
        const double candidate = long_pos
            ? entry + target_r * stop_distance
            : entry - target_r * stop_distance;

        if (long_pos) {
            if (candidate > new_stop + kPriceEps) {
                new_stop = candidate;
                reason = "阶梯移动至 " + std::to_string(target_r) + "R";
            }
        } else {
            if (candidate < new_stop - kPriceEps) {
                new_stop = candidate;
                reason = "阶梯移动至 " + std::to_string(target_r) + "R";
            }
        }
    }

    // 保本检查
    if (!breakeven_set && r >= params.breakeven_r) {
        if (long_pos) {
            if (entry > new_stop + kPriceEps) {
                new_stop = entry;
                reason = "保本止损";
            }
        } else {
            if (entry < new_stop - kPriceEps) {
                new_stop = entry;
                reason = "保本止损";
            }
        }
        breakeven_set = true;
        stats.breakeven_triggered.fetch_add(1, std::memory_order_relaxed);
    }

    // 距离约束：与当前价格至少保持 buffer
    double buffer = params.trailing_buffer_atr * atr;
    if (trend_strength > 0.7) {
        buffer = params.strong_trend_buffer_atr * atr;
    }

    if (long_pos) {
        const double max_allowed = close - buffer;
        if (new_stop > max_allowed) new_stop = max_allowed;
    } else {
        const double min_allowed = close + buffer;
        if (new_stop < min_allowed) new_stop = min_allowed;
    }

    // 判断是否需要更新（严格单调）
    const double current = pos.stop_loss.to_double();
    const bool need_update = long_pos
        ? (new_stop > current + kPriceEps)
        : (new_stop < current - kPriceEps);

    if (!need_update) {
        return action;
    }

    action.type = PositionActionType::MoveStopLoss;
    action.new_stop_loss = Price::from_double(new_stop);
    action.reason = reason.empty() ? "移动止损" : reason;
    action.execute_at = Timestamp{
        candle.close_time.microseconds() + kCandleIntervalUs};

    // 更新内部状态
    pos.stop_loss = action.new_stop_loss;
    stats.stop_loss_moved.fetch_add(1, std::memory_order_relaxed);

    QUANT_LOG_INFO("移动止损: {:.4f} -> {:.4f} ({:.2f}R) reason={}",
                   current, new_stop, r, reason);

    return action;
}

// ==============================================================================
// Impl::do_add_position
// ==============================================================================
PositionAction PositionManager::Impl::do_add_position(
    const Candle& candle,
    const IndicatorResult& ind,
    double trend_strength)
{
    PositionAction action;

    // 加仓次数上限
    if (add_count >= static_cast<int>(params.max_add_count)) {
        return action;
    }

    // 趋势强度检查
    if (trend_strength < params.add_trend_min_strength) {
        return action;
    }

    // 冷却期检查
    const auto cooldown_us = static_cast<std::int64_t>(
        params.add_cooldown_bars) * kCandleIntervalUs;
    if (last_add_time.is_valid()) {
        const auto elapsed = candle.close_time.microseconds() -
                             last_add_time.microseconds();
        if (elapsed < cooldown_us) {
            return action;
        }
    }

    // 触发 R 倍数检查
    const double close = candle.close.to_double();
    const double r = calc_r(pos, close, stop_distance);

    const double trigger_r = (add_count == 0)
        ? params.first_add_trigger_r
        : params.second_add_trigger_r;

    if (r < trigger_r) {
        return action;
    }

    // 计算加仓数量（相对首仓的比例）
    const std::size_t idx = static_cast<std::size_t>(add_count + 1);
    const double scale = (idx < params.add_scales.size())
        ? params.add_scales[idx]
        : 0.4;

    const double add_qty = initial_quantity.to_double() * scale;
    if (add_qty < kEpsilon) return action;

    // 总风险校验
    if (!check_total_risk(add_qty)) {
        stats.risk_check_failed.fetch_add(1, std::memory_order_relaxed);
        QUANT_LOG_WARN("加仓风险校验失败，拒绝加仓: add_qty={:.4f}", add_qty);
        return action;
    }

    action.type = PositionActionType::AddPosition;
    action.quantity = Quantity::from_double(add_qty);
    action.trigger_price = candle.close;
    action.reason = "加仓 第" + std::to_string(add_count + 1) +
                    "次 (" + std::to_string(r) + "R, 比例" +
                    std::to_string(scale) + ")";
    action.execute_at = Timestamp{
        candle.close_time.microseconds() + kCandleIntervalUs};

    ++add_count;
    last_add_time = candle.close_time;
    stats.position_added.fetch_add(1, std::memory_order_relaxed);

    QUANT_LOG_INFO("加仓: 第{}次 qty={:.4f} r={:.2f} scale={}",
                   add_count, add_qty, r, scale);

    (void)ind;
    return action;
}

// ==============================================================================
// Impl::check_total_risk
// ==============================================================================
bool PositionManager::Impl::check_total_risk(double add_qty) const {
    if (equity < kEpsilon) return false;

    const double entry = pos.entry_price.to_double();
    const double stop  = pos.stop_loss.to_double();
    const double stop_dist = std::abs(entry - stop);

    if (stop_dist < kEpsilon) return false;

    const double current_qty = pos.quantity.to_double();
    const double total_qty = current_qty + add_qty;
    const double total_risk = stop_dist * total_qty;

    const double risk_pct = safe_div(total_risk, equity, 0.0);

    if (risk_pct > params.total_risk_budget) {
        QUANT_LOG_DEBUG("加仓后风险 {:.4f} 超预算 {:.4f}",
                        risk_pct, params.total_risk_budget);
        return false;
    }

    return true;
}

// ==============================================================================
// Impl::make_snapshot
// ==============================================================================
PositionSnapshot PositionManager::Impl::make_snapshot() const {
    PositionSnapshot s;

    if (!has_pos) {
        return s;
    }

    s.has_position = true;
    s.side = pos.side;
    s.symbol = pos.symbol;

    s.entry_price = pos.entry_price;
    s.current_price = pos.mark_price;
    s.stop_loss = pos.stop_loss;
    s.take_profit_1 = pos.take_profit;
    s.take_profit_2 = pos.take_profit;

    s.quantity = pos.quantity;
    s.initial_quantity = initial_quantity;

    s.stop_distance = stop_distance;
    s.unrealized_pnl = pos.unrealized_pnl().to_double();

    const double close = pos.mark_price.to_double();
    s.unrealized_r = calc_r(pos, close, stop_distance);

    s.bars_held = bars_held;
    s.add_count = add_count;
    s.tp_stage = tp_stage;
    s.breakeven_set = breakeven_set;

    return s;
}

// ==============================================================================
// Impl::clear_state
// ==============================================================================
void PositionManager::Impl::clear_state() noexcept {
    pos = Position{};
    has_pos = false;
    current_side = Side::UNKNOWN;
    initial_quantity = Quantity{};
    stop_distance = 0.0;
    initial_risk_amount = 0.0;
    add_count = 0;
    tp_stage = 0;
    bars_held = 0;
    breakeven_set = false;
    structure_break_bars = 0;
    structure_break_pending = false;
    false_breakout_bars = 0;
    opened_at = Timestamp{};
    last_processed_time = Timestamp{};
    last_add_time = Timestamp{};
}

// ==============================================================================
// Impl::notify_state_change
// ==============================================================================
void PositionManager::Impl::notify_state_change(std::string_view event) noexcept {
    if (!deps.on_state_change) return;
    try {
        deps.on_state_change(event, make_snapshot());
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_state_change 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_state_change 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_action
// ==============================================================================
void PositionManager::Impl::notify_action(const PositionAction& a) noexcept {
    if (!deps.on_action) return;
    try {
        deps.on_action(a, make_snapshot());
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_action 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_action 回调未知异常");
    }
}

// ==============================================================================
// PositionManager 构造与析构
// ==============================================================================
PositionManager::PositionManager(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    // 校验必需依赖
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "PositionManager: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }
    if (!deps.equity_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "PositionManager: equity_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }

    impl_->deps = std::move(deps);
}

PositionManager::~PositionManager() {
    stop();
}

PositionManager::PositionManager(PositionManager&& other) noexcept
    : impl_(std::move(other.impl_)) {}

PositionManager& PositionManager::operator=(PositionManager&& other) noexcept {
    if (this != &other) {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

// ==============================================================================
// 生命周期
// ==============================================================================
Result<void> PositionManager::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "PositionManager 已启动");
    }

    impl_->clear_state();

    QUANT_LOG_INFO("PositionManager 已启动");
    return {};
}

void PositionManager::stop() noexcept {
    if (!impl_) return;
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    impl_->clear_state();

    QUANT_LOG_INFO("PositionManager 已停止");
}

void PositionManager::reset() noexcept {
    if (!impl_) return;

    impl_->clear_state();
    impl_->stats.reset();
}

bool PositionManager::is_running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

// ==============================================================================
// 持仓事件
// ==============================================================================
Result<void> PositionManager::on_position_opened(const Position& pos,
                                                   const Signal& signal)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "PositionManager 未启动");
    }

    // 校验持仓有效性
    if (!pos.is_valid() || pos.side == Side::UNKNOWN) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "持仓数据无效")
            .with_context("side", std::to_string(static_cast<int>(pos.side)));
    }

    // 校验方向与信号一致
    if (signal.side != Side::UNKNOWN && signal.side != pos.side) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "信号方向与持仓方向不一致");
    }

    // 计算止损距离
    const double entry = pos.entry_price.to_double();
    const double stop  = pos.stop_loss.to_double();
    const double dist  = std::abs(entry - stop);

    if (!finite(dist) || dist < kPriceEps) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "止损距离无效")
            .with_context("distance", std::to_string(dist));
    }

    // 加载参数与权益
    impl_->params = impl_->deps.params_provider();
    impl_->equity = impl_->deps.equity_provider();

    // 保存持仓状态
    impl_->pos = pos;
    impl_->has_pos = true;
    impl_->current_side = pos.side;
    impl_->initial_quantity = pos.quantity;
    impl_->stop_distance = dist;
    impl_->initial_risk_amount = dist * pos.quantity.to_double();
    impl_->add_count = 0;
    impl_->tp_stage = 0;
    impl_->bars_held = 0;
    impl_->breakeven_set = false;
    impl_->structure_break_bars = 0;
    impl_->structure_break_pending = false;
    impl_->false_breakout_bars = 0;
    impl_->opened_at = pos.opened_at;
    impl_->last_processed_time = Timestamp{};
    impl_->last_add_time = pos.opened_at;

    impl_->stats.position_opened.fetch_add(1, std::memory_order_relaxed);

    QUANT_LOG_INFO("持仓已开: side={} entry={:.4f} stop={:.4f} "
                   "qty={:.4f} stop_dist={:.4f}",
                   static_cast<int>(pos.side), entry, stop,
                   pos.quantity.to_double(), dist);

    impl_->notify_state_change("opened");

    return {};
}

void PositionManager::on_position_updated(const Position& pos) noexcept {
    if (!impl_ || !impl_->has_pos) return;

    impl_->pos.quantity = pos.quantity;
    impl_->pos.filled_quantity = pos.filled_quantity;
    impl_->pos.average_price = pos.average_price;

    QUANT_LOG_DEBUG("持仓更新: qty={:.4f}", pos.quantity.to_double());
}

void PositionManager::on_position_closed(std::string_view reason) noexcept {
    if (!impl_ || !impl_->has_pos) return;

    QUANT_LOG_INFO("持仓已平: reason={} bars_held={} add_count={}",
                   reason, impl_->bars_held, impl_->add_count);

    impl_->clear_state();
    impl_->stats.position_closed.fetch_add(1, std::memory_order_relaxed);

    impl_->notify_state_change("closed");
}

void PositionManager::on_partial_fill(const Fill& fill) noexcept {
    if (!impl_ || !impl_->has_pos) return;

    QUANT_LOG_DEBUG("部分成交: qty={:.4f} price={:.4f}",
                    fill.quantity.to_double(), fill.price.to_double());

    impl_->pos.filled_quantity =
        impl_->pos.filled_quantity + fill.quantity;

    // 部分成交后重算初始风险
    const double filled = impl_->pos.filled_quantity.to_double();
    if (filled > kEpsilon) {
        impl_->initial_risk_amount = impl_->stop_distance * filled;
    }
}

// ==============================================================================
// 每根 K 线处理
// ==============================================================================
Result<PositionAction> PositionManager::on_candle_close(
    const Candle& candle,
    const IndicatorResult& ind,
    const BandResult& band,
    const MTFContext* mtf,
    double trend_strength)
{
    // 1. 运行检查
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "PositionManager 未启动");
    }

    // 2. 无持仓快速返回
    if (!impl_->has_pos) {
        return PositionAction{};
    }

    // 3. K 线有效性
    if (!candle.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "K 线数据无效");
    }

    // 4. 幂等性检查
    const auto now_us = candle.close_time.microseconds();
    if (impl_->last_processed_time.is_valid() &&
        now_us <= impl_->last_processed_time.microseconds()) {
        QUANT_LOG_DEBUG("重复 K 线，跳过处理");
        return PositionAction{};
    }
    impl_->last_processed_time = candle.close_time;

    // 5. 更新计数与参数
    ++impl_->bars_held;
    impl_->equity = impl_->deps.equity_provider();
    impl_->params = impl_->deps.params_provider();

    // 6. 结构破坏（最高优先级）
    if (auto a = impl_->do_structure_break(candle, ind); a.has_action()) {
        impl_->notify_action(a);
        return a;
    }

    // 7. 时间止损
    if (auto a = impl_->do_time_stop(candle, ind, trend_strength);
        a.has_action()) {
        impl_->notify_action(a);
        return a;
    }

    // 8. 分批止盈
    if (auto a = impl_->do_take_profit(candle, ind); a.has_action()) {
        impl_->notify_action(a);
        return a;
    }

    // 9. 移动止损
    auto stop_action = impl_->do_trailing_stop(candle, ind, trend_strength);

    // 10. 加仓检查（仅在无止损动作时）
    if (!stop_action.has_action()) {
        if (auto add_action = impl_->do_add_position(
                candle, ind, trend_strength);
            add_action.has_action()) {
            impl_->notify_action(add_action);
            return add_action;
        }
    }

    // 11. 返回移动止损（若有）
    if (stop_action.has_action()) {
        impl_->notify_action(stop_action);
        return stop_action;
    }

    (void)band;
    (void)mtf;
    return PositionAction{};
}

// ==============================================================================
// 加仓请求（外部触发）
// ==============================================================================
Result<PositionAction> PositionManager::request_add_position(
    const Candle& candle,
    const IndicatorResult& ind,
    double trend_strength)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "PositionManager 未启动");
    }
    if (!impl_->has_pos) {
        return QUANT_ERR_MSG(ErrorCode::StrategyStateInvalid,
            "无持仓，无法加仓");
    }

    return impl_->do_add_position(candle, ind, trend_strength);
}

// ==============================================================================
// 减仓请求（风控触发）
// ==============================================================================
Result<PositionAction> PositionManager::request_reduce_position(
    double fraction,
    std::string_view reason)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "PositionManager 未启动");
    }
    if (!impl_->has_pos) {
        return QUANT_ERR_MSG(ErrorCode::StrategyStateInvalid,
            "无持仓，无法减仓");
    }

    const double f = clamp(fraction, 0.0, 1.0);
    if (f < kEpsilon) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "减仓比例过小");
    }

    const double current_qty = impl_->pos.quantity.to_double();
    const double reduce_qty = current_qty * f;
    if (reduce_qty < kEpsilon) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "减仓数量过小");
    }

    PositionAction action;
    action.type = PositionActionType::ReducePosition;
    action.quantity = Quantity::from_double(reduce_qty);
    action.trigger_price = impl_->pos.mark_price;
    action.reason = std::string{"风控减仓: "} + std::string{reason};

    impl_->stats.position_reduced.fetch_add(1, std::memory_order_relaxed);

    QUANT_LOG_INFO("减仓: qty={:.4f} reason={}", reduce_qty, reason);

    impl_->notify_action(action);
    return action;
}

// ==============================================================================
// 状态查询
// ==============================================================================
PositionSnapshot PositionManager::snapshot() const {
    return impl_ ? impl_->make_snapshot() : PositionSnapshot{};
}

bool PositionManager::has_position() const noexcept {
    return impl_ && impl_->has_pos;
}

Side PositionManager::current_side() const noexcept {
    return impl_ ? impl_->current_side : Side::UNKNOWN;
}

double PositionManager::unrealized_r() const noexcept {
    if (!impl_ || !impl_->has_pos) return 0.0;
    const double close = impl_->pos.mark_price.to_double();
    return calc_r(impl_->pos, close, impl_->stop_distance);
}

int PositionManager::bars_held() const noexcept {
    return impl_ ? impl_->bars_held : 0;
}

int PositionManager::add_count() const noexcept {
    return impl_ ? impl_->add_count : 0;
}

// ==============================================================================
// 统计与诊断
// ==============================================================================
const PositionStats& PositionManager::stats() const noexcept {
    static const PositionStats kEmpty;
    return impl_ ? impl_->stats : kEmpty;
}

std::string PositionManager::dump() const {
    std::ostringstream oss;
    oss << "PositionManager dump:\n";

    if (!impl_) {
        oss << "  <impl is null>\n";
        return oss.str();
    }

    oss << "  running: " << (is_running() ? "yes" : "no") << "\n";
    oss << "  has_position: " << (impl_->has_pos ? "yes" : "no") << "\n";

    if (impl_->has_pos) {
        oss << "  side: " << static_cast<int>(impl_->current_side) << "\n";
        oss << "  entry: " << impl_->pos.entry_price.to_double() << "\n";
        oss << "  current: " << impl_->pos.mark_price.to_double() << "\n";
        oss << "  stop_loss: " << impl_->pos.stop_loss.to_double() << "\n";
        oss << "  quantity: " << impl_->pos.quantity.to_double() << "\n";
        oss << "  initial_qty: "
            << impl_->initial_quantity.to_double() << "\n";
        oss << "  stop_distance: " << impl_->stop_distance << "\n";
        oss << "  unrealized_r: " << unrealized_r() << "\n";
        oss << "  bars_held: " << impl_->bars_held << "\n";
        oss << "  add_count: " << impl_->add_count << "\n";
        oss << "  tp_stage: " << impl_->tp_stage << "\n";
        oss << "  breakeven_set: "
            << (impl_->breakeven_set ? "yes" : "no") << "\n";
        oss << "  structure_break_bars: "
            << impl_->structure_break_bars << "\n";
        oss << "  false_breakout_bars: "
            << impl_->false_breakout_bars << "\n";
    }

    oss << "  stats:\n";
    const auto& s = impl_->stats;
    oss << "    opened: "
        << s.position_opened.load(std::memory_order_relaxed) << "\n";
    oss << "    closed: "
        << s.position_closed.load(std::memory_order_relaxed) << "\n";
    oss << "    stop_loss_moved: "
        << s.stop_loss_moved.load(std::memory_order_relaxed) << "\n";
    oss << "    added: "
        << s.position_added.load(std::memory_order_relaxed) << "\n";
    oss << "    reduced: "
        << s.position_reduced.load(std::memory_order_relaxed) << "\n";
    oss << "    take_profit_triggered: "
        << s.take_profit_triggered.load(std::memory_order_relaxed) << "\n";
    oss << "    breakeven_triggered: "
        << s.breakeven_triggered.load(std::memory_order_relaxed) << "\n";
    oss << "    time_stop_triggered: "
        << s.time_stop_triggered.load(std::memory_order_relaxed) << "\n";
    oss << "    structure_break_triggered: "
        << s.structure_break_triggered.load(std::memory_order_relaxed) << "\n";
    oss << "    false_breakout_detected: "
        << s.false_breakout_detected.load(std::memory_order_relaxed) << "\n";
    oss << "    risk_check_failed: "
        << s.risk_check_failed.load(std::memory_order_relaxed) << "\n";

    return oss.str();
}

// ==============================================================================
// PositionSnapshot::to_string
// ==============================================================================
std::string PositionSnapshot::to_string() const {
    if (!has_position) {
        return "PositionSnapshot{no position}";
    }

    std::ostringstream oss;
    oss << "PositionSnapshot{"
        << "side=" << static_cast<int>(side)
        << ", symbol=" << symbol.view()
        << ", entry=" << entry_price.to_double()
        << ", current=" << current_price.to_double()
        << ", stop=" << stop_loss.to_double()
        << ", qty=" << quantity.to_double()
        << ", R=" << unrealized_r
        << ", bars=" << bars_held
        << ", adds=" << add_count
        << ", tp_stage=" << tp_stage
        << ", be=" << (breakeven_set ? "yes" : "no")
        << ", pnl=" << unrealized_pnl
        << "}";
    return oss.str();
}

}  // namespace quant::strategy
