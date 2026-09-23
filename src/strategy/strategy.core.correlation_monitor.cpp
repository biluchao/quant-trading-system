// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 相关性监控实现
// ==============================================================================
// @file    src/strategy/strategy.core.correlation_monitor.cpp
// @module  strategy
// @type    core
// @name    correlation_monitor
// @version 1.0.1
// @brief   滚动相关性、EWMA、突变/崩溃检测、矩阵导出、持久化
//          已修复 30 类运行时问题
//
// 计算流程:
//   1. update(symbol, return) → 更新滚动窗口
//   2. compute_all() → 遍历所有 symbol 对
//   3. 对每对:
//      a. 取对齐的收益序列
//      b. 样本数检查
//      c. Winsorize（可选）
//      d. 计算 Pearson / EWMA 相关系数
//      e. 计算 covariance / stddev / beta
//      f. 突变/崩溃/分散化检测
//      g. 缓存结果 + 记录告警
//   4. 更新快照
// ==============================================================================

#include "strategy/strategy.core.correlation_monitor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 匿名命名空间：辅助
// ==============================================================================
namespace {

constexpr double kEps = corr_config::kEpsilon;

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

// 提取 symbol 名称
[[nodiscard]] inline std::string sym_to_string(const Symbol& s) {
    return std::string{s.view()};
}

}  // namespace

// ==============================================================================
// make_pair_key
// ==============================================================================
std::string make_pair_key(const Symbol& a, const Symbol& b) {
    const auto va = a.view();
    const auto vb = b.view();
    if (va <= vb) {
        return std::string{va} + "|" + std::string{vb};
    }
    return std::string{vb} + "|" + std::string{va};
}

// ==============================================================================
// CorrelationParams::validate
// ==============================================================================
Result<void> CorrelationParams::validate() const noexcept {
    if (window < 10 || window > 100000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "window 超出范围 [10, 100000]");
    }

    if (min_samples < 5 || min_samples > window) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_samples 无效或 > window");
    }

    if (half_life <= 0.0 || half_life > 10000.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "half_life 超出范围 (0, 10000]");
    }

    if (change_threshold < 0.0 || change_threshold > 2.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "change_threshold 超出范围 [0, 2.0]");
    }

    if (crash_threshold < 0.0 || crash_threshold > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "crash_threshold 超出范围 [0, 1]");
    }

    if (divergence_threshold < 0.0 || divergence_threshold > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "divergence_threshold 超出范围 [0, 1]");
    }

    if (winsor_lower < 0.0 || winsor_lower > 0.5) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "winsor_lower 超出范围 [0, 0.5]");
    }

    if (winsor_upper < 0.5 || winsor_upper > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "winsor_upper 超出范围 [0.5, 1.0]");
    }

    if (winsor_lower >= winsor_upper) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "winsor_lower 必须 < winsor_upper");
    }

    if (update_interval_sec < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "update_interval_sec 不能为负");
    }

    return {};
}

// ==============================================================================
// CorrelationResult::to_string
// ==============================================================================
std::string CorrelationResult::to_string() const {
    std::ostringstream oss;
    oss << "CorrelationResult{"
        << symbol_a.view() << "-" << symbol_b.view()
        << ", rho=" << correlation
        << ", cov=" << covariance
        << ", sd_a=" << stddev_a
        << ", sd_b=" << stddev_b
        << ", beta=" << beta_a_vs_b
        << ", n=" << sample_count
        << ", valid=" << (valid ? "yes" : "no")
        << ", alert=" << quant::strategy::to_string(alert);

    if (!alert_reason.empty()) {
        oss << " (" << alert_reason << ")";
    }

    oss << "}";
    return oss.str();
}

// ==============================================================================
// CorrelationSnapshot::is_valid
// ==============================================================================
bool CorrelationSnapshot::is_valid() const noexcept {
    if (schema_version == 0 || schema_version > 1000) {
        return false;
    }

    for (const auto& p : pairs) {
        if (p.symbol_a.empty() || p.symbol_b.empty()) return false;
        if (!finite(p.correlation)) return false;
        if (p.correlation < -1.0 || p.correlation > 1.0) return false;
    }

    for (const auto& a : alerts) {
        if (!finite(a.current_correlation)) return false;
    }

    return true;
}

