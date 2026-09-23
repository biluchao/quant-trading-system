// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 多策略实现
// ==============================================================================
// @file    src/strategy/strategy.core.strategies.cpp
// @module  strategy
// @type    core
// @name    strategies
// @version 1.0.1
// @brief   5 种策略实现 + 工厂 + 注册表 + 管理器
//          已修复 30 类运行时问题
//
// 通用实现模式:
//   每个策略的 Impl 持有:
//     - deps（依赖）
//     - params（参数缓存）
//     - stats（统计）
//     - last_processed_time（幂等性）
//     - processed_bars（预热计数）
//     - running（运行标志）
//   每个策略的 on_candle_close:
//     1. 运行检查
//     2. 幂等性检查
//     3. 上下文有效性
//     4. 预热检查
//     5. 策略特有逻辑
//     6. 信号构造 + 校验
// ==============================================================================

#include "strategy/strategy.core.strategies.hpp"
#include "strategy/strategy.core.mtf_checker.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 匿名命名空间：辅助函数
// ==============================================================================
namespace {

constexpr double kEps = 1e-9;
constexpr std::int64_t kCandleIntervalUs =
    3LL * 60LL * 1000LL * 1000LL;

[[nodiscard]] inline double safe_div(double a, double b,
                                       double def = 0.0) noexcept {
    if (std::abs(b) < kEps) return def;
    const double r = a / b;
    return std::isfinite(r) ? r : def;
}

[[nodiscard]] inline double clamp_val(double v, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, v));
}

[[nodiscard]] inline bool finite(double v) noexcept {
    return std::isfinite(v);
}

[[nodiscard]] inline bool is_long(Side s) noexcept {
    return s == Side::BUY;
}

[[nodiscard]] inline bool valid_side(Side s) noexcept {
    return s == Side::BUY || s == Side::SELL;
}

// 幂等性检查辅助
[[nodiscard]] inline bool is_duplicate_candle(
    Timestamp last, Timestamp current) noexcept
{
    if (!last.is_valid()) return false;
    return current.microseconds() <= last.microseconds();
}

// 计算止损距离
[[nodiscard]] inline double compute_stop_distance(
    double entry, double stop) noexcept
{
    const double d = std::abs(entry - stop);
    return finite(d) ? d : 0.0;
}

// 校验信号盈亏比
[[nodiscard]] inline double compute_rr(
    double entry, double stop, double target) noexcept
{
    const double risk = compute_stop_distance(entry, stop);
    if (risk < kEps) return 0.0;
    const double reward = std::abs(target - entry);
    return safe_div(reward, risk, 0.0);
}

}  // namespace

// ==============================================================================
// StrategyParams::validate
// ==============================================================================
Result<void> StrategyParams::validate() const noexcept {
    // 通用
    if (warmup_bars < 0 || warmup_bars > 10000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "warmup_bars 超出范围 [0, 10000]");
    }

    // 趋势
    if (ema_period < 5 || ema_period > 200) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ema_period 超出范围 [5, 200]");
    }
    if (ema_fast_period < 3 || ema_fast_period >= ema_period) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ema_fast_period 无效");
    }
    if (band_k < 0.3 || band_k > 3.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "band_k 超出范围 [0.3, 3.0]");
    }
    if (entry_threshold < 0.0 || entry_threshold > 100.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "entry_threshold 超出范围 [0, 100]");
    }
    if (reverse_threshold < entry_threshold ||
        reverse_threshold > 100.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "reverse_threshold 无效");
    }
    if (stop_atr < 0.3 || stop_atr > 5.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "stop_atr 超出范围 [0.3, 5.0]");
    }
    if (target_r < 1.0 || target_r > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "target_r 超出范围 [1.0, 10.0]");
    }
    if (escape_atr < 1.5 || escape_atr > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "escape_atr 超出范围 [1.5, 10.0]");
    }
    if (max_add_count < 0 || max_add_count > 5) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_add_count 超出范围 [0, 5]");
    }
    if (cooldown_bars < 0 || cooldown_bars > 50) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "cooldown_bars 超出范围 [0, 50]");
    }

    // 反转
    if (reversal_rsi_overbought < 50.0 ||
        reversal_rsi_overbought > 100.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "reversal_rsi_overbought 超出范围 [50, 100]");
    }
    if (reversal_rsi_oversold < 0.0 ||
        reversal_rsi_oversold > 50.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "reversal_rsi_oversold 超出范围 [0, 50]");
    }
    if (reversal_divergence_lookback < 2 ||
        reversal_divergence_lookback > 50) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "reversal_divergence_lookback 超出范围 [2, 50]");
    }

    // 区间
    if (range_lookback < 10 || range_lookback > 500) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "range_lookback 超出范围 [10, 500]");
    }
    if (range_min_height_atr < 0.5 || range_min_height_atr > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "range_min_height_atr 超出范围 [0.5, 10.0]");
    }

    // 突破
    if (breakout_channel_period < 5 ||
        breakout_channel_period > 200) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "breakout_channel_period 超出范围 [5, 200]");
    }
    if (breakout_confirm_atr < 0.0 || breakout_confirm_atr > 2.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "breakout_confirm_atr 超出范围 [0, 2.0]");
    }
    if (breakout_volume_ratio < 0.5 ||
        breakout_volume_ratio > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "breakout_volume_ratio 超出范围 [0.5, 10.0]");
    }

    // AI
    if (ai_confidence_threshold < 0.0 ||
        ai_confidence_threshold > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ai_confidence_threshold 超出范围 [0, 1]");
    }

    return {};
}

