// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 策略参数映射实现
// ==============================================================================
// @file    src/strategy/strategy.core.config_mapper.cpp
// @module  strategy
// @type    core
// @name    config_mapper
// @version 1.0.1
// @brief   参数加载、校验、热更新、历史记录、元信息
//          已修复 30 类运行时问题
//
// 加载流程:
//   1. 检查运行状态
//   2. 从 ConfigManager 获取快照
//   3. 逐参数读取（带默认值）
//   4. 范围校验
//   5. 联动校验
//   6. 构造 StrategyParams
//   7. 原子交换
//   8. 递增版本
//   9. 记录历史
//  10. 触发回调
// ==============================================================================

#include "strategy/strategy.core.config_mapper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"

namespace quant::strategy {

// ==============================================================================
// 匿名命名空间：辅助函数
// ==============================================================================
namespace {

constexpr double kEps = config_mapper_config::kEpsilon;

// -----------------------------------------------------------------------------
// 浮点比较
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool approx_equal(double a, double b) noexcept {
    return std::abs(a - b) < kEps;
}

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
// 判断参数是否在范围内
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool in_range(double v, double lo, double hi) noexcept {
    return v >= lo - kEps && v <= hi + kEps;
}

// -----------------------------------------------------------------------------
// 将 double 转为可读字符串
// -----------------------------------------------------------------------------
[[nodiscard]] std::string to_str(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

[[nodiscard]] std::string to_str(int v) {
    return std::to_string(v);
}

// -----------------------------------------------------------------------------
// 已知策略类型
// -----------------------------------------------------------------------------
inline constexpr std::array<std::string_view, 5> kKnownStrategyTypes = {
    "trend_following",
    "reversal",
    "range",
    "breakout",
    "ai_adaptive",
};

[[nodiscard]] bool is_known_strategy_type(std::string_view t) noexcept {
    for (auto k : kKnownStrategyTypes) {
        if (k == t) return true;
    }
    return false;
}

}  // namespace

// ==============================================================================
// StrategyParams::validate
// ==============================================================================
Result<void> StrategyParams::validate() const noexcept {
    // ---------- 趋势过滤 ----------
    if (ema_period < 5 || ema_period > 200) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ema_period 超出范围 [5, 200]")
            .with_context("value", std::to_string(ema_period));
    }
    if (ema_fast_period < 3 || ema_fast_period > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ema_fast_period 超出范围 [3, 100]");
    }
    if (ema_fast_period >= ema_period) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ema_fast_period 必须 < ema_period");
    }
    if (ema_slope_period < 1 || ema_slope_period > 20) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "ema_slope_period 超出范围 [1, 20]");
    }

    // ---------- 带宽 ----------
    if (band_k < 0.3 || band_k > 3.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "band_k 超出范围 [0.3, 3.0]");
    }
    if (band_k_min < 0.1 || band_k_min > band_k) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "band_k_min 无效或 > band_k");
    }
    if (band_k_max < band_k || band_k_max > 5.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "band_k_max 无效或 < band_k");
    }

    // ---------- 评分 ----------
    if (entry_threshold < 0.0 || entry_threshold > 100.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "entry_threshold 超出范围 [0, 100]");
    }
    if (reverse_threshold < entry_threshold ||
        reverse_threshold > 100.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "reverse_threshold 必须 ≥ entry_threshold 且 ≤ 100");
    }
    if (neutral_range < 0.0 || neutral_range >= entry_threshold) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "neutral_range 必须 < entry_threshold");
    }

    // ---------- 止损止盈 ----------
    if (stop_atr < 0.3 || stop_atr > 5.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "stop_atr 超出范围 [0.3, 5.0]");
    }
    if (min_stop_atr < 0.1 || min_stop_atr > stop_atr) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_stop_atr 无效或 > stop_atr");
    }
    if (target_r < 1.0 || target_r > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "target_r 超出范围 [1.0, 10.0]");
    }
    if (tp1_r < 1.0 || tp1_r > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "tp1_r 超出范围 [1.0, 10.0]");
    }
    if (tp2_r <= tp1_r || tp2_r > 20.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "tp2_r 必须 > tp1_r 且 ≤ 20");
    }
    if (tp1_fraction < 0.0 || tp1_fraction > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "tp1_fraction 超出范围 [0, 1]");
    }
    if (tp2_fraction < 0.0 || tp2_fraction > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "tp2_fraction 超出范围 [0, 1]");
    }
    if (tp1_fraction + tp2_fraction > 1.0 + kEps) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "止盈比例之和不能超过 1.0");
    }

    // ---------- 逃顶 ----------
    if (escape_atr < 1.5 || escape_atr > 10.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "escape_atr 超出范围 [1.5, 10.0]");
    }
    if (escape_confirm_bars < 1 || escape_confirm_bars > 10) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "escape_confirm_bars 超出范围 [1, 10]");
    }

    // ---------- 加仓 ----------
    if (max_add_count < 0 || max_add_count > 5) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_add_count 超出范围 [0, 5]");
    }
    if (add_scale_1 < 0.0 || add_scale_1 > 2.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "add_scale_1 超出范围 [0, 2.0]");
    }
    if (add_scale_2 < 0.0 || add_scale_2 > 2.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "add_scale_2 超出范围 [0, 2.0]");
    }
    if (add_scale_3 < 0.0 || add_scale_3 > 2.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "add_scale_3 超出范围 [0, 2.0]");
    }
    if (add_trend_min < 0.0 || add_trend_min > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "add_trend_min 超出范围 [0, 1]");
    }
    if (add_cooldown_bars < 0 || add_cooldown_bars > 50) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "add_cooldown_bars 超出范围 [0, 50]");
    }

    // ---------- 时间止损 ----------
    if (time_stop_bars < 1 || time_stop_bars > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "time_stop_bars 超出范围 [1, 100]");
    }
    if (time_stop_min_r < 0.0 || time_stop_min_r > 5.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "time_stop_min_r 超出范围 [0, 5.0]");
    }

    // ---------- 冷却 ----------
    if (cooldown_bars < 0 || cooldown_bars > 50) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "cooldown_bars 超出范围 [0, 50]");
    }

    // ---------- 风险 ----------
    if (risk_per_trade < 0.001 || risk_per_trade > 0.10) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "risk_per_trade 超出范围 [0.001, 0.10]");
    }
    if (reverse_risk < 0.001 || reverse_risk > risk_per_trade) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "reverse_risk 无效或 > risk_per_trade");
    }
    if (max_daily_loss < 0.01 || max_daily_loss > 0.50) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_daily_loss 超出范围 [0.01, 0.50]");
    }
    if (risk_per_trade > max_daily_loss / 2.0 + kEps) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "risk_per_trade 应 ≤ max_daily_loss / 2");
    }

    // ---------- 策略类型 ----------
    if (strategy_type.empty() ||
        !is_known_strategy_type(strategy_type)) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "strategy_type 未知")
            .with_context("type", strategy_type);
    }

    return {};
}

