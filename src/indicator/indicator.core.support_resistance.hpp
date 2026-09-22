// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 支撑阻力映射
// ==============================================================================
// @file    src/indicator/indicator.core.support_resistance.hpp
// @module  indicator
// @type    core
// @name    support_resistance
// @version 1.0.1
// @brief   将 5 分钟 K 线高低点映射为 3 分钟策略的支撑/阻力位
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 严格时间对齐：只用已收盘的 5m K 线，避免未来数据
//   - 聚类合并：相近价格合并为一个水平
//   - 强度评分：触及次数 × 量能 × 时间衰减
//   - 动态类型：按价格位置判定支撑/阻力
//   - 突破失效：突破后标记，回踩确认后重置
//   - 环形缓冲：限制历史，避免内存增长
//   - 配置化：阈值、窗口、聚类精度可配
//
// 线程安全:
//   - 每实例单线程使用，不共享
// ==============================================================================

#ifndef QUANT_INDICATOR_CORE_SUPPORT_RESISTANCE_HPP
#define QUANT_INDICATOR_CORE_SUPPORT_RESISTANCE_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
#include "common/common.core.timestamp.hpp"
#include "common/common.core.types.hpp"

namespace quant {
namespace indicator {

// ==============================================================================
// 常量
// ==============================================================================
inline constexpr double kSrEpsilon = 1e-9;
inline constexpr std::size_t kDefaultMaxLevels = 16;
inline constexpr std::size_t kDefaultWindow5m = 288;     // 24 小时
inline constexpr std::size_t kDefaultMinWarmup5m = 20;
inline constexpr double kDefaultClusterAtr = 0.3;
inline constexpr double kDefaultProximityAtr = 0.5;
inline constexpr double kDefaultDecayTauBars = 100.0;

// 5 分钟周期（微秒）
inline constexpr int64_t kInterval5mUs = 5LL * 60 * 1000000;

// ==============================================================================
// 配置
// ==============================================================================
struct SRConfig {
    // 时间框架（微秒），默认 5 分钟
    int64_t timeframe_us{kInterval5mUs};

    // 保留的历史 5m K 线数量
    std::size_t window_size{kDefaultWindow5m};

    // 预热最少 5m K 线数
    std::size_t min_warmup{kDefaultMinWarmup5m};

    // 聚类合并阈值（ATR 倍数）
    double cluster_atr{kDefaultClusterAtr};

    // "接近" 判定阈值（ATR 倍数）
    double proximity_atr{kDefaultProximityAtr};

    // 时间衰减系数 τ（K 线根数）
    double decay_tau_bars{kDefaultDecayTauBars};

    // 最大保留的水平数量（避免过度聚类）
    std::size_t max_levels{kDefaultMaxLevels};

    // 是否启用聚类
    bool enable_clustering{true};

    // 校验
    [[nodiscard]] bool is_valid() const noexcept {
        if (timeframe_us <= 0) return false;
        if (window_size < 10) return false;
        if (min_warmup < 5) return false;
        if (cluster_atr < 0.05 || cluster_atr > 1.0) return false;
        if (proximity_atr < 0.1 || proximity_atr > 2.0) return false;
        if (decay_tau_bars < 1.0) return false;
        if (max_levels < 2 || max_levels > 128) return false;
        return true;
    }

    [[nodiscard]] std::string validate() const;
};

// ==============================================================================
// 水平类型
// ==============================================================================
enum class SRLevelType : std::uint8_t {
    Unknown     = 0,
    Support     = 1,   // 价格在水平上方，水平起支撑作用
    Resistance  = 2,   // 价格在水平下方，水平起阻力作用
    Pivot       = 3,   // 价格正好在水平附近
};

[[nodiscard]] constexpr std::string_view to_string(SRLevelType t) noexcept {
    switch (t) {
        case SRLevelType::Support:    return "Support";
        case SRLevelType::Resistance: return "Resistance";
        case SRLevelType::Pivot:      return "Pivot";
        default:                      return "Unknown";
    }
}

// ==============================================================================
// 支撑/阻力水平
// ==============================================================================
struct SRLevel {
    // 价格
    double price{0.0};

