// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 盈亏比第一原则强制校验实现
// ==============================================================================
// @file    src/strategy/strategy.core.risk_check.cpp
// @module  strategy
// @type    core
// @name    risk_check
// @version 1.0.1
// @brief   盈亏比、仓位、成本、账户、组合风险的多层校验实现
//          已修复 30 类运行时问题
//
// 校验优先级（validate_signal）:
//   1. 运行检查
//   2. 输入有效性（side/price/finite）
//   3. 止损方向一致性
//   4. 止损距离范围
//   5. 盈亏比（硬约束 ≥ 1.5）
//   6. 仓位计算
//   7. 总风险预算（≤ 2%）
//   8. 成本覆盖（预期盈利 > 3× 成本）
//   9. 账户健康（日亏损/回撤/杠杆/持仓数）
//  10. 组合风险（相关性）
// ==============================================================================

#include "strategy/strategy.core.risk_check.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 匿名命名空间：辅助函数
// ==============================================================================
namespace {

constexpr double kEps = risk_config::kEpsilon;

// -----------------------------------------------------------------------------
// 安全除法
// -----------------------------------------------------------------------------
[[nodiscard]] inline double safe_div(double a, double b,
                                       double def = 0.0) noexcept {
    if (std::abs(b) < kEps) return def;
    const double r = a / b;
    return std::isfinite(r) ? r : def;
}

// -----------------------------------------------------------------------------
// 限幅
// -----------------------------------------------------------------------------
[[nodiscard]] inline double clamp_val(double v, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, v));
}

// -----------------------------------------------------------------------------
// 有限数检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool finite(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 计算止损距离
// -----------------------------------------------------------------------------
[[nodiscard]] inline double calc_stop_distance(double entry,
                                                 double stop) noexcept {
    const double d = std::abs(entry - stop);
    return finite(d) ? d : 0.0;
}

// -----------------------------------------------------------------------------
// 计算盈亏比（对给定 target）
// -----------------------------------------------------------------------------
[[nodiscard]] inline double calc_rr(double entry, double stop,
                                      double target) noexcept {
    const double risk = calc_stop_distance(entry, stop);
    if (risk < kEps) return 0.0;
    const double reward = std::abs(target - entry);
    if (!finite(reward)) return 0.0;
    const double rr = reward / risk;
    return finite(rr) ? rr : 0.0;
}

// -----------------------------------------------------------------------------
// 判断方向一致性
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_direction_consistent(Side side,
                                                    double entry,
                                                    double stop,
                                                    double tp1) noexcept {
    if (side == Side::BUY) {
        return stop < entry && entry < tp1;
    }
    if (side == Side::SELL) {
        return tp1 < entry && entry < stop;
    }
    return false;
}

}  // namespace

// ==============================================================================
// AccountState::drawdown_pct
// ==============================================================================
double AccountState::drawdown_pct() const noexcept {
    if (peak_equity < kEps) return 0.0;
    const double dd = (peak_equity - equity) / peak_equity;
    return clamp_val(finite(dd) ? dd : 0.0, 0.0, 1.0);
}

// ==============================================================================
// AccountState::daily_loss_pct
// ==============================================================================
double AccountState::daily_loss_pct() const noexcept {
    if (daily_start_equity < kEps) return 0.0;
    const double loss = -daily_pnl / daily_start_equity;
    return clamp_val(finite(loss) ? loss : 0.0, 0.0, 1.0);
}

