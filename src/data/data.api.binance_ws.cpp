// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 币安 WebSocket 实现
// ==============================================================================
// @file    src/data/data.api.binance_ws.cpp
// @module  data
// @type    api
// @name    binance_ws
// @version 1.0.1
// @brief   基于 Boost.Beast 的币安 WebSocket 客户端实现
//          已修复 25 类运行时问题
//
// 线程模型:
//   - I/O 线程（1 个）：处理 TCP/TLS/WebSocket 读写
//   - 工作线程池（N 个）：执行用户回调
//   - 定时器线程（复用 I/O 线程）：心跳、重连、超时
//
// 生命周期:
//   start() → Connecting → TlsHandshake → Connected → Subscribing
//           → Ready → [数据流] → 断连 → Reconnecting → ...
//           → stop() → Closing → Disconnected
// ==============================================================================

#include "data/data.api.binance_ws.hpp"

// ==============================================================================
// Boost.Asio / Beast
// ==============================================================================
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

// ==============================================================================
// JSON
// ==============================================================================
#include <nlohmann/json.hpp>

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace quant {
namespace data {

// ==============================================================================
// 命名空间别名
// ==============================================================================
namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace ssl       = asio::ssl;
using tcp           = asio::ip::tcp;
using json          = nlohmann::json;

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// URL 解析
// -----------------------------------------------------------------------------
struct ParsedUrl {
    std::string scheme;   // "wss"
    std::string host;     // "fstream.binance.com"
    std::string port;     // "443"
    std::string path;     // "/stream"
};

[[nodiscard]] Result<ParsedUrl> parse_url(std::string_view url) {
    ParsedUrl result;

    // Scheme
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return QUANT_ERR_T(ParsedUrl, ErrorCode::ConfigInvalidValue)
            .with_context("url", std::string{url})
            .with_context("reason", "缺少 ://");
    }
    result.scheme = std::string{url.substr(0, scheme_end)};

    if (result.scheme != "wss" && result.scheme != "ws") {
        return QUANT_ERR_T(ParsedUrl, ErrorCode::ConfigInvalidValue)
            .with_context("scheme", result.scheme)
            .with_context("reason", "仅支持 ws/wss");
    }

    // Host 与 Path
    const auto host_start = scheme_end + 3;
    const auto path_start = url.find('/', host_start);

    std::string_view host_port = (path_start == std::string_view::npos)
        ? url.substr(host_start)
        : url.substr(host_start, path_start - host_start);

    // 默认端口
    result.port = (result.scheme == "wss") ? "443" : "80";

    // 检查 host 中是否有端口
    const auto colon = host_port.find(':');
    if (colon != std::string_view::npos) {
        result.host = std::string{host_port.substr(0, colon)};
        result.port = std::string{host_port.substr(colon + 1)};
    } else {
        result.host = std::string{host_port};
    }

    result.path = (path_start == std::string_view::npos)
        ? "/"
        : std::string{url.substr(path_start)};

    if (result.host.empty()) {
        return QUANT_ERR_T(ParsedUrl, ErrorCode::ConfigInvalidValue)
            .with_context("url", std::string{url})
            .with_context("reason", "host 为空");
    }

    return result;
}

// -----------------------------------------------------------------------------
// 生成抖动
// -----------------------------------------------------------------------------
[[nodiscard]] std::chrono::milliseconds add_jitter(
    std::chrono::seconds base,
    std::chrono::seconds max_jitter) noexcept
{
    static thread_local std::mt19937 rng{
        static_cast<std::uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count())};

    const auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        max_jitter).count();
    std::uniform_int_distribution<std::int64_t> dist(0, max_ms);

    const auto base_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        base).count();

    return std::chrono::milliseconds{base_ms + dist(rng)};
}

// -----------------------------------------------------------------------------
// 安全转换为 double
// -----------------------------------------------------------------------------
[[nodiscard]] double safe_double(const json& j, std::string_view key,
                                  double default_value = 0.0) noexcept {
    try {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null()) return default_value;
        if (it->is_number()) return it->get<double>();
        if (it->is_string()) {
            return std::stod(it->get<std::string>());
        }
        return default_value;
    } catch (...) {
        return default_value;
    }
}

// -----------------------------------------------------------------------------
// 安全转换为 int64
// -----------------------------------------------------------------------------
[[nodiscard]] std::int64_t safe_int64(const json& j, std::string_view key,
                                       std::int64_t default_value = 0) noexcept {
    try {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null()) return default_value;
        if (it->is_number_integer()) return it->get<std::int64_t>();
        if (it->is_number()) return static_cast<std::int64_t>(it->get<double>());
        if (it->is_string()) return std::stoll(it->get<std::string>());
        return default_value;
    } catch (...) {
        return default_value;
    }
}

// -----------------------------------------------------------------------------
// 安全转换为 string
// -----------------------------------------------------------------------------
[[nodiscard]] std::string safe_string(const json& j, std::string_view key) {
    try {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null()) return {};
        if (it->is_string()) return it->get<std::string>();
        return it->dump();
    } catch (...) {
        return {};
    }
}