// ==============================================================================
// StrategyParams::defaults_for
// ==============================================================================
StrategyParams StrategyParams::defaults_for(
    StrategyType type) noexcept
{
    StrategyParams p;
    p.type = type;

    switch (type) {
        case StrategyType::TrendFollowing:
            p.warmup_bars = 100;
            p.ema_period = 26;
            p.ema_fast_period = 12;
            p.band_k = 1.0;
            p.entry_threshold = 60.0;
            p.reverse_threshold = 80.0;
            p.stop_atr = 1.5;
            p.target_r = 2.0;
            p.escape_atr = 4.0;
            p.max_add_count = 3;
            p.cooldown_bars = 3;
            break;

        case StrategyType::Reversal:
            p.warmup_bars = 50;
            p.ema_period = 26;
            p.ema_fast_period = 12;
            p.band_k = 1.0;
            p.entry_threshold = 60.0;
            p.reverse_threshold = 80.0;
            p.stop_atr = 1.0;
            p.target_r = 1.5;
            p.escape_atr = 3.0;
            p.max_add_count = 0;
            p.cooldown_bars = 5;
            p.reversal_rsi_overbought = 70.0;
            p.reversal_rsi_oversold = 30.0;
            p.reversal_divergence_lookback = 5;
            break;

        case StrategyType::Range:
            p.warmup_bars = 100;
            p.ema_period = 26;
            p.ema_fast_period = 12;
            p.band_k = 1.3;
            p.entry_threshold = 65.0;
            p.reverse_threshold = 80.0;
            p.stop_atr = 1.0;
            p.target_r = 1.5;
            p.escape_atr = 3.0;
            p.max_add_count = 0;
            p.cooldown_bars = 3;
            p.range_lookback = 50;
            p.range_min_height_atr = 1.0;
            break;

        case StrategyType::Breakout:
            p.warmup_bars = 100;
            p.ema_period = 26;
            p.ema_fast_period = 12;
            p.band_k = 0.8;
            p.entry_threshold = 60.0;
            p.reverse_threshold = 80.0;
            p.stop_atr = 2.0;
            p.target_r = 3.0;
            p.escape_atr = 4.0;
            p.max_add_count = 2;
            p.cooldown_bars = 5;
            p.breakout_channel_period = 20;
            p.breakout_confirm_atr = 0.2;
            p.breakout_volume_ratio = 1.5;
            break;

        case StrategyType::AIAdaptive:
            p.warmup_bars = 150;
            p.ema_period = 26;
            p.ema_fast_period = 12;
            p.band_k = 1.0;
            p.entry_threshold = 55.0;
            p.reverse_threshold = 80.0;
            p.stop_atr = 1.5;
            p.target_r = 2.5;
            p.escape_atr = 4.0;
            p.max_add_count = 3;
            p.cooldown_bars = 3;
            p.ai_confidence_threshold = 0.55;
            break;

        case StrategyType::Unknown:
        default:
            break;
    }

    return p;
}

// ==============================================================================
// 辅助函数：从上下文构造 Signal
// ==============================================================================
Signal make_signal_from_context(
    const StrategyContext& ctx,
    Side side,
    Price entry,
    Price stop,
    Price tp1,
    Price tp2,
    double score,
    std::string_view reason) noexcept
{
    Signal signal;
    signal.symbol = Symbol{};
    signal.entry_price = entry;
    signal.stop_loss = stop;
    signal.take_profit_1 = tp1;
    signal.take_profit_2 = tp2;
    signal.side = side;
    signal.score = score;
    signal.confidence = clamp_val(score / 100.0, 0.0, 1.0);
    signal.timestamp = ctx.candle.close_time;
    signal.suggested_quantity = Quantity{};

    // reason 通过 SignalId 或外部记录，signal 本身不持有 string_view
    (void)reason;

    return signal;
}

// ==============================================================================
// 校验策略参数
// ==============================================================================
Result<void> validate_strategy_params(
    const StrategyParams& params) noexcept
{
    return params.validate();
}

// ==============================================================================
// 策略能力（各策略的能力声明）
// ==============================================================================
namespace capabilities {

[[nodiscard]] StrategyCapabilities trend_following() noexcept {
    StrategyCapabilities c;
    c.supports_long = true;
    c.supports_short = true;
    c.supports_add_position = true;
    c.supports_reverse = true;
    c.requires_trend = true;
    c.suitable_for_range = false;
    c.suitable_for_high_vol = true;
    c.warmup_bars = 100;
    c.description = "趋势跟踪：均线+动态带宽+多状态机";
    return c;
}

[[nodiscard]] StrategyCapabilities reversal() noexcept {
    StrategyCapabilities c;
    c.supports_long = true;
    c.supports_short = true;
    c.supports_add_position = false;
    c.supports_reverse = true;
    c.requires_trend = false;
    c.suitable_for_range = true;
    c.suitable_for_high_vol = false;
    c.warmup_bars = 50;
    c.description = "反转：超买超卖 + RSI 背离";
    return c;
}

[[nodiscard]] StrategyCapabilities range() noexcept {
    StrategyCapabilities c;
    c.supports_long = true;
    c.supports_short = true;
    c.supports_add_position = false;
    c.supports_reverse = true;
    c.requires_trend = false;
    c.suitable_for_range = true;
    c.suitable_for_high_vol = false;
    c.warmup_bars = 100;
    c.description = "区间震荡：上下沿高抛低吸";
    return c;
}

[[nodiscard]] StrategyCapabilities breakout() noexcept {
    StrategyCapabilities c;
    c.supports_long = true;
    c.supports_short = true;
    c.supports_add_position = true;
    c.supports_reverse = true;
    c.requires_trend = true;
    c.suitable_for_range = false;
    c.suitable_for_high_vol = true;
    c.warmup_bars = 100;
    c.description = "突破：唐奇安通道 + 量能确认";
    return c;
}

[[nodiscard]] StrategyCapabilities ai_adaptive() noexcept {
    StrategyCapabilities c;
    c.supports_long = true;
    c.supports_short = true;
    c.supports_add_position = true;
    c.supports_reverse = true;
    c.requires_trend = false;
    c.suitable_for_range = true;
    c.suitable_for_high_vol = true;
    c.warmup_bars = 150;
    c.description = "AI 自适应：动态选择 + 权重融合";
    return c;
}

}  // namespace capabilities

// ==============================================================================
// 通用策略实现辅助：StrategyImplBase
// ==============================================================================
// 每个策略共享的字段与通用逻辑，减少重复
struct StrategyImplBase {
    StrategyDependencies deps;
    StrategyParams params;
    StrategyStats stats;
    std::atomic<bool> running{false};

    // 幂等性
    Timestamp last_processed_time{};

    // 预热
    std::uint64_t processed_bars{0};

    // 冷却
    Timestamp cooldown_until{};

    // 上次信号（用于去重）
    std::optional<Signal> last_signal;

    // 读写锁
    mutable std::shared_mutex mutex;

    // ---------------------------------------------------------------
    // 通用生命周期
    // ---------------------------------------------------------------
    [[nodiscard]] Result<void> base_start(StrategyType type) {
        bool expected = false;
        if (!running.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
                "策略已启动");
        }

        // 加载参数
        if (auto r = load_params(type); r.is_err()) {
            running.store(false, std::memory_order_release);
            return r;
        }

        // 清空状态
        {
            std::unique_lock<std::shared_mutex> lock(mutex);
            last_processed_time = Timestamp{};
            processed_bars = 0;
            cooldown_until = Timestamp{};
            last_signal.reset();
        }