// ==============================================================================
// 参数元信息表（编译期常量）
// ==============================================================================
namespace {

using MetaTable = std::array<ParamMeta, config_mapper_config::kParamCount>;

[[nodiscard]] const MetaTable& metadata_table() noexcept {
    static const MetaTable kTable = {{
        // ---------- 趋势 ----------
        { config_keys::kEmaPeriod, "趋势均线周期",
          "判断趋势方向的均线长度，值越大越滞后但越稳定",
          "根", 26, 5, 200, 20, 30, true, "trend" },

        { config_keys::kEmaFastPeriod, "快速均线周期",
          "短期动能均线，用于金叉死叉判断",
          "根", 12, 3, 100, 8, 16, true, "trend" },

        { config_keys::kEmaSlopePeriod, "斜率计算周期",
          "计算 EMA 斜率时的回看 K 线数",
          "根", 5, 1, 20, 3, 8, true, "trend" },

        // ---------- 带宽 ----------
        { config_keys::kBandK, "突破灵敏度",
          "带宽系数，值越小越早发现突破但假信号多",
          "", 1.0, 0.3, 3.0, 0.8, 1.3, false, "band" },

        { config_keys::kBandKMin, "带宽下限",
          "自适应带宽的系数下限",
          "", 0.5, 0.1, 3.0, 0.4, 0.7, false, "band" },

        { config_keys::kBandKMax, "带宽上限",
          "自适应带宽的系数上限",
          "", 2.0, 0.3, 5.0, 1.8, 2.5, false, "band" },

        // ---------- 评分 ----------
        { config_keys::kEntryThreshold, "进场阈值",
          "顺势进场所需的最低评分",
          "分", 60, 0, 100, 55, 70, false, "scoring" },

        { config_keys::kReverseThreshold, "反向阈值",
          "反向做单所需的最低评分",
          "分", 80, 0, 100, 75, 90, false, "scoring" },

        { config_keys::kNeutralRange, "中性区间",
          "评分绝对值低于此值时视为中性",
          "分", 30, 0, 100, 20, 40, false, "scoring" },

        // ---------- 止损止盈 ----------
        { config_keys::kStopAtr, "止损宽松度",
          "止损距离 = ATR × 此倍数",
          "ATR", 1.5, 0.3, 5.0, 1.2, 2.0, false, "stoploss" },

        { config_keys::kMinStopAtr, "最小止损",
          "止损距离的下限",
          "ATR", 0.5, 0.1, 5.0, 0.4, 0.8, false, "stoploss" },

        { config_keys::kTargetR, "盈亏比目标",
          "第一止盈目标的 R 倍数",
          "R", 2.0, 1.0, 10.0, 1.5, 3.0, false, "takeprofit" },

        { config_keys::kTp1R, "止盈1目标",
          "第一止盈 R 倍数",
          "R", 2.0, 1.0, 10.0, 1.8, 2.5, false, "takeprofit" },

        { config_keys::kTp2R, "止盈2目标",
          "第二止盈 R 倍数",
          "R", 3.0, 1.5, 20.0, 2.5, 4.0, false, "takeprofit" },

        { config_keys::kTp1Fraction, "止盈1比例",
          "达到止盈1时平仓的比例",
          "", 0.30, 0.0, 1.0, 0.20, 0.50, false, "takeprofit" },

        { config_keys::kTp2Fraction, "止盈2比例",
          "达到止盈2时平仓的比例",
          "", 0.30, 0.0, 1.0, 0.20, 0.50, false, "takeprofit" },

        // ---------- 逃顶 ----------
        { config_keys::kEscapeAtr, "逃顶距离",
          "价格偏离均线的 ATR 倍数，触发逃顶检测",
          "ATR", 4.0, 1.5, 10.0, 3.0, 6.0, false, "escape" },

        { config_keys::kEscapeConfirmBars, "逃顶确认",
          "逃顶信号需要连续确认的 K 线数",
          "根", 3, 1, 10, 2, 5, true, "escape" },

        // ---------- 加仓 ----------
        { config_keys::kMaxAddCount, "最大加仓次数",
          "倒金字塔最多加仓次数",
          "次", 3, 0, 5, 2, 3, true, "add" },

        { config_keys::kAddScale1, "首次加仓比例",
          "相对首仓的加仓比例",
          "", 0.8, 0.0, 2.0, 0.6, 1.0, false, "add" },

        { config_keys::kAddScale2, "二次加仓比例",
          "相对首仓的加仓比例",
          "", 0.4, 0.0, 2.0, 0.3, 0.6, false, "add" },

        { config_keys::kAddCooldownBars, "加仓冷却",
          "两次加仓之间的最小间隔",
          "根", 5, 0, 50, 3, 10, true, "add" },

        // ---------- 时间止损 ----------
        { config_keys::kTimeStopBars, "时间止损",
          "入场后多少根 K 线未达 min_r 则平仓",
          "根", 6, 1, 100, 4, 15, true, "timestop" },

        // ---------- 冷却 ----------
        { config_keys::kCooldownBars, "冷却期",
          "平仓后多少根 K 线内不开新仓",
          "根", 3, 0, 50, 2, 8, true, "cooldown" },
    }};

    // 编译期校验：表大小与 kParamCount 一致
    static_assert(kTable.size() == config_mapper_config::kParamCount,
                  "元信息表大小必须等于 kParamCount");

    return kTable;
}

}  // namespace