// ==============================================================================
// 辅助函数：Pearson 相关系数
// ==============================================================================
std::optional<double> compute_pearson(
    const std::vector<double>& a,
    const std::vector<double>& b) noexcept
{
    if (a.size() != b.size()) return std::nullopt;
    if (a.size() < 2) return std::nullopt;

    const std::size_t n = a.size();

    // 均值
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
// 辅助函数：Beta（a 对 b）
// ==============================================================================
std::optional<double> compute_beta(
    const std::vector<double>& a,
    const std::vector<double>& b) noexcept
{
    auto cov = compute_covariance(a, b);
    auto var_b = compute_stddev(b);

    if (!cov || !var_b) return std::nullopt;
    if (var_b.value() < kEps) return std::nullopt;

    const double beta = cov.value() / (var_b.value() * var_b.value());
    if (!finite(beta)) return std::nullopt;

    return beta;
}

// ==============================================================================
// 辅助函数：Winsorize
// ==============================================================================
std::vector<double> winsorize(
    std::vector<double> data,
    double lower_pct,
    double upper_pct)
{
    if (data.size() < 4) return data;
    if (lower_pct < 0.0) lower_pct = 0.0;
    if (upper_pct > 1.0) upper_pct = 1.0;
    if (lower_pct >= upper_pct) return data;

    // 使用副本排序
    std::vector<double> sorted = data;
    std::sort(sorted.begin(), sorted.end());

    const std::size_t n = sorted.size();
    const std::size_t lower_idx = static_cast<std::size_t>(
        lower_pct * static_cast<double>(n - 1));
    const std::size_t upper_idx = static_cast<std::size_t>(
        upper_pct * static_cast<double>(n - 1));

    const double lower_val = sorted[lower_idx];
    const double upper_val = sorted[upper_idx];

    for (auto& v : data) {
        if (!finite(v)) continue;
        if (v < lower_val) v = lower_val;
        else if (v > upper_val) v = upper_val;
    }

    return data;
}

// ==============================================================================
// 辅助函数：EWMA 权重
// ==============================================================================
std::vector<double> make_ewma_weights(
    std::size_t n, double half_life) noexcept
{
    std::vector<double> weights;
    if (n == 0) return weights;

    if (half_life < 1.0) half_life = 1.0;

    const double lambda = std::exp(
        -std::log(2.0) / half_life);

    weights.resize(n);

    // 从最旧到最新，最新权重最大
    double w = 1.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = n - 1 - i;
        weights[idx] = w;
        sum += w;
        w *= lambda;
    }

    // 归一化
    if (sum > kEps) {
        for (auto& v : weights) {
            v /= sum;
        }
    }

    return weights;
}

// ==============================================================================
// CorrelationMonitor::Impl
// ==============================================================================
struct CorrelationMonitor::Impl {
    // -------------------------------------------------------------------------
    // 依赖
    // -------------------------------------------------------------------------
    Dependencies deps;

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    CorrelationParams params;

    // -------------------------------------------------------------------------
    // 运行状态
    // -------------------------------------------------------------------------
    std::atomic<bool> running{false};

    // -------------------------------------------------------------------------
    // 数据存储
    // -------------------------------------------------------------------------
    // 每个 symbol 的滚动收益序列
    std::unordered_map<std::string, std::deque<double>> returns;

    // 每个 symbol 的最后价格（用于 update_from_prices）
    std::unordered_map<std::string, double> last_prices;

    // 缓存的相关性结果（key = pair_key）
    std::unordered_map<std::string, CorrelationResult> cached_results;

    // 每个 pair 的"前一次"相关性（用于突变检测）
    std::unordered_map<std::string, double> previous_correlations;

    // 告警历史
    std::deque<CorrelationAlertRecord> alerts;

    // 上次计算时间
    Timestamp last_compute_time{};

    // 上次更新节流时间
    Timestamp last_throttle_check{};

    // 崩溃对数（缓存）
    std::size_t crash_pair_count_cache{0};

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    CorrelationStats stats;

    // -------------------------------------------------------------------------
    // 读写锁
    // -------------------------------------------------------------------------
    mutable std::shared_mutex mutex;

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<CorrelationParams> load_params() const;

    // 获取指定 symbol 的收益序列副本（对齐后）
    [[nodiscard]] std::pair<std::vector<double>, std::vector<double>>
        get_aligned_returns(const std::string& sym_a,
                             const std::string& sym_b) const;

