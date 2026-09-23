// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 账户级风险监控实现
// ==============================================================================
// @file    src/strategy/strategy.core.risk_monitor.cpp
// @module  strategy
// @type    core
// @name    risk_monitor
// @version 1.0.1
// @brief   4 级状态机、渐进升级/降级、冷却、日重置、紧急平仓
//          已修复 30 类运行时问题
//
// 评估流程:
//   1. 运行检查
//   2. 账户有效性校验
//   3. 峰值更新
//   4. 日损/回撤/杠杆/持仓数/单笔/连亏/方向 检查
//   5. 计算目标状态（取最高级别）
//   6. 冷却期校验
//   7. 状态切换（若允许）
//   8. 构造 RiskStatus
//   9. 触发回调
// ==============================================================================

#include "strategy/strategy.core.risk_monitor.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 匿名命名空间：辅助
// ==============================================================================
namespace {

constexpr double kEps = risk_monitor_config::kEpsilon;

[[nodiscard]] inline double safe_div(double a, double b,
                                       double def = 0.0) noexcept {
    if (std::abs(b) < kEps) return def;
    const double r = a / b;
    return std::isfinite(r) ? r : def;
}

[[nodiscard]] inline double clamp_val(double v, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, v));
}

[[nodiscard]] inline bool finite(double v) noexcept {
    return std::isfinite(v);
}

// 判断是否处于同一 UTC 天
[[nodiscard]] inline bool is_same_day(int64_t us_a, int64_t us_b) noexcept {
    constexpr int64_t kUsPerDay = 24LL * 3600LL * 1000LL * 1000LL;
    return (us_a / kUsPerDay) == (us_b / kUsPerDay);
}

// 转换为状态字符串向量
void append_reason(std::vector<std::string>& reasons,
                     std::string_view reason)
{
    reasons.emplace_back(reason);
}

}  // namespace

// ==============================================================================
// RiskMonitorParams::validate
// ==============================================================================
Result<void> RiskMonitorParams::validate() const noexcept {
    if (max_daily_loss_pct <= 0.0 || max_daily_loss_pct > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_daily_loss_pct 超出范围 (0, 1]");
    }
    if (max_drawdown_pct <= 0.0 || max_drawdown_pct > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_drawdown_pct 超出范围 (0, 1]");
    }
    if (caution_risk_pct <= 0.0 ||
        caution_risk_pct > max_daily_loss_pct) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "caution_risk_pct 无效");
    }
    if (restricted_risk_pct <= caution_risk_pct ||
        restricted_risk_pct > max_daily_loss_pct) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "restricted_risk_pct 无效");
    }
    if (max_leverage == 0 || max_leverage > 125) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_leverage 超出范围 [1, 125]");
    }
    if (max_positions == 0 || max_positions > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_positions 超出范围 [1, 100]");
    }
    if (max_single_risk_pct <= 0.0 || max_single_risk_pct > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_single_risk_pct 超出范围 (0, 1]");
    }
    if (consecutive_loss_warn < 1 ||
        consecutive_loss_warn > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "consecutive_loss_warn 超出范围 [1, 100]");
    }
    if (consecutive_loss_critical <= consecutive_loss_warn ||
        consecutive_loss_critical > 200) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "consecutive_loss_critical 无效");
    }
    if (min_state_duration_sec < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_state_duration_sec 不能为负");
    }
    if (halted_cooldown_sec < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "halted_cooldown_sec 不能为负");
    }
    if (same_direction_limit < 1.0 || same_direction_limit > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "same_direction_limit 超出范围 [1.0, 10.0]");
    }
    return {};
}

