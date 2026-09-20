// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 币安 WebSocket 接入
// ==============================================================================
// @file    src/data/data.api.binance_ws.hpp
// @module  data
// @type    api
// @name    binance_ws
// @version 1.0.1
// @brief   币安合约 WebSocket 客户端，支持 K 线/深度/订单流实时接入
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 异步 I/O：Boost.Asio + Beast，独立 I/O 线程
//   - 自动重连：状态机 + 指数退避 + 抖动
//   - 心跳保活：3 分钟 ping/pong 监控
//   - 数据完整：区分未收盘/已收盘 K 线，缺口自动补齐
//   - 故障隔离：单条坏消息不影响流
//   - 背压控制：有界队列，超限丢弃并告警
//   - 回调分离：I/O 线程入队，工作线程处理
//   - 状态通知：连接/断连/错误/重连事件回调
//   - 指标暴露：消息速率、延迟、断连次数
// ==============================================================================

#ifndef QUANT_DATA_API_BINANCE_WS_HPP
#define QUANT_DATA_API_BINANCE_WS_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.types.hpp"
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"
#include "common/common.core.lockfree_queue.hpp"
#include "common/common.core.metrics.hpp"

namespace quant {
namespace data {

// ==============================================================================
// 常量
// ==============================================================================
namespace binance_ws_config {

// 币安 WebSocket 端点
inline constexpr std::string_view kMainnetWsUrl = "wss://fstream.binance.com/stream";
inline constexpr std::string_view kTestnetWsUrl = "wss://stream.binancefuture.com/stream";

// 单连接最多订阅流数
inline constexpr std::size_t kMaxStreamsPerConnection = 1024;

// 心跳间隔（币安要求 3 分钟内必须有心跳）
inline constexpr std::chrono::seconds kPingInterval{30};
inline constexpr std::chrono::seconds kPongTimeout{10};

// 重连退避
inline constexpr std::chrono::seconds kReconnectMinDelay{1};
inline constexpr std::chrono::seconds kReconnectMaxDelay{60};
inline constexpr std::chrono::seconds kReconnectJitterMax{2};

// 连接超时
inline constexpr std::chrono::seconds kConnectTimeout{10};
inline constexpr std::chrono::seconds kTlsHandshakeTimeout{10};
inline constexpr std::chrono::seconds kSubscribeTimeout{5};

// 消息队列
inline constexpr std::size_t kMessageQueueCapacity = 16384;

// 消息延迟阈值（超过则告警）
inline constexpr std::chrono::milliseconds kMessageLatencyWarnThreshold{2000};

// 消息速率告警阈值（每秒消息数）
inline constexpr std::size_t kMessageRateWarnThreshold{10000};

}  // namespace binance_ws_config

// ==============================================================================
// 连接状态
// ==============================================================================
enum class WsConnectionState : std::uint8_t {
    Disconnected = 0,
    Connecting   = 1,
    TlsHandshake = 2,
    Connected    = 3,
    Subscribing  = 4,
    Ready        = 5,   // 已连接且订阅完成
    Reconnecting = 6,
    Closing      = 7,
    Failed       = 8,
};

[[nodiscard]] constexpr std::string_view to_string(WsConnectionState s) noexcept {
    switch (s) {
        case WsConnectionState::Disconnected: return "Disconnected";
        case WsConnectionState::Connecting:   return "Connecting";
        case WsConnectionState::TlsHandshake: return "TlsHandshake";
        case WsConnectionState::Connected:    return "Connected";
        case WsConnectionState::Subscribing:  return "Subscribing";
        case WsConnectionState::Ready:        return "Ready";
        case WsConnectionState::Reconnecting: return "Reconnecting";
        case WsConnectionState::Closing:      return "Closing";
        case WsConnectionState::Failed:       return "Failed";
    }
    return "Unknown";
}

// ==============================================================================
// WebSocket 统计
// ==============================================================================
struct WebSocketStats {
    alignas(64) std::atomic<std::uint64_t> messages_received{0};
    alignas(64) std::atomic<std::uint64_t> messages_dropped{0};
    alignas(64) std::atomic<std::uint64_t> messages_invalid{0};
    alignas(64) std::atomic<std::uint64_t> connect_count{0};
    alignas(64) std::atomic<std::uint64_t> disconnect_count{0};
    alignas(64) std::atomic<std::uint64_t> reconnect_count{0};
    alignas(64) std::atomic<std::uint64_t> pong_timeout_count{0};
    alignas(64) std::atomic<std::uint64_t> subscribe_failure_count{0};
    alignas(64) std::atomic<std::uint64_t> gap_filled_count{0};
    alignas(64) std::atomic<std::uint64_t> bytes_received{0};
    alignas(64) std::atomic<std::int64_t>  last_message_latency_us{0};
    alignas(64) std::atomic<std::int64_t>  max_message_latency_us{0};
    alignas(64) std::atomic<std::uint64_t> queue_size{0};
    alignas(64) std::atomic<std::uint64_t> queue_peak{0};

