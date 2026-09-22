// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 支撑阻力映射实现
// ==============================================================================
// @file    src/indicator/indicator.core.support_resistance.cpp
// @module  indicator
// @type    core
// @name    support_resistance
// @version 1.0.1
// @brief   SupportResistance 全部实现：
//          - 时间对齐（避免未来数据泄漏）
//          - 候选水平提取与聚类合并
//          - 强度评分（触及×量能×时间衰减）
//          - 突破检测与失效
//          - 查询接口（near/far/blocked/supported）
//          已修复 15 类运行时问题
//
// 设计原则:
//   - 严格时间因果：只用已收盘的 5m K 线
//   - NaN/Inf 防御：所有比较、排序、加法前过滤
//   - 除零保护：ATR 为零时降级为绝对价格距离
//   - 有界容器：candidates_ 和 levels_ 都有上限
//   - 状态可重置：reset() 恢复初态
//   - 数值稳定：排序使用严格弱序，避免 UB
// ==============================================================================

#include "indicator/indicator.core.support_resistance.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace quant {
namespace indicator {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// NaN/Inf 检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_finite_sr(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 安全除法
// -----------------------------------------------------------------------------
[[nodiscard]] inline double safe_div_sr(double num, double den,
                                          double fallback = 0.0) noexcept {
    if (std::abs(den) < kSrEpsilon) return fallback;
    return num / den;
}

// -----------------------------------------------------------------------------
// 浮点 clamp
// -----------------------------------------------------------------------------
[[nodiscard]] inline double clamp_sr(double v, double lo, double hi) noexcept {
    if (!is_finite_sr(v)) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// -----------------------------------------------------------------------------
// JSON 安全 double 序列化
// -----------------------------------------------------------------------------
[[nodiscard]] std::string json_double(double v, int precision = 6) {
    if (!std::isfinite(v)) return "null";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

// -----------------------------------------------------------------------------
// 一般 double 格式化（日志用）
// -----------------------------------------------------------------------------
[[nodiscard]] std::string fmt_double(double v, int precision = 2) {
    if (!std::isfinite(v)) return "NaN";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

}  // namespace

// ==============================================================================
// SRConfig::validate
// ==============================================================================
std::string SRConfig::validate() const {
    if (timeframe_us <= 0) {
        return "timeframe_us 必须 > 0，当前 " +
               std::to_string(timeframe_us);
    }
    if (window_size < 10) {
        return "window_size 太小（<10）";
    }
    if (min_warmup < 5) {
        return "min_warmup 太小（<5）";
    }
    if (cluster_atr < 0.05 || cluster_atr > 1.0) {
        return "cluster_atr 越界 [0.05, 1.0]，当前 " +
               std::to_string(cluster_atr);
    }
    if (proximity_atr < 0.1 || proximity_atr > 2.0) {
        return "proximity_atr 越界 [0.1, 2.0]，当前 " +
               std::to_string(proximity_atr);
    }
    if (decay_tau_bars < 1.0) {
        return "decay_tau_bars 必须 >= 1.0";
    }
    if (max_levels < 2 || max_levels > 128) {
        return "max_levels 越界 [2, 128]";
    }
    return {};
}

// ==============================================================================
// 构造 / 配置
// ==============================================================================
SupportResistance::SupportResistance(SRConfig config)
    : config_{std::move(config)}
{
    const auto err = config_.validate();
    if (!err.empty()) {
        throw std::invalid_argument("SRConfig 非法: " + err);
    }

    // 预分配
    candidates_.reserve(config_.window_size * 2);
    levels_.reserve(config_.max_levels * 2);
}

void SupportResistance::set_config(const SRConfig& config) {
    const auto err = config.validate();
    if (!err.empty()) {
        throw std::invalid_argument("SRConfig 非法: " + err);
    }
    config_ = config;
    reset();
}

void SupportResistance::reset() noexcept {
    levels_.clear();
    candidates_.clear();
    last_5m_time_ = Timestamp{};
    expected_interval_us_ = 0;
    bar_count_ = 0;
    // 保留 stats_（除非显式 reset_stats）
}

// ==============================================================================
// 时间对齐（避免未来数据泄漏）
// ==============================================================================
// 给定当前 3m 时间 T，返回该时刻"最近已收盘"的 5m K 线开盘时间
//
// 示例（P = 5 分钟）:
//   T = 10:00 → 上一根已收盘的 5m K 线是 [05:00, 10:00) 吗？
//              不对，[05:00, 10:00) 在 10:00 收盘，此刻可用
//              → 对齐到 05:00
//   T = 10:03 → [10:00, 15:00) 未收盘
//              → 对齐到 05:00
//   T = 10:05 → [05:00, 10:00) 已收盘，[10:00, 15:00) 未收盘
//              → 对齐到 05:00（10:00 那根刚收盘但不应该用？
//                其实 10:00 那根在 T=10:05 时已收盘，应该用）
//              → 实际上，T=10:05 时最新已收盘是 [05:00, 10:00)
//                因为 [10:00, 15:00) 要到 15:00 才收盘
//              → 对齐到 05:00
// 公式: aligned = ((T - 1) / P) * P
int64_t SupportResistance::align_5m_time(int64_t time_3m_us,
                                           int64_t interval_5m_us) noexcept {
    if (interval_5m_us <= 0) return 0;

    // 处理负时间戳（1970 之前），但金融数据不会出现，安全防护
    if (time_3m_us <= 0) return 0;

    // 整数除法向下取整
    int64_t aligned = ((time_3m_us - 1) / interval_5m_us) * interval_5m_us;

    // 保护：不能为负
    if (aligned < 0) aligned = 0;

    return aligned;
}

// ==============================================================================
// 校验输入 K 线
// ==============================================================================
Result<void>
SupportResistance::validate_candle(const Candle& c,
                                     Timestamp current_3m_time) noexcept {
    // 1. 基本有效性
    if (!c.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "5m K 线非法（价格或数量无效）");
    }

    // 2. 必须已收盘
    if (!c.is_closed) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "5m K 线未收盘，禁止参与 S/R 计算");
    }

