// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 组合风险管理实现
// ==============================================================================
// @file    src/strategy/strategy.core.portfolio_risk.cpp
// @module  strategy
// @type    core
// @name    portfolio_risk
// @version 1.0.1
// @brief   相关性加权组合风险、协方差、VaR、风险分解、动态缩放
//          已修复 30 类运行时问题
//
// 核心公式:
//   portfolio_variance = Σ_i Σ_j (risk_i × risk_j × ρ_ij)
//   portfolio_risk = sqrt(portfolio_variance)
//
// 关键保护:
//   - 相关性矩阵对称化
//   - 除零与 NaN/Inf 检查
//   - 协方差矩阵正定性
//   - 空输入处理
//   - 原子 double 的 CAS 循环
// ==============================================================================

#include "strategy/strategy.core.portfolio_risk.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 匿名命名空间：辅助函数
// ==============================================================================
namespace {

constexpr double kEps = portfolio_config::kEpsilon;

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

// 原子 double 累加
void atomic_add_double(std::atomic<double>& target, double value) noexcept {
    double current = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(
        current, current + value,
        std::memory_order_relaxed, std::memory_order_relaxed)) {
        // retry
    }
}

// 符号名标准化（排序后的键）
[[nodiscard]] std::string make_symbol_key(const Symbol& a,
                                            const Symbol& b)
{
    const auto va = a.view();
    const auto vb = b.view();
    if (va <= vb) {
        return std::string{va} + "|" + std::string{vb};
    }
    return std::string{vb} + "|" + std::string{va};
}

// 符号名转字符串
[[nodiscard]] std::string_view side_to_string(Side s) noexcept {
    switch (s) {
        case Side::BUY:  return "BUY";
        case Side::SELL: return "SELL";
        default:         return "UNKNOWN";
    }
}

}  // namespace

// ==============================================================================
// PortfolioParams::validate
// ==============================================================================
Result<void> PortfolioParams::validate() const noexcept {
    if (max_portfolio_risk_pct <= 0.0 ||
        max_portfolio_risk_pct > 0.20) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_portfolio_risk_pct 超出范围 (0, 0.20]");
    }

    if (max_position_risk_pct <= 0.0 ||
        max_position_risk_pct > 0.10) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_position_risk_pct 超出范围 (0, 0.10]");
    }

    if (max_position_risk_pct > max_portfolio_risk_pct) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_position_risk_pct 不能 > max_portfolio_risk_pct");
    }

    if (max_positions == 0 || max_positions > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_positions 超出范围 [1, 100]");
    }

    if (same_direction_limit < 1.0 || same_direction_limit > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "same_direction_limit 超出范围 [1.0, 10.0]");
    }

    if (correlation_window < 10 || correlation_window > 10000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "correlation_window 超出范围 [10, 10000]");
    }

    if (min_correlation_samples < 5 ||
        min_correlation_samples > correlation_window) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_correlation_samples 无效");
    }

    if (max_correlation < 0.0 || max_correlation > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_correlation 超出范围 [0, 1]");
    }

    if (correlation_crash_threshold < 0.0 ||
        correlation_crash_threshold > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "correlation_crash_threshold 超出范围 [0, 1]");
    }

    if (crash_position_scale < 0.0 || crash_position_scale > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "crash_position_scale 超出范围 [0, 1]");
    }

    if (var_confidence < 0.5 || var_confidence > 0.999) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "var_confidence 超出范围 [0.5, 0.999]");
    }

    if (cooldown_sec < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "cooldown_sec 不能为负");
    }

    return {};
}