        return {};
    }

    void base_stop() noexcept {
        if (!running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock(mutex);
        last_processed_time = Timestamp{};
        processed_bars = 0;
        cooldown_until = Timestamp{};
        last_signal.reset();
    }

    void base_reset() noexcept {
        std::unique_lock<std::shared_mutex> lock(mutex);
        last_processed_time = Timestamp{};
        processed_bars = 0;
        cooldown_until = Timestamp{};
        last_signal.reset();
        stats.reset();
    }

    [[nodiscard]] Result<void> load_params(StrategyType type) {
        if (!deps.params_provider) {
            return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
                "params_provider 未注入");
        }

        StrategyParams p;
        try {
            p = deps.params_provider();
        } catch (const std::exception& e) {
            return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
                "参数加载异常")
                .with_context("error", e.what());
        } catch (...) {
            return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
                "参数加载未知异常");
        }

        if (auto r = p.validate(); r.is_err()) {
            return r;
        }

        // 类型不匹配修正
        if (p.type != type) {
            p.type = type;
        }

        std::unique_lock<std::shared_mutex> lock(mutex);
        params = p;
        return {};
    }

    // ---------------------------------------------------------------
    // 通用检查
    // ---------------------------------------------------------------
    [[nodiscard]] bool is_warming_up() const noexcept {
        return processed_bars < static_cast<std::uint64_t>(params.warmup_bars);
    }

    [[nodiscard]] bool in_cooldown(Timestamp now) const noexcept {
        if (!cooldown_until.is_valid()) return false;
        return now.microseconds() < cooldown_until.microseconds();
    }

    void set_cooldown(Timestamp now, int bars) noexcept {
        if (bars <= 0) return;
        const auto delta = static_cast<std::int64_t>(bars) *
                           kCandleIntervalUs;
        cooldown_until = now.saturating_add(delta);
    }

    // 通用预处理：返回是否应继续处理
    [[nodiscard]] Result<bool> preprocess(const StrategyContext& ctx) {
        // 运行检查
        if (!running.load(std::memory_order_acquire)) {
            return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
                "策略未启动");
        }

        // 上下文校验
        if (!ctx.is_valid()) {
            stats.error_count.fetch_add(1, std::memory_order_relaxed);
            return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
                "上下文无效");
        }

        // 幂等性
        {
            std::shared_lock<std::shared_mutex> lock(mutex);
            if (is_duplicate_candle(last_processed_time,
                                     ctx.candle.close_time)) {
                return false;  // 重复，跳过
            }
        }

        // 更新状态
        {
            std::unique_lock<std::shared_mutex> lock(mutex);
            last_processed_time = ctx.candle.close_time;
            ++processed_bars;
        }

        stats.on_candle_count.fetch_add(1, std::memory_order_relaxed);

        // 预热期
        if (is_warming_up()) {
            stats.warmup_skipped_count.fetch_add(
                1, std::memory_order_relaxed);
            return false;
        }

        // 冷却期
        const auto now = Timestamp::now();
        if (in_cooldown(now)) {
            return false;
        }

        return true;
    }

    // 通用信号发出
    [[nodiscard]] Result<std::optional<Signal>> emit_signal(
        const StrategyContext& ctx,
        Signal signal)
    {
        // 校验信号
        if (!signal.is_valid()) {
            stats.error_count.fetch_add(1, std::memory_order_relaxed);
            return QUANT_ERR_MSG(ErrorCode::StrategyInvalidSignal,
                "信号无效");
        }

        // 盈亏比硬校验
        const double rr = compute_rr(
            signal.entry_price.to_double(),
            signal.stop_loss.to_double(),
            signal.take_profit_1.to_double());

        if (rr < 1.5) {
            stats.signal_rejected_count.fetch_add(
                1, std::memory_order_relaxed);
            return std::optional<Signal>{};
        }

        signal.risk_reward_ratio = rr;

        // 冷却
        {
            std::unique_lock<std::shared_mutex> lock(mutex);
            set_cooldown(ctx.candle.close_time, params.cooldown_bars);
            last_signal = signal;
        }

        stats.signal_generated_count.fetch_add(
            1, std::memory_order_relaxed);

        // 回调
        if (deps.on_signal) {
            try {
                deps.on_signal(signal);
            } catch (const std::exception& e) {
                QUANT_LOG_WARN("on_signal 回调异常: {}", e.what());
            } catch (...) {
                QUANT_LOG_WARN("on_signal 回调未知异常");
            }
        }

        return std::optional<Signal>{std::move(signal)};
    }

    // 通用快照构造
    [[nodiscard]] StrategySnapshot base_snapshot(
        StrategyType type, std::uint32_t version) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);

        StrategySnapshot snap;
        snap.schema_version = 1;
        snap.strategy_type = static_cast<std::uint8_t>(type);
        snap.version = version;
        snap.last_processed_time_us = last_processed_time.microseconds();
        snap.processed_bars = processed_bars;
        return snap;
    }

    [[nodiscard]] Result<void> base_restore(
        const StrategySnapshot& snapshot, StrategyType expected_type)
    {
        if (!snapshot.is_valid()) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "快照数据无效");
        }

        if (snapshot.strategy_type != static_cast<std::uint8_t>(expected_type)) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "策略类型不匹配")
                .with_context("expected",
                    std::to_string(static_cast<int>(expected_type)))
                .with_context("got",
                    std::to_string(snapshot.strategy_type));
        }

        std::unique_lock<std::shared_mutex> lock(mutex);
        last_processed_time = Timestamp{snapshot.last_processed_time_us};
        processed_bars = snapshot.processed_bars;
        return {};
    }

    [[nodiscard]] std::string base_dump(std::string_view name,
                                          std::uint32_t version) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);

        std::ostringstream oss;
        oss << "Strategy[" << name << "] dump:\n";
        oss << "  running: "
            << (running.load(std::memory_order_acquire) ? "yes" : "no")
            << "\n";
        oss << "  version: " << version << "\n";
        oss << "  processed_bars: " << processed_bars << "\n";
        oss << "  warmup_bars: " << params.warmup_bars << "\n";
        oss << "  last_processed_time: "
            << last_processed_time.to_iso8601() << "\n";
        oss << "  cooldown_until: "
            << cooldown_until.to_iso8601() << "\n";
        oss << "  stats:\n";
        oss << "    on_candle: "
            << stats.on_candle_count.load(std::memory_order_relaxed)
            << "\n";
        oss << "    signal_generated: "
            << stats.signal_generated_count.load(
                   std::memory_order_relaxed)
            << "\n";
        oss << "    signal_rejected: "
            << stats.signal_rejected_count.load(
                   std::memory_order_relaxed)
            << "\n";
        oss << "    error_count: "
            << stats.error_count.load(std::memory_order_relaxed)
            << "\n";
        oss << "    warmup_skipped: "
            << stats.warmup_skipped_count.load(
                   std::memory_order_relaxed)
            << "\n";
        return oss.str();
    }
};

