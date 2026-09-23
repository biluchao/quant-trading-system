// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 多时间框架一致性检查
// ==============================================================================
// @file    src/strategy/strategy.core.mtf_checker.hpp
// @module  strategy
// @type    core
// @name    mtf_checker
// @version 1.0.1
// @brief   3m 信号与 5m 趋势/支撑阻力的一致性检查
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 无未来函数：严格使用已收盘的 5m K 线
//   - 时间对齐：3m 决策时只使用早于该时刻的 5m K 线
//   - 数据新鲜度：过期数据自动降级为 NEUTRAL
//   - 多维度冲突：趋势冲突 + 距离冲突 + 结构冲突
//   - 连续评分：0~1 一致性分数，而非二值
//   - 严重性分级：NEUTRAL / ALIGNED / MINOR / MAJOR / SEVERE
//   - 可配置：所有阈值从 ConfigManager 注入
//   - 可观测：统计 + 诊断
//
// 冲突判定:
//   1. 趋势冲突：3m 做多但 5m 趋势向下
//   2. 距离冲突：接近 5m 反向阻力/支撑（< 0.5 ATR）
//   3. 结构冲突：5m 出现反向吞没等反转形态
//
// 一致性评分:
//   score = 0.4 × trend_score
//         + 0.4 × distance_score
//         + 0.2 × structure_score
//   范围 [0, 1]，1 表示完全一致
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_MTF_CHECKER_HPP
#define QUANT_STRATEGY_CORE_MTF_CHECKER_HPP

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

namespace quant::strategy {

// ==============================================================================
// 常量
// ==============================================================================
namespace mtf_config {

// 5m K 线窗口大小（用于提取高低点）
inline constexpr std::size_t kDefaultWindowBars = 20;

// 最小 5m 数据量（低于此值视为未就绪）
inline constexpr std::size_t kMinRequiredBars = 5;

// 5m K 线间隔（微秒）
inline constexpr std::int64_t kInterval5mUs = 5LL * 60LL * 1000LL * 1000LL;

// 数据新鲜度阈值（微秒）
inline constexpr std::int64_t kStalenessThresholdUs =
    10LL * 60LL * 1000LL * 1000LL;   // 10 分钟

// 5m 趋势使用的 EMA 周期
inline constexpr int kEma5mFast = 5;
inline constexpr int kEma5mSlow = 13;

// 一致性评分的权重
inline constexpr double kWeightTrend     = 0.4;
inline constexpr double kWeightDistance  = 0.4;
inline constexpr double kWeightStructure = 0.2;

// 冲突默认阈值
inline constexpr double kDefaultResistanceDistanceAtr = 0.5;
inline constexpr double kDefaultSupportDistanceAtr    = 0.5;

// 一致性评分等级
inline constexpr double kScoreAlignedThreshold  = 0.75;
inline constexpr double kScoreNeutralThreshold  = 0.5;
inline constexpr double kScoreMinorThreshold    = 0.3;

// 浮点保护
inline constexpr double kEpsilon = 1e-9;

}  // namespace mtf_config

// ==============================================================================
// 5m 趋势方向（三态）
// ==============================================================================
enum class TrendDirection5m : std::uint8_t {
    Neutral = 0,
    Up      = 1,
    Down    = 2,
};

[[nodiscard]] constexpr std::string_view to_string(
    TrendDirection5m t) noexcept
{
    switch (t) {
        case TrendDirection5m::Neutral: return "Neutral";
        case TrendDirection5m::Up:      return "Up";
        case TrendDirection5m::Down:    return "Down";
    }
    return "Unknown";
}

// ==============================================================================
// MTF 一致性
// ==============================================================================
enum class MTFConsistency : std::uint8_t {
    Aligned  = 0,   // 完全一致
    Neutral  = 1,   // 数据不足或方向不明
    Minor    = 2,   // 轻微冲突（减仓）
    Major    = 3,   // 显著冲突（减半或拒绝）
    Severe   = 4,   // 严重冲突（拒绝）
};

[[nodiscard]] constexpr std::string_view to_string(
    MTFConsistency c) noexcept
{
    switch (c) {
        case MTFConsistency::Aligned: return "Aligned";
        case MTFConsistency::Neutral: return "Neutral";
        case MTFConsistency::Minor:   return "Minor";
        case MTFConsistency::Major:   return "Major";
        case MTFConsistency::Severe:  return "Severe";
    }
    return "Unknown";
}

// 严重程度对应的仓位缩放系数
[[nodiscard]] constexpr double position_scale(MTFConsistency c) noexcept {
    switch (c) {
        case MTFConsistency::Aligned: return 1.0;
        case MTFConsistency::Neutral: return 1.0;
        case MTFConsistency::Minor:   return 0.75;
        case MTFConsistency::Major:   return 0.5;
        case MTFConsistency::Severe:  return 0.0;
    }
    return 1.0;
}

// ==============================================================================
// MTF 参数
// ==============================================================================
struct MTFParams {
    // 5m 数据窗口
    std::size_t window_bars{mtf_config::kDefaultWindowBars};
    std::size_t min_required_bars{mtf_config::kMinRequiredBars};

