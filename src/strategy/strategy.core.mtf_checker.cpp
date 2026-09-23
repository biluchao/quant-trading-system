// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 多时间框架一致性检查实现
// ==============================================================================
// @file    src/strategy/strategy.core.mtf_checker.cpp
// @module  strategy
// @type    core
// @name    mtf_checker
// @version 1.0.1
// @brief   3m 与 5m 一致性检查实现，含时间对齐、趋势、距离、形态
//          已修复 30 类运行时问题
//
// 处理流程:
//   1. update_5m_context(candles_5m, current_3m_time)
//      - 过滤未收盘的 5m K 线
//      - 提取窗口内高低点
//      - 计算 5m EMA 双均线趋势
//      - 检测吞没形态
//      - 构造 MTFContext
//
//   2. check(dir_3m, current_price, atr)
//      - 趋势一致性评分
//      - 距离一致性评分
//      - 结构一致性评分
//      - 综合评分 + 严重性分级
//
// 时间对齐原则:
//   current_3m_time = t
//   只使用 close_time <= t 的 5m K 线
//   避免未来函数
// ==============================================================================

#include "strategy/strategy.core.mtf_checker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 匿名命名空间：辅助函数
// ==============================================================================
namespace {

constexpr double kEps = mtf_config::kEpsilon;

// -----------------------------------------------------------------------------
// 安全除法
// -----------------------------------------------------------------------------
[[nodiscard]] inline double safe_div(double a, double b,
                                       double def = 0.0) noexcept {
    if (std::abs(b) < kEps) return def;
    const double r = a / b;
    return std::isfinite(r) ? r : def;
}

// -----------------------------------------------------------------------------
// 限幅
// -----------------------------------------------------------------------------
[[nodiscard]] inline double clamp_val(double v, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, v));
}

// -----------------------------------------------------------------------------
// 有限数检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool finite(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 计算 EMA 序列（返回最后一个 EMA 值）
// -----------------------------------------------------------------------------
[[nodiscard]] double compute_ema_last(const std::vector<double>& prices,
                                        int period) noexcept {
    if (prices.empty() || period <= 0) return 0.0;

    const std::size_t n = prices.size();
    const std::size_t p = static_cast<std::size_t>(period);

    if (n < p) {
        // 数据不足，使用简单平均
        double sum = 0.0;
        std::size_t count = 0;
        for (double v : prices) {
            if (finite(v)) {
                sum += v;
                ++count;
            }
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    }

    const double alpha = 2.0 / (static_cast<double>(p) + 1.0);

    // 初始值用前 p 个的 SMA
    double ema = 0.0;
    std::size_t valid_count = 0;
    for (std::size_t i = 0; i < p; ++i) {
        if (finite(prices[i])) {
            ema += prices[i];
            ++valid_count;
        }
    }
    if (valid_count == 0) return 0.0;
    ema /= static_cast<double>(valid_count);

    // 递推
    for (std::size_t i = p; i < n; ++i) {
        if (!finite(prices[i])) continue;
        ema = alpha * prices[i] + (1.0 - alpha) * ema;
    }

    return finite(ema) ? ema : 0.0;
}

// -----------------------------------------------------------------------------
// 检测看跌吞没
// -----------------------------------------------------------------------------
[[nodiscard]] bool is_bearish_engulfing(const Candle& prev,
                                          const Candle& curr) noexcept {
    if (!prev.is_valid() || !curr.is_valid()) return false;

    const double prev_open = prev.open.to_double();
    const double prev_close = prev.close.to_double();
    const double curr_open = curr.open.to_double();
    const double curr_close = curr.close.to_double();

    // 前一根阳线，当前阴线
    const bool prev_bull = prev_close > prev_open;
    const bool curr_bear = curr_close < curr_open;
    if (!prev_bull || !curr_bear) return false;

    // 当前实体完全覆盖前一根实体
    return curr_open > prev_close &&
           curr_close < prev_open;
}

// -----------------------------------------------------------------------------
// 检测看涨吞没
// -----------------------------------------------------------------------------
[[nodiscard]] bool is_bullish_engulfing(const Candle& prev,
                                          const Candle& curr) noexcept {
    if (!prev.is_valid() || !curr.is_valid()) return false;

    const double prev_open = prev.open.to_double();
    const double prev_close = prev.close.to_double();
    const double curr_open = curr.open.to_double();
    const double curr_close = curr.close.to_double();

    // 前一根阴线，当前阳线
    const bool prev_bear = prev_close < prev_open;
    const bool curr_bull = curr_close > curr_open;
    if (!prev_bear || !curr_bull) return false;

    // 当前实体完全覆盖前一根实体
    return curr_open < prev_close &&
           curr_close > prev_open;
}

// -----------------------------------------------------------------------------
// 5m 趋势方向转字符串
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::string_view side_to_string(Side s) noexcept {
    switch (s) {
        case Side::BUY:  return "BUY";
        case Side::SELL: return "SELL";
        default:         return "UNKNOWN";
    }
}

}  // namespace

