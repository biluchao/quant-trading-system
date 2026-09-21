// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 微观结构指标
// ==============================================================================
// @file    src/data/data.core.microstructure.hpp
// @module  data
// @type    core
// @name    microstructure
// @version 1.0.1
// @brief   订单簿微观结构：OBI、OFI、VWAP、VP、斜率、微价格、大单检测
//          已修复 25 类运行时问题
//
// 指标清单:
//   - OBI: Order Book Imbalance（加权订单簿失衡）
//   - OFI: Order Flow Imbalance（订单流失衡，基于快照差分）
//   - Spread: 价差（绝对值 / 百分比 / bps）
//   - Mid Price / Micro Price
//   - Depth Profile: 距离分桶深度分布
//   - VWAP: 成交量加权平均价
//   - Volume Profile: 成交量分布
//   - Book Slope: 订单簿斜率
//   - Trade Imbalance: 成交失衡（基于成交流）
//   - Large Trade Detection: 大单检测
//
// 与上下游的分工:
//   - 上游: DepthCleaner 提供 CleanedDepth；WS/REST 提供 Trade
//   - 本模块: 计算微观结构指标
//   - 下游: Strategy / AI 消费 MicrostructureSnapshot
//
// 设计原则:
//   - 零分配热路径：滚动窗口使用固定容量 deque
//   - 加权衰减：指数衰减强调顶部档位
//   - NaN 保护：所有输出有限性检查
//   - 线程安全：mutex 保护滚动窗口
//   - 可观测：MicrostructureStats + dump()
//   - 可配置：MicrostructureConfig 30+ 参数
// ==============================================================================

#ifndef QUANT_DATA_CORE_MICROSTRUCTURE_HPP
#define QUANT_DATA_CORE_MICROSTRUCTURE_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"
#include "data/data.core.depth_cleaner.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace microstructure_config {

// 默认参数
inline constexpr std::size_t kDefaultObiLevels = 5;
inline constexpr std::size_t kDefaultOfiWindow = 20;
inline constexpr std::size_t kDefaultVwapWindow = 60;
inline constexpr std::size_t kDefaultTradeWindow = 100;
inline constexpr std::size_t kDefaultVpBuckets = 20;
inline constexpr std::size_t kDefaultDepthBuckets = 10;

// 加权衰减
inline constexpr double kDefaultObiDecay = 1.0;
inline constexpr double kDefaultDepthDecay = 0.5;

// 大单阈值
inline constexpr double kDefaultLargeTradeMultiple = 10.0;

// 滚动窗口上限
inline constexpr std::size_t kMaxWindowSize = 10'000;

// 浮点容差
inline constexpr double kEpsilon = 1e-12;

// 深度衰减距离（相对中间价）
inline constexpr double kDefaultDepthRangePct = 0.02;

}  // namespace microstructure_config

// ==============================================================================
// 成交流（用于微观结构分析）
// ==============================================================================
struct MicroTrade {
    Timestamp timestamp{};
    double price{0.0};
    double quantity{0.0};
    bool is_buyer_maker{false};   // true = 主动卖单（卖方主动）

    [[nodiscard]] bool is_valid() const noexcept {
        return timestamp.is_valid() &&
               std::isfinite(price) && price > 0.0 &&
               std::isfinite(quantity) && quantity > 0.0;
    }

    [[nodiscard]] double notional() const noexcept {
        return price * quantity;
    }

    // 主动方向：is_buyer_maker = true 表示卖方主动
    [[nodiscard]] Side aggressor_side() const noexcept {
        return is_buyer_maker ? Side::SELL : Side::BUY;
    }
};

// ==============================================================================
// 深度桶
// ==============================================================================
struct DepthBucket {
    double price_low{0.0};        // 桶下界
    double price_high{0.0};       // 桶上界
    double bid_qty{0.0};          // 买盘累积量
    double ask_qty{0.0};          // 卖盘累积量

    [[nodiscard]] double imbalance() const noexcept {
        const double total = bid_qty + ask_qty;
        if (total <= 0.0) return 0.0;
        return (bid_qty - ask_qty) / total;
    }
};

// ==============================================================================
// 成交量分布桶
// ==============================================================================
struct VolumeProfileBucket {
    double price_low{0.0};
    double price_high{0.0};
    double buy_volume{0.0};
    double sell_volume{0.0};

    [[nodiscard]] double total_volume() const noexcept {
        return buy_volume + sell_volume;
    }

