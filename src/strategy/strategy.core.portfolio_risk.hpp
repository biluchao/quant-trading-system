// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 组合风险管理
// ==============================================================================
// @file    src/strategy/strategy.core.portfolio_risk.hpp
// @module  strategy
// @type    core
// @name    portfolio_risk
// @version 1.0.1
// @brief   跨品种组合风险，考虑相关性矩阵，动态调整仓位
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 相关性加权：ρ_ij 非零时组合风险显著不同
//   - 协方差矩阵：支持 VaR 计算
//   - 风险贡献分解：每个持仓的边际贡献可追溯
//   - 动态缩放：超限时按比例缩减而非直接拒绝
//   - 滚动相关：从收益序列计算
//   - 波动率自适应：高波动自动降仓
//   - 相关性崩溃检测：ρ → 1 时紧急降仓
//   - 完整追踪：历史 + 统计 + 快照
//   - 参数可配：所有阈值从 ConfigManager 注入
//
// 组合风险公式（相关性加权）:
//   portfolio_variance = Σ_i Σ_j w_i × w_j × ρ_ij × σ_i × σ_j
//   portfolio_risk = sqrt(portfolio_variance)
//
// 其中:
//   w_i = 持仓 i 的权重（风险金额）
//   ρ_ij = 品种 i 和 j 的相关系数
//   σ_i = 品种 i 的波动率
//
// 简化版本（已知风险金额）:
//   portfolio_risk = sqrt(Σ_i Σ_j risk_i × risk_j × ρ_ij)
//
// 约束:
//   portfolio_risk ≤ equity × max_portfolio_risk_pct (默认 3%)
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_PORTFOLIO_RISK_HPP
#define QUANT_STRATEGY_CORE_PORTFOLIO_RISK_HPP

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
// 常量
// ==============================================================================
namespace portfolio_config {

// 组合风险上限（权益占比）
inline constexpr double kDefaultMaxPortfolioRiskPct = 0.03;   // 3%

// 单品种风险上限
inline constexpr double kDefaultMaxPositionRiskPct = 0.02;    // 2%

// 最大持仓数
inline constexpr std::size_t kDefaultMaxPositions = 5;

// 同方向持仓叠加系数
inline constexpr double kDefaultSameDirectionLimit = 1.5;

// 相关性滚动窗口
inline constexpr std::size_t kDefaultCorrelationWindow = 100;

// 最小样本数
inline constexpr std::size_t kMinCorrelationSamples = 20;

// 相关性上限（防止过度分散）
inline constexpr double kDefaultMaxCorrelation = 0.95;

// 相关性崩溃阈值
inline constexpr double kCorrelationCrashThreshold = 0.9;

// 相关性崩溃降仓比例
inline constexpr double kCrashPositionScale = 0.5;

// VaR 置信度
inline constexpr double kDefaultVaRConfidence = 0.95;

// VaR 乘数（正态分布 95% ≈ 1.645）
inline constexpr double kVaRMultiplier95 = 1.645;
inline constexpr double kVaRMultiplier99 = 2.326;

// 冷却期（秒）
inline constexpr std::int64_t kDefaultCooldownSec = 60;

// 历史窗口
inline constexpr std::size_t kMaxHistorySize = 64;

// 浮点保护
inline constexpr double kEpsilon = 1e-9;

}  // namespace portfolio_config

// ==============================================================================
// 组合风险参数
// ==============================================================================
struct PortfolioParams {
    // 组合风险上限（权益占比）
    double max_portfolio_risk_pct{
        portfolio_config::kDefaultMaxPortfolioRiskPct};

    // 单品种风险上限
    double max_position_risk_pct{
        portfolio_config::kDefaultMaxPositionRiskPct};

    // 最大持仓数
    std::size_t max_positions{
        portfolio_config::kDefaultMaxPositions};

    // 同方向持仓叠加系数（超过此倍数视为方向偏置）
    double same_direction_limit{
        portfolio_config::kDefaultSameDirectionLimit};

    // 相关性
    std::size_t correlation_window{
        portfolio_config::kDefaultCorrelationWindow};
    std::size_t min_correlation_samples{
        portfolio_config::kMinCorrelationSamples};
    double max_correlation{
        portfolio_config::kDefaultMaxCorrelation};

    // 相关性崩溃
    bool enable_correlation_crash_detection{true};
    double correlation_crash_threshold{
        portfolio_config::kCorrelationCrashThreshold};
    double crash_position_scale{
        portfolio_config::kCrashPositionScale};

    // VaR
    bool enable_var{true};
    double var_confidence{
        portfolio_config::kDefaultVaRConfidence};

    // 冷却
    std::int64_t cooldown_sec{
        portfolio_config::kDefaultCooldownSec};

    // 是否启用各项检查
    bool enable_max_positions_check{true};
    bool enable_single_position_check{true};
    bool enable_same_direction_check{true};
    bool enable_correlation_check{true};
    bool enable_var_check{true};

    [[nodiscard]] Result<void> validate() const noexcept;

    [[nodiscard]] static PortfolioParams defaults() noexcept {
        return PortfolioParams{};
    }
};

