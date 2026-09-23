// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 反向单独立评分系统
// ==============================================================================
// @file    src/strategy/strategy.core.reverse_scorer.hpp
// @module  strategy
// @type    core
// @name    reverse_scorer
// @version 1.0.1
// @brief   逃顶后反向做单的独立评分，5 因子加权，方向对称
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 与顺势评分完全独立：权重、阈值、因子不同
//   - 方向严格对称：多空逻辑镜像
//   - 5 因子加权：下跌速度 + OBI + 成交量 + 加速度 + RSI
//   - 阈值更高：≥ 80（vs 顺势 60）
//   - 无状态设计：每次调用传入完整上下文，便于测试与回放
//   - 评分限幅：clip [0, 100]
//   - 完整分解：每个因子贡献可追溯
//   - 冷却防抖：两次触发之间有最小间隔
//   - MTF 加成：5m 反向确认加分
//
// 评分公式:
//   ReverseScore = 0.25 × drop_speed
//                + 0.20 × obi_flip
//                + 0.20 × volume_spike
//                + 0.20 × acceleration
//                + 0.15 × rsi_flip
//   + MTF bonus (可选 +5)
//
// 触发阈值:
//   反向做空: ReverseScore(short) ≥ 80
//   反向做多: ReverseScore(long)  ≥ 80
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_REVERSE_SCORER_HPP
#define QUANT_STRATEGY_CORE_REVERSE_SCORER_HPP

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
    struct EscapeSignal;
}

namespace quant::strategy {

// ==============================================================================
// 常量
// ==============================================================================
namespace reverse_config {

// 默认触发阈值
inline constexpr double kDefaultThreshold = 80.0;

// 评分上限（防止溢出）
inline constexpr double kScoreMax = 100.0;
inline constexpr double kScoreMin = 0.0;

// 因子权重（默认）
inline constexpr double kDefaultWeightDropSpeed   = 0.25;
inline constexpr double kDefaultWeightObiFlip     = 0.20;
inline constexpr double kDefaultWeightVolumeSpike = 0.20;
inline constexpr double kDefaultWeightAcceleration= 0.20;
inline constexpr double kDefaultWeightRsiFlip     = 0.15;

// 归一化参考值
inline constexpr double kDropSpeedRefAtr   = 2.0;    // 2× ATR 为满分
inline constexpr double kObiFlipRef         = 0.3;    // |OBI| = 0.3 为满分
inline constexpr double kVolumeRatioRef    = 2.0;    // 2 倍均量为满分
inline constexpr double kAccelerationRef   = 1.0;    // 归一化加速度满值

// RSI 阈值
inline constexpr double kRsiOverboughtThreshold = 70.0;
inline constexpr double kRsiOversoldThreshold   = 30.0;

// MTF 加成
inline constexpr double kMtfBonus          = 5.0;    // 加 5 分
inline constexpr double kMtfDistanceRefAtr = 0.3;    // 5m 反向距离阈值

// 冷却期（秒）
inline constexpr std::int64_t kDefaultMinIntervalSec = 60;

// 历史窗口
inline constexpr std::size_t kMaxHistorySize = 64;

// 浮点保护
inline constexpr double kEpsilon = 1e-9;

}  // namespace reverse_config

// ==============================================================================
// 反向评分因子名称（编译期常量）
// ==============================================================================
namespace reverse_factor_names {
inline constexpr std::string_view kDropSpeed    = "drop_speed";
inline constexpr std::string_view kObiFlip       = "obi_flip";
inline constexpr std::string_view kVolumeSpike   = "volume_spike";
inline constexpr std::string_view kAcceleration  = "acceleration";
inline constexpr std::string_view kRsiFlip       = "rsi_flip";
inline constexpr std::string_view kMtfBonus      = "mtf_bonus";

inline constexpr std::size_t kFactorCount = 6;
}  // namespace reverse_factor_names

// ==============================================================================
// 反向评分参数
// ==============================================================================
struct ReverseParams {
    // 触发阈值
    double threshold{reverse_config::kDefaultThreshold};

    // 因子权重
    double weight_drop_speed{reverse_config::kDefaultWeightDropSpeed};
    double weight_obi_flip{reverse_config::kDefaultWeightObiFlip};
    double weight_volume_spike{reverse_config::kDefaultWeightVolumeSpike};
    double weight_acceleration{reverse_config::kDefaultWeightAcceleration};
    double weight_rsi_flip{reverse_config::kDefaultWeightRsiFlip};

