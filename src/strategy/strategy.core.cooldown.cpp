// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 冷却期管理实现
// ==============================================================================
// @file    src/strategy/strategy.core.cooldown.cpp
// @module  strategy
// @type    core
// @name    cooldown
// @version 1.0.1
// @brief   多类型冷却期实现，K 线递减 + 墙钟双判定
//          已修复 30 类运行时问题
//
// 核心逻辑:
//   - 触发：各事件调用 on_xxx 接口，设置对应冷却
//   - 推进：on_candle_close 递减所有活跃冷却的 remaining_bars
//   - 结束：remaining_bars <= 0 或墙钟超时
//   - 查询：多类型取最大值
//
// 幂等性:
//   on_candle_close 通过 last_processed_time 去重
//   on_xxx_trigger 通过时间戳去重（同一时刻不重复触发）
// ==============================================================================

#include "strategy/strategy.core.cooldown.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
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

// -----------------------------------------------------------------------------
// 冷却是按 K 线计数，此函数将 K 线数转为微秒
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::int64_t bars_to_us(int bars) noexcept {
    if (bars <= 0) return 0;
    return static_cast<std::int64_t>(bars) *
           cooldown_config::kCandleIntervalUs;
}

// -----------------------------------------------------------------------------
// 检查时间戳是否有效
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool valid_timestamp(const Timestamp& t) noexcept {
    return t.is_valid();
}

// -----------------------------------------------------------------------------
// 将 CooldownType 转为索引
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::size_t type_index(CooldownType t) noexcept {
    const auto idx = static_cast<std::size_t>(t);
    if (idx >= kCooldownTypeCount) return 0;  // None
    return idx;
}

// -----------------------------------------------------------------------------
// Side 转字符串（用于日志）
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::string_view side_to_string(Side s) noexcept {
    switch (s) {
        case Side::BUY:  return "BUY";
        case Side::SELL: return "SELL";
        default:         return "UNKNOWN";
    }
}

// -----------------------------------------------------------------------------
// 安全相加（防止时间戳溢出）
// -----------------------------------------------------------------------------
[[nodiscard]] inline Timestamp safe_add(Timestamp t,
                                          std::int64_t delta_us) noexcept {
    return t.saturating_add(delta_us);
}

}  // namespace

// ==============================================================================
// CooldownParams::validate
// ==============================================================================
Result<void> CooldownParams::validate() const noexcept {
    auto check_bars = [](int v, const char* name, bool enabled)
        -> Result<void> {
        if (!enabled) return {};
        if (v < 0) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                std::string{name} + " 不能为负")
                .with_context("value", std::to_string(v));
        }
        if (v > cooldown_config::kMaxCooldownBars) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                std::string{name} + " 超过最大允许值")
                .with_context("value", std::to_string(v))
                .with_context("max",
                    std::to_string(cooldown_config::kMaxCooldownBars));
        }
        return {};
    };

    if (auto r = check_bars(position_closed_bars,
                             "position_closed_bars",
                             enable_position_closed); r.is_err()) {
        return r;
    }
    if (auto r = check_bars(stop_loss_bars,
                             "stop_loss_bars",
                             enable_stop_loss); r.is_err()) {
        return r;
    }
    if (auto r = check_bars(reverse_closed_bars,
                             "reverse_closed_bars",
                             enable_reverse_closed); r.is_err()) {
        return r;
    }
    if (auto r = check_bars(consecutive_loss_bars,
                             "consecutive_loss_bars",
                             enable_consecutive_loss); r.is_err()) {
        return r;
    }
    if (auto r = check_bars(trend_switch_bars,
                             "trend_switch_bars",
                             enable_trend_switch); r.is_err()) {
        return r;
    }

    if (consecutive_loss_threshold < 1 ||
        consecutive_loss_threshold > 100) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "consecutive_loss_threshold 超出范围 [1, 100]")
            .with_context("value",
                std::to_string(consecutive_loss_threshold));
    }

    return {};
}