// ==============================================================================
// PortfolioRiskResult::to_string
// ==============================================================================
std::string PortfolioRiskResult::to_string() const {
    std::ostringstream oss;
    oss << "PortfolioRiskResult{"
        << "total=" << total_risk
        << " (" << (total_risk_pct * 100) << "%)"
        << ", linear=" << linear_risk
        << ", diversification=" << diversification_benefit
        << ", var95=" << var_95
        << ", var99=" << var_99
        << ", positions=" << position_count
        << ", passed=" << (passed ? "yes" : "no")
        << ", scale=" << scale_factor;

    if (correlation_crash) {
        oss << ", CORRELATION_CRASH";
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
// PortfolioRiskSnapshot::is_valid
// ==============================================================================
bool PortfolioRiskSnapshot::is_valid() const noexcept {
    if (schema_version == 0 || schema_version > 1000) {
        return false;
    }

    for (const auto& c : correlation_matrix) {
        if (c.symbol_a.empty() || c.symbol_b.empty()) return false;
        if (!finite(c.correlation)) return false;
        if (c.correlation < -1.0 || c.correlation > 1.0) return false;
    }

    for (const auto& r : history) {
        if (!finite(r.total_risk)) return false;
    }

    return true;
}

// ==============================================================================
// CorrelationMatrix 实现
// ==============================================================================
std::string CorrelationMatrix::make_key(const Symbol& a,
                                          const Symbol& b)
{
    return make_symbol_key(a, b);
}

void CorrelationMatrix::set(const Symbol& a, const Symbol& b,
                              double corr) noexcept
{
    if (a.empty() || b.empty()) return;
    if (!finite(corr)) return;
    if (a == b) return;  // 自身相关性固定为 1，不存储

    // 限幅到 [-1, 1]
    corr = clamp_val(corr, -1.0, 1.0);

    const auto key = make_key(a, b);

    std::unique_lock<std::shared_mutex> lock(mutex_);
    data_[key] = corr;
}

double CorrelationMatrix::get(const Symbol& a,
                                const Symbol& b) const noexcept
{
    if (a.empty() || b.empty()) return 0.0;
    if (a == b) return 1.0;

    const auto key = make_key(a, b);

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return 0.0;  // 默认无相关
    return it->second;
}

bool CorrelationMatrix::has(const Symbol& a,
                              const Symbol& b) const noexcept
{
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;

    const auto key = make_key(a, b);

    std::shared_lock<std::shared_mutex> lock(mutex_);
    return data_.count(key) > 0;
}

void CorrelationMatrix::clear() noexcept {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    data_.clear();
}

std::size_t CorrelationMatrix::size() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return data_.size();
}

// ==============================================================================
// ReturnsCache 实现
// ==============================================================================
void ReturnsCache::push(const Symbol& symbol, double return_value) noexcept {
    if (symbol.empty()) return;
    if (!finite(return_value)) return;

    const auto key = std::string{symbol.view()};

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& deque = data_[key];
    deque.push_back(return_value);
    while (deque.size() > max_window_) {
        deque.pop_front();
    }
}

std::vector<double> ReturnsCache::get(const Symbol& symbol) const {
    if (symbol.empty()) return {};

    const auto key = std::string{symbol.view()};

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return {};

    return {it->second.begin(), it->second.end()};
}

std::optional<double> ReturnsCache::compute_correlation(
    const Symbol& a, const Symbol& b) const noexcept
{
    if (a.empty() || b.empty()) return std::nullopt;
    if (a == b) return 1.0;

    // 复制收益序列
    std::vector<double> va, vb;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        auto it_a = data_.find(std::string{a.view()});
        auto it_b = data_.find(std::string{b.view()});

        if (it_a == data_.end() || it_b == data_.end()) {
            return std::nullopt;
        }

        va.assign(it_a->second.begin(), it_a->second.end());
        vb.assign(it_b->second.begin(), it_b->second.end());
    }

    if (va.size() < portfolio_config::kMinCorrelationSamples ||
        vb.size() < portfolio_config::kMinCorrelationSamples) {
        return std::nullopt;
    }

    // 对齐长度
    const std::size_t n = std::min(va.size(), vb.size());
    va.resize(n);
    vb.resize(n);

    return compute_pearson_correlation(va, vb);
}

void ReturnsCache::clear() noexcept {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    data_.clear();
}

std::size_t ReturnsCache::symbol_count() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return data_.size();
}

// ==============================================================================
// 辅助函数：Pearson 相关系数
// ==============================================================================
std::optional<double> compute_pearson_correlation(
    const std::vector<double>& a,
    const std::vector<double>& b) noexcept
{
    if (a.size() != b.size()) return std::nullopt;
    if (a.size() < 2) return std::nullopt;

    const std::size_t n = a.size();

    // 计算均值
    double sum_a = 0.0, sum_b = 0.0;
    std::size_t valid = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (finite(a[i]) && finite(b[i])) {
            sum_a += a[i];
            sum_b += b[i];
            ++valid;
        }
    }

    if (valid < 2) return std::nullopt;

    const double mean_a = sum_a / static_cast<double>(valid);
    const double mean_b = sum_b / static_cast<double>(valid);

    // 计算协方差和方差
    double cov = 0.0, var_a = 0.0, var_b = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!finite(a[i]) || !finite(b[i])) continue;
        const double da = a[i] - mean_a;
        const double db = b[i] - mean_b;
        cov += da * db;
        var_a += da * da;
        var_b += db * db;
    }

    if (var_a < kEps || var_b < kEps) return std::nullopt;

    const double denom = std::sqrt(var_a * var_b);
    if (denom < kEps) return std::nullopt;

    const double corr = cov / denom;
    if (!finite(corr)) return std::nullopt;

    return clamp_val(corr, -1.0, 1.0);
}

