// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 相关性监控
// ==============================================================================
// @file    src/strategy/strategy.core.correlation_monitor.hpp
// @module  strategy
// @type    core
// @name    correlation_monitor
// @version 1.0.1
// @brief   多品种滚动相关性、突变检测、崩溃检测、EWMA 支持
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 多品种矩阵：Symbol 对为键，对称存储
//   - 滚动窗口：固定窗口滑动
//   - EWMA 支持：指数加权，半衰期可配
//   - 突变检测：相邻窗口变化 > 阈值告警
//   - 崩溃检测：|ρ| > threshold 触发降仓
//   - 增量更新：仅重算受影响的对
//   - 异常值过滤：Winsorize 或 MAD
//   - 完整追踪：历史 + 统计 + 快照
//   - 可观测：dump + 矩阵导出
//   - 参数可配：所有阈值从 ConfigManager 注入
//
// 相关性计算:
//   Pearson: ρ = Cov(a,b) / (σ_a × σ_b)
//   EWMA:    ρ_t = λ × ρ_{t-1} + (1-λ) × ρ_sample
//            λ = exp(-ln(2) / half_life)
//
// 突变检测:
//   Δρ = |ρ_current - ρ_previous|
//   若 Δρ > change_threshold → 触发告警
//
// 崩溃检测:
//   若 |ρ| ≥ crash_threshold → 触发降仓
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_CORRELATION_MONITOR_HPP
#define QUANT_STRATEGY_CORE_CORRELATION_MONITOR_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <cmath>
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
#include <unordered_set>
#include <utility>
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
namespace corr_config {

// 默认滚动窗口
inline constexpr std::size_t kDefaultWindow = 100;

// 最小样本数
inline constexpr std::size_t kDefaultMinSamples = 20;

// 默认突变阈值
inline constexpr double kDefaultChangeThreshold = 0.3;

// 默认崩溃阈值
inline constexpr double kDefaultCrashThreshold = 0.9;

// 默认 EWMA 半衰期（样本数）
inline constexpr double kDefaultHalfLife = 20.0;

// 默认更新间隔（秒），0 表示每次更新
inline constexpr std::int64_t kDefaultUpdateIntervalSec = 0;

// 历史窗口大小
inline constexpr std::size_t kMaxHistorySize = 256;

// 浮点保护
inline constexpr double kEpsilon = 1e-9;

// Winsorize 分位数
inline constexpr double kDefaultWinsorLower = 0.01;
inline constexpr double kDefaultWinsorUpper = 0.99;

}  // namespace corr_config

// ==============================================================================
// 相关性计算模式
// ==============================================================================
enum class CorrelationMode : std::uint8_t {
    Pearson   = 0,   // 简单 Pearson
    EWMA      = 1,   // 指数加权
    Spearman  = 2,   // 秩相关（预留）
};

[[nodiscard]] constexpr std::string_view to_string(
    CorrelationMode m) noexcept
{
    switch (m) {
        case CorrelationMode::Pearson:  return "Pearson";
        case CorrelationMode::EWMA:     return "EWMA";
        case CorrelationMode::Spearman: return "Spearman";
    }
    return "Unknown";
}

// ==============================================================================
// 相关性告警类型
// ==============================================================================
enum class CorrelationAlert : std::uint8_t {
    None       = 0,
    Change     = 1,   // 突变
    Crash      = 2,   // 崩溃（ρ → 1）
    Divergence = 3,   // 分散化（ρ → 0 或负）
};

[[nodiscard]] constexpr std::string_view to_string(
    CorrelationAlert a) noexcept
{
    switch (a) {
        case CorrelationAlert::None:       return "None";
        case CorrelationAlert::Change:     return "Change";
        case CorrelationAlert::Crash:      return "Crash";
        case CorrelationAlert::Divergence: return "Divergence";
    }
    return "Unknown";
}

// ==============================================================================
// 相关性参数
// ==============================================================================
struct CorrelationParams {
    // 滚动窗口
    std::size_t window{corr_config::kDefaultWindow};
    std::size_t min_samples{corr_config::kDefaultMinSamples};

    // 计算模式
    CorrelationMode mode{CorrelationMode::Pearson};

    // EWMA 半衰期
    double half_life{corr_config::kDefaultHalfLife};

