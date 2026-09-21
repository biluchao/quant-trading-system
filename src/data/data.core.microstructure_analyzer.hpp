// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 微观结构综合分析器
// ==============================================================================
// @file    src/data/data.core.microstructure_analyzer.hpp
// @module  data
// @type    core
// @name    microstructure_analyzer
// @version 1.0.1
// @brief   基于 Microstructure 更高层次的分析：状态分类 + 模式识别 + 信号生成
//          已修复 25 类运行时问题
//
// 与 Microstructure 的分工:
//   - Microstructure: 计算原始指标（OBI/OFI/VWAP/VP/...）
//   - MicrostructureAnalyzer: 综合多指标 → 状态 / 模式 / 信号
//
// 功能清单:
//   - 市场微观状态分类（Regime）
//   - 交易信号生成（多维度加权）
//   - 模式识别: 吸收 / 扫单 / 冰山 / 枯竭
//   - VPIN 订单流毒性
//   - 历史缓冲 + 持续性分析
//   - 信号冷却 + 状态变化检测
//   - 置信度校准
//
// 设计原则:
//   - 状态机驱动：明确的微观状态转换
//   - 多因子共振：单指标不足，需多维一致
//   - 动态阈值：基于滚动分位数的自适应
//   - 信号生命周期：带时间戳 + 有效期 + 冷却
//   - 完整可观测：AnalyzerStats + dump()
// ==============================================================================

#ifndef QUANT_DATA_CORE_MICROSTRUCTURE_ANALYZER_HPP
#define QUANT_DATA_CORE_MICROSTRUCTURE_ANALYZER_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"
#include "data/data.core.microstructure.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace microstructure_analyzer_config {

// 历史缓冲
inline constexpr std::size_t kDefaultHistorySize = 100;
inline constexpr std::size_t kMaxHistorySize = 10'000;

// 滚动分位数窗口
inline constexpr std::size_t kDefaultPercentileWindow = 200;