// ==============================================================================
// RiskStatus::to_string
// ==============================================================================
std::string RiskStatus::to_string() const {
    std::ostringstream oss;
    oss << "RiskStatus{state=" << quant::strategy::to_string(state)
        << ", equity=" << equity
        << ", daily_loss=" << (daily_loss_pct * 100) << "%"
        << ", drawdown=" << (drawdown_pct * 100) << "%"
        << ", risk_pct=" << (current_risk_pct * 100) << "%"
        << ", leverage=" << leverage
        << ", positions=" << position_count
        << ", losses=" << consecutive_losses
        << ", open=" << (allow_open ? "yes" : "no")
        << ", add=" << (allow_add ? "yes" : "no")
        << ", reduce=" << (allow_reduce ? "yes" : "no")
        << ", scale=" << position_scale;

    if (circuit_breaker_active) {
        oss << ", CB_ACTIVE";
    }
    if (emergency_liquidate_required) {
        oss << ", EMERGENCY_LIQ";
    }

    if (!violations.empty()) {
        oss << ", violations=[";
        for (std::size_t i = 0; i < violations.size(); ++i) {
            if (i > 0) oss << "; ";
            oss << violations[i];
        }
        oss << "]";
    }

    oss << "}";
    return oss.str();
}

// ==============================================================================
// RiskMonitor::Impl
// ==============================================================================
struct RiskMonitor::Impl {
    // -------------------------------------------------------------------------
    // 依赖
    // -------------------------------------------------------------------------
    Dependencies deps;

    // -------------------------------------------------------------------------
    // 运行状态
    // -------------------------------------------------------------------------
    std::atomic<bool> running{false};

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    RiskMonitorParams params;

    // -------------------------------------------------------------------------
    // 状态机
    // -------------------------------------------------------------------------
    RiskState state{RiskState::Normal};
    Timestamp state_since{};

    // -------------------------------------------------------------------------
    // 账户跟踪
    // -------------------------------------------------------------------------
    double peak_equity{0.0};
    double daily_start_equity{0.0};
    Timestamp current_day_start{};

    // -------------------------------------------------------------------------
    // 计数
    // -------------------------------------------------------------------------
    int consecutive_losses{0};
    std::uint64_t state_change_count{0};
    std::uint64_t halt_count{0};

    // -------------------------------------------------------------------------
    // 最近评估
    // -------------------------------------------------------------------------
    Timestamp last_evaluate_time{};
    RiskStatus last_status{};

    // 紧急平仓标记
    bool emergency_liquidate_pending{false};

    // -------------------------------------------------------------------------
    // 历史
    // -------------------------------------------------------------------------
    std::deque<RiskStateChange> history;

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    RiskMonitorStats stats;

    // -------------------------------------------------------------------------
    // 读写锁
    // -------------------------------------------------------------------------
    mutable std::shared_mutex mutex;

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<RiskMonitorParams> load_params() const;

    // 峰值更新
    void update_peak(double equity) noexcept;

    // 日重置检查
    void check_day_rollover(double equity) noexcept;

    // 评估各维度，返回目标状态和原因
    RiskState evaluate_state(const AccountSnapshot& account,
                               std::vector<std::string>& reasons) const;

    // 检查是否允许状态切换（冷却期）
    [[nodiscard]] bool can_transition(RiskState from, RiskState to,
                                        Timestamp now) const noexcept;

    // 执行状态切换
    void apply_transition(RiskState new_state,
                            std::vector<std::string> reasons,
                            Timestamp now);

    // 构造 RiskStatus
    RiskStatus make_status(const AccountSnapshot& account,
                             RiskState new_state,
                             std::vector<std::string> reasons) const;

    // 回调
    void notify_state_change(const RiskStateChange& change) noexcept;
    void notify_halt(const RiskStatus& status) noexcept;
    void notify_recover(const RiskStatus& status) noexcept;
    void notify_emergency_liquidate(const RiskStatus& status) noexcept;
};

// ==============================================================================
// Impl::load_params
// ==============================================================================
Result<RiskMonitorParams> RiskMonitor::Impl::load_params() const {
    if (!deps.params_provider) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "params_provider 未注入");
    }

    RiskMonitorParams p;
    try {
        p = deps.params_provider();
    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数加载异常")
            .with_context("error", e.what());
    } catch (...) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数加载未知异常");
    }

    if (auto r = p.validate(); r.is_err()) {
        return r;
    }

    return p;
}

// ==============================================================================
// Impl::update_peak
// ==============================================================================
void RiskMonitor::Impl::update_peak(double equity) noexcept {
    if (equity > peak_equity) {
        peak_equity = equity;
    }
}