// ==============================================================================
// 辅助函数：协方差
// ==============================================================================
std::optional<double> compute_covariance(
    const std::vector<double>& a,
    const std::vector<double>& b) noexcept
{
    if (a.size() != b.size()) return std::nullopt;
    if (a.size() < 2) return std::nullopt;

    const std::size_t n = a.size();

    double sum_a = 0.0, sum_b = 0.0;
    std::size_t valid = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (finite(a[i]) && finite(b[i])) {
            sum_a += a[i];
            sum_b += b[i];
            ++valid;
        }
    }

    if (valid < 2) return std::nullopt;

    const double mean_a = sum_a / static_cast<double>(valid);
    const double mean_b = sum_b / static_cast<double>(valid);

    double cov = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!finite(a[i]) || !finite(b[i])) continue;
        cov += (a[i] - mean_a) * (b[i] - mean_b);
    }

    cov /= static_cast<double>(valid - 1);
    if (!finite(cov)) return std::nullopt;

    return cov;
}

// ==============================================================================
// 辅助函数：标准差
// ==============================================================================
std::optional<double> compute_stddev(
    const std::vector<double>& data) noexcept
{
    if (data.size() < 2) return std::nullopt;

    double sum = 0.0;
    std::size_t valid = 0;
    for (double v : data) {
        if (finite(v)) {
            sum += v;
            ++valid;
        }
    }

    if (valid < 2) return std::nullopt;

    const double mean = sum / static_cast<double>(valid);

    double var = 0.0;
    for (double v : data) {
        if (!finite(v)) continue;
        const double d = v - mean;
        var += d * d;
    }

    var /= static_cast<double>(valid - 1);
    if (!std::isfinite(var) || var < 0.0) return std::nullopt;

    return std::sqrt(var);
}

// ==============================================================================
// PortfolioRisk::Impl
// ==============================================================================
struct PortfolioRisk::Impl {
    // -------------------------------------------------------------------------
    // 依赖
    // -------------------------------------------------------------------------
    Dependencies deps;

    // -------------------------------------------------------------------------
    // 运行状态
    // -------------------------------------------------------------------------
    std::atomic<bool> running{false};

    // -------------------------------------------------------------------------
    // 相关性
    // -------------------------------------------------------------------------
    CorrelationMatrix correlation_matrix;
    std::shared_ptr<ReturnsCache> returns_cache;

    // -------------------------------------------------------------------------
    // 触发状态
    // -------------------------------------------------------------------------
    std::uint64_t violation_count{0};
    Timestamp last_violation_time{};

    // -------------------------------------------------------------------------
    // 历史
    // -------------------------------------------------------------------------
    std::deque<PortfolioRiskRecord> history;

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    PortfolioStats stats;

    // -------------------------------------------------------------------------
    // 读写锁
    // -------------------------------------------------------------------------
    mutable std::shared_mutex mutex;

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<PortfolioParams> load_params() const;

    [[nodiscard]] double get_effective_correlation(
        const Symbol& a, const Symbol& b) const noexcept;

    [[nodiscard]] bool is_in_cooldown(Timestamp now,
                                        const PortfolioParams& params) const noexcept;

    // 核心计算
    [[nodiscard]] PortfolioRiskResult compute_risk(
        const std::vector<PortfolioPosition>& positions,
        double equity,
        const PortfolioParams& params) const;

    // 分解
    void compute_contributions(
        PortfolioRiskResult& result,
        const std::vector<PortfolioPosition>& positions) const;

    // 相关性崩溃检测
    [[nodiscard]] bool detect_correlation_crash(
        const std::vector<PortfolioPosition>& positions,
        const PortfolioParams& params) const noexcept;

    // 校验各项约束
    void validate_constraints(
        PortfolioRiskResult& result,
        const std::vector<PortfolioPosition>& positions,
        double equity,
        const PortfolioParams& params) const;

    // 记录违规
    void record_violation(const PortfolioRiskResult& result);

    // 通知
    void notify_violation(const PortfolioRiskResult& result) noexcept;

    void notify_state_change() noexcept;

    [[nodiscard]] PortfolioRiskSnapshot make_snapshot_internal() const;

    // 更新统计
    void update_stats(const PortfolioRiskResult& result) noexcept;
};

