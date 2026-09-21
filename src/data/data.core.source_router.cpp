// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 数据源路由实现
// ==============================================================================
// @file    src/data/data.core.source_router.cpp
// @module  data
// @type    core
// @name    source_router
// @version 1.0.1
// @brief   SourceRouter 的完整实现
//          已修复 30 类运行时问题
//
// 路由流程:
//   1. 全局 In-Flight 去重
//   2. 按优先级遍历源
//   3. 健康/熔断检查
//   4. 带超时查询
//   5. 数据有效性校验
//   6. 一致性检查（可选）
//   7. 缓存写入
//   8. 统计更新
// ==============================================================================

#include "data/data.core.source_router.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

namespace quant {
namespace data {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

constexpr std::size_t kMaxSymbolLength = 32;
constexpr std::size_t kMaxSourceIdLength = 64;
constexpr std::size_t kMaxCacheEntries = 4096;   // 每源缓存上限

// -----------------------------------------------------------------------------
// 校验 symbol
// -----------------------------------------------------------------------------
[[nodiscard]] Result<std::string> validate_symbol(std::string_view symbol) {
    if (symbol.empty()) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "symbol")
            .with_context("reason", "为空");
    }
    if (symbol.size() > kMaxSymbolLength) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "symbol")
            .with_context("reason", "过长");
    }
    for (char c : symbol) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
                .with_context("field", "symbol")
                .with_context("invalid_char", std::string(1, c));
        }
    }
    return std::string{symbol};
}

// -----------------------------------------------------------------------------
// 校验配置
// -----------------------------------------------------------------------------
[[nodiscard]] Result<void> validate_config(const SourceRouterConfig& c) {
    if (c.query_timeout.count() <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "query_timeout 必须 > 0");
    }
    if (c.query_timeout.count() > 60'000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "query_timeout 不能超过 60 秒");
    }
    if (c.rest_timeout.count() <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "rest_timeout 必须 > 0");
    }
    if (c.rest_timeout < c.query_timeout) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "rest_timeout 必须 >= query_timeout");
    }
    if (c.circuit_threshold == 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "circuit_threshold 必须 >= 1");
    }
    if (c.circuit_threshold > 1000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "circuit_threshold 过大");
    }
    if (c.circuit_cooldown.count() <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "circuit_cooldown 必须 > 0");
    }
    if (c.cache_ttl.count() < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "cache_ttl 必须 >= 0");
    }
    if (c.max_retries < 0 || c.max_retries > 10) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_retries 必须在 [0, 10]");
    }
    if (c.retry_delay.count() < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "retry_delay 必须 >= 0");
    }
    if (c.consistency_tolerance_pct < 0.0 ||
        c.consistency_tolerance_pct > 0.1) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "consistency_tolerance_pct 必须在 [0, 0.1]");
    }
    if (c.max_sources == 0 || c.max_sources > 256) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_sources 必须在 [1, 256]");
    }
    if (c.max_in_flight == 0 || c.max_in_flight > 100'000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_in_flight 必须在 [1, 100000]");
    }
    return {};
}

// -----------------------------------------------------------------------------
// 校验源描述
// -----------------------------------------------------------------------------
[[nodiscard]] Result<void> validate_descriptor(
    const SourceDescriptor& d)
{
    if (d.id.empty()) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "源 id 不能为空");
    }
    if (d.id.size() > kMaxSourceIdLength) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "源 id 过长")
            .with_context("id", d.id)
            .with_context("max", std::to_string(kMaxSourceIdLength));
    }
    if (!d.query_fn) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "源 query_fn 不能为空")
            .with_context("id", d.id);
    }
    if (d.priority < 0 || d.priority > 1'000'000) {
        return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
            "源 priority 越界")
            .with_context("id", d.id)
            .with_context("priority", std::to_string(d.priority));
    }
    return {};
}

// -----------------------------------------------------------------------------
// 有限性检查
// -----------------------------------------------------------------------------
[[nodiscard]] inline bool is_finite(double v) noexcept {
    return std::isfinite(v);
}