// ==============================================================================
// 1. 趋势跟踪策略实现
// ==============================================================================
struct TrendFollowingStrategy::Impl : StrategyImplBase {
    static constexpr std::uint32_t kVersion = 1;
};

TrendFollowingStrategy::TrendFollowingStrategy(StrategyDependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "TrendFollowingStrategy: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }
    impl_->deps = std::move(deps);
}

TrendFollowingStrategy::~TrendFollowingStrategy() {
    stop();
}

Result<void> TrendFollowingStrategy::start() {
    return impl_->base_start(StrategyType::TrendFollowing);
}

void TrendFollowingStrategy::stop() noexcept {
    impl_->base_stop();
}

void TrendFollowingStrategy::reset() noexcept {
    impl_->base_reset();
}

bool TrendFollowingStrategy::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

StrategyCapabilities TrendFollowingStrategy::capabilities() const noexcept {
    return capabilities::trend_following();
}

std::uint32_t TrendFollowingStrategy::version() const noexcept {
    return Impl::kVersion;
}

const StrategyStats& TrendFollowingStrategy::stats() const noexcept {
    return impl_->stats;
}

Result<std::optional<Signal>>
TrendFollowingStrategy::on_candle_close(const StrategyContext& ctx) {
    // 通用预处理
    auto pre = impl_->preprocess(ctx);
    if (pre.is_err()) {
        return pre.error();
    }
    if (!pre.value()) {
        return std::optional<Signal>{};
    }

    // 方向判定
    const double close = ctx.candle.close.to_double();
    const double ema = ctx.indicators.ema26;
    const double atr = ctx.indicators.atr14;

    if (!finite(close) || !finite(ema) || !finite(atr) || atr < kEps) {
        return std::optional<Signal>{};
    }

    const bool long_bias = close > ema && ctx.indicators.ema_slope > 0;
    const bool short_bias = close < ema && ctx.indicators.ema_slope < 0;

    if (!long_bias && !short_bias) {
        return std::optional<Signal>{};
    }

    const Side side = long_bias ? Side::BUY : Side::SELL;

    // 带宽突破
    const bool breakout_up = close > ctx.band.upper_band;
    const bool breakout_down = close < ctx.band.lower_band;

    if (side == Side::BUY && !breakout_up) {
        return std::optional<Signal>{};
    }
    if (side == Side::SELL && !breakout_down) {
        return std::optional<Signal>{};
    }

    // 评分（此处简化：使用 ema_slope + rsi + volume）
    double score = 60.0;
    if (finite(ctx.indicators.rsi7)) {
        if (side == Side::BUY &&
            ctx.indicators.rsi7 > 45.0 &&
            ctx.indicators.rsi7 < 70.0) {
            score += 10.0;
        }
        if (side == Side::SELL &&
            ctx.indicators.rsi7 > 30.0 &&
            ctx.indicators.rsi7 < 55.0) {
            score += 10.0;
        }
    }

    if (score < impl_->params.entry_threshold) {
        return std::optional<Signal>{};
    }

    // 止损止盈
    const double stop_distance = std::max(
        impl_->params.stop_atr * atr,
        0.5 * atr);

    const double stop = is_long(side)
        ? close - stop_distance
        : close + stop_distance;

    const double tp1 = is_long(side)
        ? close + impl_->params.target_r * stop_distance
        : close - impl_->params.target_r * stop_distance;

    const double tp2 = is_long(side)
        ? close + (impl_->params.target_r + 1.0) * stop_distance
        : close - (impl_->params.target_r + 1.0) * stop_distance;

    // 构造信号
    Signal signal;
    signal.side = side;
    signal.entry_price = Price::from_double(close);
    signal.stop_loss = Price::from_double(stop);
    signal.take_profit_1 = Price::from_double(tp1);
    signal.take_profit_2 = Price::from_double(tp2);
    signal.score = score;
    signal.confidence = clamp_val(score / 100.0, 0.0, 1.0);
    signal.timestamp = ctx.candle.close_time;

    return impl_->emit_signal(ctx, std::move(signal));
}

std::string TrendFollowingStrategy::dump() const {
    return impl_->base_dump(name(), Impl::kVersion);
}

StrategySnapshot TrendFollowingStrategy::to_snapshot() const {
    return impl_->base_snapshot(StrategyType::TrendFollowing,
                                  Impl::kVersion);
}

Result<void> TrendFollowingStrategy::from_snapshot(
    const StrategySnapshot& snapshot)
{
    return impl_->base_restore(snapshot, StrategyType::TrendFollowing);
}

// ==============================================================================
// 2. 反转策略实现
// ==============================================================================
struct ReversalStrategy::Impl : StrategyImplBase {
    static constexpr std::uint32_t kVersion = 1;
};

ReversalStrategy::ReversalStrategy(StrategyDependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "ReversalStrategy: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }
    impl_->deps = std::move(deps);
}

ReversalStrategy::~ReversalStrategy() {
    stop();
}

Result<void> ReversalStrategy::start() {
    return impl_->base_start(StrategyType::Reversal);
}

void ReversalStrategy::stop() noexcept {
    impl_->base_stop();
}

void ReversalStrategy::reset() noexcept {
    impl_->base_reset();
}

bool ReversalStrategy::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

StrategyCapabilities ReversalStrategy::capabilities() const noexcept {
    return capabilities::reversal();
}

std::uint32_t ReversalStrategy::version() const noexcept {
    return Impl::kVersion;
}

const StrategyStats& ReversalStrategy::stats() const noexcept {
    return impl_->stats;
}

