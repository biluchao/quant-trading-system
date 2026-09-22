// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 多因子正交化评分实现
// ==============================================================================
// @file    src/indicator/indicator.core.scoring.cpp
// @module  indicator
// @type    core
// @name    scoring
// @version 1.0.1
// @brief   ScoringSystem 全部实现：
//          - 三组正交化：动能 / 量能 / 情绪
//          - 状态依赖权重选择
//          - 归一化（均值 + 分位数 + epsilon 三重保护）
//          - AI 温和增强
//          - 方向性评分 + 强度分级
//          已修复 15 类运行时问题
//
// 设计原则:
//   - 正交化：组内先合成，组间再加权，消除共线性
//   - 归一化保护：denom 永不为零，否则用 fallback
//   - 方向保留：[-100, 100]，不 clip 丢空头
//   - NaN 防御：所有输入先过滤
//   - 贡献可追溯：每个因子贡献度可输出
//   - 状态切换安全：MarketRegime switch 有兜底分支
// ==============================================================================

#include "indicator/indicator.core.scoring.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace quant {
namespace indicator {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

[[nodiscard]] inline bool is_finite_s(double v) noexcept {
    return std::isfinite(v);
}

[[nodiscard]] inline double safe_div_s(double num, double den,
                                         double fallback = 0.0) noexcept {
    if (std::abs(den) < kScoreEpsilon) return fallback;
    return num / den;
}

[[nodiscard]] inline double clamp_s(double v, double lo, double hi) noexcept {
    if (!is_finite_s(v)) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

[[nodiscard]] std::string json_double_s(double v, int precision = 4) {
    if (!std::isfinite(v)) return "null";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

[[nodiscard]] std::string fmt_double_s(double v, int precision = 2) {
    if (!std::isfinite(v)) return "NaN";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

}  // namespace

// ==============================================================================
// ScoringConfig::validate
// ==============================================================================
std::string ScoringConfig::validate() const {
    if (!trend_weights.is_valid()) {
        return "trend_weights 非法（含负值或和为 0）";
    }
    if (!range_weights.is_valid()) {
        return "range_weights 非法";
    }
    if (!high_vol_weights.is_valid()) {
        return "high_vol_weights 非法";
    }
    if (denom_window < 4 || denom_window > 512) {
        return "denom_window 越界 [4, 512]，当前 " +
               std::to_string(denom_window);
    }
    if (ai_boost_weight < 0.0 || ai_boost_weight > 0.5) {
        return "ai_boost_weight 越界 [0, 0.5]，当前 " +
               std::to_string(ai_boost_weight);
    }
    if (entry_threshold <= 0.0 || entry_threshold > kScoreMax) {
        return "entry_threshold 越界 (0, 100]，当前 " +
               std::to_string(entry_threshold);
    }
    if (reverse_threshold < entry_threshold ||
        reverse_threshold > kScoreMax) {
        return "reverse_threshold 必须 >= entry_threshold 且 <= 100";
    }
    return {};
}

// ==============================================================================
// FactorContributions::to_string
// ==============================================================================
std::string FactorContributions::to_string() const {
    std::ostringstream oss;
    oss << "Contrib{"
        << "M[" << fmt_double_s(momentum, 3) << ": "
        << "acc=" << fmt_double_s(acceleration_norm, 2)
        << ", macd=" << fmt_double_s(macd_norm, 2)
        << ", obv=" << fmt_double_s(obv_norm, 2)
        << "], V[" << fmt_double_s(volume, 3) << ": "
        << "vol=" << fmt_double_s(vol_ratio_norm, 2)
        << ", obi=" << fmt_double_s(obi_norm, 2)
        << "], S[" << fmt_double_s(sentiment, 3) << ": "
        << "rsi=" << fmt_double_s(rsi_norm, 2)
        << ", fund=" << fmt_double_s(funding_norm, 2)
        << "]}";
    return oss.str();
}

// ==============================================================================
// ScoringSystem 构造 / 配置
// ==============================================================================
ScoringSystem::ScoringSystem(ScoringConfig config)
    : config_{config}
{
    const auto err = config_.validate();
    if (!err.empty()) {
        throw std::invalid_argument("ScoringConfig 非法: " + err);
    }

    // 归一化权重（防止用户传入未归一化的权重）
    config_.trend_weights.normalize();
    config_.range_weights.normalize();
    config_.high_vol_weights.normalize();

    // 历史零初始化
    acc_history_.fill(0.0);
    macd_history_.fill(0.0);
    obv_history_.fill(0.0);
    vol_ratio_history_.fill(0.0);
}

void ScoringSystem::set_config(const ScoringConfig& config) {
    const auto err = config.validate();
    if (!err.empty()) {
        throw std::invalid_argument("ScoringConfig 非法: " + err);
    }
    config_ = config;
    config_.trend_weights.normalize();
    config_.range_weights.normalize();
    config_.high_vol_weights.normalize();
    reset();
}

void ScoringSystem::reset() noexcept {
    bar_count_ = 0;
    history_head_ = 0;
    history_count_ = 0;
    acc_history_.fill(0.0);
    macd_history_.fill(0.0);
    obv_history_.fill(0.0);
    vol_ratio_history_.fill(0.0);
    // 保留 stats_（除非显式 reset_stats）
}

// ==============================================================================
// 状态权重选择
// ==============================================================================
GroupWeights
ScoringSystem::select_weights(MarketRegime regime) const noexcept {
    switch (regime) {
        case MarketRegime::TREND_UP:
        case MarketRegime::TREND_DOWN:
            return config_.trend_weights;

        case MarketRegime::RANGE:
            return config_.range_weights;

        case MarketRegime::HIGH_VOL:
            return config_.high_vol_weights;

        case MarketRegime::LOW_VOL:
            // 低波动接近震荡，用震荡权重
            return config_.range_weights;

        case MarketRegime::UNKNOWN:
        default:
            // 兜底：用趋势权重（保守）
            return config_.trend_weights;
    }
}

// ==============================================================================
// 归一化
// ==============================================================================
double ScoringSystem::normalize_factor(double value, double denom,
                                         double clip_range) const noexcept {
    if (!is_finite_s(value)) return 0.0;
    if (!is_finite_s(denom) || std::abs(denom) < kMinNormalizeDenom) {
        return 0.0;
    }

    const double norm = value / denom;
    return clamp_s(norm, -clip_range, clip_range);
}

// ==============================================================================
// 计算分母（均值 + 分位数混合）
// ==============================================================================
double ScoringSystem::compute_denom(
    const std::vector<double>& history) const noexcept
{
    if (history.empty()) {
        return kMinNormalizeDenom;
    }

    // 过滤有限值
    std::vector<double> valid;
    valid.reserve(history.size());
    double abs_sum = 0.0;
    for (double v : history) {
        if (is_finite_s(v)) {
            const double a = std::abs(v);
            valid.push_back(a);
            abs_sum += a;
        }
    }

    if (valid.empty()) {
        return kMinNormalizeDenom;
    }

    const double mean = abs_sum / static_cast<double>(valid.size());

    if (!config_.use_percentile_denom) {
        return std::max(mean, kMinNormalizeDenom);
    }

    // 计算 90 分位数
    std::sort(valid.begin(), valid.end());
    const std::size_t idx = std::min(
        valid.size() - 1,
        static_cast<std::size_t>(valid.size() * 0.9));
    const double p90 = valid[idx];

    // 混合：0.5 均值 + 0.5 分位数
    const double denom = 0.5 * mean + 0.5 * p90;
    return std::max(denom, kMinNormalizeDenom);
}

// ==============================================================================
// 历史更新
// ==============================================================================
void ScoringSystem::update_history(const IndicatorResult& ind) noexcept {
    // 写入环形缓冲
    acc_history_[history_head_] = is_finite_s(ind.ema_slope)
        ? ind.ema_slope : 0.0;
    macd_history_[history_head_] = is_finite_s(ind.macd_hist)
        ? ind.macd_hist : 0.0;
    obv_history_[history_head_] = is_finite_s(ind.obv_slope)
        ? ind.obv_slope : 0.0;
    vol_ratio_history_[history_head_] = is_finite_s(ind.volume_ratio)
        ? ind.volume_ratio : 1.0;

    history_head_ = (history_head_ + 1) % kHistorySize;
    if (history_count_ < kHistorySize) {
        ++history_count_;
    }
}

// ==============================================================================
// 提取最近的窗口值（用于 compute_denom）
// ==============================================================================
namespace {
[[nodiscard]] std::vector<double> extract_recent(
    const std::array<double, ScoringSystem_kHistorySize_placeholder>&,
    std::size_t,
    std::size_t) noexcept;
}  // namespace

// 由于 kHistorySize 是类内 static constexpr，用模板辅助
namespace {

template <std::size_t N>
[[nodiscard]] std::vector<double> extract_recent(
    const std::array<double, N>& arr,
    std::size_t head,
    std::size_t count,
    std::size_t window) noexcept
{
    const std::size_t n = std::min({count, window, N});
    std::vector<double> result;
    result.reserve(n);

    // 从最新到最旧读取
    // head 指向下一个写入位置，所以最新位置是 (head + N - 1) % N
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = (head + N - 1 - i) % N;
        result.push_back(arr[idx]);
    }
    return result;
}

}  // namespace

// ==============================================================================
// 动能组
// ==============================================================================
double ScoringSystem::compute_momentum_group(
    const IndicatorResult& ind,
    FactorContributions& contrib) noexcept
{
    // 1. 提取历史窗口
    const auto acc_hist = extract_recent(
        acc_history_, history_head_, history_count_, config_.denom_window);
    const auto macd_hist_hist = extract_recent(
        macd_history_, history_head_, history_count_, config_.denom_window);
    const auto obv_hist_hist = extract_recent(
        obv_history_, history_head_, history_count_, config_.denom_window);

    // 2. 计算分母
    const double denom_acc  = compute_denom(acc_hist);
    const double denom_macd = compute_denom(macd_hist_hist);
    const double denom_obv  = compute_denom(obv_hist_hist);

    // 3. 归一化各因子
    const double acc_norm = normalize_factor(
        ind.ema_slope, denom_acc, 1.0);
    const double macd_norm = normalize_factor(
        ind.macd_hist, denom_macd, 1.0);
    const double obv_norm = normalize_factor(
        ind.obv_slope, denom_obv, 1.0);

    // 4. 记录贡献
    contrib.acceleration_norm = acc_norm;
    contrib.macd_norm = macd_norm;
    contrib.obv_norm = obv_norm;

    // 5. 组内合成
    const double score = kMomentumAccWeight * acc_norm
                       + kMomentumMacdWeight * macd_norm
                       + kMomentumObvWeight * obv_norm;

    contrib.momentum = score;
    return score;
}

// ==============================================================================
// 量能组
// ==============================================================================
double ScoringSystem::compute_volume_group(
    const IndicatorResult& ind,
    const MicrostructureData& micro,
    FactorContributions& contrib) noexcept
{
    // 1. 成交量比：volume_ratio ∈ [0, ∞) → clip 到 [-1, 1]
    //    基准：1.0 为中性，>1.0 为放大
    double vol_norm = 0.0;
    if (is_finite_s(ind.volume_ratio)) {
        vol_norm = clamp_s((ind.volume_ratio - 1.0) / 1.5, -1.0, 1.0);
    }

    // 2. OBI：直接是 [-1, 1]，再除以 0.3 增强灵敏度
    double obi_norm = 0.0;
    if (micro.has_obi && is_finite_s(micro.obi)) {
        obi_norm = clamp_s(micro.obi / 0.3, -1.0, 1.0);
    }

    // 3. 记录贡献
    contrib.vol_ratio_norm = vol_norm;
    contrib.obi_norm = obi_norm;

    // 4. 组内合成
    const double score = kVolumeVolRatioWeight * vol_norm
                       + kVolumeObiWeight * obi_norm;

    contrib.volume = score;
    return score;
}

// ==============================================================================
// 情绪组
// ==============================================================================
double ScoringSystem::compute_sentiment_group(
    const IndicatorResult& ind,
    const MicrostructureData& micro,
    FactorContributions& contrib) noexcept
{
    // 1. RSI：50 为中性 → (RSI - 50) / 50 ∈ [-1, 1]
    double rsi_norm = 0.0;
    if (is_finite_s(ind.rsi_short)) {
        rsi_norm = clamp_s((ind.rsi_short - 50.0) / 50.0, -1.0, 1.0);
    }

    // 2. 资金费率：0.0003（0.03%）为基准
    //    正费率（多头拥挤）→ 看跌；负费率（空头拥挤）→ 看涨
    //    注意：此处是反向逻辑
    double funding_norm = 0.0;
    if (micro.has_funding && is_finite_s(micro.funding_rate)) {
        // 反向：正费率贡献负分
        funding_norm = clamp_s(-micro.funding_rate / 0.0003, -1.0, 1.0);
    }

    // 3. 记录贡献
    contrib.rsi_norm = rsi_norm;
    contrib.funding_norm = funding_norm;

    // 4. 组内合成
    const double score = kSentimentRsiWeight * rsi_norm
                       + kSentimentFundingWeight * funding_norm;

    contrib.sentiment = score;
    return score;
}

// ==============================================================================
// AI 温和增强
// ==============================================================================
double ScoringSystem::apply_ai_boost(double raw_score,
                                       double ai_trend_prob) const noexcept
{
    if (!config_.enable_ai_boost) return raw_score;
    if (!is_finite_s(ai_trend_prob)) return raw_score;

    // 概率 [0, 1] → bias [-1, 1]
    const double prob = clamp_s(ai_trend_prob, 0.0, 1.0);
    const double ai_bias = (prob - 0.5) * 2.0;

    // AI 贡献：bias × weight × 100
    // 最大 ±0.15 × 100 = ±15 分
    const double ai_contribution =
        ai_bias * config_.ai_boost_weight * 100.0;

    const double boosted = raw_score + ai_contribution;
    return clamp_s(boosted, kScoreMin, kScoreMax);
}

// ==============================================================================
// 强度分级
// ==============================================================================
ScoringResult::Strength
ScoringSystem::classify_strength(double abs_score) const noexcept {
    if (abs_score >= 80.0) return ScoringResult::Strength::Strong;
    if (abs_score >= 60.0) return ScoringResult::Strength::Medium;
    if (abs_score >= 30.0) return ScoringResult::Strength::Weak;
    return ScoringResult::Strength::None;
}

// ==============================================================================
// 核心计算
// ==============================================================================
Result<ScoringResult>
ScoringSystem::compute(const IndicatorResult& ind,
                        const BandResult& band,
                        const MicrostructureData& micro,
                        MarketRegime regime,
                        double ai_trend_prob) noexcept
{
    ++stats_.total_computes;

    // -------------------------------------------------------------------------
    // 1. 输入校验
    // -------------------------------------------------------------------------
    if (ind.is_warmup) {
        ++stats_.warmup_skips;
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "指标处于预热期，无法计算评分")
            .with_context("bar", std::to_string(ind.bar_index));
    }

    if (!ind.is_finite()) {
        ++stats_.error_count;
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "指标含 NaN/Inf")
            .with_context("bar", std::to_string(ind.bar_index));
    }

