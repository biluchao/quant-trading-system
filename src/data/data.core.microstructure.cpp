// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 微观结构指标实现
// ==============================================================================
// @file    src/data/data.core.microstructure.cpp
// @module  data
// @type    core
// @name    microstructure
// @version 1.0.1
// @brief   Microstructure 的完整实现
//          已修复 30 类运行时问题
//
// 计算流程:
//   1. 输入校验（depth / trade）
//   2. 计算订单簿指标（OBI / 微价格 / 斜率 / 分桶）
//   3. 计算成交指标（VWAP / 失衡 / 大单）
//   4. 计算成交量分布（POC / Value Area）
//   5. 计算流动性与价格冲击
//   6. 汇总快照 + 更新统计
// ==============================================================================

#include "data/data.core.microstructure.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace quant {
namespace data {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

constexpr std::size_t kMaxSymbolLength = 32;

// -----------------------------------------------------------------------------
// 校验 symbol
// -----------------------------------------------------------------------------
[[nodiscard]] Result<std::string> validate_symbol(std::string_view symbol) {
    if (symbol.empty()) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "symbol")
            .with_context("reason", "为空");
    }
    if (symbol.size() > kMaxSymbolLength) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "symbol")
            .with_context("reason", "过长");
    }
    for (char c : symbol) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
                .with_context("field", "symbol")
                .with_context("invalid_char", std::string(1, c));
        }
    }
    return std::string{symbol};
}

// -----------------------------------------------------------------------------
// 校验配置
// -----------------------------------------------------------------------------
[[nodiscard]] Result<void> validate_config(
    const MicrostructureConfig& c)
{
    if (c.obi_levels == 0 || c.obi_levels > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "obi_levels 必须在 [1, 100]");
    }
    if (c.obi_decay < 0.0 || c.obi_decay > 10.0 ||
        !std::isfinite(c.obi_decay)) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "obi_decay 必须在 [0, 10] 且有限");
    }
    if (c.ofi_window == 0 || c.ofi_window > 1000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ofi_window 越界");
    }
    if (c.depth_buckets == 0 || c.depth_buckets > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "depth_buckets 越界");
    }
    if (c.depth_range_pct <= 0.0 || c.depth_range_pct >= 0.5) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "depth_range_pct 必须在 (0, 0.5)");
    }
    if (c.depth_decay < 0.0 || c.depth_decay > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "depth_decay 越界");
    }
    if (c.vwap_short_window == 0 ||
        c.vwap_long_window == 0 ||
        c.vwap_long_window < c.vwap_short_window) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "VWAP 窗口配置非法");
    }
    if (c.trade_window == 0 || c.trade_window > 100'000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "trade_window 越界");
    }
    if (c.vp_buckets == 0 || c.vp_buckets > 500) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "vp_buckets 越界");
    }
    if (c.vp_window == 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "vp_window 必须 > 0");
    }
    if (c.large_trade_multiple <= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "large_trade_multiple 必须 > 1");
    }
    if (c.target_notional <= 0.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "target_notional 必须 > 0");
    }
    if (c.max_window_size == 0 || c.max_window_size > 1'000'000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_window_size 越界");
    }
    return {};
}

// -----------------------------------------------------------------------------
// 有限性检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_finite(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 计算中位数（不修改原数组的拷贝）
// -----------------------------------------------------------------------------
[[nodiscard]] double compute_median_copy(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const auto n = values.size();
    const auto mid = n / 2;

    std::nth_element(values.begin(), values.begin() + mid, values.end());
    const double a = values[mid];

    if ((n & 1) == 1) return a;

    // 偶数个：取中间两个的平均
    const auto max_lower = std::max_element(
        values.begin(), values.begin() + mid);
    return (a + *max_lower) / 2.0;
}

// -----------------------------------------------------------------------------
// 时间戳单调性检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_monotonic(
    const std::deque<MicroTrade>& window,
    const MicroTrade& trade) noexcept
{
    if (window.empty()) return true;
    return trade.timestamp >= window.back().timestamp;
}

// -----------------------------------------------------------------------------
// 求 depth levels 的总量
// -----------------------------------------------------------------------------
[[nodiscard]] double sum_quantity(
    const std::vector<DepthLevel>& levels,
    std::size_t limit = std::numeric_limits<std::size_t>::max()) noexcept
{
    double sum = 0.0;
    const auto n = std::min(levels.size(), limit);
    for (std::size_t i = 0; i < n; ++i) {
        const double q = levels[i].quantity;
        if (is_finite(q) && q > 0.0) sum += q;
    }
    return sum;
}

}  // namespace

// ==============================================================================
// 构造函数
// ==============================================================================
Microstructure::Microstructure(MicrostructureConfig config)
    : config_{std::move(config)}
{
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[Microstructure] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = MicrostructureConfig{};
    }

    // 初始化窗口
    vwap_short_value_ = 0.0;
    vwap_long_value_ = 0.0;
}

