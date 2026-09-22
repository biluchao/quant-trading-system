// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 策略层评分系统
// ==============================================================================
// @file    src/strategy/strategy.core.scoring.hpp
// @module  strategy
// @type    core
// @name    scoring
// @version 1.0.1
// @brief   多因子正交化评分，状态依赖权重，动态阈值，AI 融合
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 因子分组正交化：组内合成 → 组间加权，消除共线性
//   - 状态依赖权重：趋势/震荡/高波动三套权重表
//   - 方向性保留：Score_signed ∈ [-100, 100]，不 clip
//   - 多空分离：score_long / score_short 显式
//   - 权重校验：注册时校验和 = 1.0，禁止负权重
//   - 动态阈值：按状态独立维护评分历史，P70/P90 自适应
//   - AI 融合：AI 概率调整 ±10 分，限幅保护
//   - 贡献度分解：固定数组 + string_view，无堆分配
//   - 热更新：订阅配置变更，清空对应状态历史
//
// 评分公式:
//   组内合成:
//     momentum  = w_acc·acc_norm + w_macd·macd_norm + w_obv·obv_norm
//     volume    = w_vol·vol_norm + w_obi·obi_norm
//     sentiment = w_rsi·rsi_norm + w_funding·funding_norm
//
//   组间加权（状态依赖）:
//     Score_signed = (
//         W_momentum × momentum +
//         W_volume   × volume +
//         W_sentiment × sentiment
//     ) × 100 + AI_adjustment
//
//   限制:
//     Score_signed ∈ [-100, 100]
//     多头信号: Score ≥ +60
//     空头信号: Score ≤ -60
//     反向信号: |Score| ≥ 80
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_SCORING_HPP
#define QUANT_STRATEGY_CORE_SCORING_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
#include "indicator/indicator.core.calculator.hpp"
#include "indicator/indicator.core.band.hpp"

namespace quant::strategy {

// ==============================================================================
// 市场状态（与 engine 保持一致）
// ==============================================================================
enum class MarketRegime : std::uint8_t {
    Unknown   = 0,
    TrendUp   = 1,
    TrendDown = 2,
    Range     = 3,
    HighVol   = 4,
    LowVol    = 5,
};

[[nodiscard]] constexpr std::string_view to_string(MarketRegime r) noexcept {
    switch (r) {
        case MarketRegime::Unknown:   return "Unknown";
        case MarketRegime::TrendUp:   return "TrendUp";
        case MarketRegime::TrendDown: return "TrendDown";
        case MarketRegime::Range:     return "Range";
        case MarketRegime::HighVol:   return "HighVol";
        case MarketRegime::LowVol:    return "LowVol";
    }
    return "Unknown";
}

// ==============================================================================
// 评分常量
// ==============================================================================
namespace scoring_config {

// 评分的有效范围
inline constexpr double kScoreMin = -100.0;
inline constexpr double kScoreMax =  100.0;

// 阈值
inline constexpr double kEntryThreshold      = 60.0;
inline constexpr double kReverseThreshold    = 80.0;
inline constexpr double kNeutralRange        = 30.0;

// AI 调整上下限
inline constexpr double kMaxAiAdjustment     = 10.0;
inline constexpr double kMinAiAdjustment     = -10.0;

// 评分历史窗口大小（用于动态阈值）
inline constexpr std::size_t kHistoryWindow  = 100;

// 动态阈值所需的最小样本数
inline constexpr std::size_t kMinHistorySamples = 20;

// 归一化保护 epsilon
inline constexpr double kNormEpsilon         = 1e-6;

// 因子数量（编译期）
inline constexpr std::size_t kFactorCount    = 7;

}  // namespace scoring_config

// ==============================================================================
// 因子名称（编译期字符串，无堆分配）
// ==============================================================================
namespace factor_names {
inline constexpr std::string_view kAcceleration = "acceleration";
inline constexpr std::string_view kMacd         = "macd";
inline constexpr std::string_view kObv          = "obv";
inline constexpr std::string_view kVolume       = "volume";
inline constexpr std::string_view kObi          = "obi";
inline constexpr std::string_view kRsi          = "rsi";
inline constexpr std::string_view kFunding      = "funding";
}  // namespace factor_names

// ==============================================================================
// 因子贡献（固定布局，无堆分配）
// ==============================================================================
struct FactorContribution {
    std::string_view name;        // 静态字符串，不持有所有权
    double raw_value{0.0};        // 归一化后的原始值 [-1, 1]
    double weight{0.0};           // 权重 [0, 1]
    double contribution{0.0};     // raw_value × weight × 100
    bool   valid{true};           // 因子是否有效（NaN 检测）
};

// ==============================================================================
// 权重表
// ==============================================================================
struct MomentumWeights {
    double acc   {0.40};
    double macd  {0.40};
    double obv   {0.20};