// ==============================================================================
// MTFParams::validate
// ==============================================================================
Result<void> MTFParams::validate() const noexcept {
    if (window_bars == 0 || window_bars > 500) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "window_bars 超出范围 [1, 500]")
            .with_context("value", std::to_string(window_bars));
    }

    if (min_required_bars == 0 ||
        min_required_bars > window_bars) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_required_bars 无效或 > window_bars");
    }

    if (staleness_threshold_us <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "staleness_threshold_us 必须 > 0");
    }

    if (resistance_distance_atr < 0.0 ||
        resistance_distance_atr > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "resistance_distance_atr 超出范围 [0, 10]");
    }

    if (support_distance_atr < 0.0 ||
        support_distance_atr > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "support_distance_atr 超出范围 [0, 10]");
    }

    if (ema_fast_period < 2 || ema_fast_period > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ema_fast_period 超出范围 [2, 100]");
    }

    if (ema_slow_period <= ema_fast_period ||
        ema_slow_period > 200) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ema_slow_period 必须 > ema_fast_period 且 ≤ 200");
    }

    if (aligned_threshold <= neutral_threshold ||
        aligned_threshold > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "aligned_threshold 无效");
    }

    if (neutral_threshold <= minor_threshold ||
        neutral_threshold > aligned_threshold) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "neutral_threshold 无效");
    }

    if (minor_threshold < 0.0 || minor_threshold > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "minor_threshold 超出范围 [0, 1]");
    }

    return {};
}

// ==============================================================================
// MTFCheckResult::to_string
// ==============================================================================
std::string MTFCheckResult::to_string() const {
    std::ostringstream oss;
    oss << "MTFCheckResult{consistency="
        << quant::strategy::to_string(consistency)
        << ", score=" << score
        << ", trend=" << trend_score
        << ", distance=" << distance_score
        << ", structure=" << structure_score
        << ", allow=" << (allow_trade ? "yes" : "no")
        << ", scale=" << position_scale;

    if (!reasons.empty()) {
        oss << ", reasons=[";
        for (std::size_t i = 0; i < reasons.size(); ++i) {
            if (i > 0) oss << "; ";
            oss << reasons[i];
        }
        oss << "]";
    }

    oss << "}";
    return oss.str();
}

// ==============================================================================
// compute_trend_5m（辅助函数）
// ==============================================================================
TrendDirection5m compute_trend_5m(
    const std::vector<Candle>& candles,
    int ema_fast,
    int ema_slow) noexcept
{
    if (candles.empty()) return TrendDirection5m::Neutral;

    std::vector<double> closes;
    closes.reserve(candles.size());
    for (const auto& c : candles) {
        if (c.is_valid() && finite(c.close.to_double())) {
            closes.push_back(c.close.to_double());
        }
    }

    if (closes.size() < static_cast<std::size_t>(
            std::max(ema_fast, ema_slow))) {
        return TrendDirection5m::Neutral;
    }

    const double fast = compute_ema_last(closes, ema_fast);
    const double slow = compute_ema_last(closes, ema_slow);

    if (!finite(fast) || !finite(slow)) return TrendDirection5m::Neutral;

    const double diff = fast - slow;
    const double threshold = slow * 0.0005;   // 0.05%

    if (diff > threshold) return TrendDirection5m::Up;
    if (diff < -threshold) return TrendDirection5m::Down;
    return TrendDirection5m::Neutral;
}