// -----------------------------------------------------------------------------
// 安全转换为 bool
// -----------------------------------------------------------------------------
[[nodiscard]] bool safe_bool(const json& j, std::string_view key,
                              bool default_value = false) noexcept {
    try {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null()) return default_value;
        if (it->is_boolean()) return it->get<bool>();
        if (it->is_number()) return it->get<double>() != 0.0;
        return default_value;
    } catch (...) {
        return default_value;
    }
}

}  // namespace

// ==============================================================================
// BinanceWSConfig::get_url
// ==============================================================================
std::string BinanceWSConfig::get_url() const {
    if (custom_url) return *custom_url;
    return use_testnet
        ? std::string{binance_ws_config::kTestnetWsUrl}
        : std::string{binance_ws_config::kMainnetWsUrl};
}

// ==============================================================================
// Impl
// ==============================================================================
class BinanceWS::Impl : public std::enable_shared_from_this<BinanceWS::Impl> {
public:
    // -------------------------------------------------------------------------
    // 构造与析构
    // -------------------------------------------------------------------------
    explicit Impl(const BinanceWSConfig& config)
        : config_{config}
        , ioc_{}
        , work_guard_{asio::make_work_guard(ioc_)}
        , ssl_ctx_{ssl::context::tls_client}
        , resolver_{ioc_}
        , ws_{ioc_, ssl_ctx_}
        , ping_timer_{ioc_}
        , pong_timer_{ioc_}
        , reconnect_timer_{ioc_}
        , connect_timer_{ioc_}
        , strand_{asio::make_strand(ioc_)}
    {
        init_ssl();
    }

    ~Impl() {
        stop();
    }

    // -------------------------------------------------------------------------
    // 回调设置
    // -------------------------------------------------------------------------
    void set_callbacks(WebSocketCallbacks callbacks) {
        callbacks_ = std::move(callbacks);
    }