Microstructure::Microstructure(std::string_view symbol,
                                 MicrostructureConfig config)
    : config_{std::move(config)}
{
    // 校验 symbol
    if (auto r = validate_symbol(symbol); r.is_ok()) {
        symbol_ = std::move(r.value());
    } else {
        QUANT_LOG_WARN("[Microstructure] symbol 非法: {}，使用 UNKNOWN",
            r.error().to_string());
        symbol_ = "UNKNOWN";
    }

    // 校验配置
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[Microstructure] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = MicrostructureConfig{};
    }

    vwap_short_value_ = 0.0;
    vwap_long_value_ = 0.0;
}

// ==============================================================================
// 纯函数：OBI 计算
// ==============================================================================
double Microstructure::compute_obi(
    const std::vector<DepthLevel>& bids,
    const std::vector<DepthLevel>& asks,
    std::size_t levels,
    double decay) noexcept
{
    if (bids.empty() || asks.empty()) return 0.0;

    const auto n_bids = std::min(levels, bids.size());
    const auto n_asks = std::min(levels, asks.size());

    // 若任一为空，返回 0
    if (n_bids == 0 || n_asks == 0) return 0.0;

    // clamp decay
    const double d = std::clamp(decay, 0.0, 10.0);

    double bid_weighted = 0.0;
    double ask_weighted = 0.0;
    double bid_weight_total = 0.0;
    double ask_weight_total = 0.0;

    for (std::size_t i = 0; i < n_bids; ++i) {
        const double q = bids[i].quantity;
        if (!is_finite(q) || q <= 0.0) continue;

        const double w = std::exp(-d * static_cast<double>(i));
        if (!is_finite(w) || w <= 0.0) continue;

        bid_weighted += q * w;
        bid_weight_total += w;
    }

    for (std::size_t i = 0; i < n_asks; ++i) {
        const double q = asks[i].quantity;
        if (!is_finite(q) || q <= 0.0) continue;

        const double w = std::exp(-d * static_cast<double>(i));
        if (!is_finite(w) || w <= 0.0) continue;

        ask_weighted += q * w;
        ask_weight_total += w;
    }

    const double total = bid_weighted + ask_weighted;
    if (std::abs(total) < microstructure_config::kEpsilon) {
        return 0.0;
    }

    const double obi = (bid_weighted - ask_weighted) / total;
    if (!is_finite(obi)) return 0.0;

    return std::clamp(obi, -1.0, 1.0);
}

double Microstructure::compute_obi_simple(
    const std::vector<DepthLevel>& bids,
    const std::vector<DepthLevel>& asks,
    std::size_t levels) noexcept
{
    if (bids.empty() || asks.empty()) return 0.0;

    const double bid_sum = sum_quantity(bids, levels);
    const double ask_sum = sum_quantity(asks, levels);

    const double total = bid_sum + ask_sum;
    if (std::abs(total) < microstructure_config::kEpsilon) {
        return 0.0;
    }

    const double obi = (bid_sum - ask_sum) / total;
    return std::isfinite(obi) ? std::clamp(obi, -1.0, 1.0) : 0.0;
}

double Microstructure::compute_obi_top1(
    const std::vector<DepthLevel>& bids,
    const std::vector<DepthLevel>& asks) noexcept
{
    if (bids.empty() || asks.empty()) return 0.0;

    const double bid_qty = bids.front().quantity;
    const double ask_qty = asks.front().quantity;

    if (!is_finite(bid_qty) || !is_finite(ask_qty)) return 0.0;

    const double total = bid_qty + ask_qty;
    if (std::abs(total) < microstructure_config::kEpsilon) {
        return 0.0;
    }

    const double obi = (bid_qty - ask_qty) / total;
    return std::isfinite(obi) ? std::clamp(obi, -1.0, 1.0) : 0.0;
}

// ==============================================================================
// 纯函数：微价格
// ==============================================================================
double Microstructure::compute_micro_price(
    const std::vector<DepthLevel>& bids,
    const std::vector<DepthLevel>& asks) noexcept
{
    if (bids.empty() || asks.empty()) return 0.0;

    const double best_bid = bids.front().price;
    const double best_ask = asks.front().price;
    const double bid_qty = bids.front().quantity;
    const double ask_qty = asks.front().quantity;

    if (!is_finite(best_bid) || !is_finite(best_ask) ||
        !is_finite(bid_qty) || !is_finite(ask_qty)) {
        return 0.0;
    }

    if (best_bid <= 0.0 || best_ask <= 0.0) return 0.0;

    const double total = bid_qty + ask_qty;
    if (std::abs(total) < microstructure_config::kEpsilon) {
        // 无挂单，返回中点价
        return (best_bid + best_ask) / 2.0;
    }

    // micro_price = (bid × ask_qty + ask × bid_qty) / (bid_qty + ask_qty)
    const double micro = (best_bid * ask_qty + best_ask * bid_qty) / total;
    return std::isfinite(micro) ? micro : (best_bid + best_ask) / 2.0;
}