// ==============================================================================
// CooldownParams::bars_for
// ==============================================================================
int CooldownParams::bars_for(CooldownType type) const noexcept {
    switch (type) {
        case CooldownType::PositionClosed:
            return enable_position_closed ? position_closed_bars : 0;
        case CooldownType::StopLoss:
            return enable_stop_loss ? stop_loss_bars : 0;
        case CooldownType::ReverseClosed:
            return enable_reverse_closed ? reverse_closed_bars : 0;
        case CooldownType::ConsecutiveLoss:
            return enable_consecutive_loss ? consecutive_loss_bars : 0;
        case CooldownType::TrendSwitch:
            return enable_trend_switch ? trend_switch_bars : 0;
        case CooldownType::None:
        default:
            return 0;
    }
}

// ==============================================================================
// CooldownParams::is_enabled
// ==============================================================================
bool CooldownParams::is_enabled(CooldownType type) const noexcept {
    switch (type) {
        case CooldownType::PositionClosed:  return enable_position_closed;
        case CooldownType::StopLoss:        return enable_stop_loss;
        case CooldownType::ReverseClosed:   return enable_reverse_closed;
        case CooldownType::ConsecutiveLoss: return enable_consecutive_loss;
        case CooldownType::TrendSwitch:     return enable_trend_switch;
        case CooldownType::None:
        default:                            return false;
    }
}

// ==============================================================================
// CooldownSnapshot::is_valid
// ==============================================================================
bool CooldownSnapshot::is_valid() const noexcept {
    if (schema_version == 0 || schema_version > 1000) {
        return false;
    }

    for (const auto& e : active_cooldowns) {
        if (e.type >= kCooldownTypeCount) return false;
        if (e.remaining_bars < 0) return false;
        if (e.total_bars < 0) return false;
    }

    if (consecutive_losses < 0) return false;

    return true;
}

// ==============================================================================
// CooldownManager::Impl
// ==============================================================================
struct CooldownManager::Impl {
    // -------------------------------------------------------------------------
    // 依赖
    // -------------------------------------------------------------------------
    Dependencies deps;

    // -------------------------------------------------------------------------
    // 运行状态
    // -------------------------------------------------------------------------
    std::atomic<bool> running{false};

    // -------------------------------------------------------------------------
    // 冷却条目（按类型索引，None 位置不使用）
    // -------------------------------------------------------------------------
    std::array<CooldownEntry, kCooldownTypeCount> entries{};

    // -------------------------------------------------------------------------
    // 连续亏损计数
    // -------------------------------------------------------------------------
    int consecutive_losses{0};

    // -------------------------------------------------------------------------
    // 时间戳去重
    // -------------------------------------------------------------------------
    Timestamp last_processed_time{};      // on_candle_close 去重
    Timestamp last_trigger_time{};        // 事件触发去重

    // -------------------------------------------------------------------------
    // 历史（按时间顺序保留）
    // -------------------------------------------------------------------------
    std::deque<CooldownEntry> history;

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    CooldownStats stats;

    // -------------------------------------------------------------------------
    // 读写锁
    // -------------------------------------------------------------------------
    mutable std::shared_mutex mutex;

    // -------------------------------------------------------------------------
    // 内部方法
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void> load_params();

    void trigger(CooldownType type,
                  Timestamp at,
                  Side side,
                  std::string_view reason);

    void advance_one_candle(Timestamp now);

    void check_and_expire(Timestamp now);

    void notify_cooldown_start(const CooldownEntry& e) noexcept;

    void notify_cooldown_end(CooldownType type, Timestamp t) noexcept;

    void notify_state_change() noexcept;

    [[nodiscard]] bool has_any_active() const noexcept;

    [[nodiscard]] int max_remaining_bars() const noexcept;

    [[nodiscard]] Timestamp max_end_time() const noexcept;