    // band 可允许 warmup，只是参考，不作为主要输入
    (void)band;

    // -------------------------------------------------------------------------
    // 2. 选择权重
    // -------------------------------------------------------------------------
    const GroupWeights weights = select_weights(regime);

    // -------------------------------------------------------------------------
    // 3. 计算三组得分
    // -------------------------------------------------------------------------
    FactorContributions contrib;

    const double momentum_score =
        compute_momentum_group(ind, contrib);
    const double volume_score =
        compute_volume_group(ind, micro, contrib);
    const double sentiment_score =
        compute_sentiment_group(ind, micro, contrib);

    // 组内得分边界保护（理论上已在 [-1, 1] 内）
    const double m = clamp_s(momentum_score, -1.0, 1.0);
    const double v = clamp_s(volume_score, -1.0, 1.0);
    const double s = clamp_s(sentiment_score, -1.0, 1.0);

    // -------------------------------------------------------------------------
    // 4. 组间加权
    // -------------------------------------------------------------------------
    const double raw_score = (
        m * weights.momentum +
        v * weights.volume +
        s * weights.sentiment
    ) * 100.0;

    // -------------------------------------------------------------------------
    // 5. AI 增强
    // -------------------------------------------------------------------------
    const double final_score = apply_ai_boost(raw_score, ai_trend_prob);
    const double ai_boost = final_score - raw_score;