Result<std::optional<Signal>>
ReversalStrategy::on_candle_close(const StrategyContext& ctx) {
    auto pre = impl_->preprocess(ctx);
    if (pre.is_err()) return pre.error();
    if (!pre.value()) return std::optional<Signal>{};

    const double close = ctx.candle.close.to_double();
    const double atr = ctx.indicators.atr14;
    const double rsi = ctx.indicators.rsi7;

    if (!finite(close) || !finite(atr) || atr < kEps ||
        !finite(rsi)) {
        return std::optional<Signal>{};
    }

    // 反转信号：极端 RSI
    const bool oversold = rsi < impl_->params.reversal_rsi_oversold;
    const bool overbought = rsi > impl_->params.reversal_rsi_overbought;

    if (!oversold && !overbought) {
        return std::optional<Signal>{};
    }

    // 反向做单：超卖做多，超买做空
    const Side side = oversold ? Side::BUY : Side::SELL;

    double score = 65.0;
    if (oversold && rsi < 20.0) score += 10.0;
    if (overbought && rsi > 80.0) score += 10.0;

    if (score < impl_->params.entry_threshold) {
        return std::optional<Signal>{};
    }

    const double stop_distance = impl_->params.stop_atr * atr;
    const double stop = is_long(side)
        ? close - stop_distance
        : close + stop_distance;

    const double tp1 = is_long(side)
        ? close + impl_->params.target_r * stop_distance
        : close - impl_->params.target_r * stop_distance;

    const double tp2 = is_long(side)
        ? close + (impl_->params.target_r + 0.5) * stop_distance
        : close - (impl_->params.target_r + 0.5) * stop_distance;

    Signal signal;
    signal.side = side;
    signal.entry_price = Price::from_double(close);
    signal.stop_loss = Price::from_double(stop);
    signal.take_profit_1 = Price::from_double(tp1);
    signal.take_profit_2 = Price::from_double(tp2);
    signal.score = score;
    signal.confidence = clamp_val(score / 100.0, 0.0, 1.0);
    signal.timestamp = ctx.candle.close_time;

    return impl_->emit_signal(ctx, std::move(signal));
}

std::string ReversalStrategy::dump() const {
    return impl_->base_dump(name(), Impl::kVersion);
}

StrategySnapshot ReversalStrategy::to_snapshot() const {
    return impl_->base_snapshot(StrategyType::Reversal, Impl::kVersion);
}

Result<void> ReversalStrategy::from_snapshot(
    const StrategySnapshot& snapshot)
{
    return impl_->base_restore(snapshot, StrategyType::Reversal);
}

// ==============================================================================
// 3. 区间震荡策略实现
// ==============================================================================
struct RangeStrategy::Impl : StrategyImplBase {
    static constexpr std::uint32_t kVersion = 1;

    // 区间高低点
    std::deque<double> highs;
    std::deque<double> lows;
};

RangeStrategy::RangeStrategy(StrategyDependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "RangeStrategy: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }
    impl_->deps = std::move(deps);
}

RangeStrategy::~RangeStrategy() {
    stop();
}

Result<void> RangeStrategy::start() {
    auto r = impl_->base_start(StrategyType::Range);
    if (r.is_err()) return r;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->highs.clear();
    impl_->lows.clear();
    return {};
}

void RangeStrategy::stop() noexcept {
    impl_->base_stop();
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->highs.clear();
    impl_->lows.clear();
}

void RangeStrategy::reset() noexcept {
    impl_->base_reset();
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->highs.clear();
    impl_->lows.clear();
}

bool RangeStrategy::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

StrategyCapabilities RangeStrategy::capabilities() const noexcept {
    return capabilities::range();
}

std::uint32_t RangeStrategy::version() const noexcept {
    return Impl::kVersion;
}

const StrategyStats& RangeStrategy::stats() const noexcept {
    return impl_->stats;
}

Result<std::optional<Signal>>
RangeStrategy::on_candle_close(const StrategyContext& ctx) {
    auto pre = impl_->preprocess(ctx);
    if (pre.is_err()) return pre.error();
    if (!pre.value()) return std::optional<Signal>{};

    const double close = ctx.candle.close.to_double();
    const double high = ctx.candle.high.to_double();
    const double low = ctx.candle.low.to_double();
    const double atr = ctx.indicators.atr14;

    if (!finite(close) || !finite(high) || !finite(low) ||
        !finite(atr) || atr < kEps) {
        return std::optional<Signal>{};
    }

    // 更新高低点窗口
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->highs.push_back(high);
        impl_->lows.push_back(low);

        const std::size_t max_size =
            static_cast<std::size_t>(impl_->params.range_lookback);
        while (impl_->highs.size() > max_size) {
            impl_->highs.pop_front();
        }
        while (impl_->lows.size() > max_size) {
            impl_->lows.pop_front();
        }

        // 数据不足
        if (impl_->highs.size() < max_size) {
            return std::optional<Signal>{};
        }

        // 计算区间
        const double range_high = *std::max_element(
            impl_->highs.begin(), impl_->highs.end());
        const double range_low = *std::min_element(
            impl_->lows.begin(), impl_->lows.end());

        const double range_height = range_high - range_low;
        if (range_height < impl_->params.range_min_height_atr * atr) {
            return std::optional<Signal>{};
        }

        // 判断是否在上下沿
        const double position = (close - range_low) / range_height;

        Side side = Side::UNKNOWN;
        if (position < 0.15) {
            side = Side::BUY;   // 接近下沿做多
        } else if (position > 0.85) {
            side = Side::SELL;  // 接近上沿做空
        } else {
            return std::optional<Signal>{};
        }

        // 构造信号
        const double stop_distance = impl_->params.stop_atr * atr;
        const double stop = is_long(side)
            ? range_low - 0.3 * atr
            : range_high + 0.3 * atr;

        const double tp1 = is_long(side)
            ? range_low + range_height * 0.5
            : range_high - range_height * 0.5;

        const double tp2 = is_long(side)
            ? range_high - 0.1 * atr
            : range_low + 0.1 * atr;

        Signal signal;
        signal.side = side;
        signal.entry_price = Price::from_double(close);
        signal.stop_loss = Price::from_double(stop);
        signal.take_profit_1 = Price::from_double(tp1);
        signal.take_profit_2 = Price::from_double(tp2);
        signal.score = 65.0;
        signal.confidence = 0.65;
        signal.timestamp = ctx.candle.close_time;

        (void)stop_distance;
        return impl_->emit_signal(ctx, std::move(signal));
    }
}

std::string RangeStrategy::dump() const {
    return impl_->base_dump(name(), Impl::kVersion);
}

StrategySnapshot RangeStrategy::to_snapshot() const {
    return impl_->base_snapshot(StrategyType::Range, Impl::kVersion);
}

Result<void> RangeStrategy::from_snapshot(
    const StrategySnapshot& snapshot)
{
    return impl_->base_restore(snapshot, StrategyType::Range);
}

// ==============================================================================
// 4. 突破策略实现
// ==============================================================================
struct BreakoutStrategy::Impl : StrategyImplBase {
    static constexpr std::uint32_t kVersion = 1;

    std::deque<double> highs;
    std::deque<double> lows;
};

BreakoutStrategy::BreakoutStrategy(StrategyDependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "BreakoutStrategy: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }
    impl_->deps = std::move(deps);
}

BreakoutStrategy::~BreakoutStrategy() {
    stop();
}

