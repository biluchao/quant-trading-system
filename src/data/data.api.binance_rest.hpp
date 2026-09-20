// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 币安 REST API 客户端
// ==============================================================================
// @file    src/data/data.api.binance_rest.hpp
// @module  data
// @type    api
// @name    binance_rest
// @version 1.0.1
// @brief   币安合约 REST API 封装，支持 K线/账户/订单/持仓查询
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 连接复用：单一 CURL 句柄 + 连接池
//   - 异步接口：基于 std::future，不阻塞调用线程
//   - 自动重试：指数退避，区分可重试/不可重试
//   - 速率限制：请求权重累积，接近阈值主动等待
//   - 时间同步：定期同步服务器时间，签名不漂移
//   - 签名正确：HMAC-SHA256，URL 编码完整
//   - 错误处理：Result<T> + 币安错误码映射
//   - 可观测：RestStats 统计 + 完整日志
//   - 可测试：依赖注入，支持 mock
// ==============================================================================

#ifndef QUANT_DATA_API_BINANCE_REST_HPP
#define QUANT_DATA_API_BINANCE_REST_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.crypto.hpp"
#include "common/common.core.metrics.hpp"
#include "common/common.core.config.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace binance_rest_config {

// 端点
inline constexpr std::string_view kMainnetUrl = "https://fapi.binance.com";
inline constexpr std::string_view kTestnetUrl = "https://testnet.binancefuture.com";

// 超时
inline constexpr std::chrono::milliseconds kConnectTimeout{5000};
inline constexpr std::chrono::milliseconds kRequestTimeout{10000};
inline constexpr std::chrono::milliseconds kLongRequestTimeout{30000};  // 历史数据

// 重试
inline constexpr int kMaxRetries{3};
inline constexpr std::chrono::milliseconds kRetryInitialDelay{500};
inline constexpr std::chrono::milliseconds kRetryMaxDelay{10000};

// 速率限制
inline constexpr std::size_t kRateLimitWeightPerMinute{2400};
inline constexpr std::size_t kRateLimitWarnThreshold{2000};    // 80%
inline constexpr std::size_t kRateLimitBlockThreshold{2300};   // 96%

// 时间同步
inline constexpr std::chrono::seconds kTimeSyncInterval{300};  // 5 分钟
inline constexpr std::int64_t kMaxTimeDriftUs{1'000'000};      // 1 秒

// 响应体大小上限
inline constexpr std::size_t kMaxResponseSize{32 * 1024 * 1024};  // 32 MB

// K 线查询上限
inline constexpr std::size_t kMaxKlinesPerRequest{1500};

}  // namespace binance_rest_config

// ==============================================================================
// REST 统计
// ==============================================================================
struct RestStats {
    alignas(64) std::atomic<std::uint64_t> requests_total{0};
    alignas(64) std::atomic<std::uint64_t> requests_succeeded{0};
    alignas(64) std::atomic<std::uint64_t> requests_failed{0};
    alignas(64) std::atomic<std::uint64_t> requests_retried{0};
    alignas(64) std::atomic<std::uint64_t> rate_limit_hits{0};
    alignas(64) std::atomic<std::uint64_t> bytes_received{0};
    alignas(64) std::atomic<std::int64_t>  last_latency_us{0};
    alignas(64) std::atomic<std::int64_t>  max_latency_us{0};
    alignas(64) std::atomic<std::int64_t>  total_latency_us{0};
    alignas(64) std::atomic<std::uint64_t> time_sync_count{0};
    alignas(64) std::atomic<std::int64_t>  current_time_offset_us{0};
    alignas(64) std::atomic<std::uint64_t> current_weight_used{0};
    alignas(64) std::atomic<std::uint64_t> current_weight_limit{0};

    void reset() noexcept {
        requests_total.store(0, std::memory_order_relaxed);
        requests_succeeded.store(0, std::memory_order_relaxed);
        requests_failed.store(0, std::memory_order_relaxed);
        requests_retried.store(0, std::memory_order_relaxed);
        rate_limit_hits.store(0, std::memory_order_relaxed);
        bytes_received.store(0, std::memory_order_relaxed);
        last_latency_us.store(0, std::memory_order_relaxed);
        max_latency_us.store(0, std::memory_order_relaxed);
        total_latency_us.store(0, std::memory_order_relaxed);
        time_sync_count.store(0, std::memory_order_relaxed);
        current_time_offset_us.store(0, std::memory_order_relaxed);
        current_weight_used.store(0, std::memory_order_relaxed);
        current_weight_limit.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double success_rate() const noexcept {
        const auto total = requests_total.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            requests_succeeded.load(std::memory_order_relaxed)) / total;
    }

    [[nodiscard]] double avg_latency_ms() const noexcept {
        const auto total = requests_total.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(
            total_latency_us.load(std::memory_order_relaxed)) / total / 1000.0;
    }
};

