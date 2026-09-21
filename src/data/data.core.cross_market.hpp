// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 跨市场数据
// ==============================================================================
// @file    src/data/data.core.cross_market.hpp
// @module  data
// @type    core
// @name    cross_market
// @version 1.0.1
// @brief   跨市场信号聚合：BTC 主导、市值、恐慌指数、稳定币流入、期货基差、
//          跨交易所价差、宏观事件日历、跨市场背离
//          已修复 25 类运行时问题
//
// 数据来源:
//   - CoinGecko / CoinMarketCap (市值、BTC 主导)
//   - Alternative.me (恐惧与贪婪指数)
//   - DefiLlama (稳定币流入)
//   - Binance/OKX (期货基差、跨交易所价差)
//   - 经济日历 API (宏观事件)
//
// 设计原则:
//   - Provider 抽象：可插拔的数据源
//   - TTL 缓存：减少外部调用
//   - 异步更新：后台线程定时刷新
//   - 降级保留：API 失败保留上次有效值
//   - 数据校验：范围检查 + 时间戳
//   - 线程安全：shared_mutex
//   - 完整可观测：CrossMarketStats + dump()
// ==============================================================================

#ifndef QUANT_DATA_CORE_CROSS_MARKET_HPP
#define QUANT_DATA_CORE_CROSS_MARKET_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace cross_market_config {

// 默认 TTL
inline constexpr std::chrono::seconds kDefaultTtl{60};          // 1 分钟
inline constexpr std::chrono::seconds kDefaultStaleThreshold{300}; // 5 分钟
inline constexpr std::chrono::seconds kDefaultRefreshInterval{30};

// 范围校验
inline constexpr double kBtcDominanceMin{20.0};
inline constexpr double kBtcDominanceMax{90.0};
inline constexpr double kFearGreedMin{0.0};
inline constexpr double kFearGreedMax{100.0};
inline constexpr double kMarketCapMin{1e10};
inline constexpr double kMarketCapMax{1e14};
inline constexpr double kBasisMin{-0.20};   // ±20%
inline constexpr double kBasisMax{0.20};

// 滚动窗口
inline constexpr std::size_t kDefaultHistorySize{100};
inline constexpr std::size_t kDefaultCorrelationWindow{60};

}  // namespace cross_market_config

// ==============================================================================
// 数据质量
// ==============================================================================
enum class DataQuality : std::uint8_t {
    Fresh    = 0,   // 新鲜
    Cached   = 1,   // 缓存中
    Stale    = 2,   // 过期
    Unknown  = 3,   // 未知（首次未获取）
    Invalid  = 4,   // 无效（校验失败）
};

[[nodiscard]] constexpr std::string_view to_string(DataQuality q) noexcept {
    switch (q) {
        case DataQuality::Fresh:   return "Fresh";
        case DataQuality::Cached:  return "Cached";
        case DataQuality::Stale:   return "Stale";
        case DataQuality::Unknown: return "Unknown";
        case DataQuality::Invalid: return "Invalid";
    }
    return "Unknown";
}

// ==============================================================================
// 带时间戳的数值
// ==============================================================================
template <typename T>
struct TimedValue {
    T value{};
    Timestamp timestamp{};
    DataQuality quality{DataQuality::Unknown};

    [[nodiscard]] bool is_valid() const noexcept {
        return timestamp.is_valid() &&
               quality != DataQuality::Unknown &&
               quality != DataQuality::Invalid;
    }

    [[nodiscard]] bool is_stale(std::chrono::seconds threshold) const noexcept {
        if (!timestamp.is_valid()) return true;
        const auto now_us = Timestamp::now().microseconds();
        const auto age_us = now_us - timestamp.microseconds();
        return age_us > std::chrono::duration_cast<
            std::chrono::microseconds>(threshold).count();
    }
};

// ==============================================================================
// 跨市场信号
// ==============================================================================
struct CrossMarketSignals {
    // -------------------------------------------------------------------------
    // 主信号
    // -------------------------------------------------------------------------
    TimedValue<double> btc_dominance;      // BTC 市占率 [%]
    TimedValue<double> total_market_cap;   // 总市值 [USD]
    TimedValue<double> fear_greed_index;   // 恐慌贪婪 [0, 100]
    TimedValue<double> stablecoin_flow;    // 稳定币净流入 [USD]
    TimedValue<double> futures_basis;      // 期货基差 [%]

    // -------------------------------------------------------------------------
    // 辅助
    // -------------------------------------------------------------------------
    TimedValue<double> eth_dominance;
    TimedValue<double> usdt_premium;       // USDT 溢价
    TimedValue<double> funding_rate_avg;   // 平均资金费率

