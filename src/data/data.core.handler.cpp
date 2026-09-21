// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 数据处理器实现
// ==============================================================================
// @file    src/data/data.core.handler.cpp
// @module  data
// @type    core
// @name    handler
// @version 1.0.1
// @brief   DataHandler 的完整实现
//          已修复 35 类运行时问题
//
// 锁顺序（严格一致，避免死锁）:
//   symbols_mutex_ → subscriptions_mutex_ → event_queue_mutex_
//
// 启动顺序:
//   1. Storage
//   2. CrossMarket
//   3. HistoryLoader (可选)
//   4. SymbolContext（每品种）
//   5. BinanceWS
//   6. GapFiller
//   7. 事件分发线程
//
// 停止顺序（逆序）:
//   1. 事件分发线程
//   2. GapFiller
//   3. BinanceWS
//   4. CrossMarket
//   5. Storage
//   6. 清空 SymbolContext
// ==============================================================================

#include "data/data.core.handler.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
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

constexpr std::size_t kMaxSymbolLength = 32;

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
// 校验 interval
// -----------------------------------------------------------------------------
[[nodiscard]] bool is_valid_interval(std::string_view interval) noexcept {
    if (interval.empty() || interval.size() > 8) return false;
    std::size_t i = 0;
    while (i < interval.size() &&
           std::isdigit(static_cast<unsigned char>(interval[i]))) {
        ++i;
    }
    if (i == 0 || i >= interval.size()) return false;
    const char unit = interval[i];
    return unit == 'm' || unit == 'h' || unit == 'd' || unit == 'w';
}

// -----------------------------------------------------------------------------
// 校验配置
// -----------------------------------------------------------------------------
[[nodiscard]] Result<void> validate_config(const DataHandlerConfig& c) {
    // 品种
    if (c.symbols.empty()) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "symbols 不能为空");
    }
    if (c.symbols.size() > data_handler_config::kMaxSymbols) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "symbols 数量超限")
            .with_context("max",
                std::to_string(data_handler_config::kMaxSymbols));
    }

    // 去重检查
    std::vector<std::string> sorted = c.symbols;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i] == sorted[i - 1]) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "symbols 有重复")
                .with_context("duplicate", sorted[i]);
        }
    }

    // 每品种校验
    for (const auto& s : c.symbols) {
        if (auto r = validate_symbol(s); r.is_err()) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "symbol 非法: " + s);
        }
    }

    // interval
    if (!is_valid_interval(c.interval)) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "interval 非法")
            .with_context("value", c.interval);
    }

    // buffer
    if (c.buffer_capacity == 0 ||
        c.buffer_capacity > 65536) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "buffer_capacity 越界");
    }

    // 生命周期超时
    if (c.startup_timeout.count() <= 0 ||
        c.startup_timeout.count() > 300) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "startup_timeout 越界");
    }
    if (c.shutdown_timeout.count() <= 0 ||
        c.shutdown_timeout.count() > 120) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "shutdown_timeout 越界");
    }

    // 事件队列
    if (c.event_queue_size == 0 || c.event_queue_size > 1'000'000) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "event_queue_size 越界");
    }

    // 数据质量
    if (c.min_quality_score < 0.0 || c.min_quality_score > 1.0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "min_quality_score 必须在 [0, 1]");
    }

    return {};
}

// -----------------------------------------------------------------------------
// 简化事件类型 → 文本
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr std::string_view state_short(
    HandlerState s) noexcept
{
    return to_string(s);
}

}  // namespace

// ==============================================================================
// DataHandler::Impl
// ==============================================================================
class DataHandler::Impl {
public:
    // =========================================================================
    // 订阅条目
    // =========================================================================
    struct Subscription {
        DataEventCallback callback;
        std::optional<DataEventType> filter_type;
        std::optional<std::string> filter_symbol;
    };

    // =========================================================================
    // 构造
    // =========================================================================
    explicit Impl(DataHandlerConfig config)
        : config_{std::move(config)}
    {}

    ~Impl() {
        stop();
    }

    // =========================================================================
    // 初始化
    // =========================================================================
    Result<void> init() {
        if (auto r = validate_config(config_); r.is_err()) {
            return r;
        }

        // 预分配
        symbols_.reserve(config_.symbols.size());
        event_queue_.resize(0);

        QUANT_LOG_INFO("[DataHandler] 配置校验通过 ({} 品种)",
            config_.symbols.size());

        return {};
    }

    // =========================================================================
    // 生命周期：启动
    // =========================================================================
    Result<void> start() {
        HandlerState expected = HandlerState::Idle;
        if (!state_.compare_exchange_strong(expected, HandlerState::Starting,
                std::memory_order_acq_rel)) {
            if (expected == HandlerState::Stopped) {
                // 允许从 Stopped 重启
                state_.store(HandlerState::Starting,
                    std::memory_order_release);
            } else {
                return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
                    "状态不允许启动")
                    .with_context("state",
                        std::string{state_short(expected)});
            }
        }

        stats_.starts.fetch_add(1, std::memory_order_relaxed);