    // 强度评分 [0, 1]
    double strength{0.0};

    // 动态类型（按当前价格判定）
    SRLevelType type{SRLevelType::Unknown};

    // 触及次数
    std::uint32_t touch_count{0};

    // 量能加权（触及时的平均量能比）
    double avg_volume_ratio{1.0};

    // 首次形成时间
    Timestamp first_seen{};

    // 最近触及时间
    Timestamp last_touch{};

    // 时间衰减因子 [0, 1]
    double time_decay{1.0};

    // 是否被突破
    bool is_broken{false};

    // 突破时间
    Timestamp broken_at{};

    // 突破后回踩确认
    bool retest_confirmed{false};

    [[nodiscard]] bool is_valid() const noexcept {
        return price > 0.0 && strength >= 0.0 && strength <= 1.0;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return !is_broken;
    }
};

// ==============================================================================
// 统计
// ==============================================================================
struct SRStats {
    std::uint64_t total_updates{0};
    std::uint64_t levels_created{0};
    std::uint64_t levels_merged{0};
    std::uint64_t levels_expired{0};
    std::uint64_t breakouts_detected{0};
    std::uint64_t retests_confirmed{0};
    std::uint64_t fake_breakouts{0};
    std::uint64_t gap_count{0};

    void reset() noexcept { *this = SRStats{}; }
};

// ==============================================================================
// 关键位查询结果
// ==============================================================================
struct SRProximity {
    std::optional<SRLevel> nearest_resistance;
    std::optional<SRLevel> nearest_support;
    double distance_to_resistance_atr{0.0};   // 正数 = 距阻力位
    double distance_to_support_atr{0.0};      // 正数 = 距支撑位
    bool is_blocked_by_resistance{false};     // 距阻力 < proximity_atr
    bool is_supported_by_support{false};      // 距支撑 < proximity_atr

    [[nodiscard]] bool has_any() const noexcept {
        return nearest_resistance.has_value() || nearest_support.has_value();
    }
};

// ==============================================================================
// 支撑/阻力映射
// ==============================================================================
class SupportResistance {
public:
    // -------------------------------------------------------------------------
    // 构造 / 配置
    // -------------------------------------------------------------------------
    explicit SupportResistance(SRConfig config = {});

    SupportResistance(const SupportResistance&) = delete;
    SupportResistance& operator=(const SupportResistance&) = delete;
    SupportResistance(SupportResistance&&) noexcept = default;
    SupportResistance& operator=(SupportResistance&&) noexcept = default;

    ~SupportResistance() = default;

    [[nodiscard]] const SRConfig& config() const noexcept { return config_; }
    void set_config(const SRConfig& config);

    // -------------------------------------------------------------------------
    // 更新
    // -------------------------------------------------------------------------
    // 增量更新：传入一根已收盘的 5m K 线
    //   current_3m_time - 当前 3m 策略时间，用于时间对齐检查
    //   atr             - 当前 ATR（用于聚类和距离归一化）
    // 返回错误：时间戳回退、时间对齐失败
    [[nodiscard]] Result<void>
        update(const Candle& candle_5m,
               Timestamp current_3m_time,
               double atr) noexcept;

    // 批量更新历史（回测初始化）
    [[nodiscard]] Result<void>
        load_history(const std::vector<Candle>& candles_5m,
                     Timestamp current_3m_time,
                     double atr) noexcept;

    // -------------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------------
    // 获取当前价格附近的支撑/阻力
    [[nodiscard]] SRProximity
        query(double current_price, double atr) const noexcept;

    // 最近的阻力位
    [[nodiscard]] std::optional<SRLevel>
        nearest_resistance(double current_price, double atr) const noexcept;

