// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 盈亏比第一原则强制校验
// ==============================================================================
// @file    src/strategy/strategy.core.risk_check.hpp
// @module  strategy
// @type    core
// @name    risk_check
// @version 1.0.1
// @brief   盈亏比第一原则的硬约束实现，任何信号必须通过本模块校验
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 盈亏比第一：RR < 1.5 硬拒绝，不可绕过
//   - 总风险预算：首仓 + 加仓 ≤ 2% 权益
//   - 反向单更严：1.0%~1.2% 独立预算
//   - 组合风险：跨品种相关性约束 ≤ 3%
//   - 成本过滤：预期盈利 > 3× (手续费 + 滑点 + 资金费)
//   - 全字段校验：NaN/Inf/方向/范围全部检查
//   - 完整错误码：Result<T> 携带具体拒绝原因
//   - 可配置：所有阈值从 ConfigManager 注入
//   - 可观测：统计计数器 + dump()
//
// 校验流程:
//   1. 输入有效性（非空、非 NaN、方向一致）
//   2. 止损距离（≥ 最小阈值，≤ 最大百分比）
//   3. 盈亏比（≥ 1.5，≤ 上限）
//   4. 仓位计算（固定风险金额反推）
//   5. 数量有效性（> 0，≥ 交易所最小数量）
//   6. 总风险预算（≤ 2%）
//   7. 成本覆盖（预期盈利 > 3× 成本）
//   8. 账户风控（日亏损、回撤、杠杆）
//   9. 组合风险（相关性约束）
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_RISK_CHECK_HPP
#define QUANT_STRATEGY_CORE_RISK_CHECK_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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

namespace quant::strategy {

// ==============================================================================
// 常量
// ==============================================================================
namespace risk_config {

// 盈亏比
inline constexpr double kMinRrRatio           = 1.5;    // 硬约束下限
inline constexpr double kMaxRrRatio           = 50.0;   // 上限（防数据错误）

// 风险预算（权益占比）
inline constexpr double kDefaultRiskPerTrade  = 0.02;   // 2%
inline constexpr double kMaxRiskPerTrade      = 0.02;   // 上限
inline constexpr double kMinRiskPerTrade      = 0.001;  // 下限 0.1%

// 反向单风险
inline constexpr double kDefaultReverseRisk    = 0.012; // 1.2%
inline constexpr double kMaxReverseRisk        = 0.012;

// 加仓风险分配（总和 = 2%）
inline constexpr double kFirstAddRisk         = 0.010;  // 首仓 1.0%
inline constexpr double kSecondAddRisk        = 0.006;  // 一次加仓 0.6%
inline constexpr double kThirdAddRisk         = 0.004;  // 二次加仓 0.4%

// 止损距离
inline constexpr double kMinStopDistancePct   = 0.0005; // 0.05%（防止过近）
inline constexpr double kMaxStopDistancePct   = 0.10;   // 10%（防止过远）

// 成本
inline constexpr double kMinCostCoverageRatio = 3.0;    // 预期盈利 > 3× 成本

// 账户风控
inline constexpr double kMaxDailyLossPct      = 0.05;   // 5%
inline constexpr double kMaxDrawdownPct       = 0.15;   // 15%
inline constexpr std::size_t kMaxConcurrentPositions = 3;
inline constexpr std::size_t kMaxLeverage     = 20;

// 组合风险
inline constexpr double kMaxPortfolioRiskPct  = 0.03;   // 3%

// 浮点保护
inline constexpr double kEpsilon              = 1e-9;

}  // namespace risk_config

// ==============================================================================
// 风险参数（从 ConfigManager 加载）
// ==============================================================================
struct RiskParams {
    // 单笔风险
    double risk_per_trade{risk_config::kDefaultRiskPerTrade};
    double max_risk_per_trade{risk_config::kMaxRiskPerTrade};
    double min_risk_per_trade{risk_config::kMinRiskPerTrade};

    // 反向单风险
    double reverse_risk{risk_config::kDefaultReverseRisk};
    double max_reverse_risk{risk_config::kMaxReverseRisk};

    // 加仓风险分配
    double first_add_risk{risk_config::kFirstAddRisk};
    double second_add_risk{risk_config::kSecondAddRisk};
    double third_add_risk{risk_config::kThirdAddRisk};

    // 盈亏比
    double min_rr_ratio{risk_config::kMinRrRatio};
    double max_rr_ratio{risk_config::kMaxRrRatio};

    // 止损距离
    double min_stop_distance_pct{risk_config::kMinStopDistancePct};
    double max_stop_distance_pct{risk_config::kMaxStopDistancePct};