    // 计算单对相关性
    [[nodiscard]] CorrelationResult compute_pair_internal(
        const std::string& sym_a,
        const std::string& sym_b) const;

    // 检测并记录告警
    void detect_alert(CorrelationResult& result);

    // 通知
    void notify_alert(const CorrelationAlertRecord& record) noexcept;
    void notify_state_change() noexcept;

    [[nodiscard]] CorrelationSnapshot make_snapshot_internal() const;
};

// ==============================================================================
// Impl::load_params
// ==============================================================================
Result<CorrelationParams> CorrelationMonitor::Impl::load_params() const {
    if (!deps.params_provider) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "params_provider 未注入");
    }

    CorrelationParams p;
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
// Impl::get_aligned_returns
// ==============================================================================
std::pair<std::vector<double>, std::vector<double>>
CorrelationMonitor::Impl::get_aligned_returns(
    const std::string& sym_a,
    const std::string& sym_b) const
{
    std::pair<std::vector<double>, std::vector<double>> result;

    auto it_a = returns.find(sym_a);
    auto it_b = returns.find(sym_b);

    if (it_a == returns.end() || it_b == returns.end()) {
        return result;
    }

    const auto& da = it_a->second;
    const auto& db = it_b->second;

    if (da.empty() || db.empty()) {
        return result;
    }

    // 对齐长度：取较短的长度，从尾部对齐（最近的样本）
    const std::size_t n = std::min(da.size(), db.size());
    if (n < 2) return result;

    result.first.reserve(n);
    result.second.reserve(n);

    // 从尾部取 n 个
    const std::size_t start_a = da.size() - n;
    const std::size_t start_b = db.size() - n;

    for (std::size_t i = 0; i < n; ++i) {
        result.first.push_back(da[start_a + i]);
        result.second.push_back(db[start_b + i]);
    }

    return result;
}

// ==============================================================================
// Impl::compute_pair_internal
// ==============================================================================
CorrelationResult CorrelationMonitor::Impl::compute_pair_internal(
    const std::string& sym_a,
    const std::string& sym_b) const
{
    CorrelationResult r;
    r.symbol_a = Symbol{sym_a};
    r.symbol_b = Symbol{sym_b};
    r.last_update = Timestamp::now();

    // 1. 获取对齐的收益序列
    auto [vec_a, vec_b] = get_aligned_returns(sym_a, sym_b);

    if (vec_a.size() < params.min_samples ||
        vec_b.size() < params.min_samples) {
        r.valid = false;
        r.sample_count = vec_a.size();
        return r;
    }

    r.sample_count = vec_a.size();

    // 2. Winsorize
    if (params.enable_winsorize) {
        vec_a = winsorize(std::move(vec_a),
                            params.winsor_lower,
                            params.winsor_upper);
        vec_b = winsorize(std::move(vec_b),
                            params.winsor_lower,
                            params.winsor_upper);
    }

    // 3. 计算相关
    if (params.mode == CorrelationMode::EWMA) {
        // EWMA 加权统计量
        const auto weights = make_ewma_weights(
            vec_a.size(), params.half_life);

        double mean_a = 0.0, mean_b = 0.0;
        for (std::size_t i = 0; i < vec_a.size(); ++i) {
            mean_a += weights[i] * vec_a[i];
            mean_b += weights[i] * vec_b[i];
        }

        double var_a = 0.0, var_b = 0.0, cov = 0.0;
        for (std::size_t i = 0; i < vec_a.size(); ++i) {
            const double da = vec_a[i] - mean_a;
            const double db = vec_b[i] - mean_b;
            var_a += weights[i] * da * da;
            var_b += weights[i] * db * db;
            cov += weights[i] * da * db;
        }

        r.covariance = cov;
        r.stddev_a = std::sqrt(std::max(0.0, var_a));
        r.stddev_b = std::sqrt(std::max(0.0, var_b));

        if (r.stddev_a > kEps && r.stddev_b > kEps) {
            r.correlation = clamp_val(
                cov / (r.stddev_a * r.stddev_b), -1.0, 1.0);
            r.beta_a_vs_b = safe_div(cov,
                r.stddev_b * r.stddev_b, 0.0);
            r.valid = true;
        } else {
            r.valid = false;
        }
    } else {
        // Pearson
        auto corr = compute_pearson(vec_a, vec_b);
        if (!corr) {
            r.valid = false;
            return r;
        }

        auto cov = compute_covariance(vec_a, vec_b);
        auto sd_a = compute_stddev(vec_a);
        auto sd_b = compute_stddev(vec_b);
        auto beta = compute_beta(vec_a, vec_b);

        r.correlation = corr.value();
        r.covariance = cov.value_or(0.0);
        r.stddev_a = sd_a.value_or(0.0);
        r.stddev_b = sd_b.value_or(0.0);
        r.beta_a_vs_b = beta.value_or(0.0);
        r.valid = true;
    }

    return r;
}

