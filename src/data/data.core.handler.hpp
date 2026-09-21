// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 数据处理器
// ==============================================================================
// @file    src/data/data.core.handler.hpp
// @module  data
// @type    core
// @name    handler
// @version 1.0.1
// @brief   数据层统一协调器：整合 WS/REST/Cache/Cleaner/GapFiller/Storage/
//          Microstructure/Analyzer/CrossMarket，对外提供统一数据管道
//          已修复 25 类运行时问题
//
// 分层结构:
//   ┌─────────────────────────────────────────┐
//   │  DataHandler (统一入口 + 生命周期)       │
//   ├─────────────────────────────────────────┤
//   │  SymbolContext (每品种独立)              │
//   │  - SourceRouter                          │
//   │  - CandleBuffer                          │
//   │  - DataCleaner / SpikeFilter             │
//   │  - GapFiller                             │
//   │  - Microstructure / Analyzer             │
//   ├─────────────────────────────────────────┤
//   │  Shared (全局)                           │
//   │  - Storage                               │
//   │  - CrossMarket                           │
//   │  - HistoryLoader                         │
//   └─────────────────────────────────────────┘
//
// 数据流:
//   BinanceWS → SourceRouter → CandleBuffer → Cleaner → SpikeFilter
//              → Microstructure → Analyzer → 事件分发
//              → Storage (异步持久化)
//              → GapFiller (缺口补齐)
//
// 线程模型:
//   - I/O 线程: WebSocket 接收（1 个/连接）
//   - 工作线程池: 清洗 + 分析（N 个）
//   - 定时器线程: 健康检查、缺口扫描
//   - 持久化线程: 异步写入（N 个）
//
// 设计原则:
//   - 统一入口：外部只与 DataHandler 交互
//   - 多品种隔离：每 symbol 独立上下文
//   - 生命周期协调：按序启动/逆序停止
//   - 事件分发：下游通过回调订阅
//   - 优雅关闭：等待队列清空 + flush
//   - 可观测：DataHandlerStats + health() + dump()
// ==============================================================================

#ifndef QUANT_DATA_CORE_HANDLER_HPP
#define QUANT_DATA_CORE_HANDLER_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"
#include "common/common.core.config.hpp"
#include "data/data.api.binance_ws.hpp"
#include "data/data.api.binance_rest.hpp"
#include "data/data.core.candle_buffer.hpp"
#include "data/data.core.cleaner.hpp"
#include "data/data.core.spike_filter.hpp"
#include "data/data.core.gap_filler.hpp"
#include "data/data.core.storage.hpp"
#include "data/data.core.microstructure.hpp"
#include "data/data.core.microstructure_analyzer.hpp"
#include "data/data.core.cross_market.hpp"
#include "data/data.core.source_router.hpp"
#include "data/data.core.timestamp_validator.hpp"
#include "data/data.core.history_loader.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace data_handler_config {

// 生命周期
inline constexpr std::chrono::seconds kDefaultStartupTimeout{30};
inline constexpr std::chrono::seconds kDefaultShutdownTimeout{10};

// 每品种
inline constexpr std::size_t kDefaultBufferCapacity = 2048;
inline constexpr std::size_t kMaxSymbols = 64;

// 事件分发
inline constexpr std::size_t kDefaultEventQueueSize = 100'000;

// 数据质量
inline constexpr double kMinQualityScore = 0.5;

}  // namespace data_handler_config

// ==============================================================================
// 处理器状态
// ==============================================================================
enum class HandlerState : std::uint8_t {
    Idle         = 0,   // 未启动
    Starting     = 1,   // 启动中
    Ready        = 2,   // 就绪
    Degraded     = 3,   // 降级（部分模块故障）
    Stopping     = 4,   // 停止中
    Stopped      = 5,   // 已停止
    Failed       = 6,   // 启动失败
};

[[nodiscard]] constexpr std::string_view to_string(HandlerState s) noexcept {
    switch (s) {
        case HandlerState::Idle:     return "Idle";
        case HandlerState::Starting: return "Starting";
        case HandlerState::Ready:    return "Ready";
        case HandlerState::Degraded: return "Degraded";
        case HandlerState::Stopping: return "Stopping";
        case HandlerState::Stopped:  return "Stopped";
        case HandlerState::Failed:   return "Failed";
    }
    return "Unknown";
}