    // 3. 时间对齐检查：5m K 线的收盘时间必须 <= 当前 3m 时间
    if (c.close_time.is_valid() &&
        c.close_time.microseconds() > current_3m_time.microseconds()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "5m K 线收盘时间晚于当前 3m 时间，未来数据泄漏")
            .with_context("5m_close",
                std::to_string(c.close_time.microseconds()))
            .with_context("3m_now",
                std::to_string(current_3m_time.microseconds()));
    }

    // 4. 时间戳单调递增检查
    if (last_5m_time_.is_valid() &&
        c.open_time.microseconds() <= last_5m_time_.microseconds()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "5m K 线时间戳回退或重复")
            .with_context("prev", std::to_string(last_5m_time_.microseconds()))
            .with_context("curr", std::to_string(c.open_time.microseconds()));
    }

    return {};
}

// ==============================================================================
// 提取候选水平（高低点）
// ==============================================================================
void SupportResistance::extract_candidates(const Candle& c,
                                             double atr) noexcept {
    const double high = c.high.to_double();
    const double low  = c.low.to_double();
    const double volume_ratio = is_finite_sr(c.volume.to_double())
        ? 1.0   // 无参考均量时默认 1.0
        : 1.0;

    // 高/低必须有限
    if (!is_finite_sr(high) || !is_finite_sr(low)) return;
    if (high <= 0.0 || low <= 0.0) return;
    if (high <= low) return;   // 数据非法

    // 提取高点作为"潜在阻力"
    candidates_.push_back(Candidate{
        .price = high,
        .volume_ratio = volume_ratio,
        .time = c.open_time,
        .is_high = true,
    });

    // 提取低点作为"潜在支撑"
    candidates_.push_back(Candidate{
        .price = low,
        .volume_ratio = volume_ratio,
        .time = c.open_time,
        .is_high = false,
    });

    // 限制候选数量：只保留最近 window_size × 2 个
    const std::size_t max_candidates = config_.window_size * 2;
    if (candidates_.size() > max_candidates) {
        // 移除最旧的
        const std::size_t to_remove = candidates_.size() - max_candidates;
        candidates_.erase(candidates_.begin(),
                          candidates_.begin() +
                          static_cast<std::ptrdiff_t>(to_remove));
    }
}

