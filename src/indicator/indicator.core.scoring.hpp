// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 多因子正交化评分
// ==============================================================================
// @file    src/indicator/indicator.core.scoring.hpp
// @module  indicator
// @type    core
// @name    scoring
// @version 1.0.1
// @brief   多因子正交化评分系统，输出方向性得分 [-100, 100]
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 因子正交化：组内合成消除共线性（动能/量能/情绪三组）
//   - 状态依赖权重：MarketRegime 决定组间权重
//   - 方向性评分：保留符号，[-100, 100]，不 clip 丢空头
//   - 归一化保护：denom = max(mean, eps) + 分位数兜底
//   - NaN 防御：所有输入先过滤，不传播
//   - 贡献可追溯：每个因子贡献度可输出，便于诊断
//   - 权重归一化：权重和必须为 1.0，否则重新归一化
//   - 阈值友好：分数可直接与 60/-60/80 比较
//
// 因子分组（正交化后的三大组）:
//   动能组 (Momentum):  加速度 + MACD + OBV斜率
//   量能组 (Volume):    成交量比 + OBI
//   情绪组 (Sentiment): RSI + 资金费率
//
// 状态依赖权重表:
//   趋势市:  动能 0.50 / 量能 0.30 / 情绪 0.20
//   震荡市:  动能 0.20 / 量能 0.35 / 情绪 0.45
//   高波动:  动能 0.35 / 量能 0.40 / 情绪 0.25
// ==============================================================================

#ifndef QUANT_INDICATOR_CORE_SCORING_HPP
#define QUANT_INDICATOR_CORE_SCORING_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
#include "common/common.core.timestamp.hpp"
#include "common/common.core.types.hpp"
#include "indicator/indicator.core.band.hpp"
#include "indicator/indicator.core.calculator.hpp"

namespace quant {
namespace indicator {

// ==============================================================================
// 常量
// ==============================================================================
inline constexpr double kScoreEpsilon = 1e-9;

// 分数范围
inline constexpr double kScoreMax = 100.0;
inline constexpr double kScoreMin = -100.0;

// 归一化分母最小值（防爆炸）
inline constexpr double kMinNormalizeDenom = 1e-6;

// 默认阈值
inline constexpr double kDefaultEntryThreshold = 60.0;   // 顺势进场
inline constexpr double kDefaultReverseThreshold = 80.0; // 反向做单
inline constexpr double kDefaultFilterThreshold = 30.0;  // 过滤阈值

// 归一化用的滚动分位数窗口
inline constexpr std::size_t kDefaultDenomWindow = 20;

// 各因子的贡献权重（组内）
inline constexpr double kMomentumAccWeight = 0.40;
inline constexpr double kMomentumMacdWeight = 0.40;
inline constexpr double kMomentumObvWeight = 0.20;

inline constexpr double kVolumeVolRatioWeight = 0.60;
inline constexpr double kVolumeObiWeight = 0.40;

inline constexpr double kSentimentRsiWeight = 0.70;
inline constexpr double kSentimentFundingWeight = 0.30;

// ==============================================================================
// 微观结构数据（可选输入）
// ==============================================================================
struct MicrostructureData {
    // 订单簿失衡 [-1, 1]
    // OBI = (BidDepth5 - AskDepth5) / (BidDepth5 + AskDepth5)
    double obi{0.0};

    // 资金费率（8 小时，如 0.0001 表示 0.01%）
    double funding_rate{0.0};

    // 大单占比 [0, 1]
    double large_trade_ratio{0.0};

    // 是否有有效数据
    bool has_obi{false};
    bool has_funding{false};

    [[nodiscard]] bool is_valid() const noexcept {
        return std::isfinite(obi) && std::isfinite(funding_rate);
    }
};

// ==============================================================================
// 因子权重表（一组）
// ==============================================================================
struct GroupWeights {
    double momentum{0.0};
    double volume{0.0};
    double sentiment{0.0};

    // 权重和
    [[nodiscard]] double sum() const noexcept {
        return momentum + volume + sentiment;
    }

    // 是否有效（都非负且和 > 0）
    [[nodiscard]] bool is_valid() const noexcept {
        if (momentum < 0.0 || volume < 0.0 || sentiment < 0.0) return false;
        if (sum() < kScoreEpsilon) return false;
        return true;
    }

