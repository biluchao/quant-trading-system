// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 逃顶/逃底检测
// ==============================================================================
// @file    src/strategy/strategy.core.escape_detector.hpp
// @module  strategy
// @type    core
// @name    escape_detector
// @version 1.0.1
// @brief   趋势强度自适应逃顶/逃底检测，多条件加权，二次逃顶累积
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 趋势强度自适应：弱 2.5 / 中 4.0 / 强 6.0 倍 ATR
//   - 多条件加权：5 个维度，加权分数 ≥ 阈值才触发
//   - 方向对称：多空严格对称
//   - 二次逃顶：反弹创新高时累积计数
//   - 冷却期：防止频繁触发
//   - 与 MTF 集成：5m 阻力/支撑参与判定
//   - 完整追踪：历史 + 统计 + 快照
//   - 参数可配置：所有阈值从 ConfigManager 注入
//
// 触发条件（加权评分 ≥ kEscapeScoreThreshold）:
//   1. 价格偏离 EMA26 ≥ 动态阈值（趋势强度分级）  → 权重 0.35
//   2. RSI 背离（价格新高但 RSI 未新高）           → 权重 0.25
//   3. MACD 柱连续缩短                             → 权重 0.20
//   4. 最近 N 根未创新高/新低                       → 权重 0.15
//   5. 成交量放大但价格未动                         → 权重 0.05
//   + 5m 阻力位阻挡（可选加分）
//
// 二次逃顶:
//   escape_count ≥ 2 → 强烈反转信号
//   escape_count ≥ 3 → 趋势切换
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_ESCAPE_DETECTOR_HPP
#define QUANT_STRATEGY_CORE_ESCAPE_DETECTOR_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
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

// ==============================================================================
// 前向声明
// ==============================================================================
namespace quant::strategy {
    struct MTFContext;
    enum class TrendGrade : std::uint8_t;
}

namespace quant::strategy {

// ==============================================================================
// 趋势等级（供逃顶距离分级用）
// ==============================================================================
enum class TrendGrade : std::uint8_t {
    Weak   = 0,   // 弱趋势（< 0.4）
    Medium = 1,   // 中趋势（0.4 ~ 0.7）
    Strong = 2,   // 强趋势（> 0.7）
};

[[nodiscard]] constexpr std::string_view to_string(TrendGrade g) noexcept {
    switch (g) {
        case TrendGrade::Weak:   return "Weak";
        case TrendGrade::Medium: return "Medium";
        case TrendGrade::Strong: return "Strong";
    }
    return "Unknown";
}

[[nodiscard]] constexpr TrendGrade grade_from_strength(
    double strength) noexcept
{
    if (strength > 0.7) return TrendGrade::Strong;
    if (strength > 0.4) return TrendGrade::Medium;
    return TrendGrade::Weak;
}

// ==============================================================================
// 常量
// ==============================================================================
namespace escape_config {

// 趋势强度 → 逃顶距离（ATR 倍数）
inline constexpr double kEscapeAtrWeak     = 2.5;
inline constexpr double kEscapeAtrMedium   = 4.0;
inline constexpr double kEscapeAtrStrong   = 6.0;

// 触发阈值（加权评分）
inline constexpr double kEscapeScoreThreshold = 0.60;

// 各条件权重
inline constexpr double kWeightDistance   = 0.35;
inline constexpr double kWeightRsiDivergence = 0.25;
inline constexpr double kWeightMacdShrink = 0.20;
inline constexpr double kWeightNoNewHigh  = 0.15;
inline constexpr double kWeightVolumeSpike = 0.05;

// RSI 背离检测周期
inline constexpr int kRsiDivergenceLookback = 5;

// MACD 缩短确认周期
inline constexpr int kMacdShrinkBars = 3;

// 未创新高/新低确认周期
inline constexpr int kNoNewExtremeBars = 5;

// 成交量放大倍数
inline constexpr double kVolumeSpikeRatio = 2.0;

// 冷却期（秒）
inline constexpr std::int64_t kMinEscapeIntervalSec = 180;   // 3 分钟

// 逃顶历史窗口
inline constexpr std::size_t kMaxEscapeHistory = 16;

// 二次逃顶需要的最小间隔（秒）
inline constexpr std::int64_t kSecondaryEscapeMinIntervalSec = 60;

// 浮点保护
inline constexpr double kEpsilon = 1e-9;

}  // namespace escape_config

// ==============================================================================
// 逃顶严重程度
// ==============================================================================
enum class EscapeSeverity : std::uint8_t {
    None       = 0,   // 未触发
    Weak       = 1,   // 弱（首次）
    Medium     = 2,   // 中（二次）
    Strong     = 3,   // 强（三次及以上）
};

[[nodiscard]] constexpr std::string_view to_string(
    EscapeSeverity s) noexcept
{
    switch (s) {
        case EscapeSeverity::None:   return "None";
        case EscapeSeverity::Weak:   return "Weak";
        case EscapeSeverity::Medium: return "Medium";
        case EscapeSeverity::Strong: return "Strong";
    }
    return "Unknown";
}

// ==============================================================================
// 逃顶参数
// ==============================================================================
struct EscapeParams {
    // 距离分级
    double escape_atr_weak{escape_config::kEscapeAtrWeak};
    double escape_atr_medium{escape_config::kEscapeAtrMedium};
    double escape_atr_strong{escape_config::kEscapeAtrStrong};