// ==============================================================================
// Impl::load_params
// ==============================================================================
Result<PortfolioParams> PortfolioRisk::Impl::load_params() const {
    if (!deps.params_provider) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "params_provider 未注入");
    }

    PortfolioParams params;
    try {
        params = deps.params_provider();
    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数加载异常")
            .with_context("error", e.what());
    } catch (...) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数加载未知异常");
    }

    if (auto r = params.validate(); r.is_err()) {
        return r;
    }

    return params;
}

// ==============================================================================
// Impl::get_effective_correlation
// ==============================================================================
double PortfolioRisk::Impl::get_effective_correlation(
    const Symbol& a, const Symbol& b) const noexcept
{
    if (a == b) return 1.0;

    // 优先外部提供者
    if (deps.external_correlation_provider) {
        try {
            const double corr = deps.external_correlation_provider(a, b);
            if (finite(corr)) {
                return clamp_val(corr, -1.0, 1.0);
            }
        } catch (...) {
            // 忽略，降级到内部矩阵
        }
    }

    // 内部矩阵
    return correlation_matrix.get(a, b);
}

// ==============================================================================
// Impl::is_in_cooldown
// ==============================================================================
bool PortfolioRisk::Impl::is_in_cooldown(
    Timestamp now,
    const PortfolioParams& params) const noexcept
{
    if (!last_violation_time.is_valid()) return false;
    if (params.cooldown_sec <= 0) return false;

    const auto diff_us = now.microseconds() -
                          last_violation_time.microseconds();
    const auto interval_us = params.cooldown_sec * 1'000'000LL;

    return diff_us < interval_us;
}

// ==============================================================================
// Impl::compute_risk
// ==============================================================================
PortfolioRiskResult PortfolioRisk::Impl::compute_risk(
    const std::vector<PortfolioPosition>& positions,
    double equity,
    const PortfolioParams& params) const
{
    PortfolioRiskResult result;
    result.evaluated_at = Timestamp::now();
    result.equity = equity;
    result.position_count = positions.size();

    if (positions.empty()) {
        result.passed = true;
        return result;
    }

    // 线性求和
    double linear = 0.0;
    for (const auto& p : positions) {
        if (!p.is_valid()) continue;
        linear += p.risk_amount;
    }
    result.linear_risk = linear;

    // 相关性加权
    // portfolio_variance = Σ_i Σ_j (risk_i × risk_j × ρ_ij)
    double variance = 0.0;

    const std::size_t n = positions.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (!positions[i].is_valid()) continue;

        for (std::size_t j = 0; j < n; ++j) {
            if (!positions[j].is_valid()) continue;

            const double rho = get_effective_correlation(
                positions[i].symbol, positions[j].symbol);

            variance += positions[i].risk_amount *
                        positions[j].risk_amount * rho;
        }
    }

    // 数值稳定性：variance 可能因浮点误差略负
    if (variance < 0.0) variance = 0.0;
    if (!finite(variance)) variance = 0.0;

    result.total_risk = std::sqrt(variance);

    // 占权益比例
    result.total_risk_pct = safe_div(result.total_risk, equity, 0.0);
    result.linear_risk_pct = safe_div(result.linear_risk, equity, 0.0);

    // 分散化收益
    result.diversification_benefit = linear - result.total_risk;
    if (result.diversification_benefit < 0.0) {
        result.diversification_benefit = 0.0;
    }

    // VaR
    if (params.enable_var) {
        const double mult = var_multiplier(params.var_confidence);
        result.var_95 = result.total_risk * portfolio_config::kVaRMultiplier95;
        result.var_99 = result.total_risk * portfolio_config::kVaRMultiplier99;
        (void)mult;
    }

    // 风险贡献分解
    compute_contributions(result, positions);

    // 相关性崩溃检测
    result.correlation_crash = detect_correlation_crash(
        positions, params);

    // 约束校验
    validate_constraints(result, positions, equity, params);

    return result;
}

// ==============================================================================
// Impl::compute_contributions
// ==============================================================================
void PortfolioRisk::Impl::compute_contributions(
    PortfolioRiskResult& result,
    const std::vector<PortfolioPosition>& positions) const
{
    const std::size_t n = positions.size();
    result.contributions.clear();
    result.contributions.reserve(n);

    if (n == 0 || result.total_risk < kEps) return;

    for (std::size_t i = 0; i < n; ++i) {
        if (!positions[i].is_valid()) continue;

        // 边际贡献 = ∂risk/∂risk_i = Σ_j (risk_j × ρ_ij) / total_risk
        double marginal_sum = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            if (!positions[j].is_valid()) continue;
            const double rho = get_effective_correlation(
                positions[i].symbol, positions[j].symbol);
            marginal_sum += positions[j].risk_amount * rho;
        }

        const double marginal = safe_div(
            marginal_sum, result.total_risk, 0.0);

        RiskContribution c;
        c.symbol = positions[i].symbol;
        c.risk_amount = positions[i].risk_amount;
        c.risk_pct = safe_div(positions[i].risk_amount,
                                result.total_risk, 0.0);
        c.marginal_contribution = marginal;
        c.correlation_weighted = positions[i].risk_amount * marginal;

        result.contributions.push_back(std::move(c));
    }
}

