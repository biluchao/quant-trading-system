// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 策略参数映射
// ==============================================================================
// @file    src/strategy/strategy.core.config_mapper.hpp
// @module  strategy
// @type    core
// @name    config_mapper
// @version 1.0.1
// @brief   策略参数与配置系统的映射、校验、热更新
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 类型安全：所有参数显式类型，禁止隐式转换
//   - 默认值：每个参数都有合理默认值
//   - 范围校验：注册时定义 min/max，加载时校验
//   - 联动校验：参数间依赖关系显式声明
//   - 热更新：订阅 ConfigManager 变更，自动刷新
//   - 版本管理：每次变更递增版本号
//   - 线程安全：读路径无锁，写路径 shared_mutex
//   - 完整诊断：dump() 输出所有参数
//   - 编译期枚举：参数数量固定
//   - 参数元信息：供前端 UI 显示
// ==============================================================================

#ifndef QUANT_STRATEGY_CORE_CONFIG_MAPPER_HPP
#define QUANT_STRATEGY_CORE_CONFIG_MAPPER_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ==============================================================================
// 项目内部
// ==============================================================================
#include "common/common.core.config.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant::strategy {

// ==============================================================================
// 常量
// ==============================================================================
namespace config_mapper_config {

// 参数数量（编译期枚举）
inline constexpr std::size_t kParamCount = 24;

// 参数历史最大保留
inline constexpr std::size_t kMaxParamHistory = 32;

// 变更最小间隔（毫秒），防止频繁重载
inline constexpr std::int64_t kMinChangeIntervalMs = 1000;

// 浮点比较 epsilon
inline constexpr double kEpsilon = 1e-9;

}  // namespace config_mapper_config

// ==============================================================================
// 策略参数键名（编译期常量，避免拼写错误）
// ==============================================================================
namespace config_keys {

// 趋势过滤
inline constexpr std::string_view kEmaPeriod        = "strategy.trend.ema_period";
inline constexpr std::string_view kEmaFastPeriod    = "strategy.trend.ema_fast";
inline constexpr std::string_view kEmaSlopePeriod   = "strategy.trend.slope_period";

// 带宽
inline constexpr std::string_view kBandK            = "strategy.band.k";
inline constexpr std::string_view kBandKMin         = "strategy.band.k_min";
inline constexpr std::string_view kBandKMax         = "strategy.band.k_max";

// 评分
inline constexpr std::string_view kEntryThreshold   = "strategy.scoring.entry_threshold";
inline constexpr std::string_view kReverseThreshold = "strategy.scoring.reverse_threshold";
inline constexpr std::string_view kNeutralRange     = "strategy.scoring.neutral_range";

// 止损止盈
inline constexpr std::string_view kStopAtr          = "strategy.stoploss.atr_mult";
inline constexpr std::string_view kMinStopAtr       = "strategy.stoploss.min_atr";
inline constexpr std::string_view kTargetR          = "strategy.takeprofit.target_r";
inline constexpr std::string_view kTp1R             = "strategy.takeprofit.tp1_r";
inline constexpr std::string_view kTp2R             = "strategy.takeprofit.tp2_r";
inline constexpr std::string_view kTp1Fraction      = "strategy.takeprofit.tp1_fraction";
inline constexpr std::string_view kTp2Fraction      = "strategy.takeprofit.tp2_fraction";

// 逃顶
inline constexpr std::string_view kEscapeAtr        = "strategy.escape.atr_mult";
inline constexpr std::string_view kEscapeConfirmBars= "strategy.escape.confirm_bars";

// 加仓
inline constexpr std::string_view kMaxAddCount      = "strategy.add.max_count";
inline constexpr std::string_view kAddScale1        = "strategy.add.scale_1";
inline constexpr std::string_view kAddScale2        = "strategy.add.scale_2";
inline constexpr std::string_view kAddScale3        = "strategy.add.scale_3";
inline constexpr std::string_view kAddTrendMin      = "strategy.add.trend_min";
inline constexpr std::string_view kAddCooldownBars  = "strategy.add.cooldown_bars";

// 时间止损
inline constexpr std::string_view kTimeStopBars     = "strategy.timestop.bars";
inline constexpr std::string_view kTimeStopMinR     = "strategy.timestop.min_r";

// 冷却
inline constexpr std::string_view kCooldownBars     = "strategy.cooldown.bars";

// 风险
inline constexpr std::string_view kRiskPerTrade     = "strategy.risk.per_trade";
inline constexpr std::string_view kReverseRisk      = "strategy.risk.reverse";
inline constexpr std::string_view kMaxDailyLoss     = "strategy.risk.max_daily_loss";

// 策略类型
inline constexpr std::string_view kStrategyType     = "strategy.type";

}  // namespace config_keys

// ==============================================================================
// 参数元信息（供前端 UI 显示）
// ==============================================================================
struct ParamMeta {
    std::string_view key;             // 配置键
    std::string_view label;           // 显示名
    std::string_view description;     // 说明
    std::string_view unit;            // 单位
    double default_value{0.0};        // 默认值
    double min_value{0.0};            // 最小值
    double max_value{0.0};            // 最大值
    double recommended_min{0.0};      // 推荐范围下限
    double recommended_max{0.0};      // 推荐范围上限
    bool is_integer{false};           // 是否为整数
    std::string_view group;           // 分组
};