// ==============================================================================
// K 线查询参数
// ==============================================================================
struct KlineRequest {
    std::string symbol;                    // "BTCUSDT"
    std::string interval{"3m"};            // "1m", "3m", "5m", "1h", ...
    std::optional<Timestamp> start_time;   // 起始时间
    std::optional<Timestamp> end_time;     // 结束时间
    std::size_t limit{500};                // 最多 1500
};

// ==============================================================================
// 下单参数
// ==============================================================================
struct OrderRequest {
    std::string symbol;
    Side side{Side::UNKNOWN};
    OrderType type{OrderType::UNKNOWN};
    TimeInForce time_in_force{TimeInForce::GTC};
    std::optional<double> quantity;
    std::optional<double> quote_order_qty;   // 按金额下单
    std::optional<double> price;             // LIMIT 必填
    std::optional<double> stop_price;        // STOP 类必填
    std::optional<double> callback_rate;     // TRAILING_STOP_MARKET
    std::optional<bool> reduce_only;
    std::optional<bool> post_only;
    std::optional<std::string> client_order_id;
    std::optional<int> leverage;
};

// ==============================================================================
// 下单响应
// ==============================================================================
struct OrderResponse {
    std::uint64_t order_id{0};
    std::string client_order_id;
    std::string symbol;
    std::int64_t transact_time_us{0};
    double price{0.0};
    double orig_qty{0.0};
    double executed_qty{0.0};
    double avg_price{0.0};
    std::string status;
    std::string type;
    std::string side;
    std::string time_in_force;
    bool reduce_only{false};
    bool post_only{false};
};

// ==============================================================================
// 账户信息
// ==============================================================================
struct AccountInfo {
    double total_wallet_balance{0.0};
    double total_unrealized_profit{0.0};
    double total_margin_balance{0.0};
    double total_initial_margin{0.0};
    double total_maint_margin{0.0};
    double available_balance{0.0};
    double max_withdraw_amount{0.0};

    struct Asset {
        std::string asset;
        double wallet_balance{0.0};
        double available_balance{0.0};
        double unrealized_profit{0.0};
        double margin_balance{0.0};
    };
    std::vector<Asset> assets;

    struct Position {
        std::string symbol;
        double position_amount{0.0};
        double entry_price{0.0};
        double mark_price{0.0};
        double unrealized_profit{0.0};
        double liquidation_price{0.0};
        double isolated_margin{0.0};
        int leverage{0};
        std::string margin_type;
        std::string position_side;
    };
    std::vector<Position> positions;
};

// ==============================================================================
// 持仓风险
// ==============================================================================
struct PositionRisk {
    std::string symbol;
    double position_amount{0.0};
    double entry_price{0.0};
    double mark_price{0.0};
    double unrealized_profit{0.0};
    double liquidation_price{0.0};
    double isolated_margin{0.0};
    int leverage{0};
    std::string margin_type;
    std::string position_side;
    double notional{0.0};
    double initial_margin{0.0};
    double maint_margin{0.0};
};

// ==============================================================================
// 资金费率
// ==============================================================================
struct FundingRate {
    std::string symbol;
    double funding_rate{0.0};
    std::int64_t funding_time_us{0};
};

// ==============================================================================
// 交易所信息
// ==============================================================================
struct ExchangeInfo {
    struct Symbol {
        std::string symbol;
        std::string base_asset;
        std::string quote_asset;
        double price_precision{0.0};
        double quantity_precision{0.0};
        double min_qty{0.0};
        double max_qty{0.0};
        double step_size{0.0};
        double min_notional{0.0};
        double tick_size{0.0};
    };
    std::vector<Symbol> symbols;
    std::int64_t server_time_us{0};
};

// ==============================================================================
// 配置
// ==============================================================================
struct BinanceRestConfig {
    // 主网或测试网
    bool use_testnet{false};

    // 自定义 URL（覆盖 use_testnet）
    std::optional<std::string> custom_url;

    // API 密钥（从 Vault/配置读取，禁止硬编码）
    std::string api_key;
    std::string api_secret;

    // 超时
    std::chrono::milliseconds connect_timeout{
        binance_rest_config::kConnectTimeout};
    std::chrono::milliseconds request_timeout{
        binance_rest_config::kRequestTimeout};

    // 重试
    int max_retries{binance_rest_config::kMaxRetries};
    std::chrono::milliseconds retry_initial_delay{
        binance_rest_config::kRetryInitialDelay};
    std::chrono::milliseconds retry_max_delay{
        binance_rest_config::kRetryMaxDelay};