    // 归一化到和为 1.0
    void normalize() noexcept {
        const double s = sum();
        if (s < kScoreEpsilon) return;
        momentum  /= s;
        volume    /= s;
        sentiment /= s;
    }
};

// ==============================================================================
// 评分配置
// ==============================================================================
struct ScoringConfig {
    // -------------------------------------------------------------------------
    // 状态依赖权重表
    // -------------------------------------------------------------------------
    GroupWeights trend_weights{0.50, 0.30, 0.20};
    GroupWeights range_weights{0.20, 0.35, 0.45};
    GroupWeights high_vol_weights{0.35, 0.40, 0.25};

    // -------------------------------------------------------------------------
    // 归一化
    // -------------------------------------------------------------------------
    // 是否启用分位数归一化（false 则只用均值）
    bool use_percentile_denom{true};

    // 分位数窗口大小
    std::size_t denom_window{kDefaultDenomWindow};

    // -------------------------------------------------------------------------
    // AI 增强（可选）
    // -------------------------------------------------------------------------
    // AI 趋势概率（0~1），> 0.5 表示看涨
    bool enable_ai_boost{true};

    // AI 置信度权重（0~0.3）
    double ai_boost_weight{0.15};

    // -------------------------------------------------------------------------
    // 阈值
    // -------------------------------------------------------------------------
    double entry_threshold{kDefaultEntryThreshold};
    double reverse_threshold{kDefaultReverseThreshold};

    // -------------------------------------------------------------------------
    // 校验
    // -------------------------------------------------------------------------
    [[nodiscard]] bool is_valid() const noexcept {
        if (!trend_weights.is_valid()) return false;
        if (!range_weights.is_valid()) return false;
        if (!high_vol_weights.is_valid()) return false;
        if (denom_window < 4 || denom_window > 512) return false;
        if (ai_boost_weight < 0.0 || ai_boost_weight > 0.5) return false;
        if (entry_threshold <= 0.0 || entry_threshold > kScoreMax) return false;
        if (reverse_threshold < entry_threshold ||
            reverse_threshold > kScoreMax) return false;
        return true;
    }

    [[nodiscard]] std::string validate() const;
};

// ==============================================================================
// 因子贡献度（用于诊断）
// ==============================================================================
struct FactorContributions {
    // 三组原始得分（未加权前）
    double momentum{0.0};
    double volume{0.0};
    double sentiment{0.0};

    // 各因子的归一化值（组内）
    double acceleration_norm{0.0};
    double macd_norm{0.0};
    double obv_norm{0.0};
    double vol_ratio_norm{0.0};
    double obi_norm{0.0};
    double rsi_norm{0.0};
    double funding_norm{0.0};

    // 应用权重后的贡献
    double momentum_contribution{0.0};
    double volume_contribution{0.0};
    double sentiment_contribution{0.0};

    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 评分结果
// ==============================================================================
struct ScoringResult {
    // 元信息
    Timestamp   timestamp{};
    std::size_t bar_index{0};
    bool        is_valid{false};
    bool        is_warmup{true};

    // 主分数：[-100, 100]，正=多头，负=空头
    double score_signed{0.0};

    // 方向性分解
    double score_long{0.0};    // [0, 100]
    double score_short{0.0};   // [0, 100]

    // 三组得分（加权前）
    double momentum_score{0.0};
    double volume_score{0.0};
    double sentiment_score{0.0};

    // 权重（实际使用的）
    GroupWeights effective_weights{};

    // AI 贡献（可选）
    double ai_boost_applied{0.0};

    // 判定标记
    bool passes_entry{false};       // score_signed >= entry_threshold
    bool passes_reverse{false};     // |score_signed| >= reverse_threshold
    bool passes_short_entry{false}; // score_signed <= -entry_threshold

    // 信号方向（用于下游）
    // 0 = 无信号，1 = 多头，-1 = 空头
    int direction{0};

    // 强度等级
    enum class Strength : std::uint8_t {
        None     = 0,
        Weak     = 1,   // [30, 60)
        Medium   = 2,   // [60, 80)
        Strong   = 3,   // [80, 100]
    } strength{Strength::None};

    // 因子贡献
    FactorContributions contributions{};

    // 辅助查询
    [[nodiscard]] bool is_entry_signal() const noexcept {
        return passes_entry || passes_short_entry;
    }