Result<void> BreakoutStrategy::start() {
    auto r = impl_->base_start(StrategyType::Breakout);
    if (r.is_err()) return r;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->highs.clear();
    impl_->lows.clear();
    return {};
}

void BreakoutStrategy::stop() noexcept {
    impl_->base_stop();
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->highs.clear();
    impl_->lows.clear();
}

void BreakoutStrategy::reset() noexcept {
    impl_->base_reset();
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->highs.clear();
    impl_->lows.clear();
}

bool BreakoutStrategy::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

StrategyCapabilities BreakoutStrategy::capabilities() const noexcept {
    return capabilities::breakout();
}

std::uint32_t BreakoutStrategy::version() const noexcept {
    return Impl::kVersion;
}

const StrategyStats& BreakoutStrategy::stats() const noexcept {
    return impl_->stats;
}

Result<std::optional<Signal>>
BreakoutStrategy::on_candle_close(const StrategyContext& ctx) {
    auto pre = impl_->preprocess(ctx);
    if (pre.is_err()) return pre.error();
    if (!pre.value()) return std::optional<Signal>{};

    const double close = ctx.candle.close.to_double();
    const double high = ctx.candle.high.to_double();
    const double low = ctx.candle.low.to_double();
    const double atr = ctx.indicators.atr14;
    const double vol = ctx.candle.volume.to_double();
    const double vol_ma = ctx.indicators.volume_ma20;

    if (!finite(close) || !finite(high) || !finite(low) ||
        !finite(atr) || atr < kEps) {
        return std::optional<Signal>{};
    }

    // 更新通道
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);

        const std::size_t period =
            static_cast<std::size_t>(impl_->params.breakout_channel_period);

        // 用历史数据计算通道（不含当前 K 线）
        if (impl_->highs.size() >= period) {
            const double channel_high = *std::max_element(
                impl_->highs.end() - static_cast<std::ptrdiff_t>(period),
                impl_->highs.end());
            const double channel_low = *std::min_element(
                impl_->lows.end() - static_cast<std::ptrdiff_t>(period),
                impl_->lows.end());

            // 突破判定
            const bool breakout_up =
                close > channel_high +
                    impl_->params.breakout_confirm_atr * atr;
            const bool breakout_down =
                close < channel_low -
                    impl_->params.breakout_confirm_atr * atr;

            // 量能确认
            const bool volume_ok =
                !finite(vol_ma) || vol_ma < kEps ||
                (vol / vol_ma) >= impl_->params.breakout_volume_ratio;

            if ((breakout_up || breakout_down) && volume_ok) {
                const Side side = breakout_up ? Side::BUY : Side::SELL;
                const double stop_distance = impl_->params.stop_atr * atr;

                const double stop = is_long(side)
                    ? close - stop_distance
                    : close + stop_distance;

                const double tp1 = is_long(side)
                    ? close + impl_->params.target_r * stop_distance
                    : close - impl_->params.target_r * stop_distance;

                const double tp2 = is_long(side)
                    ? close + (impl_->params.target_r + 1.5) * stop_distance
                    : close - (impl_->params.target_r + 1.5) * stop_distance;

                Signal signal;
                signal.side = side;
                signal.entry_price = Price::from_double(close);
                signal.stop_loss = Price::from_double(stop);
                signal.take_profit_1 = Price::from_double(tp1);
                signal.take_profit_2 = Price::from_double(tp2);
                signal.score = 70.0;
                signal.confidence = 0.70;
                signal.timestamp = ctx.candle.close_time;

                // 更新窗口
                impl_->highs.push_back(high);
                impl_->lows.push_back(low);
                while (impl_->highs.size() > period) {
                    impl_->highs.pop_front();
                }
                while (impl_->lows.size() > period) {
                    impl_->lows.pop_front();
                }

                return impl_->emit_signal(ctx, std::move(signal));
            }
        }

        // 更新窗口（无论是否触发）
        impl_->highs.push_back(high);
        impl_->lows.push_back(low);
        while (impl_->highs.size() > period) {
            impl_->highs.pop_front();
        }
        while (impl_->lows.size() > period) {
            impl_->lows.pop_front();
        }
    }

    return std::optional<Signal>{};
}

std::string BreakoutStrategy::dump() const {
    return impl_->base_dump(name(), Impl::kVersion);
}

StrategySnapshot BreakoutStrategy::to_snapshot() const {
    return impl_->base_snapshot(StrategyType::Breakout, Impl::kVersion);
}

Result<void> BreakoutStrategy::from_snapshot(
    const StrategySnapshot& snapshot)
{
    return impl_->base_restore(snapshot, StrategyType::Breakout);
}

// ==============================================================================
// 5. AI 自适应策略实现
// ==============================================================================
struct AIAdaptiveStrategy::Impl : StrategyImplBase {
    static constexpr std::uint32_t kVersion = 1;

    // 内部委托给趋势和区间策略
    double last_trend_strength{0.0};
};

AIAdaptiveStrategy::AIAdaptiveStrategy(StrategyDependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "AIAdaptiveStrategy: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }
    impl_->deps = std::move(deps);
}

AIAdaptiveStrategy::~AIAdaptiveStrategy() {
    stop();
}

Result<void> AIAdaptiveStrategy::start() {
    return impl_->base_start(StrategyType::AIAdaptive);
}

void AIAdaptiveStrategy::stop() noexcept {
    impl_->base_stop();
}

void AIAdaptiveStrategy::reset() noexcept {
    impl_->base_reset();
}

bool AIAdaptiveStrategy::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

StrategyCapabilities AIAdaptiveStrategy::capabilities() const noexcept {
    return capabilities::ai_adaptive();
}

std::uint32_t AIAdaptiveStrategy::version() const noexcept {
    return Impl::kVersion;
}

const StrategyStats& AIAdaptiveStrategy::stats() const noexcept {
    return impl_->stats;
}