// ==============================================================================
// ConfigMapper::Impl
// ==============================================================================
struct ConfigMapper::Impl {
    Dependencies deps;

    // 当前参数（RCU 模式）
    StrategyParams current;
    std::atomic<std::uint32_t> version{0};
    std::atomic<bool> running{false};

    // 读锁保护
    mutable std::shared_mutex params_mutex;

    // 参数历史
    std::deque<ParamChange> history;
    mutable std::shared_mutex history_mutex;

    // 节流
    std::atomic<std::int64_t> last_change_ms{0};

    // 订阅 ID
    std::uint64_t config_subscription_id{0};

    // 统计
    std::atomic<std::uint64_t> reload_count{0};
    std::atomic<std::uint64_t> reload_failed_count{0};
    std::atomic<std::uint64_t> param_changed_count{0};

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<StrategyParams> load_from_config();

    void apply_params(StrategyParams new_params,
                       std::string_view source);

    void record_change(const StrategyParams& old_p,
                        const StrategyParams& new_p,
                        std::string_view source);

    void notify_change(const StrategyParams& new_p,
                        const StrategyParams& old_p,
                        const ParamChange& chg);

    [[nodiscard]] bool is_throttled() const noexcept;

    [[nodiscard]] StrategyParams make_default() const noexcept;
};