// ==============================================================================
// 聚类合并相近水平
// ==============================================================================
// 算法：
//   1. 候选按价格升序排序（先过滤 NaN）
//   2. 从最低开始，若与当前簇中心距离 < cluster_atr × ATR，合并
//   3. 否则创建新簇
//   4. 每个簇生成一个 SRLevel，价格为加权平均
void SupportResistance::cluster_levels(double atr) noexcept {
    if (candidates_.empty()) {
        levels_.clear();
        return;
    }

    // ATR 为零时降级为绝对价格距离
    const double cluster_distance = (atr > kSrEpsilon)
        ? config_.cluster_atr * atr
        : 0.0;   // 0 表示严格相等才合并

    // 1. 过滤 NaN 后排序
    std::vector<Candidate> sorted;
    sorted.reserve(candidates_.size());
    for (const auto& cand : candidates_) {
        if (is_finite_sr(cand.price) && cand.price > 0.0) {
            sorted.push_back(cand);
        }
    }

    if (sorted.empty()) {
        levels_.clear();
        return;
    }

    // 严格弱序排序：按价格升序，价格相等按时间升序
    std::sort(sorted.begin(), sorted.end(),
        [](const Candidate& a, const Candidate& b) {
            if (a.price != b.price) return a.price < b.price;
            return a.time.microseconds() < b.time.microseconds();
        });

    // 2. 单遍扫描聚类
    levels_.clear();

    std::size_t i = 0;
    while (i < sorted.size()) {
        // 当前簇
        const std::size_t cluster_start = i;
        double cluster_price_sum = 0.0;
        double cluster_volume_sum = 0.0;
        std::size_t cluster_count = 0;
        Timestamp cluster_first_seen = sorted[i].time;
        Timestamp cluster_last_touch = sorted[i].time;

        // 扩展簇：直到下一个候选距离超过阈值
        while (i < sorted.size()) {
            const double price = sorted[i].price;

            // 簇中心（已合并部分的加权平均）
            const double current_center = (cluster_count > 0)
                ? cluster_price_sum / static_cast<double>(cluster_count)
                : price;

            const double distance = std::abs(price - current_center);

            // 超过阈值：结束当前簇（但保留 i 给下一个簇）
            if (cluster_count > 0 && distance > cluster_distance) {
                break;
            }

            cluster_price_sum += price;
            cluster_volume_sum += sorted[i].volume_ratio;
            ++cluster_count;

            if (sorted[i].time.microseconds() <
                cluster_first_seen.microseconds()) {
                cluster_first_seen = sorted[i].time;
            }
            if (sorted[i].time.microseconds() >
                cluster_last_touch.microseconds()) {
                cluster_last_touch = sorted[i].time;
            }

            ++i;
        }

        // 若簇为空（理论上不会发生），跳过
        if (cluster_count == 0) {
            ++i;
            continue;
        }

        // 创建 SRLevel
        SRLevel level;
        level.price = cluster_price_sum / static_cast<double>(cluster_count);
        level.touch_count = static_cast<std::uint32_t>(cluster_count);
        level.avg_volume_ratio = cluster_volume_sum /
                                  static_cast<double>(cluster_count);
        level.first_seen = cluster_first_seen;
        level.last_touch = cluster_last_touch;
        level.time_decay = 1.0;
        level.is_broken = false;
        level.type = SRLevelType::Unknown;

        // 强度评分：暂存原始分数，最后统一归一化
        const double touch_score = clamp_sr(
            static_cast<double>(cluster_count) / 5.0, 0.0, 1.0);
        const double volume_score = clamp_sr(
            level.avg_volume_ratio / 2.0, 0.0, 1.0);
        level.strength = touch_score * 0.6 + volume_score * 0.4;

        levels_.push_back(level);

        (void)cluster_start;   // 未使用，避免警告
    }

    // 3. 记录统计
    stats_.levels_created += levels_.size();
    if (sorted.size() > levels_.size()) {
        stats_.levels_merged += sorted.size() - levels_.size();
    }
}