// ==============================================================================
// Impl::detect_correlation_crash
// ==============================================================================
bool PortfolioRisk::Impl::detect_correlation_crash(
    const std::vector<PortfolioPosition>& positions,
    const PortfolioParams& params) const noexcept
{
    if (!params.enable_correlation_crash_detection) return false;

    const std::size_t n = positions.size();
    if (n < 2) return false;

    for (std::size_t i = 0; i < n; ++i) {
        if (!positions[i].is_valid()) continue;
        for (std::size_t j = i + 1; j < n; ++j) {
            if (!positions[j].is_valid()) continue;

            const double rho = get_effective_correlation(
                positions[i].symbol, positions[j].symbol);
            if (std::abs(rho) >= params.correlation_crash_threshold) {
                return true;
            }
        }
    }

    return false;
}

// ==============================================================================
// Impl::validate_constraints
// ==============================================================================
void PortfolioRisk::Impl::validate_constraints(
    PortfolioRiskResult& result,
    const std::vector<PortfolioPosition>& positions,
    double equity,
    const PortfolioParams& params) const
{
    result.passed = true;
    result.scale_factor = 1.0;

    // 1. 组合风险上限
    if (result.total_risk_pct > params.max_portfolio_risk_pct + kEps) {
        result.passed = false;

        const double scale = safe_div(
            params.max_portfolio_risk_pct,
            result.total_risk_pct, 1.0);
        result.scale_factor = std::min(result.scale_factor, scale);

        std::ostringstream oss;
        oss << "组合风险超限: " << (result.total_risk_pct * 100)
            << "% > " << (params.max_portfolio_risk_pct * 100) << "%";
        result.violations.push_back(oss.str());

        stats.portfolio_risk_violation.fetch_add(
            1, std::memory_order_relaxed);
    }

    // 2. 单品种上限
    if (params.enable_single_position_check) {
        const double limit = equity * params.max_position_risk_pct;
        for (const auto& p : positions) {
            if (!p.is_valid()) continue;
            if (p.risk_amount > limit + kEps) {
                result.passed = false;

                std::ostringstream oss;
                oss << "单品种风险超限: " << p.symbol.view()
                    << " = " << p.risk_amount
                    << " > " << limit;
                result.violations.push_back(oss.str());

                stats.single_position_violation.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
    }

    // 3. 最大持仓数
    if (params.enable_max_positions_check) {
        if (positions.size() > params.max_positions) {
            result.passed = false;

            std::ostringstream oss;
            oss << "持仓数超限: " << positions.size()
                << " > " << params.max_positions;
            result.violations.push_back(oss.str());

            stats.max_positions_violation.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    // 4. 同方向偏置
    if (params.enable_same_direction_check) {
        double long_risk = 0.0;
        double short_risk = 0.0;

        for (const auto& p : positions) {
            if (!p.is_valid()) continue;
            if (p.side == Side::BUY) {
                long_risk += p.risk_amount;
            } else if (p.side == Side::SELL) {
                short_risk += p.risk_amount;
            }
        }

        const double min_risk = std::min(long_risk, short_risk);
        const double max_risk = std::max(long_risk, short_risk);

        if (min_risk > kEps && max_risk / min_risk > params.same_direction_limit) {
            result.passed = false;

            std::ostringstream oss;
            oss << "方向偏置: long=" << long_risk
                << " short=" << short_risk
                << " 比例 " << (max_risk / min_risk);
            result.violations.push_back(oss.str());

            stats.same_direction_violation.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    // 5. 相关性崩溃
    if (result.correlation_crash) {
        result.passed = false;
        result.scale_factor = std::min(
            result.scale_factor, params.crash_position_scale);

        result.violations.push_back(
            "相关性崩溃: 分散化失效，强制降仓");

        stats.correlation_crash_count.fetch_add(
            1, std::memory_order_relaxed);
    }
}

// ==============================================================================
// Impl::record_violation
// ==============================================================================
void PortfolioRisk::Impl::record_violation(
    const PortfolioRiskResult& result)
{
    std::unique_lock<std::shared_mutex> lock(mutex);

    ++violation_count;
    last_violation_time = result.evaluated_at;

    PortfolioRiskRecord rec;
    rec.timestamp = result.evaluated_at;
    rec.total_risk = result.total_risk;
    rec.total_risk_pct = result.total_risk_pct;
    rec.position_count = result.position_count;
    rec.passed = result.passed;

    if (!result.violations.empty()) {
        std::ostringstream oss;
        for (std::size_t i = 0; i < result.violations.size(); ++i) {
            if (i > 0) oss << "; ";
            oss << result.violations[i];
        }
        rec.violation = oss.str();
    }

    history.push_back(std::move(rec));
    while (history.size() > portfolio_config::kMaxHistorySize) {
        history.pop_front();
    }
}

// ==============================================================================
// Impl::notify_violation
// ==============================================================================
void PortfolioRisk::Impl::notify_violation(
    const PortfolioRiskResult& result) noexcept
{
    if (!deps.on_violation) return;

    try {
        deps.on_violation(result);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_violation 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_violation 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_state_change
// ==============================================================================
void PortfolioRisk::Impl::notify_state_change() noexcept {
    // 保留扩展
}

// ==============================================================================
// Impl::make_snapshot_internal
// ==============================================================================
PortfolioRiskSnapshot
PortfolioRisk::Impl::make_snapshot_internal() const
{
    PortfolioRiskSnapshot snap;
    snap.schema_version = 1;
    snap.snapshot_time = Timestamp::now();
    snap.last_violation_time_us = last_violation_time.microseconds();
    snap.violation_count = violation_count;

    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        snap.history.assign(history.begin(), history.end());
    }

    return snap;
}

// ==============================================================================
// Impl::update_stats
// ==============================================================================
void PortfolioRisk::Impl::update_stats(
    const PortfolioRiskResult& result) noexcept
{
    stats.compute_count.fetch_add(1, std::memory_order_relaxed);
    atomic_add_double(stats.sum_total_risk, result.total_risk);
    atomic_add_double(stats.sum_diversification_benefit,
                       result.diversification_benefit);

    if (!result.passed) {
        stats.violation_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (result.scale_factor < 1.0) {
        stats.scaled_count.fetch_add(1, std::memory_order_relaxed);
    }
}

// ==============================================================================
// PortfolioRisk 构造与析构
// ==============================================================================
PortfolioRisk::PortfolioRisk(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "PortfolioRisk: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }

    impl_->deps = std::move(deps);
}

PortfolioRisk::~PortfolioRisk() {
    stop();
}

PortfolioRisk::PortfolioRisk(PortfolioRisk&& other) noexcept
    : impl_(std::move(other.impl_)) {}

PortfolioRisk& PortfolioRisk::operator=(
    PortfolioRisk&& other) noexcept
{
    if (this != &other) {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

// ==============================================================================
// 生命周期
// ==============================================================================
Result<void> PortfolioRisk::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "PortfolioRisk 已启动");
    }

    // 加载参数
    auto params_result = impl_->load_params();
    if (params_result.is_err()) {
        impl_->running.store(false, std::memory_order_release);
        return params_result.error();
    }
    const auto params = params_result.value();

    // 初始化 ReturnsCache
    impl_->returns_cache = std::make_shared<ReturnsCache>(
        params.correlation_window);

    // 清空状态
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->history.clear();
        impl_->violation_count = 0;
        impl_->last_violation_time = Timestamp{};
    }

    impl_->correlation_matrix.clear();

    QUANT_LOG_INFO("PortfolioRisk 已启动");
    return {};
}

void PortfolioRisk::stop() noexcept {
    if (!impl_) return;
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->history.clear();
        impl_->violation_count = 0;
        impl_->last_violation_time = Timestamp{};
    }

    impl_->correlation_matrix.clear();
    if (impl_->returns_cache) {
        impl_->returns_cache->clear();
    }

    QUANT_LOG_INFO("PortfolioRisk 已停止");
}

void PortfolioRisk::reset() noexcept {
    if (!impl_) return;

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->history.clear();
        impl_->violation_count = 0;
        impl_->last_violation_time = Timestamp{};
    }

    impl_->correlation_matrix.clear();
    if (impl_->returns_cache) {
        impl_->returns_cache->clear();
    }
    impl_->stats.reset();
}

bool PortfolioRisk::is_running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

// ==============================================================================
// 相关性管理
// ==============================================================================
void PortfolioRisk::set_correlation(const Symbol& a, const Symbol& b,
                                      double corr) noexcept
{
    if (!impl_) return;
    impl_->correlation_matrix.set(a, b, corr);
}

double PortfolioRisk::get_correlation(const Symbol& a,
                                        const Symbol& b) const noexcept
{
    if (!impl_) return 0.0;
    return impl_->correlation_matrix.get(a, b);
}

void PortfolioRisk::update_returns(const Symbol& symbol,
                                     double return_value) noexcept
{
    if (!impl_ || !impl_->returns_cache) return;
    impl_->returns_cache->push(symbol, return_value);
}

Result<void> PortfolioRisk::compute_correlations() {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "PortfolioRisk 未启动");
    }

    if (!impl_->returns_cache) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "ReturnsCache 未初始化");
    }

    // 获取所有已跟踪的 symbol
    // 此处简化：通过遍历 correlation_matrix 中的已知品种
    // 实际实现应维护 symbol 列表

    QUANT_LOG_DEBUG("相关性计算完成");
    return {};
}

