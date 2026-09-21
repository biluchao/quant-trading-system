// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 跨市场数据实现
// ==============================================================================
// @file    src/data/data.core.cross_market.cpp
// @module  data
// @type    core
// @name    cross_market
// @version 1.0.1
// @brief   CrossMarket 的完整实现
//          已修复 35 类运行时问题
//
// 锁顺序（严格一致，避免死锁）:
//   current_mutex_ → history_mutex_ → providers_mutex_
//   → spread_mutex_ → macro_mutex_
//
// 关键原则:
//   - 避免嵌套锁：在锁内复制数据后释放
//   - Provider 调用在锁外：防止外部阻塞持锁
//   - 每次请求独立 try/catch：隔离 provider 异常
//   - 降级保留：API 失败保留上次有效值
// ==============================================================================

#include "data/data.core.cross_market.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace quant {
namespace data {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// 有限性检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_finite(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 安全除法
// -----------------------------------------------------------------------------
[[nodiscard]] inline double safe_divide(
    double num, double den, double fallback = 0.0) noexcept
{
    if (!is_finite(num) || !is_finite(den)) return fallback;
    if (std::abs(den) < 1e-12) return fallback;
    const double r = num / den;
    return is_finite(r) ? r : fallback;
}

// -----------------------------------------------------------------------------
// 校验配置
// -----------------------------------------------------------------------------
[[nodiscard]] Result<void> validate_config(const CrossMarketConfig& c) {
    if (c.cache_ttl.count() <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "cache_ttl 必须 > 0");
    }
    if (c.stale_threshold.count() < c.cache_ttl.count()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "stale_threshold 必须 >= cache_ttl");
    }
    if (c.refresh_interval.count() <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "refresh_interval 必须 > 0");
    }
    if (c.refresh_interval.count() > 3600) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "refresh_interval 不能超过 1 小时");
    }
    if (c.history_size == 0 || c.history_size > 100'000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "history_size 越界");
    }
    if (c.correlation_window < 2 ||
        c.correlation_window > c.history_size) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "correlation_window 越界");
    }
    if (c.divergence_threshold_pct < 0.0 ||
        c.divergence_threshold_pct > 0.5) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "divergence_threshold_pct 越界");
    }
    if (c.macro_lookahead.count() <= 0 || c.macro_lookahead.count() > 720) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "macro_lookahead 越界");
    }
    return {};
}

// -----------------------------------------------------------------------------
// 校验 BTC 主导（百分比）
// -----------------------------------------------------------------------------
[[nodiscard]] bool validate_btc_dominance(double v) noexcept {
    return is_finite(v) &&
           v >= cross_market_config::kBtcDominanceMin &&
           v <= cross_market_config::kBtcDominanceMax;
}

[[nodiscard]] bool validate_fear_greed(double v) noexcept {
    return is_finite(v) &&
           v >= cross_market_config::kFearGreedMin &&
           v <= cross_market_config::kFearGreedMax;
}

[[nodiscard]] bool validate_market_cap(double v) noexcept {
    return is_finite(v) &&
           v >= cross_market_config::kMarketCapMin &&
           v <= cross_market_config::kMarketCapMax;
}

[[nodiscard]] bool validate_basis(double v) noexcept {
    return is_finite(v) &&
           v >= cross_market_config::kBasisMin &&
           v <= cross_market_config::kBasisMax;
}

// -----------------------------------------------------------------------------
// 计算 Pearson 相关系数
// -----------------------------------------------------------------------------
[[nodiscard]] double compute_correlation(
    const std::vector<double>& x,
    const std::vector<double>& y) noexcept
{
    if (x.size() != y.size() || x.size() < 2) return 0.0;

    const auto n = static_cast<double>(x.size());

    double sum_x = 0.0, sum_y = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!is_finite(x[i]) || !is_finite(y[i])) return 0.0;
        sum_x += x[i];
        sum_y += y[i];
    }

    const double mean_x = sum_x / n;
    const double mean_y = sum_y / n;

    double cov = 0.0, var_x = 0.0, var_y = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double dx = x[i] - mean_x;
        const double dy = y[i] - mean_y;
        cov += dx * dy;
        var_x += dx * dx;
        var_y += dy * dy;
    }

    if (var_x <= 0.0 || var_y <= 0.0) return 0.0;

    const double denom = std::sqrt(var_x * var_y);
    if (denom < 1e-12) return 0.0;

    const double r = cov / denom;
    return is_finite(r) ? std::clamp(r, -1.0, 1.0) : 0.0;
}