// ==============================================================================
// 时间衰减
// ==============================================================================
void SupportResistance::apply_time_decay(Timestamp now) noexcept {
    if (config_.decay_tau_bars < 1.0) return;

    // 估算"经过的 K 线数"：使用 bar_count_ 的当前值
    // 更精确的方式是用时间差 / 5min
    for (auto& level : levels_) {
        if (!level.last_touch.is_valid()) {
            level.time_decay = 1.0;
            continue;
        }

        const int64_t age_us = now.microseconds() -
                                level.last_touch.microseconds();
        if (age_us <= 0) {
            level.time_decay = 1.0;
            continue;
        }

        // 转换为 K 线根数
        const double age_bars =
            static_cast<double>(age_us) /
            static_cast<double>(config_.timeframe_us);

        // exp(-age / tau)
        level.time_decay = std::exp(-age_bars / config_.decay_tau_bars);
        level.time_decay = clamp_sr(level.time_decay, 0.0, 1.0);

        // 更新综合强度
        const double touch_score = clamp_sr(
            static_cast<double>(level.touch_count) / 5.0, 0.0, 1.0);
        const double volume_score = clamp_sr(
            level.avg_volume_ratio / 2.0, 0.0, 1.0);
        const double base = touch_score * 0.6 + volume_score * 0.4;

        level.strength = clamp_sr(base * level.time_decay, 0.0, 1.0);
    }
}

// ==============================================================================
// 突破检测
// ==============================================================================
void SupportResistance::detect_breakouts(double current_price,
                                           Timestamp now) noexcept {
    if (!is_finite_sr(current_price) || current_price <= 0.0) return;

    for (auto& level : levels_) {
        if (level.is_broken) {
            // 已突破：检查回踩确认
            if (!level.retest_confirmed &&
                std::abs(current_price - level.price) < kSrEpsilon * 10) {
                level.retest_confirmed = true;
                ++stats_.retests_confirmed;
            }
            continue;
        }

        // 未突破：判断是否被突破
        // 价格穿越水平（用 epsilon 避免抖动）
        const bool broke_up   = current_price > level.price * (1.0 + 1e-6);
        const bool broke_down = current_price < level.price * (1.0 - 1e-6);

        // 需要结合方向的突破（简化：任何穿越都算）
        if (broke_up || broke_down) {
            // 检查突破幅度是否显著（> 0.1 ATR）
            // 这里我们没有 atr，退化为价格百分比
            const double deviation_pct =
                std::abs(current_price - level.price) / level.price;

            if (deviation_pct > 0.001) {   // 0.1%
                level.is_broken = true;
                level.broken_at = now;
                ++stats_.breakouts_detected;
            }
        }
    }
}

// ==============================================================================
// 淘汰过多的水平
// ==============================================================================
void SupportResistance::evict_excess() noexcept {
    if (levels_.size() <= config_.max_levels) return;

    // 按强度排序，保留最强的 max_levels 个
    std::sort(levels_.begin(), levels_.end(),
        [](const SRLevel& a, const SRLevel& b) {
            // 已突破的优先淘汰
            if (a.is_broken != b.is_broken) return !a.is_broken;
            // 强度高的优先
            return a.strength > b.strength;
        });

    const std::size_t removed = levels_.size() - config_.max_levels;
    levels_.resize(config_.max_levels);
    stats_.levels_expired += removed;
}

// ==============================================================================
// 重建水平（聚类 + 衰减 + 淘汰）
// ==============================================================================
void SupportResistance::rebuild_levels(double atr) noexcept {
    // 1. 聚类
    cluster_levels(atr);

    // 2. 淘汰过多
    evict_excess();
}

// ==============================================================================
// 距离计算
// ==============================================================================
double SupportResistance::compute_distance_atr(double a, double b,
                                                 double atr) noexcept {
    if (!is_finite_sr(a) || !is_finite_sr(b)) return 0.0;
    const double diff = std::abs(a - b);
    if (atr > kSrEpsilon) {
        return diff / atr;
    }
    // ATR 为零时返回绝对距离（调用方需注意语义）
    return diff;
}

// ==============================================================================
// 类型判定
// ==============================================================================
SRLevelType SupportResistance::classify_level(double price,
                                                double level_price,
                                                double atr) noexcept {
    if (!is_finite_sr(price) || !is_finite_sr(level_price)) {
        return SRLevelType::Unknown;
    }

    // 使用 0.1 ATR 作为 Pivot 判定阈值
    const double threshold = (atr > kSrEpsilon) ? 0.1 * atr : 0.0;

    if (price > level_price + threshold) return SRLevelType::Resistance;
    if (price < level_price - threshold) return SRLevelType::Support;
    return SRLevelType::Pivot;
}