    // 归一化参考值
    double drop_speed_ref_atr{reverse_config::kDropSpeedRefAtr};
    double obi_flip_ref{reverse_config::kObiFlipRef};
    double volume_ratio_ref{reverse_config::kVolumeRatioRef};
    double acceleration_ref{reverse_config::kAccelerationRef};

    // RSI 阈值
    double rsi_overbought{reverse_config::kRsiOverboughtThreshold};
    double rsi_oversold{reverse_config::kRsiOversoldThreshold};

    // MTF
    bool enable_mtf_bonus{true};
    double mtf_bonus{reverse_config::kMtfBonus};
    double mtf_distance_ref_atr{reverse_config::kMtfDistanceRefAtr};

    // 冷却
    std::int64_t min_interval_sec{reverse_config::kDefaultMinIntervalSec};

    // 是否启用各因子
    bool enable_drop_speed{true};
    bool enable_obi_flip{true};
    bool enable_volume_spike{true};
    bool enable_acceleration{true};
    bool enable_rsi_flip{true};

    [[nodiscard]] Result<void> validate() const noexcept;

    // 权重之和
    [[nodiscard]] double total_weight() const noexcept {
        return weight_drop_speed + weight_obi_flip +
               weight_volume_spike + weight_acceleration +
               weight_rsi_flip;
    }
};

// ==============================================================================
// 单个因子贡献
// ==============================================================================
struct ReverseFactor {
    std::string_view name;
    double raw_value{0.0};       // 原始归一化值 [0, 1]
    double weight{0.0};          // 权重
    double contribution{0.0};    // 贡献 = raw × weight × 100
    bool valid{true};            // 数据是否有效
    std::string detail;          // 详细说明
};

// ==============================================================================
// 反向评分结果
// ==============================================================================
struct ReverseScoreResult {
    // 主评分（带方向）
    double score{0.0};                   // [0, 100]
    double threshold{0.0};
    bool triggered{false};

    // 方向
    Side side{Side::UNKNOWN};

    // 置信度 = score / 100
    double confidence{0.0};

    // 因子分解
    std::array<ReverseFactor, reverse_factor_names::kFactorCount> factors{};
    std::size_t factor_count{0};

    // MTF 加成
    double mtf_bonus_applied{0.0};
    bool blocked_by_5m{false};

    // 逃顶价关联
    Price escape_price{};
    Price current_price{};

    // 时间
    Timestamp evaluated_at{};

    // 是否有效
    bool valid{false};
    std::string_view invalid_reason;

    // 人类可读原因
    std::string reason;

    [[nodiscard]] bool is_actionable() const noexcept {
        return valid && triggered;
    }

    [[nodiscard]] std::string to_string() const;
};

// ==============================================================================
// 反向评分输入上下文
// ==============================================================================
struct ReverseContext {
    // 逃顶信号（若存在，用于确定逃顶价）
    const EscapeSignal* escape_signal{nullptr};

    // 逃顶价（若无 escape_signal，则显式提供）
    Price escape_price{};

    // 当前 K 线
    Candle candle;

    // 指标
    IndicatorResult indicators;

    // 微观结构（可选）
    struct {
        double obi{0.0};    // 订单簿失衡 [-1, 1]
        bool valid{false};
    } microstructure;

    // MTF 上下文（可选）
    const MTFContext* mtf{nullptr};

    // 当前价格（若未提供，用 candle.close）
    double current_price{0.0};

    [[nodiscard]] bool is_valid() const noexcept;
};

// ==============================================================================
// 反向评分历史记录
// ==============================================================================
struct ReverseScoreRecord {
    Timestamp timestamp{};
    Side side{Side::UNKNOWN};
    double score{0.0};
    bool triggered{false};
    Price escape_price{};
    Price current_price{};
};

// ==============================================================================
// 反向评分快照（持久化）
// ==============================================================================
struct ReverseScorerSnapshot {
    std::uint32_t schema_version{1};
    Timestamp snapshot_time{};
    std::int64_t last_trigger_time_us{0};
    std::uint64_t trigger_count{0};
    std::vector<ReverseScoreRecord> history;