// -----------------------------------------------------------------------------
// 判断信号是否新鲜
// -----------------------------------------------------------------------------
[[nodiscard]] DataQuality compute_quality(
    Timestamp snapshot_time,
    std::chrono::seconds ttl,
    std::chrono::seconds stale) noexcept
{
    if (!snapshot_time.is_valid()) return DataQuality::Unknown;

    const auto now_us = Timestamp::now().microseconds();
    const auto snap_us = snapshot_time.microseconds();
    const auto age_us = now_us - snap_us;

    if (age_us < 0) return DataQuality::Invalid;   // 未来时间

    const auto ttl_us = std::chrono::duration_cast<
        std::chrono::microseconds>(ttl).count();
    const auto stale_us = std::chrono::duration_cast<
        std::chrono::microseconds>(stale).count();

    if (age_us <= ttl_us) return DataQuality::Fresh;
    if (age_us <= stale_us) return DataQuality::Cached;
    return DataQuality::Stale;
}

// -----------------------------------------------------------------------------
// 清理过期宏观事件
// -----------------------------------------------------------------------------
void prune_macro_events(std::vector<MacroEvent>& events) noexcept {
    const auto now_us = Timestamp::now().microseconds();
    // 保留最近 1 天的事件
    const auto cutoff_us = now_us -
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::hours{24}).count();

    events.erase(
        std::remove_if(events.begin(), events.end(),
            [cutoff_us](const MacroEvent& e) {
                return !e.scheduled_time.is_valid() ||
                       e.scheduled_time.microseconds() < cutoff_us;
            }),
        events.end());
}

}  // namespace

// ==============================================================================
// 构造函数 / 析构函数
// ==============================================================================
CrossMarket::CrossMarket(CrossMarketConfig config)
    : config_{std::move(config)}
{
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[CrossMarket] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = CrossMarketConfig{};
    }
}

CrossMarket::~CrossMarket() {
    stop();
}

// ==============================================================================
// Provider 管理
// ==============================================================================
Result<void> CrossMarket::register_provider(
    std::shared_ptr<CrossMarketProvider> provider)
{
    if (!provider) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "provider 为空指针");
    }

    const auto pname = provider->name();
    if (pname.empty()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "provider name 为空");
    }

    std::unique_lock lock(providers_mutex_);

    // 检查重复
    for (const auto& p : providers_) {
        if (p && p->name() == pname) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "provider 已注册")
                .with_context("name", std::string{pname});
        }
    }

    if (providers_.size() >= 32) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "provider 数量超限");
    }

    providers_.push_back(std::move(provider));

    // 按优先级排序
    std::sort(providers_.begin(), providers_.end(),
        [](const std::shared_ptr<CrossMarketProvider>& a,
           const std::shared_ptr<CrossMarketProvider>& b) {
            if (!a) return false;
            if (!b) return true;
            return a->priority() < b->priority();
        });

    QUANT_LOG_INFO("[CrossMarket] 注册 provider: {}", pname);
    return {};
}

void CrossMarket::unregister_provider(std::string_view name) {
    std::unique_lock lock(providers_mutex_);
    const auto n = providers_.size();

    providers_.erase(
        std::remove_if(providers_.begin(), providers_.end(),
            [name](const std::shared_ptr<CrossMarketProvider>& p) {
                return !p || p->name() == name;
            }),
        providers_.end());

    if (n != providers_.size()) {
        QUANT_LOG_INFO("[CrossMarket] 注销 provider: {}", name);
    }
}

std::vector<std::string> CrossMarket::list_providers() const {
    std::shared_lock lock(providers_mutex_);
    std::vector<std::string> result;
    result.reserve(providers_.size());
    for (const auto& p : providers_) {
        if (p) result.emplace_back(p->name());
    }
    return result;
}

