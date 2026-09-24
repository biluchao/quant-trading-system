// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - AI 特征工程
// ==============================================================================
// @file    src/ai/ai.core.feature_engine.hpp
// @module  ai
// @type    core
// @name    feature_engine
// @version 1.0.1
// @brief   将原始市场数据转换为模型输入特征，编译期固定顺序、零分配、NaN 安全
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 编译期顺序：FeatureIndex 枚举固定特征顺序，训练推理共享
//   - 零分配热路径：std::array 栈上分配，无 malloc
//   - NaN 安全：sanitize() 清理 NaN/Inf，质量评分降级
//   - 版本管理：kFeatureVersion 编译期常量，模型校验
//   - 分布跟踪：有界环形缓冲，滑动窗口 PSI
//   - 线程安全：无状态设计 + thread_local 缓冲
//   - 类型一致：全部 float32，与 ONNX 模型对齐
//   - 前视偏差：明确只用 [0, t-1] 数据
// ==============================================================================

#ifndef QUANT_AI_CORE_FEATURE_ENGINE_HPP
#define QUANT_AI_CORE_FEATURE_ENGINE_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ==============================================================================
// 项目依赖
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.timestamp.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "indicator/indicator.core.calculator.hpp"
#include "indicator/indicator.core.band.hpp"

namespace quant {
namespace ai {

// ==============================================================================
// 特征版本与维度
// ==============================================================================
inline constexpr uint32_t kFeatureVersion = 1;
inline constexpr std::size_t kNumFeatures = 32;
inline constexpr std::size_t kSequenceLength = 50;   // 时间序列长度
inline constexpr std::size_t kDistributionCapacity = 10000;

// ==============================================================================
// 特征索引（编译期固定顺序，训练/推理/服务三方共享）
// ==============================================================================
// 警告：修改此枚举会破坏与已训练模型的兼容性。
//       如需新增特征，必须：
//         1. 在末尾追加
//         2. 递增 kFeatureVersion
//         3. 重新训练模型
enum class FeatureIndex : std::size_t {
    // ---------- 趋势特征 (0~5) ----------
    EmaSlope           = 0,   // EMA26 斜率（归一化）
    EmaFastSlope       = 1,   // EMA12 斜率
    PriceToEma26       = 2,   // (close - ema26) / atr14
    EmaAlignment       = 3,   // EMA12 与 EMA26 同向程度
    TrendStrength      = 4,   // 趋势强度 [0, 1]
    TrendAge           = 5,   // 趋势年龄（K线数/100）

    // ---------- 动能特征 (6~11) ----------
    Rsi7               = 6,   // RSI7 归一化到 [-1, 1]
    Rsi14              = 7,   // RSI14 归一化
    MacdHist           = 8,   // MACD 柱归一化
    MacdSlope          = 9,   // MACD 柱斜率
    Accelerator        = 10,  // 价格加速度
    Momentum           = 11,  // 动量 (close_t - close_{t-3}) / atr

    // ---------- 波动特征 (12~15) ----------
    AtrRatio           = 12,  // ATR14 / ATR50
    BandPct            = 13,  // 带宽百分比
    BandKAdaptive      = 14,  // 自适应 k
    VolatilityRank     = 15,  // 波动率分位数 [0, 1]

    // ---------- 量能特征 (16~19) ----------
    VolumeRatio        = 16,  // 当前量 / MA20
    ObvSlope           = 17,  // OBV 斜率
    VolPriceCorr       = 18,  // 量价相关性
    VwapDeviation      = 19,  // VWAP 偏离

    // ---------- 微观结构 (20~23) ----------
    Obi                = 20,  // 订单簿失衡
    LargeTradeRatio    = 21,  // 大单占比
    SpreadBps          = 22,  // 价差（基点）
    DepthImbalance     = 23,  // 深度失衡

    // ---------- 持仓状态 (24~27) ----------
    PositionSide       = 24,  // 持仓方向 (-1/0/+1)
    PositionR          = 25,  // 当前浮盈 R 倍数
    AddCount           = 26,  // 加仓次数 / 3
    HoldingBars        = 27,  // 持仓K线数 / 100

    // ---------- 市场上下文 (28~30) ----------
    FundingRate        = 28,  // 资金费率
    OpenInterestDelta  = 29,  // 持仓量变化
    TimeOfDay          = 30,  // 一天中的时间 (0~1)

    // ---------- 派生特征 (31) ----------
    QualityScore       = 31,  // 特征质量 [0, 1]