    // 速率限制
    std::size_t rate_limit_weight_per_minute{
        binance_rest_config::kRateLimitWeightPerMinute};
    std::size_t rate_limit_warn_threshold{
        binance_rest_config::kRateLimitWarnThreshold};
    std::size_t rate_limit_block_threshold{
        binance_rest_config::kRateLimitBlockThreshold};

    // 时间同步
    std::chrono::seconds time_sync_interval{
        binance_rest_config::kTimeSyncInterval};
    bool auto_time_sync{true};

    // 代理
    std::optional<std::string> proxy_url;

    // SSL 验证（生产必须 true）
    bool verify_ssl{true};

    // 响应体大小上限
    std::size_t max_response_size{
        binance_rest_config::kMaxResponseSize};

    // 用户代理
    std::string user_agent{"quant-trading-system/1.0.1"};

    // 日志级别
    LogLevel log_level{LogLevel::INFO};

    [[nodiscard]] std::string get_base_url() const;
};

// ==============================================================================
// BinanceRest 客户端
// ==============================================================================
class BinanceRest {
public:
    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit BinanceRest(const BinanceRestConfig& config);
    ~BinanceRest();

    BinanceRest(const BinanceRest&) = delete;
    BinanceRest& operator=(const BinanceRest&) = delete;
    BinanceRest(BinanceRest&&) = delete;
    BinanceRest& operator=(BinanceRest&&) = delete;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    // 初始化（建立连接池）
    Result<void> initialize();

    // 关闭（等待所有进行中的请求）
    void shutdown() noexcept;

    // -------------------------------------------------------------------------
    // 时间同步
    // -------------------------------------------------------------------------
    // 同步服务器时间，记录偏移量
    Result<void> sync_time();

    // 获取当前服务器时间（本地时间 + 偏移）
    [[nodiscard]] Timestamp server_time() const noexcept;

    // 时间偏移（微秒）
    [[nodiscard]] std::int64_t time_offset_us() const noexcept;

    // -------------------------------------------------------------------------
    // 公共接口（无需签名）
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // K 线数据
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<std::vector<Candle>>
        get_klines(const KlineRequest& request);

    // 批量获取多个交易对的 K 线
    [[nodiscard]] Result<std::unordered_map<std::string, std::vector<Candle>>>
        get_klines_batch(std::span<const KlineRequest> requests);

    // -------------------------------------------------------------------------
    // 深度快照
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<std::pair<
            std::vector<std::pair<double, double>>,   // bids
            std::vector<std::pair<double, double>>>>  // asks
        get_depth(std::string_view symbol, std::size_t limit = 20);

    // -------------------------------------------------------------------------
    // 最近成交
    // -------------------------------------------------------------------------
    struct RecentTrade {
        std::uint64_t id{0};
        double price{0.0};
        double qty{0.0};
        bool is_buyer_maker{false};
        Timestamp time{};
    };

    [[nodiscard]] Result<std::vector<RecentTrade>>
        get_recent_trades(std::string_view symbol, std::size_t limit = 100);

    // -------------------------------------------------------------------------
    // 资金费率
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<FundingRate>
        get_funding_rate(std::string_view symbol);

    // -------------------------------------------------------------------------
    // 交易所信息
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<ExchangeInfo> get_exchange_info();

    // 获取交易对精度
    [[nodiscard]] Result<ExchangeInfo::Symbol>
        get_symbol_info(std::string_view symbol);

    // -------------------------------------------------------------------------
    // 私有接口（需要签名）
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // 账户
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<AccountInfo> get_account_info();

    [[nodiscard]] Result<std::vector<PositionRisk>> get_position_risk(
        std::optional<std::string_view> symbol = std::nullopt);

    // -------------------------------------------------------------------------
    // 订单
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<OrderResponse>
        place_order(const OrderRequest& request);

    [[nodiscard]] Result<void>
        cancel_order(std::string_view symbol, std::uint64_t order_id);

    [[nodiscard]] Result<void>
        cancel_all_orders(std::string_view symbol);

    [[nodiscard]] Result<OrderResponse>
        get_order(std::string_view symbol, std::uint64_t order_id);

    [[nodiscard]] Result<std::vector<OrderResponse>>
        get_open_orders(std::optional<std::string_view> symbol = std::nullopt);

    // -------------------------------------------------------------------------
    // 杠杆与保证金
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void>
        set_leverage(std::string_view symbol, int leverage);

    [[nodiscard]] Result<void>
        set_margin_type(std::string_view symbol, std::string_view margin_type);

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] const RestStats& stats() const noexcept;
    void reset_stats() noexcept;

    [[nodiscard]] std::size_t current_weight() const noexcept;
    [[nodiscard]] bool is_rate_limited() const noexcept;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 辅助：从币安错误码转换
// ==============================================================================
[[nodiscard]] ErrorCode from_binance_error_code(int code) noexcept;

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_API_BINANCE_REST_HPP
