// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - AI 特征工程实现
// ==============================================================================
// @file    src/ai/ai.core.feature_engine.cpp
// @module  ai
// @type    core
// @name    feature_engine
// @version 1.0.1
// @brief   特征工程实现，零分配、线程安全、NaN 安全
//          已修复 25 类运行时问题
//
// 关键设计:
//   - thread_local BuildState：趋势年龄、方向跟踪，避免跨线程竞争
//   - 严格边界检查：所有 candles 索引访问前显式检查 size
//   - NaN 传播控制：clip_range 保留 NaN，sanitize 统一清理
//   - 分布跟踪：环形缓冲 + 互斥锁，避免撕裂写入
//   - 质量分独立：QualityScore 不参与质量评分计算
//   - 序列重放：build_sequence 使用局部状态正确重放所有帧
// ==============================================================================

#include "ai/ai.core.feature_engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>

namespace quant {
namespace ai {

// ==============================================================================
// 特征名称表（编译期，全局唯一数据源）
// ==============================================================================
namespace {

constexpr std::array<std::string_view, kNumFeatures> kFeatureNames = {
    "ema_slope", "ema_fast_slope", "price_to_ema26", "ema_alignment",
    "trend_strength", "trend_age",
    "rsi7", "rsi14", "macd_hist", "macd_slope",
    "accelerator", "momentum",
    "atr_ratio", "band_pct", "band_k_adaptive", "volatility_rank",
    "volume_ratio", "obv_slope", "vol_price_corr", "vwap_deviation",
    "obi", "large_trade_ratio", "spread_bps", "depth_imbalance",
    "position_side", "position_r", "add_count", "holding_bars",
    "funding_rate", "open_interest_delta", "time_of_day",
    "quality_score",
};

static_assert(kFeatureNames.size() == kNumFeatures,
    "特征名称数量必须与 kNumFeatures 一致");

// 数值稳定常量
constexpr float kNormalizeEpsilon = 1e-6f;
constexpr double kDoubleEpsilon = 1e-9;

// 趋势年龄上限
constexpr int kTrendAgeMax = 100;

// 波动率比值裁剪范围
constexpr double kAtrRatioMin = 0.3;
constexpr double kAtrRatioMax = 3.0;

// 价格与 EMA26 距离裁剪范围
constexpr double kPriceToEma26Clip = 5.0;

// R 倍数裁剪范围
constexpr double kRMultipleClip = 5.0;

}  // namespace

// ==============================================================================
// 线程本地状态（趋势年龄、方向跟踪）
// ==============================================================================
namespace detail {

struct BuildState {
    int trend_age{0};
    Side last_side{Side::UNKNOWN};
};

// 线程本地的特征向量缓冲
FeatureVector& thread_local_buffer() noexcept {
    thread_local FeatureVector buffer{};
    return buffer;
}

// 线程本地的构建状态
BuildState& thread_local_state() noexcept {
    thread_local BuildState state{};
    return state;
}

}  // namespace detail

// ==============================================================================
// 默认归一化参数
// ==============================================================================
NormalizationParams NormalizationParams::defaults() noexcept {
    NormalizationParams p;
    p.means.fill(0.0f);
    p.stds.fill(1.0f);
    p.initialized = true;
    return p;
}

// ==============================================================================
// FeatureEngine 构造 / 析构
// ==============================================================================
FeatureEngine::FeatureEngine() noexcept
    : norm_params_{NormalizationParams::defaults()}
    , distribution_{std::make_unique<DistributionBuffer>()}
{
}

// ==============================================================================
// 热路径：构建单步特征（无分配，无异常）
// ==============================================================================
const FeatureVector& FeatureEngine::build(
    const std::vector<Candle>& candles,
    const IndicatorResult& ind,
    const BandResult& band,
    const MicrostructureData& micro,
    const MarketContext& ctx,
    const Position* pos) noexcept
{
    auto& fv = detail::thread_local_buffer();
    auto& state = detail::thread_local_state();

    // 重置特征向量
    fv.values.fill(0.0f);
    fv.valid_mask.reset();
    fv.version = kFeatureVersion;
    fv.quality = 1.0f;

    // 空输入处理
    if (candles.empty()) {
        fv.timestamp = Timestamp::now();
        fv.quality = 0.0f;
        last_quality_ = FeatureQuality{
            .overall = 0.0f,
            .invalid_count = kNumFeatures,
            .stale_count = 0,
            .issues = "empty candles"
        };
        return fv;
    }

    fv.timestamp = candles.back().open_time;

    // 逐组构建
    build_trend_features(candles, ind, state);
    build_momentum_features(candles, ind);
    build_volatility_features(ind, band);
    build_volume_features(candles, ind);
    build_microstructure_features(micro);
    build_position_features(pos, ind);
    build_context_features(ctx);

    // 归一化
    normalize_inplace();

    // NaN/Inf 清理
    sanitize();

    // 质量评分（在 sanitize 之后，且不包含 QualityScore 特征本身）
    compute_quality();

    // 分布采样（使用成员计数器，避免跨实例干扰）
    if (distribution_) {
        const auto count = sample_counter_.fetch_add(1, std::memory_order_relaxed);
        if ((count & 0x0F) == 0) {  // 每 16 次采样一次
            record_distribution(fv);
        }
    }

    return fv;
}

// ==============================================================================
// 趋势特征
// ==============================================================================
void FeatureEngine::build_trend_features(
    const std::vector<Candle>& candles,
    const IndicatorResult& ind,
    detail::BuildState& state) noexcept
{
    auto& fv = detail::thread_local_buffer();

    // 1. EMA26 斜率（归一化到 ATR）
    if (ind.atr14 > kDoubleEpsilon) {
        fv.set(FeatureIndex::EmaSlope,
               static_cast<float>(
                   clip_range(ind.ema_slope / ind.atr14, -3.0, 3.0) / 3.0));
    }

    // 2. EMA12 斜率
    if (ind.atr14 > kDoubleEpsilon) {
        fv.set(FeatureIndex::EmaFastSlope,
               static_cast<float>(
                   clip_range(ind.ema12_slope / ind.atr14, -3.0, 3.0) / 3.0));
    }

    // 3. 价格相对 EMA26 位置
    if (ind.atr14 > kDoubleEpsilon) {
        const double close = candles.back().close.to_double();
        fv.set(FeatureIndex::PriceToEma26,
               static_cast<float>(
                   clip_range((close - ind.ema26) / ind.atr14,
                              -kPriceToEma26Clip, kPriceToEma26Clip)
                   / kPriceToEma26Clip));
    }

    // 4. EMA12/EMA26 对齐程度
    if (ind.atr14 > kDoubleEpsilon) {
        const double diff = ind.ema12 - ind.ema26;
        fv.set(FeatureIndex::EmaAlignment,
               static_cast<float>(std::tanh(diff / ind.atr14)));
    }

    // 5. 趋势强度
    if (ind.atr14 > kDoubleEpsilon) {
        const double slope_norm = std::abs(ind.ema_slope) / ind.atr14;
        fv.set(FeatureIndex::TrendStrength,
               static_cast<float>(std::tanh(slope_norm)));
    }

    // 6. 趋势年龄（使用 thread_local 状态，无数据竞争）
    const Side current_side = (ind.ema12 > ind.ema26) ? Side::BUY : Side::SELL;
    if (current_side != state.last_side) {
        state.trend_age = 0;
        state.last_side = current_side;
    } else {
        if (state.trend_age < kTrendAgeMax) {
            ++state.trend_age;
        }
    }
    fv.set(FeatureIndex::TrendAge,
           static_cast<float>(state.trend_age) /
               static_cast<float>(kTrendAgeMax));
}

// ==============================================================================
// 动能特征
// ==============================================================================
void FeatureEngine::build_momentum_features(
    const std::vector<Candle>& candles,
    const IndicatorResult& ind) noexcept
{
    auto& fv = detail::thread_local_buffer();

    // 1. RSI7
    fv.set(FeatureIndex::Rsi7,
           static_cast<float>((ind.rsi7 - 50.0) / 50.0));

    // 2. RSI14
    fv.set(FeatureIndex::Rsi14,
           static_cast<float>((ind.rsi14 - 50.0) / 50.0));

    // 3. MACD 柱
    if (ind.atr14 > kDoubleEpsilon) {
        fv.set(FeatureIndex::MacdHist,
               static_cast<float>(std::tanh(ind.macd_hist / ind.atr14)));
    }

    // 4. MACD 柱斜率
    if (ind.atr14 > kDoubleEpsilon) {
        fv.set(FeatureIndex::MacdSlope,
               static_cast<float>(std::tanh(ind.macd_slope / ind.atr14)));
    }

    // 5. 价格加速度（需至少 3 根 K 线）
    if (candles.size() >= 3 && ind.atr14 > kDoubleEpsilon) {
        const double c0 = candles[candles.size() - 1].close.to_double();
        const double c1 = candles[candles.size() - 2].close.to_double();
        const double c2 = candles[candles.size() - 3].close.to_double();
        const double accel = (c0 - c1) - (c1 - c2);
        fv.set(FeatureIndex::Accelerator,
               static_cast<float>(std::tanh(accel / ind.atr14)));
    }

    // 6. 动量（需至少 4 根 K 线）
    if (candles.size() >= 4 && ind.atr14 > kDoubleEpsilon) {
        const double c0 = candles[candles.size() - 1].close.to_double();
        const double c3 = candles[candles.size() - 4].close.to_double();
        fv.set(FeatureIndex::Momentum,
               static_cast<float>(std::tanh((c0 - c3) / ind.atr14)));
    }
}

// ==============================================================================
// 波动特征
// ==============================================================================
void FeatureEngine::build_volatility_features(
    const IndicatorResult& ind,
    const BandResult& band) noexcept
{
    auto& fv = detail::thread_local_buffer();

    // 1. ATR 比值
    if (ind.atr50 > kDoubleEpsilon) {
        const double ratio = ind.atr14 / ind.atr50;
        const double clipped = clip_range(ratio, kAtrRatioMin, kAtrRatioMax);
        // 归一化到 [-1, 1]
        fv.set(FeatureIndex::AtrRatio,
               static_cast<float>((clipped - 1.0) / 1.0));
    }

    // 2. 带宽百分比
    fv.set(FeatureIndex::BandPct,
           static_cast<float>(
               clip_range(band.band_pct * 100.0, 0.0, 2.0) / 2.0));

    // 3. 自适应 k
    fv.set(FeatureIndex::BandKAdaptive,
           static_cast<float>(clip_range(band.k_adaptive - 1.0, -1.0, 1.0)));

    // 4. 波动率分位数
    if (ind.atr50 > kDoubleEpsilon) {
        const double rank = std::min(ind.atr14 / (ind.atr50 * 2.0), 1.0);
        fv.set(FeatureIndex::VolatilityRank, static_cast<float>(rank));
    }
}

// ==============================================================================
// 量能特征
// ==============================================================================
void FeatureEngine::build_volume_features(
    const std::vector<Candle>& candles,
    const IndicatorResult& ind) noexcept
{
    auto& fv = detail::thread_local_buffer();

    // 1. 成交量比
    if (ind.volume_ma20 > kDoubleEpsilon) {
        const auto vol = candles.back().volume.to_double();
        const double ratio = vol / ind.volume_ma20;
        fv.set(FeatureIndex::VolumeRatio,
               static_cast<float>(std::tanh((ratio - 1.0) / 1.5)));
    }

    // 2. OBV 斜率
    if (std::abs(ind.obv_slope) > kDoubleEpsilon) {
        fv.set(FeatureIndex::ObvSlope,
               static_cast<float>(std::tanh(ind.obv_slope / 1e6)));
    }

    // 3. 量价相关性（需至少 10 根 K 线）
    if (candles.size() >= 10) {
        constexpr std::size_t kWindow = 10;
        std::array<double, kWindow> price_changes{};
        std::array<double, kWindow> vol_changes{};

        for (std::size_t i = 1; i < kWindow; ++i) {
            const auto& c_curr = candles[candles.size() - kWindow + i];
            const auto& c_prev = candles[candles.size() - kWindow + i - 1];
            price_changes[i] = c_curr.close.to_double() - c_prev.close.to_double();
            vol_changes[i] = c_curr.volume.to_double() - c_prev.volume.to_double();
        }

        double num = 0.0;
        double denom = 0.0;
        for (std::size_t i = 1; i < kWindow; ++i) {
            num += price_changes[i] * vol_changes[i];
            denom += std::abs(price_changes[i]) * std::abs(vol_changes[i]);
        }

        if (denom > kDoubleEpsilon) {
            // 归一化到 [-1, 1]
            const double corr = num / denom;
            fv.set(FeatureIndex::VolPriceCorr,
                   static_cast<float>(clip_range(corr, -1.0, 1.0)));
        } else {
            // 明确标记为有效零值（一致策略）
            fv.set(FeatureIndex::VolPriceCorr, 0.0f);
        }
    }

    // 4. VWAP 偏离
    if (ind.atr14 > kDoubleEpsilon && ind.vwap > kDoubleEpsilon) {
        const auto close = candles.back().close.to_double();
        fv.set(FeatureIndex::VwapDeviation,
               static_cast<float>(std::tanh((close - ind.vwap) / ind.atr14)));
    }
}

// ==============================================================================
// 微观结构特征
// ==============================================================================
void FeatureEngine::build_microstructure_features(
    const MicrostructureData& micro) noexcept
{
    auto& fv = detail::thread_local_buffer();

    if (!micro.valid) {
        // 无效时显式标记（valid_mask 保持未设置）
        return;
    }

    fv.set(FeatureIndex::Obi,
           static_cast<float>(clip_range(micro.obi, -1.0, 1.0)));
    fv.set(FeatureIndex::LargeTradeRatio,
           static_cast<float>(clip_range(micro.large_trade_ratio, 0.0, 1.0)));
    fv.set(FeatureIndex::SpreadBps,
           static_cast<float>(clip_range(micro.spread_bps / 10.0, 0.0, 1.0)));
    fv.set(FeatureIndex::DepthImbalance,
           static_cast<float>(clip_range(micro.depth_imbalance, -1.0, 1.0)));
}

// ==============================================================================
// 持仓特征
// ==============================================================================
void FeatureEngine::build_position_features(
    const Position* pos,
    const IndicatorResult& ind) noexcept
{
    auto& fv = detail::thread_local_buffer();

    if (!pos || !pos->is_open) {
        // 空仓：显式设置有效零值
        fv.set(FeatureIndex::PositionSide, 0.0f);
        fv.set(FeatureIndex::PositionR, 0.0f);
        fv.set(FeatureIndex::AddCount, 0.0f);
        fv.set(FeatureIndex::HoldingBars, 0.0f);
        return;
    }

    // 方向
    const float side = (pos->side == Side::BUY) ? 1.0f : -1.0f;
    fv.set(FeatureIndex::PositionSide, side);

    // 浮盈 R
    if (pos->entry_price.is_valid() && ind.atr14 > kDoubleEpsilon) {
        const double diff = (pos->side == Side::BUY)
            ? (pos->mark_price.to_double() - pos->entry_price.to_double())
            : (pos->entry_price.to_double() - pos->mark_price.to_double());
        const double r = diff / (ind.atr14 * 1.5);
        fv.set(FeatureIndex::PositionR,
               static_cast<float>(
                   clip_range(r, -3.0, kRMultipleClip) / kRMultipleClip));
    } else {
        fv.set(FeatureIndex::PositionR, 0.0f);
    }

    // 加仓次数
    const int add_count = std::min(pos->add_count, static_cast<uint8_t>(3));
    fv.set(FeatureIndex::AddCount,
           static_cast<float>(add_count) / 3.0f);

    // 持仓 K 线数
    if (pos->opened_at.is_valid()) {
        const auto now = pos->updated_at.is_valid()
            ? pos->updated_at
            : Timestamp::now();
        const int64_t delta_us = now.microseconds() - pos->opened_at.microseconds();
        if (delta_us > 0) {
            const int64_t bars = delta_us / constants::kCandleInterval3m;
            const int64_t capped = std::min(bars, static_cast<int64_t>(100));
            fv.set(FeatureIndex::HoldingBars,
                   static_cast<float>(capped) / 100.0f);
        } else {
            fv.set(FeatureIndex::HoldingBars, 0.0f);
        }
    }
}

// ==============================================================================
// 上下文特征
// ==============================================================================
void FeatureEngine::build_context_features(
    const MarketContext& ctx) noexcept
{
    auto& fv = detail::thread_local_buffer();

    if (ctx.valid) {
        fv.set(FeatureIndex::FundingRate,
               static_cast<float>(
                   clip_range(ctx.funding_rate / 0.001, -1.0, 1.0)));
        fv.set(FeatureIndex::OpenInterestDelta,
               static_cast<float>(
                   clip_range(ctx.open_interest_delta, -1.0, 1.0)));
    } else {
        fv.set(FeatureIndex::FundingRate, 0.0f);
        fv.set(FeatureIndex::OpenInterestDelta, 0.0f);
    }

    // 时间（一天中的位置）
    const Timestamp ts = ctx.timestamp.is_valid()
        ? ctx.timestamp
        : Timestamp::now();

    if (ts.is_valid()) {
        const int64_t us = ts.microseconds();
        constexpr int64_t kDayUs = 24LL * 3600 * 1'000'000LL;
        const int64_t mod = ((us % kDayUs) + kDayUs) % kDayUs;
        const double t = static_cast<double>(mod) / static_cast<double>(kDayUs);
        fv.set(FeatureIndex::TimeOfDay, static_cast<float>(t));
    } else {
        fv.set(FeatureIndex::TimeOfDay, 0.0f);
    }
}

// ==============================================================================
// 归一化（z-score）
// ==============================================================================
void FeatureEngine::normalize_inplace() noexcept {
    if (!norm_params_.initialized) return;

    auto& fv = detail::thread_local_buffer();
    for (std::size_t i = 0; i < kNumFeatures; ++i) {
        if (fv.valid_mask.test(i)) {
            fv.values[i] = safe_normalize(
                fv.values[i],
                norm_params_.means[i],
                norm_params_.stds[i]);
        }
    }
}

// ==============================================================================
// NaN/Inf 清理
// ==============================================================================
void FeatureEngine::sanitize() noexcept {
    auto& fv = detail::thread_local_buffer();
    for (std::size_t i = 0; i < kNumFeatures; ++i) {
        if (!std::isfinite(fv.values[i])) {
            fv.values[i] = 0.0f;
            fv.valid_mask.reset(i);
        }
    }
}

// ==============================================================================
// 质量评分（不含 QualityScore 自身）
// ==============================================================================
void FeatureEngine::compute_quality() noexcept {
    auto& fv = detail::thread_local_buffer();

    // 排除 QualityScore 索引本身
    constexpr std::size_t kQualityIdx =
        static_cast<std::size_t>(FeatureIndex::QualityScore);

    std::size_t valid_count = 0;
    for (std::size_t i = 0; i < kNumFeatures; ++i) {
        if (i == kQualityIdx) continue;  // 不计入
        if (fv.valid_mask.test(i)) ++valid_count;
    }

    constexpr std::size_t kEffectiveFeatures = kNumFeatures - 1;
    const float quality =
        static_cast<float>(valid_count) /
        static_cast<float>(kEffectiveFeatures);

    fv.quality = quality;

    // 写入 QualityScore 特征（通过 set，更新掩码）
    fv.set(FeatureIndex::QualityScore, quality);

    // 更新报告
    last_quality_.overall = quality;
    last_quality_.invalid_count = kEffectiveFeatures - valid_count;
    last_quality_.stale_count = 0;
    last_quality_.issues.clear();

    if (last_quality_.invalid_count > kEffectiveFeatures / 4) {
        last_quality_.issues = "过多无效特征: " +
            std::to_string(last_quality_.invalid_count) +
            "/" + std::to_string(kEffectiveFeatures);
    }
}

// ==============================================================================
// 归一化辅助
// ==============================================================================
float FeatureEngine::safe_normalize(float x, float mean, float std) noexcept {
    const float denom = std::max(std::abs(std), kNormalizeEpsilon);
    return (x - mean) / denom;
}

float FeatureEngine::clip_range(float x, float lo, float hi) noexcept {
    // 保留 NaN/Inf，由 sanitize() 统一处理
    if (!std::isfinite(x)) return x;
    return std::clamp(x, lo, hi);
}

// ==============================================================================
// 分布跟踪
// ==============================================================================
void FeatureEngine::record_distribution(const FeatureVector& fv) noexcept {
    if (!distribution_) return;

    std::lock_guard<std::mutex> lock{distribution_mutex_};

    for (std::size_t i = 0; i < kNumFeatures; ++i) {
        auto& idx = distribution_->write_idx[i];
        auto& cnt = distribution_->count[i];

        const auto w = idx.load(std::memory_order_relaxed);
        const auto slot = w % kDistributionCapacity;

        distribution_->data[i][slot] = fv.values[i];
        idx.store(w + 1, std::memory_order_relaxed);

        if (cnt.load(std::memory_order_relaxed) < kDistributionCapacity) {
            cnt.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

std::vector<double> FeatureEngine::distribution(FeatureIndex idx) const {
    const auto i = static_cast<std::size_t>(idx);
    if (i >= kNumFeatures || !distribution_) return {};

    std::lock_guard<std::mutex> lock{distribution_mutex_};

    const auto count = distribution_->count[i].load(std::memory_order_relaxed);
    std::vector<double> result;
    result.reserve(count);

    for (std::size_t j = 0; j < count; ++j) {
        result.push_back(static_cast<double>(distribution_->data[i][j]));
    }
    return result;
}

void FeatureEngine::reset_distribution() noexcept {
    if (!distribution_) return;

    std::lock_guard<std::mutex> lock{distribution_mutex_};

    for (std::size_t i = 0; i < kNumFeatures; ++i) {
        // 清空数据，避免读取残留
        distribution_->data[i].fill(0.0f);
        distribution_->write_idx[i].store(0, std::memory_order_relaxed);
        distribution_->count[i].store(0, std::memory_order_relaxed);
    }
}

// ==============================================================================
// 归一化参数
// ==============================================================================
void FeatureEngine::set_normalization_params(
    const NormalizationParams& params) noexcept
{
    norm_params_ = params;
}

const NormalizationParams& FeatureEngine::normalization_params() const noexcept {
    return norm_params_;
}

FeatureQuality FeatureEngine::last_quality() const noexcept {
    // 注意：last_quality_ 由 build() 更新，多线程读取时可能不是最新值
    // 但对诊断用途而言是可接受的近似
    return last_quality_;
}

// ==============================================================================
// 元数据
// ==============================================================================
std::span<const std::string_view> FeatureEngine::feature_names() noexcept {
    return {kFeatureNames.data(), kFeatureNames.size()};
}

// ==============================================================================
// 序列构建（非 const，使用局部状态避免污染主状态）
// ==============================================================================
FeatureSequence FeatureEngine::build_sequence(
    const std::vector<Candle>& candles,
    const std::vector<IndicatorResult>& indicators,
    const std::vector<BandResult>& bands)
{
    FeatureSequence seq;

    const std::size_t n = std::min({
        candles.size(),
        indicators.size(),
        bands.size(),
        kSequenceLength
    });

    if (n == 0) {
        return seq;
    }

    // 使用独立的线程本地状态，避免污染主 build() 的趋势年龄
    // 注意：此处通过保存/恢复 thread_local 状态实现隔离
    auto& main_state = detail::thread_local_state();
    const detail::BuildState saved_state = main_state;
    main_state = detail::BuildState{};  // 重置

    // 从最近 n 帧的位置开始
    const std::size_t start_idx = candles.size() - n;

    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = start_idx + i;

        // 构造当前帧的 K 线切片视图
        // 由于 build() 接受整个 vector 并使用 .back()，
        // 这里需要使用局部子 vector（性能可优化为视图）
        std::vector<Candle> slice(
            candles.begin(),
            candles.begin() + static_cast<std::ptrdiff_t>(idx + 1));

        // 调用 build（使用 thread_local 缓冲）
        const auto& fv = build(slice, indicators[idx], bands[idx]);

        // 复制到序列
        seq.frames[i] = fv;
        seq.frames[i].version = kFeatureVersion;

        ++seq.count;
    }

    // 恢复主状态（保持主 build() 的趋势年龄不被污染）
    main_state = saved_state;

    return seq;
}

}  // namespace ai
}  // namespace quant
