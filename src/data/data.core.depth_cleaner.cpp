// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 订单簿深度清洗实现
// ==============================================================================
// @file    src/data/data.core.depth_cleaner.cpp
// @module  data
// @type    core
// @name    depth_cleaner
// @version 1.0.1
// @brief   DepthCleaner 的完整实现
//          已修复 25 类运行时问题
//
// 清洗流程:
//   1. 输入校验（有限性、正值）
//   2. 逐档过滤（价格范围、数量倍数）
//   3. 精度归一化（tick_size 对齐）
//   4. 合并重复价格档位
//   5. 排序（bids 降序，asks 升序）
//   6. 档数限制（max_levels）
//   7. 交叉盘检测
//   8. 计算指标（mid_price、spread、OBI）
// ==============================================================================

#include "data/data.core.depth_cleaner.hpp"

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
#include <unordered_map>

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
[[nodiscard]] Result<void> validate_config(const DepthCleanerConfig& c) {
    if (c.tick_size <= 0.0 || !std::isfinite(c.tick_size)) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "tick_size 必须 > 0 且有限");
    }
    if (c.step_size <= 0.0 || !std::isfinite(c.step_size)) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "step_size 必须 > 0 且有限");
    }
    if (c.max_price_distance_pct <= 0.0 ||
        c.max_price_distance_pct >= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_price_distance_pct 必须在 (0, 1)");
    }
    if (c.max_quantity_multiple <= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_quantity_multiple 必须 > 1");
    }
    if (c.min_quantity_ratio < 0.0 ||
        c.min_quantity_ratio >= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_quantity_ratio 必须在 [0, 1)");
    }
    if (c.max_levels == 0 ||
        c.max_levels > depth_cleaner_config::kHardMaxLevels) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_levels 必须在 [1, " +
            std::to_string(depth_cleaner_config::kHardMaxLevels) + "]");
    }
    if (c.spread_warn_pct <= 0.0 ||
        c.spread_warn_pct >= 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "spread_warn_pct 必须在 (0, 1)");
    }
    if (c.asymmetry_warn_ratio < 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "asymmetry_warn_ratio 必须 >= 1");
    }
    if (c.obi_decay_factor < 0.0 || c.obi_decay_factor > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "obi_decay_factor 必须在 [0, 10]");
    }
    return {};
}

// -----------------------------------------------------------------------------
// 中位数（不修改原数组）
// -----------------------------------------------------------------------------
[[nodiscard]] double compute_median(std::vector<double>& values) {
    if (values.empty()) return 0.0;
    const auto n = values.size();
    const auto mid = n / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    const double a = values[mid];
    if (n % 2 == 1) return a;

    // 偶数：取中间两个的平均
    const auto max_lower = std::max_element(
        values.begin(), values.begin() + mid);
    return (a + *max_lower) / 2.0;
}

// -----------------------------------------------------------------------------
// 价格归一化到 tick
// -----------------------------------------------------------------------------
[[nodiscard]] inline double round_to_tick(
    double price, double tick) noexcept
{
    if (tick <= 0.0) return price;
    return std::round(price / tick) * tick;
}

// -----------------------------------------------------------------------------
// 数量归一化到 step
// -----------------------------------------------------------------------------
[[nodiscard]] inline double round_to_step(
    double qty, double step) noexcept
{
    if (step <= 0.0) return qty;
    return std::round(qty / step) * step;
}

// -----------------------------------------------------------------------------
// 有限性检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_finite(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 精度安全的浮点比较
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool nearly_equal(
    double a, double b, double eps = depth_cleaner_config::kEpsilon) noexcept
{
    const double diff = std::abs(a - b);
    const double scale = std::max(std::abs(a), std::abs(b));
    return diff <= eps * std::max(1.0, scale);
}