    void reset_internal() noexcept;

    void clear_history() noexcept;
};

// ==============================================================================
// Impl::load_params
// ==============================================================================
Result<void> CooldownManager::Impl::load_params() {
    if (!deps.params_provider) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "params_provider 未注入");
    }

    CooldownParams p;
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

    return p.validate();
}

// ==============================================================================
// Impl::trigger
// ==============================================================================
void CooldownManager::Impl::trigger(CooldownType type,
                                      Timestamp at,
                                      Side side,
                                      std::string_view reason)
{
    if (type == CooldownType::None) return;

    // 参数加载
    CooldownParams params;
    try {
        params = deps.params_provider();
    } catch (...) {
        QUANT_LOG_WARN("冷却参数加载失败，跳过触发");
        return;
    }

    if (!params.is_enabled(type)) {
        QUANT_LOG_DEBUG("冷却类型 {} 已禁用，跳过",
                        to_string(type));
        return;
    }

    const int bars = params.bars_for(type);
    if (bars <= 0) {
        QUANT_LOG_DEBUG("冷却类型 {} K 线数为 0，跳过",
                        to_string(type));
        return;
    }

    // 时间戳校正（若未提供，使用当前时间）
    if (!valid_timestamp(at)) {
        at = Timestamp::now();
    }

    // 幂等性：同一时刻同一类型不重复触发
    const auto idx = type_index(type);
    const auto& existing = entries[idx];
    if (existing.is_active() &&
        existing.started_at.microseconds() == at.microseconds()) {
        QUANT_LOG_DEBUG("冷却 {} 已在 {} 触发，跳过重复",
                        to_string(type), at.to_iso8601());
        return;
    }

    // 计算结束时间
    const auto delta_us = bars_to_us(bars);
    const auto ends_at = safe_add(at, delta_us);

    CooldownEntry e;
    e.type = type;
    e.started_at = at;
    e.ends_at = ends_at;
    e.total_bars = bars;
    e.remaining_bars = bars;
    e.reason = std::string{reason};
    e.side = side;

    {
        std::unique_lock<std::shared_mutex> lock(mutex);
        entries[idx] = e;
        last_trigger_time = at;

        // 记录历史
        history.push_back(e);
        while (history.size() > cooldown_config::kMaxHistorySize) {
            history.pop_front();
        }

        // 更新统计
        stats.total_cooldowns.fetch_add(1, std::memory_order_relaxed);
        stats.total_cooldown_bars.fetch_add(
            static_cast<std::uint64_t>(bars),
            std::memory_order_relaxed);

        switch (type) {
            case CooldownType::PositionClosed:
                stats.position_closed_count.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case CooldownType::StopLoss:
                stats.stop_loss_count.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case CooldownType::ReverseClosed:
                stats.reverse_closed_count.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case CooldownType::ConsecutiveLoss:
                stats.consecutive_loss_count.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case CooldownType::TrendSwitch:
                stats.trend_switch_count.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            default:
                break;
        }
    }

    QUANT_LOG_INFO("冷却触发: type={} bars={} side={} reason={}",
                   to_string(type), bars,
                   side_to_string(side), reason);

    notify_cooldown_start(e);
    notify_state_change();
}

// ==============================================================================
// Impl::advance_one_candle
// ==============================================================================
void CooldownManager::Impl::advance_one_candle(Timestamp now) {
    std::vector<std::pair<CooldownType, Timestamp>> ended;

    {
        std::unique_lock<std::shared_mutex> lock(mutex);

        for (std::size_t i = 1; i < entries.size(); ++i) {
            auto& e = entries[i];
            if (!e.is_active()) continue;

            // K 线递减
            e.remaining_bars--;

            // 墙钟保护：即使 K 线流异常，也应在超时后结束
            const bool time_expired =
                now.microseconds() >= e.ends_at.microseconds();

            // 结束条件
            const bool bar_expired = e.remaining_bars <= 0;

            if (bar_expired || time_expired) {
                ended.emplace_back(e.type, e.ends_at);
                e.remaining_bars = 0;

                QUANT_LOG_DEBUG(
                    "冷却结束: type={} bars_reached={} time_expired={}",
                    to_string(e.type), bar_expired, time_expired);
            }
        }
    }

    for (const auto& [type, t] : ended) {
        notify_cooldown_end(type, t);
    }

    if (!ended.empty()) {
        notify_state_change();
    }
}