    void reset() noexcept {
        messages_received.store(0, std::memory_order_relaxed);
        messages_dropped.store(0, std::memory_order_relaxed);
        messages_invalid.store(0, std::memory_order_relaxed);
        connect_count.store(0, std::memory_order_relaxed);
        disconnect_count.store(0, std::memory_order_relaxed);
        reconnect_count.store(0, std::memory_order_relaxed);
        pong_timeout_count.store(0, std::memory_order_relaxed);
        subscribe_failure_count.store(0, std::memory_order_relaxed);
        gap_filled_count.store(0, std::memory_order_relaxed);
        bytes_received.store(0, std::memory_order_relaxed);
        last_message_latency_us.store(0, std::memory_order_relaxed);
        max_message_latency_us.store(0, std::memory_order_relaxed);
        queue_size.store(0, std::memory_order_relaxed);
        queue_peak.store(0, std::memory_order_relaxed);
    }
};

// ==============================================================================
// 回调接口
// ==============================================================================
struct WebSocketCallbacks {
    // -------------------------------------------------------------------------
    // 连接生命周期
    // -------------------------------------------------------------------------
    // 连接建立（TCP + TLS）
    std::function<void()> on_connected;

    // 连接断开
    std::function<void(std::string_view reason)> on_disconnected;

    // 完全就绪（已连接且订阅完成）
    std::function<void()> on_ready;

    // 错误发生
    std::function<void(const ErrorInfo& error)> on_error;

    // -------------------------------------------------------------------------
    // 数据回调
    // -------------------------------------------------------------------------
    // K 线更新（未收盘，每秒推送）
    std::function<void(const Candle& candle)> on_kline_update;

    // K 线收盘（已收盘，每 3 分钟推送一次）
    std::function<void(const Candle& candle)> on_kline_closed;

    // 深度快照（深度更新）
    std::function<void(const std::vector<std::pair<double, double>>& bids,
                       const std::vector<std::pair<double, double>>& asks)>
        on_depth_update;

    // 逐笔成交
    std::function<void(std::string_view symbol, double price,
                       double quantity, bool is_buyer_maker,
                       Timestamp time)> on_trade;

    // 标记价格
    std::function<void(std::string_view symbol, double mark_price)> on_mark_price;

    // 资金费率
    std::function<void(std::string_view symbol, double funding_rate,
                       Timestamp next_funding_time)> on_funding_rate;

    // -------------------------------------------------------------------------
    // 数据完整性
    // -------------------------------------------------------------------------
    // 发现 K 线缺口
    std::function<void(std::string_view symbol,
                       Timestamp gap_start,
                       Timestamp gap_end)> on_gap_detected;

    // 缺口已补齐
    std::function<void(std::string_view symbol,
                       std::size_t filled_count)> on_gap_filled;

    // -------------------------------------------------------------------------
    // 交易所系统消息
    // -------------------------------------------------------------------------
    // 维护公告
    std::function<void(std::string_view message,
                       Timestamp effective_time)> on_maintenance;

    // 订阅确认
    std::function<void(std::string_view stream, bool success,
                       std::string_view error_msg)> on_subscription_result;
};

// ==============================================================================
// 配置
// ==============================================================================
struct BinanceWSConfig {
    // 主网或测试网
    bool use_testnet{false};

    // 自定义 URL（覆盖 use_testnet）
    std::optional<std::string> custom_url;

    // API 密钥（私有流需要）
    std::string api_key;
    std::string api_secret;

    // 连接选项
    std::chrono::seconds connect_timeout{
        binance_ws_config::kConnectTimeout};
    std::chrono::seconds ping_interval{
        binance_ws_config::kPingInterval};
    std::chrono::seconds pong_timeout{
        binance_ws_config::kPongTimeout};

    // 重连
    bool auto_reconnect{true};
    std::chrono::seconds reconnect_min_delay{
        binance_ws_config::kReconnectMinDelay};
    std::chrono::seconds reconnect_max_delay{
        binance_ws_config::kReconnectMaxDelay};