    // 触发阈值
    double score_threshold{escape_config::kEscapeScoreThreshold};

    // 权重
    double weight_distance{escape_config::kWeightDistance};
    double weight_rsi_divergence{escape_config::kWeightRsiDivergence};
    double weight_macd_shrink{escape_config::kWeightMacdShrink};
    double weight_no_new_high{escape_config::kWeightNoNewHigh};
    double weight_volume_spike{escape_config::kWeightVolumeSpike};

    // 检测周期
    int rsi_divergence_lookback{escape_config::kRsiDivergenceLookback};
    int macd_shrink_bars{escape_config::kMacdShrinkBars};
    int no_new_extreme_bars{escape_config::kNoNewExtremeBars};
    double volume_spike_ratio{escape_config::kVolumeSpikeRatio};

    // 冷却
    std::int64_t min_escape_interval_sec{
        escape_config::kMinEscapeIntervalSec};
    std::int64_t secondary_escape_min_interval_sec{
        escape_config::kSecondaryEscapeMinIntervalSec};

    // 是否启用各条件
    bool enable_distance{true};
    bool enable_rsi_divergence{true};
    bool enable_macd_shrink{true};
    bool enable_no_new_high{true};
    bool enable_volume_spike{true};

    [[nodiscard]] Result<void> validate() const noexcept;

    // 根据趋势强度获取逃顶距离
    [[nodiscard]] double atr_multiplier(TrendGrade g) const noexcept;
};

// ==============================================================================
// 单个条件检测结果
// ==============================================================================
struct EscapeCondition {
    std::string_view name;
    bool satisfied{false};
    double weight{0.0};
    double contribution{0.0};    // satisfied ? weight : 0
    std::string detail;          // 详细说明
};

// ==============================================================================
// 逃顶信号
// ==============================================================================
struct EscapeSignal {
    // 是否触发
    bool triggered{false};
    EscapeSeverity severity{EscapeSeverity::None};

    // 方向
    Side side{Side::UNKNOWN};

    // 逃顶价格（多头用最高价，空头用最低价）
    Price escape_price{};

    // 触发时间
    Timestamp triggered_at{};

    // 加权总分
    double score{0.0};
    double threshold{0.0};

    // 各条件
    std::vector<EscapeCondition> conditions;

    // 5m 上下文
    double dist_to_5m_resistance_atr{0.0};
    double dist_to_5m_support_atr{0.0};
    bool blocked_by_5m{false};

    // 逃顶累积
    int escape_sequence{0};      // 第几次逃顶（1, 2, 3...）

    // 原因（人类可读）
    std::string reason;

    // 趋势等级（用于判断）
    TrendGrade trend_grade{TrendGrade::Medium};

    [[nodiscard]] bool is_strong() const noexcept {
        return severity == EscapeSeverity::Strong;
    }

    [[nodiscard]] bool is_actionable() const noexcept {
        return triggered && severity != EscapeSeverity::None;
    }

    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 逃顶历史记录
// ==============================================================================
struct EscapeHistoryRecord {
    Timestamp timestamp{};
    Price escape_price{};
    Side side{Side::UNKNOWN};
    double score{0.0};
    int sequence{0};
    std::string reason;
};

// ==============================================================================
// 逃顶快照（持久化）
// ==============================================================================
struct EscapeSnapshot {
    std::uint32_t schema_version{1};
    Timestamp snapshot_time{};

    struct Entry {
        std::int64_t timestamp_us{0};
        std::int64_t escape_price_raw{0};
        std::uint8_t side{0};
        double score{0.0};
        int sequence{0};
        std::string reason;
    };
    std::vector<Entry> history;

    int escape_count{0};
    std::int64_t last_escape_time_us{0};