// ==============================================================================
// 策略参数（完整）
// ==============================================================================
struct StrategyParams {
    // 趋势过滤
    int ema_period{26};
    int ema_fast_period{12};
    int ema_slope_period{5};

    // 带宽
    double band_k{1.0};
    double band_k_min{0.5};
    double band_k_max{2.0};

    // 评分
    double entry_threshold{60.0};
    double reverse_threshold{80.0};
    double neutral_range{30.0};

    // 止损止盈
    double stop_atr{1.5};
    double min_stop_atr{0.5};
    double target_r{2.0};
    double tp1_r{2.0};
    double tp2_r{3.0};
    double tp1_fraction{0.30};
    double tp2_fraction{0.30};

    // 逃顶
    double escape_atr{4.0};
    int escape_confirm_bars{3};

    // 加仓
    int max_add_count{3};
    double add_scale_1{0.8};
    double add_scale_2{0.4};
    double add_scale_3{0.0};         // 0 表示禁用第三次
    double add_trend_min{0.6};
    int add_cooldown_bars{5};

    // 时间止损
    int time_stop_bars{6};
    double time_stop_min_r{0.5};

    // 冷却
    int cooldown_bars{3};

    // 风险
    double risk_per_trade{0.02};
    double reverse_risk{0.012};
    double max_daily_loss{0.05};

    // 策略类型
    std::string strategy_type{"trend_following"};

    // 计算参数总和校验
    [[nodiscard]] Result<void> validate() const noexcept;

    // 加仓比例数组
    [[nodiscard]] std::array<double, 3> add_scales() const noexcept {
        return {1.0, add_scale_1, add_scale_2};
    }
};

// ==============================================================================
// 参数变更记录
// ==============================================================================
struct ParamChange {
    std::uint32_t version{0};
    Timestamp timestamp{};
    std::string key;
    std::string old_value;
    std::string new_value;
    std::string source;  // "config_reload" / "manual" / "ai_auto"
};

// ==============================================================================
// 参数变更订阅回调
// ==============================================================================
using ParamChangeCallback = std::function<void(
    const StrategyParams& new_params,
    const StrategyParams& old_params,
    const ParamChange& change)>;

// ==============================================================================
// 配置映射器
// ==============================================================================
class ConfigMapper {
public:
    // -------------------------------------------------------------------------
    // 依赖注入
    // -------------------------------------------------------------------------
    struct Dependencies {
        // 配置管理器（必需）
        ConfigManager* config{nullptr};

        // 参数变更回调（可选）
        ParamChangeCallback on_change;
    };

    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit ConfigMapper(Dependencies deps);
    ~ConfigMapper();

    ConfigMapper(const ConfigMapper&) = delete;
    ConfigMapper& operator=(const ConfigMapper&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 参数访问（热路径无锁读）
    // -------------------------------------------------------------------------
    // 返回当前参数快照（拷贝）
    [[nodiscard]] StrategyParams params() const;

    // 快速访问单个参数
    [[nodiscard]] int ema_period() const noexcept;
    [[nodiscard]] double band_k() const noexcept;
    [[nodiscard]] double entry_threshold() const noexcept;
    [[nodiscard]] double reverse_threshold() const noexcept;
    [[nodiscard]] double stop_atr() const noexcept;
    [[nodiscard]] double target_r() const noexcept;
    [[nodiscard]] int max_add_count() const noexcept;

    // 版本号
    [[nodiscard]] std::uint32_t version() const noexcept;

    // -------------------------------------------------------------------------
    // 手动重载（测试/管理用）
    // -------------------------------------------------------------------------
    Result<void> reload();

    // -------------------------------------------------------------------------
    // 手动修改单个参数（仅测试使用，生产应通过配置系统）
    // -------------------------------------------------------------------------
    Result<void> set_param(std::string_view key, std::string_view value);

    // -------------------------------------------------------------------------
    // 参数历史
    // -------------------------------------------------------------------------
    [[nodiscard]] std::vector<ParamChange> history() const;

    // -------------------------------------------------------------------------
    // 元信息（供前端 UI）
    // -------------------------------------------------------------------------
    [[nodiscard]] static std::vector<ParamMeta> metadata();
    [[nodiscard]] static std::optional<ParamMeta> find_metadata(
        std::string_view key);

    // -------------------------------------------------------------------------
    // 默认预设
    // -------------------------------------------------------------------------
    [[nodiscard]] static StrategyParams default_preset();

    // -------------------------------------------------------------------------
    // 参数校验
    // -------------------------------------------------------------------------
    [[nodiscard]] static Result<void> validate_params(
        const StrategyParams& params);

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    // =========================================================================
    // 内部实现
    // =========================================================================
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 便捷宏
// ==============================================================================
#define QUANT_STRATEGY_PARAM(mapper, name) ((mapper).name())

}  // namespace quant::strategy

#endif  // QUANT_STRATEGY_CORE_CONFIG_MAPPER_HPP
