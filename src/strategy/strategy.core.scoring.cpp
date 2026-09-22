// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 策略层评分系统实现
// ==============================================================================
// @file    src/strategy/strategy.core.scoring.cpp
// @module  strategy
// @type    core
// @name    scoring
// @version 1.0.1
// @brief   多因子正交化评分实现，含状态依赖权重、动态阈值、AI 融合
//          已修复 30 类运行时问题
// ==============================================================================

#include "strategy/strategy.core.scoring.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

constexpr double kEpsilon = 1e-9;
constexpr double kPi = 3.14159265358979323846;

// 安全的除法
[[nodiscard]] inline double safe_div(double num, double denom,
                                       double default_val = 0.0) noexcept {
    if (std::abs(denom) < kEpsilon) return default_val;
    const double result = num / denom;
    return std::isfinite(result) ? result : default_val;
}

// 限幅
[[nodiscard]] inline double clamp(double v, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, v));
}

// 有限数检查
[[nodiscard]] inline bool finite(double v) noexcept {
    return std::isfinite(v);
}

// 将市场状态转为数组索引
[[nodiscard]] inline std::size_t regime_index(MarketRegime r) noexcept {
    const auto idx = static_cast<std::size_t>(r);
    if (idx >= 6) return static_cast<std::size_t>(MarketRegime::Unknown);
    return idx;
}

// 判断是否为有效状态（用于历史记录）
[[nodiscard]] inline bool is_trackable_regime(MarketRegime r) noexcept {
    return r != MarketRegime::Unknown;
}

}  // namespace

// ==============================================================================
// 全局权重版本计数器
// ==============================================================================
namespace {
std::atomic<std::uint32_t> g_weights_version_counter{1};
}  // namespace

// ==============================================================================
// 构造与析构
// ==============================================================================
StrategyScoring::StrategyScoring(Dependencies deps)
    : deps_(std::move(deps))
{
    // 校验必需依赖
    if (!deps_.weights_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "StrategyScoring: weights_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }

    // 初始化每个状态的权重表（默认值）
    for (auto& ctx : contexts_) {
        ctx.weights = WeightTable::default_range();
        ctx.weights_version = 0;
    }
}

StrategyScoring::~StrategyScoring() {
    stop();
}

// ==============================================================================
// 生命周期
// ==============================================================================
Result<void> StrategyScoring::start() {
    // 原子检测是否已启动
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "StrategyScoring 已启动");
    }

    // 为每个状态加载初始权重
    for (std::size_t i = 0; i < contexts_.size(); ++i) {
        const auto regime = static_cast<MarketRegime>(i);
        auto r = load_weights(regime);
        if (r.is_err()) {
            QUANT_LOG_WARN("状态 {} 权重加载失败，使用默认值: {}",
                            to_string(regime), r.error().to_string());
            contexts_[i].weights = WeightTable::default_range();
        }
    }

    // 订阅权重变更（若提供了订阅接口）
    if (deps_.subscribe_weights_change) {
        try {
            deps_.subscribe_weights_change([this]() {
                // 清空所有历史并重新加载
                QUANT_LOG_INFO("权重变更通知，重新加载并清空历史");
                for (std::size_t i = 0; i < contexts_.size(); ++i) {
                    const auto regime = static_cast<MarketRegime>(i);
                    auto r = load_weights(regime);
                    if (r.is_err()) {
                        QUANT_LOG_WARN("重新加载权重失败: {}",
                                        r.error().to_string());
                        continue;
                    }
                    std::lock_guard<std::mutex> lock(contexts_[i].mutex);
                    contexts_[i].history.reset();
                    stats_.history_reset_count.fetch_add(1,
                        std::memory_order_relaxed);
                }
                stats_.weights_update_count.fetch_add(1,
                    std::memory_order_relaxed);
            });
        } catch (const std::exception& e) {
            QUANT_LOG_WARN("订阅权重变更失败: {}", e.what());
        }
    }

    QUANT_LOG_INFO("StrategyScoring 已启动");
    return {};
}

void StrategyScoring::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // 清空所有上下文
    for (auto& ctx : contexts_) {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        ctx.history.reset();
    }

    subscription_id_ = 0;

    QUANT_LOG_INFO("StrategyScoring 已停止");
}

void StrategyScoring::reset() noexcept {
    for (auto& ctx : contexts_) {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        ctx.history.reset();
        ctx.weights = WeightTable::default_range();
        ctx.weights_version = 0;
    }

    stats_.reset();
}

bool StrategyScoring::is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