Result<std::optional<Signal>>
AIAdaptiveStrategy::on_candle_close(const StrategyContext& ctx) {
    auto pre = impl_->preprocess(ctx);
    if (pre.is_err()) return pre.error();
    if (!pre.value()) return std::optional<Signal>{};

    const double close = ctx.candle.close.to_double();
    const double ema = ctx.indicators.ema26;
    const double atr = ctx.indicators.atr14;
    const double rsi = ctx.indicators.rsi7;

    if (!finite(close) || !finite(ema) || !finite(atr) || atr < kEps) {
        return std::optional<Signal>{};
    }

    // AI 决策融合
    double ai_prob = 0.5;
    if (ctx.ai.valid) {
        ai_prob = clamp_val(ctx.ai.trend_probability, 0.0, 1.0);
    }

    // 市场状态权重
    double trend_weight = 1.0;
    if (ctx.regime == MarketRegime::TrendUp ||
        ctx.regime == MarketRegime::TrendDown) {
        trend_weight = 1.2;
    } else if (ctx.regime == MarketRegime::Range) {
        trend_weight = 0.8;
    }

    // 综合评分
    double score = 55.0;
    Side side = Side::UNKNOWN;

    if (close > ema && ctx.indicators.ema_slope > 0) {
        if (ai_prob > impl_->params.ai_confidence_threshold) {
            score += 15.0 * trend_weight;
            side = Side::BUY;
        }
    } else if (close < ema && ctx.indicators.ema_slope < 0) {
        if (ai_prob < 1.0 - impl_->params.ai_confidence_threshold) {
            score += 15.0 * trend_weight;
            side = Side::SELL;
        }
    }

    // RSI 辅助
    if (valid_side(side) && finite(rsi)) {
        if (side == Side::BUY && rsi > 45.0 && rsi < 70.0) {
            score += 5.0;
        }
        if (side == Side::SELL && rsi > 30.0 && rsi < 55.0) {
            score += 5.0;
        }
    }

    if (!valid_side(side) || score < impl_->params.entry_threshold) {
        return std::optional<Signal>{};
    }

    const double stop_distance = impl_->params.stop_atr * atr;
    const double stop = is_long(side)
        ? close - stop_distance
        : close + stop_distance;

    const double tp1 = is_long(side)
        ? close + impl_->params.target_r * stop_distance
        : close - impl_->params.target_r * stop_distance;

    const double tp2 = is_long(side)
        ? close + (impl_->params.target_r + 1.0) * stop_distance
        : close - (impl_->params.target_r + 1.0) * stop_distance;

    Signal signal;
    signal.side = side;
    signal.entry_price = Price::from_double(close);
    signal.stop_loss = Price::from_double(stop);
    signal.take_profit_1 = Price::from_double(tp1);
    signal.take_profit_2 = Price::from_double(tp2);
    signal.score = score;
    signal.confidence = clamp_val(score / 100.0, 0.0, 1.0);
    signal.timestamp = ctx.candle.close_time;

    impl_->last_trend_strength = ai_prob;

    return impl_->emit_signal(ctx, std::move(signal));
}

std::string AIAdaptiveStrategy::dump() const {
    return impl_->base_dump(name(), Impl::kVersion);
}

StrategySnapshot AIAdaptiveStrategy::to_snapshot() const {
    return impl_->base_snapshot(StrategyType::AIAdaptive, Impl::kVersion);
}

Result<void> AIAdaptiveStrategy::from_snapshot(
    const StrategySnapshot& snapshot)
{
    return impl_->base_restore(snapshot, StrategyType::AIAdaptive);
}

// ==============================================================================
// StrategyFactory 实现
// ==============================================================================
Result<std::unique_ptr<IStrategy>> StrategyFactory::create(
    StrategyType type,
    StrategyDependencies deps)
{
    try {
        switch (type) {
            case StrategyType::TrendFollowing:
                return std::unique_ptr<IStrategy>{
                    std::make_unique<TrendFollowingStrategy>(
                        std::move(deps))};

            case StrategyType::Reversal:
                return std::unique_ptr<IStrategy>{
                    std::make_unique<ReversalStrategy>(std::move(deps))};

            case StrategyType::Range:
                return std::unique_ptr<IStrategy>{
                    std::make_unique<RangeStrategy>(std::move(deps))};

            case StrategyType::Breakout:
                return std::unique_ptr<IStrategy>{
                    std::make_unique<BreakoutStrategy>(std::move(deps))};

            case StrategyType::AIAdaptive:
                return std::unique_ptr<IStrategy>{
                    std::make_unique<AIAdaptiveStrategy>(std::move(deps))};

            case StrategyType::Unknown:
            default:
                return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
                    "未知策略类型")
                    .with_context("type",
                        std::to_string(static_cast<int>(type)));
        }
    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
            "策略创建失败")
            .with_context("error", e.what());
    }
}

Result<std::unique_ptr<IStrategy>> StrategyFactory::create(
    std::string_view type_name,
    StrategyDependencies deps)
{
    const auto type = strategy_type_from_string(type_name);
    if (type == StrategyType::Unknown) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "未知策略名")
            .with_context("name", std::string{type_name});
    }
    return create(type, std::move(deps));
}

Result<std::unique_ptr<IStrategy>> StrategyFactory::create_default(
    StrategyDependencies deps)
{
    return create(StrategyType::TrendFollowing, std::move(deps));
}

std::vector<StrategyType> StrategyFactory::available_types() {
    return {
        StrategyType::TrendFollowing,
        StrategyType::Reversal,
        StrategyType::Range,
        StrategyType::Breakout,
        StrategyType::AIAdaptive,
    };
}

StrategyType StrategyFactory::recommend(MarketRegime regime) noexcept {
    switch (regime) {
        case MarketRegime::TrendUp:
        case MarketRegime::TrendDown:
            return StrategyType::TrendFollowing;

        case MarketRegime::Range:
            return StrategyType::Range;

        case MarketRegime::HighVol:
            return StrategyType::Breakout;

        case MarketRegime::LowVol:
            return StrategyType::Range;

        case MarketRegime::Unknown:
        default:
            return StrategyType::TrendFollowing;
    }
}

// ==============================================================================
// StrategyRegistry 实现
// ==============================================================================
struct StrategyRegistry::Impl {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::uint8_t, Creator> by_type;
    std::unordered_map<std::string, Creator> by_name;
    std::unordered_map<std::uint8_t, std::string> type_names;
};

StrategyRegistry& StrategyRegistry::instance() noexcept {
    static StrategyRegistry inst;
    return inst;
}

StrategyRegistry::StrategyRegistry()
    : impl_(std::make_unique<Impl>())
{
    // 注册内置策略
    register_strategy(
        StrategyType::TrendFollowing,
        "trend_following",
        [](StrategyDependencies deps)
            -> Result<std::unique_ptr<IStrategy>> {
            return StrategyFactory::create(
                StrategyType::TrendFollowing, std::move(deps));
        });

    register_strategy(
        StrategyType::Reversal,
        "reversal",
        [](StrategyDependencies deps)
            -> Result<std::unique_ptr<IStrategy>> {
            return StrategyFactory::create(
                StrategyType::Reversal, std::move(deps));
        });

    register_strategy(
        StrategyType::Range,
        "range",
        [](StrategyDependencies deps)
            -> Result<std::unique_ptr<IStrategy>> {
            return StrategyFactory::create(
                StrategyType::Range, std::move(deps));
        });

    register_strategy(
        StrategyType::Breakout,
        "breakout",
        [](StrategyDependencies deps)
            -> Result<std::unique_ptr<IStrategy>> {
            return StrategyFactory::create(
                StrategyType::Breakout, std::move(deps));
        });

    register_strategy(
        StrategyType::AIAdaptive,
        "ai_adaptive",
        [](StrategyDependencies deps)
            -> Result<std::unique_ptr<IStrategy>> {
            return StrategyFactory::create(
                StrategyType::AIAdaptive, std::move(deps));
        });
}