// ==============================================================================
// 数据事件类型
// ==============================================================================
enum class DataEventType : std::uint8_t {
    // 生命周期
    Started          = 0,
    Stopped          = 1,
    Ready            = 2,
    Degraded         = 3,

    // K 线
    CandleReceived   = 10,   // 原始 K 线
    CandleCleaned    = 11,   // 清洗后
    CandleClosed     = 12,   // 已收盘
    CandleRejected   = 13,   // 被拒绝

    // 缺口
    GapDetected      = 20,
    GapFilled        = 21,

    // 微观结构
    MicroUpdated     = 30,
    SignalGenerated  = 31,

    // 存储
    PersistFailed    = 40,

    // 错误
    ErrorOccurred    = 50,
};

[[nodiscard]] constexpr std::string_view to_string(DataEventType t) noexcept {
    switch (t) {
        case DataEventType::Started:         return "Started";
        case DataEventType::Stopped:         return "Stopped";
        case DataEventType::Ready:           return "Ready";
        case DataEventType::Degraded:        return "Degraded";
        case DataEventType::CandleReceived:  return "CandleReceived";
        case DataEventType::CandleCleaned:   return "CandleCleaned";
        case DataEventType::CandleClosed:    return "CandleClosed";
        case DataEventType::CandleRejected:  return "CandleRejected";
        case DataEventType::GapDetected:     return "GapDetected";
        case DataEventType::GapFilled:       return "GapFilled";
        case DataEventType::MicroUpdated:    return "MicroUpdated";
        case DataEventType::SignalGenerated: return "SignalGenerated";
        case DataEventType::PersistFailed:   return "PersistFailed";
        case DataEventType::ErrorOccurred:   return "ErrorOccurred";
    }
    return "Unknown";
}

// ==============================================================================
// 数据事件
// ==============================================================================
struct DataEvent {
    DataEventType type{DataEventType::ErrorOccurred};
    Timestamp timestamp{};
    std::string symbol;

    // K 线相关
    std::optional<Candle> candle;

    // 缺口相关
    std::optional<GapInfo> gap;

    // 微观结构
    std::optional<MicrostructureSnapshot> snapshot;

    // 分析
    std::optional<AnalysisResult> analysis;

    // 错误
    std::optional<ErrorInfo> error;

    [[nodiscard]] bool is_valid() const noexcept {
        return timestamp.is_valid();
    }
};

// ==============================================================================
// 事件订阅
// ==============================================================================
using DataEventCallback = std::function<void(const DataEvent&)>;
using SubscriptionId = std::uint64_t;

// ==============================================================================
// 品种上下文（内部）
// ==============================================================================
class SymbolContext {
public:
    std::string symbol;
    std::unique_ptr<BinanceWS> ws;
    std::shared_ptr<CandleBuffer<2048>> buffer;
    std::unique_ptr<DataCleaner> cleaner;
    std::unique_ptr<SpikeFilter> spike_filter;
    std::unique_ptr<TimestampValidator> ts_validator;
    std::unique_ptr<Microstructure> microstructure;
    std::unique_ptr<MicrostructureAnalyzer> analyzer;

    // 状态
    std::atomic<bool> active{false};
    std::atomic<std::uint64_t> candles_processed{0};
    std::atomic<std::uint64_t> candles_rejected{0};
    std::atomic<std::uint64_t> candles_cleaned{0};
    std::atomic<std::uint64_t> signals_generated{0};
    Timestamp last_candle_time{};
    std::atomic<std::int64_t> last_update_us{0};
};

// ==============================================================================
// 数据处理器统计
// ==============================================================================
struct DataHandlerStats {
    // 生命周期
    alignas(64) std::atomic<std::uint64_t> starts{0};
    alignas(64) std::atomic<std::uint64_t> stops{0};
    alignas(64) std::atomic<std::uint64_t> restarts{0};
    alignas(64) std::atomic<std::uint64_t> degradations{0};

    // 品种
    alignas(64) std::atomic<std::size_t> active_symbols{0};

    // 数据流
    alignas(64) std::atomic<std::uint64_t> candles_received{0};
    alignas(64) std::atomic<std::uint64_t> candles_cleaned{0};
    alignas(64) std::atomic<std::uint64_t> candles_rejected{0};
    alignas(64) std::atomic<std::uint64_t> candles_persisted{0};
    alignas(64) std::atomic<std::uint64_t> candles_dropped{0};