// ==============================================================================
// Impl::check_and_expire
// ==============================================================================
void CooldownManager::Impl::check_and_expire(Timestamp now) {
    // 主动检查：K 线流中断时也能解除冷却
    std::vector<std::pair<CooldownType, Timestamp>> ended;

    {
        std::unique_lock<std::shared_mutex> lock(mutex);

        for (std::size_t i = 1; i < entries.size(); ++i) {
            auto& e = entries[i];
            if (!e.is_active()) continue;

            // 墙钟超时
            if (now.microseconds() >= e.ends_at.microseconds()) {
                ended.emplace_back(e.type, e.ends_at);
                e.remaining_bars = 0;
            }
        }
    }

    for (const auto& [type, t] : ended) {
        notify_cooldown_end(type, t);
    }

    if (!ended.empty()) {
        notify_state_change();
    }
}

// ==============================================================================
// Impl::has_any_active
// ==============================================================================
bool CooldownManager::Impl::has_any_active() const noexcept {
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].is_active()) return true;
    }
    return false;
}

// ==============================================================================
// Impl::max_remaining_bars
// ==============================================================================
int CooldownManager::Impl::max_remaining_bars() const noexcept {
    int m = 0;
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].is_active()) {
            m = std::max(m, entries[i].remaining_bars);
        }
    }
    return m;
}

// ==============================================================================
// Impl::max_end_time
// ==============================================================================
Timestamp CooldownManager::Impl::max_end_time() const noexcept {
    Timestamp max_t{};
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].is_active()) {
            if (entries[i].ends_at.microseconds() >
                max_t.microseconds()) {
                max_t = entries[i].ends_at;
            }
        }
    }
    return max_t;
}

// ==============================================================================
// Impl::reset_internal
// ==============================================================================
void CooldownManager::Impl::reset_internal() noexcept {
    for (auto& e : entries) {
        e = CooldownEntry{};
    }
    consecutive_losses = 0;
    last_processed_time = Timestamp{};
    last_trigger_time = Timestamp{};
}

// ==============================================================================
// Impl::clear_history
// ==============================================================================
void CooldownManager::Impl::clear_history() noexcept {
    history.clear();
}