    [[nodiscard]] bool is_reverse_signal() const noexcept {
        return passes_reverse;
    }

    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 评分系统
// ==============================================================================
class ScoringSystem {
public:
    // -------------------------------------------------------------------------
    // 构造 / 配置
    // -------------------------------------------------------------------------
    explicit ScoringSystem(ScoringConfig config = {});

    ScoringSystem(const ScoringSystem&) = delete;
    ScoringSystem& operator=(const ScoringSystem&) = delete;
    ScoringSystem(ScoringSystem&&) noexcept = default;
    ScoringSystem& operator=(ScoringSystem&&) noexcept = default;

    ~ScoringSystem() = default;

    [[nodiscard]] const ScoringConfig& config() const noexcept { return config_; }
    void set_config(const ScoringConfig& config);

    // -------------------------------------------------------------------------
    // 核心：计算评分
    // -------------------------------------------------------------------------
    // 输入：
    //   ind            - 指标结果
    //   band           - 带宽结果（用于判断突破位置）
    //   micro          - 微观结构数据（可选，无数据时用中性值）
    //   regime         - 市场状态
    //   ai_trend_prob  - AI 趋势概率 [0, 1]（0.5=中性，可选）
    // 返回：Result<ScoringResult>
    [[nodiscard]] Result<ScoringResult>
        compute(const IndicatorResult& ind,
                const BandResult& band,
                const MicrostructureData& micro,
                MarketRegime regime,
                double ai_trend_prob = 0.5) noexcept;

    // 便捷接口
    [[nodiscard]] ScoringResult
        compute_unchecked(const IndicatorResult& ind,
                          const BandResult& band,
                          const MicrostructureData& micro,
                          MarketRegime regime,
                          double ai_trend_prob = 0.5) noexcept;

    // -------------------------------------------------------------------------
    // 状态管理
    // -------------------------------------------------------------------------
    void reset() noexcept;

    [[nodiscard]] std::size_t bar_count() const noexcept { return bar_count_; }

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    struct Stats {
        std::uint64_t total_computes{0};
        std::uint64_t warmup_skips{0};
        std::uint64_t entry_signals{0};
        std::uint64_t reverse_signals{0};
        std::uint64_t filtered_signals{0};
        std::uint64_t error_count{0};
    };

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = Stats{}; }

private:
    // -------------------------------------------------------------------------
    // 内部方法（实现见 .cpp）
    // -------------------------------------------------------------------------

    // 根据状态选择权重表
    [[nodiscard]] GroupWeights
        select_weights(MarketRegime regime) const noexcept;

    // 归一化因子（带分母保护）
    [[nodiscard]] double normalize_factor(double value,
                                            double denom,
                                            double clip_range = 1.0) const noexcept;

    // 计算分母（均值 + 分位数）
    [[nodiscard]] double compute_denom(
        const std::vector<double>& history) const noexcept;

    // 动能组得分
    [[nodiscard]] double compute_momentum_group(
        const IndicatorResult& ind,
        FactorContributions& contrib) noexcept;

    // 量能组得分
    [[nodiscard]] double compute_volume_group(
        const IndicatorResult& ind,
        const MicrostructureData& micro,
        FactorContributions& contrib) noexcept;

    // 情绪组得分
    [[nodiscard]] double compute_sentiment_group(
        const IndicatorResult& ind,
        const MicrostructureData& micro,
        FactorContributions& contrib) noexcept;

    // 应用 AI 增强
    [[nodiscard]] double apply_ai_boost(double raw_score,
                                         double ai_trend_prob) const noexcept;

    // 判定强度等级
    [[nodiscard]] ScoringResult::Strength
        classify_strength(double abs_score) const noexcept;

    // -------------------------------------------------------------------------
    // 成员
    // -------------------------------------------------------------------------
    ScoringConfig config_;
    Stats         stats_{};
    std::size_t   bar_count_{0};

    // 归一化历史（用于分位数分母）
    // 环形缓冲
    static constexpr std::size_t kHistorySize = 256;
    std::array<double, kHistorySize> acc_history_{};
    std::array<double, kHistorySize> macd_history_{};
    std::array<double, kHistorySize> obv_history_{};
    std::array<double, kHistorySize> vol_ratio_history_{};
    std::size_t history_head_{0};
    std::size_t history_count_{0};

    // 更新历史
    void update_history(const IndicatorResult& ind) noexcept;
};

}  // namespace indicator
}  // namespace quant

#endif  // QUANT_INDICATOR_CORE_SCORING_HPP