// ==============================================================================
// 纯函数：订单簿斜率
// ==============================================================================
double Microstructure::compute_book_slope(
    const std::vector<DepthLevel>& levels,
    double mid_price,
    bool is_bid) noexcept
{
    if (levels.empty() || !is_finite(mid_price) || mid_price <= 0.0) {
        return 0.0;
    }

    double qty_sum = 0.0;
    double dist_sum = 0.0;

    for (const auto& level : levels) {
        const double p = level.price;
        const double q = level.quantity;

        if (!is_finite(p) || !is_finite(q) || q <= 0.0) continue;

        const double dist = std::abs(p - mid_price);
        if (dist < microstructure_config::kEpsilon) continue;

        qty_sum += q;
        dist_sum += dist;
    }

    if (dist_sum < microstructure_config::kEpsilon) return 0.0;

    // slope = 总挂单量 / 平均距离
    const double avg_dist = dist_sum / static_cast<double>(
        std::max<std::size_t>(1, levels.size()));
    const double slope = qty_sum / std::max(avg_dist, microstructure_config::kEpsilon);

    return std::isfinite(slope) ? slope : 0.0;
}

// ==============================================================================
// 纯函数：深度分桶
// ==============================================================================
std::vector<DepthBucket> Microstructure::compute_depth_buckets(
    const std::vector<DepthLevel>& bids,
    const std::vector<DepthLevel>& asks,
    double mid_price,
    std::size_t bucket_count,
    double range_pct) noexcept
{
    std::vector<DepthBucket> result;

    if (!is_finite(mid_price) || mid_price <= 0.0) return result;
    if (bucket_count == 0 || range_pct <= 0.0) return result;

    result.resize(bucket_count);

    const double range = mid_price * range_pct;
    const double bucket_size = 2.0 * range / static_cast<double>(bucket_count);

    if (bucket_size <= 0.0) return result;

    // 初始化桶边界
    const double base = mid_price - range;
    for (std::size_t i = 0; i < bucket_count; ++i) {
        result[i].price_low = base + static_cast<double>(i) * bucket_size;
        result[i].price_high = base + static_cast<double>(i + 1) * bucket_size;
    }

    // 分配买盘
    for (const auto& b : bids) {
        const double p = b.price;
        const double q = b.quantity;
        if (!is_finite(p) || !is_finite(q) || q <= 0.0) continue;
        if (p < base || p > mid_price + range) continue;

        const auto idx = static_cast<std::size_t>(
            (p - base) / bucket_size);
        if (idx >= bucket_count) continue;

        result[idx].bid_qty += q;
    }

    // 分配卖盘
    for (const auto& a : asks) {
        const double p = a.price;
        const double q = a.quantity;
        if (!is_finite(p) || !is_finite(q) || q <= 0.0) continue;
        if (p < base || p > mid_price + range) continue;

        const auto idx = static_cast<std::size_t>(
            (p - base) / bucket_size);
        if (idx >= bucket_count) continue;

        result[idx].ask_qty += q;
    }

    return result;
}

// ==============================================================================
// 纯函数：流动性评分
// ==============================================================================
double Microstructure::compute_liquidity_score(
    const CleanedDepth& depth,
    double target_notional) noexcept
{
    if (!depth.is_valid() || target_notional <= 0.0) return 0.0;

    // 1. 深度得分：总名义价值 / 目标
    double total_notional = 0.0;
    for (const auto& b : depth.bids) {
        total_notional += b.notional();
    }
    for (const auto& a : depth.asks) {
        total_notional += a.notional();
    }

    const double depth_score = std::min(
        total_notional / (target_notional * 10.0), 1.0);

    // 2. 价差得分：spread_pct 越小越好
    const double spread_pct = depth.mid_price > 0.0
        ? depth.spread_pct : 1.0;
    const double spread_score = std::max(0.0,
        1.0 - spread_pct / 0.001);   // 0.1% 为满分线

    // 3. 平衡得分：买卖盘对称性
    const double bid_qty = depth.total_bid_qty;
    const double ask_qty = depth.total_ask_qty;
    const double balance_total = bid_qty + ask_qty;
    const double balance_score = balance_total > 0.0
        ? 1.0 - std::abs(bid_qty - ask_qty) / balance_total
        : 0.0;

    // 加权
    const double score =
        0.5 * depth_score +
        0.3 * spread_score +
        0.2 * balance_score;

    return std::clamp(score, 0.0, 1.0);
}