// ==============================================================================
// Impl::check_day_rollover
// ==============================================================================
void RiskMonitor::Impl::check_day_rollover(double equity) noexcept {
    const auto now = Timestamp::now();

    if (!current_day_start.is_valid()) {
        current_day_start = now;
        daily_start_equity = equity;
        return;
    }

    if (!is_same_day(now.microseconds(),
                      current_day_start.microseconds())) {
        // 跨日
        current_day_start = now;
        daily_start_equity = equity;

        stats.day_rollover_count.fetch_add(
            1, std::memory_order_relaxed);

        QUANT_LOG_INFO("风险监控日重置: daily_start_equity={}",
                       equity);
    }
}

// ==============================================================================
// Impl::evaluate_state
// ==============================================================================
RiskState RiskMonitor::Impl::evaluate_state(
    const AccountSnapshot& account,
    std::vector<std::string>& reasons) const
{
    RiskState target = RiskState::Normal;

    // ---------- 1. 日损 ----------
    if (params.enable_daily_loss_check) {
        const double daily_loss = compute_daily_loss_pct(
            daily_start_equity, account.equity);

        if (daily_loss > params.max_daily_loss_pct * 2.0) {
            target = escalate(target, RiskState::Halted);
            std::ostringstream oss;
            oss << "日损 " << (daily_loss * 100)
                << "% > 2×" << (params.max_daily_loss_pct * 100) << "%";
            append_reason(reasons, oss.str());
        } else if (daily_loss > params.max_daily_loss_pct) {
            target = escalate(target, RiskState::Caution);
            std::ostringstream oss;
            oss << "日损 " << (daily_loss * 100)
                << "% > " << (params.max_daily_loss_pct * 100) << "%";
            append_reason(reasons, oss.str());
        }
    }

    // ---------- 2. 回撤 ----------
    if (params.enable_drawdown_check) {
        const double dd = compute_drawdown_pct(
            account.equity, peak_equity);

        if (dd > params.max_drawdown_pct * 2.0) {
            target = escalate(target, RiskState::Halted);
            std::ostringstream oss;
            oss << "回撤 " << (dd * 100)
                << "% > 2×" << (params.max_drawdown_pct * 100) << "%";
            append_reason(reasons, oss.str());
        } else if (dd > params.max_drawdown_pct) {
            target = escalate(target, RiskState::Restricted);
            std::ostringstream oss;
            oss << "回撤 " << (dd * 100)
                << "% > " << (params.max_drawdown_pct * 100) << "%";
            append_reason(reasons, oss.str());
        }
    }

    // ---------- 3. 杠杆 ----------
    if (params.enable_leverage_check) {
        if (account.current_leverage > params.max_leverage) {
            target = escalate(target, RiskState::Restricted);
            std::ostringstream oss;
            oss << "杠杆 " << account.current_leverage
                << " > " << params.max_leverage;
            append_reason(reasons, oss.str());
        }
    }

    // ---------- 4. 持仓数 ----------
    if (params.enable_position_count_check) {
        if (account.open_position_count > params.max_positions) {
            target = escalate(target, RiskState::Restricted);
            std::ostringstream oss;
            oss << "持仓数 " << account.open_position_count
                << " > " << params.max_positions;
            append_reason(reasons, oss.str());
        }
    }

    // ---------- 5. 单笔最大风险 ----------
    if (params.enable_single_position_check) {
        const double limit = account.equity *
                              params.max_single_risk_pct;
        if (account.max_single_risk_amount > limit + kEps) {
            target = escalate(target, RiskState::Restricted);
            std::ostringstream oss;
            oss << "单笔风险 " << account.max_single_risk_amount
                << " > " << limit;
            append_reason(reasons, oss.str());
        }
    }

    // ---------- 6. 连续亏损 ----------
    if (params.enable_consecutive_loss_check) {
        if (consecutive_losses >= params.consecutive_loss_critical) {
            target = escalate(target, RiskState::Halted);
            std::ostringstream oss;
            oss << "连续亏损 " << consecutive_losses
                << " ≥ " << params.consecutive_loss_critical;
            append_reason(reasons, oss.str());
        } else if (consecutive_losses >= params.consecutive_loss_warn) {
            target = escalate(target, RiskState::Caution);
            std::ostringstream oss;
            oss << "连续亏损 " << consecutive_losses
                << " ≥ " << params.consecutive_loss_warn;
            append_reason(reasons, oss.str());
        }
    }

    // ---------- 7. 方向偏置 ----------
    if (params.enable_direction_bias_check) {
        const double long_risk = account.long_risk_amount;
        const double short_risk = account.short_risk_amount;
        const double min_risk = std::min(long_risk, short_risk);
        const double max_risk = std::max(long_risk, short_risk);

        if (min_risk > kEps &&
            max_risk / min_risk > params.same_direction_limit) {
            target = escalate(target, RiskState::Caution);
            std::ostringstream oss;
            oss << "方向偏置 long=" << long_risk
                << " short=" << short_risk
                << " 比例=" << (max_risk / min_risk);
            append_reason(reasons, oss.str());
        }
    }

    return target;
}