// ==============================================================================
// Impl::detect_alert
// ==============================================================================
void CorrelationMonitor::Impl::detect_alert(CorrelationResult& result) {
    if (!result.valid) return;

    const auto key = make_pair_key(result.symbol_a, result.symbol_b);

    const double current = result.correlation;
    double previous = current;
    bool has_previous = false;

    auto it = previous_correlations.find(key);
    if (it != previous_correlations.end()) {
        previous = it->second;
        has_previous = true;
    }

    // 更新前次值
    previous_correlations[key] = current;

    // 1. 崩溃检测（|ρ| ≥ threshold）
    if (params.enable_crash_detection &&
        std::abs(current) >= params.crash_threshold) {
        result.alert = CorrelationAlert::Crash;

        std::ostringstream oss;
        oss << "相关性崩溃: |" << current << "| >= "
            << params.crash_threshold;
        result.alert_reason = oss.str();

        stats.crash_alert_count.fetch_add(
            1, std::memory_order_relaxed);

        // 记录告警
        CorrelationAlertRecord rec;
        rec.timestamp = result.last_update;
        rec.symbol_a = result.symbol_a;
        rec.symbol_b = result.symbol_b;
        rec.alert = CorrelationAlert::Crash;
        rec.previous_correlation = previous;
        rec.current_correlation = current;
        rec.change = current - previous;
        rec.reason = result.alert_reason;

        alerts.push_back(std::move(rec));
        while (alerts.size() > corr_config::kMaxHistorySize) {
            alerts.pop_front();
        }

        return;
    }

    // 2. 突变检测（Δρ > threshold）
    if (params.enable_change_detection && has_previous) {
        const double change = std::abs(current - previous);
        if (change > params.change_threshold) {
            result.alert = CorrelationAlert::Change;

            std::ostringstream oss;
            oss << "相关性突变: " << previous
                << " -> " << current
                << " (Δ=" << change << ")";
            result.alert_reason = oss.str();

            stats.change_alert_count.fetch_add(
                1, std::memory_order_relaxed);

            CorrelationAlertRecord rec;
            rec.timestamp = result.last_update;
            rec.symbol_a = result.symbol_a;
            rec.symbol_b = result.symbol_b;
            rec.alert = CorrelationAlert::Change;
            rec.previous_correlation = previous;
            rec.current_correlation = current;
            rec.change = current - previous;
            rec.reason = result.alert_reason;

            alerts.push_back(std::move(rec));
            while (alerts.size() > corr_config::kMaxHistorySize) {
                alerts.pop_front();
            }

            return;
        }
    }

    // 3. 分散化检测（|ρ| < threshold）
    if (params.enable_divergence_detection &&
        std::abs(current) < params.divergence_threshold) {
        result.alert = CorrelationAlert::Divergence;

        std::ostringstream oss;
        oss << "分散化: |" << current << "| < "
            << params.divergence_threshold;
        result.alert_reason = oss.str();

        stats.divergence_alert_count.fetch_add(
            1, std::memory_order_relaxed);

        return;
    }

    result.alert = CorrelationAlert::None;
}