Result<void> StrategyRegistry::register_strategy(
    StrategyType type,
    std::string_view name,
    Creator creator)
{
    if (type == StrategyType::Unknown) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "策略类型不能为 Unknown");
    }
    if (name.empty()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "策略名不能为空");
    }
    if (!creator) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "creator 不能为空");
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    const auto key = static_cast<std::uint8_t>(type);

    // 覆盖是允许的（用于替换内置实现），但记录日志
    if (impl_->by_type.count(key)) {
        QUANT_LOG_WARN("策略类型 {} 已注册，将覆盖",
                        static_cast<int>(key));
    }

    impl_->by_type[key] = creator;
    impl_->by_name[std::string{name}] = creator;
    impl_->type_names[key] = std::string{name};

    return {};
}

bool StrategyRegistry::has(StrategyType type) const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->by_type.count(static_cast<std::uint8_t>(type)) > 0;
}

bool StrategyRegistry::has(std::string_view name) const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->by_name.count(std::string{name}) > 0;
}

Result<std::unique_ptr<IStrategy>> StrategyRegistry::create(
    StrategyType type,
    StrategyDependencies deps) const
{
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    auto it = impl_->by_type.find(static_cast<std::uint8_t>(type));
    if (it == impl_->by_type.end()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "策略类型未注册")
            .with_context("type",
                std::to_string(static_cast<int>(type)));
    }
    auto creator = it->second;
    lock.unlock();

    return creator(std::move(deps));
}

Result<std::unique_ptr<IStrategy>> StrategyRegistry::create(
    std::string_view name,
    StrategyDependencies deps) const
{
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    auto it = impl_->by_name.find(std::string{name});
    if (it == impl_->by_name.end()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "策略名未注册")
            .with_context("name", std::string{name});
    }
    auto creator = it->second;
    lock.unlock();

    return creator(std::move(deps));
}

std::vector<StrategyType> StrategyRegistry::list() const {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    std::vector<StrategyType> result;
    result.reserve(impl_->by_type.size());
    for (const auto& [key, _] : impl_->by_type) {
        result.push_back(static_cast<StrategyType>(key));
    }
    return result;
}

std::vector<std::string> StrategyRegistry::list_names() const {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    std::vector<std::string> result;
    result.reserve(impl_->by_name.size());
    for (const auto& [name, _] : impl_->by_name) {
        result.push_back(name);
    }
    return result;
}

// ==============================================================================
// StrategyManager 实现
// ==============================================================================
struct StrategyManager::Impl {
    Dependencies deps;
    std::atomic<bool> running{false};

    mutable std::shared_mutex mutex;
    std::unique_ptr<IStrategy> current;
    StrategyType current_type{StrategyType::Unknown};

    mutable StrategyStats empty_stats{};

    [[nodiscard]] Result<void> create_strategy(StrategyType type) {
        StrategyDependencies sdeps;
        sdeps.params_provider = deps.params_provider;
        sdeps.on_signal = deps.on_signal;

        auto result = StrategyFactory::create(type, std::move(sdeps));
        if (result.is_err()) return result.error();

        auto new_strategy = std::move(result.value());
        if (auto r = new_strategy->start(); r.is_err()) {
            return r;
        }

        std::unique_lock<std::shared_mutex> lock(mutex);
        auto old_strategy = std::move(current);
        auto old_type = current_type;

        current = std::move(new_strategy);
        current_type = type;

        lock.unlock();

        // 停止旧策略
        if (old_strategy) {
            old_strategy->stop();
        }

        // 通知
        if (deps.on_switch) {
            try {
                deps.on_switch(old_type, type);
            } catch (...) {
                QUANT_LOG_WARN("on_switch 回调异常");
            }
        }

        QUANT_LOG_INFO("策略切换: {} -> {}",
                       to_string(old_type), to_string(type));

        return {};
    }
};

StrategyManager::StrategyManager(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "StrategyManager: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }
    impl_->deps = std::move(deps);
}

StrategyManager::~StrategyManager() {
    stop();
}

Result<void> StrategyManager::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "StrategyManager 已启动");
    }

    // 创建默认策略
    if (auto r = impl_->create_strategy(
            StrategyType::TrendFollowing); r.is_err()) {
        impl_->running.store(false, std::memory_order_release);
        return r;
    }

    QUANT_LOG_INFO("StrategyManager 已启动");
    return {};
}

void StrategyManager::stop() noexcept {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    if (impl_->current) {
        impl_->current->stop();
        impl_->current.reset();
    }
    impl_->current_type = StrategyType::Unknown;

    QUANT_LOG_INFO("StrategyManager 已停止");
}

void StrategyManager::reset() noexcept {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    if (impl_->current) {
        impl_->current->reset();
    }
}

bool StrategyManager::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

Result<void> StrategyManager::switch_strategy(StrategyType type) {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "StrategyManager 未启动");
    }

    if (type == StrategyType::Unknown) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "未知策略类型");
    }

    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);
        if (impl_->current_type == type) {
            return {};  // 已是当前策略
        }
    }

    return impl_->create_strategy(type);
}

StrategyType StrategyManager::current_type() const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->current_type;
}

Result<std::optional<Signal>> StrategyManager::on_candle_close(
    const StrategyContext& ctx)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "StrategyManager 未启动");
    }

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    if (!impl_->current) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "无当前策略");
    }
    auto* strategy = impl_->current.get();
    lock.unlock();

    return strategy->on_candle_close(ctx);
}

const StrategyStats& StrategyManager::stats() const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    if (impl_->current) {
        return impl_->current->stats();
    }
    return impl_->empty_stats;
}

std::string StrategyManager::dump() const {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    std::ostringstream oss;
    oss << "StrategyManager dump:\n";
    oss << "  running: " << (is_running() ? "yes" : "no") << "\n";
    oss << "  current_type: " << to_string(impl_->current_type) << "\n";
    if (impl_->current) {
        oss << "  strategy:\n" << impl_->current->dump();
    }
    return oss.str();
}

}  // namespace quant::strategy