    [[nodiscard]] constexpr double sum() const noexcept {
        return acc + macd + obv;
    }
};

struct VolumeWeights {
    double volume{0.60};
    double obi   {0.40};

    [[nodiscard]] constexpr double sum() const noexcept {
        return volume + obi;
    }
};

struct SentimentWeights {
    double rsi    {0.70};
    double funding{0.30};

    [[nodiscard]] constexpr double sum() const noexcept {
        return rsi + funding;
    }
};

struct GroupWeights {
    double momentum {0.50};
    double volume   {0.30};
    double sentiment{0.20};

    [[nodiscard]] constexpr double sum() const noexcept {
        return momentum + volume + sentiment;
    }
};

struct WeightTable {
    GroupWeights     group;
    MomentumWeights  momentum;
    VolumeWeights    volume;
    SentimentWeights sentiment;
    std::uint32_t    version{1};

    // 校验所有子表权重和 = 1.0
    [[nodiscard]] Result<void> validate() const noexcept {
        constexpr double kTolerance = 1e-6;

        auto check_sum = [](double s, std::string_view name)
            -> Result<void> {
            if (std::abs(s - 1.0) > kTolerance) {
                return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                    "权重和不为 1.0")
                    .with_context("table", name)
                    .with_context("sum", std::to_string(s));
            }
            return {};
        };

        if (auto r = check_sum(group.sum(), "group"); r.is_err()) return r;
        if (auto r = check_sum(momentum.sum(), "momentum"); r.is_err()) return r;
        if (auto r = check_sum(volume.sum(), "volume"); r.is_err()) return r;
        if (auto r = check_sum(sentiment.sum(), "sentiment"); r.is_err()) return r;

        // 禁止负权重
        auto check_nonneg = [](double v, std::string_view name)
            -> Result<void> {
            if (v < 0.0) {
                return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                    "权重为负")
                    .with_context("factor", name);
            }
            return {};
        };

        if (auto r = check_nonneg(group.momentum, "group.momentum"); r.is_err()) return r;
        if (auto r = check_nonneg(group.volume, "group.volume"); r.is_err()) return r;
        if (auto r = check_nonneg(group.sentiment, "group.sentiment"); r.is_err()) return r;

        if (auto r = check_nonneg(momentum.acc, "momentum.acc"); r.is_err()) return r;
        if (auto r = check_nonneg(momentum.macd, "momentum.macd"); r.is_err()) return r;
        if (auto r = check_nonneg(momentum.obv, "momentum.obv"); r.is_err()) return r;

        if (auto r = check_nonneg(volume.volume, "volume.volume"); r.is_err()) return r;
        if (auto r = check_nonneg(volume.obi, "volume.obi"); r.is_err()) return r;

        if (auto r = check_nonneg(sentiment.rsi, "sentiment.rsi"); r.is_err()) return r;
        if (auto r = check_nonneg(sentiment.funding, "sentiment.funding"); r.is_err()) return r;