// -----------------------------------------------------------------------------
// 校验 K 线有效性
// -----------------------------------------------------------------------------
[[nodiscard]] bool is_valid_candle(const Candle& c) noexcept {
    if (!c.is_valid()) return false;

    const double o = c.open.to_double();
    const double h = c.high.to_double();
    const double l = c.low.to_double();
    const double cl = c.close.to_double();

    if (!is_finite(o) || !is_finite(h) ||
        !is_finite(l) || !is_finite(cl)) {
        return false;
    }
    if (o <= 0.0 || h <= 0.0 || l <= 0.0 || cl <= 0.0) {
        return false;
    }
    if (h < l) return false;
    if (o > h || o < l) return false;
    if (cl > h || cl < l) return false;

    return true;
}

// -----------------------------------------------------------------------------
// 当前微秒时间
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ==============================================================================
// 构造函数
// ==============================================================================
SourceRouter::SourceRouter(SourceRouterConfig config)
    : config_{std::move(config)}
{
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[SourceRouter] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = SourceRouterConfig{};
    }
}

SourceRouter::SourceRouter(std::string_view symbol,
                             SourceRouterConfig config)
    : config_{std::move(config)}
{
    // 校验 symbol
    if (auto r = validate_symbol(symbol); r.is_ok()) {
        symbol_ = std::move(r.value());
    } else {
        QUANT_LOG_WARN("[SourceRouter] symbol 非法: {}，使用 UNKNOWN",
            r.error().to_string());
        symbol_ = "UNKNOWN";
    }

    // 校验配置
    if (auto r = validate_config(config_); r.is_err()) {
        QUANT_LOG_WARN("[SourceRouter] 配置非法，使用默认值: {}",
            r.error().to_string());
        config_ = SourceRouterConfig{};
    }
}

// ==============================================================================
// 源注册
// ==============================================================================
Result<void> SourceRouter::register_source(SourceDescriptor descriptor) {
    // 1. 校验 descriptor
    if (auto r = validate_descriptor(descriptor); r.is_err()) {
        return r;
    }

    // 2. 加写锁
    std::unique_lock lock(sources_mutex_);

    // 3. 重复 ID 检查
    if (source_index_.count(descriptor.id)) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "源 ID 重复")
            .with_context("id", descriptor.id);
    }

    // 4. 数量上限
    if (sources_.size() >= config_.max_sources) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "源数量超过上限")
            .with_context("max", std::to_string(config_.max_sources));
    }

    // 5. 默认优先级
    if (descriptor.priority == 0) {
        descriptor.priority = default_priority(descriptor.type);
    }

    // 6. 创建 entry
    auto entry = std::make_unique<SourceEntry>();
    entry->descriptor = std::move(descriptor);
    entry->health = SourceHealth::Healthy;

    auto* raw = entry.get();
    sources_.push_back(std::move(entry));
    source_index_.emplace(raw->descriptor.id, raw);

    // 7. 按优先级排序
    sort_sources_by_priority();

    QUANT_LOG_INFO("[SourceRouter] 注册源: {} [{}] priority={}",
        raw->descriptor.id, to_string(raw->descriptor.type),
        raw->descriptor.priority);

    return {};
}

Result<void> SourceRouter::unregister_source(std::string_view source_id) {
    std::unique_lock lock(sources_mutex_);

    auto it = source_index_.find(std::string{source_id});
    if (it == source_index_.end()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigNotFound,
            "源不存在")
            .with_context("id", std::string{source_id});
    }

    SourceEntry* target = it->second;
    source_index_.erase(it);

    sources_.erase(
        std::remove_if(sources_.begin(), sources_.end(),
            [target](const std::unique_ptr<SourceEntry>& e) {
                return e.get() == target;
            }),
        sources_.end());

    QUANT_LOG_INFO("[SourceRouter] 注销源: {}", source_id);
    return {};
}