    // 突变检测
    bool enable_change_detection{true};
    double change_threshold{corr_config::kDefaultChangeThreshold};

    // 崩溃检测
    bool enable_crash_detection{true};
    double crash_threshold{corr_config::kDefaultCrashThreshold};

    // 分散化检测
    bool enable_divergence_detection{false};
    double divergence_threshold{0.1};

    // 异常值过滤
    bool enable_winsorize{false};
    double winsor_lower{corr_config::kDefaultWinsorLower};
    double winsor_upper{corr_config::kDefaultWinsorUpper};

    // 更新节流
    std::int64_t update_interval_sec{
        corr_config::kDefaultUpdateIntervalSec};

    [[nodiscard]] Result<void> validate() const noexcept;

    [[nodiscard]] static CorrelationParams defaults() noexcept {
        return CorrelationParams{};
    }
};

// ==============================================================================
// 单品种对相关性结果
// ==============================================================================
struct CorrelationResult {
    Symbol symbol_a;
    Symbol symbol_b;

    // 相关性
    double correlation{0.0};        // [-1, 1]
    double covariance{0.0};          // Cov(a, b)
    double stddev_a{0.0};
    double stddev_b{0.0};

    // Beta（以 b 为基准，a 对 b 的 Beta）
    double beta_a_vs_b{0.0};

    // 样本数
    std::size_t sample_count{0};

    // 有效
    bool valid{false};

    // 告警
    CorrelationAlert alert{CorrelationAlert::None};
    std::string alert_reason;

    // 上次更新
    Timestamp last_update{};

    [[nodiscard]] bool is_crash() const noexcept {
        return alert == CorrelationAlert::Crash;
    }

    [[nodiscard]] bool is_diverged() const noexcept {
        return alert == CorrelationAlert::Divergence;
    }

    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 相关性告警记录
// ==============================================================================
struct CorrelationAlertRecord {
    Timestamp timestamp{};
    Symbol symbol_a;
    Symbol symbol_b;
    CorrelationAlert alert{CorrelationAlert::None};
    double previous_correlation{0.0};
    double current_correlation{0.0};
    double change{0.0};
    std::string reason;
};

// ==============================================================================
// 相关性快照（持久化）
// ==============================================================================
struct CorrelationSnapshot {
    std::uint32_t schema_version{1};
    Timestamp snapshot_time{};

    struct Pair {
        std::string symbol_a;
        std::string symbol_b;
        double correlation{0.0};
    };
    std::vector<Pair> pairs;

    std::uint64_t update_count{0};
    std::uint64_t alert_count{0};

    std::vector<CorrelationAlertRecord> alerts;

    [[nodiscard]] bool is_valid() const noexcept;
};

// ==============================================================================
// 相关性统计
// ==============================================================================
struct CorrelationStats {
    std::atomic<std::uint64_t> update_count{0};
    std::atomic<std::uint64_t> compute_count{0};
    std::atomic<std::uint64_t> change_alert_count{0};
    std::atomic<std::uint64_t> crash_alert_count{0};
    std::atomic<std::uint64_t> divergence_alert_count{0};
    std::atomic<std::uint64_t> insufficient_data_count{0};
    std::atomic<std::uint64_t> invalid_input_count{0};
    std::atomic<std::uint64_t> throttled_count{0};