    // -------------------------------------------------------------------------
    // 6. 组装结果
    // -------------------------------------------------------------------------
    ScoringResult result;

    result.timestamp = ind.timestamp;
    result.bar_index = ind.bar_index;
    result.is_valid = true;
    result.is_warmup = false;

    result.score_signed = final_score;
    result.score_long = std::max(0.0, final_score);
    result.score_short = std::max(0.0, -final_score);

    result.momentum_score = m * 100.0;
    result.volume_score = v * 100.0;
    result.sentiment_score = s * 100.0;

    result.effective_weights = weights;
    result.ai_boost_applied = ai_boost;

    // 判定
    const double abs_score = std::abs(final_score);

    result.passes_entry       = final_score >= config_.entry_threshold;
    result.passes_short_entry = final_score <= -config_.entry_threshold;
    result.passes_reverse     = abs_score >= config_.reverse_threshold;

    // 方向
    if (result.passes_entry) {
        result.direction = 1;
        ++stats_.entry_signals;
    } else if (result.passes_short_entry) {
        result.direction = -1;
        ++stats_.entry_signals;
    } else {
        result.direction = 0;
        // 弱信号被过滤
        if (abs_score >= 30.0) {
            ++stats_.filtered_signals;
        }
    }

    if (result.passes_reverse) {
        ++stats_.reverse_signals;
    }