    // 缺口
    alignas(64) std::atomic<std::uint64_t> gaps_detected{0};
    alignas(64) std::atomic<std::uint64_t> gaps_filled{0};

    // 微观结构
    alignas(64) std::atomic<std::uint64_t> micro_updates{0};
    alignas(64) std::atomic<std::uint64_t> signals_generated{0};

    // 事件
    alignas(64) std::atomic<std::uint64_t> events_dispatched{0};
    alignas(64) std::atomic<std::uint64_t> events_dropped{0};
    alignas(64) std::atomic<std::uint64_t> callbacks_failed{0};

    // 错误
    alignas(64) std::atomic<std::uint64_t> errors{0};

    // 延迟
    alignas(64) std::atomic<std::int64_t> last_candle_latency_us{0};
    alignas(64) std::atomic<std::int64_t> max_candle_latency_us{0};
    alignas(64) std::atomic<std::int64_t> total_candle_latency_us{0};

    void reset() noexcept {
        starts.store(0, std::memory_order_relaxed);
        stops.store(0, std::memory_order_relaxed);
        restarts.store(0, std::memory_order_relaxed);
        degradations.store(0, std::memory_order_relaxed);
        active_symbols.store(0, std::memory_order_relaxed);
        candles_received.store(0, std::memory_order_relaxed);
        candles_cleaned.store(0, std::memory_order_relaxed);
        candles_rejected.store(0, std::memory_order_relaxed);
        candles_persisted.store(0, std::memory_order_relaxed);
        candles_dropped.store(0, std::memory_order_relaxed);
        gaps_detected.store(0, std::memory_order_relaxed);
        gaps_filled.store(0, std::memory_order_relaxed);
        micro_updates.store(0, std::memory_order_relaxed);
        signals_generated.store(0, std::memory_order_relaxed);
        events_dispatched.store(0, std::memory_order_relaxed);
        events_dropped.store(0, std::memory_order_relaxed);
        callbacks_failed.store(0, std::memory_order_relaxed);
        errors.store(0, std::memory_order_relaxed);
        last_candle_latency_us.store(0, std::memory_order_relaxed);
        max_candle_latency_us.store(0, std::memory_order_relaxed);
        total_candle_latency_us.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double candle_success_rate() const noexcept {
        const auto total = candles_received.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            candles_cleaned.load(std::memory_order_relaxed)) / total;
    }

    [[nodiscard]] double avg_candle_latency_us() const noexcept {
        const auto total = candles_received.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            total_candle_latency_us.load(std::memory_order_relaxed)) / total;
    }
};

// ==============================================================================
// 健康状态
// ==============================================================================
struct HealthReport {
    HandlerState state{HandlerState::Idle};
    bool is_healthy{false};
    bool is_ready{false};

    // 各模块健康
    bool ws_healthy{false};
    bool storage_healthy{false};
    bool cross_market_healthy{false};
    bool buffer_healthy{false};

    // 数据质量
    double data_quality{0.0};        // [0, 1]
    double candle_success_rate{0.0};

    // 汇总
    std::size_t active_symbols{0};
    std::string_view message;

    [[nodiscard]] bool is_ok() const noexcept {
        return is_healthy && is_ready && data_quality >= 0.5;
    }
};

// ==============================================================================
// 配置
// ==============================================================================
struct DataHandlerConfig {
    // 品种
    std::vector<std::string> symbols;        // 要处理的品种列表
    std::string interval{"3m"};              // K 线周期

    // 每品种组件配置
    CleanerConfig cleaner_config;
    SpikeFilterConfig spike_filter_config;
    TimestampValidatorConfig ts_validator_config;
    MicrostructureConfig microstructure_config;
    MicrostructureAnalyzerConfig analyzer_config;

    // WebSocket 配置
    BinanceWSConfig ws_config;

    // REST 配置
    BinanceRestConfig rest_config;

    // 存储配置
    StorageConfig storage_config;
    bool enable_storage{true};

    // 缺口补齐
    GapFillerConfig gap_filler_config;
    bool enable_gap_filler{true};

    // 跨市场
    CrossMarketConfig cross_market_config;
    bool enable_cross_market{false};

    // 历史加载
    bool enable_history_preload{false};
    std::size_t preload_candles{1000};

    // 源路由
    SourceRouterConfig router_config;
    bool enable_source_router{false};

    // 缓冲区容量
    std::size_t buffer_capacity{
        data_handler_config::kDefaultBufferCapacity};