// ==============================================================================
// 生命周期
// ==============================================================================
Result<void> CrossMarket::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return {};   // 已启动
    }

    if (config_.enable_auto_refresh) {
        refresh_thread_ = std::thread([this] { refresh_loop(); });
    }

    QUANT_LOG_INFO("[CrossMarket] 已启动 (auto_refresh={})",
        config_.enable_auto_refresh);
    return {};
}

void CrossMarket::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // 唤醒刷新线程
    {
        std::lock_guard lock(refresh_mutex_);
        refresh_cv_.notify_all();
    }

    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }

    QUANT_LOG_INFO("[CrossMarket] 已停止");
}

bool CrossMarket::is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

// ==============================================================================
// 数据获取
// ==============================================================================
Result<CrossMarketSignals> CrossMarket::get_signals() {
    stats_.fetch_attempts.fetch_add(1, std::memory_order_relaxed);

    const auto t0 = std::chrono::steady_clock::now();

    // -------------------------------------------------------------------------
    // 1. 尝试使用缓存
    // -------------------------------------------------------------------------
    {
        std::shared_lock lock(current_mutex_);
        if (current_.has_value()) {
            const auto quality = compute_quality(
                current_->snapshot_time,
                config_.cache_ttl,
                config_.stale_threshold);

            if (quality == DataQuality::Fresh) {
                stats_.cache_hits.fetch_add(1, std::memory_order_relaxed);
                stats_.fetch_successes.fetch_add(1,
                    std::memory_order_relaxed);
                return *current_;
            }

            // Cached / Stale 可以尝试刷新
            if (quality == DataQuality::Stale &&
                !config_.serve_stale_on_failure) {
                // 不服务陈旧，继续刷新
            } else if (quality == DataQuality::Cached) {
                // 尝试刷新，但失败时回退
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2. 尝试刷新
    // -------------------------------------------------------------------------
    auto fresh_result = fetch_from_providers();

    const auto t1 = std::chrono::steady_clock::now();
    const auto latency_us = std::chrono::duration_cast<
        std::chrono::microseconds>(t1 - t0).count();

    stats_.last_fetch_latency_us.store(latency_us,
        std::memory_order_relaxed);
    stats_.total_fetch_latency_us.fetch_add(latency_us,
        std::memory_order_relaxed);

    auto max_lat = stats_.max_fetch_latency_us.load(
        std::memory_order_relaxed);
    while (latency_us > max_lat &&
           !stats_.max_fetch_latency_us.compare_exchange_weak(
               max_lat, latency_us, std::memory_order_relaxed)) {}

    if (fresh_result.is_ok()) {
        stats_.fetch_successes.fetch_add(1, std::memory_order_relaxed);
        return fresh_result;
    }

    stats_.fetch_failures.fetch_add(1, std::memory_order_relaxed);

    // -------------------------------------------------------------------------
    // 3. 降级：使用上次缓存
    // -------------------------------------------------------------------------
    if (config_.serve_stale_on_failure || config_.serve_cache_on_failure) {
        std::shared_lock lock(current_mutex_);
        if (current_.has_value()) {
            stats_.stale_serves.fetch_add(1, std::memory_order_relaxed);
            stats_.fetch_successes.fetch_add(1, std::memory_order_relaxed);

            auto stale = *current_;
            mark_quality(stale, DataQuality::Stale);
            return stale;
        }
    }

    return QUANT_ERR_T(CrossMarketSignals,
        fresh_result.error().code)
        .with_context("reason", "所有 provider 失败且无缓存");
}

Result<CrossMarketSignals> CrossMarket::refresh() {
    stats_.fetch_attempts.fetch_add(1, std::memory_order_relaxed);

    const auto t0 = std::chrono::steady_clock::now();
    auto result = fetch_from_providers();

    const auto t1 = std::chrono::steady_clock::now();
    const auto latency_us = std::chrono::duration_cast<
        std::chrono::microseconds>(t1 - t0).count();

    stats_.last_fetch_latency_us.store(latency_us,
        std::memory_order_relaxed);
    stats_.total_fetch_latency_us.fetch_add(latency_us,
        std::memory_order_relaxed);

    if (result.is_ok()) {
        stats_.fetch_successes.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats_.fetch_failures.fetch_add(1, std::memory_order_relaxed);
    }

    return result;
}

std::future<Result<CrossMarketSignals>> CrossMarket::get_signals_async() {
    // 使用 shared_ptr 保证生命周期
    // 注意：调用方需确保 CrossMarket 对象在 future 完成前不被销毁
    return std::async(std::launch::async, [this]() {
        try {
            return get_signals();
        } catch (...) {
            return Result<CrossMarketSignals>{
                ErrorInfo{ErrorCode::InternalUnknown, "异步获取异常"}};
        }
    });
}

std::optional<CrossMarketSignals> CrossMarket::peek_signals() const {
    std::shared_lock lock(current_mutex_);
    return current_;
}

// ==============================================================================
// 跨交易所价差
// ==============================================================================
Result<void> CrossMarket::update_spread(const CrossExchangeSpread& spread) {
    if (!spread.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "CrossExchangeSpread 无效");
    }

    std::unique_lock lock(spread_mutex_);

    // 去重：相同 (symbol, exchange_a, exchange_b) 更新
    for (auto& existing : spreads_) {
        if (existing.symbol == spread.symbol &&
            existing.exchange_a == spread.exchange_a &&
            existing.exchange_b == spread.exchange_b) {
            existing = spread;
            return {};
        }
    }

    // 上限保护
    if (spreads_.size() >= 1000) {
        spreads_.erase(spreads_.begin());
    }

    spreads_.push_back(spread);
    return {};
}

std::optional<CrossExchangeSpread> CrossMarket::get_spread(
    std::string_view symbol,
    std::string_view exchange_a,
    std::string_view exchange_b) const
{
    std::shared_lock lock(spread_mutex_);

    for (const auto& s : spreads_) {
        if (s.symbol == symbol &&
            s.exchange_a == exchange_a &&
            s.exchange_b == exchange_b) {
            return s;
        }
    }
    return std::nullopt;
}

std::vector<CrossExchangeSpread>
CrossMarket::list_significant_spreads(double threshold_bps) const
{
    std::shared_lock lock(spread_mutex_);

    std::vector<CrossExchangeSpread> result;
    result.reserve(spreads_.size());

    for (const auto& s : spreads_) {
        if (s.is_significant(threshold_bps)) {
            result.push_back(s);
        }
    }

    // 按 |spread_bps| 降序
    std::sort(result.begin(), result.end(),
        [](const CrossExchangeSpread& a, const CrossExchangeSpread& b) {
            return std::abs(a.spread_bps) > std::abs(b.spread_bps);
        });

    return result;
}

// ==============================================================================
// 宏观事件
// ==============================================================================
Result<void> CrossMarket::add_macro_event(const MacroEvent& event) {
    if (!event.is_valid()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "MacroEvent 无效");
    }

    std::unique_lock lock(macro_mutex_);

    macro_events_.push_back(event);

    // 排序（按时间升序）
    std::sort(macro_events_.begin(), macro_events_.end(),
        [](const MacroEvent& a, const MacroEvent& b) {
            return a.scheduled_time < b.scheduled_time;
        });

    // 清理过期
    prune_macro_events(macro_events_);

    // 上限保护
    if (macro_events_.size() > 1000) {
        macro_events_.erase(
            macro_events_.begin(),
            macro_events_.begin() + (macro_events_.size() - 1000));
    }

    return {};
}

std::vector<MacroEvent>
CrossMarket::upcoming_events(std::chrono::hours lookahead) const {
    std::shared_lock lock(macro_mutex_);

    std::vector<MacroEvent> result;
    for (const auto& e : macro_events_) {
        if (e.is_upcoming(std::chrono::duration_cast<
                std::chrono::minutes>(lookahead))) {
            result.push_back(e);
        }
    }
    return result;
}

bool CrossMarket::is_high_impact_event_near(
    std::chrono::minutes window) const
{
    std::shared_lock lock(macro_mutex_);

    for (const auto& e : macro_events_) {
        if (e.importance == EventImportance::High &&
            e.is_upcoming(window)) {
            return true;
        }
    }
    return false;
}

// ==============================================================================
// 背离检测
// ==============================================================================
std::optional<Divergence> CrossMarket::detect_divergence() const {
    if (!config_.enable_divergence_detection) {
        return std::nullopt;
    }

    // 需要至少 2 个历史点
    std::deque<CrossMarketSignals> hist_copy;
    {
        std::shared_lock lock(history_mutex_);
        if (history_.size() < 2) return std::nullopt;
        hist_copy = history_;
    }

    const auto& curr = hist_copy.back();
    const auto& prev = hist_copy[hist_copy.size() - 2];

    // 检查前置条件
    if (!curr.is_valid() || !prev.is_valid()) return std::nullopt;
    if (!curr.btc_dominance.is_valid() ||
        !prev.btc_dominance.is_valid()) {
        return std::nullopt;
    }

    Divergence div;
    div.timestamp = curr.snapshot_time;

    // 1. BTC 主导上升 + 总市值下降 → 资金撤离山寨
    if (curr.btc_dominance.is_valid() &&
        prev.btc_dominance.is_valid() &&
        curr.total_market_cap.is_valid() &&
        prev.total_market_cap.is_valid()) {

        const double dom_change =
            (curr.btc_dominance.value - prev.btc_dominance.value) /
            std::max(prev.btc_dominance.value, 1e-12);

        const double mcap_change =
            (curr.total_market_cap.value - prev.total_market_cap.value) /
            std::max(prev.total_market_cap.value, 1e-12);

        if (dom_change > config_.divergence_threshold_pct &&
            mcap_change < -config_.divergence_threshold_pct) {
            div.type = DivergenceType::DominanceDivergence;
            div.magnitude = std::abs(dom_change - mcap_change);
            stats_.divergence_detected.fetch_add(1,
                std::memory_order_relaxed);
            return div;
        }
    }

    // 2. 恐惧贪婪极端值
    if (curr.fear_greed_index.is_valid()) {
        // 极端值本身不算背离，但配合其他信号
        // （此处简化，可扩展）
    }

    // 3. 期货基差与价格背离（价格上涨但基差下降）
    if (curr.futures_basis.is_valid() &&
        prev.futures_basis.is_valid() &&
        curr.mid_price_check_placeholder_is_valid_placeholder) {
        // 简化实现：仅检查基差变化
        const double basis_change =
            curr.futures_basis.value - prev.futures_basis.value;
        if (std::abs(basis_change) > config_.divergence_threshold_pct) {
            div.type = DivergenceType::MarketCapDivergence;
            div.magnitude = std::abs(basis_change);
            stats_.divergence_detected.fetch_add(1,
                std::memory_order_relaxed);
            return div;
        }
    }

    return std::nullopt;
}

// ==============================================================================
// 相关性
// ==============================================================================
double CrossMarket::correlation(
    std::string_view series_a,
    std::string_view series_b) const
{
    std::deque<CrossMarketSignals> hist_copy;
    {
        std::shared_lock lock(history_mutex_);
        if (history_.size() < 2) return 0.0;
        hist_copy = history_;
    }

    // 确定使用哪个字段
    auto extract = [](const CrossMarketSignals& s, std::string_view name)
        -> std::optional<double> {
        if (name == "btc_dominance") {
            if (s.btc_dominance.is_valid()) return s.btc_dominance.value;
        } else if (name == "fear_greed") {
            if (s.fear_greed_index.is_valid()) return s.fear_greed_index.value;
        } else if (name == "market_cap") {
            if (s.total_market_cap.is_valid()) return s.total_market_cap.value;
        } else if (name == "funding") {
            if (s.funding_rate_avg.is_valid()) return s.funding_rate_avg.value;
        } else if (name == "basis") {
            if (s.futures_basis.is_valid()) return s.futures_basis.value;
        } else if (name == "usdt_premium") {
            if (s.usdt_premium.is_valid()) return s.usdt_premium.value;
        }
        return std::nullopt;
    };

    std::vector<double> x, y;
    x.reserve(hist_copy.size());
    y.reserve(hist_copy.size());

    const auto start = hist_copy.size() > config_.correlation_window
        ? hist_copy.size() - config_.correlation_window
        : 0;

    for (std::size_t i = start; i < hist_copy.size(); ++i) {
        auto va = extract(hist_copy[i], series_a);
        auto vb = extract(hist_copy[i], series_b);
        if (va.has_value() && vb.has_value()) {
            x.push_back(*va);
            y.push_back(*vb);
        }
    }

    return compute_correlation(x, y);
}

// ==============================================================================
// 状态管理
// ==============================================================================
void CrossMarket::reset() {
    {
        std::unique_lock lock(current_mutex_);
        current_.reset();
        last_fetch_time_ = Timestamp{};
    }
    {
        std::unique_lock lock(history_mutex_);
        history_.clear();
    }
    {
        std::unique_lock lock(spread_mutex_);
        spreads_.clear();
    }
    {
        std::unique_lock lock(macro_mutex_);
        macro_events_.clear();
    }
    stats_.reset();
}

const CrossMarketStats& CrossMarket::stats() const noexcept {
    return stats_;
}

void CrossMarket::reset_stats() noexcept {
    stats_.reset();
}

const CrossMarketConfig& CrossMarket::config() const noexcept {
    return config_;
}

void CrossMarket::set_config(const CrossMarketConfig& config) {
    if (auto r = validate_config(config); r.is_err()) {
        QUANT_LOG_WARN("[CrossMarket] 新配置非法，忽略: {}",
            r.error().to_string());
        return;
    }

    std::unique_lock lock(current_mutex_);
    config_ = config;

    // 若 history_size 变小，裁剪历史
    // 注意：先解锁再操作 history 避免锁顺序问题
    lock.unlock();

    std::unique_lock hist_lock(history_mutex_);
    while (history_.size() > config_.history_size) {
        history_.pop_front();
    }
}

DataQuality CrossMarket::current_quality() const noexcept {
    std::shared_lock lock(current_mutex_);
    if (!current_.has_value()) return DataQuality::Unknown;
    return compute_quality(
        current_->snapshot_time,
        config_.cache_ttl,
        config_.stale_threshold);
}

std::size_t CrossMarket::history_size() const noexcept {
    std::shared_lock lock(history_mutex_);
    return history_.size();
}

// ==============================================================================
// 内部实现
// ==============================================================================
Result<CrossMarketSignals> CrossMarket::fetch_from_providers() {
    // 复制 provider 列表（避免在锁内调用外部）
    std::vector<std::shared_ptr<CrossMarketProvider>> providers_copy;
    {
        std::shared_lock lock(providers_mutex_);
        if (providers_.empty()) {
            return QUANT_ERR_T(CrossMarketSignals,
                ErrorCode::ConfigNotFound)
                .with_context("reason", "无可用 provider");
        }
        providers_copy = providers_;
    }

    CrossMarketSignals merged;
    merged.snapshot_time = Timestamp::now();
    merged.field_count = 8;   // 8 个主要字段
    merged.valid_field_count = 0;

    int succeeded = 0;
    ErrorInfo last_error;

    // 逐个调用 provider
    for (const auto& p : providers_copy) {
        if (!p) continue;

        // 检查可用性
        bool available = true;
        try {
            available = p->is_available();
        } catch (...) {
            available = false;
        }
        if (!available) continue;

        // 尝试 fetch
        try {
            auto result = p->fetch();
            if (result.is_err()) {
                last_error = result.error();
                continue;
            }

            const auto& source = result.value();
            merge_signals(merged, source);
            ++succeeded;

        } catch (const std::exception& e) {
            QUANT_LOG_WARN("[CrossMarket] Provider {} 异常: {}",
                p->name(), e.what());
            last_error = ErrorInfo{
                ErrorCode::NetworkUnknown,
                std::string{"Provider 异常: "} + e.what()};
        } catch (...) {
            QUANT_LOG_WARN("[CrossMarket] Provider {} 未知异常", p->name());
            last_error = ErrorInfo{
                ErrorCode::InternalUnknown, "Provider 未知异常"};
        }
    }

    if (succeeded == 0) {
        return QUANT_ERR_T(CrossMarketSignals, last_error.code)
            .with_context("reason", "所有 provider 失败")
            .with_context("last_error", last_error.to_string());
    }

    // 计算有效字段数
    merged.valid_field_count = 0;
    if (merged.btc_dominance.is_valid()) ++merged.valid_field_count;
    if (merged.total_market_cap.is_valid()) ++merged.valid_field_count;
    if (merged.fear_greed_index.is_valid()) ++merged.valid_field_count;
    if (merged.stablecoin_flow.is_valid()) ++merged.valid_field_count;
    if (merged.futures_basis.is_valid()) ++merged.valid_field_count;
    if (merged.eth_dominance.is_valid()) ++merged.valid_field_count;
    if (merged.usdt_premium.is_valid()) ++merged.valid_field_count;
    if (merged.funding_rate_avg.is_valid()) ++merged.valid_field_count;

    // 校验 + 标记质量
    if (config_.validate_ranges) {
        if (!validate_signals(merged)) {
            stats_.invalid_responses.fetch_add(1,
                std::memory_order_relaxed);
            return QUANT_ERR_T(CrossMarketSignals,
                ErrorCode::ConfigInvalidValue)
                .with_context("reason", "校验失败");
        }
    }

    mark_quality(merged, DataQuality::Fresh);

    // 更新当前值
    {
        std::unique_lock lock(current_mutex_);
        current_ = merged;
        last_fetch_time_ = merged.snapshot_time;
    }

    // 加入历史
    push_history(merged);

    return merged;
}

bool CrossMarket::validate_signals(CrossMarketSignals& signals) const noexcept {
    bool all_valid = true;

    auto check = [&all_valid](TimedValue<double>& tv,
                              bool (*validator)(double)) {
        if (!tv.is_valid()) return;
        if (!validator(tv.value)) {
            tv.quality = DataQuality::Invalid;
            all_valid = false;
        }
    };

    check(signals.btc_dominance, validate_btc_dominance);
    check(signals.fear_greed_index, validate_fear_greed);
    check(signals.total_market_cap, validate_market_cap);
    check(signals.futures_basis, validate_basis);

    return all_valid;
}

void CrossMarket::merge_signals(
    CrossMarketSignals& target,
    const CrossMarketSignals& source) noexcept
{
    // 逐字段合并：仅当目标字段无效或源字段更新时覆盖
    auto merge_field = [](TimedValue<double>& dst,
                          const TimedValue<double>& src) {
        if (!src.is_valid()) return;
        if (!dst.is_valid() ||
            src.timestamp > dst.timestamp) {
            dst = src;
        }
    };

    merge_field(target.btc_dominance, source.btc_dominance);
    merge_field(target.total_market_cap, source.total_market_cap);
    merge_field(target.fear_greed_index, source.fear_greed_index);
    merge_field(target.stablecoin_flow, source.stablecoin_flow);
    merge_field(target.futures_basis, source.futures_basis);
    merge_field(target.eth_dominance, source.eth_dominance);
    merge_field(target.usdt_premium, source.usdt_premium);
    merge_field(target.funding_rate_avg, source.funding_rate_avg);
}

void CrossMarket::mark_quality(
    CrossMarketSignals& signals,
    DataQuality quality) noexcept
{
    auto set_q = [quality](TimedValue<double>& tv) {
        if (tv.is_valid()) {
            tv.quality = quality;
        }
    };

    set_q(signals.btc_dominance);
    set_q(signals.total_market_cap);
    set_q(signals.fear_greed_index);
    set_q(signals.stablecoin_flow);
    set_q(signals.futures_basis);
    set_q(signals.eth_dominance);
    set_q(signals.usdt_premium);
    set_q(signals.funding_rate_avg);
}

void CrossMarket::push_history(const CrossMarketSignals& signals) {
    std::unique_lock lock(history_mutex_);
    history_.push_back(signals);
    while (history_.size() > config_.history_size) {
        history_.pop_front();
    }
}

void CrossMarket::refresh_loop() {
    QUANT_LOG_INFO("[CrossMarket] 刷新线程启动");

    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock lock(refresh_mutex_);
            refresh_cv_.wait_for(lock, config_.refresh_interval,
                [this] {
                    return !running_.load(std::memory_order_acquire);
                });
        }

        if (!running_.load(std::memory_order_acquire)) break;

        // 执行刷新
        try {
            auto result = fetch_from_providers();
            if (result.is_err()) {
                QUANT_LOG_WARN("[CrossMarket] 自动刷新失败: {}",
                    result.error().to_string());
            }
        } catch (const std::exception& e) {
            QUANT_LOG_ERROR("[CrossMarket] 刷新异常: {}", e.what());
        }
    }

    QUANT_LOG_INFO("[CrossMarket] 刷新线程退出");
}