    [[nodiscard]] double imbalance() const noexcept {
        const double total = total_buy_sell();
        if (total <= 0.0) return 0.0;
        return (buy_volume - sell_volume) / total;
    }

private:
    [[nodiscard]] double total_buy_sell() const noexcept {
        return buy_volume + sell_volume;
    }
};

// ==============================================================================
// 微观结构快照（输出）
// ==============================================================================
struct MicrostructureSnapshot {
    Timestamp timestamp{};

    // -------------------------------------------------------------------------
    // 订单簿（来自 CleanedDepth）
    // -------------------------------------------------------------------------
    double best_bid{0.0};
    double best_ask{0.0};
    double mid_price{0.0};
    double micro_price{0.0};      // 加权微价格
    double spread{0.0};
    double spread_pct{0.0};       // spread / mid
    double spread_bps{0.0};       // spread / mid × 10000

    // -------------------------------------------------------------------------
    // 订单簿失衡
    // -------------------------------------------------------------------------
    double obi{0.0};              // 加权 OBI [-1, 1]
    double obi_simple{0.0};       // 简单 OBI（等权）
    double obi_top1{0.0};         // 仅 best bid/ask

    // 订单流失衡（基于快照差分）
    double ofi{0.0};              // [-1, 1]

    // -------------------------------------------------------------------------
    // 深度
    // -------------------------------------------------------------------------
    double total_bid_qty{0.0};
    double total_ask_qty{0.0};
    double total_bid_notional{0.0};
    double total_ask_notional{0.0};
    double book_slope_bid{0.0};   // 买盘斜率
    double book_slope_ask{0.0};   // 卖盘斜率

    // 距离分桶
    std::vector<DepthBucket> depth_buckets;

    // -------------------------------------------------------------------------
    // 成交
    // -------------------------------------------------------------------------
    double vwap_short{0.0};       // 短期 VWAP（最近 N 笔）
    double vwap_long{0.0};        // 长期 VWAP
    double vwap_deviation{0.0};   // 价格偏离 VWAP

    double trade_imbalance{0.0};  // 成交失衡 [-1, 1]
    double buy_volume{0.0};       // 窗口内主动买入量
    double sell_volume{0.0};      // 窗口内主动卖出量
    std::size_t trade_count{0};

    // 大单
    std::size_t large_buy_count{0};
    std::size_t large_sell_count{0};
    double large_buy_volume{0.0};
    double large_sell_volume{0.0};

    // -------------------------------------------------------------------------
    // 成交量分布
    // -------------------------------------------------------------------------
    std::vector<VolumeProfileBucket> volume_profile;
    double poc_price{0.0};        // Point of Control（成交量最大价）
    double value_area_high{0.0};  // Value Area 上界（70% 成交量）
    double value_area_low{0.0};   // Value Area 下界

    // -------------------------------------------------------------------------
    // 流动性
    // -------------------------------------------------------------------------
    double liquidity_score{0.0};  // [0, 1] 综合流动性评分
    double price_impact_bps{0.0}; // 预估 1000 USDT 冲击成本

    // -------------------------------------------------------------------------
    // 校验
    // -------------------------------------------------------------------------
    [[nodiscard]] bool is_valid() const noexcept {
        return timestamp.is_valid() &&
               std::isfinite(mid_price) && mid_price > 0.0 &&
               std::isfinite(obi) && obi >= -1.0 && obi <= 1.0 &&
               std::isfinite(spread) && spread >= 0.0;
    }

    [[nodiscard]] bool has_crossed_book() const noexcept {
        return best_bid > 0.0 && best_ask > 0.0 && best_bid >= best_ask;
    }
};

// ==============================================================================
// 统计
// ==============================================================================
struct MicrostructureStats {
    alignas(64) std::atomic<std::uint64_t> snapshots_computed{0};
    alignas(64) std::atomic<std::uint64_t> trades_processed{0};
    alignas(64) std::atomic<std::uint64_t> trades_rejected{0};
    alignas(64) std::atomic<std::uint64_t> large_trades_detected{0};
    alignas(64) std::atomic<std::uint64_t> invalid_snapshots{0};
    alignas(64) std::atomic<std::uint64_t> crossed_books{0};
    alignas(64) std::atomic<std::uint64_t> nan_protections{0};

    alignas(64) std::atomic<std::int64_t> last_compute_latency_us{0};
    alignas(64) std::atomic<std::int64_t> max_compute_latency_us{0};
    alignas(64) std::atomic<std::int64_t> total_compute_latency_us{0};