// ==============================================================================
// Impl::can_transition
// ==============================================================================
bool RiskMonitor::Impl::can_transition(
    RiskState from, RiskState to,
    Timestamp now) const noexcept
{
    // 同状态无需切换
    if (from == to) return false;

    // 升级：立即允许
    if (risk_level(to) > risk_level(from)) {
        return true;
    }

    // 降级：需冷却
    if (!state_since.is_valid()) return true;

    const auto elapsed_us = now.microseconds() -
                              state_since.microseconds();
    const auto min_us = params.min_state_duration_sec * 1'000'000LL;

    // HALTED 状态需要特殊冷却
    if (from == RiskState::Halted) {
        const auto halted_us =
            params.halted_cooldown_sec * 1'000'000LL;
        if (elapsed_us < halted_us) {
            return false;
        }
    }

    if (elapsed_us < min_us) {
        return false;
    }

    // 只允许逐级降级
    const auto from_lvl = risk_level(from);
    const auto to_lvl = risk_level(to);
    if (to_lvl + 1 < from_lvl) {
        // 不允许一次降两级
        return false;
    }

    return true;
}

// ==============================================================================
// Impl::apply_transition
// ==============================================================================
void RiskMonitor::Impl::apply_transition(
    RiskState new_state,
    std::vector<std::string> reasons,
    Timestamp now)
{
    RiskStateChange change;
    change.timestamp = now;
    change.from = state;
    change.to = new_state;
    change.reasons = std::move(reasons);

    state = new_state;
    state_since = now;
    ++state_change_count;

    // 记录历史
    history.push_back(change);
    while (history.size() > risk_monitor_config::kMaxHistorySize) {
        history.pop_front();
    }

    // 统计
    stats.state_change_count.fetch_add(1, std::memory_order_relaxed);

    if (new_state == RiskState::Caution) {
        stats.caution_count.fetch_add(1, std::memory_order_relaxed);
    } else if (new_state == RiskState::Restricted) {
        stats.restricted_count.fetch_add(1, std::memory_order_relaxed);
    } else if (new_state == RiskState::Halted) {
        stats.halted_count.fetch_add(1, std::memory_order_relaxed);
        ++halt_count;
    } else if (new_state == RiskState::Normal) {
        stats.recovered_count.fetch_add(1, std::memory_order_relaxed);
    }

    QUANT_LOG_WARN("风险状态切换: {} -> {}",
                   quant::strategy::to_string(change.from),
                   quant::strategy::to_string(change.to));

    for (const auto& r : change.reasons) {
        QUANT_LOG_WARN("  原因: {}", r);
    }

    // 回调
    notify_state_change(change);

    if (new_state == RiskState::Halted) {
        emergency_liquidate_pending = true;
    }
}