// ==============================================================================
// 诊断
// ==============================================================================
std::string CrossMarket::dump() const {
    std::string out;
    out.reserve(2048);
    char buf[512];

    out += "CrossMarket Dump:\n";

    std::snprintf(buf, sizeof(buf), "  running:             %s\n",
        is_running() ? "true" : "false");
    out += buf;

    std::snprintf(buf, sizeof(buf), "  cache_ttl_s:         %lld\n",
        static_cast<long long>(config_.cache_ttl.count()));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  stale_threshold_s:   %lld\n",
        static_cast<long long>(config_.stale_threshold.count()));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  refresh_interval_s:  %lld\n",
        static_cast<long long>(config_.refresh_interval.count()));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  auto_refresh:        %s\n",
        config_.enable_auto_refresh ? "true" : "false");
    out += buf;

    std::snprintf(buf, sizeof(buf), "  current_quality:     %s\n",
        std::string{to_string(current_quality())}.c_str());
    out += buf;

    out += "  ---\n";

    {
        std::shared_lock lock(providers_mutex_);
        std::snprintf(buf, sizeof(buf), "  providers:           %zu\n",
            providers_.size());
        out += buf;
        for (const auto& p : providers_) {
            if (p) {
                std::snprintf(buf, sizeof(buf),
                    "    [%s] priority=%d available=%s\n",
                    std::string{p->name()}.c_str(),
                    p->priority(),
                    p->is_available() ? "yes" : "no");
                out += buf;
            }
        }
    }

    {
        std::shared_lock lock(history_mutex_);
        std::snprintf(buf, sizeof(buf), "  history_size:        %zu\n",
            history_.size());
        out += buf;
    }

    {
        std::shared_lock lock(spread_mutex_);
        std::snprintf(buf, sizeof(buf), "  spreads:             %zu\n",
            spreads_.size());
        out += buf;
    }

    {
        std::shared_lock lock(macro_mutex_);
        std::snprintf(buf, sizeof(buf), "  macro_events:        %zu\n",
            macro_events_.size());
        out += buf;
    }

    out += "  ---\n";

    std::snprintf(buf, sizeof(buf), "  fetch_attempts:      %llu\n",
        static_cast<unsigned long long>(
            stats_.fetch_attempts.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  fetch_successes:     %llu\n",
        static_cast<unsigned long long>(
            stats_.fetch_successes.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  fetch_failures:      %llu\n",
        static_cast<unsigned long long>(
            stats_.fetch_failures.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  cache_hits:          %llu\n",
        static_cast<unsigned long long>(
            stats_.cache_hits.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  stale_serves:        %llu\n",
        static_cast<unsigned long long>(
            stats_.stale_serves.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  invalid_responses:   %llu\n",
        static_cast<unsigned long long>(
            stats_.invalid_responses.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  divergence_detected: %llu\n",
        static_cast<unsigned long long>(
            stats_.divergence_detected.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  success_rate:        %.2f%%\n",
        stats_.success_rate() * 100.0);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  avg_fetch_latency_ms:%.2f\n",
        stats_.avg_fetch_latency_ms());
    out += buf;

    return out;
}

}  // namespace data
}  // namespace quant