// ==============================================================================
// RiskParams::validate
// ==============================================================================
Result<void> RiskParams::validate() const noexcept {
    // 风险百分比
    if (risk_per_trade <= 0.0 || risk_per_trade > 0.10) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "risk_per_trade 超出范围 [0, 0.10]")
            .with_context("value", std::to_string(risk_per_trade));
    }
    if (max_risk_per_trade > 0.10) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_risk_per_trade 超出范围");
    }
    if (risk_per_trade > max_risk_per_trade) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "risk_per_trade > max_risk_per_trade");
    }
    if (min_risk_per_trade > risk_per_trade) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_risk_per_trade > risk_per_trade");
    }

    // 反向风险
    if (reverse_risk <= 0.0 || reverse_risk > max_reverse_risk) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "reverse_risk 超出范围");
    }

    // 盈亏比
    if (min_rr_ratio < 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_rr_ratio 必须 ≥ 1.0");
    }
    if (max_rr_ratio <= min_rr_ratio) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_rr_ratio 必须 > min_rr_ratio");
    }

    // 止损距离
    if (min_stop_distance_pct <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_stop_distance_pct 必须 > 0");
    }
    if (max_stop_distance_pct <= min_stop_distance_pct) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_stop_distance_pct 必须 > min_stop_distance_pct");
    }

    // 加仓风险分配
    const double add_sum = first_add_risk + second_add_risk + third_add_risk;
    if (add_sum > max_risk_per_trade + kEps) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "加仓风险总分配超过 max_risk_per_trade")
            .with_context("sum", std::to_string(add_sum));
    }

    // 成本参数
    if (maker_fee < 0.0 || taker_fee < 0.0 ||
        slippage_estimate < 0.0 || funding_rate < 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "成本参数不能为负");
    }

    // 账户风控
    if (max_daily_loss_pct <= 0.0 || max_daily_loss_pct > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_daily_loss_pct 超出范围 (0, 1]");
    }
    if (max_drawdown_pct <= 0.0 || max_drawdown_pct > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_drawdown_pct 超出范围 (0, 1]");
    }
    if (max_concurrent_positions == 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_concurrent_positions 必须 > 0");
    }
    if (max_leverage == 0 || max_leverage > 125) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_leverage 超出范围 [1, 125]");
    }

    // 组合风险
    if (max_portfolio_risk_pct <= 0.0 || max_portfolio_risk_pct > 0.10) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_portfolio_risk_pct 超出范围");
    }

    return {};
}

// ==============================================================================
// RiskCheck::Impl
// ==============================================================================
struct RiskCheck::Impl {
    Dependencies deps;
    std::atomic<bool> running{false};
    RiskCheckStats stats;

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void> validate_input_signal(
        const Signal& signal) const;

    [[nodiscard]] Result<void> validate_direction(
        const Signal& signal) const;

    [[nodiscard]] Result<void> validate_stop_distance(
        const Signal& signal,
        double entry,
        double stop_distance) const;

    [[nodiscard]] Result<void> validate_rr_ratio(
        const Signal& signal,
        double rr_tp1) const;

    [[nodiscard]] Result<void> validate_position_size(
        double position_size) const;

    [[nodiscard]] Result<void> check_total_risk(
        double current_risk,
        double new_risk) const;

    [[nodiscard]] Result<void> check_cost_coverage(
        double expected_profit,
        const CostBreakdown& cost,
        double min_ratio) const;

    // 统计辅助
    void count_reject(ErrorCode code) noexcept;

    // 通用校验核心
    [[nodiscard]] Result<RiskValidation> validate_core(
        const Signal& signal,
        double risk_pct,
        bool is_reverse);
};

// ==============================================================================
// Impl::validate_input_signal
// ==============================================================================
Result<void> RiskCheck::Impl::validate_input_signal(
    const Signal& signal) const
{
    if (signal.side == Side::UNKNOWN) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "信号方向未定义 (UNKNOWN)");
    }
    if (signal.symbol.empty()) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "信号 symbol 为空");
    }

    const double entry = signal.entry_price.to_double();
    const double stop  = signal.stop_loss.to_double();
    const double tp1   = signal.take_profit_1.to_double();

    if (!finite(entry) || entry <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "入场价无效")
            .with_context("entry", std::to_string(entry));
    }
    if (!finite(stop) || stop <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "止损价无效")
            .with_context("stop", std::to_string(stop));
    }
    if (!finite(tp1) || tp1 <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "止盈1价无效")
            .with_context("tp1", std::to_string(tp1));
    }

    return {};
}

// ==============================================================================
// Impl::validate_direction
// ==============================================================================
Result<void> RiskCheck::Impl::validate_direction(
    const Signal& signal) const
{
    const double entry = signal.entry_price.to_double();
    const double stop  = signal.stop_loss.to_double();
    const double tp1   = signal.take_profit_1.to_double();

    if (!is_direction_consistent(signal.side, entry, stop, tp1)) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "止损/止盈方向与信号方向不一致")
            .with_context("side", std::to_string(static_cast<int>(signal.side)))
            .with_context("entry", std::to_string(entry))
            .with_context("stop", std::to_string(stop))
            .with_context("tp1", std::to_string(tp1));
    }

    return {};
}