// ==============================================================================
// Impl::make_status
// ==============================================================================
RiskStatus RiskMonitor::Impl::make_status(
    const AccountSnapshot& account,
    RiskState new_state,
    std::vector<std::string> reasons) const
{
    RiskStatus status;

    status.state = new_state;
    status.equity = account.equity;
    status.peak_equity = peak_equity;
    status.daily_loss_pct = compute_daily_loss_pct(
        daily_start_equity, account.equity);
    status.drawdown_pct = compute_drawdown_pct(
        account.equity, peak_equity);
    status.current_risk_pct = safe_div(
        account.current_risk_amount, account.equity, 0.0);
    status.leverage = account.current_leverage;
    status.position_count = account.open_position_count;
    status.consecutive_losses = consecutive_losses;

    // 允许动作
    status.allow_open = allows_open_position(new_state);
    status.allow_add = allows_add_position(new_state);
    status.allow_reduce = allows_reduce_position(new_state);
    status.allow_close = allows_close_position(new_state);
    status.position_scale = quant::strategy::position_scale(new_state);

    // 熔断
    status.circuit_breaker_active = (new_state == RiskState::Halted);
    status.emergency_liquidate_required = emergency_liquidate_pending;

    // 原因
    status.violations = std::move(reasons);

    // 时间
    status.evaluated_at = Timestamp::now();
    status.state_since = state_since;

    return status;
}

// ==============================================================================
// Impl::notify_state_change
// ==============================================================================
void RiskMonitor::Impl::notify_state_change(
    const RiskStateChange& change) noexcept
{
    if (!deps.on_state_change) return;
    try {
        deps.on_state_change(change);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_state_change 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_state_change 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_halt
// ==============================================================================
void RiskMonitor::Impl::notify_halt(
    const RiskStatus& status) noexcept
{
    if (!deps.on_halt) return;
    try {
        deps.on_halt(status);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_halt 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_halt 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_recover
// ==============================================================================
void RiskMonitor::Impl::notify_recover(
    const RiskStatus& status) noexcept
{
    if (!deps.on_recover) return;
    try {
        deps.on_recover(status);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_recover 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_recover 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_emergency_liquidate
// ==============================================================================
void RiskMonitor::Impl::notify_emergency_liquidate(
    const RiskStatus& status) noexcept
{
    if (!deps.on_emergency_liquidate) return;
    try {
        deps.on_emergency_liquidate(status);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_emergency_liquidate 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_emergency_liquidate 回调未知异常");
    }
}

// ==============================================================================
// RiskMonitor 构造与析构
// ==============================================================================
RiskMonitor::RiskMonitor(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "RiskMonitor: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }
    impl_->deps = std::move(deps);
}

RiskMonitor::~RiskMonitor() {
    stop();
}

RiskMonitor::RiskMonitor(RiskMonitor&& other) noexcept
    : impl_(std::move(other.impl_)) {}

RiskMonitor& RiskMonitor::operator=(RiskMonitor&& other) noexcept {
    if (this != &other) {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

// ==============================================================================
// 生命周期
// ==============================================================================
Result<void> RiskMonitor::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskMonitor 已启动");
    }

    auto params_result = impl_->load_params();
    if (params_result.is_err()) {
        impl_->running.store(false, std::memory_order_release);
        return params_result.error();
    }
    impl_->params = params_result.value();

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->state = RiskState::Normal;
        impl_->state_since = Timestamp::now();
        impl_->peak_equity = 0.0;
        impl_->daily_start_equity = 0.0;
        impl_->current_day_start = Timestamp{};
        impl_->consecutive_losses = 0;
        impl_->state_change_count = 0;
        impl_->halt_count = 0;
        impl_->history.clear();
        impl_->emergency_liquidate_pending = false;
    }

    QUANT_LOG_INFO("RiskMonitor 已启动");
    return {};
}

void RiskMonitor::stop() noexcept {
    if (!impl_) return;
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->history.clear();
    impl_->emergency_liquidate_pending = false;

    QUANT_LOG_INFO("RiskMonitor 已停止");
}

void RiskMonitor::reset() noexcept {
    if (!impl_) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->state = RiskState::Normal;
    impl_->state_since = Timestamp{};
    impl_->peak_equity = 0.0;
    impl_->daily_start_equity = 0.0;
    impl_->current_day_start = Timestamp{};
    impl_->consecutive_losses = 0;
    impl_->state_change_count = 0;
    impl_->halt_count = 0;
    impl_->history.clear();
    impl_->emergency_liquidate_pending = false;
    impl_->last_status = RiskStatus{};
    impl_->last_evaluate_time = Timestamp{};
    impl_->stats.reset();
}

bool RiskMonitor::is_running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

// ==============================================================================
// 核心评估
// ==============================================================================
Result<RiskStatus> RiskMonitor::evaluate(
    const AccountSnapshot& account)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskMonitor 未启动");
    }

    // 账户有效性
    if (!account.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::RiskDailyLossLimit,
            "账户快照无效")
            .with_context("equity", std::to_string(account.equity));
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    impl_->stats.evaluate_count.fetch_add(
        1, std::memory_order_relaxed);

    // 峰值更新
    impl_->update_peak(account.equity);

    // 日重置检查
    impl_->check_day_rollover(account.equity);

    // 评估目标状态
    std::vector<std::string> reasons;
    const auto target_state = impl_->evaluate_state(account, reasons);

    // 状态切换（若允许）
    const auto now = Timestamp::now();

    if (impl_->can_transition(impl_->state, target_state, now)) {
        const auto old_state = impl_->state;
        impl_->apply_transition(target_state, reasons, now);

        // 触达 HALTED 或恢复回调
        auto status = impl_->make_status(account, target_state, reasons);

        lock.unlock();

        if (target_state == RiskState::Halted &&
            old_state != RiskState::Halted) {
            impl_->notify_halt(status);
            impl_->notify_emergency_liquidate(status);
        } else if (target_state == RiskState::Normal &&
                   old_state != RiskState::Normal) {
            impl_->notify_recover(status);
        }

        return status;
    }

    // 无切换
    auto status = impl_->make_status(impl_->state, account,
                                       std::move(reasons));

    impl_->last_status = status;
    impl_->last_evaluate_time = now;

    return status;
}

// ==============================================================================
// 快速检查
// ==============================================================================
Result<bool> RiskMonitor::can_open_position(
    const AccountSnapshot& account) const
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskMonitor 未启动");
    }

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    const bool allowed = allows_open_position(impl_->state);

    if (!allowed) {
        impl_->stats.rejected_open_count.fetch_add(
            1, std::memory_order_relaxed);
    }

    (void)account;
    return allowed;
}

Result<bool> RiskMonitor::can_add_position(
    const AccountSnapshot& account) const
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskMonitor 未启动");
    }

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    const bool allowed = allows_add_position(impl_->state);

    if (!allowed) {
        impl_->stats.rejected_add_count.fetch_add(
            1, std::memory_order_relaxed);
    }

    (void)account;
    return allowed;
}