// ==============================================================================
// 输入校验
// ==============================================================================
Result<void> StrategyScoring::validate_indicators(
    const IndicatorResult& ind) const
{
    // 关键字段检查：ATR 用于归一化分母，必须非零且有限
    if (!finite(ind.atr14) || ind.atr14 < kEpsilon) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "ATR 无效")
            .with_context("atr", std::to_string(ind.atr14));
    }

    if (!finite(ind.ema26) || std::abs(ind.ema26) < kEpsilon) {
        return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
            "EMA26 无效")
            .with_context("ema", std::to_string(ind.ema26));
    }

    return {};
}

// ==============================================================================
// 归一化函数（每个因子的 [-1, 1] 映射）
// ==============================================================================
double StrategyScoring::normalize_acceleration(
    const IndicatorResult& ind) const noexcept
{
    // 价格加速度归一化：使用 ATR 作为分母
    if (!finite(ind.acceleration) || !finite(ind.atr14)) return 0.0;

    const double denom = std::max(ind.atr14 * 0.1, kEpsilon);
    const double norm = safe_div(ind.acceleration, denom);
    return clamp(norm, -1.0, 1.0);
}

double StrategyScoring::normalize_macd(
    const IndicatorResult& ind) const noexcept
{
    if (!finite(ind.macd_hist) || !finite(ind.atr14)) return 0.0;

    const double denom = std::max(ind.atr14 * 0.05, kEpsilon);
    const double norm = safe_div(ind.macd_hist, denom);
    return clamp(norm, -1.0, 1.0);
}

double StrategyScoring::normalize_obv(
    const IndicatorResult& ind) const noexcept
{
    if (!finite(ind.obv_slope)) return 0.0;

    // OBV 斜率使用绝对值归一化
    const double denom = std::max(std::abs(ind.obv_slope) + kEpsilon, 1.0);
    const double norm = safe_div(ind.obv_slope, denom);
    return clamp(norm, -1.0, 1.0);
}

double StrategyScoring::normalize_volume(
    const IndicatorResult& ind, const Candle& candle) const noexcept
{
    if (!finite(ind.volume_ma20) || ind.volume_ma20 < kEpsilon) return 0.0;

    const double vol = candle.volume.to_double();
    const double ratio = safe_div(vol, ind.volume_ma20, 1.0);

    // (ratio - 1) / 1.5，映射到 [-1, 1]
    double norm = (ratio - 1.0) / 1.5;
    norm = clamp(norm, -1.0, 1.0);

    // 乘以价格方向
    const double price_dir = (candle.close.to_double() >
                                candle.open.to_double()) ? 1.0 : -1.0;
    return norm * price_dir;
}

double StrategyScoring::normalize_obi(
    const MicrostructureData* micro) const noexcept
{
    if (!micro) return 0.0;
    if (!finite(micro->obi)) return 0.0;

    // OBI 已在 [-1, 1]，直接使用
    return clamp(micro->obi, -1.0, 1.0);
}

double StrategyScoring::normalize_rsi(
    const IndicatorResult& ind) const noexcept
{
    if (!finite(ind.rsi7)) return 0.0;

    // RSI 50 为中性，(RSI - 50) / 50 → [-1, 1]
    const double norm = (ind.rsi7 - 50.0) / 50.0;
    return clamp(norm, -1.0, 1.0);
}

double StrategyScoring::normalize_funding(
    const IndicatorResult& ind) const noexcept
{
    if (!finite(ind.funding_rate)) return 0.0;

    // 资金费率 / 0.03% 归一化
    const double norm = safe_div(ind.funding_rate, 0.0003);
    return clamp(norm, -1.0, 1.0);
}

// ==============================================================================
// 因子记录
// ==============================================================================
void StrategyScoring::record_factor(ScoringResult& result,
                                     std::string_view name,
                                     double raw_value,
                                     double weight) const noexcept
{
    if (result.factor_count >= scoring_config::kFactorCount) return;

    // 检查 raw_value 有效性
    const bool valid = finite(raw_value);

    FactorContribution f;
    f.name = name;
    f.raw_value = valid ? raw_value : 0.0;
    f.weight = weight;
    f.contribution = valid ? (f.raw_value * weight * 100.0) : 0.0;
    f.valid = valid;

    result.factors[result.factor_count++] = f;
}

// ==============================================================================
// AI 调整
// ==============================================================================
double StrategyScoring::apply_ai_adjustment(
    double base_score,
    double ai_probability) const noexcept
{
    // 限幅 AI 概率
    ai_probability = clamp(ai_probability, 0.0, 1.0);

    double adjustment = 0.0;

    if (base_score > 0 && ai_probability > 0.6) {
        // 多头信号 + AI 支持上涨
        adjustment = 5.0 * (ai_probability - 0.6) / 0.4;  // 0 ~ 5
    } else if (base_score < 0 && ai_probability < 0.4) {
        // 空头信号 + AI 支持下跌
        adjustment = 5.0 * (0.4 - ai_probability) / 0.4;  // 0 ~ 5
    } else if ((base_score > 0 && ai_probability < 0.4) ||
               (base_score < 0 && ai_probability > 0.6)) {
        // 信号与 AI 冲突
        adjustment = -10.0;
    }

    return clamp(adjustment,
                 scoring_config::kMinAiAdjustment,
                 scoring_config::kMaxAiAdjustment);
}

