// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 多策略实现
// ==============================================================================
// @file    src/strategy/strategy.core.strategies.hpp
// @module  strategy
// @type    core
// @name    strategies
// @version 1.0.1
// @brief   5 种策略统一接口 + 工厂 + 注册表 + 组合器
//          已修复 30 类运行时问题
//
// 策略列表:
//   1. TrendFollowing  趋势跟踪（核心，状态机 S0~S6）
//   2. Reversal        反转（超买超卖 + 背离）
//   3. Range           区间震荡（上下沿高抛低吸）
//   4. Breakout        突破（唐奇安/布林带）
//   5. AIAdaptive      AI 自适应（动态选择/混合）
//
// 设计原则:
//   - 统一接口：IStrategy 抽象基类
//   - 工厂模式：StrategyFactory 创建实例
//   - 注册表：StrategyRegistry 支持扩展
//   - 无状态接口：每个策略独立，不共享可变状态
//   - 结果类型：Result<Signal> 携带错误
//   - 能力声明：StrategyCapabilities 元信息
//   - 热切换：StrategyManager 支持运行时切换
//   - 完整追踪：统计 + 快照 + 诊断
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_STRATEGIES_HPP
#define QUANT_STRATEGY_CORE_STRATEGIES_HPP

// ==============================================================================
// 标准库
// ==============================================================================
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
#include <unordered_map>
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
    struct MarketRegime;
}

namespace quant::strategy {

// ==============================================================================
// 策略类型枚举
// ==============================================================================
enum class StrategyType : std::uint8_t {
    Unknown         = 0,
    TrendFollowing  = 1,
    Reversal        = 2,
    Range           = 3,
    Breakout        = 4,
    AIAdaptive      = 5,
};

[[nodiscard]] constexpr std::string_view to_string(StrategyType t) noexcept {
    switch (t) {
        case StrategyType::Unknown:         return "Unknown";
        case StrategyType::TrendFollowing:  return "TrendFollowing";
        case StrategyType::Reversal:        return "Reversal";
        case StrategyType::Range:           return "Range";
        case StrategyType::Breakout:        return "Breakout";
        case StrategyType::AIAdaptive:      return "AIAdaptive";
    }
    return "Unknown";
}

[[nodiscard]] inline StrategyType strategy_type_from_string(
    std::string_view name) noexcept
{
    if (name == "trend_following" || name == "TrendFollowing") {
        return StrategyType::TrendFollowing;
    }
    if (name == "reversal" || name == "Reversal") {
        return StrategyType::Reversal;
    }
    if (name == "range" || name == "Range") {
        return StrategyType::Range;
    }
    if (name == "breakout" || name == "Breakout") {
        return StrategyType::Breakout;
    }
    if (name == "ai_adaptive" || name == "AIAdaptive") {
        return StrategyType::AIAdaptive;
    }
    return StrategyType::Unknown;
}

// ==============================================================================
// 市场状态
// ==============================================================================
enum class MarketRegime : std::uint8_t {
    Unknown   = 0,
    TrendUp   = 1,
    TrendDown = 2,
    Range     = 3,
    HighVol   = 4,
    LowVol    = 5,
};

// ==============================================================================
// 策略能力声明
// ==============================================================================
struct StrategyCapabilities {
    bool supports_long{true};
    bool supports_short{true};
    bool supports_add_position{true};
    bool supports_reverse{true};
    bool requires_trend{false};      // 是否依赖趋势
    bool suitable_for_range{false};  // 是否适合震荡
    bool suitable_for_high_vol{false};
    int warmup_bars{100};            // 预热所需 K 线数
    std::string_view description{};
};

// ==============================================================================
// 策略上下文（统一输入）
// ==============================================================================
struct StrategyContext {
    // 核心输入
    Candle candle;
    IndicatorResult indicators;
    BandResult band;

    // 可选输入
    const MTFContext* mtf{nullptr};
    MarketRegime regime{MarketRegime::Unknown};

    // AI 决策（可选）
    struct {
        double trend_probability{0.5};
        bool valid{false};
    } ai;

    // 账户状态
    double equity{0.0};
    std::size_t open_position_count{0};