// ==============================================================================
// Impl::validate_stop_distance
// ==============================================================================
Result<void> RiskCheck::Impl::validate_stop_distance(
    const Signal& signal,
    double entry,
    double stop_distance) const
{
    const auto& p = deps.params_provider();

    const double min_dist = entry * p.min_stop_distance_pct;
    const double max_dist = entry * p.max_stop_distance_pct;

    if (stop_distance < min_dist) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "止损距离过小")
            .with_context("distance", std::to_string(stop_distance))
            .with_context("min", std::to_string(min_dist));
    }
    if (stop_distance > max_dist) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "止损距离过大")
            .with_context("distance", std::to_string(stop_distance))
            .with_context("max", std::to_string(max_dist));
    }

    (void)signal;
    return {};
}

// ==============================================================================
// Impl::validate_rr_ratio
// ==============================================================================
Result<void> RiskCheck::Impl::validate_rr_ratio(
    const Signal& signal,
    double rr_tp1) const
{
    const auto& p = deps.params_provider();

    if (rr_tp1 < p.min_rr_ratio) {
        return QUANT_ERR_MSG(ErrorCode::StrategyRiskRewardFail,
            "盈亏比低于最小值")
            .with_context("rr", std::to_string(rr_tp1))
            .with_context("min", std::to_string(p.min_rr_ratio));
    }
    if (rr_tp1 > p.max_rr_ratio) {
        return QUANT_ERR_MSG(ErrorCode::StrategyRiskRewardFail,
            "盈亏比异常过大（可能数据错误）")
            .with_context("rr", std::to_string(rr_tp1))
            .with_context("max", std::to_string(p.max_rr_ratio));
    }

    (void)signal;
    return {};
}

// ==============================================================================
// Impl::validate_position_size
// ==============================================================================
Result<void> RiskCheck::Impl::validate_position_size(
    double position_size) const
{
    if (!finite(position_size) || position_size <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "仓位计算结果无效")
            .with_context("size", std::to_string(position_size));
    }
    return {};
}

// ==============================================================================
// Impl::check_total_risk
// ==============================================================================
Result<void> RiskCheck::Impl::check_total_risk(
    double current_risk,
    double new_risk) const
{
    const auto account = deps.account_provider();
    if (account.equity <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::RiskDailyLossLimit,
            "账户权益为负");
    }

    const auto& p = deps.params_provider();
    const double total_risk = current_risk + new_risk;
    const double total_pct = safe_div(total_risk, account.equity, 0.0);

    if (total_pct > p.max_risk_per_trade + kEps) {
        return QUANT_ERR_MSG(ErrorCode::RiskPositionLimit,
            "总风险超预算")
            .with_context("total_pct", std::to_string(total_pct))
            .with_context("max", std::to_string(p.max_risk_per_trade));
    }

    return {};
}

// ==============================================================================
// Impl::check_cost_coverage
// ==============================================================================
Result<void> RiskCheck::Impl::check_cost_coverage(
    double expected_profit,
    const CostBreakdown& cost,
    double min_ratio) const
{
    if (cost.total < kEps) {
        // 成本极小，无需过滤
        return {};
    }

    const double coverage = safe_div(expected_profit, cost.total, 0.0);
    if (coverage < min_ratio) {
        return QUANT_ERR_MSG(ErrorCode::StrategyRiskRewardFail,
            "预期盈利不足以覆盖成本")
            .with_context("profit", std::to_string(expected_profit))
            .with_context("cost", std::to_string(cost.total))
            .with_context("coverage", std::to_string(coverage))
            .with_context("min_ratio", std::to_string(min_ratio));
    }

    return {};
}