// ==============================================================================
// 纯函数：价格冲击估算
// ==============================================================================
double Microstructure::estimate_price_impact_bps(
    const CleanedDepth& depth,
    double target_notional) noexcept
{
    if (!depth.is_valid() || target_notional <= 0.0) return 0.0;
    if (depth.bids.empty() || depth.asks.empty()) return 0.0;

    // 假设买入，消耗 asks
    const double best_ask = depth.asks.front().price;
    if (!is_finite(best_ask) || best_ask <= 0.0) return 0.0;

    double remaining = target_notional;
    double weighted_price_impact = 0.0;
    double total_consumed = 0.0;

    for (const auto& level : depth.asks) {
        if (remaining <= 0.0) break;

        const double p = level.price;
        const double q = level.quantity;
        if (!is_finite(p) || !is_finite(q) || q <= 0.0) continue;

        const double level_notional = p * q;
        const double consume = std::min(remaining, level_notional);

        // 该档位的冲击（bp）
        const double impact_bps = (p - best_ask) / best_ask * 10000.0;
        weighted_price_impact += impact_bps * consume;
        total_consumed += consume;

        remaining -= consume;
    }

    if (total_consumed < microstructure_config::kEpsilon) {
        return 0.0;
    }

    const double avg_impact = weighted_price_impact / total_consumed;
    return std::isfinite(avg_impact) ? std::max(0.0, avg_impact) : 0.0;
}

// ==============================================================================
// 数据输入：订单簿
// ==============================================================================
Result<void> Microstructure::update_depth(const CleanedDepth& depth) {
    // 1. 有效性校验
    if (!depth.is_valid()) {
        stats_.invalid_snapshots.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "CleanedDepth 无效")
            .with_context("reason", "is_valid() == false");
    }

    if (depth.has_crossed_book()) {
        stats_.crossed_books.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "买卖盘交叉")
            .with_context("best_bid", std::to_string(depth.bids.front().price))
            .with_context("best_ask", std::to_string(depth.asks.front().price));
    }

    // 2. 更新 OFI
    update_ofi(depth);

    return {};
}

// ==============================================================================
// 数据输入：成交
// ==============================================================================
Result<void> Microstructure::update_trade(const MicroTrade& trade) {
    // 1. 有效性校验
    if (!trade.is_valid()) {
        stats_.trades_rejected.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "MicroTrade 无效")
            .with_context("price", std::to_string(trade.price))
            .with_context("quantity", std::to_string(trade.quantity));
    }

    std::lock_guard lock(trade_mutex_);

    // 2. 时间戳单调性校验
    if (!is_monotonic(trade_window_, trade)) {
        stats_.trades_rejected.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "成交时间戳乱序")
            .with_context("reason", "时间戳倒退");
    }

    // 3. 加入窗口
    trade_window_.push_back(trade);

    // 4. 淘汰超过窗口大小
    while (trade_window_.size() > config_.trade_window) {
        trade_window_.pop_front();
    }

    // 5. 绝对上限保护
    while (trade_window_.size() > config_.max_window_size) {
        trade_window_.pop_front();
    }

    // 6. 更新统计
    stats_.trades_processed.fetch_add(1, std::memory_order_relaxed);

    const auto sz = trade_window_.size();
    stats_.trade_window_size.store(sz, std::memory_order_relaxed);

    auto peak = stats_.trade_window_peak.load(std::memory_order_relaxed);
    while (sz > peak &&
           !stats_.trade_window_peak.compare_exchange_weak(
               peak, sz, std::memory_order_relaxed)) {}

    // 7. 更新 VWAP
    update_vwap();

    return {};
}

Result<void> Microstructure::update_trades(
    std::span<const MicroTrade> trades)
{
    if (trades.empty()) return {};

    std::size_t accepted = 0;
    std::size_t rejected = 0;

    for (const auto& t : trades) {
        auto r = update_trade(t);
        if (r.is_ok()) {
            ++accepted;
        } else {
            ++rejected;
        }
    }

    QUANT_LOG_DEBUG("[Microstructure] 批量更新: {} 接受, {} 拒绝",
        accepted, rejected);

    return {};
}