// ==============================================================================
// MTFChecker::Impl
// ==============================================================================
struct MTFChecker::Impl {
    // -------------------------------------------------------------------------
    // 依赖
    // -------------------------------------------------------------------------
    Dependencies deps;

    // -------------------------------------------------------------------------
    // 运行状态
    // -------------------------------------------------------------------------
    std::atomic<bool> running{false};
    std::atomic<bool> ready{false};

    // -------------------------------------------------------------------------
    // 5m 上下文
    // -------------------------------------------------------------------------
    MTFContext context;

    // 缓存的 5m K 线（已过滤未收盘）
    std::vector<Candle> cached_5m;

    // 缓存的有效性时间戳（基于 current_3m_time）
    Timestamp last_aligned_time{};

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    MTFStats stats;

    // -------------------------------------------------------------------------
    // 读写锁
    // -------------------------------------------------------------------------
    mutable std::shared_mutex mutex;

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<MTFParams> load_params() const;

    [[nodiscard]] std::vector<Candle> filter_closed_5m(
        const std::vector<Candle>& candles,
        Timestamp current_3m_time) const;

    void build_context(const std::vector<Candle>& closed_5m,
                        const MTFParams& params,
                        Timestamp current_3m_time);

    [[nodiscard]] double compute_trend_score(
        Side dir_3m,
        const MTFContext& ctx) const noexcept;

    [[nodiscard]] double compute_distance_score(
        Side dir_3m,
        const MTFContext& ctx,
        const MTFParams& params) const noexcept;

    [[nodiscard]] double compute_structure_score(
        Side dir_3m,
        const MTFContext& ctx,
        const MTFParams& params) const noexcept;

    [[nodiscard]] MTFConsistency classify(
        double score,
        const MTFContext& ctx,
        Side dir_3m,
        const MTFParams& params,
        std::vector<std::string>& reasons) const;

    void notify_context_update(const MTFContext& ctx) noexcept;

    [[nodiscard]] bool is_context_stale(
        const MTFContext& ctx,
        const MTFParams& params,
        Timestamp now) const noexcept;
};

// ==============================================================================
// Impl::load_params
// ==============================================================================
Result<MTFParams> MTFChecker::Impl::load_params() const {
    if (!deps.params_provider) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "params_provider 未注入");
    }

    MTFParams params;
    try {
        params = deps.params_provider();
    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数加载异常")
            .with_context("error", e.what());
    } catch (...) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数加载未知异常");
    }

    if (auto r = params.validate(); r.is_err()) {
        return r;
    }

    return params;
}

// ==============================================================================
// Impl::filter_closed_5m
// ==============================================================================
std::vector<Candle> MTFChecker::Impl::filter_closed_5m(
    const std::vector<Candle>& candles,
    Timestamp current_3m_time) const
{
    std::vector<Candle> result;
    result.reserve(candles.size());

    for (const auto& c : candles) {
        if (!c.is_valid()) continue;
        if (!c.close_time.is_valid()) continue;

        // 只使用已收盘的 5m K 线
        if (c.close_time.microseconds() <=
            current_3m_time.microseconds()) {
            result.push_back(c);
        }
    }

    // 按时间升序
    std::sort(result.begin(), result.end(),
              [](const Candle& a, const Candle& b) {
                  return a.close_time.microseconds() <
                         b.close_time.microseconds();
              });

    return result;
}