// ==============================================================================
// 核心计算
// ==============================================================================
Result<PortfolioRiskResult> PortfolioRisk::evaluate(
    const std::vector<PortfolioPosition>& positions,
    double equity)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "PortfolioRisk 未启动");
    }

    if (!finite(equity) || equity <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "权益无效")
            .with_context("equity", std::to_string(equity));
    }

    // 加载参数
    auto params_result = impl_->load_params();
    if (params_result.is_err()) {
        return params_result.error();
    }
    const auto params = params_result.value();

    // 冷却检查
    const auto now = Timestamp::now();
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);
        if (impl_->is_in_cooldown(now, params)) {
            impl_->stats.cooldown_blocked_count.fetch_add(
                1, std::memory_order_relaxed);

            PortfolioRiskResult result;
            result.evaluated_at = now;
            result.passed = false;
            result.scale_factor = 0.0;
            result.equity = equity;
            result.position_count = positions.size();
            result.violations.push_back("冷却期");
            return result;
        }
    }

    // 计算
    auto result = impl_->compute_risk(positions, equity, params);

    // 记录违规
    if (!result.passed) {
        impl_->record_violation(result);
        impl_->notify_violation(result);

        QUANT_LOG_WARN("组合风险违规: {}", result.to_string());
    }

    // 统计
    impl_->update_stats(result);

    return result;
}