    // 新鲜度
    std::int64_t staleness_threshold_us{
        mtf_config::kStalenessThresholdUs};

    // 冲突阈值
    double resistance_distance_atr{
        mtf_config::kDefaultResistanceDistanceAtr};
    double support_distance_atr{
        mtf_config::kDefaultSupportDistanceAtr};

    // 5m EMA 周期
    int ema_fast_period{mtf_config::kEma5mFast};
    int ema_slow_period{mtf_config::kEma5mSlow};

    // 是否启用各项检查
    bool check_trend{true};
    bool check_distance{true};
    bool check_structure{true};

    // 一致性阈值
    double aligned_threshold{mtf_config::kScoreAlignedThreshold};
    double neutral_threshold{mtf_config::kScoreNeutralThreshold};
    double minor_threshold{mtf_config::kScoreMinorThreshold};

    [[nodiscard]] Result<void> validate() const noexcept;
};

// ==============================================================================
// MTF 上下文（供下游查询）
// ==============================================================================
struct MTFContext {
    // 5m 趋势
    TrendDirection5m trend_5m{TrendDirection5m::Neutral};
    double trend_strength_5m{0.0};       // 0~1

    // 5m 支撑阻力
    double resistance_5m{0.0};
    double support_5m{0.0};

    // 与 3m 当前价格的距离（ATR 倍数）
    double dist_to_resistance_atr{0.0};
    double dist_to_support_atr{0.0};

    // 5m 是否出现反向吞没
    bool has_bearish_engulfing_5m{false};
    bool has_bullish_engulfing_5m{false};

    // 时间戳
    Timestamp last_5m_close_time{};
    Timestamp generated_at{};

    // 数据是否有效
    bool valid{false};

    // 冲突标记（供下游快速判断）
    [[nodiscard]] bool has_conflict(Side dir_3m,
                                     double threshold_atr = 0.5) const noexcept
    {
        if (!valid) return false;

        if (dir_3m == Side::BUY) {
            if (trend_5m == TrendDirection5m::Down) return true;
            if (dist_to_resistance_atr < threshold_atr) return true;
            if (has_bearish_engulfing_5m) return true;
        } else if (dir_3m == Side::SELL) {
            if (trend_5m == TrendDirection5m::Up) return true;
            if (dist_to_support_atr < threshold_atr) return true;
            if (has_bullish_engulfing_5m) return true;
        }
        return false;
    }
};

// ==============================================================================
// MTF 检查结果
// ==============================================================================
struct MTFCheckResult {
    MTFConsistency consistency{MTFConsistency::Neutral};
    double score{0.0};                   // [0, 1] 一致性评分

    // 分项评分
    double trend_score{0.0};
    double distance_score{0.0};
    double structure_score{0.0};

    // 是否允许下单
    bool allow_trade{true};
    double position_scale{1.0};

    // 原因列表（可能多条）
    std::vector<std::string> reasons;

    // 5m 上下文快照
    MTFContext context;