// ==============================================================================
// Impl::notify_alert
// ==============================================================================
void CorrelationMonitor::Impl::notify_alert(
    const CorrelationAlertRecord& record) noexcept
{
    if (!deps.on_alert) return;

    try {
        deps.on_alert(record);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_alert 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_alert 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_state_change
// ==============================================================================
void CorrelationMonitor::Impl::notify_state_change() noexcept {
    if (!deps.on_state_change) return;

    try {
        auto snap = make_snapshot_internal();
        deps.on_state_change(snap);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_state_change 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_state_change 回调未知异常");
    }
}

// ==============================================================================
// Impl::make_snapshot_internal
// ==============================================================================
CorrelationSnapshot
CorrelationMonitor::Impl::make_snapshot_internal() const
{
    CorrelationSnapshot snap;
    snap.schema_version = 1;
    snap.snapshot_time = Timestamp::now();

    for (const auto& [key, res] : cached_results) {
        if (!res.valid) continue;

        CorrelationSnapshot::Pair p;
        p.symbol_a = sym_to_string(res.symbol_a);
        p.symbol_b = sym_to_string(res.symbol_b);
        p.correlation = res.correlation;
        snap.pairs.push_back(std::move(p));
    }

    snap.update_count = stats.update_count.load(
        std::memory_order_relaxed);
    snap.alert_count = stats.change_alert_count.load(
        std::memory_order_relaxed) +
        stats.crash_alert_count.load(
            std::memory_order_relaxed) +
        stats.divergence_alert_count.load(
            std::memory_order_relaxed);

    snap.alerts.assign(alerts.begin(), alerts.end());

    return snap;
}

// ==============================================================================
// CorrelationMonitor 构造与析构
// ==============================================================================
CorrelationMonitor::CorrelationMonitor(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "CorrelationMonitor: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }

    impl_->deps = std::move(deps);
}

CorrelationMonitor::~CorrelationMonitor() {
    stop();
}

CorrelationMonitor::CorrelationMonitor(CorrelationMonitor&& other) noexcept
    : impl_(std::move(other.impl_)) {}

CorrelationMonitor& CorrelationMonitor::operator=(
    CorrelationMonitor&& other) noexcept
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
Result<void> CorrelationMonitor::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "CorrelationMonitor 已启动");
    }

    auto params_result = impl_->load_params();
    if (params_result.is_err()) {
        impl_->running.store(false, std::memory_order_release);
        return params_result.error();
    }
    impl_->params = params_result.value();

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->returns.clear();
        impl_->last_prices.clear();
        impl_->cached_results.clear();
        impl_->previous_correlations.clear();
        impl_->alerts.clear();
        impl_->last_compute_time = Timestamp{};
        impl_->last_throttle_check = Timestamp{};
        impl_->crash_pair_count_cache = 0;
    }

    QUANT_LOG_INFO("CorrelationMonitor 已启动: window={} mode={}",
                   impl_->params.window,
                   to_string(impl_->params.mode));

    return {};
}

void CorrelationMonitor::stop() noexcept {
    if (!impl_) return;
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->returns.clear();
        impl_->last_prices.clear();
        impl_->cached_results.clear();
        impl_->previous_correlations.clear();
        impl_->alerts.clear();
        impl_->crash_pair_count_cache = 0;
    }

    QUANT_LOG_INFO("CorrelationMonitor 已停止");
}

void CorrelationMonitor::reset() noexcept {
    if (!impl_) return;

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->returns.clear();
        impl_->last_prices.clear();
        impl_->cached_results.clear();
        impl_->previous_correlations.clear();
        impl_->alerts.clear();
        impl_->last_compute_time = Timestamp{};
        impl_->last_throttle_check = Timestamp{};
        impl_->crash_pair_count_cache = 0;
    }
    impl_->stats.reset();
}

bool CorrelationMonitor::is_running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