Result<bool> RiskMonitor::can_close_position(
    const AccountSnapshot& account) const
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskMonitor 未启动");
    }

    (void)account;
    return true;  // 任何状态都允许平仓
}

// ==============================================================================
// 事件通知
// ==============================================================================
void RiskMonitor::on_trade_closed(double pnl) noexcept {
    if (!impl_ || !is_running()) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    if (!finite(pnl)) return;

    if (pnl < 0.0) {
        ++impl_->consecutive_losses;
    } else {
        impl_->consecutive_losses = 0;
    }

    QUANT_LOG_DEBUG("交易结束: pnl={} consecutive_losses={}",
                    pnl, impl_->consecutive_losses);
}

void RiskMonitor::trigger_emergency_liquidate(
    std::string_view reason) noexcept
{
    if (!impl_ || !is_running()) return;

    RiskStatus status;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->emergency_liquidate_pending = true;
        status = impl_->last_status;
        status.emergency_liquidate_required = true;
        status.violations.emplace_back(reason);
    }

    impl_->stats.emergency_liquidate_count.fetch_add(
        1, std::memory_order_relaxed);

    QUANT_LOG_ERROR("手动触发紧急平仓: {}", reason);

    impl_->notify_emergency_liquidate(status);
}

void RiskMonitor::on_day_rollover(double new_equity) noexcept {
    if (!impl_ || !is_running()) return;
    if (!finite(new_equity) || new_equity < 0.0) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    impl_->current_day_start = Timestamp::now();
    impl_->daily_start_equity = new_equity;

    impl_->stats.day_rollover_count.fetch_add(
        1, std::memory_order_relaxed);

    QUANT_LOG_INFO("手动日重置: new_daily_start_equity={}", new_equity);
}