    Count              = 32,
};

static_assert(static_cast<std::size_t>(FeatureIndex::Count) == kNumFeatures,
    "FeatureIndex 数量必须与 kNumFeatures 一致");

// ==============================================================================
// 特征名称表（编译期）
// ==============================================================================
[[nodiscard]] constexpr std::string_view feature_name(FeatureIndex idx) noexcept {
    constexpr std::array<std::string_view, kNumFeatures> kNames = {
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
    const auto i = static_cast<std::size_t>(idx);
    return (i < kNumFeatures) ? kNames[i] : "unknown";
}

// ==============================================================================
// 特征向量（固定大小，零分配）
// ==============================================================================
struct FeatureVector {
    std::array<float, kNumFeatures> values{};
    uint32_t version{kFeatureVersion};
    Timestamp timestamp{};
    float quality{1.0f};                    // 特征质量 [0, 1]
    std::bitset<kNumFeatures> valid_mask{}; // 每位表示该特征是否有效

    [[nodiscard]] std::size_t size() const noexcept { return kNumFeatures; }

    [[nodiscard]] float at(FeatureIndex idx) const noexcept {
        return values[static_cast<std::size_t>(idx)];
    }

    void set(FeatureIndex idx, float v) noexcept {
        const auto i = static_cast<std::size_t>(idx);
        values[i] = v;
        valid_mask.set(i, std::isfinite(v));
    }

    [[nodiscard]] bool all_valid() const noexcept {
        return valid_mask.all();
    }

    [[nodiscard]] std::size_t invalid_count() const noexcept {
        return kNumFeatures - valid_mask.count();
    }

    [[nodiscard]] std::span<const float> as_span() const noexcept {
        return {values.data(), values.size()};
    }

    // 用于 ONNX Runtime 输入形状 [1, 1, kNumFeatures]
    [[nodiscard]] std::vector<int64_t> shape() const {
        return {1, 1, static_cast<int64_t>(kNumFeatures)};
    }
};

// ==============================================================================
// 特征序列（用于 LSTM/Transformer）
// ==============================================================================
struct FeatureSequence {
    std::array<FeatureVector, kSequenceLength> frames{};
    std::size_t count{0};   // 实际填充数量

    [[nodiscard]] bool is_ready() const noexcept {
        return count >= kSequenceLength;
    }

    [[nodiscard]] std::vector<int64_t> shape() const {
        return {1, static_cast<int64_t>(count),
                static_cast<int64_t>(kNumFeatures)};
    }
};

// ==============================================================================
// 微观结构数据（输入）
// ==============================================================================
struct MicrostructureData {
    double obi{0.0};                // 订单簿失衡 [-1, 1]
    double large_trade_ratio{0.0};  // 大单占比 [0, 1]
    double spread_bps{0.0};         // 价差（基点）
    double depth_imbalance{0.0};    // 深度失衡 [-1, 1]
    bool valid{false};
};

// ==============================================================================
// 市场上下文（输入）
// ==============================================================================
struct MarketContext {
    double funding_rate{0.0};         // 资金费率
    double open_interest_delta{0.0};  // 持仓量变化（归一化）
    Timestamp timestamp{};
    bool valid{false};
};

// ==============================================================================
// 特征质量报告
// ==============================================================================
struct FeatureQuality {
    float overall{1.0f};
    std::size_t invalid_count{0};
    std::size_t stale_count{0};       // 使用前值填充的数量
    std::string issues;
};

// ==============================================================================
// 归一化参数（可从配置或模型 metadata 加载）
// ==============================================================================
struct NormalizationParams {
    // 每个特征的均值和标准差（用于 z-score）
    std::array<float, kNumFeatures> means{};
    std::array<float, kNumFeatures> stds{};
    bool initialized{false};

    [[nodiscard]] static NormalizationParams defaults() noexcept;
};

// ==============================================================================
// 特征引擎
// ==============================================================================
class FeatureEngine {
public:
    FeatureEngine() noexcept;
    ~FeatureEngine() = default;

    FeatureEngine(const FeatureEngine&) = delete;
    FeatureEngine& operator=(const FeatureEngine&) = delete;

    // -------------------------------------------------------------------------
    // 热路径：构建单步特征（无分配，无异常）
    // -------------------------------------------------------------------------
    // 输入:
    //   candles: 已收盘 K 线序列（candles.back() 是最近一根已收盘）
    //   ind: 指标结果
    //   band: 带宽结果
    //   micro: 微观结构（可选）
    //   ctx: 市场上下文（可选）
    //   pos: 当前持仓（可选）
    // 输出:
    //   引用到内部 thread_local 缓冲（调用方不应长期持有）
    [[nodiscard]] const FeatureVector& build(
        const std::vector<Candle>& candles,
        const IndicatorResult& ind,
        const BandResult& band,
        const MicrostructureData& micro = {},
        const MarketContext& ctx = {},
        const Position* pos = nullptr) noexcept;

    // -------------------------------------------------------------------------
    // 构建序列（用于 LSTM/Transformer 输入）
    // -------------------------------------------------------------------------
    // 从最近 N 根 K 线构建序列特征
    [[nodiscard]] FeatureSequence build_sequence(
        const std::vector<Candle>& candles,
        const std::vector<IndicatorResult>& indicators,
        const std::vector<BandResult>& bands) const;

    // -------------------------------------------------------------------------
    // 分布跟踪（用于 PSI 漂移检测）
    // -------------------------------------------------------------------------
    void record_distribution(const FeatureVector& fv) noexcept;
    [[nodiscard]] std::vector<double> distribution(FeatureIndex idx) const;
    void reset_distribution() noexcept;

    // -------------------------------------------------------------------------
    // 归一化参数
    // -------------------------------------------------------------------------
    void set_normalization_params(const NormalizationParams& params) noexcept;
    [[nodiscard]] const NormalizationParams& normalization_params() const noexcept;

    // -------------------------------------------------------------------------
    // 质量报告
    // -------------------------------------------------------------------------
    [[nodiscard]] FeatureQuality last_quality() const noexcept;

    // -------------------------------------------------------------------------
    // 元数据
    // -------------------------------------------------------------------------
    [[nodiscard]] static std::span<const std::string_view> feature_names() noexcept;
    [[nodiscard]] static uint32_t version() noexcept { return kFeatureVersion; }
    [[nodiscard]] static std::size_t size() noexcept { return kNumFeatures; }

private:
    // -------------------------------------------------------------------------
    // 内部：构建单个特征
    // -------------------------------------------------------------------------
    void build_trend_features(const std::vector<Candle>& candles,
                               const IndicatorResult& ind) noexcept;
    void build_momentum_features(const std::vector<Candle>& candles,
                                  const IndicatorResult& ind) noexcept;
    void build_volatility_features(const IndicatorResult& ind,
                                    const BandResult& band) noexcept;
    void build_volume_features(const std::vector<Candle>& candles,
                                const IndicatorResult& ind) noexcept;
    void build_microstructure_features(const MicrostructureData& micro) noexcept;
    void build_position_features(const Position* pos,
                                  const IndicatorResult& ind) noexcept;
    void build_context_features(const MarketContext& ctx) noexcept;

    // -------------------------------------------------------------------------
    // 内部：归一化与清理
    // -------------------------------------------------------------------------
    void normalize_inplace() noexcept;
    void sanitize() noexcept;
    void compute_quality() noexcept;

    // -------------------------------------------------------------------------
    // 内部：归一化辅助
    // -------------------------------------------------------------------------
    [[nodiscard]] static float safe_normalize(float x, float mean,
                                               float std) noexcept;
    [[nodiscard]] static float clip_range(float x, float lo, float hi) noexcept;

    // -------------------------------------------------------------------------
    // 状态
    // -------------------------------------------------------------------------
    NormalizationParams norm_params_;

    // 分布跟踪（环形缓冲）
    struct DistributionBuffer {
        std::array<std::array<float, kDistributionCapacity>, kNumFeatures> data{};
        std::array<std::atomic<std::size_t>, kNumFeatures> write_idx{};
        std::array<std::atomic<std::size_t>, kNumFeatures> count{};

        DistributionBuffer() noexcept {
            for (auto& w : write_idx) w.store(0, std::memory_order_relaxed);
            for (auto& c : count) c.store(0, std::memory_order_relaxed);
        }
    };
    std::unique_ptr<DistributionBuffer> distribution_;

    // 最近一次的质量报告
    FeatureQuality last_quality_{};

    // 趋势年龄跟踪（状态）
    int trend_age_{0};
    Direction last_trend_direction_{Direction::NEUTRAL};
};

// ==============================================================================
// thread_local 缓冲（避免多线程竞争）
// ==============================================================================
namespace detail {

// 每个线程独立的特征向量缓冲
[[nodiscard]] FeatureVector& thread_local_buffer() noexcept;

}  // namespace detail

}  // namespace ai
}  // namespace quant

#endif  // QUANT_AI_CORE_FEATURE_ENGINE_HPP