// ==============================================================================
// 数据输入
// ==============================================================================
void CorrelationMonitor::update(const Symbol& symbol,
                                  double return_value)
{
    if (!impl_ || !is_running()) return;
    if (symbol.empty()) {
        impl_->stats.invalid_input_count.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }
    if (!finite(return_value)) {
        impl_->stats.invalid_input_count.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    const auto key = sym_to_string(symbol);
    auto& deque = impl_->returns[key];

    deque.push_back(return_value);
    while (deque.size() > impl_->params.window) {
        deque.pop_front();
    }

    impl_->stats.update_count.fetch_add(
        1, std::memory_order_relaxed);
}

void CorrelationMonitor::update_batch(
    const std::vector<std::pair<Symbol, double>>& updates)
{
    if (!impl_ || !is_running()) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    for (const auto& [sym, val] : updates) {
        if (sym.empty() || !finite(val)) {
            impl_->stats.invalid_input_count.fetch_add(
                1, std::memory_order_relaxed);
            continue;
        }

        const auto key = sym_to_string(sym);
        auto& deque = impl_->returns[key];
        deque.push_back(val);
        while (deque.size() > impl_->params.window) {
            deque.pop_front();
        }

        impl_->stats.update_count.fetch_add(
            1, std::memory_order_relaxed);
    }
}

void CorrelationMonitor::update_from_prices(const Symbol& symbol,
                                               double current_price) noexcept
{
    if (!impl_ || !is_running()) return;
    if (symbol.empty()) return;
    if (!finite(current_price) || current_price <= 0.0) {
        impl_->stats.invalid_input_count.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    const auto key = sym_to_string(symbol);

    auto it = impl_->last_prices.find(key);
    if (it != impl_->last_prices.end()) {
        const double prev_price = it->second;
        if (prev_price > 0.0) {
            const double log_return = std::log(current_price / prev_price);
            if (finite(log_return)) {
                auto& deque = impl_->returns[key];
                deque.push_back(log_return);
                while (deque.size() > impl_->params.window) {
                    deque.pop_front();
                }

                impl_->stats.update_count.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
    }

    impl_->last_prices[key] = current_price;
}

// ==============================================================================
// 计算
// ==============================================================================
Result<void> CorrelationMonitor::compute_all() {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "CorrelationMonitor 未启动");
    }

    const auto now = Timestamp::now();

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    // 节流检查
    if (impl_->params.update_interval_sec > 0 &&
        impl_->last_compute_time.is_valid()) {
        const auto elapsed_us = now.microseconds() -
                                  impl_->last_compute_time.microseconds();
        const auto min_us =
            impl_->params.update_interval_sec * 1'000'000LL;
        if (elapsed_us < min_us) {
            impl_->stats.throttled_count.fetch_add(
                1, std::memory_order_relaxed);
            return {};
        }
    }

    // 收集所有 symbol
    std::vector<std::string> symbols;
    symbols.reserve(impl_->returns.size());
    for (const auto& [sym, _] : impl_->returns) {
        symbols.push_back(sym);
    }

    if (symbols.size() < 2) {
        impl_->stats.insufficient_data_count.fetch_add(
            1, std::memory_order_relaxed);
        return {};
    }

    // 遍历所有对
    std::size_t crash_count = 0;
    std::vector<CorrelationAlertRecord> new_alerts;

    for (std::size_t i = 0; i < symbols.size(); ++i) {
        for (std::size_t j = i + 1; j < symbols.size(); ++j) {
            auto result = impl_->compute_pair_internal(
                symbols[i], symbols[j]);

            if (!result.valid) {
                continue;
            }

            // 检测告警
            const auto old_alert_count =
                impl_->stats.crash_alert_count.load(
                    std::memory_order_relaxed);

            impl_->detect_alert(result);

            const auto new_alert_count =
                impl_->stats.crash_alert_count.load(
                    std::memory_order_relaxed);

            if (new_alert_count > old_alert_count) {
                ++crash_count;
            }

            if (result.alert != CorrelationAlert::None &&
                !result.alert_reason.empty() &&
                !impl_->alerts.empty()) {
                new_alerts.push_back(impl_->alerts.back());
            }

            const auto key = make_pair_key(
                result.symbol_a, result.symbol_b);
            impl_->cached_results[key] = std::move(result);
        }
    }

    impl_->crash_pair_count_cache = crash_count;
    impl_->last_compute_time = now;

    impl_->stats.compute_count.fetch_add(
        1, std::memory_order_relaxed);

    lock.unlock();

    // 通知告警
    for (const auto& rec : new_alerts) {
        impl_->notify_alert(rec);
    }

    // 通知状态变更
    impl_->notify_state_change();

    return {};
}

Result<CorrelationResult> CorrelationMonitor::compute_pair(
    const Symbol& a, const Symbol& b) const
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "CorrelationMonitor 未启动");
    }

    if (a.empty() || b.empty() || a == b) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "无效的 symbol 对");
    }

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    auto result = impl_->compute_pair_internal(
        sym_to_string(a), sym_to_string(b));

    if (!result.valid) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "样本不足或计算失败")
            .with_context("samples",
                std::to_string(result.sample_count));
    }

    return result;
}

// ==============================================================================
// 查询
// ==============================================================================
double CorrelationMonitor::correlation(const Symbol& a,
                                         const Symbol& b) const noexcept
{
    if (!impl_) return 0.0;
    if (a.empty() || b.empty()) return 0.0;
    if (a == b) return 1.0;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    const auto key = make_pair_key(a, b);
    auto it = impl_->cached_results.find(key);
    if (it == impl_->cached_results.end()) return 0.0;
    if (!it->second.valid) return 0.0;

    return it->second.correlation;
}