// ==============================================================================
// 完整快照计算
// ==============================================================================
Result<MicrostructureSnapshot>
Microstructure::compute(const CleanedDepth& depth)
{
    const auto t0 = std::chrono::steady_clock::now();

    stats_.snapshots_computed.fetch_add(1, std::memory_order_relaxed);

    // 1. 校验
    if (!depth.is_valid()) {
        stats_.invalid_snapshots.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_T(MicrostructureSnapshot,
            ErrorCode::UserInputInvalid)
            .with_context("reason", "depth 无效");
    }

    if (depth.has_crossed_book()) {
        stats_.crossed_books.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_T(MicrostructureSnapshot,
            ErrorCode::UserInputInvalid)
            .with_context("reason", "交叉盘");
    }

    // 2. 更新 OFI 窗口
    update_ofi(depth);

    // 3. 构造快照
    MicrostructureSnapshot snap;
    snap.timestamp = depth.timestamp.is_valid()
        ? depth.timestamp : Timestamp::now();

    // 4. 订单簿基础指标
    const double best_bid = depth.bids.front().price;
    const double best_ask = depth.asks.front().price;

    snap.best_bid = best_bid;
    snap.best_ask = best_ask;
    snap.mid_price = (best_bid + best_ask) / 2.0;
    snap.micro_price = compute_micro_price(depth.bids, depth.asks);
    snap.spread = best_ask - best_bid;
    snap.spread_pct = snap.mid_price > 0.0
        ? snap.spread / snap.mid_price : 0.0;
    snap.spread_bps = snap.spread_pct * 10000.0;

    // 5. OBI 三变体
    snap.obi = compute_obi(depth.bids, depth.asks,
                            config_.obi_levels, config_.obi_decay);
    snap.obi_simple = compute_obi_simple(depth.bids, depth.asks,
                                          config_.obi_levels);
    snap.obi_top1 = compute_obi_top1(depth.bids, depth.asks);

    // 6. OFI
    snap.ofi = compute_ofi();

    // 7. 总量
    snap.total_bid_qty = depth.total_bid_qty;
    snap.total_ask_qty = depth.total_ask_qty;

    for (const auto& b : depth.bids) {
        snap.total_bid_notional += b.notional();
    }
    for (const auto& a : depth.asks) {
        snap.total_ask_notional += a.notional();
    }

    // 8. 斜率
    snap.book_slope_bid = compute_book_slope(
        depth.bids, snap.mid_price, true);
    snap.book_slope_ask = compute_book_slope(
        depth.asks, snap.mid_price, false);

    // 9. 深度分桶
    snap.depth_buckets = compute_depth_buckets(
        depth.bids, depth.asks,
        snap.mid_price, config_.depth_buckets, config_.depth_range_pct);

    // 10. 成交指标
    {
        std::lock_guard lock(trade_mutex_);

        // VWAP
        snap.vwap_short = vwap_short_value_;
        snap.vwap_long = vwap_long_value_;
        if (snap.vwap_short > 0.0) {
            snap.vwap_deviation =
                (snap.mid_price - snap.vwap_short) / snap.vwap_short;
        }

        // 成交失衡
        double buy_vol = 0.0;
        double sell_vol = 0.0;
        for (const auto& t : trade_window_) {
            if (t.is_buyer_maker) {
                sell_vol += t.quantity;
            } else {
                buy_vol += t.quantity;
            }
        }
        snap.buy_volume = buy_vol;
        snap.sell_volume = sell_vol;
        snap.trade_count = trade_window_.size();

        const double total_vol = buy_vol + sell_vol;
        if (total_vol > microstructure_config::kEpsilon) {
            snap.trade_imbalance = (buy_vol - sell_vol) / total_vol;
            snap.trade_imbalance = std::clamp(
                snap.trade_imbalance, -1.0, 1.0);
        }
    }

    // 11. 大单检测
    {
        // 计算窗口中位数数量
        std::vector<double> quantities;
        {
            std::lock_guard lock(trade_mutex_);
            quantities.reserve(trade_window_.size());
            for (const auto& t : trade_window_) {
                quantities.push_back(t.quantity);
            }
        }

        const double median = compute_median_copy(std::move(quantities));

        if (median > 0.0) {
            const double threshold = median * config_.large_trade_multiple;

            std::lock_guard lock(trade_mutex_);
            for (const auto& t : trade_window_) {
                if (t.quantity >= threshold) {
                    if (t.is_buyer_maker) {
                        ++snap.large_sell_count;
                        snap.large_sell_volume += t.quantity;
                    } else {
                        ++snap.large_buy_count;
                        snap.large_buy_volume += t.quantity;
                    }
                }
            }

            const auto large = snap.large_buy_count + snap.large_sell_count;
            if (large > 0) {
                stats_.large_trades_detected.fetch_add(large,
                    std::memory_order_relaxed);
            }
        }
    }

    // 12. 成交量分布
    {
        snap.volume_profile = compute_volume_profile_internal();

        // POC + Value Area
        if (!snap.volume_profile.empty()) {
            double max_volume = 0.0;
            std::size_t poc_idx = 0;

            for (std::size_t i = 0; i < snap.volume_profile.size(); ++i) {
                const double v = snap.volume_profile[i].total_volume();
                if (v > max_volume) {
                    max_volume = v;
                    poc_idx = i;
                }
            }

            if (max_volume > 0.0) {
                snap.poc_price = (snap.volume_profile[poc_idx].price_low +
                                  snap.volume_profile[poc_idx].price_high) / 2.0;

                // Value Area：从 POC 向两侧扩展，累积 70%
                const double total_vol = std::accumulate(
                    snap.volume_profile.begin(),
                    snap.volume_profile.end(),
                    0.0,
                    [](double acc, const VolumeProfileBucket& b) {
                        return acc + b.total_volume();
                    });

                const double target = total_vol * 0.7;
                double accumulated = max_volume;

                std::size_t lo = poc_idx;
                std::size_t hi = poc_idx;

                while (accumulated < target &&
                       (lo > 0 || hi + 1 < snap.volume_profile.size())) {
                    const double lo_vol = lo > 0
                        ? snap.volume_profile[lo - 1].total_volume() : -1.0;
                    const double hi_vol = hi + 1 < snap.volume_profile.size()
                        ? snap.volume_profile[hi + 1].total_volume() : -1.0;

                    if (lo_vol >= hi_vol && lo > 0) {
                        --lo;
                        accumulated += lo_vol;
                    } else if (hi + 1 < snap.volume_profile.size()) {
                        ++hi;
                        accumulated += hi_vol;
                    } else {
                        break;
                    }
                }

                snap.value_area_low = snap.volume_profile[lo].price_low;
                snap.value_area_high = snap.volume_profile[hi].price_high;
            }
        }
    }

    // 13. 流动性评分 + 价格冲击
    if (config_.compute_liquidity_score) {
        snap.liquidity_score = compute_liquidity_score(
            depth, config_.target_notional);
    }

    snap.price_impact_bps = estimate_price_impact_bps(
        depth, config_.target_notional);

    // 14. 有限性最终校验
    if (!snap.is_valid()) {
        stats_.invalid_snapshots.fetch_add(1, std::memory_order_relaxed);
        return QUANT_ERR_T(MicrostructureSnapshot,
            ErrorCode::InternalInvariant)
            .with_context("reason", "快照校验失败");
    }

    // 15. 缓存最新快照
    {
        std::lock_guard lock(snapshot_mutex_);
        last_snapshot_ = snap;
    }

    // 16. 更新延迟统计
    const auto t1 = std::chrono::steady_clock::now();
    const auto latency_us = std::chrono::duration_cast<
        std::chrono::microseconds>(t1 - t0).count();

    stats_.last_compute_latency_us.store(latency_us,
        std::memory_order_relaxed);
    stats_.total_compute_latency_us.fetch_add(latency_us,
        std::memory_order_relaxed);

    auto max_lat = stats_.max_compute_latency_us.load(
        std::memory_order_relaxed);
    while (latency_us > max_lat &&
           !stats_.max_compute_latency_us.compare_exchange_weak(
               max_lat, latency_us, std::memory_order_relaxed)) {}

    return snap;
}