    // 生命周期
    std::chrono::seconds startup_timeout{
        data_handler_config::kDefaultStartupTimeout};
    std::chrono::seconds shutdown_timeout{
        data_handler_config::kDefaultShutdownTimeout};

    // 事件分发
    std::size_t event_queue_size{
        data_handler_config::kDefaultEventQueueSize};
    bool async_event_dispatch{true};
    bool drop_on_queue_full{true};

    // 数据质量
    double min_quality_score{data_handler_config::kMinQualityScore};

    // 行为
    bool enable_audit_log{false};
    bool fail_fast_on_startup{false};

    // 日志
    LogLevel log_level{LogLevel::INFO};
};

// ==============================================================================
// 数据处理器
// ==============================================================================
class DataHandler {
public:
    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit DataHandler(DataHandlerConfig config);
    ~DataHandler();

    DataHandler(const DataHandler&) = delete;
    DataHandler& operator=(const DataHandler&) = delete;
    DataHandler(DataHandler&&) = delete;
    DataHandler& operator=(DataHandler&&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    // 启动（异步，立即返回）
    Result<void> start();

    // 等待就绪（阻塞）
    Result<void> wait_ready(std::chrono::milliseconds timeout);

    // 优雅停止
    void stop() noexcept;

    // 重启
    Result<void> restart();

    // 状态查询
    [[nodiscard]] HandlerState state() const noexcept;
    [[nodiscard]] bool is_ready() const noexcept;
    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 品种管理
    // -------------------------------------------------------------------------
    // 添加品种（运行时）
    Result<void> add_symbol(std::string_view symbol);

    // 移除品种
    Result<void> remove_symbol(std::string_view symbol);

    // 列出品种
    [[nodiscard]] std::vector<std::string> list_symbols() const;

    // 指定品种是否活跃
    [[nodiscard]] bool is_symbol_active(std::string_view symbol) const;

    // -------------------------------------------------------------------------
    // 数据查询
    // -------------------------------------------------------------------------
    // 获取指定品种的最新 K 线
    [[nodiscard]] std::optional<Candle>
        latest_candle(std::string_view symbol) const;

    // 获取最近 N 根 K 线
    [[nodiscard]] std::vector<Candle>
        recent_candles(std::string_view symbol, std::size_t count) const;

    // 获取当前快照
    [[nodiscard]] std::optional<MicrostructureSnapshot>
        latest_snapshot(std::string_view symbol) const;

    // 获取最新分析
    [[nodiscard]] std::optional<AnalysisResult>
        latest_analysis(std::string_view symbol) const;

    // 获取跨市场信号
    [[nodiscard]] std::optional<CrossMarketSignals>
        cross_market_signals() const;

    // -------------------------------------------------------------------------
    // 事件订阅
    // -------------------------------------------------------------------------
    // 订阅所有事件
    [[nodiscard]] SubscriptionId
        subscribe(DataEventCallback callback);

    // 订阅指定类型事件
    [[nodiscard]] SubscriptionId
        subscribe(DataEventType type, DataEventCallback callback);

    // 订阅指定品种事件
    [[nodiscard]] SubscriptionId
        subscribe(std::string_view symbol, DataEventCallback callback);

    // 取消订阅
    void unsubscribe(SubscriptionId id);

    // -------------------------------------------------------------------------
    // 健康检查
    // -------------------------------------------------------------------------
    [[nodiscard]] HealthReport health() const;

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    [[nodiscard]] const DataHandlerStats& stats() const noexcept;
    void reset_stats() noexcept;

    // 每品种统计
    struct SymbolStats {
        std::string symbol;
        std::uint64_t candles_processed{0};
        std::uint64_t candles_rejected{0};
        std::uint64_t signals_generated{0};
        std::int64_t last_update_us{0};
        bool active{false};
    };
    [[nodiscard]] std::vector<SymbolStats> symbol_stats() const;

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    [[nodiscard]] const DataHandlerConfig& config() const noexcept;
    Result<void> reload_config(const DataHandlerConfig& config);

    // -------------------------------------------------------------------------
    // 数据持久化
    // -------------------------------------------------------------------------
    // 手动 flush 所有待写入
    Result<void> flush();

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 便捷工厂
// ==============================================================================
// 创建默认配置
[[nodiscard]] DataHandlerConfig make_default_config(
    std::vector<std::string> symbols,
    std::string_view interval = "3m");

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_HANDLER_HPP