// ==============================================================================
// Impl::make_default
// ==============================================================================
StrategyParams ConfigMapper::Impl::make_default() const noexcept {
    return StrategyParams{};
}

// ==============================================================================
// Impl::is_throttled
// ==============================================================================
bool ConfigMapper::Impl::is_throttled() const noexcept {
    const auto now_ms = Timestamp::now().milliseconds();
    const auto last = last_change_ms.load(std::memory_order_relaxed);
    if (last == 0) return false;
    return (now_ms - last) < config_mapper_config::kMinChangeIntervalMs;
}

// ==============================================================================
// Impl::load_from_config
// ==============================================================================
Result<StrategyParams> ConfigMapper::Impl::load_from_config() {
    if (!deps.config) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "ConfigManager 未注入");
    }

    auto snapshot = deps.config->current();
    if (!snapshot) {
        return QUANT_ERR_MSG(ErrorCode::ConfigNotFound,
            "配置快照为空");
    }

    StrategyParams p = make_default();

    // 逐参数读取（带默认值，缺失时使用默认值）
    try {
        // 趋势
        p.ema_period = static_cast<int>(snapshot->get_int(
            std::string{config_keys::kEmaPeriod}, p.ema_period));
        p.ema_fast_period = static_cast<int>(snapshot->get_int(
            std::string{config_keys::kEmaFastPeriod}, p.ema_fast_period));
        p.ema_slope_period = static_cast<int>(snapshot->get_int(
            std::string{config_keys::kEmaSlopePeriod}, p.ema_slope_period));

        // 带宽
        p.band_k = snapshot->get_double(
            std::string{config_keys::kBandK}, p.band_k);
        p.band_k_min = snapshot->get_double(
            std::string{config_keys::kBandKMin}, p.band_k_min);
        p.band_k_max = snapshot->get_double(
            std::string{config_keys::kBandKMax}, p.band_k_max);

        // 评分
        p.entry_threshold = snapshot->get_double(
            std::string{config_keys::kEntryThreshold}, p.entry_threshold);
        p.reverse_threshold = snapshot->get_double(
            std::string{config_keys::kReverseThreshold}, p.reverse_threshold);
        p.neutral_range = snapshot->get_double(
            std::string{config_keys::kNeutralRange}, p.neutral_range);

        // 止损止盈
        p.stop_atr = snapshot->get_double(
            std::string{config_keys::kStopAtr}, p.stop_atr);
        p.min_stop_atr = snapshot->get_double(
            std::string{config_keys::kMinStopAtr}, p.min_stop_atr);
        p.target_r = snapshot->get_double(
            std::string{config_keys::kTargetR}, p.target_r);
        p.tp1_r = snapshot->get_double(
            std::string{config_keys::kTp1R}, p.tp1_r);
        p.tp2_r = snapshot->get_double(
            std::string{config_keys::kTp2R}, p.tp2_r);
        p.tp1_fraction = snapshot->get_double(
            std::string{config_keys::kTp1Fraction}, p.tp1_fraction);
        p.tp2_fraction = snapshot->get_double(
            std::string{config_keys::kTp2Fraction}, p.tp2_fraction);

        // 逃顶
        p.escape_atr = snapshot->get_double(
            std::string{config_keys::kEscapeAtr}, p.escape_atr);
        p.escape_confirm_bars = static_cast<int>(snapshot->get_int(
            std::string{config_keys::kEscapeConfirmBars},
            p.escape_confirm_bars));

        // 加仓
        p.max_add_count = static_cast<int>(snapshot->get_int(
            std::string{config_keys::kMaxAddCount}, p.max_add_count));
        p.add_scale_1 = snapshot->get_double(
            std::string{config_keys::kAddScale1}, p.add_scale_1);
        p.add_scale_2 = snapshot->get_double(
            std::string{config_keys::kAddScale2}, p.add_scale_2);
        p.add_trend_min = snapshot->get_double(
            std::string{config_keys::kAddTrendMin}, p.add_trend_min);
        p.add_cooldown_bars = static_cast<int>(snapshot->get_int(
            std::string{config_keys::kAddCooldownBars}, p.add_cooldown_bars));

        // 时间止损
        p.time_stop_bars = static_cast<int>(snapshot->get_int(
            std::string{config_keys::kTimeStopBars}, p.time_stop_bars));
        p.time_stop_min_r = snapshot->get_double(
            std::string{config_keys::kTimeStopMinR}, p.time_stop_min_r);

        // 冷却
        p.cooldown_bars = static_cast<int>(snapshot->get_int(
            std::string{config_keys::kCooldownBars}, p.cooldown_bars));

        // 风险
        p.risk_per_trade = snapshot->get_double(
            std::string{config_keys::kRiskPerTrade}, p.risk_per_trade);
        p.reverse_risk = snapshot->get_double(
            std::string{config_keys::kReverseRisk}, p.reverse_risk);
        p.max_daily_loss = snapshot->get_double(
            std::string{config_keys::kMaxDailyLoss}, p.max_daily_loss);

        // 策略类型
        p.strategy_type = snapshot->get_string(
            std::string{config_keys::kStrategyType}, p.strategy_type);

    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数读取异常")
            .with_context("error", e.what());
    } catch (...) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数读取未知异常");
    }

    // 校验
    if (auto r = p.validate(); r.is_err()) {
        return r;
    }

    return p;
}