// ==============================================================================
// Impl::count_reject
// ==============================================================================
void RiskCheck::Impl::count_reject(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::StrategyInvalidSignal:
            stats.rejected_invalid_input.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case ErrorCode::StrategyRiskRewardFail:
            stats.rejected_rr_ratio.fetch_add(1, std::memory_order_relaxed);
            break;
        case ErrorCode::RiskPositionLimit:
            stats.rejected_total_risk.fetch_add(1, std::memory_order_relaxed);
            break;
        case ErrorCode::RiskDailyLossLimit:
            stats.rejected_daily_loss.fetch_add(1, std::memory_order_relaxed);
            break;
        case ErrorCode::RiskDrawdownLimit:
            stats.rejected_drawdown.fetch_add(1, std::memory_order_relaxed);
            break;
        case ErrorCode::RiskLeverageLimit:
            stats.rejected_leverage.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            stats.rejected_unknown.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

// ==============================================================================
// Impl::validate_core（通用校验核心）
// ==============================================================================
Result<RiskValidation> RiskCheck::Impl::validate_core(
    const Signal& signal,
    double risk_pct,
    bool is_reverse)
{
    RiskValidation v;

    // ---------- 1. 输入校验 ----------
    if (auto r = validate_input_signal(signal); r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(v.reject_code);
        return v;
    }

    // ---------- 2. 方向校验 ----------
    if (auto r = validate_direction(signal); r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(v.reject_code);
        return v;
    }

    const double entry = signal.entry_price.to_double();
    const double stop  = signal.stop_loss.to_double();
    const double tp1   = signal.take_profit_1.to_double();
    const double tp2   = signal.take_profit_2.to_double();

    // ---------- 3. 止损距离 ----------
    const double stop_dist = calc_stop_distance(entry, stop);
    v.stop_distance = stop_dist;
    v.stop_distance_pct = safe_div(stop_dist, entry, 0.0);

    if (auto r = validate_stop_distance(signal, entry, stop_dist);
        r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(ErrorCode::StrategyInvalidSignal);
        return v;
    }

    // ---------- 4. 盈亏比 ----------
    v.rr_ratio_tp1 = calc_rr(entry, stop, tp1);
    v.rr_ratio = v.rr_ratio_tp1;

    if (tp2 > 0.0 && finite(tp2)) {
        v.rr_ratio_tp2 = calc_rr(entry, stop, tp2);
    }

    if (auto r = validate_rr_ratio(signal, v.rr_ratio_tp1); r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(ErrorCode::StrategyRiskRewardFail);
        return v;
    }

    // ---------- 5. 仓位计算 ----------
    const auto account = deps.account_provider();
    if (!account.is_valid()) {
        v.passed = false;
        v.reject_code = ErrorCode::RiskDailyLossLimit;
        v.reject_reason = "账户状态无效";
        count_reject(v.reject_code);
        return v;
    }

    const auto& params = deps.params_provider();

    // 风险金额
    v.risk_pct = clamp_val(risk_pct, params.min_risk_per_trade,
                            params.max_risk_per_trade);
    v.risk_amount = account.equity * v.risk_pct;

    // 仓位 = 风险金额 / 止损距离
    v.position_size = safe_div(v.risk_amount, stop_dist, 0.0);
    v.position_notional = v.position_size * entry;
    v.margin_required = safe_div(v.position_notional,
                                   static_cast<double>(account.current_leverage),
                                   v.position_notional);

    if (auto r = validate_position_size(v.position_size); r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(ErrorCode::StrategyInvalidSignal);
        return v;
    }

    // ---------- 6. 总风险预算 ----------
    // 计算当前已用风险（通过持仓提供者）
    double current_risk = 0.0;
    if (deps.positions_provider) {
        try {
            auto positions = deps.positions_provider();
            for (const auto& pos : positions) {
                current_risk += pos.risk_amount;
            }
        } catch (const std::exception& e) {
            QUANT_LOG_WARN("positions_provider 异常: {}", e.what());
        }
    }

    if (auto r = check_total_risk(current_risk, v.risk_amount); r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(ErrorCode::RiskPositionLimit);
        return v;
    }

    // ---------- 7. 成本覆盖 ----------
    v.cost = compute_cost(v.position_notional, false, 1);

    // 预期盈利 = (tp1 - entry) × position_size
    const double price_reward = std::abs(tp1 - entry);
    v.expected_profit = price_reward * v.position_size;

    v.cost_coverage_ratio = safe_div(v.expected_profit, v.cost.total, 0.0);

    if (auto r = check_cost_coverage(v.expected_profit, v.cost,
                                       params.min_cost_coverage_ratio);
        r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(ErrorCode::StrategyRiskRewardFail);
        return v;
    }

    // ---------- 8. 账户健康 ----------
    if (auto r = check_daily_loss(); r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(ErrorCode::RiskDailyLossLimit);
        return v;
    }

    if (auto r = check_drawdown(); r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(ErrorCode::RiskDrawdownLimit);
        return v;
    }

    if (auto r = check_position_count(); r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(ErrorCode::RiskPositionLimit);
        return v;
    }

    if (auto r = check_leverage(account.current_leverage); r.is_err()) {
        v.passed = false;
        v.reject_code = r.error().code;
        v.reject_reason = r.error().to_string();
        count_reject(ErrorCode::RiskLeverageLimit);
        return v;
    }

    // ---------- 9. 组合风险 ----------
    if (deps.positions_provider && deps.correlation_provider) {
        if (auto r = check_portfolio_risk(signal.symbol, signal.side,
                                            v.risk_amount);
            r.is_err()) {
            v.passed = false;
            v.reject_code = r.error().code;
            v.reject_reason = r.error().to_string();
            count_reject(ErrorCode::RiskPositionLimit);
            return v;
        }
    }

    // ---------- 10. 通过 ----------
    v.passed = true;
    v.reject_code = ErrorCode::Ok;
    v.reject_reason = is_reverse ? "反向信号通过校验" : "顺势信号通过校验";

    return v;
}

// ==============================================================================
// RiskCheck 构造与析构
// ==============================================================================
RiskCheck::RiskCheck(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "RiskCheck: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }
    if (!deps.account_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "RiskCheck: account_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }

    impl_->deps = std::move(deps);
}

RiskCheck::~RiskCheck() {
    stop();
}

RiskCheck::RiskCheck(RiskCheck&& other) noexcept
    : impl_(std::move(other.impl_)) {}

RiskCheck& RiskCheck::operator=(RiskCheck&& other) noexcept {
    if (this != &other) {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

// ==============================================================================
// 生命周期
// ==============================================================================
Result<void> RiskCheck::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskCheck 已启动");
    }

    // 校验参数
    try {
        const auto p = impl_->deps.params_provider();
        if (auto r = p.validate(); r.is_err()) {
            impl_->running.store(false, std::memory_order_release);
            return r;
        }
    } catch (const std::exception& e) {
        impl_->running.store(false, std::memory_order_release);
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "参数加载异常")
            .with_context("error", e.what());
    }

    QUANT_LOG_INFO("RiskCheck 已启动");
    return {};
}

void RiskCheck::stop() noexcept {
    if (!impl_) return;
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    QUANT_LOG_INFO("RiskCheck 已停止");
}

void RiskCheck::reset() noexcept {
    if (!impl_) return;
    impl_->stats.reset();
}

bool RiskCheck::is_running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

// ==============================================================================
// 核心校验：顺势信号
// ==============================================================================
Result<RiskValidation> RiskCheck::validate_signal(const Signal& signal) {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskCheck 未启动");
    }

    impl_->stats.total_checks.fetch_add(1, std::memory_order_relaxed);

    try {
        const auto params = impl_->deps.params_provider();
        const double risk_pct = params.risk_per_trade;

        auto result = impl_->validate_core(signal, risk_pct, false);

        if (result.is_ok() && result.value().passed) {
            impl_->stats.passed_checks.fetch_add(
                1, std::memory_order_relaxed);
        }

        return result;

    } catch (const std::exception& e) {
        QUANT_LOG_ERROR("RiskCheck::validate_signal 异常: {}", e.what());
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "校验异常")
            .with_context("error", e.what());
    }
}