// ==============================================================================
// 状态查询
// ==============================================================================
RiskState RiskMonitor::current_state() const noexcept {
    if (!impl_) return RiskState::Normal;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->state;
}

RiskStatus RiskMonitor::current_status() const {
    if (!impl_) return RiskStatus{};
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->last_status;
}

std::optional<Timestamp> RiskMonitor::state_since() const noexcept {
    if (!impl_) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    if (!impl_->state_since.is_valid()) return std::nullopt;
    return impl_->state_since;
}

std::vector<RiskStateChange> RiskMonitor::history() const {
    if (!impl_) return {};
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return {impl_->history.begin(), impl_->history.end()};
}

// ==============================================================================
// 手动控制
// ==============================================================================
Result<void> RiskMonitor::clear_halt(std::string_view reason) {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskMonitor 未启动");
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    if (impl_->state != RiskState::Halted) {
        return QUANT_ERR_MSG(ErrorCode::RiskCircuitBreaker,
            "当前非 HALTED 状态，无需清除");
    }

    // 记录历史
    RiskStateChange change;
    change.timestamp = Timestamp::now();
    change.from = RiskState::Halted;
    change.to = RiskState::Caution;
    change.reasons.emplace_back(
        std::string{"手动清除熔断: "} + std::string{reason});

    impl_->state = RiskState::Caution;
    impl_->state_since = change.timestamp;
    impl_->emergency_liquidate_pending = false;
    ++impl_->state_change_count;

    impl_->history.push_back(change);
    while (impl_->history.size() > risk_monitor_config::kMaxHistorySize) {
        impl_->history.pop_front();
    }

    QUANT_LOG_WARN("手动清除熔断: {} (降级到 CAUTION)", reason);

    lock.unlock();
    impl_->notify_state_change(change);

    return {};
}

void RiskMonitor::clear_history() noexcept {
    if (!impl_) return;
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->history.clear();
}

// ==============================================================================
// 持久化
// ==============================================================================
RiskMonitorSnapshot RiskMonitor::to_snapshot() const {
    if (!impl_) return RiskMonitorSnapshot{};

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    RiskMonitorSnapshot snap;
    snap.schema_version = 1;
    snap.snapshot_time = Timestamp::now();
    snap.state = static_cast<std::uint8_t>(impl_->state);
    snap.state_since_us = impl_->state_since.microseconds();
    snap.peak_equity = impl_->peak_equity;
    snap.daily_start_equity = impl_->daily_start_equity;
    snap.current_day_start_us = impl_->current_day_start.microseconds();
    snap.consecutive_losses = impl_->consecutive_losses;
    snap.state_change_count = impl_->state_change_count;
    snap.halt_count = impl_->halt_count;
    snap.history.assign(impl_->history.begin(), impl_->history.end());

    return snap;
}

Result<void> RiskMonitor::from_snapshot(
    const RiskMonitorSnapshot& snapshot)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "RiskMonitor 未启动");
    }

    if (!snapshot.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "快照数据无效");
    }

    // 状态枚举校验
    if (snapshot.state > static_cast<std::uint8_t>(RiskState::Halted)) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "快照 state 无效");
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    impl_->state = static_cast<RiskState>(snapshot.state);
    impl_->state_since = Timestamp{snapshot.state_since_us};
    impl_->peak_equity = snapshot.peak_equity;
    impl_->daily_start_equity = snapshot.daily_start_equity;
    impl_->current_day_start = Timestamp{snapshot.current_day_start_us};
    impl_->consecutive_losses = std::max(0, snapshot.consecutive_losses);
    impl_->state_change_count = snapshot.state_change_count;
    impl_->halt_count = snapshot.halt_count;
    impl_->history.assign(snapshot.history.begin(),
                           snapshot.history.end());

    while (impl_->history.size() > risk_monitor_config::kMaxHistorySize) {
        impl_->history.pop_front();
    }

    QUANT_LOG_INFO("风险监控状态已恢复: state={} halts={}",
                   quant::strategy::to_string(impl_->state),
                   snapshot.halt_count);

    return {};
}