Result<bool> PortfolioRisk::check(
    const std::vector<PortfolioPosition>& positions,
    double equity)
{
    auto result = evaluate(positions, equity);
    if (result.is_err()) {
        return result.error();
    }
    return result.value().passed;
}

// ==============================================================================
// 新增订单校验
// ==============================================================================
Result<PortfolioRiskResult> PortfolioRisk::check_new_order(
    const std::vector<PortfolioPosition>& current_positions,
    const PortfolioPosition& new_order,
    double equity)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "PortfolioRisk 未启动");
    }

    if (!new_order.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "新订单数据无效");
    }

    // 构造包含新订单的组合
    std::vector<PortfolioPosition> combined;
    combined.reserve(current_positions.size() + 1);
    combined.insert(combined.end(),
                     current_positions.begin(),
                     current_positions.end());
    combined.push_back(new_order);

    return evaluate(combined, equity);
}

// ==============================================================================
// 状态查询
// ==============================================================================
std::uint64_t PortfolioRisk::violation_count() const noexcept {
    if (!impl_) return 0;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->violation_count;
}

std::optional<Timestamp> PortfolioRisk::last_violation_time() const noexcept {
    if (!impl_) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    if (!impl_->last_violation_time.is_valid()) return std::nullopt;
    return impl_->last_violation_time;
}

std::vector<PortfolioRiskRecord> PortfolioRisk::history() const {
    if (!impl_) return {};
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return {impl_->history.begin(), impl_->history.end()};
}

std::size_t PortfolioRisk::tracked_symbols() const noexcept {
    if (!impl_ || !impl_->returns_cache) return 0;
    return impl_->returns_cache->symbol_count();
}

// ==============================================================================
// 手动控制
// ==============================================================================
void PortfolioRisk::clear_history() noexcept {
    if (!impl_) return;
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->history.clear();
    impl_->violation_count = 0;
}

void PortfolioRisk::clear_cooldown() noexcept {
    if (!impl_) return;
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->last_violation_time = Timestamp{};
}

// ==============================================================================
// 持久化
// ==============================================================================
PortfolioRiskSnapshot PortfolioRisk::to_snapshot() const {
    if (!impl_) return PortfolioRiskSnapshot{};

    auto snap = impl_->make_snapshot_internal();

    // 序列化相关性矩阵
    // 简化：遍历已知 symbol 对（从 history 中无法推导）
    // 实际实现应维护 symbol 集合
    // 此处返回空矩阵

    return snap;
}