    // 成本
    double maker_fee{0.0002};        // 0.02%
    double taker_fee{0.0004};        // 0.04%
    double slippage_estimate{0.0002}; // 0.02%
    double funding_rate{0.0001};     // 0.01% / 8h
    double min_cost_coverage_ratio{risk_config::kMinCostCoverageRatio};

    // 账户风控
    double max_daily_loss_pct{risk_config::kMaxDailyLossPct};
    double max_drawdown_pct{risk_config::kMaxDrawdownPct};
    std::size_t max_concurrent_positions{
        risk_config::kMaxConcurrentPositions};
    std::size_t max_leverage{risk_config::kMaxLeverage};

    // 组合风险
    double max_portfolio_risk_pct{risk_config::kMaxPortfolioRiskPct};

    // 校验参数有效性
    [[nodiscard]] Result<void> validate() const noexcept;
};

// ==============================================================================
// 账户状态（用于风控）
// ==============================================================================
struct AccountState {
    double equity{0.0};                  // 当前权益
    double peak_equity{0.0};             // 历史峰值权益
    double daily_pnl{0.0};               // 当日盈亏
    double daily_start_equity{0.0};      // 当日开盘权益
    std::size_t open_position_count{0};  // 当前持仓数
    std::size_t current_leverage{1};     // 当前杠杆

    // 计算回撤百分比
    [[nodiscard]] double drawdown_pct() const noexcept;

    // 计算日亏损百分比
    [[nodiscard]] double daily_loss_pct() const noexcept;

    [[nodiscard]] bool is_valid() const noexcept {
        return equity > 0.0 && std::isfinite(equity);
    }
};

// ==============================================================================
// 成本模型
// ==============================================================================
struct CostBreakdown {
    double entry_fee{0.0};       // 开仓手续费
    double exit_fee{0.0};        // 平仓手续费
    double slippage{0.0};        // 滑点
    double funding{0.0};         // 资金费（估算）
    double total{0.0};           // 总成本

    [[nodiscard]] bool is_valid() const noexcept {
        return std::isfinite(total) && total >= 0.0;
    }
};

// ==============================================================================
// 风险校验结果
// ==============================================================================
struct RiskValidation {
    // 校验是否通过
    bool passed{false};

    // 盈亏比
    double rr_ratio{0.0};
    double rr_ratio_tp1{0.0};    // 对 TP1 的盈亏比
    double rr_ratio_tp2{0.0};    // 对 TP2 的盈亏比

    // 风险
    double risk_amount{0.0};     // 风险金额（USDT）
    double risk_pct{0.0};        // 风险占权益比例
    double stop_distance{0.0};   // 止损距离（价格）
    double stop_distance_pct{0.0}; // 止损距离占价格比例

    // 仓位
    double position_size{0.0};   // 建议仓位（币数量）
    double position_notional{0.0}; // 仓位名义价值（USDT）
    double margin_required{0.0};  // 所需保证金

    // 成本
    CostBreakdown cost{};

    // 预期盈利
    double expected_profit{0.0};       // 预期盈利（TP1 命中）
    double cost_coverage_ratio{0.0};   // 预期盈利 / 成本

    // 拒绝原因（passed = false 时）
    ErrorCode reject_code{ErrorCode::Ok};
    std::string reject_reason;

    [[nodiscard]] bool is_valid() const noexcept {
        return std::isfinite(rr_ratio) &&
               std::isfinite(position_size) &&
               std::isfinite(risk_amount);
    }
};

// ==============================================================================
// 组合风险输入
// ==============================================================================
struct PortfolioPosition {
    Symbol symbol;
    Side side{Side::UNKNOWN};
    double risk_amount{0.0};      // 该持仓的风险金额
    double notional{0.0};         // 名义价值
};

// ==============================================================================
// 风险统计
// ==============================================================================
struct RiskCheckStats {
    std::atomic<std::uint64_t> total_checks{0};
    std::atomic<std::uint64_t> passed_checks{0};
    std::atomic<std::uint64_t> rejected_invalid_input{0};
    std::atomic<std::uint64_t> rejected_rr_ratio{0};
    std::atomic<std::uint64_t> rejected_stop_distance{0};
    std::atomic<std::uint64_t> rejected_position_size{0};
    std::atomic<std::uint64_t> rejected_total_risk{0};
    std::atomic<std::uint64_t> rejected_cost_coverage{0};
    std::atomic<std::uint64_t> rejected_daily_loss{0};
    std::atomic<std::uint64_t> rejected_drawdown{0};
    std::atomic<std::uint64_t> rejected_leverage{0};
    std::atomic<std::uint64_t> rejected_portfolio_risk{0};
    std::atomic<std::uint64_t> rejected_unknown{0};