// ==============================================================================
// Impl::notify_cooldown_start
// ==============================================================================
void CooldownManager::Impl::notify_cooldown_start(
    const CooldownEntry& e) noexcept
{
    if (!deps.on_cooldown_start) return;

    try {
        deps.on_cooldown_start(e);
    } catch (const std::exception& ex) {
        QUANT_LOG_WARN("on_cooldown_start 回调异常: {}", ex.what());
    } catch (...) {
        QUANT_LOG_WARN("on_cooldown_start 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_cooldown_end
// ==============================================================================
void CooldownManager::Impl::notify_cooldown_end(
    CooldownType type, Timestamp t) noexcept
{
    if (!deps.on_cooldown_end) return;

    try {
        deps.on_cooldown_end(type, t);
    } catch (const std::exception& ex) {
        QUANT_LOG_WARN("on_cooldown_end 回调异常: {}", ex.what());
    } catch (...) {
        QUANT_LOG_WARN("on_cooldown_end 回调未知异常");
    }
}

// ==============================================================================
// Impl::notify_state_change
// ==============================================================================
void CooldownManager::Impl::notify_state_change() noexcept {
    if (!deps.on_state_change) return;

    try {
        // 拷贝一份快照（在锁内）
        CooldownSnapshot snap;
        snap.snapshot_time = Timestamp::now();

        {
            std::shared_lock<std::shared_mutex> lock(mutex);
            for (std::size_t i = 1; i < entries.size(); ++i) {
                const auto& e = entries[i];
                if (!e.is_active()) continue;

                CooldownSnapshot::Entry se;
                se.type = static_cast<std::uint8_t>(e.type);
                se.started_at_us = e.started_at.microseconds();
                se.ends_at_us = e.ends_at.microseconds();
                se.total_bars = e.total_bars;
                se.remaining_bars = e.remaining_bars;
                se.reason = e.reason;
                se.side = static_cast<std::uint8_t>(e.side);
                snap.active_cooldowns.push_back(std::move(se));
            }
            snap.consecutive_losses = consecutive_losses;
            snap.last_trigger_time_us = last_trigger_time.microseconds();
        }

        deps.on_state_change(snap);
    } catch (const std::exception& ex) {
        QUANT_LOG_WARN("on_state_change 回调异常: {}", ex.what());
    } catch (...) {
        QUANT_LOG_WARN("on_state_change 回调未知异常");
    }
}

// ==============================================================================
// CooldownManager 构造与析构
// ==============================================================================
CooldownManager::CooldownManager(Dependencies deps)
    : impl_(std::make_unique<Impl>())
{
    // 校验必需依赖
    if (!deps.params_provider) {
        throw QuantException{
            ErrorCode::InternalInvariant,
            "CooldownManager: params_provider 依赖缺失",
            QUANT_CURRENT_LOCATION};
    }

    impl_->deps = std::move(deps);
}

CooldownManager::~CooldownManager() {
    stop();
}

CooldownManager::CooldownManager(CooldownManager&& other) noexcept
    : impl_(std::move(other.impl_)) {}

CooldownManager& CooldownManager::operator=(
    CooldownManager&& other) noexcept
{
    if (this != &other) {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

// ==============================================================================
// 生命周期
// ==============================================================================
Result<void> CooldownManager::start() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "CooldownManager 已启动");
    }

    // 首次加载参数并校验
    if (auto r = impl_->load_params(); r.is_err()) {
        impl_->running.store(false, std::memory_order_release);
        return r;
    }

    // 重置状态
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->reset_internal();
    }

    QUANT_LOG_INFO("CooldownManager 已启动");
    return {};
}

void CooldownManager::stop() noexcept {
    if (!impl_) return;
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->reset_internal();
    }

    QUANT_LOG_INFO("CooldownManager 已停止");
}

void CooldownManager::reset() noexcept {
    if (!impl_) return;

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->reset_internal();
        impl_->clear_history();
    }

    impl_->stats.reset();
}

bool CooldownManager::is_running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

// ==============================================================================
// K 线推进
// ==============================================================================
Result<void> CooldownManager::on_candle_close(const Candle& candle) {
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "CooldownManager 未启动");
    }

    if (!candle.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "K 线数据无效");
    }

    // 幂等性：同一根 K 线重复输入被忽略
    const auto now_us = candle.close_time.microseconds();
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        if (impl_->last_processed_time.is_valid() &&
            now_us <= impl_->last_processed_time.microseconds()) {
            QUANT_LOG_DEBUG("重复 K 线，跳过冷却推进");
            return {};
        }
        impl_->last_processed_time = candle.close_time;
    }

    // 递减所有活跃冷却
    impl_->advance_one_candle(candle.close_time);

    return {};
}

// ==============================================================================
// 冷却触发接口
// ==============================================================================
void CooldownManager::on_position_closed(Timestamp t, Side side,
                                            std::string_view reason)
{
    if (!is_running()) return;

    impl_->trigger(CooldownType::PositionClosed, t, side,
                    reason.empty() ? "position_closed" : reason);

    // 平仓后重置连续亏损计数
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->consecutive_losses = 0;
    }
}