// ==============================================================================
// Impl::apply_params
// ==============================================================================
void ConfigMapper::Impl::apply_params(StrategyParams new_params,
                                        std::string_view source)
{
    // 读取旧参数（读锁）
    StrategyParams old_params;
    {
        std::shared_lock<std::shared_mutex> lock(params_mutex);
        old_params = current;
    }

    // 检查是否有实际变更
    const bool has_change =
        !approx_equal(old_params.band_k, new_params.band_k) ||
        old_params.ema_period != new_params.ema_period ||
        !approx_equal(old_params.entry_threshold, new_params.entry_threshold) ||
        !approx_equal(old_params.reverse_threshold, new_params.reverse_threshold) ||
        !approx_equal(old_params.stop_atr, new_params.stop_atr) ||
        !approx_equal(old_params.target_r, new_params.target_r) ||
        old_params.max_add_count != new_params.max_add_count ||
        old_params.strategy_type != new_params.strategy_type ||
        // 更全面地比较
        !approx_equal(old_params.ema_fast_period, new_params.ema_fast_period) ||
        !approx_equal(old_params.tp1_r, new_params.tp1_r) ||
        !approx_equal(old_params.tp2_r, new_params.tp2_r) ||
        !approx_equal(old_params.escape_atr, new_params.escape_atr) ||
        old_params.cooldown_bars != new_params.cooldown_bars ||
        old_params.time_stop_bars != new_params.time_stop_bars;

    if (!has_change) {
        QUANT_LOG_DEBUG("参数无变化，跳过更新");
        return;
    }

    // 写锁更新
    {
        std::unique_lock<std::shared_mutex> lock(params_mutex);
        current = new_params;
    }

    // 递增版本
    const auto new_version = version.fetch_add(1, std::memory_order_acq_rel) + 1;

    // 更新时间戳
    last_change_ms.store(Timestamp::now().milliseconds(),
                          std::memory_order_relaxed);

    // 记录历史
    record_change(old_params, new_params, source);

    // 统计
    param_changed_count.fetch_add(1, std::memory_order_relaxed);

    QUANT_LOG_INFO("参数已更新: version={} source={}",
                   new_version, source);

    // 触发回调（只记录主要变更）
    ParamChange chg;
    chg.version = new_version;
    chg.timestamp = Timestamp::now();
    chg.key = "*";
    chg.old_value = "multi";
    chg.new_value = "multi";
    chg.source = std::string{source};

    notify_change(new_params, old_params, chg);
}