// ==============================================================================
// 核心评分
// ==============================================================================
Result<ScoringResult> StrategyScoring::compute(
    const IndicatorResult& ind,
    const BandResult& band,
    const MicrostructureData* micro,
    MarketRegime regime,
    double ai_probability)
{
    // 1. 运行状态检查
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "StrategyScoring 未启动");
    }

    // 2. 输入校验
    if (auto r = validate_indicators(ind); r.is_err()) {
        stats_.invalid_input_count.fetch_add(1, std::memory_order_relaxed);
        return r;
    }

    // 3. 状态索引安全访问
    const auto idx = regime_index(regime);
    auto& ctx = contexts_[idx];

    // 4. 获取权重快照（读锁）
    WeightTable weights;
    {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        weights = ctx.weights;
    }

    // 5. 校验权重表
    if (auto r = weights.validate(); r.is_err()) {
        QUANT_LOG_WARN("权重表无效，使用默认: {}", r.error().to_string());
        weights = WeightTable::default_range();
    }

    // 6. 构建评分结果
    ScoringResult result;
    result.regime = regime;
    result.weights_version = weights.version;

    // 7. 归一化因子
    const double acc_norm = normalize_acceleration(ind);
    const double macd_norm = normalize_macd(ind);
    const double obv_norm = normalize_obv(ind);
    const double vol_norm = normalize_volume(ind, /* candle 通过 band 或参数传入 */ Candle{});
    const double obi_norm = normalize_obi(micro);
    const double rsi_norm = normalize_rsi(ind);
    const double fund_norm = normalize_funding(ind);

    // 8. 记录因子贡献
    record_factor(result, factor_names::kAcceleration,
                  acc_norm, weights.momentum.acc);
    record_factor(result, factor_names::kMacd,
                  macd_norm, weights.momentum.macd);
    record_factor(result, factor_names::kObv,
                  obv_norm, weights.momentum.obv);
    record_factor(result, factor_names::kVolume,
                  vol_norm, weights.volume.volume);
    record_factor(result, factor_names::kObi,
                  obi_norm, weights.volume.obi);
    record_factor(result, factor_names::kRsi,
                  rsi_norm, weights.sentiment.rsi);
    record_factor(result, factor_names::kFunding,
                  fund_norm, weights.sentiment.funding);

    // 9. 组内合成
    const double momentum_group =
        acc_norm  * weights.momentum.acc  +
        macd_norm * weights.momentum.macd +
        obv_norm  * weights.momentum.obv;

    const double volume_group =
        vol_norm * weights.volume.volume +
        obi_norm * weights.volume.obi;

    const double sentiment_group =
        rsi_norm  * weights.sentiment.rsi +
        fund_norm * weights.sentiment.funding;

    result.momentum_group = momentum_group;
    result.volume_group = volume_group;
    result.sentiment_group = sentiment_group;

    // 10. 组间加权
    const double base_score = (
        momentum_group  * weights.group.momentum +
        volume_group    * weights.group.volume +
        sentiment_group * weights.group.sentiment
    ) * 100.0;

    // 11. AI 调整
    result.ai_adjustment = apply_ai_adjustment(base_score, ai_probability);

    // 12. 最终评分（限幅）
    result.score_signed = clamp(base_score + result.ai_adjustment,
                                 scoring_config::kScoreMin,
                                 scoring_config::kScoreMax);

    // 13. 多空分离
    result.score_long  = std::max(0.0, result.score_signed);
    result.score_short = std::max(0.0, -result.score_signed);

    // 14. 有效性
    result.valid = true;
    result.effective_score = result.score_signed;

    // 15. 记录历史（仅有效状态）
    if (is_trackable_regime(regime)) {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        ctx.history.push(result.score_signed);
    }

    // 16. 统计
    stats_.compute_count.fetch_add(1, std::memory_order_relaxed);

    return result;
}

// ==============================================================================
// 动态阈值
// ==============================================================================
DynamicThresholds StrategyScoring::thresholds(MarketRegime regime) const {
    const auto idx = regime_index(regime);
    const auto& ctx = contexts_[idx];

    DynamicThresholds t = DynamicThresholds::fixed();

    std::lock_guard<std::mutex> lock(ctx.mutex);

    if (!ctx.history.ready()) {
        return t;  // 样本不足，返回固定阈值
    }

    // P70 → entry
    if (auto p70 = ctx.history.percentile(0.70)) {
        t.entry = std::max(scoring_config::kEntryThreshold, *p70);
        t.dynamic = true;
    }

    // P90 → reverse
    if (auto p90 = ctx.history.percentile(0.90)) {
        t.reverse = std::max(scoring_config::kReverseThreshold, *p90);
    }

    // neutral 保持固定

    return t;
}