    // 强度分级
    result.strength = classify_strength(abs_score);

    // 因子贡献
    result.contributions = contrib;

    // -------------------------------------------------------------------------
    // 7. 更新历史（供下一根 K 线的分母计算）
    // -------------------------------------------------------------------------
    update_history(ind);
    ++bar_count_;

    return result;
}

// ==============================================================================
// 便捷接口
// ==============================================================================
ScoringResult
ScoringSystem::compute_unchecked(const IndicatorResult& ind,
                                   const BandResult& band,
                                   const MicrostructureData& micro,
                                   MarketRegime regime,
                                   double ai_trend_prob) noexcept
{
    auto r = compute(ind, band, micro, regime, ai_trend_prob);
    if (r.is_ok()) {
        return std::move(r).value();
    }

    // 失败时返回中性结果
    ScoringResult empty;
    empty.timestamp = ind.timestamp;
    empty.bar_index = ind.bar_index;
    empty.is_valid = false;
    empty.is_warmup = ind.is_warmup;
    empty.score_signed = 0.0;
    empty.score_long = 0.0;
    empty.score_short = 0.0;
    empty.direction = 0;
    empty.strength = ScoringResult::Strength::None;
    return empty;
}

// ==============================================================================
// ScoringResult 序列化
// ==============================================================================
std::string ScoringResult::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"ts\":"            << timestamp.microseconds() << ","
        << "\"bar\":"           << bar_index << ","
        << "\"valid\":"         << (is_valid ? "true" : "false") << ","
        << "\"score\":"         << json_double_s(score_signed) << ","
        << "\"score_long\":"    << json_double_s(score_long) << ","
        << "\"score_short\":"   << json_double_s(score_short) << ","
        << "\"momentum\":"      << json_double_s(momentum_score) << ","
        << "\"volume\":"        << json_double_s(volume_score) << ","
        << "\"sentiment\":"     << json_double_s(sentiment_score) << ","
        << "\"w_momentum\":"    << json_double_s(effective_weights.momentum) << ","
        << "\"w_volume\":"      << json_double_s(effective_weights.volume) << ","
        << "\"w_sentiment\":"   << json_double_s(effective_weights.sentiment) << ","
        << "\"ai_boost\":"      << json_double_s(ai_boost_applied) << ","
        << "\"entry\":"         << (passes_entry ? "true" : "false") << ","
        << "\"short_entry\":"   << (passes_short_entry ? "true" : "false") << ","
        << "\"reverse\":"       << (passes_reverse ? "true" : "false") << ","
        << "\"direction\":"     << direction << ","
        << "\"strength\":"      << static_cast<int>(strength)
        << "}";
    return oss.str();
}

std::string ScoringResult::to_string() const {
    std::ostringstream oss;
    oss << "Score{"
        << "bar=" << bar_index
        << ", signed=" << fmt_double_s(score_signed, 1)
        << ", long=" << fmt_double_s(score_long, 1)
        << ", short=" << fmt_double_s(score_short, 1)
        << ", [M=" << fmt_double_s(momentum_score, 1)
        << ", V=" << fmt_double_s(volume_score, 1)
        << ", S=" << fmt_double_s(sentiment_score, 1)
        << "]";

    if (std::abs(ai_boost_applied) > 0.01) {
        oss << ", ai=" << fmt_double_s(ai_boost_applied, 1);
    }

    switch (strength) {
        case Strength::Strong: oss << " STRONG"; break;
        case Strength::Medium: oss << " MEDIUM"; break;
        case Strength::Weak:   oss << " WEAK"; break;
        case Strength::None:   oss << " NONE"; break;
    }

    if (passes_entry)       oss << " ENTRY_LONG";
    if (passes_short_entry) oss << " ENTRY_SHORT";
    if (passes_reverse)     oss << " REVERSE";

    oss << "}";
    return oss.str();
}

}  // namespace indicator
}  // namespace quant