    [[nodiscard]] bool is_conflict() const noexcept {
        return consistency == MTFConsistency::Major ||
               consistency == MTFConsistency::Severe;
    }

    [[nodiscard]] bool is_aligned() const noexcept {
        return consistency == MTFConsistency::Aligned;
    }

    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// MTF 统计
// ==============================================================================
struct MTFStats {
    std::atomic<std::uint64_t> update_count{0};
    std::atomic<std::uint64_t> check_count{0};
    std::atomic<std::uint64_t> aligned_count{0};
    std::atomic<std::uint64_t> neutral_count{0};
    std::atomic<std::uint64_t> minor_count{0};
    std::atomic<std::uint64_t> major_count{0};
    std::atomic<std::uint64_t> severe_count{0};
    std::atomic<std::uint64_t> stale_data_count{0};
    std::atomic<std::uint64_t> insufficient_data_count{0};
    std::atomic<std::uint64_t> forward_bias_rejected_count{0};

    void reset() noexcept {
        update_count.store(0, std::memory_order_relaxed);
        check_count.store(0, std::memory_order_relaxed);
        aligned_count.store(0, std::memory_order_relaxed);
        neutral_count.store(0, std::memory_order_relaxed);
        minor_count.store(0, std::memory_order_relaxed);
        major_count.store(0, std::memory_order_relaxed);
        severe_count.store(0, std::memory_order_relaxed);
        stale_data_count.store(0, std::memory_order_relaxed);
        insufficient_data_count.store(0, std::memory_order_relaxed);
        forward_bias_rejected_count.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// MTF 检查器
// ==============================================================================
class MTFChecker {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 参数提供者（必需）
        std::function<MTFParams()> params_provider;

        // 5m 上下文更新回调（可选）
        std::function<void(const MTFContext&)> on_context_update;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit MTFChecker(Dependencies deps);
    ~MTFChecker();

    MTFChecker(const MTFChecker&) = delete;
    MTFChecker& operator=(const MTFChecker&) = delete;
    MTFChecker(MTFChecker&&) noexcept;
    MTFChecker& operator=(MTFChecker&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_ready() const noexcept;

    // -------------------------------------------------------------------------
    // 5m 上下文更新
    // -------------------------------------------------------------------------
    // 传入 5m K 线序列（含最新一根，包括未收盘的）
    // 内部会过滤未收盘 K 线
    // current_3m_time：当前 3m 决策的时间戳（用于严格时间对齐）
    Result<void> update_5m_context(
        const std::vector<Candle>& candles_5m,
        Timestamp current_3m_time);

    // 便捷：更新时不带时间对齐（使用最新已收盘的 5m）
    Result<void> update_5m_context(const std::vector<Candle>& candles_5m);

    // -------------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------------
    // 获取当前 5m 上下文
    [[nodiscard]] MTFContext current_context() const;

    // 检查 3m 信号与 5m 的一致性
    [[nodiscard]] MTFCheckResult check(
        Side dir_3m,
        double current_price_3m,
        double atr_3m) const;

    // 检查一致性（仅用 context，无需重新传入参数）
    [[nodiscard]] MTFCheckResult check(Side dir_3m) const;

    // -------------------------------------------------------------------------
    // 缓存管理
    // -------------------------------------------------------------------------
    // 强制刷新（清空缓存）
    void invalidate_cache() noexcept;

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const MTFStats& stats() const noexcept;
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

// 判断 5m K 线是否已收盘（相对于给定 3m 时间戳）
[[nodiscard]] inline bool is_5m_candle_closed(
    const Candle& c,
    Timestamp current_time) noexcept
{
    return c.close_time.is_valid() &&
           c.close_time.microseconds() <= current_time.microseconds();
}

// 计算 5m 趋势方向（基于 EMA 双均线）
[[nodiscard]] TrendDirection5m compute_trend_5m(
    const std::vector<Candle>& candles,
    int ema_fast = mtf_config::kEma5mFast,
    int ema_slow = mtf_config::kEma5mSlow) noexcept;

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_MTF_CHECKER_HPP