    alignas(64) std::atomic<std::size_t> trade_window_size{0};
    alignas(64) std::atomic<std::size_t> trade_window_peak{0};
    alignas(64) std::atomic<std::size_t> ofi_window_size{0};

    void reset() noexcept {
        snapshots_computed.store(0, std::memory_order_relaxed);
        trades_processed.store(0, std::memory_order_relaxed);
        trades_rejected.store(0, std::memory_order_relaxed);
        large_trades_detected.store(0, std::memory_order_relaxed);
        invalid_snapshots.store(0, std::memory_order_relaxed);
        crossed_books.store(0, std::memory_order_relaxed);
        nan_protections.store(0, std::memory_order_relaxed);
        last_compute_latency_us.store(0, std::memory_order_relaxed);
        max_compute_latency_us.store(0, std::memory_order_relaxed);
        total_compute_latency_us.store(0, std::memory_order_relaxed);
        trade_window_size.store(0, std::memory_order_relaxed);
        trade_window_peak.store(0, std::memory_order_relaxed);
        ofi_window_size.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double avg_latency_us() const noexcept {
        const auto n = snapshots_computed.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return static_cast<double>(
            total_compute_latency_us.load(std::memory_order_relaxed)) / n;
    }

    [[nodiscard]] double rejection_rate() const noexcept {
        const auto total = trades_processed.load(std::memory_order_relaxed) +
                           trades_rejected.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            trades_rejected.load(std::memory_order_relaxed)) / total;
    }
};

// ==============================================================================
// 配置
// ==============================================================================
struct MicrostructureConfig {
    // OBI
    std::size_t obi_levels{microstructure_config::kDefaultObiLevels};
    double obi_decay{microstructure_config::kDefaultObiDecay};

    // OFI
    std::size_t ofi_window{microstructure_config::kDefaultOfiWindow};

    // 深度分桶
    std::size_t depth_buckets{microstructure_config::kDefaultDepthBuckets};
    double depth_range_pct{microstructure_config::kDefaultDepthRangePct};
    double depth_decay{microstructure_config::kDefaultDepthDecay};

    // VWAP
    std::size_t vwap_short_window{microstructure_config::kDefaultVwapWindow};
    std::size_t vwap_long_window{microstructure_config::kDefaultVwapWindow * 5};

    // 成交流
    std::size_t trade_window{microstructure_config::kDefaultTradeWindow};

    // 成交量分布
    std::size_t vp_buckets{microstructure_config::kDefaultVpBuckets};
    std::size_t vp_window{500};   // 使用的成交流数量

    // 大单
    double large_trade_multiple{
        microstructure_config::kDefaultLargeTradeMultiple};

    // 流动性评分
    bool compute_liquidity_score{true};
    double target_notional{1000.0};   // 冲击成本估算基准

    // 窗口上限
    std::size_t max_window_size{microstructure_config::kMaxWindowSize};

    // 日志
    LogLevel log_level{LogLevel::INFO};
};

// ==============================================================================
// 微观结构计算器
// ==============================================================================
class Microstructure {
public:
    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    explicit Microstructure(MicrostructureConfig config = {});

    Microstructure(std::string_view symbol,
                    MicrostructureConfig config = {});

    ~Microstructure() = default;

    Microstructure(const Microstructure&) = delete;
    Microstructure& operator=(const Microstructure&) = delete;
    Microstructure(Microstructure&&) = delete;
    Microstructure& operator=(Microstructure&&) = delete;

    // -------------------------------------------------------------------------
    // 数据输入
    // -------------------------------------------------------------------------
    // 更新订单簿快照
    Result<void> update_depth(const CleanedDepth& depth);

    // 更新单笔成交
    Result<void> update_trade(const MicroTrade& trade);

    // 批量成交
    Result<void> update_trades(std::span<const MicroTrade> trades);

    // 结合订单簿与成交，计算完整快照
    [[nodiscard]] Result<MicrostructureSnapshot>
        compute(const CleanedDepth& depth);

    // -------------------------------------------------------------------------
    // 纯函数（不修改状态）
    // -------------------------------------------------------------------------

    // 加权 OBI
    [[nodiscard]] static double compute_obi(
        const std::vector<DepthLevel>& bids,
        const std::vector<DepthLevel>& asks,
        std::size_t levels = microstructure_config::kDefaultObiLevels,
        double decay = microstructure_config::kDefaultObiDecay) noexcept;