    // 数据完整性
    Timestamp snapshot_time{};
    std::size_t field_count{0};
    std::size_t valid_field_count{0};

    [[nodiscard]] bool is_valid() const noexcept {
        return snapshot_time.is_valid() &&
               valid_field_count >= 3;   // 至少 3 个有效字段
    }

    [[nodiscard]] double completeness() const noexcept {
        if (field_count == 0) return 0.0;
        return static_cast<double>(valid_field_count) / field_count;
    }
};

// ==============================================================================
// 跨交易所价差
// ==============================================================================
struct CrossExchangeSpread {
    std::string symbol;
    std::string exchange_a;
    std::string exchange_b;
    double price_a{0.0};
    double price_b{0.0};
    double spread{0.0};           // price_a - price_b
    double spread_pct{0.0};       // spread / mid
    double spread_bps{0.0};
    Timestamp timestamp{};

    [[nodiscard]] bool is_valid() const noexcept {
        return timestamp.is_valid() &&
               std::isfinite(price_a) && price_a > 0.0 &&
               std::isfinite(price_b) && price_b > 0.0;
    }

    [[nodiscard]] bool is_significant(double threshold_bps = 5.0) const noexcept {
        return std::abs(spread_bps) >= threshold_bps;
    }
};

// ==============================================================================
// 宏观事件
// ==============================================================================
enum class MacroEventType : std::uint8_t {
    Unknown       = 0,
    FOMC          = 1,   // 美联储议息
    CPI           = 2,   // 消费者物价指数
    NFP           = 3,   // 非农
    GDP           = 4,
    PPI           = 5,
    RetailSales   = 6,
    CryptoEvent   = 7,   // 加密专属事件（减半、升级）
};

[[nodiscard]] constexpr std::string_view to_string(MacroEventType t) noexcept {
    switch (t) {
        case MacroEventType::Unknown:     return "Unknown";
        case MacroEventType::FOMC:        return "FOMC";
        case MacroEventType::CPI:         return "CPI";
        case MacroEventType::NFP:         return "NFP";
        case MacroEventType::GDP:         return "GDP";
        case MacroEventType::PPI:         return "PPI";
        case MacroEventType::RetailSales: return "RetailSales";
        case MacroEventType::CryptoEvent: return "CryptoEvent";
    }
    return "Unknown";
}

enum class EventImportance : std::uint8_t {
    Low    = 0,
    Medium = 1,
    High   = 2,
};

struct MacroEvent {
    MacroEventType type{MacroEventType::Unknown};
    EventImportance importance{EventImportance::Low};
    Timestamp scheduled_time{};
    std::string name;
    std::string description;

    [[nodiscard]] bool is_valid() const noexcept {
        return scheduled_time.is_valid() && !name.empty();
    }

    [[nodiscard]] bool is_upcoming(
        std::chrono::minutes window) const noexcept
    {
        if (!scheduled_time.is_valid()) return false;
        const auto now_us = Timestamp::now().microseconds();
        const auto event_us = scheduled_time.microseconds();
        const auto delta_us = event_us - now_us;
        const auto window_us = std::chrono::duration_cast<
            std::chrono::microseconds>(window).count();
        return delta_us >= 0 && delta_us <= window_us;
    }
};

// ==============================================================================
// 跨市场背离
// ==============================================================================
enum class DivergenceType : std::uint8_t {
    None           = 0,
    BtcUpEthDown   = 1,
    BtcDownEthUp   = 2,
    CryptoUpGoldDown = 3,
    CryptoDownGoldUp = 4,
    MarketCapDivergence = 5,
    DominanceDivergence = 6,
};

[[nodiscard]] constexpr std::string_view to_string(DivergenceType t) noexcept {
    switch (t) {
        case DivergenceType::None:               return "None";
        case DivergenceType::BtcUpEthDown:       return "BtcUpEthDown";
        case DivergenceType::BtcDownEthUp:       return "BtcDownEthUp";
        case DivergenceType::CryptoUpGoldDown:   return "CryptoUpGoldDown";
        case DivergenceType::CryptoDownGoldUp:   return "CryptoDownGoldUp";
        case DivergenceType::MarketCapDivergence:return "MarketCapDivergence";
        case DivergenceType::DominanceDivergence:return "DominanceDivergence";
    }
    return "Unknown";
}

struct Divergence {
    DivergenceType type{DivergenceType::None};
    double magnitude{0.0};       // 背离幅度
    Timestamp timestamp{};