        return {};
    }

    // 默认三套权重表
    [[nodiscard]] static WeightTable default_trend_up() noexcept {
        WeightTable t;
        t.group = {0.50, 0.30, 0.20};
        t.momentum = {0.40, 0.40, 0.20};
        t.volume = {0.60, 0.40};
        t.sentiment = {0.70, 0.30};
        return t;
    }

    [[nodiscard]] static WeightTable default_trend_down() noexcept {
        return default_trend_up();  // 与上涨对称
    }

    [[nodiscard]] static WeightTable default_range() noexcept {
        WeightTable t;
        t.group = {0.20, 0.35, 0.45};
        t.momentum = {0.40, 0.40, 0.20};
        t.volume = {0.50, 0.50};
        t.sentiment = {0.60, 0.40};
        return t;
    }

    [[nodiscard]] static WeightTable default_high_vol() noexcept {
        WeightTable t;
        t.group = {0.35, 0.40, 0.25};
        t.momentum = {0.40, 0.40, 0.20};
        t.volume = {0.55, 0.45};
        t.sentiment = {0.65, 0.35};
        return t;
    }

    [[nodiscard]] static WeightTable default_low_vol() noexcept {
        WeightTable t;
        t.group = {0.40, 0.25, 0.35};
        t.momentum = {0.40, 0.40, 0.20};
        t.volume = {0.60, 0.40};
        t.sentiment = {0.70, 0.30};
        return t;
    }
};

// ==============================================================================
// 评分结果
// ==============================================================================
struct ScoringResult {
    // 主评分（带符号）
    double score_signed{0.0};    // [-100, 100]
    double score_long{0.0};      // [0, 100]
    double score_short{0.0};     // [0, 100]

    // 因子贡献（固定数组，无堆分配）
    std::array<FactorContribution, scoring_config::kFactorCount> factors{};
    std::size_t factor_count{0};

    // 分组评分
    double momentum_group{0.0};
    double volume_group{0.0};
    double sentiment_group{0.0};

    // 元信息
    MarketRegime regime{MarketRegime::Unknown};
    std::uint32_t weights_version{0};
    double ai_adjustment{0.0};
    bool valid{false};
    std::string_view invalid_reason{};

    // 用于引擎的状态
    double effective_score{0.0};

    // 访问因子贡献（按名称）
    [[nodiscard]] std::optional<double> factor(
        std::string_view name) const noexcept
    {
        for (std::size_t i = 0; i < factor_count; ++i) {
            if (factors[i].name == name) {
                return factors[i].contribution;
            }
        }
        return std::nullopt;
    }

    // 诊断字符串
    [[nodiscard]] std::string to_string() const {
        std::string s;
        s.reserve(256);
        s += "ScoringResult{score=";
        s += std::to_string(score_signed);
        s += ", long=" + std::to_string(score_long);
        s += ", short=" + std::to_string(score_short);
        s += ", regime=" + std::string{quant::strategy::to_string(regime)};
        s += ", ai_adj=" + std::to_string(ai_adjustment);
        s += ", valid=" + std::to_string(valid);
        if (!valid) {
            s += ", reason=" + std::string{invalid_reason};
        }
        s += "}";
        return s;
    }
};

// ==============================================================================
// 评分历史（用于动态阈值）
// ==============================================================================
class ScoreHistory {
public:
    explicit ScoreHistory(std::size_t window = scoring_config::kHistoryWindow)
        : window_{window}
    {
        buffer_.reserve(window_);
    }