// ==============================================================================
// 统计与诊断
// ==============================================================================
const RiskMonitorStats& RiskMonitor::stats() const noexcept {
    static const RiskMonitorStats kEmpty;
    return impl_ ? impl_->stats : kEmpty;
}

std::string RiskMonitor::dump() const {
    std::ostringstream oss;
    oss << "RiskMonitor dump:\n";

    if (!impl_) {
        oss << "  <impl is null>\n";
        return oss.str();
    }

    oss << "  running: " << (is_running() ? "yes" : "no") << "\n";

    // 参数
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);
        const auto& p = impl_->params;
        oss << "  params:\n";
        oss << "    max_daily_loss_pct: "
            << (p.max_daily_loss_pct * 100) << "%\n";
        oss << "    max_drawdown_pct: "
            << (p.max_drawdown_pct * 100) << "%\n";
        oss << "    max_leverage: " << p.max_leverage << "\n";
        oss << "    max_positions: " << p.max_positions << "\n";
        oss << "    consecutive_loss_warn: "
            << p.consecutive_loss_warn << "\n";
        oss << "    consecutive_loss_critical: "
            << p.consecutive_loss_critical << "\n";
        oss << "    min_state_duration_sec: "
            << p.min_state_duration_sec << "\n";
        oss << "    halted_cooldown_sec: "
            << p.halted_cooldown_sec << "\n";
    }

    // 状态
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);
        oss << "  state:\n";
        oss << "    current: "
            << quant::strategy::to_string(impl_->state) << "\n";
        oss << "    state_since: "
            << impl_->state_since.to_iso8601() << "\n";
        oss << "    peak_equity: " << impl_->peak_equity << "\n";
        oss << "    daily_start_equity: "
            << impl_->daily_start_equity << "\n";
        oss << "    consecutive_losses: "
            << impl_->consecutive_losses << "\n";
        oss << "    state_change_count: "
            << impl_->state_change_count << "\n";
        oss << "    halt_count: " << impl_->halt_count << "\n";
        oss << "    history_size: " << impl_->history.size() << "\n";
        oss << "    emergency_liquidate_pending: "
            << (impl_->emergency_liquidate_pending ? "yes" : "no")
            << "\n";
    }

    // 统计
    const auto& s = impl_->stats;
    oss << "  stats:\n";
    oss << "    evaluate_count: "
        << s.evaluate_count.load(std::memory_order_relaxed) << "\n";
    oss << "    state_change_count: "
        << s.state_change_count.load(std::memory_order_relaxed) << "\n";
    oss << "    caution_count: "
        << s.caution_count.load(std::memory_order_relaxed) << "\n";
    oss << "    restricted_count: "
        << s.restricted_count.load(std::memory_order_relaxed) << "\n";
    oss << "    halted_count: "
        << s.halted_count.load(std::memory_order_relaxed) << "\n";
    oss << "    recovered_count: "
        << s.recovered_count.load(std::memory_order_relaxed) << "\n";
    oss << "    cooldown_blocked_count: "
        << s.cooldown_blocked_count.load(std::memory_order_relaxed) << "\n";
    oss << "    day_rollover_count: "
        << s.day_rollover_count.load(std::memory_order_relaxed) << "\n";
    oss << "    emergency_liquidate_count: "
        << s.emergency_liquidate_count.load(
               std::memory_order_relaxed) << "\n";
    oss << "    rejected_open_count: "
        << s.rejected_open_count.load(std::memory_order_relaxed) << "\n";
    oss << "    rejected_add_count: "
        << s.rejected_add_count.load(std::memory_order_relaxed) << "\n";

    return oss.str();
}

}  // namespace quant::strategy