// ==============================================================================
// Impl::record_change
// ==============================================================================
void ConfigMapper::Impl::record_change(const StrategyParams& old_p,
                                         const StrategyParams& new_p,
                                         std::string_view source)
{
    // 记录主要参数的变更（简化，只记录关键参数）
    struct DiffItem {
        std::string key;
        std::string old_val;
        std::string new_val;
        bool changed;
    };

    std::vector<DiffItem> diffs;

    auto check = [&](std::string_view key, auto oldv, auto newv) {
        if (oldv != newv) {
            diffs.push_back({
                std::string{key},
                to_str(oldv),
                to_str(newv),
                true
            });
        }
    };

    check(config_keys::kBandK, old_p.band_k, new_p.band_k);
    check(config_keys::kEmaPeriod, old_p.ema_period, new_p.ema_period);
    check(config_keys::kEntryThreshold,
          old_p.entry_threshold, new_p.entry_threshold);
    check(config_keys::kReverseThreshold,
          old_p.reverse_threshold, new_p.reverse_threshold);
    check(config_keys::kStopAtr, old_p.stop_atr, new_p.stop_atr);
    check(config_keys::kTargetR, old_p.target_r, new_p.target_r);
    check(config_keys::kEscapeAtr, old_p.escape_atr, new_p.escape_atr);
    check(config_keys::kMaxAddCount,
          old_p.max_add_count, new_p.max_add_count);
    check(config_keys::kCooldownBars,
          old_p.cooldown_bars, new_p.cooldown_bars);
    check(config_keys::kStrategyType,
          old_p.strategy_type, new_p.strategy_type);

    std::unique_lock<std::shared_mutex> lock(history_mutex);

    const auto ver = version.load(std::memory_order_acquire);

    for (auto& d : diffs) {
        ParamChange chg;
        chg.version = ver;
        chg.timestamp = Timestamp::now();
        chg.key = d.key;
        chg.old_value = d.old_val;
        chg.new_value = d.new_val;
        chg.source = std::string{source};

        history.push_back(std::move(chg));
    }

    // 限制历史大小
    while (history.size() > config_mapper_config::kMaxParamHistory) {
        history.pop_front();
    }
}

// ==============================================================================
// Impl::notify_change
// ==============================================================================
void ConfigMapper::Impl::notify_change(const StrategyParams& new_p,
                                         const StrategyParams& old_p,
                                         const ParamChange& chg)
{
    if (!deps.on_change) return;

    try {
        deps.on_change(new_p, old_p, chg);
    } catch (const std::exception& e) {
        QUANT_LOG_WARN("on_change 回调异常: {}", e.what());
    } catch (...) {
        QUANT_LOG_WARN("on_change 回调未知异常");
    }
}

// ==============================================================================
// ConfigMapper 构造与析构
// ==============================================================================
ConfigMapper::ConfigMapper(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    if (!deps.config) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "ConfigMapper: config 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }

    impl_->deps = std::move(deps);
}

ConfigMapper::~ConfigMapper() {
    stop();
}

// ==============================================================================
// 生命周期
// ==============================================================================
Result<void> ConfigMapper::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "ConfigMapper 已启动");
    }

    // 首次加载
    auto result = impl_->load_from_config();
    if (result.is_err()) {
        impl_->running.store(false, std::memory_order_release);
        impl_->reload_failed_count.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    {
        std::unique_lock<std::shared_mutex> lock(impl_->params_mutex);
        impl_->current = std::move(result.value());
    }

    impl_->version.fetch_add(1, std::memory_order_acq_rel);
    impl_->reload_count.fetch_add(1, std::memory_order_relaxed);

    QUANT_LOG_INFO("ConfigMapper 已启动: version={}",
                   impl_->version.load());

    return {};
}

void ConfigMapper::stop() noexcept {
    if (!impl_) return;
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    QUANT_LOG_INFO("ConfigMapper 已停止");
}

void ConfigMapper::reset() noexcept {
    if (!impl_) return;

    {
        std::unique_lock<std::shared_mutex> lock(impl_->params_mutex);
        impl_->current = StrategyParams{};
    }

    {
        std::unique_lock<std::shared_mutex> lock(impl_->history_mutex);
        impl_->history.clear();
    }

    impl_->version.store(0, std::memory_order_release);
    impl_->last_change_ms.store(0, std::memory_order_relaxed);
    impl_->reload_count.store(0, std::memory_order_relaxed);
    impl_->reload_failed_count.store(0, std::memory_order_relaxed);
    impl_->param_changed_count.store(0, std::memory_order_relaxed);
}

bool ConfigMapper::is_running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