// ==============================================================================
// Impl::build_context
// ==============================================================================
void MTFChecker::Impl::build_context(
    const std::vector<Candle>& closed_5m,
    const MTFParams& params,
    Timestamp current_3m_time)
{
    MTFContext new_ctx;

    if (closed_5m.size() < params.min_required_bars) {
        new_ctx.valid = false;
        new_ctx.generated_at = Timestamp::now();
        context = new_ctx;
        ready.store(false, std::memory_order_release);
        stats.insufficient_data_count.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    // 取最近 window_bars 根
    const std::size_t start_idx = (closed_5m.size() > params.window_bars)
        ? closed_5m.size() - params.window_bars
        : 0;

    const std::size_t n = closed_5m.size();

    // 提取窗口内的高低价
    double max_high = -std::numeric_limits<double>::infinity();
    double min_low  = std::numeric_limits<double>::infinity();

    for (std::size_t i = start_idx; i < n; ++i) {
        const auto& c = closed_5m[i];
        if (!c.is_valid()) continue;

        const double h = c.high.to_double();
        const double l = c.low.to_double();

        if (finite(h) && h > max_high) max_high = h;
        if (finite(l) && l < min_low) min_low = l;
    }

    if (!finite(max_high) || !finite(min_low) || max_high <= 0.0 ||
        min_low <= 0.0) {
        new_ctx.valid = false;
        new_ctx.generated_at = Timestamp::now();
        context = new_ctx;
        ready.store(false, std::memory_order_release);
        stats.insufficient_data_count.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    new_ctx.resistance_5m = max_high;
    new_ctx.support_5m = min_low;

    // 计算 5m EMA 双均线趋势
    std::vector<double> closes;
    closes.reserve(n - start_idx);
    for (std::size_t i = start_idx; i < n; ++i) {
        if (closed_5m[i].is_valid()) {
            closes.push_back(closed_5m[i].close.to_double());
        }
    }

    if (closes.size() >= static_cast<std::size_t>(
            std::max(params.ema_fast_period, params.ema_slow_period))) {
        const double fast = compute_ema_last(closes, params.ema_fast_period);
        const double slow = compute_ema_last(closes, params.ema_slow_period);

        if (finite(fast) && finite(slow) && slow > 0.0) {
            const double diff = fast - slow;
            const double threshold = slow * 0.0005;  // 0.05%

            if (diff > threshold) {
                new_ctx.trend_5m = TrendDirection5m::Up;
            } else if (diff < -threshold) {
                new_ctx.trend_5m = TrendDirection5m::Down;
            } else {
                new_ctx.trend_5m = TrendDirection5m::Neutral;
            }

            // 趋势强度
            const double strength = clamp_val(
                std::abs(diff) / (std::abs(slow) * 0.02 + kEps),
                0.0, 1.0);
            new_ctx.trend_strength_5m = strength;
        }
    }

    // 检测吞没形态（最近 2 根）
    if (n >= 2) {
        const auto& prev = closed_5m[n - 2];
        const auto& curr = closed_5m[n - 1];

        if (prev.is_valid() && curr.is_valid()) {
            new_ctx.has_bearish_engulfing_5m =
                is_bearish_engulfing(prev, curr);
            new_ctx.has_bullish_engulfing_5m =
                is_bullish_engulfing(prev, curr);
        }
    }

    // 时间戳
    new_ctx.last_5m_close_time = closed_5m.back().close_time;
    new_ctx.generated_at = Timestamp::now();
    new_ctx.valid = true;

    context = new_ctx;
    last_aligned_time = current_3m_time;
    ready.store(true, std::memory_order_release);

    stats.update_count.fetch_add(1, std::memory_order_relaxed);

    // 通知订阅者
    notify_context_update(new_ctx);
}

// ==============================================================================
// Impl::notify_context_update
// ==============================================================================
void MTFChecker::Impl::notify_context_update(
    const MTFContext& ctx) noexcept
{
    if (!deps.on_context_update) return;

    try {
        deps.on_context_update(ctx);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_context_update 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_context_update 回调未知异常");
    }
}

// ==============================================================================
// Impl::is_context_stale
// ==============================================================================
bool MTFChecker::Impl::is_context_stale(
    const MTFContext& ctx,
    const MTFParams& params,
    Timestamp now) const noexcept
{
    if (!ctx.valid) return true;
    if (!ctx.last_5m_close_time.is_valid()) return true;

    const auto age_us = now.microseconds() -
                         ctx.last_5m_close_time.microseconds();
    return age_us > params.staleness_threshold_us;
}

// ==============================================================================
// Impl::compute_trend_score
// ==============================================================================
double MTFChecker::Impl::compute_trend_score(
    Side dir_3m,
    const MTFContext& ctx) const noexcept
{
    // 5m 中性 → 0.7（不完全信任）
    if (ctx.trend_5m == TrendDirection5m::Neutral) {
        return 0.7;
    }

    if (dir_3m == Side::BUY) {
        return (ctx.trend_5m == TrendDirection5m::Up) ? 1.0 : 0.0;
    }
    if (dir_3m == Side::SELL) {
        return (ctx.trend_5m == TrendDirection5m::Down) ? 1.0 : 0.0;
    }

    return 0.5;  // UNKNOWN
}

// ==============================================================================
// Impl::compute_distance_score
// ==============================================================================
double MTFChecker::Impl::compute_distance_score(
    Side dir_3m,
    const MTFContext& ctx,
    const MTFParams& params) const noexcept
{
    double distance_atr = 0.0;
    double threshold = 0.5;

    if (dir_3m == Side::BUY) {
        distance_atr = ctx.dist_to_resistance_atr;
        threshold = params.resistance_distance_atr;
    } else if (dir_3m == Side::SELL) {
        distance_atr = ctx.dist_to_support_atr;
        threshold = params.support_distance_atr;
    } else {
        return 0.5;
    }

    if (threshold < kEps) return 1.0;

    // 距离越远越好
    // distance >= 2×threshold → 1.0
    // distance == threshold → 0.5
    // distance == 0 → 0.0
    const double ratio = distance_atr / threshold;

    if (ratio >= 2.0) return 1.0;
    if (ratio <= 0.0) return 0.0;

    return clamp_val(ratio / 2.0, 0.0, 1.0);
}

// ==============================================================================
// Impl::compute_structure_score
// ==============================================================================
double MTFChecker::Impl::compute_structure_score(
    Side dir_3m,
    const MTFContext& ctx,
    const MTFParams& params) const noexcept
{
    if (!params.check_structure) return 1.0;

    if (dir_3m == Side::BUY) {
        // 做多时，5m 出现看跌吞没 → 结构冲突
        if (ctx.has_bearish_engulfing_5m) return 0.0;
        return 1.0;
    }
    if (dir_3m == Side::SELL) {
        // 做空时，5m 出现看涨吞没 → 结构冲突
        if (ctx.has_bullish_engulfing_5m) return 0.0;
        return 1.0;
    }

    return 0.5;
}

// ==============================================================================
// Impl::classify
// ==============================================================================
MTFConsistency MTFChecker::Impl::classify(
    double score,
    const MTFContext& ctx,
    Side dir_3m,
    const MTFParams& params,
    std::vector<std::string>& reasons) const
{
    // 遍历所有冲突项，收集原因
    bool has_trend_conflict = false;
    bool has_distance_conflict = false;
    bool has_structure_conflict = false;

    if (params.check_trend) {
        if (dir_3m == Side::BUY &&
            ctx.trend_5m == TrendDirection5m::Down) {
            has_trend_conflict = true;
            reasons.emplace_back("5m趋势向下，与做多冲突");
        }
        if (dir_3m == Side::SELL &&
            ctx.trend_5m == TrendDirection5m::Up) {
            has_trend_conflict = true;
            reasons.emplace_back("5m趋势向上，与做空冲突");
        }
    }

    if (params.check_distance) {
        if (dir_3m == Side::BUY &&
            ctx.dist_to_resistance_atr <
                params.resistance_distance_atr) {
            has_distance_conflict = true;
            reasons.emplace_back(
                "接近5m阻力位，距离 " +
                std::to_string(ctx.dist_to_resistance_atr) + " ATR");
        }
        if (dir_3m == Side::SELL &&
            ctx.dist_to_support_atr <
                params.support_distance_atr) {
            has_distance_conflict = true;
            reasons.emplace_back(
                "接近5m支撑位，距离 " +
                std::to_string(ctx.dist_to_support_atr) + " ATR");
        }
    }

    if (params.check_structure) {
        if (dir_3m == Side::BUY &&
            ctx.has_bearish_engulfing_5m) {
            has_structure_conflict = true;
            reasons.emplace_back("5m出现看跌吞没形态");
        }
        if (dir