// ==============================================================================
// 内部：OFI 更新
// ==============================================================================
void Microstructure::update_ofi(const CleanedDepth& depth) {
    if (depth.bids.empty() || depth.asks.empty()) return;

    DepthSnapshotDelta current;
    current.best_bid = depth.bids.front().price;
    current.best_ask = depth.asks.front().price;
    current.bid_qty = depth.bids.front().quantity;
    current.ask_qty = depth.asks.front().quantity;
    current.timestamp = depth.timestamp.is_valid()
        ? depth.timestamp : Timestamp::now();

    std::lock_guard lock(ofi_mutex_);

    last_depth_snapshot_ = current;
    ofi_window_.push_back(current);

    while (ofi_window_.size() > config_.ofi_window) {
        ofi_window_.pop_front();
    }

    while (ofi_window_.size() > config_.max_window_size) {
        ofi_window_.pop_front();
    }

    stats_.ofi_window_size.store(ofi_window_.size(),
        std::memory_order_relaxed);
}

double Microstructure::compute_ofi() const noexcept {
    std::lock_guard lock(ofi_mutex_);

    if (ofi_window_.size() < 2) return 0.0;

    double ofi_sum = 0.0;
    double ofi_abs_sum = 0.0;

    for (std::size_t i = 1; i < ofi_window_.size(); ++i) {
        const auto& prev = ofi_window_[i - 1];
        const auto& curr = ofi_window_[i];

        // bid 侧贡献
        double delta_bid = 0.0;
        if (curr.best_bid > prev.best_bid) {
            delta_bid = curr.bid_qty;
        } else if (curr.best_bid < prev.best_bid) {
            delta_bid = -prev.bid_qty;
        } else {
            delta_bid = curr.bid_qty - prev.bid_qty;
        }

        // ask 侧贡献（对称，符号相反）
        double delta_ask = 0.0;
        if (curr.best_ask > prev.best_ask) {
            delta_ask = -prev.ask_qty;
        } else if (curr.best_ask < prev.best_ask) {
            delta_ask = curr.ask_qty;
        } else {
            delta_ask = -(curr.ask_qty - prev.ask_qty);
        }

        const double ofi = delta_bid + delta_ask;
        ofi_sum += ofi;
        ofi_abs_sum += std::abs(ofi);
    }

    if (ofi_abs_sum < microstructure_config::kEpsilon) return 0.0;

    const double normalized = ofi_sum / ofi_abs_sum;
    return std::isfinite(normalized)
        ? std::clamp(normalized, -1.0, 1.0)
        : 0.0;
}