// ==============================================================================
// 核心校验：反向信号（风险预算更严）
// ==============================================================================
Result<RiskValidation> RiskCheck::validate_reverse_signal(
    const Signal& signal)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskCheck 未启动");
    }

    impl_->stats.total_checks.fetch_add(1, std::memory_order_relaxed);

    try {
        const auto params = impl_->deps.params_provider();
        const double risk_pct = params.reverse_risk;

        auto result = impl_->validate_core(signal, risk_pct, true);

        if (result.is_ok() && result.value().passed) {
            impl_->stats.passed_checks.fetch_add(
                1, std::memory_order_relaxed);
        }

        return result;

    } catch (const std::exception& e) {
        QUANT_LOG_ERROR("RiskCheck::validate_reverse_signal 异常: {}", e.what());
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "校验异常")
            .with_context("error", e.what());
    }
}

// ==============================================================================
// 加仓校验
// ==============================================================================
Result<RiskValidation> RiskCheck::validate_add_position(
    const Position& pos,
    double add_quantity,
    double add_price,
    double new_stop_distance,
    int add_count)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskCheck 未启动");
    }

    impl_->stats.total_checks.fetch_add(1, std::memory_order_relaxed);

    // 基础校验
    if (!finite(add_quantity) || add_quantity <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "加仓数量无效");
    }
    if (!finite(add_price) || add_price <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "加仓价格无效");
    }
    if (!finite(new_stop_distance) || new_stop_distance <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "新的止损距离无效");
    }

    if (add_count < 0 || add_count > 3) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "加仓次数无效")
            .with_context("count", std::to_string(add_count));
    }

    // 加载账户与参数
    const auto account = impl_->deps.account_provider();
    if (!account.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::RiskDailyLossLimit,
            "账户状态无效");
    }

    const auto params = impl_->deps.params_provider();

    // 加仓风险预算
    double add_risk_pct = 0.0;
    if (add_count == 0) {
        add_risk_pct = params.first_add_risk;
    } else if (add_count == 1) {
        add_risk_pct = params.second_add_risk;
    } else {
        add_risk_pct = params.third_add_risk;
    }

    RiskValidation v;
    v.risk_pct = add_risk_pct;
    v.risk_amount = account.equity * add_risk_pct;
    v.stop_distance = new_stop_distance;
    v.stop_distance_pct = safe_div(new_stop_distance, add_price, 0.0);
    v.position_size = add_quantity;
    v.position_notional = add_quantity * add_price;
    v.margin_required = safe_div(
        v.position_notional,
        static_cast<double>(account.current_leverage),
        v.position_notional);

    // 计算加仓后的总风险
    const double current_qty = pos.quantity.to_double();
    const double current_stop = pos.stop_loss.to_double();
    const double current_entry = pos.entry_price.to_double();
    const double current_stop_dist = std::abs(current_entry - current_stop);

    // 当前风险
    const double current_risk = current_stop_dist * current_qty;

    // 加仓后总风险
    const double new_total_risk =
        (current_qty + add_quantity) * new_stop_distance;

    v.risk_amount = new_total_risk;

    // 校验总风险 ≤ max_risk_per_trade
    const double total_pct = safe_div(new_total_risk, account.equity, 0.0);
    if (total_pct > params.max_risk_per_trade + kEps) {
        v.passed = false;
        v.reject_code = ErrorCode::RiskPositionLimit;
        v.reject_reason = "加仓后总风险超预算: " +
                          std::to_string(total_pct * 100) + "%";
        impl_->count_reject(v.reject_code);
        QUANT_LOG_WARN("加仓被拒绝: {}", v.reject_reason);
        return v;
    }

    // 成本覆盖
    v.cost = impl_->compute_cost(v.position_notional, false, 1);
    // 加仓的预期盈利按剩余空间估算（简化：按 stop_distance 的 1R）
    v.expected_profit = new_stop_distance * add_quantity;

    // 通过
    v.passed = true;
    v.rr_ratio = safe_div(v.expected_profit, v.risk_amount, 0.0);
    v.reject_code = ErrorCode::Ok;
    v.reject_reason = "加仓通过校验";

    impl_->stats.passed_checks.fetch_add(1, std::memory_order_relaxed);

    (void)current_risk;
    return v;
}