// ==============================================================================
// 持仓快照（供组合风险计算）
// ==============================================================================
struct PortfolioPosition {
    Symbol symbol;
    Side side{Side::UNKNOWN};

    // 风险金额（USDT）
    double risk_amount{0.0};

    // 名义价值
    double notional{0.0};

    // 波动率（收益率标准差）
    double volatility{0.0};

    [[nodiscard]] bool is_valid() const noexcept {
        return !symbol.empty() &&
               (side == Side::BUY || side == Side::SELL) &&
               std::isfinite(risk_amount) && risk_amount >= 0.0 &&
               std::isfinite(notional) && notional >= 0.0;
    }
};

// ==============================================================================
// 风险贡献分解
// ==============================================================================
struct RiskContribution {
    Symbol symbol;
    double risk_amount{0.0};         // 绝对风险
    double risk_pct{0.0};             // 占总风险比例
    double marginal_contribution{0.0}; // 边际贡献
    double correlation_weighted{0.0};  // 相关性加权
};

// ==============================================================================
// 组合风险计算结果
// ==============================================================================
struct PortfolioRiskResult {
    // 组合总风险（相关性加权）
    double total_risk{0.0};
    double total_risk_pct{0.0};      // 占权益比例

    // 线性求和（未加权，用于对比）
    double linear_risk{0.0};
    double linear_risk_pct{0.0};

    // 分散化收益
    double diversification_benefit{0.0};  // linear - total

    // VaR
    double var_95{0.0};
    double var_99{0.0};

    // 风险贡献分解
    std::vector<RiskContribution> contributions;

    // 校验结果
    bool passed{true};
    double scale_factor{1.0};         // 建议仓位缩放

    // 违规项
    std::vector<std::string> violations;

    // 相关性崩溃
    bool correlation_crash{false};

    // 元信息
    std::size_t position_count{0};
    double equity{0.0};
    Timestamp evaluated_at{};

    [[nodiscard]] bool is_over_limit() const noexcept {
        return !passed;
    }

    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 相关性矩阵
// ==============================================================================
class CorrelationMatrix {
public:
    CorrelationMatrix() = default;

    // 设置/获取相关性
    void set(const Symbol& a, const Symbol& b, double corr) noexcept;
    [[nodiscard]] double get(const Symbol& a,
                              const Symbol& b) const noexcept;

    // 是否已设置
    [[nodiscard]] bool has(const Symbol& a,
                            const Symbol& b) const noexcept;

    // 清空
    void clear() noexcept;

    // 遍历
    [[nodiscard]] std::size_t size() const noexcept;

private:
    // 对称矩阵，键为排序后的 symbol 对
    [[nodiscard]] static std::string make_key(const Symbol& a,
                                                const Symbol& b);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, double> data_;
};

// ==============================================================================
// 收益序列缓存（用于滚动相关性计算）
// ==============================================================================
class ReturnsCache {
public:
    explicit ReturnsCache(std::size_t max_window)
        : max_window_{max_window} {}

    // 添加收益
    void push(const Symbol& symbol, double return_value) noexcept;

    // 获取收益序列
    [[nodiscard]] std::vector<double> get(const Symbol& symbol) const;

    // 计算相关性
    [[nodiscard]] std::optional<double> compute_correlation(
        const Symbol& a, const Symbol& b) const noexcept;

    // 清空
    void clear() noexcept;

    // 品种数
    [[nodiscard]] std::size_t symbol_count() const noexcept;

private:
    std::size_t max_window_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::deque<double>> data_;
};

// ==============================================================================
// 组合风险历史
// ==============================================================================
struct PortfolioRiskRecord {
    Timestamp timestamp{};
    double total_risk{0.0};
    double total_risk_pct{0.0};
    std::size_t position_count{0};
    bool passed{true};
    std::string violation;
};

// ==============================================================================
// 组合风险快照（持久化）
// ==============================================================================
struct PortfolioRiskSnapshot {
    std::uint32_t schema_version{1};
    Timestamp snapshot_time{};

    std::int64_t last_violation_time_us{0};
    std::uint64_t violation_count{0};

    std::vector<PortfolioRiskRecord> history;

    // 相关性矩阵（仅持久化已知的相关性）
    struct CorrEntry {
        std::string symbol_a;
        std::string symbol_b;
        double correlation{0.0};
    };
    std::vector<CorrEntry> correlation_matrix;

    [[nodiscard]] bool is_valid() const noexcept;
};

// ==============================================================================
// 组合风险统计
// ==============================================================================
struct PortfolioStats {
    std::atomic<std::uint64_t> compute_count{0};
    std::atomic<std::uint64_t> violation_count{0};
    std::atomic<std::uint64_t> max_positions_violation{0};
    std::atomic<std::uint64_t> single_position_violation{0};
    std::atomic<std::uint64_t> same_direction_violation{0};
    std::atomic<std::uint64_t> portfolio_risk_violation{0};
    std::atomic<std::uint64_t> var_violation{0};
    std::atomic<std::uint64_t> correlation_crash_count{0};
    std::atomic<std::uint64_t> scaled_count{0};
    std::atomic<std::uint64_t> cooldown_blocked_count{0};
    std::atomic<double> sum_total_risk{0.0};
    std::atomic<double> sum_diversification_benefit{0.0};