    // 记录一个评分（限幅后）
    void push(double score) noexcept {
        // 限幅防止异常值污染
        score = std::clamp(score,
                           scoring_config::kScoreMin,
                           scoring_config::kScoreMax);

        // 环形缓冲
        if (buffer_.size() < window_) {
            buffer_.push_back(score);
        } else {
            buffer_[head_] = score;
            head_ = (head_ + 1) % window_;
        }

        // 更新已排序副本
        sorted_.insert(score);
        if (sorted_.size() > window_) {
            // 移除最旧的
            const double old = buffer_[(head_ + window_ - 1) % window_];
            auto it = sorted_.find(old);
            if (it != sorted_.end()) {
                sorted_.erase(it);
            }
        }
    }

    // 计算分位数（p ∈ [0, 1]）
    [[nodiscard]] std::optional<double> percentile(double p) const noexcept {
        if (sorted_.size() < scoring_config::kMinHistorySamples) {
            return std::nullopt;
        }
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;

        const std::size_t idx = static_cast<std::size_t>(
            p * static_cast<double>(sorted_.size() - 1));
        auto it = sorted_.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(idx));
        return *it;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return sorted_.size();
    }

    [[nodiscard]] bool ready() const noexcept {
        return sorted_.size() >= scoring_config::kMinHistorySamples;
    }

    void reset() noexcept {
        buffer_.clear();
        sorted_.clear();
        head_ = 0;
    }

    // 诊断
    [[nodiscard]] std::string dump() const {
        std::string s = "ScoreHistory{size=" + std::to_string(sorted_.size());
        if (auto p70 = percentile(0.70)) {
            s += ", p70=" + std::to_string(*p70);
        }
        if (auto p90 = percentile(0.90)) {
            s += ", p90=" + std::to_string(*p90);
        }
        s += "}";
        return s;
    }

private:
    std::size_t window_;
    std::vector<double> buffer_;
    std::size_t head_{0};
    std::multiset<double> sorted_;
};

// ==============================================================================
// 动态阈值
// ==============================================================================
struct DynamicThresholds {
    double entry{scoring_config::kEntryThreshold};
    double reverse{scoring_config::kReverseThreshold};
    double neutral{scoring_config::kNeutralRange};
    bool dynamic{false};    // 是否使用动态值

    [[nodiscard]] static DynamicThresholds fixed() noexcept {
        return {scoring_config::kEntryThreshold,
                scoring_config::kReverseThreshold,
                scoring_config::kNeutralRange,
                false};
    }
};

// ==============================================================================
// 评分统计
// ==============================================================================
struct ScoringStats {
    std::atomic<std::uint64_t> compute_count{0};
    std::atomic<std::uint64_t> invalid_input_count{0};
    std::atomic<std::uint64_t> nan_detected_count{0};
    std::atomic<std::uint64_t> history_reset_count{0};
    std::atomic<std::uint64_t> weights_update_count{0};

    void reset() noexcept {
        compute_count.store(0);
        invalid_input_count.store(0);
        nan_detected_count.store(0);
        history_reset_count.store(0);
        weights_update_count.store(0);
    }
};

// ==============================================================================
// 策略层评分系统
// ==============================================================================
class StrategyScoring {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 权重提供者（必需，从 ConfigManager 读取）
        std::function<WeightTable(MarketRegime)> weights_provider;

        // 权重变更订阅（可选）
        std::function<void(std::function<void()>)> subscribe_weights_change;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit StrategyScoring(Dependencies deps);
    ~StrategyScoring();

    StrategyScoring(const StrategyScoring&) = delete;
    StrategyScoring& operator=(const StrategyScoring&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 核心评分（热路径）
    // -------------------------------------------------------------------------
    // 输入:
    //   ind   - 指标结果
    //   band  - 带宽结果
    //   micro - 微观结构（OBI 等，可为空）
    //   regime- 市场状态
    //   ai_prob - AI 趋势概率 [0, 1]
    // 返回: 评分结果（含完整贡献分解）
    [[nodiscard]] Result<ScoringResult> compute(
        const IndicatorResult& ind,
        const BandResult& band,
        const MicrostructureData* micro,
        MarketRegime regime,
        double ai_probability);

    // 简化版本（无微观结构）
    [[nodiscard]] Result<ScoringResult> compute(
        const IndicatorResult& ind,
        const BandResult& band,
        MarketRegime regime,
        double ai_probability)
    {
        return compute(ind, band, nullptr, regime, ai_probability);
    }

    // -------------------------------------------------------------------------
    // 动态阈值
    // -------------------------------------------------------------------------
    [[nodiscard]] DynamicThresholds thresholds(MarketRegime regime) const;

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] std::uint32_t weights_version(MarketRegime regime) const;
    [[nodiscard]] WeightTable current_weights(MarketRegime regime) const;