// ==============================================================================
// 组合风险校验
// ==============================================================================
Result<void> RiskCheck::check_portfolio_risk(
    const Symbol& new_symbol,
    Side new_side,
    double new_risk_amount)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskCheck 未启动");
    }

    if (!impl_->deps.positions_provider ||
        !impl_->deps.correlation_provider) {
        return {};  // 无组合信息，跳过
    }

    if (!finite(new_risk_amount) || new_risk_amount < 0.0) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "新风险金额无效");
    }

    const auto account = impl_->deps.account_provider();
    if (!account.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::RiskDailyLossLimit,
            "账户状态无效");
    }

    const auto params = impl_->deps.params_provider();

    // 收集持仓
    std::vector<PortfolioPosition> positions;
    try {
        positions = impl_->deps.positions_provider();
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("positions_provider 异常: {}", e.what());
        return {};
    }

    // 计算组合风险
    // portfolio_risk² = Σ Σ risk_i × risk_j × corr_ij
    std::vector<double> all_risks;
    std::vector<Symbol> all_symbols;

    for (const auto& p : positions) {
        all_risks.push_back(p.risk_amount);
        all_symbols.push_back(p.symbol);
    }

    // 加入新订单
    all_risks.push_back(new_risk_amount);
    all_symbols.push_back(new_symbol);

    const std::size_t n = all_risks.size();

    double portfolio_variance = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            double corr = (i == j) ? 1.0 : 0.0;
            if (i != j) {
                try {
                    corr = impl_->deps.correlation_provider(
                        all_symbols[i], all_symbols[j]);
                } catch (...) {
                    corr = 0.0;
                }
            }
            portfolio_variance += all_risks[i] * all_risks[j] * corr;
        }
    }

    const double portfolio_risk =
        (portfolio_variance > 0.0) ? std::sqrt(portfolio_variance) : 0.0;

    const double portfolio_pct = safe_div(portfolio_risk, account.equity, 0.0);

    if (portfolio_pct > params.max_portfolio_risk_pct + kEps) {
        return QUANT_ERR_MSG(ErrorCode::RiskPositionLimit,
            "组合风险超限")
            .with_context("portfolio_pct", std::to_string(portfolio_pct))
            .with_context("max", std::to_string(params.max_portfolio_risk_pct));
    }

    return {};
}