    // 最近的支撑位
    [[nodiscard]] std::optional<SRLevel>
        nearest_support(double current_price, double atr) const noexcept;

    // 是否被阻力位阻挡（距离 < proximity_atr）
    [[nodiscard]] bool
        is_blocked_by_resistance(double current_price, double atr) const noexcept;

    // 是否被支撑位托底
    [[nodiscard]] bool
        is_supported_by_support(double current_price, double atr) const noexcept;

    // 距离某个水平的 ATR 距离
    [[nodiscard]] double
        distance_atr(double price, const SRLevel& level, double atr) const noexcept;

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] std::size_t level_count() const noexcept { return levels_.size(); }
    [[nodiscard]] std::size_t bar_count() const noexcept { return bar_count_; }
    [[nodiscard]] bool is_warmed_up() const noexcept {
        return bar_count_ >= config_.min_warmup;
    }

    // 获取所有活跃水平（只读）
    [[nodiscard]] const std::vector<SRLevel>& levels() const noexcept {
        return levels_;
    }

    // -------------------------------------------------------------------------
    // 序列化 / 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] std::string to_string() const;

    // -------------------------------------------------------------------------
    // 状态管理
    // -------------------------------------------------------------------------
    void reset() noexcept;
    void reset_stats() noexcept { stats_.reset(); }
    void reset_all() noexcept { reset(); stats_.reset(); }

    [[nodiscard]] const SRStats& stats() const noexcept { return stats_; }

private:
    // -------------------------------------------------------------------------
    // 内部方法（实现见 .cpp）
    // -------------------------------------------------------------------------

    // 时间对齐：找到当前 3m 时间对应的最近已收盘 5m K 线开盘时间
    [[nodiscard]] static int64_t align_5m_time(int64_t time_3m_us,
                                                 int64_t interval_5m_us) noexcept;

    // 检测输入 K 线的时间戳是否合法
    [[nodiscard]] Result<void>
        validate_candle(const Candle& c, Timestamp current_3m_time) noexcept;

    // 从 5m K 线提取候选水平（高/低）
    void extract_candidates(const Candle& c, double atr) noexcept;

    // 聚类合并相近水平
    void cluster_levels(double atr) noexcept;

    // 更新水平强度
    void update_strength(SRLevel& level,
                          const Candle& c,
                          double atr,
                          Timestamp now) noexcept;

    // 时间衰减
    void apply_time_decay(Timestamp now) noexcept;

    // 检测突破
    void detect_breakouts(double current_price, Timestamp now) noexcept;

    // 淘汰最弱/最旧的水平
    void evict_excess() noexcept;

    // 重建 levels_（聚类后）
    void rebuild_levels(double atr) noexcept;

    // 计算距离（ATR 单位）
    [[nodiscard]] static double compute_distance_atr(double a, double b,
                                                       double atr) noexcept;

    // 内部：判定价格相对水平的类型
    [[nodiscard]] static SRLevelType classify_level(double price,
                                                       double level_price,
                                                       double atr) noexcept;

    // -------------------------------------------------------------------------
    // 成员
    // -------------------------------------------------------------------------
    SRConfig config_;
    SRStats  stats_{};

    // 水平列表（已聚类）
    std::vector<SRLevel> levels_;

    // 候选水平（未聚类，来自最近的 5m K 线）
    struct Candidate {
        double price{0.0};
        double volume_ratio{1.0};
        Timestamp time{};
        bool is_high{false};
    };
    std::vector<Candidate> candidates_;

    // 已处理的 5m K 线时间戳（用于去重和缺口检测）
    Timestamp last_5m_time_{};
    int64_t expected_interval_us_{0};

    // 计数
    std::size_t bar_count_{0};
};

}  // namespace indicator
}  // namespace quant

#endif  // QUANT_INDICATOR_CORE_SUPPORT_RESISTANCE_HPP