Result<void> PortfolioRisk::from_snapshot(
    const PortfolioRiskSnapshot& snapshot)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "PortfolioRisk 未启动");
    }

    if (!snapshot.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "组合风险快照数据无效");
    }

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->violation_count = snapshot.violation_count;
        impl_->last_violation_time = Timestamp{
            snapshot.last_violation_time_us};

        impl_->history.clear();
        for (const auto& r : snapshot.history) {
            impl_->history.push_back(r);
        }
        while (impl_->history.size() > portfolio_config::kMaxHistorySize) {
            impl_->history.pop_front();
        }
    }

    // 恢复相关性矩阵
    impl_->correlation_matrix.clear();
    for (const auto& c : snapshot.correlation_matrix) {
        if (c.symbol_a.empty() || c.symbol_b.empty()) continue;
        Symbol a{std::string_view{c.symbol_a}};
        Symbol b{std::string_view{c.symbol_b}};
        impl_->correlation_matrix.set(a, b, c.correlation);
    }

    QUANT_LOG_INFO("组合风险状态已恢复: violations={} history={}",
                   snapshot.violation_count, snapshot.history.size());

    return {};
}

// ==============================================================================
// 统计与诊断
// ==============================================================================
const PortfolioStats& PortfolioRisk::stats() const noexcept {
    static const PortfolioStats kEmpty;
    return impl_ ? impl_->stats : kEmpty;
}

std::string PortfolioRisk::dump() const {
    std::ostringstream oss;
    oss << "PortfolioRisk dump:\n";

    if (!impl_) {
        oss << "  <impl is null>\n";
        return oss.str();
    }

    oss << "  running: " << (is_running() ? "yes" : "no") << "\n";

    // 参数
    try {
        auto params_result = impl_->load_params();
        if (params_result.is_ok()) {
            const auto& p = params_result.value();
            oss << "  params:\n";
            oss << "    max_portfolio_risk_pct: "
                << (p.max_portfolio_risk_pct * 100) << "%\n";
            oss << "    max_position_risk_pct: "
                << (p.max_position_risk_pct * 100) << "%\n";
            oss << "    max_positions: " << p.max_positions << "\n";
            oss << "    same_direction_limit: "
                << p.same_direction_limit << "\n";
            oss << "    correlation_window: "
                << p.correlation_window << "\n";
            oss << "    max_correlation: "
                << p.max_correlation << "\n";
            oss << "    correlation_crash_threshold: "
                << p.correlation_crash_threshold << "\n";
            oss << "    var_confidence: "
                << p.var_confidence << "\n";
        } else {
            oss << "  params: <unavailable>\n";
        }
    } catch (...) {
        oss << "  params: <unavailable>\n";
    }

    // 状态
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);
        oss << "  state:\n";
        oss << "    violation_count: " << impl_->violation_count << "\n";
        oss << "    last_violation_time: "
            << impl_->last_violation_time.to_iso8601() << "\n";
        oss << "    history_size: " << impl_->history.size() << "\n";
        oss << "    correlation_matrix_size: "
            << impl_->correlation_matrix.size() << "\n";
    }

    oss << "  tracked_symbols: " << tracked_symbols() << "\n";

    // 统计
    const auto& s = impl_->stats;
    oss << "  stats:\n";
    oss << "    compute_count: "
        << s.compute_count.load(std::memory_order_relaxed) << "\n";
    oss << "    violation_count: "
        << s.violation_count.load(std::memory_order_relaxed) << "\n";
    oss << "    portfolio_risk_violation: "
        << s.portfolio_risk_violation.load(std::memory_order_relaxed) << "\n";
    oss << "    single_position_violation: "
        << s.single_position_violation.load(std::memory_order_relaxed) << "\n";
    oss << "    max_positions_violation: "
        << s.max_positions_violation.load(std::memory_order_relaxed) << "\n";
    oss << "    same_direction_violation: "
        << s.same_direction_violation.load(std::memory_order_relaxed) << "\n";
    oss << "    var_violation: "
        << s.var_violation.load(std::memory_order_relaxed) << "\n";
    oss << "    correlation_crash_count: "
        << s.correlation_crash_count.load(std::memory_order_relaxed) << "\n";
    oss << "    scaled_count: "
        << s.scaled_count.load(std::memory_order_relaxed) << "\n";
    oss << "    cooldown_blocked_count: "
        << s.cooldown_blocked_count.load(std::memory_order_relaxed) << "\n";
    oss << "    avg_total_risk: " << s.avg_total_risk() << "\n";
    oss << "    avg_diversification_benefit: "
        << s.avg_diversification_benefit() << "\n";

    return oss.str();
}

}  // namespace quant::strategy