// ==============================================================================
// 参数访问
// ==============================================================================
StrategyParams ConfigMapper::params() const {
    std::shared_lock<std::shared_mutex> lock(impl_->params_mutex);
    return impl_->current;
}

int ConfigMapper::ema_period() const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->params_mutex);
    return impl_->current.ema_period;
}

double ConfigMapper::band_k() const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->params_mutex);
    return impl_->current.band_k;
}

double ConfigMapper::entry_threshold() const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->params_mutex);
    return impl_->current.entry_threshold;
}

double ConfigMapper::reverse_threshold() const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->params_mutex);
    return impl_->current.reverse_threshold;
}

double ConfigMapper::stop_atr() const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->params_mutex);
    return impl_->current.stop_atr;
}

double ConfigMapper::target_r() const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->params_mutex);
    return impl_->current.target_r;
}

int ConfigMapper::max_add_count() const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->params_mutex);
    return impl_->current.max_add_count;
}

std::uint32_t ConfigMapper::version() const noexcept {
    return impl_->version.load(std::memory_order_acquire);
}

// ==============================================================================
// 手动重载
// ==============================================================================
Result<void> ConfigMapper::reload() {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "ConfigMapper 未启动");
    }

    // 节流检查
    if (impl_->is_throttled()) {
        QUANT_LOG_DEBUG("重载被节流，跳过");
        return {};
    }

    auto result = impl_->load_from_config();
    if (result.is_err()) {
        impl_->reload_failed_count.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    impl_->apply_params(std::move(result.value()), "config_reload");
    impl_->reload_count.fetch_add(1, std::memory_order_relaxed);

    return {};
}

// ==============================================================================
// 手动修改单个参数
// ==============================================================================
Result<void> ConfigMapper::set_param(std::string_view key,
                                       std::string_view value)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "ConfigMapper 未启动");
    }

    if (key.empty()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "参数键为空");
    }

    // 复制当前参数
    StrategyParams params_copy;
    {
        std::shared_lock<std::shared_mutex> lock(impl_->params_mutex);
        params_copy = impl_->current;
    }

    // 解析值
    auto parse_double = [&]() -> std::optional<double> {
        try {
            std::size_t pos = 0;
            const double v = std::stod(std::string{value}, &pos);
            if (pos != value.size()) return std::nullopt;
            return v;
        } catch (...) {
            return std::nullopt;
        }
    };

    auto parse_int = [&]() -> std::optional<int> {
        try {
            std::size_t pos = 0;
            const int v = std::stoi(std::string{value}, &pos);
            if (pos != value.size()) return std::nullopt;
            return v;
        } catch (...) {
            return std::nullopt;
        }
    };

    // 匹配键
    bool matched = false;

    if (key == config_keys::kEmaPeriod) {
        if (auto v = parse_int()) { params_copy.ema_period = *v; matched = true; }
    } else if (key == config_keys::kEmaFastPeriod) {
        if (auto v = parse_int()) { params_copy.ema_fast_period = *v; matched = true; }
    } else if (key == config_keys::kBandK) {
        if (auto v = parse_double()) { params_copy.band_k = *v; matched = true; }
    } else if (key == config_keys::kEntryThreshold) {
        if (auto v = parse_double()) { params_copy.entry_threshold = *v; matched = true; }
    } else if (key == config_keys::kReverseThreshold) {
        if (auto v = parse_double()) { params_copy.reverse_threshold = *v; matched = true; }
    } else if (key == config_keys::kStopAtr) {
        if (auto v = parse_double()) { params_copy.stop_atr = *v; matched = true; }
    } else if (key == config_keys::kTargetR) {
        if (auto v = parse_double()) { params_copy.target_r = *v; matched = true; }
    } else if (key == config_keys::kMaxAddCount) {
        if (auto v = parse_int()) { params_copy.max_add_count = *v; matched = true; }
    } else if (key == config_keys::kCooldownBars) {
        if (auto v = parse_int()) { params_copy.cooldown_bars = *v; matched = true; }
    } else if (key == config_keys::kStrategyType) {
        params_copy.strategy_type = std::string{value}; matched = true;
    }

    if (!matched) {
        return QUANT_ERR_MSG(ErrorCode::ConfigNotFound,
            "未找到参数键")
            .with_context("key", std::string{key});
    }

    // 校验
    if (auto r = params_copy.validate(); r.is_err()) {
        return r;
    }

    // 应用
    impl_->apply_params(std::move(params_copy), "manual");

    return {};
}