// ==============================================================================
// 账户健康检查
// ==============================================================================
Result<void> RiskCheck::check_account_health() const {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskCheck 未启动");
    }

    if (auto r = check_daily_loss(); r.is_err()) return r;
    if (auto r = check_drawdown(); r.is_err()) return r;

    const auto account = impl_->deps.account_provider();
    if (auto r = check_leverage(account.current_leverage); r.is_err()) {
        return r;
    }
    if (auto r = check_position_count(); r.is_err()) return r;

    return {};
}

Result<void> RiskCheck::check_daily_loss() const {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskCheck 未启动");
    }

    const auto account = impl_->deps.account_provider();
    const auto params = impl_->deps.params_provider();

    const double loss_pct = account.daily_loss_pct();
    if (loss_pct > params.max_daily_loss_pct + kEps) {
        return QUANT_ERR_MSG(ErrorCode::RiskDailyLossLimit,
            "当日亏损超限")
            .with_context("loss_pct", std::to_string(loss_pct))
            .with_context("max", std::to_string(params.max_daily_loss_pct));
    }

    return {};
}

Result<void> RiskCheck::check_drawdown() const {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskCheck 未启动");
    }

    const auto account = impl_->deps.account_provider();
    const auto params = impl_->deps.params_provider();

    const double dd_pct = account.drawdown_pct();
    if (dd_pct > params.max_drawdown_pct + kEps) {
        return QUANT_ERR_MSG(ErrorCode::RiskDrawdownLimit,
            "回撤超限")
            .with_context("drawdown", std::to_string(dd_pct))
            .with_context("max", std::to_string(params.max_drawdown_pct));
    }

    return {};
}

Result<void> RiskCheck::check_leverage(std::size_t leverage) const {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskCheck 未启动");
    }

    const auto params = impl_->deps.params_provider();

    if (leverage == 0 || leverage > params.max_leverage) {
        return QUANT_ERR_MSG(ErrorCode::RiskLeverageLimit,
            "杠杆超限")
            .with_context("leverage", std::to_string(leverage))
            .with_context("max", std::to_string(params.max_leverage));
    }

    return {};
}

Result<void> RiskCheck::check_position_count() const {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskCheck 未启动");
    }

    const auto account = impl_->deps.account_provider();
    const auto params = impl_->deps.params_provider();

    if (account.open_position_count >= params.max_concurrent_positions) {
        return QUANT_ERR_MSG(ErrorCode::RiskPositionLimit,
            "持仓数已达上限")
            .with_context("count",
                std::to_string(account.open_position_count))
            .with_context("max",
                std::to_string(params.max_concurrent_positions));
    }

    return {};
}