    [[nodiscard]] bool is_valid() const noexcept;
};

// ==============================================================================
// 反向评分统计
// ==============================================================================
struct ReverseStats {
    std::atomic<std::uint64_t> compute_count{0};
    std::atomic<std::uint64_t> triggered_count{0};
    std::atomic<std::uint64_t> short_triggered_count{0};
    std::atomic<std::uint64_t> long_triggered_count{0};
    std::atomic<std::uint64_t> cooldown_blocked_count{0};
    std::atomic<std::uint64_t> invalid_input_count{0};
    std::atomic<std::uint64_t> nan_detected_count{0};
    std::atomic<std::uint64_t> mtf_bonus_count{0};
    std::atomic<double> sum_score{0.0};
    std::atomic<double> sum_triggered_score{0.0};

    void reset() noexcept {
        compute_count.store(0, std::memory_order_relaxed);
        triggered_count.store(0, std::memory_order_relaxed);
        short_triggered_count.store(0, std::memory_order_relaxed);
        long_triggered_count.store(0, std::memory_order_relaxed);
        cooldown_blocked_count.store(0, std::memory_order_relaxed);
        invalid_input_count.store(0, std::memory_order_relaxed);
        nan_detected_count.store(0, std::memory_order_relaxed);
        mtf_bonus_count.store(0, std::memory_order_relaxed);
        sum_score.store(0.0, std::memory_order_relaxed);
        sum_triggered_score.store(0.0, std::memory_order_relaxed);
    }

    [[nodiscard]] double avg_score() const noexcept {
        const auto n = compute_count.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return sum_score.load(std::memory_order_relaxed) /
               static_cast<double>(n);
    }

    [[nodiscard]] double avg_triggered_score() const noexcept {
        const auto n = triggered_count.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return sum_triggered_score.load(std::memory_order_relaxed) /
               static_cast<double>(n);
    }
};

// ==============================================================================
// 反向评分器
// ==============================================================================
class ReverseScorer {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 参数提供者（必需）
        std::function<ReverseParams()> params_provider;

        // 评分触发回调（可选）
        std::function<void(const ReverseScoreResult&)> on_trigger;

        // 状态变更回调（可选，用于持久化）
        std::function<void(const ReverseScorerSnapshot&)> on_state_change;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit ReverseScorer(Dependencies deps);
    ~ReverseScorer();

    ReverseScorer(const ReverseScorer&) = delete;
    ReverseScorer& operator=(const ReverseScorer&) = delete;
    ReverseScorer(ReverseScorer&&) noexcept;
    ReverseScorer& operator=(ReverseScorer&&) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 核心评分
    // -------------------------------------------------------------------------
    // 计算反向做空评分（从逃顶下跌）
    [[nodiscard]] Result<ReverseScoreResult> compute_short(
        const ReverseContext& ctx);

    // 计算反向做多评分（从逃底上涨）
    [[nodiscard]] Result<ReverseScoreResult> compute_long(
        const ReverseContext& ctx);

    // 通用入口：根据 escape_signal 或方向自动选择
    [[nodiscard]] Result<ReverseScoreResult> compute(
        const ReverseContext& ctx,
        Side reverse_side);

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] std::uint64_t trigger_count() const noexcept;
    [[nodiscard]] std::optional<Timestamp> last_trigger_time() const noexcept;
    [[nodiscard]] std::vector<ReverseScoreRecord> history() const;

    // -------------------------------------------------------------------------
    // 手动控制
    // -------------------------------------------------------------------------
    void clear_history() noexcept;
    void clear_cooldown() noexcept;

    // -------------------------------------------------------------------------
    // 持久化
    // -------------------------------------------------------------------------
    [[nodiscard]] ReverseScorerSnapshot to_snapshot() const;
    Result<void> from_snapshot(const ReverseScorerSnapshot& snapshot);

    // -------------------------------------------------------------------------
    // 统计与诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] const ReverseStats& stats() const noexcept;
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

// 归一化辅助：clip(x / ref, 0, 1)
[[nodiscard]] inline double normalize_clip(double x, double ref) noexcept {
    if (ref < reverse_config::kEpsilon) return 0.0;
    const double r = x / ref;
    if (!std::isfinite(r)) return 0.0;
    return std::clamp(r, 0.0, 1.0);
}

// 判断方向是否有效
[[nodiscard]] inline bool is_valid_direction(Side s) noexcept {
    return s == Side::BUY || s == Side::SELL;
}

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_REVERSE_SCORER_HPP