// ==============================================================================
// 参数历史
// ==============================================================================
std::vector<ParamChange> ConfigMapper::history() const {
    std::shared_lock<std::shared_mutex> lock(impl_->history_mutex);
    return {impl_->history.begin(), impl_->history.end()};
}

// ==============================================================================
// 元信息
// ==============================================================================
std::vector<ParamMeta> ConfigMapper::metadata() {
    const auto& table = metadata_table();
    return {table.begin(), table.end()};
}

std::optional<ParamMeta> ConfigMapper::find_metadata(
    std::string_view key)
{
    const auto& table = metadata_table();
    for (const auto& m : table) {
        if (m.key == key) return m;
    }
    return std::nullopt;
}

// ==============================================================================
// 默认预设
// ==============================================================================
StrategyParams ConfigMapper::default_preset() {
    return StrategyParams{};
}

// ==============================================================================
// 参数校验
// ==============================================================================
Result<void> ConfigMapper::validate_params(const StrategyParams& params) {
    return params.validate();
}

// ==============================================================================
// 诊断
// ==============================================================================
std::string ConfigMapper::dump() const {
    std::ostringstream oss;
    oss << "ConfigMapper dump:\n";

    if (!impl_) {
        oss << "  <impl is null>\n";
        return oss.str();
    }

    oss << "  running: " << (is_running() ? "yes" : "no") << "\n";
    oss << "  version: "
        << impl_->version.load(std::memory_order_acquire) << "\n";

    // 当前参数
    StrategyParams p;
    {
        std::shared_lock<std::shared_mutex> lock(impl_->params_mutex);
        p = impl_->current;
    }

    oss << "  params:\n";
    oss << "    trend: ema=" << p.ema_period
        << " fast=" << p.ema_fast_period
        << " slope=" << p.ema_slope_period << "\n";
    oss << "    band: k=" << p.band_k
        << " min=" << p.band_k_min
        << " max=" << p.band_k_max << "\n";
    oss << "    scoring: entry=" << p.entry_threshold
        << " reverse=" << p.reverse_threshold
        << " neutral=" << p.neutral_range << "\n";
    oss << "    stoploss: stop_atr=" << p.stop_atr
        << " min_stop_atr=" << p.min_stop_atr << "\n";
    oss << "    takeprofit: target_r=" << p.target_r
        << " tp1_r=" << p.tp1_r
        << " tp2_r=" << p.tp2_r << "\n";
    oss << "    escape: escape_atr=" << p.escape_atr
        << " confirm=" << p.escape_confirm_bars << "\n";
    oss << "    add: max_count=" << p.max_add_count
        << " scale1=" << p.add_scale_1
        << " scale2=" << p.add_scale_2
        << " cooldown=" << p.add_cooldown_bars << "\n";
    oss << "    timestop: bars=" << p.time_stop_bars
        << " min_r=" << p.time_stop_min_r << "\n";
    oss << "    cooldown_bars=" << p.cooldown_bars << "\n";
    oss << "    risk: per_trade=" << p.risk_per_trade
        << " reverse=" << p.reverse_risk
        << " max_daily_loss=" << p.max_daily_loss << "\n";
    oss << "    strategy_type: " << p.strategy_type << "\n";

    // 统计
    oss << "  stats:\n";
    oss << "    reload_count: "
        << impl_->reload_count.load(std::memory_order_relaxed) << "\n";
    oss << "    reload_failed_count: "
        << impl_->reload_failed_count.load(std::memory_order_relaxed) << "\n";
    oss << "    param_changed_count: "
        << impl_->param_changed_count.load(std::memory_order_relaxed) << "\n";
    oss << "    history_size: " << impl_->history.size() << "\n";

    // 历史（最近 10 条）
    {
        std::shared_lock<std::shared_mutex> lock(impl_->history_mutex);
        oss << "  recent_changes:\n";
        const std::size_t n = std::min<std::size_t>(
            impl_->history.size(), 10);
        const std::size_t start = impl_->history.size() - n;
        for (std::size_t i = start; i < impl_->history.size(); ++i) {
            const auto& c = impl_->history[i];
            oss << "    [v" << c.version << "] "
                << c.key << ": "
                << c.old_value << " -> " << c.new_value
                << " (" << c.source << ")\n";
        }
    }

    return oss.str();
}

}  // namespace quant::strategy