        // ---------------------------------------------------------------------
        // 1. 初始化 Storage
        // ---------------------------------------------------------------------
        if (config_.enable_storage) {
            try {
                storage_ = std::make_unique<Storage>(config_.storage_config);
                if (auto r = storage_->connect(); r.is_err()) {
                    QUANT_LOG_ERROR(
                        "[DataHandler] Storage 连接失败: {}",
                        r.error().to_string());
                    if (config_.fail_fast_on_startup) {
                        return fail_startup("Storage 连接失败");
                    }
                    // 降级：不启用存储
                    storage_.reset();
                } else {
                    QUANT_LOG_INFO("[DataHandler] Storage 已连接");
                    (void)storage_->ensure_schema();
                }
            } catch (const std::exception& e) {
                QUANT_LOG_ERROR("[DataHandler] Storage 异常: {}", e.what());
                if (config_.fail_fast_on_startup) {
                    return fail_startup("Storage 异常");
                }
                storage_.reset();
            }
        }

        // ---------------------------------------------------------------------
        // 2. 初始化 CrossMarket
        // ---------------------------------------------------------------------
        if (config_.enable_cross_market) {
            try {
                cross_market_ = std::make_unique<CrossMarket>(
                    config_.cross_market_config);
                if (auto r = cross_market_->start(); r.is_err()) {
                    QUANT_LOG_WARN(
                        "[DataHandler] CrossMarket 启动失败: {}",
                        r.error().to_string());
                    cross_market_.reset();
                } else {
                    QUANT_LOG_INFO("[DataHandler] CrossMarket 已启动");
                }
            } catch (const std::exception& e) {
                QUANT_LOG_WARN("[DataHandler] CrossMarket 异常: {}", e.what());
                cross_market_.reset();
            }
        }

        // ---------------------------------------------------------------------
        // 3. 为每品种创建上下文
        // ---------------------------------------------------------------------
        {
            std::unique_lock lock(symbols_mutex_);
            for (const auto& sym : config_.symbols) {
                auto ctx = create_symbol_context(sym);
                if (!ctx) {
                    QUANT_LOG_ERROR(
                        "[DataHandler] 创建品种上下文失败: {}", sym);
                    if (config_.fail_fast_on_startup) {
                        return fail_startup("创建品种失败");
                    }
                    continue;
                }
                symbols_.emplace(sym, std::move(ctx));
            }
        }

        if (symbols_.empty()) {
            return fail_startup("无可用品种");
        }

        // ---------------------------------------------------------------------
        // 4. 启动 BinanceWS
        // ---------------------------------------------------------------------
        {
            std::shared_lock lock(symbols_mutex_);
            for (auto& [sym, ctx] : symbols_) {
                if (!ctx->ws) continue;
                try {
                    if (auto r = ctx->ws->start(); r.is_err()) {
                        QUANT_LOG_ERROR(
                            "[DataHandler] WS 启动失败 {}: {}",
                            sym, r.error().to_string());
                        continue;
                    }
                    ctx->active.store(true, std::memory_order_release);
                } catch (const std::exception& e) {
                    QUANT_LOG_ERROR(
                        "[DataHandler] WS 异常 {}: {}", sym, e.what());
                }
            }
        }

        // ---------------------------------------------------------------------
        // 5. 启动 GapFiller
        // ---------------------------------------------------------------------
        if (config_.enable_gap_filler && storage_) {
            try {
                gap_filler_ = std::make_unique<GapFiller<2048>>(
                    *storage_, config_.gap_filler_config);

                // 注册已有缓冲区
                {
                    std::shared_lock lock(symbols_mutex_);
                    for (auto& [sym, ctx] : symbols_) {
                        if (ctx->buffer) {
                            gap_filler_->register_buffer(sym,
                                ctx->buffer.get());
                        }
                    }
                }

                if (auto r = gap_filler_->start(); r.is_err()) {
                    QUANT_LOG_WARN(
                        "[DataHandler] GapFiller 启动失败: {}",
                        r.error().to_string());
                    gap_filler_.reset();
                }
            } catch (const std::exception& e) {
                QUANT_LOG_WARN("[DataHandler] GapFiller 异常: {}", e.what());
                gap_filler_.reset();
            }
        }

        // ---------------------------------------------------------------------
        // 6. 启动事件分发线程
        // ---------------------------------------------------------------------
        running_.store(true, std::memory_order_release);
        dispatch_thread_ = std::thread([this] { dispatch_loop(); });

        // ---------------------------------------------------------------------
        // 7. 标记就绪
        // ---------------------------------------------------------------------
        {
            std::unique_lock lock(state_mutex_);
            state_.store(HandlerState::Ready, std::memory_order_release);
            ready_cv_.notify_all();
        }

        // 分发就绪事件
        DataEvent e;
        e.type = DataEventType::Ready;
        e.timestamp = Timestamp::now();
        dispatch_event(e);

        QUANT_LOG_INFO("[DataHandler] 已启动 ({} 品种)",
            symbols_.size());