    [[nodiscard]] bool is_valid() const noexcept {
        return type != DivergenceType::None &&
               timestamp.is_valid();
    }
};

// ==============================================================================
// 数据源接口（Provider）
// ==============================================================================
class CrossMarketProvider {
public:
    virtual ~CrossMarketProvider() = default;

    // 提供者名称
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // 获取信号（可返回部分字段）
    [[nodiscard]] virtual Result<CrossMarketSignals> fetch() = 0;

    // 是否可用（可选）
    [[nodiscard]] virtual bool is_available() const noexcept { return true; }

    // 优先级（数字越小越优先）
    [[nodiscard]] virtual int priority() const noexcept { return 100; }
};

// ==============================================================================
// 统计
// ==============================================================================
struct CrossMarketStats {
    alignas(64) std::atomic<std::uint64_t> fetch_attempts{0};
    alignas(64) std::atomic<std::uint64_t> fetch_successes{0};
    alignas(64) std::atomic<std::uint64_t> fetch_failures{0};
    alignas(64) std::atomic<std::uint64_t> cache_hits{0};
    alignas(64) std::atomic<std::uint64_t> stale_serves{0};
    alignas(64) std::atomic<std::uint64_t> invalid_responses{0};
    alignas(64) std::atomic<std::uint64_t> provider_fallbacks{0};

    alignas(64) std::atomic<std::uint64_t> divergence_detected{0};
    alignas(64) std::atomic<std::uint64_t> macro_events_active{0};

    alignas(64) std::atomic<std::int64_t> last_fetch_latency_us{0};
    alignas(64) std::atomic<std::int64_t> max_fetch_latency_us{0};
    alignas(64) std::atomic<std::int64_t> total_fetch_latency_us{0};

    void reset() noexcept {
        fetch_attempts.store(0, std::memory_order_relaxed);
        fetch_successes.store(0, std::memory_order_relaxed);
        fetch_failures.store(0, std::memory_order_relaxed);
        cache_hits.store(0, std::memory_order_relaxed);
        stale_serves.store(0, std::memory_order_relaxed);
        invalid_responses.store(0, std::memory_order_relaxed);
        provider_fallbacks.store(0, std::memory_order_relaxed);
        divergence_detected.store(0, std::memory_order_relaxed);
        macro_events_active.store(0, std::memory_order_relaxed);
        last_fetch_latency_us.store(0, std::memory_order_relaxed);
        max_fetch_latency_us.store(0, std::memory_order_relaxed);
        total_fetch_latency_us.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double success_rate() const noexcept {
        const auto total = fetch_attempts.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            fetch_successes.load(std::memory_order_relaxed)) / total;
    }

    [[nodiscard]] double avg_fetch_latency_ms() const noexcept {
        const auto total = fetch_attempts.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            total_fetch_latency_us.load(std::memory_order_relaxed))
            / total / 1000.0;
    }
};

// ==============================================================================
// 配置
// ==============================================================================
struct CrossMarketConfig {
    // TTL
    std::chrono::seconds cache_ttl{cross_market_config::kDefaultTtl};
    std::chrono::seconds stale_threshold{
        cross_market_config::kDefaultStaleThreshold};
    std::chrono::seconds refresh_interval{
        cross_market_config::kDefaultRefreshInterval};

    // 后台刷新
    bool enable_auto_refresh{true};

    // 降级
    bool serve_stale_on_failure{true};
    bool serve_cache_on_failure{true};

    // 校验
    bool validate_ranges{true};

    // 历史
    std::size_t history_size{cross_market_config::kDefaultHistorySize};
    std::size_t correlation_window{
        cross_market_config::kDefaultCorrelationWindow};

    // 背离检测
    bool enable_divergence_detection{true};
    double divergence_threshold_pct{0.005};   // 0.5%

    // 跨交易所
    bool enable_cross_exchange{false};

    // 宏观事件
    bool enable_macro_calendar{false};
    std::chrono::hours macro_lookahead{std::chrono::hours{24}};

    // 日志
    LogLevel log_level{LogLevel::INFO};
};

// ==============================================================================
// 跨市场数据管理器
// ==============================================================================
class CrossMarket {
public:
    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit CrossMarket(CrossMarketConfig config = {});
    ~CrossMarket();

    CrossMarket(const CrossMarket&) = delete;
    CrossMarket& operator=(const CrossMarket&) = delete;
    CrossMarket(CrossMarket&&) = delete;
    CrossMarket& operator=(CrossMarket&&) = delete;