// ==============================================================================
// 内部：VWAP 更新
// ==============================================================================
void Microstructure::update_vwap() {
    // 已在 trade_mutex_ 保护下调用

    // 短期 VWAP
    {
        const auto n = std::min(trade_window_.size(),
                                 config_.vwap_short_window);

        if (n == 0) {
            vwap_short_value_ = 0.0;
            return;
        }

        double sum_pv = 0.0;
        double sum_v = 0.0;

        const auto start = trade_window_.size() - n;
        for (std::size_t i = start; i < trade_window_.size(); ++i) {
            const auto& t = trade_window_[i];
            sum_pv += t.price * t.quantity;
            sum_v += t.quantity;
        }

        vwap_short_value_ = sum_v > microstructure_config::kEpsilon
            ? sum_pv / sum_v : 0.0;
    }

    // 长期 VWAP
    {
        const auto n = std::min(trade_window_.size(),
                                 config_.vwap_long_window);

        if (n == 0) {
            vwap_long_value_ = 0.0;
            return;
        }

        double sum_pv = 0.0;
        double sum_v = 0.0;

        const auto start = trade_window_.size() - n;
        for (std::size_t i = start; i < trade_window_.size(); ++i) {
            const auto& t = trade_window_[i];
            sum_pv += t.price * t.quantity;
            sum_v += t.quantity;
        }

        vwap_long_value_ = sum_v > microstructure_config::kEpsilon
            ? sum_pv / sum_v : 0.0;
    }
}

// ==============================================================================
// 内部：成交量分布
// ==============================================================================
std::vector<VolumeProfileBucket>
Microstructure::compute_volume_profile_internal() const
{
    std::vector<VolumeProfileBucket> result;

    std::unique_lock lock(trade_mutex_);

    if (trade_window_.empty()) return result;

    // 确定价格范围
    const auto n = std::min(trade_window_.size(), config_.vp_window);

    double min_price = std::numeric_limits<double>::max();
    double max_price = std::numeric_limits<double>::lowest();

    const auto start = trade_window_.size() - n;
    for (std::size_t i = start; i < trade_window_.size(); ++i) {
        const auto& t = trade_window_[i];
        if (!is_finite(t.price) || t.price <= 0.0) continue;

        min_price = std::min(min_price, t.price);
        max_price = std::max(max_price, t.price);
    }

    if (min_price > max_price) return result;

    // 边界情况：价格完全相同
    if (std::abs(max_price - min_price) < microstructure_config::kEpsilon) {
        VolumeProfileBucket bucket;
        bucket.price_low = min_price;
        bucket.price_high = max_price;

        for (std::size_t i = start; i < trade_window_.size(); ++i) {
            const auto& t = trade_window_[i];
            if (t.is_buyer_maker) {
                bucket.sell_volume += t.quantity;
            } else {
                bucket.buy_volume += t.quantity;
            }
        }

        result.push_back(std::move(bucket));
        return result;
    }

    // 创建桶
    const auto bucket_count = config_.vp_buckets;
    result.resize(bucket_count);

    const double range = max_price - min_price;
    const double bucket_size = range / static_cast<double>(bucket_count);

    for (std::size_t i = 0; i < bucket_count; ++i) {
        result[i].price_low = min_price + static_cast<double>(i) * bucket_size;
        result[i].price_high = min_price + static_cast<double>(i + 1) * bucket_size;
    }

    // 分配成交
    for (std::size_t i = start; i < trade_window_.size(); ++i) {
        const auto& t = trade_window_[i];
        if (!is_finite(t.price) || !is_finite(t.quantity)) continue;
        if (t.quantity <= 0.0) continue;

        const auto idx = static_cast<std::size_t>(
            (t.price - min_price) / bucket_size);
        const auto clamped = std::min(idx, bucket_count - 1);

        if (t.is_buyer_maker) {
            result[clamped].sell_volume += t.quantity;
        } else {
            result[clamped].buy_volume += t.quantity;
        }
    }

    return result;
}

// ==============================================================================
// 内部：大单检测（保留辅助接口）
// ==============================================================================
void Microstructure::detect_large_trades(double median_quantity) {
    if (median_quantity <= 0.0) return;

    const double threshold = median_quantity * config_.large_trade_multiple;

    std::lock_guard lock(trade_mutex_);

    std::size_t count = 0;
    for (const auto& t : trade_window_) {
        if (t.quantity >= threshold) {
            ++count;
        }
    }

    if (count > 0) {
        stats_.large_trades_detected.fetch_add(count,
            std::memory_order_relaxed);
    }
}