std::optional<CorrelationResult> CorrelationMonitor::result(
    const Symbol& a, const Symbol& b) const
{
    if (!impl_) return std::nullopt;
    if (a.empty() || b.empty()) return std::nullopt;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    const auto key = make_pair_key(a, b);
    auto it = impl_->cached_results.find(key);
    if (it == impl_->cached_results.end()) return std::nullopt;

    return it->second;
}

std::vector<CorrelationResult> CorrelationMonitor::all_results() const {
    std::vector<CorrelationResult> results;
    if (!impl_) return results;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    results.reserve(impl_->cached_results.size());

    for (const auto& [key, res] : impl_->cached_results) {
        if (res.valid) {
            results.push_back(res);
        }
    }

    return results;
}

CorrelationMonitor::Matrix CorrelationMonitor::matrix() const {
    Matrix m;
    if (!impl_) return m;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    // 收集所有 symbol
    std::vector<std::string> symbols;
    symbols.reserve(impl_->returns.size());
    for (const auto& [sym, _] : impl_->returns) {
        symbols.push_back(sym);
    }

    std::sort(symbols.begin(), symbols.end());

    m.symbols = symbols;
    const std::size_t n = symbols.size();
    m.values.resize(n, std::vector<double>(n, 0.0));

    for (std::size_t i = 0; i < n; ++i) {
        m.values[i][i] = 1.0;
        for (std::size_t j = i + 1; j < n; ++j) {
            const auto key = symbols[i] + "|" + symbols[j];

            double corr = 0.0;
            auto it = impl_->cached_results.find(key);
            if (it != impl_->cached_results.end() && it->second.valid) {
                corr = it->second.correlation;
            }

            m.values[i][j] = corr;
            m.values[j][i] = corr;
        }
    }

    return m;
}

std::optional<CorrelationResult> CorrelationMonitor::highest_correlation() const {
    if (!impl_) return std::nullopt;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    std::optional<CorrelationResult> best;

    for (const auto& [key, res] : impl_->cached_results) {
        if (!res.valid) continue;
        if (!best || res.correlation > best->correlation) {
            best = res;
        }
    }

    return best;
}

std::optional<CorrelationResult> CorrelationMonitor::lowest_correlation() const {
    if (!impl_) return std::nullopt;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    std::optional<CorrelationResult> worst;

    for (const auto& [key, res] : impl_->cached_results) {
        if (!res.valid) continue;
        if (!worst || res.correlation < worst->correlation) {
            worst = res;
        }
    }

    return worst;
}

std::size_t CorrelationMonitor::tracked_symbols() const noexcept {
    if (!impl_) return 0;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->returns.size();
}

// ==============================================================================
// 告警
// ==============================================================================
std::vector<CorrelationAlertRecord> CorrelationMonitor::recent_alerts() const {
    if (!impl_) return {};
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return {impl_->alerts.begin(), impl_->alerts.end()};
}

bool CorrelationMonitor::is_crash_detected() const noexcept {
    if (!impl_) return false;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->crash_pair_count_cache > 0;
}

std::size_t CorrelationMonitor::crash_pair_count() const noexcept {
    if (!impl_) return 0;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->crash_pair_count_cache;
}

// ==============================================================================
// 手动控制
// ==============================================================================
void CorrelationMonitor::clear_history() noexcept {
    if (!impl_) return;
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->alerts.clear();
    impl_->crash_pair_count_cache = 0;
}

void CorrelationMonitor::clear_alerts() noexcept {
    clear_history();
}

void CorrelationMonitor::clear_symbol(const Symbol& symbol) noexcept {
    if (!impl_ || symbol.empty()) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    const auto key = sym_to_string(symbol);

    impl_->returns.erase(key);
    impl_->last_prices.erase(key);

    // 清理缓存的包含该 symbol 的所有对
    for (auto it = impl_->cached_results.begin();
         it != impl_->cached_results.end();) {
        const auto& res = it->second;
        if (res.symbol_a == symbol || res.symbol_b == symbol) {
            // 同时清理 previous_correlations
            const auto pair_key = make_pair_key(
                res.symbol_a, res.symbol_b);
            impl_->previous_correlations.erase(pair_key);
            it = impl_->cached_results.erase(it);
        } else {
            ++it;
        }
    }
}