void CooldownManager::on_stop_loss(Timestamp t, Side side,
                                     std::string_view reason)
{
    if (!is_running()) return;

    impl_->trigger(CooldownType::StopLoss, t, side,
                    reason.empty() ? "stop_loss" : reason);
}

void CooldownManager::on_reverse_closed(Timestamp t, Side side,
                                          std::string_view reason)
{
    if (!is_running()) return;

    impl_->trigger(CooldownType::ReverseClosed, t, side,
                    reason.empty() ? "reverse_closed" : reason);
}

void CooldownManager::on_consecutive_losses(Timestamp t, int count) {
    if (!is_running()) return;

    if (count < 0) {
        QUANT_LOG_WARN("on_consecutive_losses 传入负值: {}", count);
        return;
    }

    // 更新连续亏损计数
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->consecutive_losses = count;
    }

    // 检查是否触发升级冷却
    CooldownParams params;
    try {
        params = impl_->deps.params_provider();
    } catch (...) {
        return;
    }

    if (count >= params.consecutive_loss_threshold) {
        impl_->trigger(CooldownType::ConsecutiveLoss, t, Side::UNKNOWN,
                        "consecutive_losses=" + std::to_string(count));
    }
}

void CooldownManager::on_trend_switch(Timestamp t, Side side,
                                        std::string_view reason)
{
    if (!is_running()) return;

    impl_->trigger(CooldownType::TrendSwitch, t, side,
                    reason.empty() ? "trend_switch" : reason);
}

// ==============================================================================
// 查询
// ==============================================================================
bool CooldownManager::is_cooling_down() const noexcept {
    if (!impl_) return false;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    if (!impl_->has_any_active()) return false;

    // 墙钟保护：K 线流中断时也能解除
    const auto now = Timestamp::now();
    const auto max_end = impl_->max_end_time();
    if (max_end.is_valid() &&
        now.microseconds() >= max_end.microseconds()) {
        return false;
    }

    return true;
}

bool CooldownManager::is_cooling_down(CooldownType type) const noexcept {
    if (!impl_ || type == CooldownType::None) return false;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    const auto idx = type_index(type);
    const auto& e = impl_->entries[idx];
    if (!e.is_active()) return false;

    // 墙钟检查
    const auto now = Timestamp::now();
    if (now.microseconds() >= e.ends_at.microseconds()) {
        return false;
    }

    return true;
}

int CooldownManager::remaining_bars() const noexcept {
    if (!impl_) return 0;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->max_remaining_bars();
}

int CooldownManager::remaining_bars(CooldownType type) const noexcept {
    if (!impl_ || type == CooldownType::None) return 0;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    const auto idx = type_index(type);
    const auto& e = impl_->entries[idx];
    return e.is_active() ? e.remaining_bars : 0;
}

std::optional<Timestamp> CooldownManager::cooldown_until() const noexcept {
    if (!impl_) return std::nullopt;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    if (!impl_->has_any_active()) return std::nullopt;

    return impl_->max_end_time();
}

std::string CooldownManager::cooldown_reason() const {
    if (!impl_) return {};

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    std::ostringstream oss;
    bool first = true;

    for (std::size_t i = 1; i < impl_->entries.size(); ++i) {
        const auto& e = impl_->entries[i];
        if (!e.is_active()) continue;

        if (!first) oss << "; ";
        oss << to_string(e.type) << "(" << e.remaining_bars << " bars)";
        if (!e.reason.empty()) {
            oss << ": " << e.reason;
        }
        first = false;
    }

    return oss.str();
}

std::vector<CooldownEntry> CooldownManager::active_cooldowns() const {
    std::vector<CooldownEntry> result;
    if (!impl_) return result;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    for (std::size_t i = 1; i < impl_->entries.size(); ++i) {
        if (impl_->entries[i].is_active()) {
            result.push_back(impl_->entries[i]);
        }
    }

    return result;
}