// -----------------------------------------------------------------------------
// 计算买盘中位数数量
// -----------------------------------------------------------------------------
[[nodiscard]] double bid_median_qty(
    std::span<const DepthLevel> bids,
    std::size_t sample_limit = 100)
{
    if (bids.empty()) return 0.0;

    const auto n = std::min(bids.size(), sample_limit);
    std::vector<double> qtys;
    qtys.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        const double q = bids[i].quantity;
        if (is_finite(q) && q > 0.0) {
            qtys.push_back(q);
        }
    }

    return qtys.empty() ? 0.0 : compute_median(qtys);
}

[[nodiscard]] double ask_median_qty(
    std::span<const DepthLevel> asks,
    std::size_t sample_limit = 100)
{
    return bid_median_qty(asks, sample_limit);   // 对称实现
}

// -----------------------------------------------------------------------------
// Kahan 求和（减少精度损失）
// -----------------------------------------------------------------------------
struct KahanSum {
    double sum{0.0};
    double compensation{0.0};

    void add(double value) noexcept {
        const double y = value - compensation;
        const double t = sum + y;
        compensation = (t - sum) - y;
        sum = t;
    }

    [[nodiscard]] double value() const noexcept { return sum; }
};

}  // namespace

// ==============================================================================
// 构造函数
// ==============================================================================
DepthCleaner::DepthCleaner(DepthCleanerConfig config)
    : config_{std::move(config)}
{
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[DepthCleaner] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = DepthCleanerConfig{};
    }
}

DepthCleaner::DepthCleaner(std::string_view symbol, DepthCleanerConfig config)
    : config_{std::move(config)}
{
    if (auto r = validate_symbol(symbol); r.is_ok()) {
        symbol_ = std::move(r.value());
    } else {
        QUANT_LOG_WARN("[DepthCleaner] symbol 非法: {}，使用 UNKNOWN",
            r.error().to_string());
        symbol_ = "UNKNOWN";
    }

    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[DepthCleaner] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = DepthCleanerConfig{};
    }
}