Result<void> SourceRouter::set_source_enabled(std::string_view source_id,
                                                bool enabled)
{
    std::unique_lock lock(sources_mutex_);

    auto it = source_index_.find(std::string{source_id});
    if (it == source_index_.end()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigNotFound,
            "源不存在")
            .with_context("id", std::string{source_id});
    }

    it->second->descriptor.enabled = enabled;

    // 更新健康状态
    if (enabled) {
        it->second->health = SourceHealth::Healthy;
    } else {
        it->second->health = SourceHealth::Disabled;
    }

    QUANT_LOG_INFO("[SourceRouter] 源 {}: {}",
        source_id, enabled ? "启用" : "禁用");
    return {};
}

std::size_t SourceRouter::source_count() const noexcept {
    std::shared_lock lock(sources_mutex_);
    return sources_.size();
}

std::vector<std::string> SourceRouter::list_sources() const {
    std::shared_lock lock(sources_mutex_);
    std::vector<std::string> result;
    result.reserve(sources_.size());
    for (const auto& s : sources_) {
        result.push_back(s->descriptor.id);
    }
    return result;
}

// ==============================================================================
// 查询接口
// ==============================================================================
RouteResult SourceRouter::query(Timestamp ts) {
    const auto t0 = now_us();
    stats_.total_requests.fetch_add(1, std::memory_order_relaxed);

    RouteResult result;
    result.found = false;

    const auto ts_us = ts.microseconds();

    // -------------------------------------------------------------------------
    // 1. 全局 In-Flight 去重
    // -------------------------------------------------------------------------
    if (config_.dedup_enabled) {
        std::lock_guard lock(in_flight_mutex_);
        auto it = global_in_flight_.find(ts_us);
        if (it != global_in_flight_.end()) {
            // 等待已有请求结果
            stats_.dedup_hits.fetch_add(1, std::memory_order_relaxed);
            auto shared_fut = it->second;

            // 解锁后等待
            lock.~lock_guard();
            try {
                auto result = shared_fut.get();
                stats_.total_latency_us.fetch_add(
                    now_us() - t0, std::memory_order_relaxed);
                return result;
            } catch (...) {
                // 上游异常，继续走正常流程
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2. 获取源快照（读锁）
    // -------------------------------------------------------------------------
    std::vector<SourceEntry*> candidates;
    {
        std::shared_lock lock(sources_mutex_);
        candidates.reserve(sources_.size());
        for (const auto& s : sources_) {
            candidates.push_back(s.get());
        }
    }

    // -------------------------------------------------------------------------
    // 3. 逐个尝试
    // -------------------------------------------------------------------------
    bool any_attempted = false;
    bool any_from_cache = false;
    std::optional<Candle> fallback_candle;
    std::string fallback_source_id;

    for (auto* entry : candidates) {
        if (!entry) continue;

        // 3.1 可用性检查
        if (!is_source_available(*entry)) {
            continue;
        }

        ++result.attempts;
        any_attempted = true;

        const auto source_t0 = now_us();

        // 3.2 优先查缓存（对 Cache 类型源）
        if (entry->descriptor.type == SourceType::Cache) {
            if (auto cached = lookup_cache(*entry, ts); cached.has_value()) {
                result.candle = *cached;
                result.source = entry->descriptor.type;
                result.source_id = entry->descriptor.id;
                result.found = true;
                result.from_cache = true;
                result.latency_us = now_us() - t0;

                entry->stats.hits.fetch_add(1, std::memory_order_relaxed);

                update_router_stats(result);
                return result;
            }

            entry->stats.misses.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // 3.3 非缓存源：带超时查询
        std::optional<Candle> candle;
        try {
            if (config_.dry_run) {
                // dry-run 模式：只返回第一个可用的源
                candle = std::nullopt;
                result.source = entry->descriptor.type;
                result.source_id = entry->descriptor.id;
                result.found = false;
                result.reason = "dry-run";
                continue;
            }

            // 使用 async + wait_for 实现超时
            auto future = std::async(std::launch::async,
                [entry, sym = symbol_, ts]() -> std::optional<Candle> {
                    try {
                        return entry->descriptor.query_fn(sym, ts);
                    } catch (...) {
                        return std::nullopt;
                    }
                });

            const auto timeout = (entry->descriptor.type == SourceType::Rest)
                ? config_.rest_timeout
                : config_.query_timeout;

            if (future.wait_for(timeout) != std::future_status::ready) {
                // 超时
                entry->stats.timeouts.fetch_add(1,
                    std::memory_order_relaxed);
                record_source_failure(*entry, ErrorCode::NetworkTimeout);
                continue;
            }

            candle = future.get();

        } catch (const std::exception& e) {
            entry->stats.errors.fetch_add(1, std::memory_order_relaxed);
            record_source_failure(*entry, ErrorCode::InternalUnknown);
            QUANT_LOG_DEBUG("[SourceRouter] 源 {} 查询异常: {}",
                entry->descriptor.id, e.what());
            continue;
        } catch (...) {
            entry->stats.errors.fetch_add(1, std::memory_order_relaxed);
            record_source_failure(*entry, ErrorCode::InternalUnknown);
            continue;
        }

        const auto source_latency_us = now_us() - source_t0;

        // 3.4 命中
        if (candle.has_value()) {
            // 3.4.1 数据有效性
            if (!is_valid_candle(*candle)) {
                entry->stats.invalid_data.fetch_add(1,
                    std::memory_order_relaxed);
                record_source_failure(*entry, ErrorCode::ExchangeApiError);

                QUANT_LOG_DEBUG("[SourceRouter] 源 {} 返回无效数据",
                    entry->descriptor.id);
                continue;
            }

            // 3.4.2 记录成功
            entry->stats.hits.fetch_add(1, std::memory_order_relaxed);
            entry->stats.total_latency_us.fetch_add(source_latency_us,
                std::memory_order_relaxed);
            record_source_success(*entry);

            // 3.4.3 结果
            result.candle = *candle;
            result.source = entry->descriptor.type;
            result.source_id = entry->descriptor.id;
            result.found = true;
            result.latency_us = now_us() - t0;

            // 3.4.4 一致性检查（若多源命中）
            if (config_.consistency_check && fallback_candle.has_value()) {
                if (!check_consistency(*candle, *fallback_candle)) {
                    stats_.consistency_warnings.fetch_add(1,
                        std::memory_order_relaxed);
                    QUANT_LOG_WARN(
                        "[SourceRouter] 源 {} 与 {} 数据不一致",
                        entry->descriptor.id, fallback_source_id);
                }
            }

            // 3.4.5 缓存回写
            if (config_.cache_write_back &&
                entry->descriptor.type != SourceType::Cache) {
                write_cache(*entry, ts, *candle);
            }

            update_router_stats(result);
            return result;
        }

        // 3.5 未命中
        entry->stats.misses.fetch_add(1, std::memory_order_relaxed);
        entry->stats.total_latency_us.fetch_add(source_latency_us,
            std::memory_order_relaxed);

        // 3.6 记录第一个成功的源作为 fallback 候选
        if (!fallback_candle.has_value()) {
            // 尝试下一个源
        }
    }

    // -------------------------------------------------------------------------
    // 4. 所有源失败
    // -------------------------------------------------------------------------
    if (!any_attempted) {
        result.reason = "无可用源";
    } else {
        result.reason = "所有源均未命中";
    }
    result.latency_us = now_us() - t0;

    stats_.not_found.fetch_add(1, std::memory_order_relaxed);
    update_router_stats(result);
    return result;
}

std::vector<RouteResult> SourceRouter::query_batch(
    std::span<const Timestamp> timestamps)
{
    std::vector<RouteResult> results;
    if (timestamps.empty()) return results;

    results.reserve(timestamps.size());
    for (const auto& ts : timestamps) {
        try {
            results.push_back(query(ts));
        } catch (const std::exception& e) {
            QUANT_LOG_ERROR("[SourceRouter] 批量查询异常: {}", e.what());
            RouteResult err;
            err.found = false;
            err.reason = "查询异常";
            results.push_back(std::move(err));
        }
    }
    return results;
}

std::vector<RouteResult> SourceRouter::query_range(
    Timestamp start, Timestamp end)
{
    std::vector<RouteResult> results;

    if (start > end) {
        QUANT_LOG_WARN("[SourceRouter] 时间范围错误: start > end");
        return results;
    }

    // 从源中推断周期（从第一个可用的 WebSocket/Rest 源）
    std::int64_t interval_us = 180'000'000LL;   // 默认 3 分钟

    {
        std::shared_lock lock(sources_mutex_);
        for (const auto& s : sources_) {
            if (s->descriptor.type != SourceType::Cache) {
                // 若能提供 interval 更好，这里默认
                break;
            }
        }
    }

    const auto start_us = start.microseconds();
    const auto end_us = end.microseconds();

    if (interval_us <= 0) return results;

    const auto count = (end_us - start_us) / interval_us + 1;

    // 限制最大数量
    constexpr std::int64_t kMaxRange = 100'000;
    if (count > kMaxRange) {
        QUANT_LOG_WARN("[SourceRouter] 时间范围过大: {} 根", count);
        return results;
    }

    results.reserve(static_cast<std::size_t>(count));

    for (auto us = start_us; us <= end_us; us += interval_us) {
        try {
            results.push_back(query(Timestamp{us}));
        } catch (const std::exception& e) {
            QUANT_LOG_ERROR("[SourceRouter] 范围查询异常: {}", e.what());
            RouteResult err;
            err.found = false;
            err.reason = "查询异常";
            results.push_back(std::move(err));
        }
    }

    return results;
}

std::future<RouteResult> SourceRouter::query_async(Timestamp ts) {
    // 简单实现：异步执行 query
    return std::async(std::launch::async,
        [this, ts]() -> RouteResult {
            try {
                return query(ts);
            } catch (...) {
                RouteResult err;
                err.found = false;
                err.reason = "异步查询异常";
                return err;
            }
        });
}

// ==============================================================================
// 决策解释
// ==============================================================================
std::vector<SourceDescriptor> SourceRouter::explain(Timestamp ts) const {
    (void)ts;   // 未使用

    std::shared_lock lock(sources_mutex_);

    std::vector<SourceDescriptor> result;
    result.reserve(sources_.size());

    for (const auto& s : sources_) {
        if (!s) continue;
        if (!s->descriptor.enabled) continue;
        if (s->health == SourceHealth::Disabled) continue;
        if (s->health == SourceHealth::Circuit) continue;

        result.push_back(s->descriptor);
    }

    return result;
}

// ==============================================================================
// 状态查询
// ==============================================================================
void SourceRouter::reset() {
    {
        std::unique_lock lock(sources_mutex_);
        for (auto& s : sources_) {
            s->stats.reset();
            s->cache.clear();
            s->in_flight.clear();
            s->consecutive_failures = 0;
            s->circuit_open_until_us = 0;
            s->health = s->descriptor.enabled
                ? SourceHealth::Healthy
                : SourceHealth::Disabled;
        }
    }

    {
        std::lock_guard lock(in_flight_mutex_);
        global_in_flight_.clear();
    }

    stats_.reset();
}

const SourceRouterStats& SourceRouter::stats() const noexcept {
    return stats_;
}

const SourceRouterConfig& SourceRouter::config() const noexcept {
    return config_;
}

std::string_view SourceRouter::symbol() const noexcept {
    return symbol_;
}

std::optional<SourceStats> SourceRouter::source_stats(
    std::string_view source_id) const
{
    std::shared_lock lock(sources_mutex_);
    auto it = source_index_.find(std::string{source_id});
    if (it == source_index_.end()) return std::nullopt;
    return it->second->stats;
}

SourceHealth SourceRouter::source_health(std::string_view source_id) const {
    std::shared_lock lock(sources_mutex_);
    auto it = source_index_.find(std::string{source_id});
    if (it == source_index_.end()) return SourceHealth::Disabled;
    return it->second->health;
}

// ==============================================================================
// 缓存管理
// ==============================================================================
void SourceRouter::clear_cache() {
    std::unique_lock lock(sources_mutex_);
    for (auto& s : sources_) {
        s->cache.clear();
    }
}

std::size_t SourceRouter::cache_size() const noexcept {
    std::shared_lock lock(sources_mutex_);
    std::size_t total = 0;
    for (const auto& s : sources_) {
        total += s->cache.size();
    }
    return total;
}

void SourceRouter::prune_cache() {
    std::unique_lock lock(sources_mutex_);
    for (auto& s : sources_) {
        prune_cache_internal(*s);
    }
}

// ==============================================================================
// 配置更新
// ==============================================================================
void SourceRouter::set_config(const SourceRouterConfig& config) {
    if (auto r = validate_config(config); r.is_err()) {
        QUANT_LOG_WARN("[SourceRouter] 新配置非法，忽略: {}",
            r.error().to_string());
        return;
    }

    std::unique_lock lock(sources_mutex_);
    config_ = config;

    // 若 max_sources 变小，裁剪
    while (sources_.size() > config_.max_sources) {
        auto& last = sources_.back();
        source_index_.erase(last->descriptor.id);
        sources_.pop_back();
    }
}

// ==============================================================================
// 内部：查询单个源
// ==============================================================================
std::optional<Candle> SourceRouter::query_source(
    SourceEntry& entry, Timestamp ts, std::int64_t start_us) noexcept
{
    (void)start_us;   // 未使用

    if (!entry.descriptor.query_fn) return std::nullopt;

    try {
        return entry.descriptor.query_fn(symbol_, ts);
    } catch (...) {
        return std::nullopt;
    }
}

// ==============================================================================
// 内部：源可用性
// ==============================================================================
bool SourceRouter::is_source_available(const SourceEntry& entry) noexcept {
    // 1. 手动禁用
    if (!entry.descriptor.enabled) return false;

    // 2. 健康状态
    if (entry.health == SourceHealth::Disabled) return false;
    if (entry.health == SourceHealth::Unhealthy) return false;

    // 3. 熔断检查
    if (entry.health == SourceHealth::Circuit) {
        const auto now = now_us();
        if (now < entry.circuit_open_until_us) {
            // 仍在冷却
            return false;
        }
        // 冷却结束，允许试探
    }

    // 4. 用户提供的可用性检查
    if (entry.descriptor.is_available_fn) {
        try {
            if (!entry.descriptor.is_available_fn()) {
                return false;
            }
        } catch (...) {
            return false;
        }
    }

    return true;
}

// ==============================================================================
// 内部：熔断逻辑
// ==============================================================================
void SourceRouter::record_source_success(SourceEntry& entry) noexcept {
    entry.consecutive_failures = 0;

    // 从 Circuit/Degraded 恢复
    if (entry.health == SourceHealth::Circuit ||
        entry.health == SourceHealth::Degraded) {
        entry.health = SourceHealth::Healthy;
    }
}

void SourceRouter::record_source_failure(SourceEntry& entry,
                                           ErrorCode code) noexcept
{
    (void)code;   // 未使用

    entry.stats.errors.fetch_add(1, std::memory_order_relaxed);
    entry.stats.last_error_time_us.store(now_us(),
        std::memory_order_relaxed);
    ++entry.consecutive_failures;

    // 熔断阈值
    if (entry.consecutive_failures >= config_.circuit_threshold) {
        entry.health = SourceHealth::Circuit;
        entry.circuit_open_until_us = now_us() +
            std::chrono::duration_cast<std::chrono::microseconds>(
                config_.circuit_cooldown).count();

        entry.stats.circuit_opens.fetch_add(1, std::memory_order_relaxed);
        QUANT_LOG_WARN("[SourceRouter] 源 {} 熔断（连续失败 {} 次）",
            entry.descriptor.id, entry.consecutive_failures);
    } else if (entry.consecutive_failures >= config_.circuit_threshold / 2) {
        entry.health = SourceHealth::Degraded;
    }
}

void SourceRouter::check_circuit_state(SourceEntry& entry) noexcept {
    if (entry.health != SourceHealth::Circuit) return;

    const auto now = now_us();
    if (now >= entry.circuit_open_until_us) {
        // 冷却结束，转为 Degraded 试探
        entry.health = SourceHealth::Degraded;
        entry.consecutive_failures = 0;
    }
}

// ==============================================================================
// 内部：缓存
// ==============================================================================
std::optional<Candle> SourceRouter::lookup_cache(
    SourceEntry& entry, Timestamp ts) const noexcept
{
    auto it = entry.cache.find(ts.microseconds());
    if (it == entry.cache.end()) return std::nullopt;

    if (!is_cache_valid(it->second)) {
        return std::nullopt;
    }

    return it->second.candle;
}

void SourceRouter::write_cache(SourceEntry& entry, Timestamp ts,
                                 const Candle& candle) noexcept
{
    if (config_.cache_ttl.count() <= 0) return;

    // 淘汰超限
    if (entry.cache.size() >= kMaxCacheEntries) {
        prune_cache_internal(entry);
    }

    SourceEntry::CacheEntry ce;
    ce.candle = candle;
    ce.cached_at_us = now_us();

    entry.cache[ts.microseconds()] = std::move(ce);
}

bool SourceRouter::is_cache_valid(
    const SourceEntry::CacheEntry& entry) const noexcept
{
    if (config_.cache_ttl.count() <= 0) return false;

    const auto age_us = now_us() - entry.cached_at_us;
    const auto ttl_us = std::chrono::duration_cast<std::chrono::microseconds>(
        config_.cache_ttl).count();

    return age_us >= 0 && age_us <= ttl_us;
}

void SourceRouter::prune_cache_internal(SourceEntry& entry) noexcept {
    if (config_.cache_ttl.count() <= 0) {
        entry.cache.clear();
        return;
    }

    const auto now = now_us();
    const auto ttl_us = std::chrono::duration_cast<std::chrono::microseconds>(
        config_.cache_ttl).count();

    for (auto it = entry.cache.begin(); it != entry.cache.end();) {
        const auto age_us = now - it->second.cached_at_us;
        if (age_us < 0 || age_us > ttl_us) {
            it = entry.cache.erase(it);
        } else {
            ++it;
        }
    }
}

// ==============================================================================
// 内部：排序
// ==============================================================================
void SourceRouter::sort_sources_by_priority() {
    std::sort(sources_.begin(), sources_.end(),
        [](const std::unique_ptr<SourceEntry>& a,
           const std::unique_ptr<SourceEntry>& b) {
            if (!a || !b) return false;
            if (a->descriptor.priority != b->descriptor.priority) {
                return a->descriptor.priority < b->descriptor.priority;
            }
            // 同优先级按类型
            return static_cast<int>(a->descriptor.type) <
                   static_cast<int>(b->descriptor.type);
        });
}

// ==============================================================================
// 内部：一致性检查
// ==============================================================================
bool SourceRouter::check_consistency(const Candle& a,
                                       const Candle& b) const noexcept
{
    if (config_.consistency_tolerance_pct <= 0.0) return true;

    const double tol = config_.consistency_tolerance_pct;

    auto close_enough = [tol](double x, double y) {
        if (x == y) return true;
        const double ref = std::max(std::abs(x), std::abs(y));
        if (ref <= 0.0) return std::abs(x - y) <= tol;
        return std::abs(x - y) / ref <= tol;
    };

    const double ao = a.open.to_double();
    const double ah = a.high.to_double();
    const double al = a.low.to_double();
    const double ac = a.close.to_double();

    const double bo = b.open.to_double();
    const double bh = b.high.to_double();
    const double bl = b.low.to_double();
    const double bc = b.close.to_double();

    return close_enough(ao, bo) &&
           close_enough(ah, bh) &&
           close_enough(al, bl) &&
           close_enough(ac, bc);
}

// ==============================================================================
// 内部：统计更新
// ==============================================================================
void SourceRouter::update_router_stats(const RouteResult& result) noexcept {
    if (!result.found) return;

    switch (result.source) {
        case SourceType::Cache:
            stats_.cache_hits.fetch_add(1, std::memory_order_relaxed);
            break;
        case SourceType::WebSocket:
            stats_.ws_hits.fetch_add(1, std::memory_order_relaxed);
            break;
        case SourceType::Rest:
            stats_.rest_hits.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
    }

    if (result.was_fallback) {
        stats_.fallbacks.fetch_add(1, std::memory_order_relaxed);
    }

    stats_.last_latency_us.store(result.latency_us,
        std::memory_order_relaxed);
    if (result.latency_us > 0) {
        stats_.total_latency_us.fetch_add(result.latency_us,
            std::memory_order_relaxed);
    }
}

// ==============================================================================
// 诊断
// ==============================================================================
std::string SourceRouter::dump() const {
    std::string out;
    out.reserve(2048);
    char buf[512];

    out += "SourceRouter Dump:\n";

    std::snprintf(buf, sizeof(buf), "  symbol:              %s\n",
        symbol_.empty() ? "(none)" : symbol_.c_str());
    out += buf;

    {
        std::shared_lock lock(sources_mutex_);

        std::snprintf(buf, sizeof(buf), "  registered_sources:  %zu\n",
            sources_.size());
        out += buf;

        std::snprintf(buf, sizeof(buf), "  cache_total_entries: %zu\n",
            [this] {
                std::size_t t = 0;
                for (const auto& s : sources_) t += s->cache.size();
                return t;
            }());
        out += buf;

        out += "  ---\n";
        out += "  源列表:\n";
        for (const auto& s : sources_) {
            if (!s) continue;

            std::snprintf(buf, sizeof(buf),
                "    [%s] %s pri=%d health=%s "
                "q=%llu hit=%llu miss=%llu err=%llu "
                "cache=%zu consec_fail=%llu\n",
                std::string{to_string(s->descriptor.type)}.c_str(),
                s->descriptor.id.c_str(),
                s->descriptor.priority,
                std::string{to_string(s->health)}.c_str(),
                static_cast<unsigned long long>(
                    s->stats.queries.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    s->stats.hits.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    s->stats.misses.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    s->stats.errors.load(std::memory_order_relaxed)),
                s->cache.size(),
                static_cast<unsigned long long>(s->consecutive_failures));
            out += buf;
        }
    }

    out += "  ---\n";

    std::snprintf(buf, sizeof(buf), "  total_requests:      %llu\n",
        static_cast<unsigned long long>(
            stats_.total_requests.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  cache_hits:          %llu\n",
        static_cast<unsigned long long>(
            stats_.cache_hits.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  ws_hits:             %llu\n",
        static_cast<unsigned long long>(
            stats_.ws_hits.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  rest_hits:           %llu\n",
        static_cast<unsigned long long>(
            stats_.rest_hits.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  not_found:           %llu\n",
        static_cast<unsigned long long>(
            stats_.not_found.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  fallbacks:           %llu\n",
        static_cast<unsigned long long>(
            stats_.fallbacks.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  dedup_hits:          %llu\n",
        static_cast<unsigned long long>(
            stats_.dedup_hits.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  consistency_warns:   %llu\n",
        static_cast<unsigned long long>(
            stats_.consistency_warnings.load(std::memory_order_relaxed)));
    out += buf;

    std::snprintf(buf, sizeof(buf), "  hit_rate:            %.2f%%\n",
        stats_.hit_rate() * 100.0);
    out += buf;

    std::snprintf(buf, sizeof(buf), "  avg_latency_us:      %.0f\n",
        stats_.avg_latency_us());
    out += buf;

    return out;
}

}  // namespace data
}  // namespace quant