    // -------------------------------------------------------------------------
    // Provider 注册
    // -------------------------------------------------------------------------
    Result<void> register_provider(
        std::shared_ptr<CrossMarketProvider> provider);

    void unregister_provider(std::string_view name);

    [[nodiscard]] std::vector<std::string> list_providers() const;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    Result<void> start();
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    // -------------------------------------------------------------------------
    // 数据获取
    // -------------------------------------------------------------------------
    // 同步获取（可能使用缓存）
    [[nodiscard]] Result<CrossMarketSignals> get_signals();

    // 强制刷新（忽略缓存）
    [[nodiscard]] Result<CrossMarketSignals> refresh();

    // 异步获取
    [[nodiscard]] std::future<Result<CrossMarketSignals>>
        get_signals_async();

    // 只读当前值（不触发请求）
    [[nodiscard]] std::optional<CrossMarketSignals>
        peek_signals() const;

    // -------------------------------------------------------------------------
    // 跨交易所价差
    // -------------------------------------------------------------------------
    Result<void> update_spread(const CrossExchangeSpread& spread);
    [[nodiscard]] std::optional<CrossExchangeSpread>
        get_spread(std::string_view symbol,
                    std::string_view exchange_a,
                    std::string_view exchange_b) const;
    [[nodiscard]] std::vector<CrossExchangeSpread>
        list_significant_spreads(double threshold_bps = 5.0) const;

    // -------------------------------------------------------------------------
    // 宏观事件
    // -------------------------------------------------------------------------
    Result<void> add_macro_event(const MacroEvent& event);
    [[nodiscard]] std::vector<MacroEvent>
        upcoming_events(std::chrono::hours lookahead) const;
    [[nodiscard]] bool is_high_impact_event_near(
        std::chrono::minutes window) const;

    // -------------------------------------------------------------------------
    // 背离检测
    // -------------------------------------------------------------------------
    [[nodiscard]] std::optional<Divergence>
        detect_divergence() const;

    // -------------------------------------------------------------------------
    // 相关性
    // -------------------------------------------------------------------------
    [[nodiscard]] double correlation(
        std::string_view series_a,
        std::string_view series_b) const;

    // -------------------------------------------------------------------------
    // 状态
    // -------------------------------------------------------------------------
    void reset();

    [[nodiscard]] const CrossMarketStats& stats() const noexcept;
    void reset_stats() noexcept;

    [[nodiscard]] const CrossMarketConfig& config() const noexcept;
    void set_config(const CrossMarketConfig& config);

    [[nodiscard]] DataQuality current_quality() const noexcept;

    [[nodiscard]] std::size_t history_size() const noexcept;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    // -------------------------------------------------------------------------
    // 内部
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<CrossMarketSignals> fetch_from_providers();

    [[nodiscard]] bool validate_signals(CrossMarketSignals& signals) const noexcept;

    void merge_signals(CrossMarketSignals& target,
                        const CrossMarketSignals& source) noexcept;

    void mark_quality(CrossMarketSignals& signals,
                       DataQuality quality) noexcept;

    void push_history(const CrossMarketSignals& signals);

    void refresh_loop();

    // 成员
    CrossMarketConfig config_;
    CrossMarketStats stats_;

    // 当前值
    mutable std::shared_mutex current_mutex_;
    std::optional<CrossMarketSignals> current_;
    Timestamp last_fetch_time_;

    // 历史缓冲
    mutable std::shared_mutex history_mutex_;
    std::deque<CrossMarketSignals> history_;

    // Providers
    mutable std::shared_mutex providers_mutex_;
    std::vector<std::shared_ptr<CrossMarketProvider>> providers_;

    // 跨交易所价差
    mutable std::shared_mutex spread_mutex_;
    std::vector<CrossExchangeSpread> spreads_;

    // 宏观事件
    mutable std::shared_mutex macro_mutex_;
    std::vector<MacroEvent> macro_events_;

    // 后台线程
    std::atomic<bool> running_{false};
    std::thread refresh_thread_;
    mutable std::mutex refresh_mutex_;
    std::condition_variable refresh_cv_;
};

// ==============================================================================
// 便捷工具
// ==============================================================================
[[nodiscard]] inline double safe_divide(
    double num, double den, double fallback = 0.0) noexcept
{
    if (!std::isfinite(num) || !std::isfinite(den)) return fallback;
    if (std::abs(den) < 1e-12) return fallback;
    const double r = num / den;
    return std::isfinite(r) ? r : fallback;
}

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_CORE_CROSS_MARKET_HPP