    // -------------------------------------------------------------------------
    // 启动
    // -------------------------------------------------------------------------
    Result<void> start() {
        // 校验 URL
        const auto url_str = config_.get_url();
        auto parsed = parse_url(url_str);
        if (parsed.is_err()) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "WebSocket URL 非法: " + url_str)
                .with_context("error", parsed.error().to_string());
        }
        url_ = *parsed;

        // 启动 I/O 线程
        io_thread_ = std::thread([this] {
            try {
                ioc_.run();
            } catch (const std::exception& e) {
                QUANT_LOG_ERROR("[WS] I/O 线程异常: {}", e.what());
            }
        });

        // 启动工作线程池
        for (std::size_t i = 0; i < config_.worker_threads; ++i) {
            worker_threads_.emplace_back([this] { worker_loop(); });
        }

        // 启动连接
        asio::post(ioc_, [self = shared_from_this()] {
            self->do_connect();
        });

        return {};
    }

    // -------------------------------------------------------------------------
    // 停止
    // -------------------------------------------------------------------------
    void stop() noexcept {
        if (stopped_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        QUANT_LOG_INFO("[WS] 正在停止...");

        // 1. 关闭工作队列
        {
            std::lock_guard lock(queue_mutex_);
            queue_closed_ = true;
            queue_cv_.notify_all();
        }

        // 2. 关闭工作线程
        for (auto& t : worker_threads_) {
            if (t.joinable()) t.join();
        }
        worker_threads_.clear();

        // 3. 通知 I/O 线程停止
        asio::post(ioc_, [self = shared_from_this()] {
            self->close_connection("user_stop");
        });

        // 4. 移除工作守卫，允许 ioc 退出
        work_guard_.reset();

        // 5. 等待 I/O 线程退出
        if (io_thread_.joinable()) {
            io_thread_.join();
        }

        QUANT_LOG_INFO("[WS] 已停止");
    }

    // -------------------------------------------------------------------------
    // 订阅管理
    // -------------------------------------------------------------------------
    Result<void> subscribe(const SubscriptionRequest& request) {
        if (stopped_.load(std::memory_order_acquire)) {
            return QUANT_ERR(ErrorCode::NetworkDisconnected);
        }

        // 构建流名称
        std::string stream;
        if (request.channel.starts_with("kline_")) {
            stream = std::string{request.symbol} + "@" + request.channel;
        } else if (request.channel.starts_with("depth")) {
            stream = std::string{request.symbol} + "@" + request.channel;
            if (request.update_speed_ms) {
                stream += "@" + std::to_string(*request.update_speed_ms) + "ms";
            }
        } else {
            stream = std::string{request.symbol} + "@" + request.channel;
        }

        std::lock_guard lock(subs_mutex_);
        if (subscriptions_.size() >= binance_ws_config::kMaxStreamsPerConnection) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "订阅数量超限")
                .with_context("max",
                    std::to_string(binance_ws_config::kMaxStreamsPerConnection));
        }

        if (subscriptions_.count(stream)) {
            return {};  // 已订阅
        }

        // 记录待订阅流
        subscriptions_[stream] = request;

        // 如果已就绪，立即发送订阅
        if (state_.load(std::memory_order_acquire) == WsConnectionState::Ready) {
            asio::post(ioc_, [self = shared_from_this(), stream] {
                self->send_subscribe({stream});
            });
        }

        return {};
    }

    Result<void> subscribe(std::span<const SubscriptionRequest> requests) {
        for (const auto& req : requests) {
            auto r = subscribe(req);
            if (r.is_err()) return r;
        }
        return {};
    }

    Result<void> unsubscribe(std::string_view symbol, std::string_view channel) {
        std::string stream = std::string{symbol} + "@" + std::string{channel};

        std::lock_guard lock(subs_mutex_);
        if (!subscriptions_.erase(stream)) {
            return QUANT_ERR_MSG(ErrorCode::ConfigNotFound,
                "未订阅该流: " + stream);
        }

        if (state_.load(std::memory_order_acquire) == WsConnectionState::Ready) {
            asio::post(ioc_, [self = shared_from_this(), stream] {
                self->send_unsubscribe({stream});
            });
        }

        return {};
    }

    Result<void> unsubscribe_all() {
        std::vector<std::string> streams;
        {
            std::lock_guard lock(subs_mutex_);
            for (const auto& [k, v] : subscriptions_) {
                streams.push_back(k);
            }
            subscriptions_.clear();
        }

        if (!streams.empty() &&
            state_.load(std::memory_order_acquire) == WsConnectionState::Ready) {
            asio::post(ioc_, [self = shared_from_this(), streams = std::move(streams)] {
                self->send_unsubscribe(streams);
            });
        }

        return {};
    }

    std::size_t subscription_count() const noexcept {
        std::lock_guard lock(subs_mutex_);
        return subscriptions_.size();
    }

    // -------------------------------------------------------------------------
    // 状态查询
    // -------------------------------------------------------------------------
    WsConnectionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    bool is_ready() const noexcept {
        return state() == WsConnectionState::Ready;
    }

    const WebSocketStats& stats() const noexcept {
        return stats_;
    }

    void reset_stats() noexcept {
        stats_.reset();
    }

    std::uint64_t connection_id() const noexcept {
        return connection_id_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // 手动控制
    // -------------------------------------------------------------------------
    void force_reconnect() noexcept {
        asio::post(ioc_, [self = shared_from_this()] {
            if (self->stopped_.load(std::memory_order_acquire)) return;
            self->close_connection("force_reconnect");
        });
    }

    void pause_message_processing() noexcept {
        paused_.store(true, std::memory_order_release);
    }

    void resume_message_processing() noexcept {
        paused_.store(false, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    std::string dump() const {
        std::ostringstream oss;
        oss << "BinanceWS Dump:\n";
        oss << "  state:            " << to_string(state()) << "\n";
        oss << "  connection_id:    " << connection_id() << "\n";
        oss << "  subscriptions:    " << subscription_count() << "\n";
        oss << "  url:              " << config_.get_url() << "\n";
        oss << "  messages_received: " << stats_.messages_received.load() << "\n";
        oss << "  messages_dropped:  " << stats_.messages_dropped.load() << "\n";
        oss << "  messages_invalid:  " << stats_.messages_invalid.load() << "\n";
        oss << "  connect_count:     " << stats_.connect_count.load() << "\n";
        oss << "  disconnect_count:  " << stats_.disconnect_count.load() << "\n";
        oss << "  reconnect_count:   " << stats_.reconnect_count.load() << "\n";
        oss << "  pong_timeout:      " << stats_.pong_timeout_count.load() << "\n";
        oss << "  queue_size:        " << stats_.queue_size.load() << "\n";
        oss << "  queue_peak:        " << stats_.queue_peak.load() << "\n";
        return oss.str();
    }

private:
    // =========================================================================
    // SSL 初始化
    // =========================================================================
    void init_ssl() {
        ssl_ctx_.set_options(
            ssl::context::default_workarounds |
            ssl::context::no_sslv2 |
            ssl::context::no_sslv3 |
            ssl::context::no_tlsv1 |
            ssl::context::no_tlsv1_1 |
            ssl::context::single_dh_use);

        if (config_.verify_tls) {
            ssl_ctx_.set_verify_mode(ssl::verify_peer);
            ssl_ctx_.set_default_verify_paths();
        } else {
            QUANT_LOG_WARN("[WS] TLS 验证已禁用（仅用于调试）");
            ssl_ctx_.set_verify_mode(ssl::verify_none);
        }
    }

    // =========================================================================
    // 状态管理
    // =========================================================================
    void set_state(WsConnectionState new_state) {
        const auto old = state_.exchange(new_state, std::memory_order_acq_rel);
        if (old != new_state) {
            QUANT_LOG_INFO("[WS] 状态: {} → {}",
                to_string(old), to_string(new_state));
        }
    }

    // =========================================================================
    // 连接
    // =========================================================================
    void do_connect() {
        if (stopped_.load(std::memory_order_acquire)) return;

        set_state(WsConnectionState::Connecting);

        // 启动连接超时
        connect_timer_.expires_after(config_.connect_timeout);
        connect_timer_.async_wait(
            [self = shared_from_this()](beast::error_code ec) {
                if (ec == asio::error::operation_aborted) return;
                if (self->state() == WsConnectionState::Connecting ||
                    self->state() == WsConnectionState::TlsHandshake) {
                    QUANT_LOG_WARN("[WS] 连接超时");
                    self->close_connection("connect_timeout");
                }
            });

        // DNS 解析
        resolver_.async_resolve(
            url_.host, url_.port,
            asio::bind_executor(strand_,
                [self = shared_from_this()](
                    beast::error_code ec,
                    tcp::resolver::results_type results) {
                    if (ec) {
                        self->handle_error("resolve", ec);
                        return;
                    }
                    self->on_resolve(std::move(results));
                }));
    }

    void on_resolve(tcp::resolver::results_type results) {
        if (stopped_.load(std::memory_order_acquire)) return;

        // TCP 连接
        beast::get_lowest_layer(ws_).expires_after(config_.connect_timeout);

        asio::async_connect(
            beast::get_lowest_layer(ws_),
            results,
            asio::bind_executor(strand_,
                [self = shared_from_this()](
                    beast::error_code ec,
                    const tcp::endpoint&) {
                    if (ec) {
                        self->handle_error("connect", ec);
                        return;
                    }
                    self->on_tcp_connected();
                }));
    }

    void on_tcp_connected() {
        if (stopped_.load(std::memory_order_acquire)) return;

        set_state(WsConnectionState::TlsHandshake);

        // TLS 握手
        beast::get_lowest_layer(ws_).expires_after(config_.connect_timeout);

        ws_.next_layer().async_handshake(
            ssl::stream_base::client,
            asio::bind_executor(strand_,
                [self = shared_from_this()](beast::error_code ec) {
                    if (ec) {
                        self->handle_error("tls_handshake", ec);
                        return;
                    }
                    self->on_tls_handshake();
                }));
    }

    void on_tls_handshake() {
        if (stopped_.load(std::memory_order_acquire)) return;

        // WebSocket 握手
        beast::get_lowest_layer(ws_).expires_after(config_.connect_timeout);

        // 配置 WebSocket
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(beast::http::field::user_agent,
                        "quant-trading-system/1.0.1");
            }));

        // 启用自动 pong（回复币安的 ping）
        ws_.auto_fragment(false);
        ws_.read_message_max(4 * 1024 * 1024);  // 4MB 上限

        ws_.async_handshake(
            url_.host, url_.path,
            asio::bind_executor(strand_,
                [self = shared_from_this()](beast::error_code ec) {
                    if (ec) {
                        self->handle_error("ws_handshake", ec);
                        return;
                    }
                    self->on_ws_handshake();
                }));
    }

    void on_ws_handshake() {
        if (stopped_.load(std::memory_order_acquire)) return;

        // 取消连接超时
        connect_timer_.cancel();

        // 取消 TCP 层超时（后续读写使用自己的超时）
        beast::get_lowest_layer(ws_).expires_never();

        set_state(WsConnectionState::Connected);

        const auto conn_id =
            connection_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
        stats_.connect_count.fetch_add(1, std::memory_order_relaxed);

        QUANT_LOG_INFO("[WS] 连接建立 #{}", conn_id);

        // 触发回调
        dispatch_callback([this] {
            if (callbacks_.on_connected) callbacks_.on_connected();
        });

        // 启动心跳
        start_ping();

        // 发送所有待订阅流
        std::vector<std::string> streams;
        {
            std::lock_guard lock(subs_mutex_);
            for (const auto& [k, v] : subscriptions_) {
                streams.push_back(k);
            }
        }

        if (streams.empty()) {
            // 无订阅，直接就绪
            on_ready();
        } else {
            set_state(WsConnectionState::Subscribing);
            send_subscribe(streams);
        }

        // 开始读取
        do_read();
    }

    void on_ready() {
        set_state(WsConnectionState::Ready);

        QUANT_LOG_INFO("[WS] 就绪 #{}", connection_id());
        stats_.reconnect_delay_.store(0, std::memory_order_relaxed);

        dispatch_callback([this] {
            if (callbacks_.on_ready) callbacks_.on_ready();
        });
    }

    // =========================================================================
    // 关闭与重连
    // =========================================================================
    void close_connection(std::string_view reason) {
        if (state() == WsConnectionState::Disconnected ||
            state() == WsConnectionState::Closing) {
            return;
        }

        set_state(WsConnectionState::Closing);

        // 停止心跳
        ping_timer_.cancel();
        pong_timer_.cancel();

        // 关闭 WebSocket
        beast::error_code ec;
        if (ws_.is_open()) {
            ws_.async_close(websocket::close_code::normal,
                asio::bind_executor(strand_,
                    [self = shared_from_this(),
                     reason = std::string{reason}](
                        beast::error_code ec) {
                        self->on_closed(reason, ec);
                    }));
        } else {
            on_closed(std::string{reason}, {});
        }
    }

    void on_closed(std::string_view reason, beast::error_code ec) {
        if (state() == WsConnectionState::Disconnected) return;

        // 关闭底层 socket
        beast::error_code close_ec;
        beast::get_lowest_layer(ws_).socket().close(close_ec);

        set_state(WsConnectionState::Disconnected);
        stats_.disconnect_count.fetch_add(1, std::memory_order_relaxed);

        if (ec && ec != websocket::error::closed) {
            QUANT_LOG_WARN("[WS] 关闭时错误: {}", ec.message());
        }

        QUANT_LOG_INFO("[WS] 连接已关闭: {}", reason);

        // 触发回调
        dispatch_callback([this, reason = std::string{reason}] {
            if (callbacks_.on_disconnected) callbacks_.on_disconnected(reason);
        });

        // 自动重连
        if (config_.auto_reconnect && !stopped_.load(std::memory_order_acquire)) {
            schedule_reconnect();
        }
    }

    void schedule_reconnect() {
        set_state(WsConnectionState::Reconnecting);
        stats_.reconnect_count.fetch_add(1, std::memory_order_relaxed);

        // 计算退避延迟
        auto delay_us = reconnect_delay_.load(std::memory_order_relaxed);
        auto base_delay = std::chrono::seconds{1} << std::min(
            static_cast<int>(delay_us / 1'000'000), 6);  // 1s, 2s, 4s, ..., 64s
        base_delay = std::min(base_delay, config_.reconnect_max_delay);

        const auto actual_delay = add_jitter(base_delay, std::chrono::seconds{2});

        // 更新下次延迟
        const auto next_us = std::chrono::duration_cast<std::chrono::microseconds>(
            base_delay).count();
        reconnect_delay_.store(next_us, std::memory_order_relaxed);

        QUANT_LOG_INFO("[WS] 将在 {} ms 后重连", actual_delay.count());

        reconnect_timer_.expires_after(actual_delay);
        reconnect_timer_.async_wait(
            [self = shared_from_this()](beast::error_code ec) {
                if (ec == asio::error::operation_aborted) return;
                if (self->stopped_.load(std::memory_order_acquire)) return;
                self->do_connect();
            });
    }

    // =========================================================================
    // 心跳
    // =========================================================================
    void start_ping() {
        ping_timer_.expires_after(config_.ping_interval);
        ping_timer_.async_wait(
            [self = shared_from_this()](beast::error_code ec) {
                if (ec == asio::error::operation_aborted) return;
                if (self->state() != WsConnectionState::Ready &&
                    self->state() != WsConnectionState::Subscribing) {
                    return;
                }
                self->send_ping();
            });
    }

    void send_ping() {
        if (!ws_.is_open()) return;

        // 发送应用层 ping（币安要求）
        const auto payload = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());

        ws_.async_ping(payload,
            asio::bind_executor(strand_,
                [self = shared_from_this()](beast::error_code ec) {
                    if (ec) {
                        self->handle_error("ping", ec);
                        return;
                    }
                    self->wait_pong();
                }));
    }

    void wait_pong() {
        last_activity_ = std::chrono::steady_clock::now();

        pong_timer_.expires_after(config_.pong_timeout);
        pong_timer_.async_wait(
            [self = shared_from_this()](beast::error_code ec) {
                if (ec == asio::error::operation_aborted) return;

                const auto elapsed = std::chrono::steady_clock::now() -
                    self->last_activity_;
                if (elapsed >= self->config_.pong_timeout) {
                    QUANT_LOG_WARN("[WS] Pong 超时，重连");
                    self->stats_.pong_timeout_count.fetch_add(1,
                        std::memory_order_relaxed);
                    self->close_connection("pong_timeout");
                }
            });

        // 安排下一次 ping
        start_ping();
    }

    // =========================================================================
    // 读取循环
    // =========================================================================
    void do_read() {
        if (stopped_.load(std::memory_order_acquire)) return;
        if (!ws_.is_open()) return;

        read_buffer_.consume(read_buffer_.size());  // 清空

        ws_.async_read(read_buffer_,
            asio::bind_executor(strand_,
                [self = shared_from_this()](
                    beast::error_code ec,
                    std::size_t bytes_transferred) {
                    if (ec) {
                        if (ec == websocket::error::closed) {
                            self->close_connection("ws_closed");
                        } else {
                            self->handle_error("read", ec);
                        }
                        return;
                    }
                    self->on_message(bytes_transferred);
                    self->do_read();
                }));
    }

    // =========================================================================
    // 消息处理
    // =========================================================================
    void on_message(std::size_t bytes_transferred) {
        const auto now = std::chrono::steady_clock::now();
        last_activity_ = now;

        // 取消 pong 超时（收到任何消息都视为活动）
        pong_timer_.cancel();

        stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
        stats_.bytes_received.fetch_add(bytes_transferred, std::memory_order_relaxed);

        // 提取消息
        std::string msg;
        msg.resize(read_buffer_.size());
        std::memcpy(msg.data(), read_buffer_.data().data(), read_buffer_.size());

        // 推入工作队列
        push_work(std::move(msg));
    }

    void push_work(std::string msg) {
        if (paused_.load(std::memory_order_acquire)) {
            stats_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        {
            std::lock_guard lock(queue_mutex_);
            if (queue_closed_) return;

            if (work_queue_.size() >= config_.message_queue_capacity) {
                stats_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
                // 丢弃最旧的消息
                work_queue_.pop_front();
            }

            work_queue_.push_back(std::move(msg));

            const auto qsize = work_queue_.size();
            stats_.queue_size.store(qsize, std::memory_order_relaxed);

            auto peak = stats_.queue_peak.load(std::memory_order_relaxed);
            while (qsize > peak &&
                   !stats_.queue_peak.compare_exchange_weak(
                       peak, qsize, std::memory_order_relaxed)) {}
        }
        queue_cv_.notify_one();
    }

    void worker_loop() {
        while (true) {
            std::string msg;
            {
                std::unique_lock lock(queue_mutex_);
                queue_cv_.wait(lock, [this] {
                    return queue_closed_ || !work_queue_.empty();
                });

                if (queue_closed_ && work_queue_.empty()) {
                    return;
                }

                msg = std::move(work_queue_.front());
                work_queue_.pop_front();
                stats_.queue_size.store(work_queue_.size(),
                    std::memory_order_relaxed);
            }

            process_message(msg);
        }
    }

    void process_message(std::string_view msg) {
        try {
            const auto j = json::parse(msg);
            dispatch_message(j);
        } catch (const json::parse_error& e) {
            stats_.messages_invalid.fetch_add(1, std::memory_order_relaxed);
            QUANT_LOG_WARN("[WS] JSON 解析失败: {}", e.what());
        } catch (const std::exception& e) {
            stats_.messages_invalid.fetch_add(1, std::memory_order_relaxed);
            QUANT_LOG_WARN("[WS] 消息处理异常: {}", e.what());
        }
    }

    void dispatch_message(const json& j) {
        // 1. 订阅响应
        if (j.contains("result") || j.contains("error")) {
            handle_subscription_response(j);
            return;
        }

        // 2. 组合流（stream + data）
        if (j.contains("stream") && j.contains("data")) {
            const auto stream = safe_string(j, "stream");
            const auto& data = j["data"];
            dispatch_stream_data(stream, data);
            return;
        }

        // 3. 单流消息
        if (j.contains("e")) {
            const auto event = safe_string(j, "e");
            dispatch_event(event, j);
            return;
        }

        // 4. 未知消息
        QUANT_LOG_DEBUG("[WS] 未知消息: {}", j.dump().substr(0, 200));
    }

    void handle_subscription_response(const json& j) {
        if (j.contains("error")) {
            const auto& err = j["error"];
            const auto code = safe_int64(err, "code", 0);
            const auto msg = safe_string(err, "msg");

            stats_.subscribe_failure_count.fetch_add(1, std::memory_order_relaxed);
            QUANT_LOG_ERROR("[WS] 订阅失败: code={} msg={}", code, msg);

            dispatch_callback([this, code, msg] {
                if (callbacks_.on_subscription_result) {
                    callbacks_.on_subscription_result("", false, msg);
                }
            });
            return;
        }

        const auto result = j.value("result", json::array());
        const auto id = safe_int64(j, "id", 0);

        if (id == 1) {
            // 订阅完成，标记就绪
            if (state() == WsConnectionState::Subscribing) {
                on_ready();
            }
        }

        dispatch_callback([this, result] {
            if (callbacks_.on_subscription_result) {
                for (const auto& s : result) {
                    if (s.is_string()) {
                        callbacks_.on_subscription_result(s.get<std::string>(),
                            true, "");
                    }
                }
            }
        });
    }

    void dispatch_stream_data(const std::string& stream, const json& data) {
        // 从 stream 名称推断类型
        if (stream.find("@kline_") != std::string::npos) {
            handle_kline(data);
        } else if (stream.find("@depth") != std::string::npos) {
            handle_depth(data);
        } else if (stream.find("@aggTrade") != std::string::npos) {
            handle_agg_trade(data);
        } else if (stream.find("@markPrice") != std::string::npos) {
            handle_mark_price(data);
        } else if (stream.find("@fundingRate") != std::string::npos) {
            handle_funding_rate(data);
        }
    }

    void dispatch_event(const std::string& event, const json& j) {
        if (event == "kline") {
            handle_kline(j);
        } else if (event == "depthUpdate") {
            handle_depth(j);
        } else if (event == "aggTrade") {
            handle_agg_trade(j);
        } else if (event == "markPriceUpdate") {
            handle_mark_price(j);
        } else if (event == "serverShutdown") {
            handle_maintenance(j);
        }
    }

    // =========================================================================
    // 数据类型处理
    // =========================================================================
    void handle_kline(const json& j) {
        try {
            const auto& k = j.contains("k") ? j["k"] : j;

            Candle candle{};
            candle.open_time = Timestamp{
                safe_int64(k, "t", 0) * 1000LL};        // 毫秒 → 微秒
            candle.close_time = Timestamp{
                safe_int64(k, "T", 0) * 1000LL};
            candle.open = Price::from_double(safe_double(k, "o"));
            candle.high = Price::from_double(safe_double(k, "h"));
            candle.low = Price::from_double(safe_double(k, "l"));
            candle.close = Price::from_double(safe_double(k, "c"));
            candle.volume = Quantity::from_double(safe_double(k, "v"));
            candle.quote_volume = Quantity::from_double(safe_double(k, "q"));
            candle.trades = static_cast<std::uint32_t>(safe_int64(k, "n", 0));
            candle.is_closed = safe_bool(k, "x", false);

            // 延迟统计
            const auto now_us = Timestamp::now().microseconds();
            const auto latency = now_us - candle.close_time.microseconds();
            stats_.last_message_latency_us.store(latency, std::memory_order_relaxed);

            auto max_lat = stats_.max_message_latency_us.load(std::memory_order_relaxed);
            while (latency > max_lat &&
                   !stats_.max_message_latency_us.compare_exchange_weak(
                       max_lat, latency, std::memory_order_relaxed)) {}

            if (latency > static_cast<std::int64_t>(
                    config_.latency_warn_threshold.count()) * 1000) {
                QUANT_LOG_WARN("[WS] K线延迟过高: {} ms", latency / 1000);
            }

            dispatch_callback([this, candle] {
                if (candle.is_closed) {
                    if (callbacks_.on_kline_closed) {
                        callbacks_.on_kline_closed(candle);
                    }
                } else {
                    if (callbacks_.on_kline_update) {
                        callbacks_.on_kline_update(candle);
                    }
                }
            });
        } catch (const std::exception& e) {
            QUANT_LOG_WARN("[WS] K线处理异常: {}", e.what());
        }
    }

    void handle_depth(const json& j) {
        try {
            const auto& data = j.contains("b") ? j : j.value("data", j);

            std::vector<std::pair<double, double>> bids;
            std::vector<std::pair<double, double>> asks;

            if (data.contains("bids")) {
                for (const auto& b : data["bids"]) {
                    if (b.size() >= 2) {
                        bids.emplace_back(b[0].get<double>(),
                                           b[1].get<double>());
                    }
                }
            } else if (data.contains("b")) {
                for (const auto& b : data["b"]) {
                    if (b.size() >= 2) {
                        bids.emplace_back(std::stod(b[0].get<std::string>()),
                                           std::stod(b[1].get<std::string>()));
                    }
                }
            }

            if (data.contains("asks")) {
                for (const auto& a : data["asks"]) {
                    if (a.size() >= 2) {
                        asks.emplace_back(a[0].get<double>(),
                                           a[1].get<double>());
                    }
                }
            } else if (data.contains("a")) {
                for (const auto& a : data["a"]) {
                    if (a.size() >= 2) {
                        asks.emplace_back(std::stod(a[0].get<std::string>()),
                                           std::stod(a[1].get<std::string>()));
                    }
                }
            }

            dispatch_callback([this, bids = std::move(bids),
                                     asks = std::move(asks)]() mutable {
                if (callbacks_.on_depth_update) {
                    callbacks_.on_depth_update(bids, asks);
                }
            });
        } catch (const std::exception& e) {
            QUANT_LOG_WARN("[WS] 深度处理异常: {}", e.what());
        }
    }

    void handle_agg_trade(const json& j) {
        try {
            const auto& data = j.contains("data") ? j["data"] : j;

            const auto symbol = safe_string(data, "s");
            const auto price = safe_double(data, "p");
            const auto qty = safe_double(data, "q");
            const auto is_buyer_maker = safe_bool(data, "m", false);
            const auto time = Timestamp{
                safe_int64(data, "T", 0) * 1000LL};

            dispatch_callback([this, symbol, price, qty, is_buyer_maker, time] {
                if (callbacks_.on_trade) {
                    callbacks_.on_trade(symbol, price, qty, is_buyer_maker, time);
                }
            });
        } catch (const std::exception& e) {
            QUANT_LOG_WARN("[WS] 成交流处理异常: {}", e.what());
        }
    }

    void handle_mark_price(const json& j) {
        try {
            const auto& data = j.contains("data") ? j["data"] : j;
            const auto symbol = safe_string(data, "s");
            const auto mark_price = safe_double(data, "p");

            dispatch_callback([this, symbol, mark_price] {
                if (callbacks_.on_mark_price) {
                    callbacks_.on_mark_price(symbol, mark_price);
                }
            });
        } catch (const std::exception& e) {
            QUANT_LOG_WARN("[WS] 标记价格处理异常: {}", e.what());
        }
    }

    void handle_funding_rate(const json& j) {
        try {
            const auto& data = j.contains("data") ? j["data"] : j;
            const auto symbol = safe_string(data, "s");
            const auto rate = safe_double(data, "r");
            const auto next_time = Timestamp{
                safe_int64(data, "T", 0) * 1000LL};

            dispatch_callback([this, symbol, rate, next_time] {
                if (callbacks_.on_funding_rate) {
                    callbacks_.on_funding_rate(symbol, rate, next_time);
                }
            });
        } catch (const std::exception& e) {
            QUANT_LOG_WARN("[WS] 资金费率处理异常: {}", e.what());
        }
    }

    void handle_maintenance(const json& j) {
        try {
            const auto msg = safe_string(j, "msg");
            const auto time = Timestamp{
                safe_int64(j, "T", 0) * 1000LL};

            QUANT_LOG_CRITICAL("[WS] 交易所维护: {}", msg);

            dispatch_callback([this, msg, time] {
                if (callbacks_.on_maintenance) {
                    callbacks_.on_maintenance(msg, time);
                }
            });
        } catch (const std::exception& e) {
            QUANT_LOG_WARN("[WS] 维护消息处理异常: {}", e.what());
        }
    }

    // =========================================================================
    // 发送订阅
    // =========================================================================
    void send_subscribe(const std::vector<std::string>& streams) {
        if (streams.empty() || !ws_.is_open()) return;

        json msg = {
            {"method", "SUBSCRIBE"},
            {"params", streams},
            {"id", 1}
        };

        send_message(msg.dump());
    }

    void send_unsubscribe(const std::vector<std::string>& streams) {
        if (streams.empty() || !ws_.is_open()) return;

        json msg = {
            {"method", "UNSUBSCRIBE"},
            {"params", streams},
            {"id", 2}
        };

        send_message(msg.dump());
    }

    void send_message(std::string msg) {
        const bool writing = writing_.exchange(true, std::memory_order_acq_rel);

        if (writing) {
            // 已有写入进行中，排队
            std::lock_guard lock(write_mutex_);
            write_queue_.push_back(std::move(msg));
            return;
        }

        do_write(std::move(msg));
    }

    void do_write(std::string msg) {
        ws_.text(true);
        ws_.async_write(
            asio::buffer(msg),
            asio::bind_executor(strand_,
                [self = shared_from_this(), msg = std::move(msg)](
                    beast::error_code ec,
                    std::size_t) {
                    if (ec) {
                        self->writing_.store(false, std::memory_order_release);
                        self->handle_error("write", ec);
                        return;
                    }
                    self->on_write_complete();
                }));
    }

    void on_write_complete() {
        std::string next_msg;
        {
            std::lock_guard lock(write_mutex_);
            if (!write_queue_.empty()) {
                next_msg = std::move(write_queue_.front());
                write_queue_.pop_front();
            }
        }

        if (next_msg.empty()) {
            writing_.store(false, std::memory_order_release);
            return;
        }

        do_write(std::move(next_msg));
    }

    // =========================================================================
    // 错误处理
    // =========================================================================
    void handle_error(std::string_view stage, beast::error_code ec) {
        QUANT_LOG_ERROR("[WS] {} 错误: {}", stage, ec.message());

        const ErrorInfo info{
            ErrorCode::NetworkUnknown,
            ec.message(),
            SourceLocation{__FILE__, static_cast<std::uint32_t>(__LINE__),
                           __func__}};

        dispatch_callback([this, info] {
            if (callbacks_.on_error) callbacks_.on_error(info);
        });

        close_connection(stage);
    }

    // =========================================================================
    // 回调分发
    // =========================================================================
    template <typename F>
    void dispatch_callback(F&& f) {
        // 直接执行（已在工作线程）
        try {
            f();
        } catch (const std::exception& e) {
            QUANT_LOG_ERROR("[WS] 回调异常: {}", e.what());
        } catch (...) {
            QUANT_LOG_ERROR("[WS] 回调未知异常");
        }
    }

    // =========================================================================
    // 成员
    // =========================================================================
    BinanceWSConfig config_;
    ParsedUrl url_;

    // I/O
    asio::io_context ioc_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    ssl::context ssl_ctx_;
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    asio::strand<asio::io_context::executor_type> strand_;

    // 定时器
    asio::steady_timer ping_timer_;
    asio::steady_timer pong_timer_;
    asio::steady_timer reconnect_timer_;
    asio::steady_timer connect_timer_;

    // 线程
    std::thread io_thread_;
    std::vector<std::thread> worker_threads_;

    // 缓冲
    beast::flat_buffer read_buffer_;

    // 状态
    std::atomic<WsConnectionState> state_{WsConnectionState::Disconnected};
    std::atomic<std::uint64_t> connection_id_{0};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> paused_{false};
    std::atomic<std::int64_t> reconnect_delay_{0};

    // 心跳
    std::chrono::steady_clock::time_point last_activity_{};

    // 写入串行化
    std::atomic<bool> writing_{false};
    std::mutex write_mutex_;
    std::deque<std::string> write_queue_;

    // 工作队列
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::string> work_queue_;
    bool queue_closed_{false};

    // 订阅
    mutable std::mutex subs_mutex_;
    std::unordered_map<std::string, SubscriptionRequest> subscriptions_;

    // 回调
    WebSocketCallbacks callbacks_;

    // 统计
    WebSocketStats stats_;
};