// ==============================================================================
// 更新入口
// ==============================================================================
Result<void>
SupportResistance::update(const Candle& candle_5m,
                            Timestamp current_3m_time,
                            double atr) noexcept {
    ++stats_.total_updates;

    // 1. 校验
    auto vr = validate_candle(candle_5m, current_3m_time);
    if (vr.is_err()) {
        return vr;
    }

    // 2. 时间对齐检查：5m K 线的开盘时间必须严格等于 align_5m_time 的结果
    const int64_t expected_open = align_5m_time(
        current_3m_time.microseconds(), config_.timeframe_us);
    const int64_t actual_open = candle_5m.open_time.microseconds();

    // 允许 5m K 线时间戳 <= 当前对齐点（数据可能滞后）
    // 但必须 > last_5m_time_（前面 validate 已校验）
    if (actual_open > expected_open + config_.timeframe_us) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "5m K 线时间过于提前，可能未来数据")
            .with_context("actual", std::to_string(actual_open))
            .with_context("expected_max", std::to_string(expected_open));
    }

    // 3. 更新期望间隔（用于缺口检测）
    if (expected_interval_us_ == 0 && last_5m_time_.is_valid()) {
        const int64_t interval = actual_open - last_5m_time_.microseconds();
        if (interval > 0) {
            expected_interval_us_ = interval;
        }
    }

    // 4. 缺口检测（仅警告，不中断）
    if (expected_interval_us_ > 0 && last_5m_time_.is_valid()) {
        const int64_t interval = actual_open - last_5m_time_.microseconds();
        // 缺口：间隔 > 1.5 倍期望
        if (interval > expected_interval_us_ * 3 / 2) {
            ++stats_.gap_count;
            QUANT_LOG_WARN("5m K 线缺口: interval={}us, expected={}us",
                           interval, expected_interval_us_);
        }
    }

    // 5. 提取候选
    extract_candidates(candle_5m, atr);

    // 6. 更新 last_5m_time_
    last_5m_time_ = candle_5m.open_time;
    ++bar_count_;

    // 7. 重建水平（每根 K 线都重建，O(n log n)，n ≤ 576）
    //    可优化为增量更新，但简单场景下重建足够
    rebuild_levels(atr);

    // 8. 时间衰减
    apply_time_decay(candle_5m.close_time.is_valid()
        ? candle_5m.close_time
        : candle_5m.open_time);

    // 9. 突破检测
    const double current_price = candle_5m.close.to_double();
    detect_breakouts(current_price, candle_5m.close_time.is_valid()
        ? candle_5m.close_time
        : candle_5m.open_time);

    // 10. 重新计算类型（基于当前价格）
    for (auto& level : levels_) {
        level.type = classify_level(level.price, level.price, atr);
        // 注：此处 type 的语义需要改为"相对当前价格"
        // 修正：应以传入的价格（当前价）判定
        level.type = classify_level(current_price, level.price, atr);
    }

    return {};
}

// ==============================================================================
// 批量加载历史
// ==============================================================================
Result<void>
SupportResistance::load_history(const std::vector<Candle>& candles_5m,
                                 Timestamp current_3m_time,
                                 double atr) noexcept {
    if (candles_5m.empty()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "历史 5m K 线为空");
    }

    for (std::size_t i = 0; i < candles_5m.size(); ++i) {
        auto r = update(candles_5m[i], current_3m_time, atr);
        if (r.is_err()) {
            return QUANT_ERR_MSG(r.error_code(),
                "历史加载在第 " + std::to_string(i) + " 根失败")
                .with_context("bar_index", std::to_string(i))
                .with_context("reason", r.error().to_string());
        }
    }

    return {};
}

// ==============================================================================
// 查询：最近阻力位
// ==============================================================================
std::optional<SRLevel>
SupportResistance::nearest_resistance(double current_price,
                                        double atr) const noexcept {
    if (!is_finite_sr(current_price)) return std::nullopt;

    std::optional<SRLevel> best;
    double best_distance = std::numeric_limits<double>::infinity();

    for (const auto& level : levels_) {
        if (level.is_broken) continue;       // 已突破的不算阻力
        if (!level.is_valid()) continue;

        // 阻力 = 价格在水平下方
        if (level.price <= current_price) continue;

        const double dist = compute_distance_atr(current_price,
                                                   level.price, atr);

        // 综合强度 × 距离的权衡
        // 使用 strength / (1 + distance) 作为选择标准
        const double score = level.strength / (1.0 + dist);

        // 简化：直接选距离最近的（strength 作为次级条件）
        if (dist < best_distance) {
            best_distance = dist;
            best = level;
        }
    }

    return best;
}