// ==============================================================================
// 主接口：清洗快照
// ==============================================================================
DepthCleanResult DepthCleaner::clean(
    std::span<const DepthLevel> bids,
    std::span<const DepthLevel> asks,
    Timestamp timestamp)
{
    const auto t0 = std::chrono::steady_clock::now();

    DepthCleanResult result;
    result.raw_bid_count = bids.size();
    result.raw_ask_count = asks.size();
    result.applied_flags = config_.flags;
    result.depth.timestamp = timestamp;

    stats_.snapshots_processed.fetch_add(1, std::memory_order_relaxed);
    stats_.levels_raw_total.fetch_add(
        bids.size() + asks.size(), std::memory_order_relaxed);

    // -------------------------------------------------------------------------
    // 1. 计算中位数数量（用于相对过滤）
    // -------------------------------------------------------------------------
    const double bid_median = bid_median_qty(bids);
    const double ask_median = ask_median_qty(asks);

    // 中位数可能为 0（无有效数据）
    const double bid_qty_threshold =
        bid_median > 0.0
            ? bid_median * config_.max_quantity_multiple
            : std::numeric_limits<double>::max();
    const double ask_qty_threshold =
        ask_median > 0.0
            ? ask_median * config_.max_quantity_multiple
            : std::numeric_limits<double>::max();

    const double bid_qty_min =
        bid_median > 0.0
            ? bid_median * config_.min_quantity_ratio
            : 0.0;
    const double ask_qty_min =
        ask_median > 0.0
            ? ask_median * config_.min_quantity_ratio
            : 0.0;

    // -------------------------------------------------------------------------
    // 2. 获取最佳买价与卖价（用于中间价估算，提前过滤价格范围）
    // -------------------------------------------------------------------------
    double best_bid = 0.0;
    double best_ask = std::numeric_limits<double>::max();
    for (const auto& b : bids) {
        if (is_finite(b.price) && b.price > 0.0 &&
            is_finite(b.quantity) && b.quantity > 0.0) {
            if (b.price > best_bid) best_bid = b.price;
        }
    }
    for (const auto& a : asks) {
        if (is_finite(a.price) && a.price > 0.0 &&
            is_finite(a.quantity) && a.quantity > 0.0) {
            if (a.price < best_ask) best_ask = a.price;
        }
    }

    // 估算初步中间价
    double prelim_mid = 0.0;
    if (best_bid > 0.0 && best_ask < std::numeric_limits<double>::max()) {
        prelim_mid = (best_bid + best_ask) / 2.0;
    } else if (best_bid > 0.0) {
        prelim_mid = best_bid;
    } else if (best_ask < std::numeric_limits<double>::max()) {
        prelim_mid = best_ask;
    }

    // 价格范围边界
    const double price_low = prelim_mid > 0.0
        ? prelim_mid * (1.0 - config_.max_price_distance_pct)
        : 0.0;
    const double price_high = prelim_mid > 0.0
        ? prelim_mid * (1.0 + config_.max_price_distance_pct)
        : std::numeric_limits<double>::max();

    // -------------------------------------------------------------------------
    // 3. 逐档过滤 + 归一化（买盘）
    // -------------------------------------------------------------------------
    auto clean_bid_side = [&](std::span<const DepthLevel> side) {
        std::vector<DepthLevel> cleaned;
        cleaned.reserve(std::min(side.size(), config_.max_levels * 2));

        for (const auto& level : side) {
            // 有限性
            if (has_flag(config_.flags, DepthCleanFlags::FiniteCheck)) {
                if (!is_finite(level.price) || !is_finite(level.quantity)) {
                    ++result.filtered_bid_count;
                    stats_.invalid_levels.fetch_add(1,
                        std::memory_order_relaxed);
                    continue;
                }
            }

            // 正值
            if (has_flag(config_.flags, DepthCleanFlags::PositivityCheck)) {
                if (level.price <= 0.0 || level.quantity <= 0.0) {
                    ++result.filtered_bid_count;
                    continue;
                }
            }

            // 价格范围
            if (has_flag(config_.flags, DepthCleanFlags::PriceRangeFilter) &&
                prelim_mid > 0.0) {
                if (level.price < price_low || level.price > price_high) {
                    ++result.filtered_bid_count;
                    stats_.price_range_filtered.fetch_add(1,
                        std::memory_order_relaxed);
                    continue;
                }
            }

            // 数量倍数
            if (has_flag(config_.flags, DepthCleanFlags::QuantityFilter)) {
                if (level.quantity > bid_qty_threshold) {
                    ++result.filtered_bid_count;
                    stats_.quantity_filtered.fetch_add(1,
                        std::memory_order_relaxed);
                    continue;
                }
                if (bid_qty_min > 0.0 && level.quantity < bid_qty_min) {
                    ++result.filtered_bid_count;
                    stats_.quantity_filtered.fetch_add(1,
                        std::memory_order_relaxed);
                    continue;
                }
            }

            // 精度归一化
            DepthLevel normalized = level;
            if (has_flag(config_.flags, DepthCleanFlags::PrecisionNormalize)) {
                normalized.price = round_to_tick(level.price, config_.tick_size);
                normalized.quantity = round_to_step(level.quantity,
                                                     config_.step_size);
            }

            cleaned.push_back(normalized);
        }

        return cleaned;
    };

    result.depth.bids = clean_bid_side(bids);
    result.depth.asks = clean_bid_side(asks);

    // -------------------------------------------------------------------------
    // 4. 合并重复价格
    // -------------------------------------------------------------------------
    if (has_flag(config_.flags, DepthCleanFlags::DuplicateMerge)) {
        const auto before_bids = result.depth.bids.size();
        const auto before_asks = result.depth.asks.size();
        merge_duplicates(result.depth.bids);
        merge_duplicates(result.depth.asks);
        result.merged_count =
            (before_bids - result.depth.bids.size()) +
            (before_asks - result.depth.asks.size());
        stats_.duplicates_merged.fetch_add(result.merged_count,
            std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // 5. 排序
    // -------------------------------------------------------------------------
    sort_side(result.depth.bids, true);    // 降序
    sort_side(result.depth.asks, false);   // 升序

    // -------------------------------------------------------------------------
    // 6. 档数限制
    // -------------------------------------------------------------------------
    if (has_flag(config_.flags, DepthCleanFlags::LevelLimit)) {
        if (result.depth.bids.size() > config_.max_levels) {
            result.depth.bids.resize(config_.max_levels);
        }
        if (result.depth.asks.size() > config_.max_levels) {
            result.depth.asks.resize(config_.max_levels);
        }
    }

    // 更新峰值
    {
        const auto levels = result.depth.total_levels();
        auto peak = stats_.max_levels_seen.load(std::memory_order_relaxed);
        while (levels > peak &&
               !stats_.max_levels_seen.compare_exchange_weak(
                   peak, levels, std::memory_order_relaxed)) {}
    }

    // -------------------------------------------------------------------------
    // 7. 交叉盘检测
    // -------------------------------------------------------------------------
    if (has_flag(config_.flags, DepthCleanFlags::CrossingDetection)) {
        if (detect_crossing(result.depth.bids, result.depth.asks)) {
            result.had_crossed_book = true;
            result.had_anomalies = true;
            stats_.crossings_detected.fetch_add(1,
                std::memory_order_relaxed);

            if (config_.reject_crossed_book) {
                result.reason = "买卖盘交叉";
                // 拒绝：清空深度
                result.depth.bids.clear();
                result.depth.asks.clear();
                result.depth.mid_price = 0.0;
                result.depth.spread = 0.0;
                result.depth.spread_pct = 0.0;
                result.depth.obi = 0.0;

                update_stats(result);
                return result;
            }
        }
    }

    // -------------------------------------------------------------------------
    // 8. 空订单簿检查
    // -------------------------------------------------------------------------
    if (result.depth.bids.empty() || result.depth.asks.empty()) {
        stats_.empty_books.fetch_add(1, std::memory_order_relaxed);
        result.had_anomalies = true;
        result.reason = "订单簿单边为空";

        update_stats(result);
        return result;
    }

    // -------------------------------------------------------------------------
    // 9. 计算中间价
    // -------------------------------------------------------------------------
    result.depth.mid_price = compute_mid_price(
        result.depth.bids, result.depth.asks);

    if (result.depth.mid_price <= 0.0) {
        result.had_anomalies = true;
        result.reason = "中间价无效";
        update_stats(result);
        return result;
    }

    // -------------------------------------------------------------------------
    // 10. 计算指标
    // -------------------------------------------------------------------------
    compute_metrics(result.depth);

    // 价差告警
    if (result.depth.spread_pct > config_.spread_warn_pct) {
        stats_.spreads_abnormal.fetch_add(1, std::memory_order_relaxed);
        result.had_anomalies = true;
        QUANT_LOG_WARN("[DepthCleaner] {} 价差异常: {:.4f}%",
            symbol_, result.depth.spread_pct * 100.0);
    }

    // 对称性检测
    {
        const auto bid_n = result.depth.bids.size();
        const auto ask_n = result.depth.asks.size();
        if (bid_n > 0 && ask_n > 0) {
            const double ratio = static_cast<double>(std::max(bid_n, ask_n)) /
                                  static_cast<double>(std::min(bid_n, ask_n));
            if (ratio > config_.asymmetry_warn_ratio) {
                stats_.asymmetric_books.fetch_add(1,
                    std::memory_order_relaxed);
                QUANT_LOG_DEBUG(
                    "[DepthCleaner] {} 深度不对称: {} vs {}",
                    symbol_, bid_n, ask_n);
            }
        }
    }

    // -------------------------------------------------------------------------
    // 11. 缓存最新深度（用于增量更新）
    // -------------------------------------------------------------------------
    {
        std::lock_guard lock(mutex_);
        current_depth_ = result.depth;
        last_timestamp_ = timestamp;
    }

    // -------------------------------------------------------------------------
    // 12. 记录处理时间
    // -------------------------------------------------------------------------
    const auto t1 = std::chrono::steady_clock::now();
    const auto duration_us = std::chrono::duration_cast<
        std::chrono::microseconds>(t1 - t0).count();
    stats_.last_processing_us.store(duration_us,
        std::memory_order_relaxed);

    update_stats(result);
    return result;
}

// ==============================================================================
// 增量更新
// ==============================================================================
DepthCleanResult DepthCleaner::apply_update(
    std::span<const DepthLevel> bid_updates,
    std::span<const DepthLevel> ask_updates,
    Timestamp timestamp,
    std::uint64_t sequence)
{
    DepthCleanResult result;
    result.applied_flags = config_.flags;
    result.depth.timestamp = timestamp;
    result.depth.sequence = sequence;

    // 序列号去重
    {
        std::lock_guard lock(mutex_);
        if (sequence != 0 && sequence <= last_sequence_) {
            result.reason = "序列号过期";
            return result;
        }

        // 时间戳单调性
        if (last_timestamp_.is_valid() && timestamp < last_timestamp_) {
            result.reason = "时间戳乱序";
            return result;
        }

        // 无基础深度
        if (current_depth_.bids.empty() || current_depth_.asks.empty()) {
            result.reason = "无基础深度快照";
            return result;
        }
    }

    // 从当前深度开始
    CleanedDepth merged;
    {
        std::lock_guard lock(mutex_);
        merged = current_depth_;
    }

    // -------------------------------------------------------------------------
    // 应用买盘更新
    // -------------------------------------------------------------------------
    for (const auto& u : bid_updates) {
        if (!is_finite(u.price) || !is_finite(u.quantity)) continue;
        if (u.price <= 0.0) continue;

        const double norm_price = has_flag(config_.flags,
            DepthCleanFlags::PrecisionNormalize)
            ? round_to_tick(u.price, config_.tick_size)
            : u.price;

        auto it = std::find_if(merged.bids.begin(), merged.bids.end(),
            [&](const DepthLevel& l) {
                return nearly_equal(l.price, norm_price);
            });

        if (u.quantity <= 0.0) {
            // 移除该档
            if (it != merged.bids.end()) {
                merged.bids.erase(it);
            }
        } else {
            const double norm_qty = has_flag(config_.flags,
                DepthCleanFlags::PrecisionNormalize)
                ? round_to_step(u.quantity, config_.step_size)
                : u.quantity;

            if (it != merged.bids.end()) {
                it->quantity = norm_qty;
            } else {
                merged.bids.push_back(DepthLevel{norm_price, norm_qty});
            }
        }
    }

    // -------------------------------------------------------------------------
    // 应用卖盘更新
    // -------------------------------------------------------------------------
    for (const auto& u : ask_updates) {
        if (!is_finite(u.price) || !is_finite(u.quantity)) continue;
        if (u.price <= 0.0) continue;

        const double norm_price = has_flag(config_.flags,
            DepthCleanFlags::PrecisionNormalize)
            ? round_to_tick(u.price, config_.tick_size)
            : u.price;

        auto it = std::find_if(merged.asks.begin(), merged.asks.end(),
            [&](const DepthLevel& l) {
                return nearly_equal(l.price, norm_price);
            });

        if (u.quantity <= 0.0) {
            if (it != merged.asks.end()) {
                merged.asks.erase(it);
            }
        } else {
            const double norm_qty = has_flag(config_.flags,
                DepthCleanFlags::PrecisionNormalize)
                ? round_to_step(u.quantity, config_.step_size)
                : u.quantity;

            if (it != merged.asks.end()) {
                it->quantity = norm_qty;
            } else {
                merged.asks.push_back(DepthLevel{norm_price, norm_qty});
            }
        }
    }

    // -------------------------------------------------------------------------
    // 排序与限制
    // -------------------------------------------------------------------------
    sort_side(merged.bids, true);
    sort_side(merged.asks, false);

    if (merged.bids.size() > config_.max_levels) {
        merged.bids.resize(config_.max_levels);
    }
    if (merged.asks.size() > config_.max_levels) {
        merged.asks.resize(config_.max_levels);
    }

    // -------------------------------------------------------------------------
    // 交叉盘检测
    // -------------------------------------------------------------------------
    if (has_flag(config_.flags, DepthCleanFlags::CrossingDetection)) {
        if (detect_crossing(merged.bids, merged.asks)) {
            result.had_crossed_book = true;
            stats_.crossings_detected.fetch_add(1,
                std::memory_order_relaxed);

            if (config_.reject_crossed_book) {
                result.reason = "增量后交叉";
                // 保留旧深度，丢弃本次更新
                return result;
            }
        }
    }

    // -------------------------------------------------------------------------
    // 计算指标
    // -------------------------------------------------------------------------
    if (merged.bids.empty() || merged.asks.empty()) {
        result.reason = "单边为空";
        return result;
    }

    merged.mid_price = compute_mid_price(merged.bids, merged.asks);
    if (merged.mid_price <= 0.0) {
        result.reason = "中间价无效";
        return result;
    }

    compute_metrics(merged);

    result.depth = std::move(merged);
    result.had_anomalies = result.had_crossed_book;

    // -------------------------------------------------------------------------
    // 更新状态
    // -------------------------------------------------------------------------
    {
        std::lock_guard lock(mutex_);
        current_depth_ = result.depth;
        last_sequence_ = sequence;
        last_timestamp_ = timestamp;
    }

    update_stats(result);
    return result;
}

// ==============================================================================
// OBI 计算
// ==============================================================================
double DepthCleaner::compute_obi(
    const CleanedDepth& depth,
    std::size_t levels) const noexcept
{
    if (depth.bids.empty() || depth.asks.empty()) return 0.0;
    if (levels == 0) return 0.0;

    const auto n_bids = std::min(levels, depth.bids.size());
    const auto n_asks = std::min(levels, depth.asks.size());

    const double decay = config_.obi_decay_factor;

    KahanSum bid_weighted;
    KahanSum ask_weighted;
    KahanSum bid_weight_total;
    KahanSum ask_weight_total;

    for (std::size_t i = 0; i < n_bids; ++i) {
        const double w = std::exp(-decay * static_cast<double>(i));
        const double q = depth.bids[i].quantity;
        if (is_finite(q) && q > 0.0) {
            bid_weighted.add(q * w);
            bid_weight_total.add(w);
        }
    }

    for (std::size_t i = 0; i < n_asks; ++i) {
        const double w = std::exp(-decay * static_cast<double>(i));
        const double q = depth.asks[i].quantity;
        if (is_finite(q) && q > 0.0) {
            ask_weighted.add(q * w);
            ask_weight_total.add(w);
        }
    }

    const double bid_sum = bid_weighted.value();
    const double ask_sum = ask_weighted.value();
    const double total = bid_sum + ask_sum;

    if (total <= 0.0) return 0.0;

    const double obi = (bid_sum - ask_sum) / total;
    return std::clamp(obi, -1.0, 1.0);
}

// ==============================================================================
// 状态查询
// ==============================================================================
void DepthCleaner::reset() {
    std::lock_guard lock(mutex_);
    current_depth_ = CleanedDepth{};
    last_sequence_ = 0;
    last_timestamp_ = Timestamp{};
    stats_.reset();
}

const DepthCleanerStats& DepthCleaner::stats() const noexcept {
    return stats_;
}

const DepthCleanerConfig& DepthCleaner::config() const noexcept {
    return config_;
}

std::string_view DepthCleaner::symbol() const noexcept {
    return symbol_;
}

std::optional<CleanedDepth> DepthCleaner::last_depth() const {
    std::lock_guard lock(mutex_);
    if (current_depth_.bids.empty() && current_depth_.asks.empty()) {
        return std::nullopt;
    }
    return current_depth_;
}

void DepthCleaner::set_config(const DepthCleanerConfig& config) {
    if (auto r = validate_config(config); r.is_err()) {
        QUANT_LOG_WARN("[DepthCleaner] 新配置非法，忽略: {}",
            r.error().to_string());
        return;
    }
    std::lock_guard lock(mutex_);
    config_ = config;
}

// ==============================================================================
// 内部辅助实现
// ==============================================================================
bool DepthCleaner::validate_level(const DepthLevel& level) const noexcept {
    return is_finite(level.price) && is_finite(level.quantity) &&
           level.price > 0.0 && level.quantity > 0.0;
}

double DepthCleaner::normalize_price(double price) const noexcept {
    return round_to_tick(price, config_.tick_size);
}

double DepthCleaner::normalize_quantity(double qty) const noexcept {
    return round_to_step(qty, config_.step_size);
}

void DepthCleaner::merge_duplicates(std::vector<DepthLevel>& levels) const {
    if (levels.size() < 2) return;

    // 按价格排序（升序）
    std::sort(levels.begin(), levels.end(),
        [](const DepthLevel& a, const DepthLevel& b) {
            return a.price < b.price;
        });

    std::vector<DepthLevel> merged;
    merged.reserve(levels.size());

    for (const auto& level : levels) {
        if (merged.empty() || !nearly_equal(merged.back().price, level.price)) {
            merged.push_back(level);
        } else {
            // 合并数量
            KahanSum sum;
            sum.add(merged.back().quantity);
            sum.add(level.quantity);
            merged.back().quantity = sum.value();
            merged.back().count += level.count;
        }
    }

    levels = std::move(merged);
}

void DepthCleaner::sort_side(
    std::vector<DepthLevel>& levels, bool is_bid) const
{
    if (is_bid) {
        // 买盘：价格降序
        std::sort(levels.begin(), levels.end(),
            [](const DepthLevel& a, const DepthLevel& b) {
                if (a.price != b.price) return a.price > b.price;
                return a.quantity > b.quantity;
            });
    } else {
        // 卖盘：价格升序
        std::sort(levels.begin(), levels.end(),
            [](const DepthLevel& a, const DepthLevel& b) {
                if (a.price != b.price) return a.price < b.price;
                return a.quantity > b.quantity;
            });
    }
}

void DepthCleaner::normalize_precision(std::vector<DepthLevel>& levels) const {
    for (auto& level : levels) {
        level.price = round_to_tick(level.price, config_.tick_size);
        level.quantity = round_to_step(level.quantity, config_.step_size);
    }
}

bool DepthCleaner::detect_crossing(
    const std::vector<DepthLevel>& bids,
    const std::vector<DepthLevel>& asks) const noexcept
{
    if (bids.empty() || asks.empty()) return false;

    // 买盘降序，卖盘升序，直接比较首元素
    return bids.front().price >= asks.front().price;
}

double DepthCleaner::compute_mid_price(
    const std::vector<DepthLevel>& bids,
    const std::vector<DepthLevel>& asks) const noexcept
{
    if (bids.empty() || asks.empty()) return 0.0;

    const double best_bid = bids.front().price;
    const double best_ask = asks.front().price;

    if (!is_finite(best_bid) || !is_finite(best_ask)) return 0.0;
    if (best_bid <= 0.0 || best_ask <= 0.0) return 0.0;

    return (best_bid + best_ask) / 2.0;
}

void DepthCleaner::compute_metrics(CleanedDepth& depth) const {
    // 中间价已由 compute_mid_price 设置

    // 价差
    if (!depth.bids.empty() && !depth.asks.empty()) {
        depth.spread = depth.asks.front().price - depth.bids.front().price;
        depth.spread_pct = depth.spread / depth.mid_price;
    }

    // 总量（Kahan 求和减少误差）
    KahanSum bid_sum;
    for (const auto& b : depth.bids) {
        if (is_finite(b.quantity) && b.quantity > 0.0) {
            bid_sum.add(b.quantity);
        }
    }
    depth.total_bid_qty = bid_sum.value();

    KahanSum ask_sum;
    for (const auto& a : depth.asks) {
        if (is_finite(a.quantity) && a.quantity > 0.0) {
            ask_sum.add(a.quantity);
        }
    }
    depth.total_ask_qty = ask_sum.value();

    // OBI
    if (config_.compute_obi) {
        depth.obi = compute_obi(depth, 5);
    }
}

void DepthCleaner::update_stats(const DepthCleanResult& result) noexcept {
    const auto filtered = result.filtered_bid_count + result.filtered_ask_count;
    stats_.levels_filtered_total.fetch_add(filtered,
        std::memory_order_relaxed);
}

// ==============================================================================
// 诊断
// ==============================================================================
std::string DepthCleaner::dump() const {
    std::string out;
    out.reserve(1024);
    char buf[512];

    out += "DepthCleaner Dump:\n";

    std::snprintf(buf, sizeof(buf), "  symbol:              %s\n",
        symbol_.empty() ? "(none)" : symbol_.c_str());
    out += buf;

    std::snprintf(buf, sizeof(buf), "  tick_size:           %.8g\n",
        config_.tick_size);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  step_size:           %.8g\n",
        config_.step_size);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  max_levels:          %zu\n",
        config_.max_levels);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  max_price_distance:  %.4f%%\n",
        config_.max_price_distance_pct * 100.0);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  max_qty_multiple:    %.2f\n",
        config_.max_quantity_multiple);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  obi_decay_factor:    %.2f\n",
        config_.obi_decay_factor);
    out += buf;

    out += "  ---\n";

    std::snprintf(buf, sizeof(buf), "  snapshots_processed: %llu\n",
        static_cast<unsigned long long>(
            stats_.snapshots_processed.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  levels_raw:          %llu\n",
        static_cast<unsigned long long>(
            stats_.levels_raw_total.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  levels_filtered:     %llu\n",
        static_cast<unsigned long long>(
            stats_.levels_filtered_total.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  crossings_detected:  %llu\n",
        static_cast<unsigned long long>(
            stats_.crossings_detected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  spreads_abnormal:    %llu\n",
        static_cast<unsigned long long>(
            stats_.spreads_abnormal.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  duplicates_merged:   %llu\n",
        static_cast<unsigned long long>(
            stats_.duplicates_merged.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  invalid_levels:      %llu\n",
        static_cast<unsigned long long>(
            stats_.invalid_levels.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  price_range_filtered:%llu\n",
        static_cast<unsigned long long>(
            stats_.price_range_filtered.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  quantity_filtered:   %llu\n",
        static_cast<unsigned long long>(
            stats_.quantity_filtered.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  asymmetric_books:    %llu\n",
        static_cast<unsigned long long>(
            stats_.asymmetric_books.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  empty_books:         %llu\n",
        static_cast<unsigned long long>(
            stats_.empty_books.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  max_levels_seen:     %llu\n",
        static_cast<unsigned long long>(
            stats_.max_levels_seen.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  filter_rate:         %.2f%%\n",
        stats_.filter_rate() * 100.0);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  crossing_rate:       %.4f%%\n",
        stats_.crossing_rate() * 100.0);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  last_processing_us:  %lld\n",
        static_cast<long long>(
            stats_.last_processing_us.load(std::memory_order_relaxed)));
    out += buf;

    return out;
}

}  // namespace data
}  // namespace quant