    [[nodiscard]] bool is_valid() const noexcept;
};

// ==============================================================================
// 逃顶统计
// ==============================================================================
struct EscapeStats {
    std::atomic<std::uint64_t> detect_count{0};
    std::atomic<std::uint64_t> triggered_count{0};
    std::atomic<std::uint64_t> weak_count{0};
    std::atomic<std::uint64_t> medium_count{0};
    std::atomic<std::uint64_t> strong_count{0};
    std::atomic<std::uint64_t> cooldown_blocked_count{0};
    std::atomic<std::uint64_t> condition_hits{0};       // 各条件命中总数
    std::atomic<std::uint64_t> distance_hits{0};
    std::atomic<std::uint64_t> rsi_div_hits{0};
    std::atomic<std::uint64_t> macd_shrink_hits{0};
    std::atomic<std::uint64_t> no_new_high_hits{0};
    std::atomic<std::uint64_t> volume_spike_hits{0};

    void reset() noexcept {
        detect_count.store(0, std::memory_order_relaxed);
        triggered_count.store(0, std::memory_order_relaxed);
        weak_count.store(0, std::memory_order_relaxed);
        medium_count.store(0, std::memory_order_relaxed);
        strong_count.store(0, std::memory_order_relaxed);
        cooldown_blocked_count.store(0, std::memory_order_relaxed);
        condition_hits.store(0, std::memory_order_relaxed);
        distance_hits.store(0, std::memory_order_relaxed);
        rsi_div_hits.store(0, std::memory_order_relaxed);
        macd_shrink_hits.store(0, std::memory_order_relaxed);
        no_new_high_hits.store(0, std::memory_order_relaxed);
        volume_spike_hits.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// 逃顶检测输入上下文
// ==============================================================================
struct EscapeContext {
    // 持仓
    const Position* position{nullptr};

    // 当前 K 线
    Candle candle;

    // 指标
    IndicatorResult indicators;

    // 带宽
    BandResult band;

    // 趋势强度（0~1）
    double trend_strength{0.0};

    // 最近 N 根 K 线（用于创新高检测）
    const std::deque<Candle>* recent_candles{nullptr};

    // MTF 上下文（可选）
    const MTFContext* mtf{nullptr};

    [[nodiscard]] bool is_valid() const noexcept;
};

// ==============================================================================
// 逃顶检测器
// ==============================================================================
class EscapeDetector {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 参数提供者（必需）
        std::function<EscapeParams()> params_provider;

        // 逃顶触发回调（可选）
        std::function<void(const EscapeSignal&)> on_escape;

        // 状态变更回调（可选，用于持久化）
        std::function<void(const EscapeSnapshot&)> on_state_change;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit EscapeDetector(Dependencies deps);
    ~EscapeDetector();

    EscapeDetector(const EscapeDetector&) = delete;
    EscapeDetector& operator=(const EscapeDetector&) = delete;
    EscapeDetector(EscapeDetector&&) noexcept;
    EscapeDetector& operator=(EscapeDetector&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 核心检测
    // -------------------------------------------------------------------------
    // 检测逃顶信号
    [[nodiscard]] Result<EscapeSignal> detect(const EscapeContext& ctx);

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] int escape_count() const noexcept;
    [[nodiscard]] std::optional<Timestamp> last_escape_time() const noexcept;
    [[nodiscard]] std::vector<EscapeHistoryRecord> history() const;
    [[nodiscard]] std::vector<Price> recent_escape_prices() const;

    // -------------------------------------------------------------------------
    // 手动控制
    // -------------------------------------------------------------------------
    // 清空逃顶历史（重置累积）
    void clear_history() noexcept;

    // 清空冷却（允许立即触发）
    void clear_cooldown() noexcept;

    // -------------------------------------------------------------------------
    // 持久化
    // -------------------------------------------------------------------------
    [[nodiscard]] EscapeSnapshot to_snapshot() const;
    Result<void> from_snapshot(const EscapeSnapshot& snapshot);

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const EscapeStats& stats() const noexcept;
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

// 检测 RSI 背离
[[nodiscard]] bool detect_rsi_divergence(
    const std::deque<Candle>& candles,
    double current_rsi,
    Side side,
    int lookback) noexcept;

// 检测 MACD 柱连续缩短
[[nodiscard]] bool detect_macd_shrinking(
    const IndicatorResult& ind,
    int bars) noexcept;

// 检测最近 N 根未创新高/新低
[[nodiscard]] bool detect_no_new_extreme(
    const std::deque<Candle>& candles,
    Side side,
    int bars) noexcept;

// 检测放量滞涨
[[nodiscard]] bool detect_volume_spike_no_move(
    const Candle& candle,
    double volume_ma20,
    double atr,
    double spike_ratio) noexcept;

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_ESCAPE_DETECTOR_HPP