// ==============================================================================
// 成本计算
// ==============================================================================
CostBreakdown RiskCheck::compute_cost(
    double notional,
    bool is_maker,
    int holding_hours) const noexcept
{
    CostBreakdown cost;

    if (!impl_ || !finite(notional) || notional <= 0.0) {
        return cost;
    }

    try {
        const auto params = impl_->deps.params_provider();

        const double fee_rate = is_maker ? params.maker_fee : params.taker_fee;

        cost.entry_fee = notional * fee_rate;
        cost.exit_fee  = notional * fee_rate;
        cost.slippage  = notional * params.slippage_estimate;

        // 资金费：按 8 小时结算，holding_hours 估算
        const double funding_periods =
            static_cast<double>(std::max(holding_hours, 0)) / 8.0;
        cost.funding = notional * params.funding_rate * funding_periods;

        cost.total = cost.entry_fee + cost.exit_fee +
                     cost.slippage + cost.funding;

    } catch (...) {
        // 参数获取失败，返回零成本
        cost = CostBreakdown{};
    }

    return cost;
}

// ==============================================================================
// 参数与账户查询
// ==============================================================================
RiskParams RiskCheck::current_params() const {
    if (!impl_ || !impl_->deps.params_provider) {
        return RiskParams{};
    }
    try {
        return impl_->deps.params_provider();
    } catch (...) {
        return RiskParams{};
    }
}

AccountState RiskCheck::current_account() const {
    if (!impl_ || !impl_->deps.account_provider) {
        return AccountState{};
    }
    try {
        return impl_->deps.account_provider();
    } catch (...) {
        return AccountState{};
    }
}

// ==============================================================================
// 统计与诊断
// ==============================================================================
const RiskCheckStats& RiskCheck::stats() const noexcept {
    static const RiskCheckStats kEmpty;
    return impl_ ? impl_->stats : kEmpty;
}

std::string RiskCheck::dump() const {
    std::ostringstream oss;
    oss << "RiskCheck dump:\n";

    if (!impl_) {
        oss << "  <impl is null>\n";
        return oss.str();
    }

    oss << "  running: " << (is_running() ? "yes" : "no") << "\n";

    // 参数
    try {
        const auto p = impl_->deps.params_provider();
        oss << "  params:\n";
        oss << "    risk_per_trade: " << p.risk_per_trade << "\n";
        oss << "    reverse_risk: " << p.reverse_risk << "\n";
        oss << "    min_rr_ratio: " << p.min_rr_ratio << "\n";
        oss << "    max_leverage: " << p.max_leverage << "\n";
        oss << "    max_daily_loss_pct: " << p.max_daily_loss_pct << "\n";
        oss << "    max_drawdown_pct: " << p.max_drawdown_pct << "\n";
        oss << "    max_portfolio_risk_pct: "
            << p.max_portfolio_risk_pct << "\n";
    } catch (...) {
        oss << "  params: <unavailable>\n";
    }

    // 账户
    try {
        const auto a = impl_->deps.account_provider();
        oss << "  account:\n";
        oss << "    equity: " << a.equity << "\n";
        oss << "    peak_equity: " << a.peak_equity << "\n";
        oss << "    daily_pnl: " << a.daily_pnl << "\n";
        oss << "    daily_loss_pct: " << a.daily_loss_pct() << "\n";
        oss << "    drawdown_pct: " << a.drawdown_pct() << "\n";
        oss << "    open_positions: " << a.open_position_count << "\n";
        oss << "    leverage: " << a.current_leverage << "\n";
    } catch (...) {
        oss << "  account: <unavailable>\n";
    }

    // 统计
    const auto& s = impl_->stats;
    oss << "  stats:\n";
    oss << "    total_checks: "
        << s.total_checks.load(std::memory_order_relaxed) << "\n";
    oss << "    passed_checks: "
        << s.passed_checks.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_invalid_input: "
        << s.rejected_invalid_input.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_rr_ratio: "
        << s.rejected_rr_ratio.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_stop_distance: "
        << s.rejected_stop_distance.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_position_size: "
        << s.rejected_position_size.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_total_risk: "
        << s.rejected_total_risk.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_cost_coverage: "
        << s.rejected_cost_coverage.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_daily_loss: "
        << s.rejected_daily_loss.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_drawdown: "
        << s.rejected_drawdown.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_leverage: "
        << s.rejected_leverage.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_portfolio_risk: "
        << s.rejected_portfolio_risk.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_unknown: "
        << s.rejected_unknown.load(std::memory_order_relaxed) << "\n";

    return oss.str();
}

}  // namespace quant::strategy