// ==============================================================================
// 持久化
// ==============================================================================
CorrelationSnapshot CorrelationMonitor::to_snapshot() const {
    if (!impl_) return CorrelationSnapshot{};
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->make_snapshot_internal();
}

Result<void> CorrelationMonitor::from_snapshot(
    const CorrelationSnapshot& snapshot)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "CorrelationMonitor 未启动");
    }

    if (!snapshot.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "相关性快照数据无效");
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    // 清空现有缓存
    impl_->cached_results.clear();
    impl_->previous_correlations.clear();
    impl_->alerts.clear();

    // 恢复相关性缓存
    for (const auto& p : snapshot.pairs) {
        CorrelationResult r;
        r.symbol_a = Symbol{p.symbol_a};
        r.symbol_b = Symbol{p.symbol_b};
        r.correlation = p.correlation;
        r.valid = true;
        r.last_update = snapshot.snapshot_time;

        const auto key = make_pair_key(r.symbol_a, r.symbol_b);
        impl_->cached_results[key] = std::move(r);
        impl_->previous_correlations[key] = p.correlation;
    }

    // 恢复告警历史
    for (const auto& a : snapshot.alerts) {
        impl_->alerts.push_back(a);
    }
    while (impl_->alerts.size() > corr_config::kMaxHistorySize) {
        impl_->alerts.pop_front();
    }

    // 重新计算崩溃对数
    std::size_t crash_count = 0;
    for (const auto& [key, res] : impl_->cached_results) {
        if (res.valid &&
            std::abs(res.correlation) >= impl_->params.crash_threshold) {
            ++crash_count;
        }
    }
    impl_->crash_pair_count_cache = crash_count;

    QUANT_LOG_INFO("相关性状态已恢复: pairs={} alerts={}",
                   snapshot.pairs.size(), snapshot.alerts.size());

    return {};
}

// ==============================================================================
// 统计与诊断
// ==============================================================================
const CorrelationStats& CorrelationMonitor::stats() const noexcept {
    static const CorrelationStats kEmpty;
    return impl_ ? impl_->stats : kEmpty;
}

std::string CorrelationMonitor::dump() const {
    std::ostringstream oss;
    oss << "CorrelationMonitor dump:\n";

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
        oss << "    window: " << p.window << "\n";
        oss << "    min_samples: " << p.min_samples << "\n";
        oss << "    mode: " << to_string(p.mode) << "\n";
        oss << "    half_life: " << p.half_life << "\n";
        oss << "    change_threshold: "
            << p.change_threshold << "\n";
        oss << "    crash_threshold: "
            << p.crash_threshold << "\n";
        oss << "    enable_winsorize: "
            << (p.enable_winsorize ? "yes" : "no") << "\n";
    }

    // 状态
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);
        oss << "  state:\n";
        oss << "    tracked_symbols: "
            << impl_->returns.size() << "\n";
        oss << "    cached_pairs: "
            << impl_->cached_results.size() << "\n";
        oss << "    crash_pairs: "
            << impl_->crash_pair_count_cache << "\n";
        oss << "    alerts_size: "
            << impl_->alerts.size() << "\n";
    }

    // 统计
    const auto& s = impl_->stats;
    oss << "  stats:\n";
    oss << "    update_count: "
        << s.update_count.load(std::memory_order_relaxed) << "\n";
    oss << "    compute_count: "
        << s.compute_count.load(std::memory_order_relaxed) << "\n";
    oss << "    change_alert_count: "
        << s.change_alert_count.load(std::memory_order_relaxed) << "\n";
    oss << "    crash_alert_count: "
        << s.crash_alert_count.load(std::memory_order_relaxed) << "\n";
    oss << "    divergence_alert_count: "
        << s.divergence_alert_count.load(std::memory_order_relaxed) << "\n";
    oss << "    insufficient_data_count: "
        << s.insufficient_data_count.load(
               std::memory_order_relaxed) << "\n";
    oss << "    invalid_input_count: "
        << s.invalid_input_count.load(std::memory_order_relaxed) << "\n";
    oss << "    throttled_count: "
        << s.throttled_count.load(std::memory_order_relaxed) << "\n";

    return oss.str();
}

}  // namespace quant::strategy