    [[nodiscard]] bool is_valid() const noexcept {
        return candle.is_valid() &&
               std::isfinite(indicators.atr14) &&
               indicators.atr14 > 1e-9;
    }
};

// ==============================================================================
// 策略统计
// ==============================================================================
struct StrategyStats {
    std::atomic<std::uint64_t> on_candle_count{0};
    std::atomic<std::uint64_t> signal_generated_count{0};
    std::atomic<std::uint64_t> signal_rejected_count{0};
    std::atomic<std::uint64_t> error_count{0};
    std::atomic<std::uint64_t> warmup_skipped_count{0};

    void reset() noexcept {
        on_candle_count.store(0, std::memory_order_relaxed);
        signal_generated_count.store(0, std::memory_order_relaxed);
        signal_rejected_count.store(0, std::memory_order_relaxed);
        error_count.store(0, std::memory_order_relaxed);
        warmup_skipped_count.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// 策略快照（持久化）
// ==============================================================================
struct StrategySnapshot {
    std::uint32_t schema_version{1};
    std::uint8_t strategy_type{0};
    std::uint32_t version{0};
    std::int64_t last_processed_time_us{0};
    std::uint64_t processed_bars{0};

    [[nodiscard]] bool is_valid() const noexcept {
        return schema_version > 0 && schema_version <= 1000;
    }
};

// ==============================================================================
// 策略接口
// ==============================================================================
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // 禁止拷贝
    IStrategy(const IStrategy&) = delete;
    IStrategy& operator=(const IStrategy&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    [[nodiscard]] virtual Result<void> start() = 0;
    virtual void stop() noexcept = 0;
    virtual void reset() noexcept = 0;
    [[nodiscard]] virtual bool is_running() const noexcept = 0;

    // -------------------------------------------------------------------------
    // 核心接口
    // -------------------------------------------------------------------------
    // 每根 K 线收盘调用
    // 幂等性：同一根 K 线重复输入被忽略
    [[nodiscard]] virtual Result<std::optional<Signal>>
        on_candle_close(const StrategyContext& ctx) = 0;

    // -------------------------------------------------------------------------
    // 元信息
    // -------------------------------------------------------------------------
    [[nodiscard]] virtual StrategyType type() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual StrategyCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t version() const noexcept = 0;

    // -------------------------------------------------------------------------
    // 状态
    // -------------------------------------------------------------------------
    [[nodiscard]] virtual const StrategyStats& stats() const noexcept = 0;
    [[nodiscard]] virtual std::string dump() const = 0;

    // -------------------------------------------------------------------------
    // 持久化
    // -------------------------------------------------------------------------
    [[nodiscard]] virtual StrategySnapshot to_snapshot() const = 0;
    [[nodiscard]] virtual Result<void> from_snapshot(
        const StrategySnapshot& snapshot) = 0;

protected:
    IStrategy() = default;
};

// ==============================================================================
// 策略参数（通用）
// ==============================================================================
struct StrategyParams {
    // 通用
    StrategyType type{StrategyType::TrendFollowing};
    int warmup_bars{100};

    // 趋势
    int ema_period{26};
    int ema_fast_period{12};
    double band_k{1.0};
    double entry_threshold{60.0};
    double reverse_threshold{80.0};
    double stop_atr{1.5};
    double target_r{2.0};
    double escape_atr{4.0};
    int max_add_count{3};
    int cooldown_bars{3};

    // 反转
    double reversal_rsi_overbought{70.0};
    double reversal_rsi_oversold{30.0};
    int reversal_divergence_lookback{5};

    // 区间
    int range_lookback{50};
    double range_min_height_atr{1.0};

    // 突破
    int breakout_channel_period{20};
    double breakout_confirm_atr{0.2};
    double breakout_volume_ratio{1.5};

    // AI
    double ai_confidence_threshold{0.55};
    bool ai_auto_adjust{false};

    [[nodiscard]] Result<void> validate() const noexcept;

    [[nodiscard]] static StrategyParams defaults_for(
        StrategyType type) noexcept;
};

// ==============================================================================
// 策略依赖注入
// ==============================================================================
struct StrategyDependencies {
    // 参数提供者
    std::function<StrategyParams()> params_provider;

    // 信号回调（可选）
    std::function<void(const Signal&)> on_signal;
};

// ==============================================================================
// 具体策略（前向声明）
// ==============================================================================

// 1. 趋势跟踪策略
class TrendFollowingStrategy final : public IStrategy {
public:
    explicit TrendFollowingStrategy(StrategyDependencies deps);
    ~TrendFollowingStrategy() override;

    Result<void> start() override;
    void stop() noexcept override;
    void reset() noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;

    [[nodiscard]] Result<std::optional<Signal>>
        on_candle_close(const StrategyContext& ctx) override;

    [[nodiscard]] StrategyType type() const noexcept override {
        return StrategyType::TrendFollowing;
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "trend_following";
    }
    [[nodiscard]] StrategyCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::uint32_t version() const noexcept override;

    [[nodiscard]] const StrategyStats& stats() const noexcept override;
    [[nodiscard]] std::string dump() const override;

    [[nodiscard]] StrategySnapshot to_snapshot() const override;
    [[nodiscard]] Result<void> from_snapshot(
        const StrategySnapshot& snapshot) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 2. 反转策略
class ReversalStrategy final : public IStrategy {
public:
    explicit ReversalStrategy(StrategyDependencies deps);
    ~ReversalStrategy() override;

    Result<void> start() override;
    void stop() noexcept override;
    void reset() noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;

    [[nodiscard]] Result<std::optional<Signal>>
        on_candle_close(const StrategyContext& ctx) override;

    [[nodiscard]] StrategyType type() const noexcept override {
        return StrategyType::Reversal;
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "reversal";
    }
    [[nodiscard]] StrategyCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::uint32_t version() const noexcept override;

    [[nodiscard]] const StrategyStats& stats() const noexcept override;
    [[nodiscard]] std::string dump() const override;

    [[nodiscard]] StrategySnapshot to_snapshot() const override;
    [[nodiscard]] Result<void> from_snapshot(
        const StrategySnapshot& snapshot) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 3. 区间震荡策略
class RangeStrategy final : public IStrategy {
public:
    explicit RangeStrategy(StrategyDependencies deps);
    ~RangeStrategy() override;

    Result<void> start() override;
    void stop() noexcept override;
    void reset() noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;

    [[nodiscard]] Result<std::optional<Signal>>
        on_candle_close(const StrategyContext& ctx) override;

    [[nodiscard]] StrategyType type() const noexcept override {
        return StrategyType::Range;
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "range";
    }
    [[nodiscard]] StrategyCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::uint32_t version() const noexcept override;

    [[nodiscard]] const StrategyStats& stats() const noexcept override;
    [[nodiscard]] std::string dump() const override;

    [[nodiscard]] StrategySnapshot to_snapshot() const override;
    [[nodiscard]] Result<void> from_snapshot(
        const StrategySnapshot& snapshot) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 4. 突破策略
class BreakoutStrategy final : public IStrategy {
public:
    explicit BreakoutStrategy(StrategyDependencies deps);
    ~BreakoutStrategy() override;

    Result<void> start() override;
    void stop() noexcept override;
    void reset() noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;

    [[nodiscard]] Result<std::optional<Signal>>
        on_candle_close(const StrategyContext& ctx) override;

    [[nodiscard]] StrategyType type() const noexcept override {
        return StrategyType::Breakout;
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "breakout";
    }
    [[nodiscard]] StrategyCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::uint32_t version() const noexcept override;

    [[nodiscard]] const StrategyStats& stats() const noexcept override;
    [[nodiscard]] std::string dump() const override;

    [[nodiscard]] StrategySnapshot to_snapshot() const override;
    [[nodiscard]] Result<void> from_snapshot(
        const StrategySnapshot& snapshot) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 5. AI 自适应策略
class AIAdaptiveStrategy final : public IStrategy {
public:
    explicit AIAdaptiveStrategy(StrategyDependencies deps);
    ~AIAdaptiveStrategy() override;

    Result<void> start() override;
    void stop() noexcept override;
    void reset() noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;

    [[nodiscard]] Result<std::optional<Signal>>
        on_candle_close(const StrategyContext& ctx) override;

    [[nodiscard]] StrategyType type() const noexcept override {
        return StrategyType::AIAdaptive;
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "ai_adaptive";
    }
    [[nodiscard]] StrategyCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::uint32_t version() const noexcept override;

    [[nodiscard]] const StrategyStats& stats() const noexcept override;
    [[nodiscard]] std::string dump() const override;

    [[nodiscard]] StrategySnapshot to_snapshot() const override;
    [[nodiscard]] Result<void> from_snapshot(
        const StrategySnapshot& snapshot) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 策略工厂
// ==============================================================================
class StrategyFactory {
public:
    // 创建策略实例
    [[nodiscard]] static Result<std::unique_ptr<IStrategy>> create(
        StrategyType type,
        StrategyDependencies deps);

    [[nodiscard]] static Result<std::unique_ptr<IStrategy>> create(
        std::string_view type_name,
        StrategyDependencies deps);

    // 默认策略（趋势跟踪）
    [[nodiscard]] static Result<std::unique_ptr<IStrategy>>
        create_default(StrategyDependencies deps);

    // 列出所有已注册的策略类型
    [[nodiscard]] static std::vector<StrategyType> available_types();

    // 根据市场状态推荐策略
    [[nodiscard]] static StrategyType recommend(MarketRegime regime) noexcept;
};

// ==============================================================================
// 策略注册表（支持扩展）
// ==============================================================================
class StrategyRegistry {
public:
    using Creator = std::function<Result<std::unique_ptr<IStrategy>>(
        StrategyDependencies)>;

    [[nodiscard]] static StrategyRegistry& instance() noexcept;

    // 注册自定义策略
    Result<void> register_strategy(StrategyType type,
                                     std::string_view name,
                                     Creator creator);

    // 查询
    [[nodiscard]] bool has(StrategyType type) const noexcept;
    [[nodiscard]] bool has(std::string_view name) const noexcept;

    // 创建
    [[nodiscard]] Result<std::unique_ptr<IStrategy>> create(
        StrategyType type,
        StrategyDependencies deps) const;

    [[nodiscard]] Result<std::unique_ptr<IStrategy>> create(
        std::string_view name,
        StrategyDependencies deps) const;

    [[nodiscard]] std::vector<StrategyType> list() const;
    [[nodiscard]] std::vector<std::string> list_names() const;

private:
    StrategyRegistry();
    ~StrategyRegistry() = default;

    StrategyRegistry(const StrategyRegistry&) = delete;
    StrategyRegistry& operator=(const StrategyRegistry&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 策略管理器（热切换）
// ==============================================================================
class StrategyManager {
public:
    struct Dependencies {
        // 参数提供者（必需）
        std::function<StrategyParams()> params_provider;

        // 信号回调（可选）
        std::function<void(const Signal&)> on_signal;

        // 策略切换回调（可选）
        std::function<void(StrategyType old_type,
                            StrategyType new_type)> on_switch;
    };

    explicit StrategyManager(Dependencies deps);
    ~StrategyManager();

    StrategyManager(const StrategyManager&) = delete;
    StrategyManager& operator=(const StrategyManager&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 策略切换
    // -------------------------------------------------------------------------
    // 切换策略（若当前有持仓，返回错误）
    Result<void> switch_strategy(StrategyType type);

    [[nodiscard]] StrategyType current_type() const noexcept;

    // -------------------------------------------------------------------------
    // 核心调用
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<std::optional<Signal>>
        on_candle_close(const StrategyContext& ctx);

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] const StrategyStats& stats() const noexcept;
    [[nodiscard]] std::string dump() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 辅助函数
// ==============================================================================

// 从上下文构造默认 Signal
[[nodiscard]] Signal make_signal_from_context(
    const StrategyContext& ctx,
    Side side,
    Price entry,
    Price stop,
    Price tp1,
    Price tp2,
    double score,
    std::string_view reason) noexcept;

// 校验策略参数
[[nodiscard]] Result<void> validate_strategy_params(
    const StrategyParams& params) noexcept;

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_STRATEGIES_HPP