    void reset() noexcept {
        total_checks.store(0, std::memory_order_relaxed);
        passed_checks.store(0, std::memory_order_relaxed);
        rejected_invalid_input.store(0, std::memory_order_relaxed);
        rejected_rr_ratio.store(0, std::memory_order_relaxed);
        rejected_stop_distance.store(0, std::memory_order_relaxed);
        rejected_position_size.store(0, std::memory_order_relaxed);
        rejected_total_risk.store(0, std::memory_order_relaxed);
        rejected_cost_coverage.store(0, std::memory_order_relaxed);
        rejected_daily_loss.store(0, std::memory_order_relaxed);
        rejected_drawdown.store(0, std::memory_order_relaxed);
        rejected_leverage.store(0, std::memory_order_relaxed);
        rejected_portfolio_risk.store(0, std::memory_order_relaxed);
        rejected_unknown.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// 风险校验器
// ==============================================================================
class RiskCheck {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 参数提供者（必需）
        std::function<RiskParams()> params_provider;

        // 账户状态提供者（必需）
        std::function<AccountState()> account_provider;

        // 持仓提供者（可选，用于组合风险）
        std::function<std::vector<PortfolioPosition>()> positions_provider;

        // 相关性提供者（可选，用于组合风险）
        std::function<double(const Symbol&, const Symbol&)>
            correlation_provider;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit RiskCheck(Dependencies deps);
    ~RiskCheck();

    RiskCheck(const RiskCheck&) = delete;
    RiskCheck& operator=(const RiskCheck&) = delete;
    RiskCheck(RiskCheck&&) noexcept;
    RiskCheck& operator=(RiskCheck&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 核心校验：顺势信号
    // -------------------------------------------------------------------------
    // 校验开仓信号，返回完整的风险参数
    [[nodiscard]] Result<RiskValidation> validate_signal(
        const Signal& signal);

    // -------------------------------------------------------------------------
    // 核心校验：反向信号（风险预算更严）
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<RiskValidation> validate_reverse_signal(
        const Signal& signal);

    // -------------------------------------------------------------------------
    // 加仓校验
    // -------------------------------------------------------------------------
    // 校验加仓后总风险是否超预算
    [[nodiscard]] Result<RiskValidation> validate_add_position(
        const Position& pos,
        double add_quantity,
        double add_price,
        double new_stop_distance,
        int add_count);

    // -------------------------------------------------------------------------
    // 组合风险校验
    // -------------------------------------------------------------------------
    // 校验新订单加入后组合风险是否超限
    [[nodiscard]] Result<void> check_portfolio_risk(
        const Symbol& new_symbol,
        Side new_side,
        double new_risk_amount);

    // -------------------------------------------------------------------------
    // 账户级风控
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void> check_account_health() const;
    [[nodiscard]] Result<void> check_daily_loss() const;
    [[nodiscard]] Result<void> check_drawdown() const;
    [[nodiscard]] Result<void> check_leverage(std::size_t leverage) const;
    [[nodiscard]] Result<void> check_position_count() const;

    // -------------------------------------------------------------------------
    // 成本计算
    // -------------------------------------------------------------------------
    [[nodiscard]] CostBreakdown compute_cost(
        double notional,
        bool is_maker,
        int holding_hours = 0) const noexcept;

    // -------------------------------------------------------------------------
    // 参数查询
    // -------------------------------------------------------------------------
    [[nodiscard]] RiskParams current_params() const;
    [[nodiscard]] AccountState current_account() const;

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const RiskCheckStats& stats() const noexcept;
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

// 计算盈亏比
[[nodiscard]] inline double compute_rr(double entry, double stop,
                                         double target) noexcept {
    const double risk = std::abs(entry - stop);
    if (risk < risk_config::kEpsilon) return 0.0;
    const double reward = std::abs(target - entry);
    const double rr = reward / risk;
    return std::isfinite(rr) ? rr : 0.0;
}

// 判断是否为有效价格
[[nodiscard]] inline bool is_valid_price(const Price& p) noexcept {
    const double v = p.to_double();
    return std::isfinite(v) && v > 0.0;
}

// 判断是否为有效数量
[[nodiscard]] inline bool is_valid_quantity(const Quantity& q) noexcept {
    const double v = q.to_double();
    return std::isfinite(v) && v > 0.0;
}

// ==============================================================================
// 便捷校验宏
// ==============================================================================
#define QUANT_RISK_CHECK(expr)                          \
    do {                                                \
        auto _rc = (expr);                              \
        if (_rc.is_err()) return _rc;                   \
    } while (0)

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_RISK_CHECK_HPP