    // 简单 OBI（等权）
    [[nodiscard]] static double compute_obi_simple(
        const std::vector<DepthLevel>& bids,
        const std::vector<DepthLevel>& asks,
        std::size_t levels = microstructure_config::kDefaultObiLevels) noexcept;

    // 顶部 OBI（仅 best bid/ask）
    [[nodiscard]] static double compute_obi_top1(
        const std::vector<DepthLevel>& bids,
        const std::vector<DepthLevel>& asks) noexcept;

    // 微价格
    [[nodiscard]] static double compute_micro_price(
        const std::vector<DepthLevel>& bids,
        const std::vector<DepthLevel>& asks) noexcept;

    // 订单簿斜率
    [[nodiscard]] static double compute_book_slope(
        const std::vector<DepthLevel>& levels,
        double mid_price,
        bool is_bid) noexcept;

    // 深度分桶
    [[nodiscard]] static std::vector<DepthBucket> compute_depth_buckets(
        const std::vector<DepthLevel>& bids,
        const std::vector<DepthLevel>& asks,
        double mid_price,
        std::size_t bucket_count,
        double range_pct) noexcept;

    // 流动性评分
    [[nodiscard]] static double compute_liquidity_score(
        const CleanedDepth& depth,
        double target_notional) noexcept;

    // 价格冲击估算（bps）
    [[nodiscard]] static double estimate_price_impact_bps(
        const CleanedDepth& depth,
        double target_notional) noexcept;

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    void reset();

    [[nodiscard]] const MicrostructureStats& stats() const noexcept;
    void reset_stats() noexcept;

    [[nodiscard]] const MicrostructureConfig& config() const noexcept;
    void set_config(const MicrostructureConfig& config);

    [[nodiscard]] std::string_view symbol() const noexcept;

    [[nodiscard]] std::optional<MicrostructureSnapshot>
        last_snapshot() const;

    [[nodiscard]] std::size_t trade_window_size() const noexcept;
    [[nodiscard]] std::size_t ofi_window_size() const noexcept;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    // -------------------------------------------------------------------------
    // 内部
    // -------------------------------------------------------------------------

    // 计算 OFI（基于快照差分）
    void update_ofi(const CleanedDepth& depth);

    [[nodiscard]] double compute_ofi() const noexcept;

    // 计算 VWAP
    void update_vwap();

    // 成交量分布
    void update_volume_profile();
    [[nodiscard]] std::vector<VolumeProfileBucket>
        compute_volume_profile_internal() const;

    // 大单检测
    void detect_large_trades(double median_quantity);

    // 有效分母
    [[nodiscard]] static double safe_denom(double v) noexcept {
        return std::abs(v) < microstructure_config::kEpsilon
            ? microstructure_config::kEpsilon
            : v;
    }

    // NaN 保护
    [[nodiscard]] static double sanitize(double v) noexcept {
        return std::isfinite(v) ? v : 0.0;
    }

    // -------------------------------------------------------------------------
    // 成员
    // -------------------------------------------------------------------------
    std::string symbol_;
    MicrostructureConfig config_;
    MicrostructureStats stats_;

    // 成交滚动窗口
    mutable std::mutex trade_mutex_;
    std::deque<MicroTrade> trade_window_;

    // OFI 快照窗口
    struct DepthSnapshotDelta {
        double best_bid{0.0};
        double best_ask{0.0};
        double bid_qty{0.0};
        double ask_qty{0.0};
        Timestamp timestamp{};
    };

    mutable std::mutex ofi_mutex_;
    std::deque<DepthSnapshotDelta> ofi_window_;
    std::optional<DepthSnapshotDelta> last_depth_snapshot_;

    // VWAP 累积
    mutable std::mutex vwap_mutex_;
    double vwap_short_value_{0.0};
    double vwap_long_value_{0.0};

    // 最新快照
    mutable std::mutex snapshot_mutex_;
    std::optional<MicrostructureSnapshot> last_snapshot_;
};

// ==============================================================================
// 便捷函数
// ==============================================================================
[[nodiscard]] inline double safe_divide(
    double numerator, double denominator, double fallback = 0.0) noexcept
{
    if (std::abs(denominator) < microstructure_config::kEpsilon) {
        return fallback;
    }
    const double result = numerator / denominator;
    return std::isfinite(result) ? result : fallback;
}

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_MICROSTRUCTURE_HPP