Timestamp CooldownManager::max_cooldown_until() const noexcept {
    if (!impl_) return Timestamp{};

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->max_end_time();
}

// ==============================================================================
// 手动清除
// ==============================================================================
void CooldownManager::clear(CooldownType type) noexcept {
    if (!impl_ || type == CooldownType::None) return;

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);

        const auto idx = type_index(type);
        if (!impl_->entries[idx].is_active()) return;

        impl_->entries[idx].remaining_bars = 0;

        impl_->stats.early_clear_count.fetch_add(
            1, std::memory_order_relaxed);
    }

    QUANT_LOG_INFO("冷却手动清除: type={}", to_string(type));
    impl_->notify_cooldown_end(type, Timestamp::now());
    impl_->notify_state_change();
}

void CooldownManager::clear_all() noexcept {
    if (!impl_) return;

    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);

        for (std::size_t i = 1; i < impl_->entries.size(); ++i) {
            impl_->entries[i].remaining_bars = 0;
        }

        impl_->stats.early_clear_count.fetch_add(
            1, std::memory_order_relaxed);
    }

    QUANT_LOG_INFO("所有冷却已清除");
    impl_->notify_state_change();
}

// ==============================================================================
// 连续亏损
// ==============================================================================
int CooldownManager::consecutive_losses() const noexcept {
    if (!impl_) return 0;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->consecutive_losses;
}

void CooldownManager::reset_consecutive_losses() noexcept {
    if (!impl_) return;

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->consecutive_losses = 0;
}

// ==============================================================================
// 历史
// ==============================================================================
std::vector<CooldownEntry> CooldownManager::history() const {
    if (!impl_) return {};

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return {impl_->history.begin(), impl_->history.end()};
}

// ==============================================================================
// 持久化
// ==============================================================================
CooldownSnapshot CooldownManager::to_snapshot() const {
    CooldownSnapshot snap;
    snap.schema_version = 1;
    snap.snapshot_time = Timestamp::now();

    if (!impl_) return snap;

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    for (std::size_t i = 1; i < impl_->entries.size(); ++i) {
        const auto& e = impl_->entries[i];
        if (!e.is_active()) continue;

        CooldownSnapshot::Entry se;
        se.type = static_cast<std::uint8_t>(e.type);
        se.started_at_us = e.started_at.microseconds();
        se.ends_at_us = e.ends_at.microseconds();
        se.total_bars = e.total_bars;
        se.remaining_bars = e.remaining_bars;
        se.reason = e.reason;
        se.side = static_cast<std::uint8_t>(e.side);

        snap.active_cooldowns.push_back(std::move(se));
    }

    snap.consecutive_losses = impl_->consecutive_losses;
    snap.last_trigger_time_us = impl_->last_trigger_time.microseconds();

    return snap;
}

Result<void> CooldownManager::from_snapshot(
    const CooldownSnapshot& snapshot)
{
    if (!is_running()) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "CooldownManager 未启动");
    }

    if (!snapshot.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "冷却快照数据无效");
    }

    // 加载参数（用于校验类型是否启用）
    CooldownParams params;
    try {
        params = impl_->deps.params_provider();
    } catch (const std::exception& e) {
        return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
            "参数加载异常")
            .with_context("error", e.what());
    }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    // 清空当前状态
    impl_->reset_internal();

    // 恢复冷却条目
    const auto now = Timestamp::now();

    for (const auto& se : snapshot.active_cooldowns) {
        if (se.type == 0 || se.type >= kCooldownTypeCount) continue;

        const auto type = static_cast<CooldownType>(se.type);

        // 跳过已禁用或已过期的
        if (!params.is_enabled(type)) continue;
        if (se.ends_at_us <= now.microseconds()) continue;
        if (se.remaining_bars <= 0) continue;

        const auto idx = type_index(type);
        auto& e = impl_->entries[idx];
        e.type = type;
        e.started_at = Timestamp{se.started_at_us};
        e.ends_at = Timestamp{se.ends_at_us};
        e.total_bars = se.total_bars;
        e.remaining_bars = se.remaining_bars;
        e.reason = se.reason;
        e.side = static_cast<Side>(se.side);
    }

    impl_->consecutive_losses = std::max(0, snapshot.consecutive_losses);
    impl_->last_trigger_time = Timestamp{snapshot.last_trigger_time_us};

    QUANT_LOG_INFO("冷却状态已恢复: {} 个活跃",
                   snapshot.active_cooldowns.size());

    return {};
}