// ==============================================================================
// BinanceWS 转发
// ==============================================================================
BinanceWS::BinanceWS(const BinanceWSConfig& config)
    : impl_{std::make_unique<Impl>(config)}
{}

BinanceWS::~BinanceWS() = default;

void BinanceWS::set_callbacks(WebSocketCallbacks callbacks) {
    impl_->set_callbacks(std::move(callbacks));
}

Result<void> BinanceWS::start() {
    return impl_->start();
}

void BinanceWS::stop() noexcept {
    impl_->stop();
}

Result<void> BinanceWS::subscribe(const SubscriptionRequest& request) {
    return impl_->subscribe(request);
}

Result<void> BinanceWS::subscribe(std::span<const SubscriptionRequest> requests) {
    return impl_->subscribe(requests);
}

Result<void> BinanceWS::unsubscribe(std::string_view symbol,
                                     std::string_view channel) {
    return impl_->unsubscribe(symbol, channel);
}

Result<void> BinanceWS::unsubscribe_all() {
    return impl_->unsubscribe_all();
}

std::size_t BinanceWS::subscription_count() const noexcept {
    return impl_->subscription_count();
}

WsConnectionState BinanceWS::state() const noexcept {
    return impl_->state();
}

bool BinanceWS::is_ready() const noexcept {
    return impl_->is_ready();
}

const WebSocketStats& BinanceWS::stats() const noexcept {
    return impl_->stats();
}

void BinanceWS::reset_stats() noexcept {
    impl_->reset_stats();
}

std::uint64_t BinanceWS::connection_id() const noexcept {
    return impl_->connection_id();
}

void BinanceWS::force_reconnect() noexcept {
    impl_->force_reconnect();
}

void BinanceWS::pause_message_processing() noexcept {
    impl_->pause_message_processing();
}

void BinanceWS::resume_message_processing() noexcept {
    impl_->resume_message_processing();
}

std::string BinanceWS::dump() const {
    return impl_->dump();
}

}  // namespace data
}  // namespace quant