    // -------------------------------------------------------------------------
    // 历史管理
    // -------------------------------------------------------------------------
    void reset_history(MarketRegime regime) noexcept;
    void reset_all_history() noexcept;
    [[nodiscard]] std::size_t history_size(MarketRegime regime) const;

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const ScoringStats& stats() const noexcept;
    [[nodiscard]] std::string dump() const;

private:
    // =========================================================================
    // 内部实现
    // =========================================================================

    // 权重表缓存（按状态）
    struct RegimeContext {
        WeightTable weights;
        ScoreHistory history;
        std::uint32_t weights_version{1};
        mutable std::mutex mutex;
    };

    // 获取状态上下文
    [[nodiscard]] RegimeContext& context_for(MarketRegime regime) noexcept;
    [[nodiscard]] const RegimeContext& context_for(MarketRegime regime) const noexcept;

    // 加载权重
    Result<void> load_weights(MarketRegime regime);

    // 因子归一化
    [[nodiscard]] double normalize_acceleration(
        const IndicatorResult& ind) const noexcept;
    [[nodiscard]] double normalize_macd(
        const IndicatorResult& ind) const noexcept;
    [[nodiscard]] double normalize_obv(
        const IndicatorResult& ind) const noexcept;
    [[nodiscard]] double normalize_volume(
        const IndicatorResult& ind, const Candle& candle) const noexcept;
    [[nodiscard]] double normalize_obi(
        const MicrostructureData* micro) const noexcept;
    [[nodiscard]] double normalize_rsi(
        const IndicatorResult& ind) const noexcept;
    [[nodiscard]] double normalize_funding(
        const IndicatorResult& ind) const noexcept;

    // 输入校验
    [[nodiscard]] Result<void> validate_indicators(
        const IndicatorResult& ind) const;

    // 计算贡献度
    void record_factor(ScoringResult& result,
                       std::string_view name,
                       double raw_value,
                       double weight) const noexcept;

    // 应用 AI 调整
    [[nodiscard]] double apply_ai_adjustment(
        double base_score,
        double ai_probability) const noexcept;

    // =========================================================================
    // 成员
    // =========================================================================
    Dependencies deps_;

    // 状态上下文（按 MarketRegime 索引）
    std::array<RegimeContext, 6> contexts_;

    // 运行标志
    std::atomic<bool> running_{false};

    // 统计
    ScoringStats stats_;

    // 订阅 ID
    std::uint64_t subscription_id_{0};
};

// ==============================================================================
// 便捷函数
// ==============================================================================

// 判断评分是否满足进场条件
[[nodiscard]] inline bool is_entry_signal(
    const ScoringResult& r,
    const DynamicThresholds& t) noexcept
{
    return r.valid && r.score_signed >= t.entry;
}

// 判断评分是否满足反向条件
[[nodiscard]] inline bool is_reverse_signal(
    const ScoringResult& r,
    const DynamicThresholds& t) noexcept
{
    return r.valid && std::abs(r.score_signed) >= t.reverse;
}

// 判断评分是否中性
[[nodiscard]] inline bool is_neutral(
    const ScoringResult& r,
    const DynamicThresholds& t) noexcept
{
    return r.valid && std::abs(r.score_signed) <= t.neutral;
}

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_SCORING_HPP