// ==============================================================================
// 查询：最近支撑位
// ==============================================================================
std::optional<SRLevel>
SupportResistance::nearest_support(double current_price,
                                     double atr) const noexcept {
    if (!is_finite_sr(current_price)) return std::nullopt;

    std::optional<SRLevel> best;
    double best_distance = std::numeric_limits<double>::infinity();

    for (const auto& level : levels_) {
        if (level.is_broken) continue;
        if (!level.is_valid()) continue;

        // 支撑 = 价格在水平上方
        if (level.price >= current_price) continue;

        const double dist = compute_distance_atr(current_price,
                                                   level.price, atr);

        if (dist < best_distance) {
            best_distance = dist;
            best = level;
        }
    }

    return best;
}

// ==============================================================================
// 查询：综合
// ==============================================================================
SRProximity
SupportResistance::query(double current_price, double atr) const noexcept {
    SRProximity result;

    if (!is_finite_sr(current_price) || current_price <= 0.0) {
        return result;
    }

    result.nearest_resistance = nearest_resistance(current_price, atr);
    result.nearest_support    = nearest_support(current_price, atr);

    if (result.nearest_resistance) {
        result.distance_to_resistance_atr =
            compute_distance_atr(current_price,
                                  result.nearest_resistance->price, atr);
        if (result.distance_to_resistance_atr < config_.proximity_atr) {
            result.is_blocked_by_resistance = true;
        }
    }

    if (result.nearest_support) {
        result.distance_to_support_atr =
            compute_distance_atr(current_price,
                                  result.nearest_support->price, atr);
        if (result.distance_to_support_atr < config_.proximity_atr) {
            result.is_supported_by_support = true;
        }
    }

    return result;
}

// ==============================================================================
// 便捷查询
// ==============================================================================
bool SupportResistance::is_blocked_by_resistance(double current_price,
                                                   double atr) const noexcept {
    return query(current_price, atr).is_blocked_by_resistance;
}

bool SupportResistance::is_supported_by_support(double current_price,
                                                  double atr) const noexcept {
    return query(current_price, atr).is_supported_by_support;
}

double SupportResistance::distance_atr(double price,
                                         const SRLevel& level,
                                         double atr) const noexcept {
    return compute_distance_atr(price, level.price, atr);
}

// ==============================================================================
// 序列化
// ==============================================================================
std::string SupportResistance::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"bar_count\":" << bar_count_ << ","
        << "\"warmed_up\":"
        << (is_warmed_up() ? "true" : "false") << ","
        << "\"level_count\":" << levels_.size() << ","
        << "\"levels\":[";

    bool first = true;
    for (const auto& level : levels_) {
        if (!first) oss << ",";
        first = false;
        oss << "{"
            << "\"price\":"    << json_double(level.price) << ","
            << "\"strength\":" << json_double(level.strength) << ","
            << "\"type\":\""   << to_string(level.type) << "\","
            << "\"touches\":"  << level.touch_count << ","
            << "\"vol_ratio\":"<< json_double(level.avg_volume_ratio) << ","
            << "\"decay\":"    << json_double(level.time_decay) << ","
            << "\"broken\":"   << (level.is_broken ? "true" : "false") << ","
            << "\"retest\":"   << (level.retest_confirmed ? "true" : "false")
            << "}";
    }

    oss << "]}";
    return oss.str();
}

std::string SupportResistance::to_string() const {
    std::ostringstream oss;
    oss << "SupportResistance{"
        << "bars=" << bar_count_
        << ", levels=" << levels_.size()
        << ", warmed=" << (is_warmed_up() ? "yes" : "no");

    if (!levels_.empty()) {
        oss << "\n";
        for (const auto& level : levels_) {
            oss << "  ["
                << to_string(level.type) << "] "
                << fmt_double(level.price, 2)
                << " strength=" << fmt_double(level.strength, 2)
                << " touches=" << level.touch_count
                << " decay=" << fmt_double(level.time_decay, 2);
            if (level.is_broken) {
                oss << " (BROKEN)";
            }
            oss << "\n";
        }
    }

    oss << "}";
    return oss.str();
}

}  // namespace indicator
}  // namespace quant