    // 队列
    std::size_t message_queue_capacity{
        binance_ws_config::kMessageQueueCapacity};

    // 工作线程数（处理回调）
    std::size_t worker_threads{2};

    // 缺口自动补齐
    bool auto_fill_gap{true};

    // 消息延迟告警阈值
    std::chrono::milliseconds latency_warn_threshold{
        binance_ws_config::kMessageLatencyWarnThreshold};

    // 是否校验 TLS 证书（生产环境必须 true）
    bool verify_tls{true};

    // 是否启用私有流（需要 API 密钥）
    bool enable_private_stream{false};

    // 日志级别
    LogLevel log_level{LogLevel::INFO};

    // 初始化方法
    [[nodiscard]] std::string get_url() const;
};

// ==============================================================================
// 订阅请求
// ==============================================================================
struct SubscriptionRequest {
    std::string symbol;                   // 如 "BTCUSDT"
    std::string channel;                  // 如 "kline_3m"、"depth20"、"aggTrade"
    std::optional<std::size_t> update_speed_ms;  // 深度更新速度
};

// ==============================================================================
// BinanceWS 客户端
// ==============================================================================
class BinanceWS {
public:
    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit BinanceWS(const BinanceWSConfig& config);
    ~BinanceWS();

    BinanceWS(const BinanceWS&) = delete;
    BinanceWS& operator=(const BinanceWS&) = delete;
    BinanceWS(BinanceWS&&) = delete;
    BinanceWS& operator=(BinanceWS&&) = delete;

    // -------------------------------------------------------------------------
    // 回调注册（必须在 start 之前调用）
    // -------------------------------------------------------------------------
    void set_callbacks(WebSocketCallbacks callbacks);

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------
    // 启动客户端（异步，立即返回）
    // 连接成功、订阅完成后触发 on_ready
    Result<void> start();

    // 优雅关闭（等待所有回调完成）
    void stop() noexcept;

    // -------------------------------------------------------------------------
    // 订阅
    // -------------------------------------------------------------------------
    // 订阅单个流
    Result<void> subscribe(const SubscriptionRequest& request);

    // 批量订阅
    Result<void> subscribe(std::span<const SubscriptionRequest> requests);

    // 取消订阅
    Result<void> unsubscribe(std::string_view symbol, std::string_view channel);

    // 取消所有订阅
    Result<void> unsubscribe_all();

    // 已订阅的流数
    [[nodiscard]] std::size_t subscription_count() const noexcept;

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    [[nodiscard]] WsConnectionState state() const noexcept;
    [[nodiscard]] bool is_ready() const noexcept;
    [[nodiscard]] const WebSocketStats& stats() const noexcept;
    void reset_stats() noexcept;

    // 当前连接 ID（每次重连递增）
    [[nodiscard]] std::uint64_t connection_id() const noexcept;

    // -------------------------------------------------------------------------
    // 手动控制
    // -------------------------------------------------------------------------
    // 强制重连（用于调试或网络恢复后）
    void force_reconnect() noexcept;

    // 暂停消息处理（回调不再触发，但连接保持）
    void pause_message_processing() noexcept;

    // 恢复消息处理
    void resume_message_processing() noexcept;

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ==============================================================================
// 便捷函数
// ==============================================================================
namespace binance_ws_helper {

// 构建 K 线流名称
[[nodiscard]] inline std::string kline_stream(
    std::string_view symbol, std::string_view interval = "3m")
{
    return std::string{symbol} + "@kline_" + std::string{interval};
}

// 构建深度流名称
[[nodiscard]] inline std::string depth_stream(
    std::string_view symbol, std::size_t levels = 20,
    std::size_t update_ms = 100)
{
    return std::string{symbol} + "@depth" + std::to_string(levels) +
           "@" + std::to_string(update_ms) + "ms";
}

// 构建逐笔成交流名称
[[nodiscard]] inline std::string agg_trade_stream(std::string_view symbol) {
    return std::string{symbol} + "@aggTrade";
}

// 构建标记价格流名称
[[nodiscard]] inline std::string mark_price_stream(std::string_view symbol) {
    return std::string{symbol} + "@markPrice@1s";
}

// 构建资金费率流名称
[[nodiscard]] inline std::string funding_rate_stream(std::string_view symbol) {
    return std::string{symbol} + "@fundingRate";
}

}  // namespace binance_ws_helper

}  // namespace data
}  // namespace quant

#endif  // QUANT_DATA_API_BINANCE_WS_HPP