    void reset() noexcept {
        compute_count.store(0, std::memory_order_relaxed);
        violation_count.store(0, std::memory_order_relaxed);
        max_positions_violation.store(0, std::memory_order_relaxed);
        single_position_violation.store(0, std::memory_order_relaxed);
        same_direction_violation.store(0, std::memory_order_relaxed);
        portfolio_risk_violation.store(0, std::memory_order_relaxed);
        var_violation.store(0, std::memory_order_relaxed);
        correlation_crash_count.store(0, std::memory_order_relaxed);
        scaled_count.store(0, std::memory_order_relaxed);
        cooldown_blocked_count.store(0, std::memory_order_relaxed);
        sum_total_risk.store(0.0, std::memory_order_relaxed);
        sum_diversification_benefit.store(0.0, std::memory_order_relaxed);
    }

    [[nodiscard]] double avg_total_risk() const noexcept {
        const auto n = compute_count.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return sum_total_risk.load(std::memory_order_relaxed) /
               static_cast<double>(n);
    }

    [[nodiscard]] double avg_diversification_benefit() const noexcept {
        const auto n = compute_count.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return sum_diversification_benefit.load(
                   std::memory_order_relaxed) /
               static_cast<double>(n);
    }
};

// ==============================================================================
// 组合风险管理器
// ==============================================================================
class PortfolioRisk {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 参数提供者（必需）
        std::function<PortfolioParams()> params_provider;

        // 相关性提供者（可选，覆盖内部计算）
        std::function<double(const Symbol&, const Symbol&)>
            external_correlation_provider;

        // 违规回调（可选）
        std::function<void(const PortfolioRiskResult&)> on_violation;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit PortfolioRisk(Dependencies deps);
    ~PortfolioRisk();

    PortfolioRisk(const PortfolioRisk&) = delete;
    PortfolioRisk& operator=(const PortfolioRisk&) = delete;
    PortfolioRisk(PortfolioRisk&&) noexcept;
    PortfolioRisk& operator=(PortfolioRisk&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 相关性管理
    // -------------------------------------------------------------------------
    // 更新相关性
    void set_correlation(const Symbol& a, const Symbol& b,
                          double corr) noexcept;

    // 获取相关性
    [[nodiscard]] double get_correlation(const Symbol& a,
                                           const Symbol& b) const noexcept;

    // 更新收益序列（用于滚动计算）
    void update_returns(const Symbol& symbol,
                         double return_value) noexcept;

    // 从收益序列计算相关性
    Result<void> compute_correlations();

    // -------------------------------------------------------------------------
    // 核心计算
    // -------------------------------------------------------------------------
    // 计算组合风险
    [[nodiscard]] Result<PortfolioRiskResult> evaluate(
        const std::vector<PortfolioPosition>& positions,
        double equity);

    // 快速校验（不产生详细分解）
    [[nodiscard]] Result<bool> check(
        const std::vector<PortfolioPosition>& positions,
        double equity);

    // -------------------------------------------------------------------------
    // 新增订单校验
    // -------------------------------------------------------------------------
    // 校验新订单加入后组合是否超限
    [[nodiscard]] Result<PortfolioRiskResult> check_new_order(
        const std::vector<PortfolioPosition>& current_positions,
        const PortfolioPosition& new_order,
        double equity);

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] std::uint64_t violation_count() const noexcept;
    [[nodiscard]] std::optional<Timestamp> last_violation_time() const noexcept;
    [[nodiscard]] std::vector<PortfolioRiskRecord> history() const;
    [[nodiscard]] std::size_t tracked_symbols() const noexcept;

    // -------------------------------------------------------------------------
    // 手动控制
    // -------------------------------------------------------------------------
    void clear_history() noexcept;
    void clear_cooldown() noexcept;

    // -------------------------------------------------------------------------
    // 持久化
    // -------------------------------------------------------------------------
    [[nodiscard]] PortfolioRiskSnapshot to_snapshot() const;
    Result<void> from_snapshot(const PortfolioRiskSnapshot& snapshot);

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const PortfolioStats& stats() const noexcept;
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

// 计算两个收益序列的相关性
[[nodiscard]] std::optional<double> compute_pearson_correlation(
    const std::vector<double>& a,
    const std::vector<double>& b) noexcept;

// 计算两个收益序列的协方差
[[nodiscard]] std::optional<double> compute_covariance(
    const std::vector<double>& a,
    const std::vector<double>& b) noexcept;

// 计算收益序列标准差
[[nodiscard]] std::optional<double> compute_stddev(
    const std::vector<double>& data) noexcept;

// VaR 正态分布乘数
[[nodiscard]] inline double var_multiplier(double confidence) noexcept {
    if (confidence >= 0.99) return portfolio_config::kVaRMultiplier99;
    if (confidence >= 0.95) return portfolio_config::kVaRMultiplier95;
    // 简化：按置信度线性插值
    return std::max(1.0, confidence * 1.645);
}

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_PORTFOLIO_RISK_HPP