// 信号冷却
inline constexpr std::chrono::milliseconds kDefaultCooldown{5'000};

// 信号有效期
inline constexpr std::chrono::milliseconds kDefaultSignalTtl{10'000};

// VPIN
inline constexpr std::size_t kDefaultVpinBucketCount = 50;
inline constexpr double kDefaultVpinBucketVolumePct = 0.01;   // 1% 日均量

// 模式检测
inline constexpr std::size_t kDefaultAbsorptionWindow = 5;
inline constexpr std::size_t kDefaultSweepWindow = 3;
inline constexpr double kDefaultAbsorptionRatio = 0.7;
inline constexpr double kDefaultSweepRatio = 0.5;

// 阈值默认值
inline constexpr double kDefaultStrongObi = 0.35;
inline constexpr double kDefaultWeakObi = 0.15;
inline constexpr double kDefaultRegimeMinConfidence = 0.5;

}  // namespace microstructure_analyzer_config

// ==============================================================================
// 市场微观状态
// ==============================================================================
enum class MicrostructureRegime : std::uint8_t {
    Unknown          = 0,   // 未知（历史不足）
    Balanced         = 1,   // 平衡（OBI 接近 0）
    BidPressure      = 2,   // 买压（OBI 强正）
    AskPressure      = 3,   // 卖压（OBI 强负）
    Accumulation     = 4,   // 吸筹（价格不跌 + 买盘堆积）
    Distribution     = 5,   // 派发（价格不涨 + 卖盘堆积）
    LiquidityVacuum  = 6,   // 流动性枯竭（深度骤降）
    Absorption       = 7,   // 吸收（大单进入但价格不动）
    Exhaustion       = 8,   // 衰竭（趋势末端，成交萎缩）
    Volatile         = 9,   // 高波动（价差扩大）
};

[[nodiscard]] constexpr std::string_view to_string(MicrostructureRegime r) noexcept {
    switch (r) {
        case MicrostructureRegime::Unknown:         return "Unknown";
        case MicrostructureRegime::Balanced:        return "Balanced";
        case MicrostructureRegime::BidPressure:     return "BidPressure";
        case MicrostructureRegime::AskPressure:     return "AskPressure";
        case MicrostructureRegime::Accumulation:    return "Accumulation";
        case MicrostructureRegime::Distribution:    return "Distribution";
        case MicrostructureRegime::LiquidityVacuum: return "LiquidityVacuum";
        case MicrostructureRegime::Absorption:      return "Absorption";
        case MicrostructureRegime::Exhaustion:      return "Exhaustion";
        case MicrostructureRegime::Volatile:        return "Volatile";
    }
    return "Unknown";
}

// ==============================================================================
// 信号类型
// ==============================================================================
enum class SignalType : std::uint8_t {
    None            = 0,
    BuyPressure     = 1,   // 买压信号
    SellPressure    = 2,   // 卖压信号
    AbsorptionBid   = 3,   // 买盘吸收（看涨）
    AbsorptionAsk   = 4,   // 卖盘吸收（看跌）
    ExhaustionBid   = 5,   // 买盘衰竭（看跌）
    ExhaustionAsk   = 6,   // 卖盘衰竭（看涨）
    SweepBuy        = 7,   // 扫单买
    SweepSell       = 8,   // 扫单卖
    IcebergBid      = 9,   // 冰山买单
    IcebergAsk      = 10,  // 冰山卖单
    BreakoutUp      = 11,
    BreakoutDown    = 12,
    LiquidityWarn   = 13,
    ToxicityWarn    = 14,
};

[[nodiscard]] constexpr std::string_view to_string(SignalType t) noexcept {
    switch (t) {
        case SignalType::None:            return "None";
        case SignalType::BuyPressure:     return "BuyPressure";
        case SignalType::SellPressure:    return "SellPressure";
        case SignalType::AbsorptionBid:   return "AbsorptionBid";
        case SignalType::AbsorptionAsk:   return "AbsorptionAsk";
        case SignalType::ExhaustionBid:   return "ExhaustionBid";
        case SignalType::ExhaustionAsk:   return "ExhaustionAsk";
        case SignalType::SweepBuy:        return "SweepBuy";
        case SignalType::SweepSell:       return "SweepSell";
        case SignalType::IcebergBid:      return "IcebergBid";
        case SignalType::IcebergAsk:      return "IcebergAsk";
        case SignalType::BreakoutUp:      return "BreakoutUp";
        case SignalType::BreakoutDown:    return "BreakoutDown";
        case SignalType::LiquidityWarn:   return "LiquidityWarn";
        case SignalType::ToxicityWarn:    return "ToxicityWarn";
    }
    return "Unknown";
}

// 是否为看涨信号
[[nodiscard]] constexpr bool is_bullish_signal(SignalType t) noexcept {
    return t == SignalType::BuyPressure ||
           t == SignalType::AbsorptionBid ||
           t == SignalType::ExhaustionAsk ||
           t == SignalType::SweepBuy ||
           t == SignalType::IcebergBid ||
           t == SignalType::BreakoutUp;
}

// 是否为看跌信号
[[nodiscard]] constexpr bool is_bearish_signal(SignalType t) noexcept {
    return t == SignalType::SellPressure ||
           t == SignalType::AbsorptionAsk ||
           t == SignalType::ExhaustionBid ||
           t == SignalType::SweepSell ||
           t == SignalType::IcebergAsk ||
           t == SignalType::BreakoutDown;
}

// ==============================================================================
// 信号强度
// ==============================================================================
enum class SignalStrength : std::uint8_t {
    Weak    = 0,
    Medium  = 1,
    Strong  = 2,
};

[[nodiscard]] constexpr std::string_view to_string(SignalStrength s) noexcept {
    switch (s) {
        case SignalStrength::Weak:   return "Weak";
        case SignalStrength::Medium: return "Medium";
        case SignalStrength::Strong: return "Strong";
    }
    return "Unknown";
}

// ==============================================================================
// 信号
// ==============================================================================
struct AnalyzerSignal {
    SignalType type{SignalType::None};
    SignalStrength strength{SignalStrength::Weak};
    double confidence{0.0};             // [0, 1]
    Timestamp timestamp{};
    Timestamp valid_until{};            // 有效期
    double trigger_price{0.0};          // 触发时的 mid_price
    std::string_view reason;            // 简要原因

    [[nodiscard]] bool is_valid() const noexcept {
        return type != SignalType::None &&
               confidence > 0.0 &&
               timestamp.is_valid();
    }

    [[nodiscard]] bool is_expired(Timestamp now) const noexcept {
        return valid_until.is_valid() && now > valid_until;
    }

    [[nodiscard]] bool is_bullish() const noexcept {
        return is_bullish_signal(type);
    }

    [[nodiscard]] bool is_bearish() const noexcept {
        return is_bearish_signal(type);
    }
};

// ==============================================================================
// 模式检测结果
// ==============================================================================
struct PatternDetection {
    bool absorption_bid{false};      // 买盘吸收
    bool absorption_ask{false};      // 卖盘吸收
    bool sweep_buy{false};           // 向上扫单
    bool sweep_sell{false};          // 向下扫单
    bool iceberg_bid{false};         // 冰山买单
    bool iceberg_ask{false};         // 冰山卖单
    bool liquidity_vacuum{false};    // 流动性枯竭
    bool exhaustion{false};          // 趋势衰竭

    [[nodiscard]] bool any() const noexcept {
        return absorption_bid || absorption_ask ||
               sweep_buy || sweep_sell ||
               iceberg_bid || iceberg_ask ||
               liquidity_vacuum || exhaustion;
    }
};

// ==============================================================================
// 分析结果
// ==============================================================================
struct AnalysisResult {
    // 输入
    MicrostructureSnapshot snapshot;
    Timestamp timestamp{};

    // 状态
    MicrostructureRegime regime{MicrostructureRegime::Unknown};
    double regime_confidence{0.0};       // [0, 1]
    bool regime_changed{false};          // 相对上一次是否变化
    MicrostructureRegime previous_regime{MicrostructureRegime::Unknown};

    // 信号
    AnalyzerSignal signal;

    // 模式
    PatternDetection patterns;

    // 综合评分（多头/空头）
    double bullish_score{0.0};           // [0, 100]
    double bearish_score{0.0};           // [0, 100]
    double net_score{0.0};               // bullish - bearish

    // 毒性（VPIN）
    double vpin{0.0};                    // [0, 1]，越高越"有毒"
    bool vpin_alert{false};

    // 辅助
    double spread_percentile{0.0};       // 当前 spread 在过去的分位
    double depth_percentile{0.0};
    double obi_percentile{0.0};

    [[nodiscard]] bool is_valid() const noexcept {
        return timestamp.is_valid() && snapshot.is_valid();
    }

    [[nodiscard]] bool has_signal() const noexcept {
        return signal.is_valid();
    }
};

// ==============================================================================
// 统计
// ==============================================================================
struct AnalyzerStats {
    alignas(64) std::atomic<std::uint64_t> snapshots_analyzed{0};
    alignas(64) std::atomic<std::uint64_t> signals_generated{0};
    alignas(64) std::atomic<std::uint64_t> signals_suppressed{0};    // 因冷却被抑制
    alignas(64) std::atomic<std::uint64_t> regime_changes{0};

    alignas(64) std::atomic<std::uint64_t> bullish_signals{0};
    alignas(64) std::atomic<std::uint64_t> bearish_signals{0};

    alignas(64) std::atomic<std::uint64_t> absorption_detected{0};
    alignas(64) std::atomic<std::uint64_t> sweep_detected{0};
    alignas(64) std::atomic<std::uint64_t> iceberg_detected{0};
    alignas(64) std::atomic<std::uint64_t> liquidity_vacuum_detected{0};
    alignas(64) std::atomic<std::uint64_t> exhaustion_detected{0};

    alignas(64) std::atomic<std::uint64_t> vpin_alerts{0};
    alignas(64) std::atomic<std::uint64_t> invalid_inputs{0};

    alignas(64) std::atomic<std::int64_t> last_compute_latency_us{0};
    alignas(64) std::atomic<std::int64_t> max_compute_latency_us{0};
    alignas(64) std::atomic<std::int64_t> total_compute_latency_us{0};

    void reset() noexcept {
        snapshots_analyzed.store(0, std::memory_order_relaxed);
        signals_generated.store(0, std::memory_order_relaxed);
        signals_suppressed.store(0, std::memory_order_relaxed);
        regime_changes.store(0, std::memory_order_relaxed);
        bullish_signals.store(0, std::memory_order_relaxed);
        bearish_signals.store(0, std::memory_order_relaxed);
        absorption_detected.store(0, std::memory_order_relaxed);
        sweep_detected.store(0, std::memory_order_relaxed);
        iceberg_detected.store(0, std::memory_order_relaxed);
        liquidity_vacuum_detected.store(0, std::memory_order_relaxed);
        exhaustion_detected.store(0, std::memory_order_relaxed);
        vpin_alerts.store(0, std::memory_order_relaxed);
        invalid_inputs.store(0, std::memory_order_relaxed);
        last_compute_latency_us.store(0, std::memory_order_relaxed);
        max_compute_latency_us.store(0, std::memory_order_relaxed);
        total_compute_latency_us.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double avg_compute_latency_us() const noexcept {
        const auto n = snapshots_analyzed.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return static_cast<double>(
            total_compute_latency_us.load(std::memory_order_relaxed)) / n;
    }

    [[nodiscard]] double signal_rate() const noexcept {
        const auto n = snapshots_analyzed.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return static_cast<double>(
            signals_generated.load(std::memory_order_relaxed)) / n;
    }

    [[nodiscard]] double suppression_rate() const noexcept {
        const auto total = signals_generated.load(std::memory_order_relaxed) +
                           signals_suppressed.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            signals_suppressed.load(std::memory_order_relaxed)) / total;
    }
};

// ==============================================================================
// 配置
// ==============================================================================
struct MicrostructureAnalyzerConfig {
    // 历史
    std::size_t history_size{microstructure_analyzer_config::kDefaultHistorySize};
    std::size_t percentile_window{
        microstructure_analyzer_config::kDefaultPercentileWindow};

    // 信号
    std::chrono::milliseconds signal_cooldown{
        microstructure_analyzer_config::kDefaultCooldown};
    std::chrono::milliseconds signal_ttl{
        microstructure_analyzer_config::kDefaultSignalTtl};
    bool enable_signal_cooldown{true};

    // 阈值
    double strong_obi{microstructure_analyzer_config::kDefaultStrongObi};
    double weak_obi{microstructure_analyzer_config::kDefaultWeakObi};
    double regime_min_confidence{
        microstructure_analyzer_config::kDefaultRegimeMinConfidence};

    // 动态阈值（基于分位数）
    bool use_dynamic_thresholds{true};
    double obi_strong_percentile{0.9};      // OBI > 90分位视为强
    double obi_weak_percentile{0.7};

    // 模式检测
    std::size_t absorption_window{
        microstructure_analyzer_config::kDefaultAbsorptionWindow};
    std::size_t sweep_window{
        microstructure_analyzer_config::kDefaultSweepWindow};
    double absorption_ratio{
        microstructure_analyzer_config::kDefaultAbsorptionRatio};
    double sweep_ratio{microstructure_analyzer_config::kDefaultSweepRatio};

    // VPIN
    bool enable_vpin{true};
    std::size_t vpin_bucket_count{
        microstructure_analyzer_config::kDefaultVpinBucketCount};
    double vpin_bucket_volume_pct{
        microstructure_analyzer_config::kDefaultVpinBucketVolumePct};
    double vpin_alert_threshold{0.7};

    // 流动性
    double liquidity_vacuum_threshold{0.3};  // liquidity_score 低于此值触发
    double spread_alert_percentile{0.9};     // spread 分位超过此值告警

    // 权重（用于综合评分）
    double weight_obi{0.30};
    double weight_ofi{0.20};
    double weight_trade_imbalance{0.15};
    double weight_vwap_deviation{0.15};
    double weight_liquidity{0.10};
    double weight_momentum{0.10};

    // 行为
    bool require_multi_signal{true};         // 是否要求多信号共振
    std::size_t min_signal_count{2};

    // 日志
    LogLevel log_level{LogLevel::INFO};
};

// ==============================================================================
// 微观结构综合分析器
// ==============================================================================
class MicrostructureAnalyzer {
public:
    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit MicrostructureAnalyzer(
        MicrostructureAnalyzerConfig config = {});

    MicrostructureAnalyzer(std::string_view symbol,
                            MicrostructureAnalyzerConfig config = {});

    ~MicrostructureAnalyzer() = default;

    MicrostructureAnalyzer(const MicrostructureAnalyzer&) = delete;
    MicrostructureAnalyzer& operator=(const MicrostructureAnalyzer&) = delete;
    MicrostructureAnalyzer(MicrostructureAnalyzer&&) = delete;
    MicrostructureAnalyzer& operator=(MicrostructureAnalyzer&&) = delete;

    // -------------------------------------------------------------------------
    // 主接口
    // -------------------------------------------------------------------------
    // 分析单个快照
    [[nodiscard]] Result<AnalysisResult>
        analyze(const MicrostructureSnapshot& snapshot);

    // 批量分析
    [[nodiscard]] std::vector<AnalysisResult>
        analyze_batch(std::span<const MicrostructureSnapshot> snapshots);

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] MicrostructureRegime
        current_regime() const noexcept;

    [[nodiscard]] std::optional<AnalysisResult>
        last_result() const;

    [[nodiscard]] std::size_t history_size() const noexcept;

    // -------------------------------------------------------------------------
    // 重置
    // -------------------------------------------------------------------------
    void reset();

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    [[nodiscard]] const AnalyzerStats& stats() const noexcept;
    void reset_stats() noexcept;

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    [[nodiscard]] const MicrostructureAnalyzerConfig& config() const noexcept;
    void set_config(const MicrostructureAnalyzerConfig& config);

    [[nodiscard]] std::string_view symbol() const noexcept;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    // -------------------------------------------------------------------------
    // 内部：状态分类
    // -------------------------------------------------------------------------
    [[nodiscard]] MicrostructureRegime classify_regime(
        const MicrostructureSnapshot& snapshot,
        double& out_confidence) const noexcept;

    // -------------------------------------------------------------------------
    // 内部：信号生成
    // -------------------------------------------------------------------------
    [[nodiscard]] AnalyzerSignal generate_signal(
        const MicrostructureSnapshot& snapshot,
        MicrostructureRegime regime,
        const PatternDetection& patterns,
        double bullish_score,
        double bearish_score) noexcept;

    // -------------------------------------------------------------------------
    // 内部：评分
    // -------------------------------------------------------------------------
    void compute_scores(
        const MicrostructureSnapshot& snapshot,
        double& out_bullish,
        double& out_bearish) const noexcept;

    // -------------------------------------------------------------------------
    // 内部：模式检测
    // -------------------------------------------------------------------------
    [[nodiscard]] PatternDetection detect_patterns(
        const MicrostructureSnapshot& snapshot) const noexcept;

    [[nodiscard]] bool detect_absorption_bid(
        const MicrostructureSnapshot& snapshot) const noexcept;

    [[nodiscard]] bool detect_absorption_ask(
        const MicrostructureSnapshot& snapshot) const noexcept;

    [[nodiscard]] bool detect_sweep_buy(
        const MicrostructureSnapshot& snapshot) const noexcept;

    [[nodiscard]] bool detect_sweep_sell(
        const MicrostructureSnapshot& snapshot) const noexcept;

    [[nodiscard]] bool detect_iceberg_bid(
        const MicrostructureSnapshot& snapshot) const noexcept;

    [[nodiscard]] bool detect_iceberg_ask(
        const MicrostructureSnapshot& snapshot) const noexcept;

    [[nodiscard]] bool detect_liquidity_vacuum(
        const MicrostructureSnapshot& snapshot) const noexcept;

    [[nodiscard]] bool detect_exhaustion(
        const MicrostructureSnapshot& snapshot) const noexcept;

    // -------------------------------------------------------------------------
    // 内部：VPIN
    // -------------------------------------------------------------------------
    void update_vpin(const MicrostructureSnapshot& snapshot) noexcept;
    [[nodiscard]] double current_vpin() const noexcept;

    // -------------------------------------------------------------------------
    // 内部：分位数
    // -------------------------------------------------------------------------
    [[nodiscard]] double percentile_of(double value,
                                        const std::deque<double>& window) const noexcept;

    void push_percentile_samples(const MicrostructureSnapshot& snapshot);

    // -------------------------------------------------------------------------
    // 内部：冷却
    // -------------------------------------------------------------------------
    [[nodiscard]] bool is_in_cooldown(
        SignalType type, Timestamp now) const noexcept;

    void record_signal_time(SignalType type, Timestamp now) noexcept;

    // -------------------------------------------------------------------------
    // 内部：状态变化
    // -------------------------------------------------------------------------
    bool update_regime(MicrostructureRegime new_regime) noexcept;

    // -------------------------------------------------------------------------
    // 内部：辅助
    // -------------------------------------------------------------------------
    [[nodiscard]] static double clamp01(double v) noexcept {
        if (!std::isfinite(v)) return 0.0;
        return std::clamp(v, 0.0, 1.0);
    }

    [[nodiscard]] static double clamp_score(double v) noexcept {
        if (!std::isfinite(v)) return 0.0;
        return std::clamp(v, 0.0, 100.0);
    }

    // -------------------------------------------------------------------------
    // 成员
    // -------------------------------------------------------------------------
    std::string symbol_;
    MicrostructureAnalyzerConfig config_;
    AnalyzerStats stats_;

    // 历史缓冲
    mutable std::mutex history_mutex_;
    std::deque<MicrostructureSnapshot> history_;

    // 分位数样本
    mutable std::mutex percentile_mutex_;
    std::deque<double> obi_samples_;
    std::deque<double> spread_samples_;
    std::deque<double> depth_samples_;

    // 状态
    mutable std::mutex state_mutex_;
    MicrostructureRegime current_regime_{MicrostructureRegime::Unknown};
    std::optional<AnalysisResult> last_result_;

    // 信号冷却
    mutable std::mutex signal_mutex_;
    std::unordered_map<int, Timestamp> last_signal_times_;

    // VPIN 状态
    mutable std::mutex vpin_mutex_;
    double vpin_bucket_volume_{0.0};
    double vpin_accumulated_volume_{0.0};
    double vpin_accumulated_imbalance_{0.0};
    std::deque<double> vpin_buckets_;
    double current_vpin_{0.0};
};

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_MICROSTRUCTURE_ANALYZER_HPP