    void reset() noexcept {
        update_count.store(0, std::memory_order_relaxed);
        compute_count.store(0, std::memory_order_relaxed);
        change_alert_count.store(0, std::memory_order_relaxed);
        crash_alert_count.store(0, std::memory_order_relaxed);
        divergence_alert_count.store(0, std::memory_order_relaxed);
        insufficient_data_count.store(0, std::memory_order_relaxed);
        invalid_input_count.store(0, std::memory_order_relaxed);
        throttled_count.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// 相关性监控器
// ==============================================================================
class CorrelationMonitor {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 参数提供者（必需）
        std::function<CorrelationParams()> params_provider;

        // 告警回调（可选）
        std::function<void(const CorrelationAlertRecord&)> on_alert;

        // 状态变更回调（可选）
        std::function<void(const CorrelationSnapshot&)> on_state_change;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit CorrelationMonitor(Dependencies deps);
    ~CorrelationMonitor();

    CorrelationMonitor(const CorrelationMonitor&) = delete;
    CorrelationMonitor& operator=(const CorrelationMonitor&) = delete;
    CorrelationMonitor(CorrelationMonitor&&) noexcept;
    CorrelationMonitor& operator=(CorrelationMonitor&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 数据输入
    // -------------------------------------------------------------------------
    // 添加一个收益观测（推荐使用对数收益）
    // 内部自动维护每个 symbol 的滚动 deque
    void update(const Symbol& symbol, double return_value);

    // 批量更新（同一时刻多个 symbol）
    void update_batch(const std::vector<std::pair<Symbol, double>>& updates);

    // 从价格序列计算对数收益并更新
    void update_from_prices(const Symbol& symbol,
                              double current_price) noexcept;

    // -------------------------------------------------------------------------
    // 计算
    // -------------------------------------------------------------------------
    // 立即重新计算所有对
    Result<void> compute_all();

    // 计算指定对
    [[nodiscard]] Result<CorrelationResult> compute_pair(
        const Symbol& a, const Symbol& b) const;

    // -------------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------------
    // 获取相关性
    [[nodiscard]] double correlation(const Symbol& a,
                                       const Symbol& b) const noexcept;

    // 获取完整结果
    [[nodiscard]] std::optional<CorrelationResult> result(
        const Symbol& a, const Symbol& b) const;

    // 获取当前所有已计算的对
    [[nodiscard]] std::vector<CorrelationResult> all_results() const;

    // 导出相关性矩阵
    // 返回: symbols 列表 + 矩阵
    struct Matrix {
        std::vector<std::string> symbols;
        std::vector<std::vector<double>> values;   // N × N
        [[nodiscard]] bool empty() const noexcept {
            return symbols.empty();
        }
    };
    [[nodiscard]] Matrix matrix() const;

    // 获取相关系数最高的对
    [[nodiscard]] std::optional<CorrelationResult> highest_correlation() const;

    // 获取相关系数最低的对
    [[nodiscard]] std::optional<CorrelationResult> lowest_correlation() const;

    // 跟踪的 symbol 数量
    [[nodiscard]] std::size_t tracked_symbols() const noexcept;

    // -------------------------------------------------------------------------
    // 告警
    // -------------------------------------------------------------------------
    [[nodiscard]] std::vector<CorrelationAlertRecord> recent_alerts() const;
    [[nodiscard]] bool is_crash_detected() const noexcept;
    [[nodiscard]] std::size_t crash_pair_count() const noexcept;

    // -------------------------------------------------------------------------
    // 手动控制
    // -------------------------------------------------------------------------
    void clear_history() noexcept;
    void clear_alerts() noexcept;
    void clear_symbol(const Symbol& symbol) noexcept;

    // -------------------------------------------------------------------------
    // 持久化
    // -------------------------------------------------------------------------
    [[nodiscard]] CorrelationSnapshot to_snapshot() const;
    Result<void> from_snapshot(const CorrelationSnapshot& snapshot);

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const CorrelationStats& stats() const noexcept;
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

// 计算 Pearson 相关系数
[[nodiscard]] std::optional<double> compute_pearson(
    const std::vector<double>& a,
    const std::vector<double>& b) noexcept;

// 计算协方差
[[nodiscard]] std::optional<double> compute_covariance(
    const std::vector<double>& a,
    const std::vector<double>& b) noexcept;

// 计算标准差
[[nodiscard]] std::optional<double> compute_stddev(
    const std::vector<double>& data) noexcept;

// 计算 Beta（a 对 b）
[[nodiscard]] std::optional<double> compute_beta(
    const std::vector<double>& a,
    const std::vector<double>& b) noexcept;

// Winsorize 处理
[[nodiscard]] std::vector<double> winsorize(
    std::vector<double> data,
    double lower_pct = 0.01,
    double upper_pct = 0.99);

// EWMA 权重生成
[[nodiscard]] std::vector<double> make_ewma_weights(
    std::size_t n, double half_life) noexcept;

// 符号对键（排序后）
[[nodiscard]] std::string make_pair_key(const Symbol& a,
                                          const Symbol& b);

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_CORRELATION_MONITOR_HPP