// ==============================================================================
// 状态管理
// ==============================================================================
void Microstructure::reset() {
    {
        std::lock_guard lock(trade_mutex_);
        trade_window_.clear();
    }
    {
        std::lock_guard lock(ofi_mutex_);
        ofi_window_.clear();
        last_depth_snapshot_.reset();
    }
    {
        std::lock_guard lock(vwap_mutex_);
        vwap_short_value_ = 0.0;
        vwap_long_value_ = 0.0;
    }
    {
        std::lock_guard lock(snapshot_mutex_);
        last_snapshot_.reset();
    }

    stats_.reset();
}

const MicrostructureStats& Microstructure::stats() const noexcept {
    return stats_;
}

void Microstructure::reset_stats() noexcept {
    stats_.reset();
}

const MicrostructureConfig& Microstructure::config() const noexcept {
    return config_;
}

void Microstructure::set_config(const MicrostructureConfig& config) {
    if (auto r = validate_config(config); r.is_err()) {
        QUANT_LOG_WARN("[Microstructure] 新配置非法，忽略: {}",
            r.error().to_string());
        return;
    }

    config_ = config;

    // 裁剪超出新窗口限制的数据
    {
        std::lock_guard lock(trade_mutex_);
        while (trade_window_.size() > config_.trade_window) {
            trade_window_.pop_front();
        }
    }
    {
        std::lock_guard lock(ofi_mutex_);
        while (ofi_window_.size() > config_.ofi_window) {
            ofi_window_.pop_front();
        }
    }
}

std::string_view Microstructure::symbol() const noexcept {
    return symbol_;
}

std::optional<MicrostructureSnapshot>
Microstructure::last_snapshot() const
{
    std::lock_guard lock(snapshot_mutex_);
    return last_snapshot_;
}

std::size_t Microstructure::trade_window_size() const noexcept {
    std::lock_guard lock(trade_mutex_);
    return trade_window_.size();
}

std::size_t Microstructure::ofi_window_size() const noexcept {
    std::lock_guard lock(ofi_mutex_);
    return ofi_window_.size();
}

// ==============================================================================
// 诊断
// ==============================================================================
std::string Microstructure::dump() const {
    std::string out;
    out.reserve(2048);
    char buf[512];

    out += "Microstructure Dump:\n";

    std::snprintf(buf, sizeof(buf), "  symbol:              %s\n",
        symbol_.empty() ? "(none)" : symbol_.c_str());
    out += buf;

    std::snprintf(buf, sizeof(buf), "  obi_levels:          %zu\n",
        config_.obi_levels);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  obi_decay:           %.2f\n",
        config_.obi_decay);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  ofi_window:          %zu\n",
        config_.ofi_window);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  trade_window:        %zu\n",
        config_.trade_window);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  vp_buckets:          %zu\n",
        config_.vp_buckets);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  large_trade_mult:    %.2f\n",
        config_.large_trade_multiple);
    out += buf;

    out += "  ---\n";

    {
        std::lock_guard lock(trade_mutex_);
        std::snprintf(buf, sizeof(buf), "  trade_window_size:   %zu\n",
            trade_window_.size());
        out += buf;
    }

    {
        std::lock_guard lock(ofi_mutex_);
        std::snprintf(buf, sizeof(buf), "  ofi_window_size:     %zu\n",
            ofi_window_.size());
        out += buf;
    }

    {
        std::lock_guard lock(vwap_mutex_);
        std::snprintf(buf, sizeof(buf), "  vwap_short:          %.4f\n",
            vwap_short_value_);
        out += buf;
        std::snprintf(buf, sizeof(buf), "  vwap_long:           %.4f\n",
            vwap_long_value_);
        out += buf;
    }

    out += "  ---\n";

    std::snprintf(buf, sizeof(buf), "  snapshots_computed:  %llu\n",
        static_cast<unsigned long long>(
            stats_.snapshots_computed.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  trades_processed:    %llu\n",
        static_cast<unsigned long long>(
            stats_.trades_processed.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  trades_rejected:     %llu\n",
        static_cast<unsigned long long>(
            stats_.trades_rejected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  large_trades:        %llu\n",
        static_cast<unsigned long long>(
            stats_.large_trades_detected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  invalid_snapshots:   %llu\n",
        static_cast<unsigned long long>(
            stats_.invalid_snapshots.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  crossed_books:       %llu\n",
        static_cast<unsigned long long>(
            stats_.crossed_books.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  nan_protections:     %llu\n",
        static_cast<unsigned long long>(
            stats_.nan_protections.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  rejection_rate:      %.2f%%\n",
        stats_.rejection_rate() * 100.0);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  avg_latency_us:      %.0f\n",
        stats_.avg_latency_us());
    out += buf;

    std::snprintf(buf, sizeof(buf), "  max_latency_us:      %lld\n",
        static_cast<long long>(
            stats_.max_compute_latency_us.load(std::memory_order_relaxed)));
    out += buf;

    return out;
}

}  // namespace data
}  // namespace quant