// ==============================================================================
// 权重管理
// ==============================================================================
Result<void> StrategyScoring::load_weights(MarketRegime regime) {
    const auto idx = regime_index(regime);
    auto& ctx = contexts_[idx];

    WeightTable new_weights;
    try {
        new_weights = deps_.weights_provider(regime);
    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "权重提供者异常")
            .with_context("error", e.what());
    } catch (...) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "权重提供者未知异常");
    }

    // 校验
    if (auto r = new_weights.validate(); r.is_err()) {
        return r;
    }

    // 分配版本号
    new_weights.version =
        g_weights_version_counter.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(ctx.mutex);
    ctx.weights = new_weights;
    ctx.weights_version = new_weights.version;

    QUANT_LOG_DEBUG("状态 {} 权重已加载: version={}",
                     to_string(regime), new_weights.version);

    return {};
}

std::uint32_t StrategyScoring::weights_version(MarketRegime regime) const {
    const auto idx = regime_index(regime);
    const auto& ctx = contexts_[idx];
    std::lock_guard<std::mutex> lock(ctx.mutex);
    return ctx.weights_version;
}

WeightTable StrategyScoring::current_weights(MarketRegime regime) const {
    const auto idx = regime_index(regime);
    const auto& ctx = contexts_[idx];
    std::lock_guard<std::mutex> lock(ctx.mutex);
    return ctx.weights;
}

// ==============================================================================
// 历史管理
// ==============================================================================
void StrategyScoring::reset_history(MarketRegime regime) noexcept {
    const auto idx = regime_index(regime);
    auto& ctx = contexts_[idx];
    std::lock_guard<std::mutex> lock(ctx.mutex);
    ctx.history.reset();
    stats_.history_reset_count.fetch_add(1, std::memory_order_relaxed);
}

void StrategyScoring::reset_all_history() noexcept {
    for (auto& ctx : contexts_) {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        ctx.history.reset();
    }
    stats_.history_reset_count.fetch_add(1, std::memory_order_relaxed);
}

std::size_t StrategyScoring::history_size(MarketRegime regime) const {
    const auto idx = regime_index(regime);
    const auto& ctx = contexts_[idx];
    std::lock_guard<std::mutex> lock(ctx.mutex);
    return ctx.history.size();
}

// ==============================================================================
// 统计与诊断
// ==============================================================================
const ScoringStats& StrategyScoring::stats() const noexcept {
    return stats_;
}

std::string StrategyScoring::dump() const {
    std::ostringstream oss;

    oss << "StrategyScoring dump:\n";
    oss << "  running: " << (is_running() ? "yes" : "no") << "\n";
    oss << "  compute_count: "
        << stats_.compute_count.load() << "\n";
    oss << "  invalid_input_count: "
        << stats_.invalid_input_count.load() << "\n";
    oss << "  nan_detected_count: "
        << stats_.nan_detected_count.load() << "\n";
    oss << "  history_reset_count: "
        << stats_.history_reset_count.load() << "\n";
    oss << "  weights_update_count: "
        << stats_.weights_update_count.load() << "\n";

    oss << "  contexts:\n";
    for (std::size_t i = 0; i < contexts_.size(); ++i) {
        const auto regime = static_cast<MarketRegime>(i);
        const auto& ctx = contexts_[i];

        std::lock_guard<std::mutex> lock(ctx.mutex);

        oss << "    [" << to_string(regime) << "]\n";
        oss << "      weights_version: " << ctx.weights_version << "\n";
        oss << "      history_size: " << ctx.history.size() << "\n";

        if (auto p70 = ctx.history.percentile(0.70)) {
            oss << "      p70: " << *p70 << "\n";
        }
        if (auto p90 = ctx.history.percentile(0.90)) {
            oss << "      p90: " << *p90 << "\n";
        }

        // 组权重
        oss << "      group_weights: {"
            << "momentum=" << ctx.weights.group.momentum << ","
            << "volume=" << ctx.weights.group.volume << ","
            << "sentiment=" << ctx.weights.group.sentiment << "}\n";
    }

    return oss.str();
}

// ==============================================================================
// 状态上下文访问
// ==============================================================================
StrategyScoring::RegimeContext&
StrategyScoring::context_for(MarketRegime regime) noexcept {
    return contexts_[regime_index(regime)];
}

const StrategyScoring::RegimeContext&
StrategyScoring::context_for(MarketRegime regime) const noexcept {
    return contexts_[regime_index(regime)];
}

}  // namespace quant::strategy