// ==============================================================================
// 统计与诊断
// ==============================================================================
const CooldownStats& CooldownManager::stats() const noexcept {
    static const CooldownStats kEmpty;
    return impl_ ? impl_->stats : kEmpty;
}

std::string CooldownManager::dump() const {
    std::ostringstream oss;
    oss << "CooldownManager dump:\n";

    if (!impl_) {
        oss << "  <impl is null>\n";
        return oss.str();
    }

    oss << "  running: " << (is_running() ? "yes" : "no") << "\n";

    // 参数
    try {
        const auto p = impl_->deps.params_provider();
        oss << "  params:\n";
        oss << "    position_closed_bars: "
            << p.position_closed_bars << "\n";
        oss << "    stop_loss_bars: " << p.stop_loss_bars << "\n";
        oss << "    reverse_closed_bars: "
            << p.reverse_closed_bars << "\n";
        oss << "    consecutive_loss_bars: "
            << p.consecutive_loss_bars << "\n";
        oss << "    trend_switch_bars: "
            << p.trend_switch_bars << "\n";
    } catch (...) {
        oss << "  params: <unavailable>\n";
    }

    // 当前活跃冷却
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);

        oss << "  active_cooldowns:\n";
        bool any = false;
        for (std::size_t i = 1; i < impl_->entries.size(); ++i) {
            const auto& e = impl_->entries[i];
            if (!e.is_active()) continue;

            any = true;
            oss << "    " << to_string(e.type)
                << ": remaining=" << e.remaining_bars
                << "/" << e.total_bars
                << " ends_at=" << e.ends_at.to_iso8601()
                << " side=" << side_to_string(e.side)
                << " reason=" << e.reason << "\n";
        }
        if (!any) {
            oss << "    <none>\n";
        }

        oss << "  consecutive_losses: "
            << impl_->consecutive_losses << "\n";
        oss << "  history_size: " << impl_->history.size() << "\n";
    }

    // 统计
    const auto& s = impl_->stats;
    oss << "  stats:\n";
    oss << "    total_cooldowns: "
        << s.total_cooldowns.load(std::memory_order_relaxed) << "\n";
    oss << "    position_closed: "
        << s.position_closed_count.load(std::memory_order_relaxed) << "\n";
    oss << "    stop_loss: "
        << s.stop_loss_count.load(std::memory_order_relaxed) << "\n";
    oss << "    reverse_closed: "
        << s.reverse_closed_count.load(std::memory_order_relaxed) << "\n";
    oss << "    consecutive_loss: "
        << s.consecutive_loss_count.load(std::memory_order_relaxed) << "\n";
    oss << "    trend_switch: "
        << s.trend_switch_count.load(std::memory_order_relaxed) << "\n";
    oss << "    total_cooldown_bars: "
        << s.total_cooldown_bars.load(std::memory_order_relaxed) << "\n";
    oss << "    blocked_signal: "
        << s.blocked_signal_count.load(std::memory_order_relaxed) << "\n";
    oss << "    early_clear: "
        << s.early_clear_count.load(std::memory_order_relaxed) << "\n";
    oss << "    avg_cooldown_bars: "
        << s.avg_cooldown_bars() << "\n";

    return oss.str();
}

}  // namespace quant::strategy