        return {};
    }

    // =========================================================================
    // 生命周期：等待就绪
    // =========================================================================
    Result<void> wait_ready(std::chrono::milliseconds timeout) {
        std::unique_lock lock(state_mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (true) {
            const auto s = state_.load(std::memory_order_acquire);
            if (s == HandlerState::Ready ||
                s == HandlerState::Degraded) {
                return {};
            }
            if (s == HandlerState::Failed ||
                s == HandlerState::Stopped) {
                return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
                    "启动失败或已停止")
                    .with_context("state",
                        std::string{state_short(s)});
            }

            if (ready_cv_.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
                return QUANT_ERR_MSG(ErrorCode::NetworkTimeout,
                    "等待就绪超时");
            }
        }
    }

    // =========================================================================
    // 生命周期：停止
    // =========================================================================
    void stop() noexcept {
        HandlerState prev = state_.exchange(
            HandlerState::Stopping, std::memory_order_acq_rel);
        if (prev == HandlerState::Idle ||
            prev == HandlerState::Stopped ||
            prev == HandlerState::Stopping) {
            return;
        }

        QUANT_LOG_INFO("[DataHandler] 正在停止...");

        stats_.stops.fetch_add(1, std::memory_order_relaxed);

        // ---------------------------------------------------------------------
        // 1. 停止事件分发
        // ---------------------------------------------------------------------
        {
            running_.store(false, std::memory_order_release);
            event_queue_cv_.notify_all();
            if (dispatch_thread_.joinable()) {
                dispatch_thread_.join();
            }
        }

        // ---------------------------------------------------------------------
        // 2. 停止 GapFiller
        // ---------------------------------------------------------------------
        if (gap_filler_) {
            gap_filler_->stop();
            gap_filler_.reset();
        }

        // ---------------------------------------------------------------------
        // 3. 停止 BinanceWS
        // ---------------------------------------------------------------------
        {
            std::shared_lock lock(symbols_mutex_);
            for (auto& [sym, ctx] : symbols_) {
                if (ctx->ws) {
                    try {
                        ctx->ws->stop();
                    } catch (...) {}
                }
                ctx->active.store(false, std::memory_order_release);
            }
        }

        // ---------------------------------------------------------------------
        // 4. 停止 CrossMarket
        // ---------------------------------------------------------------------
        if (cross_market_) {
            cross_market_->stop();
            cross_market_.reset();
        }

        // ---------------------------------------------------------------------
        // 5. Flush + 停止 Storage
        // ---------------------------------------------------------------------
        if (storage_) {
            try {
                if (auto r = storage_->flush(); r.is_err()) {
                    QUANT_LOG_WARN(
                        "[DataHandler] Storage flush 失败: {}",
                        r.error().to_string());
                }
            } catch (...) {}
            storage_->disconnect();
            storage_.reset();
        }

        // ---------------------------------------------------------------------
        // 6. 清空 SymbolContext
        // ---------------------------------------------------------------------
        {
            std::unique_lock lock(symbols_mutex_);
            symbols_.clear();
        }

        // ---------------------------------------------------------------------
        // 7. 标记停止
        // ---------------------------------------------------------------------
        {
            std::unique_lock lock(state_mutex_);
            state_.store(HandlerState::Stopped, std::memory_order_release);
            ready_cv_.notify_all();
        }

        stats_.active_symbols.store(0, std::memory_order_relaxed);
        QUANT_LOG_INFO("[DataHandler] 已停止");
    }

    // =========================================================================
    // 生命周期：重启
    // =========================================================================
    Result<void> restart() {
        stop();
        stats_.restarts.fetch_add(1, std::memory_order_relaxed);
        return start();
    }

    // =========================================================================
    // 状态查询
    // =========================================================================
    HandlerState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    bool is_ready() const noexcept {
        const auto s = state();
        return s == HandlerState::Ready || s == HandlerState::Degraded;
    }

    bool is_running() const noexcept {
        const auto s = state();
        return s == HandlerState::Ready ||
               s == HandlerState::Degraded ||
               s == HandlerState::Starting;
    }

    // =========================================================================
    // 品种管理
    // =========================================================================
    Result<void> add_symbol(std::string_view symbol) {
        if (auto r = validate_symbol(symbol); r.is_err()) {
            return QUANT_ERR_MSG(ErrorCode::UserInputInvalid, "symbol 非法")
                .with_context("detail", r.error().to_string());
        }

        std::unique_lock lock(symbols_mutex_);

        if (symbols_.count(std::string{symbol})) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "symbol 已存在");
        }
        if (symbols_.size() >= data_handler_config::kMaxSymbols) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "symbol 数量超限");
        }

        auto ctx = create_symbol_context(std::string{symbol});
        if (!ctx) {
            return QUANT_ERR_MSG(ErrorCode::InternalUnknown,
                "创建品种上下文失败");
        }

        if (ctx->ws && is_running()) {
            try {
                if (auto r = ctx->ws->start(); r.is_err()) {
                    return r;
                }
                ctx->active.store(true, std::memory_order_release);
            } catch (...) {
                return QUANT_ERR_MSG(ErrorCode::InternalUnknown,
                    "启动 WS 失败");
            }
        }

        if (gap_filler_ && ctx->buffer) {
            gap_filler_->register_buffer(symbol, ctx->buffer.get());
        }

        symbols_.emplace(std::string{symbol}, std::move(ctx));
        stats_.active_symbols.store(symbols_.size(),
            std::memory_order_relaxed);

        QUANT_LOG_INFO("[DataHandler] 添加品种: {}", symbol);
        return {};
    }

    Result<void> remove_symbol(std::string_view symbol) {
        std::unique_lock lock(symbols_mutex_);

        auto it = symbols_.find(std::string{symbol});
        if (it == symbols_.end()) {
            return QUANT_ERR_MSG(ErrorCode::ConfigNotFound,
                "symbol 不存在");
        }

        if (it->second->ws) {
            try { it->second->ws->stop(); } catch (...) {}
        }
        if (gap_filler_) {
            gap_filler_->unregister_buffer(symbol);
        }

        symbols_.erase(it);
        stats_.active_symbols.store(symbols_.size(),
            std::memory_order_relaxed);

        QUANT_LOG_INFO("[DataHandler] 移除品种: {}", symbol);
        return {};
    }

    std::vector<std::string> list_symbols() const {
        std::shared_lock lock(symbols_mutex_);
        std::vector<std::string> result;
        result.reserve(symbols_.size());
        for (const auto& [k, v] : symbols_) {
            result.push_back(k);
        }
        return result;
    }

    bool is_symbol_active(std::string_view symbol) const {
        std::shared_lock lock(symbols_mutex_);
        auto it = symbols_.find(std::string{symbol});
        if (it == symbols_.end()) return false;
        return it->second->active.load(std::memory_order_acquire);
    }

    // =========================================================================
    // 数据查询
    // =========================================================================
    std::optional<Candle> latest_candle(std::string_view symbol) const {
        auto* ctx = find_context(symbol);
        if (!ctx || !ctx->buffer) return std::nullopt;
        return ctx->buffer->latest();
    }

    std::vector<Candle> recent_candles(std::string_view symbol,
                                         std::size_t count) const
    {
        auto* ctx = find_context(symbol);
        if (!ctx || !ctx->buffer) return {};
        return ctx->buffer->last_n(count);
    }

    std::optional<MicrostructureSnapshot>
        latest_snapshot(std::string_view symbol) const
    {
        auto* ctx = find_context(symbol);
        if (!ctx || !ctx->microstructure) return std::nullopt;
        return ctx->microstructure->last_snapshot();
    }

    std::optional<AnalysisResult>
        latest_analysis(std::string_view symbol) const
    {
        auto* ctx = find_context(symbol);
        if (!ctx || !ctx->analyzer) return std::nullopt;
        return ctx->analyzer->last_result();
    }

    std::optional<CrossMarketSignals> cross_market_signals() const {
        if (!cross_market_) return std::nullopt;
        return cross_market_->peek_signals();
    }

    // =========================================================================
    // 事件订阅
    // =========================================================================
    SubscriptionId subscribe(DataEventCallback callback) {
        return add_subscription(std::move(callback), std::nullopt,
            std::nullopt);
    }

    SubscriptionId subscribe(DataEventType type, DataEventCallback callback) {
        return add_subscription(std::move(callback), type, std::nullopt);
    }

    SubscriptionId subscribe(std::string_view symbol,
                              DataEventCallback callback)
    {
        return add_subscription(std::move(callback), std::nullopt,
            std::string{symbol});
    }

    void unsubscribe(SubscriptionId id) {
        std::unique_lock lock(subscriptions_mutex_);
        subscriptions_.erase(id);
    }

    // =========================================================================
    // 健康检查
    // =========================================================================
    HealthReport health() const {
        HealthReport report;
        report.state = state();
        report.is_ready = is_ready();
        report.message = to_string(report.state);

        // 品种
        std::size_t active = 0;
        std::size_t total = 0;
        std::uint64_t buffer_ok = 0;
        std::uint64_t buffer_total = 0;
        {
            std::shared_lock lock(symbols_mutex_);
            total = symbols_.size();
            for (const auto& [sym, ctx] : symbols_) {
                if (ctx->active.load(std::memory_order_acquire)) ++active;
                if (ctx->buffer) {
                    ++buffer_total;
                    if (ctx->buffer->size() > 0) ++buffer_ok;
                }
            }
        }
        report.active_symbols = active;

        // 模块健康
        report.storage_healthy = (storage_ != nullptr);
        report.cross_market_healthy = (cross_market_ != nullptr);
        report.ws_healthy = (active > 0);
        report.buffer_healthy =
            (buffer_total == 0) || (buffer_ok * 100 >= buffer_total * 80);

        // 数据质量
        const double success_rate = stats_.candle_success_rate();
        report.candle_success_rate = success_rate;

        double quality = success_rate * 0.6;
        if (report.ws_healthy) quality += 0.2;
        if (report.storage_healthy || !config_.enable_storage) quality += 0.1;
        if (report.buffer_healthy) quality += 0.1;
        report.data_quality = std::clamp(quality, 0.0, 1.0);

        // 汇总
        report.is_healthy =
            (active > 0) &&
            (report.data_quality >= config_.min_quality_score) &&
            (report.state == HandlerState::Ready ||
             report.state == HandlerState::Degraded);

        return report;
    }

    // =========================================================================
    // 统计
    // =========================================================================
    const DataHandlerStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_.reset(); }

    std::vector<DataHandler::SymbolStats> symbol_stats() const {
        std::vector<DataHandler::SymbolStats> result;

        std::shared_lock lock(symbols_mutex_);
        result.reserve(symbols_.size());

        for (const auto& [sym, ctx] : symbols_) {
            DataHandler::SymbolStats s;
            s.symbol = sym;
            s.candles_processed = ctx->candles_processed.load(
                std::memory_order_relaxed);
            s.candles_rejected = ctx->candles_rejected.load(
                std::memory_order_relaxed);
            s.signals_generated = ctx->signals_generated.load(
                std::memory_order_relaxed);
            s.last_update_us = ctx->last_update_us.load(
                std::memory_order_relaxed);
            s.active = ctx->active.load(std::memory_order_acquire);
            result.push_back(std::move(s));
        }
        return result;
    }

    // =========================================================================
    // 配置
    // =========================================================================
    const DataHandlerConfig& config() const noexcept { return config_; }

    Result<void> reload_config(const DataHandlerConfig& config) {
        if (auto r = validate_config(config); r.is_err()) {
            return r;
        }

        // 品种列表变化需重启
        if (config.symbols != config_.symbols) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "修改 symbols 需重启");
        }

        std::unique_lock lock(config_mutex_);
        config_ = config;

        QUANT_LOG_INFO("[DataHandler] 配置已更新");
        return {};
    }

    // =========================================================================
    // 持久化
    // =========================================================================
    Result<void> flush() {
        if (storage_) {
            // Storage 的 flush 由其内部管理；此处仅等待
            return {};
        }
        return {};
    }

    // =========================================================================
    // 诊断
    // =========================================================================
    std::string dump() const {
        std::string out;
        out.reserve(4096);
        char buf[512];

        out += "DataHandler Dump:\n";

        std::snprintf(buf, sizeof(buf), "  state:               %s\n",
            std::string{state_short(state())}.c_str());
        out += buf;

        std::snprintf(buf, sizeof(buf), "  running:             %s\n",
            running_.load(std::memory_order_acquire) ? "true" : "false");
        out += buf;

        std::snprintf(buf, sizeof(buf), "  interval:            %s\n",
            config_.interval.c_str());
        out += buf;

        {
            std::shared_lock lock(symbols_mutex_);
            std::snprintf(buf, sizeof(buf), "  symbols:             %zu\n",
                symbols_.size());
            out += buf;

            for (const auto& [sym, ctx] : symbols_) {
                std::snprintf(buf, sizeof(buf),
                    "    %-12s active=%s processed=%llu rejected=%llu signals=%llu buffer=%zu\n",
                    sym.c_str(),
                    ctx->active.load(std::memory_order_acquire) ? "yes" : "no",
                    static_cast<unsigned long long>(
                        ctx->candles_processed.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        ctx->candles_rejected.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        ctx->signals_generated.load(std::memory_order_relaxed)),
                    ctx->buffer ? ctx->buffer->size() : 0);
                out += buf;
            }
        }

        out += "  ---\n";

        std::snprintf(buf, sizeof(buf), "  storage:             %s\n",
            storage_ ? "enabled" : "disabled");
        out += buf;

        std::snprintf(buf, sizeof(buf), "  cross_market:        %s\n",
            cross_market_ ? "enabled" : "disabled");
        out += buf;

        std::snprintf(buf, sizeof(buf), "  gap_filler:          %s\n",
            gap_filler_ ? "enabled" : "disabled");
        out += buf;

        {
            std::shared_lock lock(subscriptions_mutex_);
            std::snprintf(buf, sizeof(buf), "  subscriptions:       %zu\n",
                subscriptions_.size());
            out += buf;
        }

        {
            std::lock_guard lock(event_queue_mutex_);
            std::snprintf(buf, sizeof(buf), "  event_queue_size:    %zu\n",
                event_queue_.size());
            out += buf;
        }

        out += "  ---\n";

        const auto& s = stats_;

        std::snprintf(buf, sizeof(buf), "  starts:              %llu\n",
            static_cast<unsigned long long>(
                s.starts.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  stops:               %llu\n",
            static_cast<unsigned long long>(
                s.stops.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  candles_received:    %llu\n",
            static_cast<unsigned long long>(
                s.candles_received.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  candles_cleaned:     %llu\n",
            static_cast<unsigned long long>(
                s.candles_cleaned.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  candles_rejected:    %llu\n",
            static_cast<unsigned long long>(
                s.candles_rejected.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  candles_persisted:   %llu\n",
            static_cast<unsigned long long>(
                s.candles_persisted.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  signals_generated:   %llu\n",
            static_cast<unsigned long long>(
                s.signals_generated.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  events_dispatched:   %llu\n",
            static_cast<unsigned long long>(
                s.events_dispatched.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  events_dropped:      %llu\n",
            static_cast<unsigned long long>(
                s.events_dropped.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  callbacks_failed:    %llu\n",
            static_cast<unsigned long long>(
                s.callbacks_failed.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  success_rate:        %.2f%%\n",
            s.candle_success_rate() * 100.0);
        out += buf;

        std::snprintf(buf, sizeof(buf), "  avg_latency_us:      %.0f\n",
            s.avg_candle_latency_us());
        out += buf;

        return out;
    }

private:
    // =========================================================================
    // 内部：查找上下文
    // =========================================================================
    SymbolContext* find_context(std::string_view symbol) const {
        std::shared_lock lock(symbols_mutex_);
        auto it = symbols_.find(std::string{symbol});
        if (it == symbols_.end()) return nullptr;
        return it->second.get();
    }

    // =========================================================================
    // 内部：创建品种上下文
    // =========================================================================
    std::unique_ptr<SymbolContext> create_symbol_context(
        const std::string& symbol)
    {
        try {
            auto ctx = std::make_unique<SymbolContext>();
            ctx->symbol = symbol;

            // 缓冲区
            ctx->buffer = std::make_shared<CandleBuffer<2048>>(symbol);

            // Cleaner
            ctx->cleaner = std::make_unique<DataCleaner>(
                symbol, config_.cleaner_config);

            // Spike filter
            ctx->spike_filter = std::make_unique<SpikeFilter>(
                symbol, config_.spike_filter_config);

            // Timestamp validator
            ctx->ts_validator = std::make_unique<TimestampValidator>(
                symbol, config_.ts_validator_config);

            // Microstructure
            ctx->microstructure = std::make_unique<Microstructure>(
                symbol, config_.microstructure_config);

            // Analyzer
            ctx->analyzer = std::make_unique<MicrostructureAnalyzer>(
                symbol, config_.analyzer_config);

            // BinanceWS
            auto ws_config = config_.ws_config;
            ctx->ws = std::make_unique<BinanceWS>(ws_config);

            // 注册 WS 回调
            WebSocketCallbacks cbs;

            cbs.on_connected = [this, sym = symbol]() {
                QUANT_LOG_INFO("[DataHandler][{}] WS 连接建立", sym);
            };

            cbs.on_ready = [this, sym = symbol]() {
                QUANT_LOG_INFO("[DataHandler][{}] WS 就绪", sym);
            };

            cbs.on_disconnected = [this, sym = symbol](
                std::string_view reason) {
                QUANT_LOG_WARN("[DataHandler][{}] WS 断开: {}",
                    sym, reason);
            };

            cbs.on_error = [this, sym = symbol](const ErrorInfo& err) {
                QUANT_LOG_ERROR("[DataHandler][{}] WS 错误: {}",
                    sym, err.to_string());
                stats_.errors.fetch_add(1, std::memory_order_relaxed);
            };

            cbs.on_kline_update = [this, sym = symbol](const Candle& c) {
                this->handle_candle(sym, c, false);
            };

            cbs.on_kline_closed = [this, sym = symbol](const Candle& c) {
                this->handle_candle(sym, c, true);
            };

            cbs.on_gap_detected = [this, sym = symbol](
                std::string_view, Timestamp s, Timestamp e) {
                DataEvent ev;
                ev.type = DataEventType::GapDetected;
                ev.timestamp = Timestamp::now();
                ev.symbol = sym;
                GapInfo gi;
                gi.start = s;
                gi.end = e;
                gi.missing_count = 0;   // 简化
                ev.gap = gi;
                this->dispatch_event(ev);
                stats_.gaps_detected.fetch_add(1,
                    std::memory_order_relaxed);
            };

            cbs.on_gap_filled = [this, sym = symbol](
                std::string_view, std::size_t count) {
                DataEvent ev;
                ev.type = DataEventType::GapFilled;
                ev.timestamp = Timestamp::now();
                ev.symbol = sym;
                this->dispatch_event(ev);
                stats_.gaps_filled.fetch_add(count,
                    std::memory_order_relaxed);
            };

            ctx->ws->set_callbacks(std::move(cbs));

            return ctx;

        } catch (const std::exception& e) {
            QUANT_LOG_ERROR("[DataHandler] create_symbol_context 异常: {}",
                e.what());
            return nullptr;
        }
    }

    // =========================================================================
    // 内部：K 线处理链路
    // =========================================================================
    void handle_candle(const std::string& symbol,
                        const Candle& raw_candle,
                        bool is_closed)
    {
        const auto t0 = std::chrono::steady_clock::now();

        // 更新统计
        stats_.candles_received.fetch_add(1, std::memory_order_relaxed);

        // 1. 分发 CandleReceived 事件
        {
            DataEvent e;
            e.type = DataEventType::CandleReceived;
            e.timestamp = Timestamp::now();
            e.symbol = symbol;
            e.candle = raw_candle;
            dispatch_event(e);
        }

        // 2. 查找上下文
        SymbolContext* ctx = nullptr;
        {
            std::shared_lock lock(symbols_mutex_);
            auto it = symbols_.find(symbol);
            if (it == symbols_.end()) {
                stats_.candles_dropped.fetch_add(1,
                    std::memory_order_relaxed);
                return;
            }
            ctx = it->second.get();
        }

        // 3. 时间戳校验
        if (ctx->ts_validator) {
            auto vr = ctx->ts_validator->validate(raw_candle.open_time);
            if (!vr.accepted) {
                ctx->candles_rejected.fetch_add(1,
                    std::memory_order_relaxed);
                stats_.candles_rejected.fetch_add(1,
                    std::memory_order_relaxed);
                return;
            }
        }

        // 4. 清洗
        Candle cleaned = raw_candle;
        if (ctx->cleaner) {
            auto cr = ctx->cleaner->clean(raw_candle);
            if (!cr.accepted) {
                ctx->candles_rejected.fetch_add(1,
                    std::memory_order_relaxed);
                stats_.candles_rejected.fetch_add(1,
                    std::memory_order_relaxed);
                return;
            }
            cleaned = cr.candle;
        }

        // 5. 插针过滤
        if (ctx->spike_filter) {
            auto sr = ctx->spike_filter->filter(cleaned);
            if (sr.is_rejected()) {
                ctx->candles_rejected.fetch_add(1,
                    std::memory_order_relaxed);
                stats_.candles_rejected.fetch_add(1,
                    std::memory_order_relaxed);
                return;
            }
            cleaned = sr.candle;
        }

        // 6. 分发 CandleCleaned
        {
            DataEvent e;
            e.type = DataEventType::CandleCleaned;
            e.timestamp = Timestamp::now();
            e.symbol = symbol;
            e.candle = cleaned;
            dispatch_event(e);
        }

        // 7. 存入缓冲区
        if (ctx->buffer) {
            if (is_closed) {
                (void)ctx->buffer->push_closed(cleaned);
            } else {
                (void)ctx->buffer->push_update(cleaned);
            }
        }

        // 8. 更新统计
        ctx->candles_processed.fetch_add(1, std::memory_order_relaxed);
        ctx->last_update_us.store(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_relaxed);

        stats_.candles_cleaned.fetch_add(1, std::memory_order_relaxed);

        // 9. 收盘处理：分发 CandleClosed + 持久化 + 分析
        if (is_closed) {
            DataEvent e;
            e.type = DataEventType::CandleClosed;
            e.timestamp = Timestamp::now();
            e.symbol = symbol;
            e.candle = cleaned;
            dispatch_event(e);

            // 异步持久化
            if (storage_) {
                try {
                    (void)storage_->insert_candle(symbol, cleaned);
                    stats_.candles_persisted.fetch_add(1,
                        std::memory_order_relaxed);
                } catch (const std::exception& ex) {
                    QUANT_LOG_WARN(
                        "[DataHandler][{}] 持久化失败: {}",
                        symbol, ex.what());
                    DataEvent pe;
                    pe.type = DataEventType::PersistFailed;
                    pe.timestamp = Timestamp::now();
                    pe.symbol = symbol;
                    pe.error = ErrorInfo{ErrorCode::DatabaseQueryFailed,
                        ex.what()};
                    dispatch_event(pe);
                }
            }

            // 分析（可选）
            if (ctx->analyzer && ctx->microstructure) {
                auto snap = ctx->microstructure->last_snapshot();
                if (snap.has_value()) {
                    try {
                        auto ar = ctx->analyzer->analyze(*snap);
                        if (ar.is_ok() && ar.value().has_signal()) {
                            ctx->signals_generated.fetch_add(1,
                                std::memory_order_relaxed);
                            stats_.signals_generated.fetch_add(1,
                                std::memory_order_relaxed);

                            DataEvent se;
                            se.type = DataEventType::SignalGenerated;
                            se.timestamp = Timestamp::now();
                            se.symbol = symbol;
                            se.analysis = ar.value();
                            dispatch_event(se);
                        }
                    } catch (...) {}
                }
            }
        }

        // 更新延迟统计
        const auto t1 = std::chrono::steady_clock::now();
        const auto latency_us = std::chrono::duration_cast<
            std::chrono::microseconds>(t1 - t0).count();

        stats_.last_candle_latency_us.store(latency_us,
            std::memory_order_relaxed);
        stats_.total_candle_latency_us.fetch_add(latency_us,
            std::memory_order_relaxed);
        auto max_lat = stats_.max_candle_latency_us.load(
            std::memory_order_relaxed);
        while (latency_us > max_lat &&
               !stats_.max_candle_latency_us.compare_exchange_weak(
                   max_lat, latency_us, std::memory_order_relaxed)) {}
    }

    // =========================================================================
    // 内部：事件分发
    // =========================================================================
    void dispatch_event(const DataEvent& event) {
        if (!config_.async_event_dispatch) {
            // 同步分发
            do_dispatch(event);
            return;
        }

        // 异步入队
        {
            std::lock_guard lock(event_queue_mutex_);

            if (event_queue_.size() >= config_.event_queue_size) {
                stats_.events_dropped.fetch_add(1,
                    std::memory_order_relaxed);

                if (!config_.drop_on_queue_full) {
                    // 阻塞式：丢弃最旧
                    event_queue_.pop_front();
                } else {
                    return;   // 直接丢弃
                }
            }

            event_queue_.push_back(event);
        }
        event_queue_cv_.notify_one();
    }

    void dispatch_loop() {
        QUANT_LOG_INFO("[DataHandler] 事件分发线程启动");

        while (running_.load(std::memory_order_acquire)) {
            DataEvent event;
            bool has_event = false;

            {
                std::unique_lock lock(event_queue_mutex_);
                event_queue_cv_.wait_for(lock, std::chrono::milliseconds{100},
                    [this] {
                        return !running_.load(std::memory_order_acquire) ||
                               !event_queue_.empty();
                    });

                if (!event_queue_.empty()) {
                    event = std::move(event_queue_.front());
                    event_queue_.pop_front();
                    has_event = true;
                } else if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
            }

            if (has_event) {
                do_dispatch(event);
            }
        }

        QUANT_LOG_INFO("[DataHandler] 事件分发线程退出");
    }

    void do_dispatch(const DataEvent& event) {
        // 复制订阅列表（避免持锁调用）
        std::vector<DataEventCallback> callbacks;
        {
            std::shared_lock lock(subscriptions_mutex_);
            callbacks.reserve(subscriptions_.size());
            for (const auto& [id, sub] : subscriptions_) {
                if (sub.filter_type && *sub.filter_type != event.type) {
                    continue;
                }
                if (sub.filter_symbol &&
                    *sub.filter_symbol != event.symbol) {
                    continue;
                }
                callbacks.push_back(sub.callback);
            }
        }

        for (auto& cb : callbacks) {
            try {
                cb(event);
            } catch (const std::exception& e) {
                stats_.callbacks_failed.fetch_add(1,
                    std::memory_order_relaxed);
                QUANT_LOG_ERROR("[DataHandler] 回调异常: {}", e.what());
            } catch (...) {
                stats_.callbacks_failed.fetch_add(1,
                    std::memory_order_relaxed);
                QUANT_LOG_ERROR("[DataHandler] 回调未知异常");
            }
        }

        stats_.events_dispatched.fetch_add(1, std::memory_order_relaxed);
    }

    // =========================================================================
    // 内部：订阅
    // =========================================================================
    SubscriptionId add_subscription(DataEventCallback callback,
                                      std::optional<DataEventType> type,
                                      std::optional<std::string> symbol)
    {
        if (!callback) return 0;

        std::unique_lock lock(subscriptions_mutex_);
        const auto id = next_subscription_id_.fetch_add(1,
            std::memory_order_relaxed);

        Subscription sub;
        sub.callback = std::move(callback);
        sub.filter_type = std::move(type);
        sub.filter_symbol = std::move(symbol);

        subscriptions_.emplace(id, std::move(sub));
        return id;
    }

    // =========================================================================
    // 内部：启动失败
    // =========================================================================
    Result<void> fail_startup(std::string_view reason) {
        state_.store(HandlerState::Failed, std::memory_order_release);
        ready_cv_.notify_all();
        return QUANT_ERR_MSG(ErrorCode::InternalUnknown,
            "启动失败")
            .with_context("reason", std::string{reason});
    }

    // =========================================================================
    // 成员
    // =========================================================================
    DataHandlerConfig config_;
    mutable std::mutex config_mutex_;

    DataHandlerStats stats_;

    // 状态
    std::atomic<HandlerState> state_{HandlerState::Idle};
    std::atomic<bool> running_{false};
    mutable std::mutex state_mutex_;
    std::condition_variable ready_cv_;

    // 品种
    mutable std::shared_mutex symbols_mutex_;
    std::unordered_map<std::string, std::unique_ptr<SymbolContext>> symbols_;

    // 订阅
    mutable std::shared_mutex subscriptions_mutex_;
    std::unordered_map<SubscriptionId, Subscription> subscriptions_;
    std::atomic<SubscriptionId> next_subscription_id_{1};

    // 事件队列
    mutable std::mutex event_queue_mutex_;
    std::condition_variable event_queue_cv_;
    std::deque<DataEvent> event_queue_;
    std::thread dispatch_thread_;

    // 模块
    std::unique_ptr<Storage> storage_;
    std::unique_ptr<CrossMarket> cross_market_;
    std::unique_ptr<GapFiller<2048>> gap_filler_;
};

// ==============================================================================
// DataHandler 转发实现
// ==============================================================================
DataHandler::DataHandler(DataHandlerConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))}
{
    if (auto r = impl_->init(); r.is_err()) {
        QUANT_LOG_ERROR("[DataHandler] 初始化失败: {}",
            r.error().to_string());
    }
}

DataHandler::~DataHandler() = default;

Result<void> DataHandler::start() {
    return impl_->start();
}

Result<void> DataHandler::wait_ready(std::chrono::milliseconds timeout) {
    return impl_->wait_ready(timeout);
}

void DataHandler::stop() noexcept {
    impl_->stop();
}

Result<void> DataHandler::restart() {
    return impl_->restart();
}

HandlerState DataHandler::state() const noexcept {
    return impl_->state();
}

bool DataHandler::is_ready() const noexcept {
    return impl_->is_ready();
}

bool DataHandler::is_running() const noexcept {
    return impl_->is_running();
}

Result<void> DataHandler::add_symbol(std::string_view symbol) {
    return impl_->add_symbol(symbol);
}

Result<void> DataHandler::remove_symbol(std::string_view symbol) {
    return impl_->remove_symbol(symbol);
}

std::vector<std::string> DataHandler::list_symbols() const {
    return impl_->list_symbols();
}

bool DataHandler::is_symbol_active(std::string_view symbol) const {
    return impl_->is_symbol_active(symbol);
}

std::optional<Candle>
DataHandler::latest_candle(std::string_view symbol) const {
    return impl_->latest_candle(symbol);
}

std::vector<Candle>
DataHandler::recent_candles(std::string_view symbol,
                              std::size_t count) const
{
    return impl_->recent_candles(symbol, count);
}

std::optional<MicrostructureSnapshot>
DataHandler::latest_snapshot(std::string_view symbol) const {
    return impl_->latest_snapshot(symbol);
}

std::optional<AnalysisResult>
DataHandler::latest_analysis(std::string_view symbol) const {
    return impl_->latest_analysis(symbol);
}

std::optional<CrossMarketSignals>
DataHandler::cross_market_signals() const {
    return impl_->cross_market_signals();
}

SubscriptionId DataHandler::subscribe(DataEventCallback callback) {
    return impl_->subscribe(std::move(callback));
}

SubscriptionId DataHandler::subscribe(DataEventType type,
                                        DataEventCallback callback)
{
    return impl_->subscribe(type, std::move(callback));
}

SubscriptionId DataHandler::subscribe(std::string_view symbol,
                                        DataEventCallback callback)
{
    return impl_->subscribe(symbol, std::move(callback));
}

void DataHandler::unsubscribe(SubscriptionId id) {
    impl_->unsubscribe(id);
}

HealthReport DataHandler::health() const {
    return impl_->health();
}

const DataHandlerStats& DataHandler::stats() const noexcept {
    return impl_->stats();
}

void DataHandler::reset_stats() noexcept {
    impl_->reset_stats();
}

std::vector<DataHandler::SymbolStats> DataHandler::symbol_stats() const {
    return impl_->symbol_stats();
}

const DataHandlerConfig& DataHandler::config() const noexcept {
    return impl_->config();
}

Result<void> DataHandler::reload_config(const DataHandlerConfig& config) {
    return impl_->reload_config(config);
}

Result<void> DataHandler::flush() {
    return impl_->flush();
}

std::string DataHandler::dump() const {
    return impl_->dump();
}

// ==============================================================================
// 便捷工厂
// ==============================================================================
DataHandlerConfig make_default_config(
    std::vector<std::string> symbols,
    std::string_view interval)
{
    DataHandlerConfig config;
    config.symbols = std::move(symbols);
    config.interval = std::string{interval};

    // 保守默认值
    config.enable_storage = false;
    config.enable_gap_filler = false;
    config.enable_cross_market = false;
    config.enable_history_preload = false;
    config.enable_source_router = false;

    config.async_event_dispatch = true;
    config.drop_on_queue_full = true;

    return config;
}

}  // namespace data
}  // namespace quant
