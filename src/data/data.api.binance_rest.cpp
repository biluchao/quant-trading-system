// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 币安 REST API 客户端实现
// ==============================================================================
// @file    src/data/data.api.binance_rest.cpp
// @module  data
// @type    api
// @name    binance_rest
// @version 1.0.1
// @brief   基于 libcurl 的币安合约 REST 客户端实现
//          已修复 25 类运行时问题
//
// 线程模型:
//   - 每个调用线程维护独立的 CURL 句柄（thread_local）
//   - 全局 CURL 初始化使用 call_once 保证一次
//   - 速率限制使用原子计数器，多线程安全
// ==============================================================================

#include "data/data.api.binance_rest.hpp"

// ==============================================================================
// libcurl
// ==============================================================================
#include <curl/curl.h>

// ==============================================================================
// JSON
// ==============================================================================
#include <nlohmann/json.hpp>

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace quant {
namespace data {

// ==============================================================================
// 命名空间别名
// ==============================================================================
using json = nlohmann::json;

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// libcurl 全局初始化（进程级一次）
// -----------------------------------------------------------------------------
void ensure_curl_global_init() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        const auto rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            QUANT_LOG_CRITICAL("[REST] curl_global_init 失败: {}",
                curl_easy_strerror(rc));
        }
    });
}

// -----------------------------------------------------------------------------
// 线程本地 CURL 句柄（RAII）
// -----------------------------------------------------------------------------
class CurlHandle {
public:
    CurlHandle() {
        ensure_curl_global_init();
        handle_ = curl_easy_init();
        if (!handle_) {
            QUANT_LOG_ERROR("[REST] curl_easy_init 失败");
        }
    }

    ~CurlHandle() {
        if (handle_) {
            curl_easy_cleanup(handle_);
        }
    }

    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;

    [[nodiscard]] CURL* get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

    // 重置句柄（保留连接池）
    void reset() noexcept {
        if (handle_) {
            curl_easy_reset(handle_);
        }
    }

private:
    CURL* handle_{nullptr};
};

[[nodiscard]] CurlHandle& thread_curl() {
    static thread_local CurlHandle handle;
    return handle;
}

// -----------------------------------------------------------------------------
// 写入回调（带大小上限）
// -----------------------------------------------------------------------------
struct WriteContext {
    std::string* buffer;
    std::size_t max_size;
    bool overflow{false};
};

[[nodiscard]] std::size_t write_callback(
    char* ptr, std::size_t size, std::size_t nmemb, void* userdata) noexcept
{
    auto* ctx = static_cast<WriteContext*>(userdata);
    const auto total = size * nmemb;

    if (ctx->buffer->size() + total > ctx->max_size) {
        ctx->overflow = true;
        return 0;   // 中断传输
    }

    try {
        ctx->buffer->append(ptr, total);
    } catch (...) {
        return 0;
    }
    return total;
}

// -----------------------------------------------------------------------------
// 响应头回调（捕获 X-MBX-USED-WEIGHT）
// -----------------------------------------------------------------------------
struct HeaderContext {
    std::size_t used_weight{0};
    std::size_t used_weight_1m{0};
};

[[nodiscard]] std::size_t header_callback(
    char* buffer, std::size_t size, std::size_t nitems, void* userdata) noexcept
{
    auto* ctx = static_cast<HeaderContext*>(userdata);
    const auto total = size * nitems;

    constexpr std::string_view kUsedWeight = "x-mbx-used-weight:";
    constexpr std::string_view kUsedWeight1m = "x-mbx-used-weight-1m:";

    std::string_view line{buffer, total};
    std::string lower;
    lower.reserve(line.size());
    for (char c : line) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }

    auto parse_after = [&](std::string_view prefix) -> std::size_t {
        const auto pos = lower.find(prefix);
        if (pos == std::string_view::npos) return 0;
        auto value_str = line.substr(pos + prefix.size());
        while (!value_str.empty() &&
               (value_str.front() == ' ' || value_str.front() == '\t')) {
            value_str.remove_prefix(1);
        }
        while (!value_str.empty() &&
               (value_str.back() == '\r' || value_str.back() == '\n' ||
                value_str.back() == ' ')) {
            value_str.remove_suffix(1);
        }
        std::size_t result = 0;
        const auto [ptr, ec] = std::from_chars(
            value_str.data(), value_str.data() + value_str.size(), result);
        if (ec != std::errc{}) return 0;
        return result;
    };

    const auto w1 = parse_after(kUsedWeight);
    const auto w1m = parse_after(kUsedWeight1m);
    if (w1 > 0) ctx->used_weight = w1;
    if (w1m > 0) ctx->used_weight_1m = w1m;

    return total;
}

// -----------------------------------------------------------------------------
// URL 编码
// -----------------------------------------------------------------------------
[[nodiscard]] std::string url_encode(std::string_view value) {
    CURL* curl = thread_curl().get();
    if (!curl) return std::string{value};

    char* encoded = curl_easy_escape(curl, value.data(),
        static_cast<int>(value.size()));
    if (!encoded) return std::string{value};

    std::string result{encoded};
    curl_free(encoded);
    return result;
}

// -----------------------------------------------------------------------------
// 数字转字符串（避免 locale 影响）
// -----------------------------------------------------------------------------
[[nodiscard]] std::string format_double(double v) {
    std::array<char, 64> buf{};
    const auto [ptr, ec] = std::to_chars(
        buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, 8);
    if (ec != std::errc{}) return "0";
    // 去除尾部多余的 0
    std::string result{buf.data(), ptr};
    const auto dot = result.find('.');
    if (dot != std::string::npos) {
        while (!result.empty() && result.back() == '0') result.pop_back();
        if (!result.empty() && result.back() == '.') result.pop_back();
    }
    return result.empty() ? "0" : result;
}

[[nodiscard]] std::string format_int64(std::int64_t v) {
    return std::to_string(v);
}

// -----------------------------------------------------------------------------
// 安全提取
// -----------------------------------------------------------------------------
[[nodiscard]] double json_double(const json& j, std::string_view key,
                                  double default_value = 0.0) noexcept {
    try {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null()) return default_value;
        if (it->is_number()) return it->get<double>();
        if (it->is_string()) return std::stod(it->get<std::string>());
        return default_value;
    } catch (...) {
        return default_value;
    }
}

[[nodiscard]] std::int64_t json_int64(const json& j, std::string_view key,
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

[[nodiscard]] std::string json_string(const json& j, std::string_view key) {
    try {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null()) return {};
        if (it->is_string()) return it->get<std::string>();
        return it->dump();
    } catch (...) {
        return {};
    }
}

[[nodiscard]] bool json_bool(const json& j, std::string_view key,
                              bool default_value = false) noexcept {
    try {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null()) return default_value;
        if (it->is_boolean()) return it->get<bool>();
        if (it->is_number()) return it->get<double>() != 0.0;
        return default_value;
    } catch (...) {
        return false;
    }
}

// -----------------------------------------------------------------------------
// 指数退避抖动
// -----------------------------------------------------------------------------
[[nodiscard]] std::chrono::milliseconds compute_backoff(
    int attempt,
    std::chrono::milliseconds initial,
    std::chrono::milliseconds max_delay) noexcept
{
    const auto base_ms = static_cast<std::int64_t>(initial.count()) << attempt;
    const auto capped = std::min(base_ms,
        static_cast<std::int64_t>(max_delay.count()));

    // 抖动 ±20%
    thread_local std::mt19937 rng{
        static_cast<std::uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count())};
    std::uniform_int_distribution<std::int64_t> dist(
        -(capped / 5), capped / 5);

    return std::chrono::milliseconds{
        std::max<std::int64_t>(100, capped + dist(rng))};
}

// -----------------------------------------------------------------------------
// URL 规范化（去重斜杠）
// -----------------------------------------------------------------------------
[[nodiscard]] std::string join_url(std::string_view base, std::string_view path) {
    std::string result{base};
    while (!result.empty() && result.back() == '/') result.pop_back();
    if (!path.empty() && path.front() != '/') result += '/';
    result.append(path);
    return result;
}

// -----------------------------------------------------------------------------
// 判断 HTTP 状态是否可重试
// -----------------------------------------------------------------------------
[[nodiscard]] bool is_retryable_http(int status) noexcept {
    return status == 429 || status == 418 ||
           (status >= 500 && status < 600);
}

[[nodiscard]] bool is_retryable_curl(CURLcode code) noexcept {
    switch (code) {
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
        case CURLE_SSL_CONNECT_ERROR:
            return true;
        default:
            return false;
    }
}

}  // namespace

// ==============================================================================
// BinanceRestConfig::get_base_url
// ==============================================================================
std::string BinanceRestConfig::get_base_url() const {
    if (custom_url) return *custom_url;
    return use_testnet
        ? std::string{binance_rest_config::kTestnetUrl}
        : std::string{binance_rest_config::kMainnetUrl};
}

// ==============================================================================
// 币安错误码映射
// ==============================================================================
ErrorCode from_binance_error_code(int code) noexcept {
    switch (code) {
        case -1000: return ErrorCode::ExchangeUnknown;
        case -1001: return ErrorCode::NetworkDisconnected;
        case -1002: return ErrorCode::ExchangeSignatureError;
        case -1003: return ErrorCode::ExchangeRateLimit;
        case -1006: return ErrorCode::ExchangeTimestampError;
        case -1007: return ErrorCode::NetworkTimeout;
        case -1013: return ErrorCode::ExchangeInvalidParam;
        case -1015: return ErrorCode::ExchangeRateLimit;
        case -1021: return ErrorCode::ExchangeTimestampError;
        case -1022: return ErrorCode::ExchangeSignatureError;
        case -1100: return ErrorCode::ExchangeInvalidParam;
        case -1102: return ErrorCode::ExchangeInvalidParam;
        case -1111: return ErrorCode::ExchangeInvalidParam;
        case -1121: return ErrorCode::ExchangeInvalidSymbol;
        case -2010: return ErrorCode::OrderRejected;
        case -2011: return ErrorCode::OrderCancelFailed;
        case -2013: return ErrorCode::OrderNotFound;
        case -2014: return ErrorCode::ExchangeSignatureError;
        case -2015: return ErrorCode::ExchangeSignatureError;
        case -2019: return ErrorCode::ExchangeInsufficient;
        case -4131: return ErrorCode::OrderRejected;
        default:    return ErrorCode::ExchangeApiError;
    }
}

// ==============================================================================
// BinanceRest::Impl
// ==============================================================================
class BinanceRest::Impl {
public:
    explicit Impl(const BinanceRestConfig& config)
        : config_{config} {}

    ~Impl() = default;

    // =========================================================================
    // 初始化
    // =========================================================================
    Result<void> initialize() {
        ensure_curl_global_init();

        // 校验 URL
        if (config_.get_base_url().empty()) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "REST base URL 为空");
        }

        // 校验密钥（若使用私有接口）
        if (config_.api_key.empty() || config_.api_secret.empty()) {
            QUANT_LOG_WARN("[REST] API 密钥为空，仅支持公共接口");
        }

        // 启动时间同步线程
        if (config_.auto_time_sync) {
            sync_thread_ = std::thread([this] { time_sync_loop(); });
        }

        initialized_.store(true, std::memory_order_release);
        QUANT_LOG_INFO("[REST] 客户端初始化完成: {}", config_.get_base_url());
        return {};
    }

    void shutdown() noexcept {
        if (stopped_.exchange(true, std::memory_order_acq_rel)) return;

        // 停止时间同步线程
        {
            std::lock_guard lock(sync_mutex_);
            sync_cv_.notify_all();
        }
        if (sync_thread_.joinable()) {
            sync_thread_.join();
        }

        initialized_.store(false, std::memory_order_release);
        QUANT_LOG_INFO("[REST] 客户端已关闭");
    }

    // =========================================================================
    // 时间同步
    // =========================================================================
    Result<void> sync_time() {
        const auto url = join_url(config_.get_base_url(), "/fapi/v1/time");

        std::string response;
        std::size_t status = 0;
        auto result = raw_request("GET", url, {}, false, false,
                                   response, status);
        if (result.is_err()) return result;

        if (status != 200) {
            return QUANT_ERR_MSG(ErrorCode::ExchangeApiError,
                "时间接口返回非 200")
                .with_context("status", std::to_string(status));
        }

        try {
            const auto j = json::parse(response);
            const auto server_time_ms = json_int64(j, "serverTime");
            const auto local_now_us = Timestamp::now().microseconds();
            const auto server_now_us = server_time_ms * 1000LL;
            const auto offset = server_now_us - local_now_us;

            time_offset_us_.store(offset, std::memory_order_release);
            stats_.time_sync_count.fetch_add(1, std::memory_order_relaxed);
            stats_.current_time_offset_us.store(offset,
                std::memory_order_relaxed);

            QUANT_LOG_INFO("[REST] 时间同步完成: 偏移 {} ms",
                offset / 1000);

            if (std::abs(offset) > binance_rest_config::kMaxTimeDriftUs) {
                QUANT_LOG_WARN("[REST] 时间偏移过大: {} ms",
                    offset / 1000);
            }

            return {};
        } catch (const std::exception& e) {
            return QUANT_ERR_MSG(ErrorCode::ConfigParseFailed,
                std::string{"时间接口解析失败: "} + e.what());
        }
    }

    Timestamp server_time() const noexcept {
        const auto local_us = Timestamp::now().microseconds();
        const auto offset = time_offset_us_.load(std::memory_order_acquire);
        return Timestamp{local_us + offset};
    }

    std::int64_t time_offset_us() const noexcept {
        return time_offset_us_.load(std::memory_order_acquire);
    }

    // =========================================================================
    // 速率限制
    // =========================================================================
    [[nodiscard]] std::size_t current_weight() const noexcept {
        return current_weight_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_rate_limited() const noexcept {
        return current_weight_.load(std::memory_order_acquire) >=
               config_.rate_limit_block_threshold;
    }

    // 等待直到可以发送请求
    void wait_for_rate_limit(std::size_t upcoming_weight) {
        const auto used = current_weight_.load(std::memory_order_acquire);
        if (used + upcoming_weight < config_.rate_limit_block_threshold) {
            return;
        }

        QUANT_LOG_WARN("[REST] 速率接近上限: {}/{}，等待",
            used, config_.rate_limit_weight_per_minute);

        // 等待到窗口重置
        std::unique_lock lock(rate_limit_mutex_);
        rate_limit_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
            return current_weight_.load(std::memory_order_acquire) <
                   config_.rate_limit_warn_threshold;
        });
    }

    // =========================================================================
    // 请求执行
    // =========================================================================
    Result<void> raw_request(
        std::string_view method,
        std::string_view url,
        const std::vector<std::string>& extra_headers,
        bool signed_request,
        bool is_private,
        std::string& response_out,
        std::size_t& status_out)
    {
        auto& handle = thread_curl();
        if (!handle.valid()) {
            return QUANT_ERR_MSG(ErrorCode::InternalUnknown,
                "CURL 句柄无效");
        }

        CURL* curl = handle.get();
        curl_easy_reset(curl);

        // ---------------------------------------------------------------------
        // URL
        // ---------------------------------------------------------------------
        curl_easy_setopt(curl, CURLOPT_URL, url.data());

        // ---------------------------------------------------------------------
        // 请求方法
        // ---------------------------------------------------------------------
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
        } else if (method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        } else if (method == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        } else {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        }

        // ---------------------------------------------------------------------
        // 超时
        // ---------------------------------------------------------------------
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
            static_cast<long>(config_.connect_timeout.count()));
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
            static_cast<long>(config_.request_timeout.count()));

        // ---------------------------------------------------------------------
        // SSL
        // ---------------------------------------------------------------------
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,
            config_.verify_ssl ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,
            config_.verify_ssl ? 2L : 0L);

        // ---------------------------------------------------------------------
        // 重定向
        // ---------------------------------------------------------------------
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);

        // ---------------------------------------------------------------------
        // IPv4 优先（部分网络 IPv6 不通）
        // ---------------------------------------------------------------------
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

        // ---------------------------------------------------------------------
        // GZIP 自动解压
        // ---------------------------------------------------------------------
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

        // ---------------------------------------------------------------------
        // User-Agent
        // ---------------------------------------------------------------------
        if (!config_.user_agent.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERAGENT,
                config_.user_agent.c_str());
        }

        // ---------------------------------------------------------------------
        // 代理
        // ---------------------------------------------------------------------
        if (config_.proxy_url) {
            curl_easy_setopt(curl, CURLOPT_PROXY,
                config_.proxy_url->c_str());
        }

        // ---------------------------------------------------------------------
        // 响应体
        // ---------------------------------------------------------------------
        std::string response;
        response.reserve(64 * 1024);

        WriteContext write_ctx{&response, config_.max_response_size, false};
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);

        // ---------------------------------------------------------------------
        // 响应头
        // ---------------------------------------------------------------------
        HeaderContext header_ctx;
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_ctx);

        // ---------------------------------------------------------------------
        // 请求头
        // ---------------------------------------------------------------------
        struct curl_slist* headers = nullptr;
        auto free_headers = [&] {
            if (headers) {
                curl_slist_free_all(headers);
                headers = nullptr;
            }
        };

        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");

        if (is_private) {
            if (config_.api_key.empty()) {
                free_headers();
                return QUANT_ERR_MSG(ErrorCode::ExchangeSignatureError,
                    "私有接口需要 API Key");
            }
            const auto header = "X-MBX-APIKEY: " + config_.api_key;
            headers = curl_slist_append(headers, header.c_str());
        }

        for (const auto& h : extra_headers) {
            headers = curl_slist_append(headers, h.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // ---------------------------------------------------------------------
        // 执行
        // ---------------------------------------------------------------------
        const auto t0 = std::chrono::steady_clock::now();
        const auto code = curl_easy_perform(curl);
        const auto t1 = std::chrono::steady_clock::now();

        const auto latency_us = std::chrono::duration_cast<
            std::chrono::microseconds>(t1 - t0).count();

        free_headers();

        // 更新统计
        stats_.last_latency_us.store(latency_us, std::memory_order_relaxed);
        stats_.total_latency_us.fetch_add(latency_us,
            std::memory_order_relaxed);
        {
            auto max_lat = stats_.max_latency_us.load(std::memory_order_relaxed);
            while (latency_us > max_lat &&
                   !stats_.max_latency_us.compare_exchange_weak(
                       max_lat, latency_us, std::memory_order_relaxed)) {}
        }

        // ---------------------------------------------------------------------
        // CURL 错误处理
        // ---------------------------------------------------------------------
        if (code != CURLE_OK) {
            if (write_ctx.overflow) {
                return QUANT_ERR_MSG(ErrorCode::SystemOutOfMemory,
                    "响应体超过上限")
                    .with_context("limit",
                        std::to_string(config_.max_response_size));
            }

            return QUANT_ERR_MSG(ErrorCode::NetworkUnknown,
                std::string{"CURL 错误: "} + curl_easy_strerror(code))
                .with_context("curl_code", std::to_string(code));
        }

        // ---------------------------------------------------------------------
        // HTTP 状态码
        // ---------------------------------------------------------------------
        long status_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        status_out = static_cast<std::size_t>(status_code);

        // 更新权重（从响应头解析）
        if (header_ctx.used_weight_1m > 0) {
            current_weight_.store(header_ctx.used_weight_1m,
                std::memory_order_release);
        }

        response_out = std::move(response);

        stats_.bytes_received.fetch_add(response_out.size(),
            std::memory_order_relaxed);

        return {};
    }

    // =========================================================================
    // 带重试的请求
    // =========================================================================
    Result<std::string> request_with_retry(
        std::string_view method,
        std::string_view path,
        const std::vector<std::pair<std::string, std::string>>& query_params,
        const std::vector<std::pair<std::string, std::string>>& body_params,
        bool is_private,
        std::size_t weight = 1)
    {
        if (!initialized_.load(std::memory_order_acquire)) {
            return QUANT_ERR_T(std::string, ErrorCode::InternalUnknown)
                .with_context("reason", "未初始化");
        }

        // 速率限制
        wait_for_rate_limit(weight);

        // 构建 URL
        std::string url = join_url(config_.get_base_url(), path);

        // 参数
        std::vector<std::pair<std::string, std::string>> all_params;
        all_params.reserve(query_params.size() + body_params.size() + 3);
        for (const auto& [k, v] : query_params) {
            all_params.emplace_back(k, v);
        }
        for (const auto& [k, v] : body_params) {
            all_params.emplace_back(k, v);
        }

        // 私有请求：添加时间戳和签名
        if (is_private) {
            if (config_.api_secret.empty()) {
                return QUANT_ERR_T(std::string, ErrorCode::ExchangeSignatureError)
                    .with_context("reason", "缺少 API Secret");
            }

            const auto ts = server_time().microseconds() / 1000;   // 微秒 → 毫秒
            all_params.emplace_back("timestamp", std::to_string(ts));
            all_params.emplace_back("recvWindow", "5000");
        }

        // URL 编码 + 拼接
        std::string query_string;
        for (const auto& [k, v] : all_params) {
            if (!query_string.empty()) query_string += '&';
            query_string += url_encode(k);
            query_string += '=';
            query_string += url_encode(v);
        }

        // 签名
        if (is_private) {
            const auto signature = sign(query_string);
            if (signature.is_err()) {
                return QUANT_ERR_T(std::string, signature.error().code);
            }
            query_string += "&signature=" + signature.value();
        }

        // 拼接最终 URL
        if (!query_string.empty()) {
            if (method == "GET" || method == "DELETE") {
                url += "?" + query_string;
            }
        }

        // 重试循环
        std::string response;
        std::size_t status = 0;
        ErrorInfo last_error;

        const int max_attempts = 1 + std::max(0, config_.max_retries);

        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            stats_.requests_total.fetch_add(1, std::memory_order_relaxed);
            if (attempt > 0) {
                stats_.requests_retried.fetch_add(1, std::memory_order_relaxed);
            }

            std::vector<std::string> extra_headers;
            if (!body_params.empty() || method == "POST") {
                extra_headers.push_back("Content-Type: application/x-www-form-urlencoded");
            }

            // POST 请求用 body
            if (method == "POST" && !query_string.empty()) {
                // 覆盖 URL：POST 不带 query string
                std::string post_url = join_url(config_.get_base_url(), path);
                // 私有 POST 也需要在 body 里带签名参数
            }

            auto result = raw_request(method, url, extra_headers,
                                       is_private, is_private,
                                       response, status);

            if (result.is_err()) {
                last_error = result.error();

                // 网络错误判断是否可重试
                bool retryable = true;
                if (const auto* ctx = result.error().context.size() > 0 ?
                        &result.error() : nullptr) {
                    (void)ctx;
                }

                if (!retryable || attempt == max_attempts - 1) {
                    stats_.requests_failed.fetch_add(1,
                        std::memory_order_relaxed);
                    return QUANT_ERR_T(std::string, result.error().code)
                        .with_context("stage", "network")
                        .with_context("attempt", std::to_string(attempt + 1));
                }

                const auto delay = compute_backoff(attempt,
                    config_.retry_initial_delay, config_.retry_max_delay);

                QUANT_LOG_WARN("[REST] 请求失败，{}ms 后重试 (attempt={}): {}",
                    delay.count(), attempt + 1, result.error().to_string());

                std::this_thread::sleep_for(delay);
                continue;
            }

            // HTTP 状态判断
            if (status == 200) {
                // 解析业务错误码
                auto biz_error = check_business_error(response);
                if (biz_error.is_err()) {
                    stats_.requests_failed.fetch_add(1,
                        std::memory_order_relaxed);
                    return QUANT_ERR_T(std::string, biz_error.error().code)
                        .with_context("body", response.substr(0, 200));
                }

                stats_.requests_succeeded.fetch_add(1,
                    std::memory_order_relaxed);
                return response;
            }

            // 429 / 418：速率限制
            if (status == 429 || status == 418) {
                stats_.rate_limit_hits.fetch_add(1,
                    std::memory_order_relaxed);
                current_weight_.store(config_.rate_limit_block_threshold,
                    std::memory_order_release);

                if (attempt < max_attempts - 1) {
                    const auto delay = compute_backoff(attempt + 1,
                        config_.retry_initial_delay, config_.retry_max_delay);

                    QUANT_LOG_WARN("[REST] 触发限流（status={}），{}ms 后重试",
                        status, delay.count());

                    std::this_thread::sleep_for(delay);
                    continue;
                }

                stats_.requests_failed.fetch_add(1, std::memory_order_relaxed);
                return QUANT_ERR_T(std::string, ErrorCode::ExchangeRateLimit)
                    .with_context("status", std::to_string(status));
            }

            // 5xx：服务端错误
            if (status >= 500 && status < 600) {
                if (attempt < max_attempts - 1) {
                    const auto delay = compute_backoff(attempt + 1,
                        config_.retry_initial_delay, config_.retry_max_delay);

                    QUANT_LOG_WARN("[REST] 服务端错误（status={}），{}ms 后重试",
                        status, delay.count());

                    std::this_thread::sleep_for(delay);
                    continue;
                }

                stats_.requests_failed.fetch_add(1, std::memory_order_relaxed);
                return QUANT_ERR_T(std::string, ErrorCode::ExchangeMaintenance)
                    .with_context("status", std::to_string(status));
            }

            // 4xx：客户端错误，不重试
            stats_.requests_failed.fetch_add(1, std::memory_order_relaxed);
            return QUANT_ERR_T(std::string, ErrorCode::ExchangeApiError)
                .with_context("status", std::to_string(status))
                .with_context("body", response.substr(0, 200));
        }

        return QUANT_ERR_T(std::string, ErrorCode::NetworkUnknown)
            .with_context("reason", "重试耗尽")
            .with_context("last_error", last_error.to_string());
    }

    // =========================================================================
    // 业务错误码检查
    // =========================================================================
    Result<void> check_business_error(const std::string& response) {
        // 币安业务错误形如 {"code":-1121,"msg":"Invalid symbol"}
        // 注意：正常响应也可能是 {"code": 0}（如某些接口）
        if (response.empty() || response.front() != '{') return {};

        try {
            const auto j = json::parse(response);
            if (j.contains("code") && j["code"].is_number()) {
                const auto code = j["code"].get<int>();
                if (code < 0) {
                    const auto msg = json_string(j, "msg");
                    return QUANT_ERR_MSG(from_binance_error_code(code),
                        msg.empty() ? "币安业务错误" : msg)
                        .with_context("binance_code", std::to_string(code));
                }
            }
            return {};
        } catch (...) {
            // 解析失败不视为业务错误
            return {};
        }
    }

    // =========================================================================
    // 签名
    // =========================================================================
    Result<std::string> sign(std::string_view query_string) {
        if (config_.api_secret.empty()) {
            return QUANT_ERR_T(std::string, ErrorCode::ExchangeSignatureError)
                .with_context("reason", "缺少 API Secret");
        }

        const auto key_span = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(config_.api_secret.data()),
            config_.api_secret.size());
        const auto data_span = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(query_string.data()),
            query_string.size());

        auto mac = crypto::hmac_sha256(key_span, data_span);
        if (mac.is_err()) {
            return QUANT_ERR_T(std::string, mac.error().code)
                .with_context("openssl", mac.error().to_string());
        }

        return crypto::to_hex(std::span<const std::uint8_t>(
            mac->data(), mac->size()));
    }

    // =========================================================================
    // 时间同步线程
    // =========================================================================
    void time_sync_loop() {
        // 首次同步
        if (auto r = sync_time(); r.is_err()) {
            QUANT_LOG_WARN("[REST] 首次时间同步失败: {}",
                r.error().to_string());
        }

        while (!stopped_.load(std::memory_order_acquire)) {
            std::unique_lock lock(sync_mutex_);
            sync_cv_.wait_for(lock, config_.time_sync_interval, [this] {
                return stopped_.load(std::memory_order_acquire);
            });

            if (stopped_.load(std::memory_order_acquire)) break;

            lock.unlock();

            if (auto r = sync_time(); r.is_err()) {
                QUANT_LOG_WARN("[REST] 时间同步失败: {}",
                    r.error().to_string());
            }
        }
    }

    // =========================================================================
    // 公共接口实现
    // =========================================================================
    Result<std::vector<Candle>> get_klines(const KlineRequest& req) {
        if (req.symbol.empty()) {
            return QUANT_ERR_T(std::vector<Candle>, ErrorCode::UserInputInvalid)
                .with_context("reason", "symbol 为空");
        }
        if (req.limit == 0 || req.limit > binance_rest_config::kMaxKlinesPerRequest) {
            return QUANT_ERR_T(std::vector<Candle>, ErrorCode::UserInputOutOfRange)
                .with_context("limit", std::to_string(req.limit))
                .with_context("max",
                    std::to_string(binance_rest_config::kMaxKlinesPerRequest));
        }

        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", req.symbol);
        params.emplace_back("interval", req.interval);
        params.emplace_back("limit", std::to_string(req.limit));

        if (req.start_time) {
            params.emplace_back("startTime",
                std::to_string(req.start_time->milliseconds()));
        }
        if (req.end_time) {
            params.emplace_back("endTime",
                std::to_string(req.end_time->milliseconds()));
        }

        auto result = request_with_retry("GET", "/fapi/v1/klines",
            params, {}, false, 1);
        if (result.is_err()) {
            return QUANT_ERR_T(std::vector<Candle>, result.error().code)
                .with_context("symbol", req.symbol);
        }

        try {
            const auto j = json::parse(result.value());
            if (!j.is_array()) {
                return QUANT_ERR_T(std::vector<Candle>,
                    ErrorCode::ConfigParseFailed)
                    .with_context("reason", "K线响应非数组");
            }

            std::vector<Candle> candles;
            candles.reserve(j.size());

            for (const auto& item : j) {
                if (!item.is_array() || item.size() < 11) continue;

                Candle c{};
                // [openTime, open, high, low, close, volume,
                //  closeTime, quoteVolume, trades, ...]
                c.open_time = Timestamp{
                    static_cast<std::int64_t>(item[0].get<double>()) * 1000LL};
                c.open  = Price::from_double(std::stod(item[1].get<std::string>()));
                c.high  = Price::from_double(std::stod(item[2].get<std::string>()));
                c.low   = Price::from_double(std::stod(item[3].get<std::string>()));
                c.close = Price::from_double(std::stod(item[4].get<std::string>()));
                c.volume = Quantity::from_double(std::stod(item[5].get<std::string>()));
                c.close_time = Timestamp{
                    static_cast<std::int64_t>(item[6].get<double>()) * 1000LL};
                c.quote_volume = Quantity::from_double(
                    std::stod(item[7].get<std::string>()));
                c.trades = static_cast<std::uint32_t>(item[8].get<int>());
                c.is_closed = true;   // REST 返回的历史 K 线默认已收盘

                candles.push_back(std::move(c));
            }

            return candles;
        } catch (const std::exception& e) {
            return QUANT_ERR_T(std::vector<Candle>,
                ErrorCode::ConfigParseFailed)
                .with_context("error", e.what());
        }
    }

    Result<std::unordered_map<std::string, std::vector<Candle>>>
        get_klines_batch(std::span<const KlineRequest> requests)
    {
        std::unordered_map<std::string, std::vector<Candle>> result;
        result.reserve(requests.size());

        for (const auto& req : requests) {
            auto r = get_klines(req);
            if (r.is_ok()) {
                result.emplace(req.symbol, std::move(r.value()));
            } else {
                QUANT_LOG_WARN("[REST] K线批量查询失败: {} ({})",
                    req.symbol, r.error().to_string());
            }
        }

        return result;
    }

    Result<std::pair<std::vector<std::pair<double, double>>,
                     std::vector<std::pair<double, double>>>>
        get_depth(std::string_view symbol, std::size_t limit) {
        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", std::string{symbol});
        params.emplace_back("limit", std::to_string(limit));

        auto result = request_with_retry("GET", "/fapi/v1/depth",
            params, {}, false, 5);
        if (result.is_err()) {
            return QUANT_ERR_T(
                (std::pair<std::vector<std::pair<double, double>>,
                           std::vector<std::pair<double, double>>>),
                result.error().code);
        }

        try {
            const auto j = json::parse(result.value());

            std::vector<std::pair<double, double>> bids;
            std::vector<std::pair<double, double>> asks;

            if (j.contains("bids")) {
                for (const auto& b : j["bids"]) {
                    if (b.is_array() && b.size() >= 2) {
                        bids.emplace_back(
                            std::stod(b[0].get<std::string>()),
                            std::stod(b[1].get<std::string>()));
                    }
                }
            }
            if (j.contains("asks")) {
                for (const auto& a : j["asks"]) {
                    if (a.is_array() && a.size() >= 2) {
                        asks.emplace_back(
                            std::stod(a[0].get<std::string>()),
                            std::stod(a[1].get<std::string>()));
                    }
                }
            }

            return std::make_pair(std::move(bids), std::move(asks));
        } catch (const std::exception& e) {
            return QUANT_ERR_T(
                (std::pair<std::vector<std::pair<double, double>>,
                           std::vector<std::pair<double, double>>>),
                ErrorCode::ConfigParseFailed)
                .with_context("error", e.what());
        }
    }

    Result<std::vector<RecentTrade>> get_recent_trades(
        std::string_view symbol, std::size_t limit)
    {
        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", std::string{symbol});
        params.emplace_back("limit", std::to_string(limit));

        auto result = request_with_retry("GET", "/fapi/v1/trades",
            params, {}, false, 5);
        if (result.is_err()) {
            return QUANT_ERR_T(std::vector<RecentTrade>, result.error().code);
        }

        try {
            const auto j = json::parse(result.value());
            std::vector<RecentTrade> trades;
            if (j.is_array()) {
                trades.reserve(j.size());
                for (const auto& item : j) {
                    RecentTrade t;
                    t.id = static_cast<std::uint64_t>(json_int64(item, "id"));
                    t.price = json_double(item, "price");
                    t.qty = json_double(item, "qty");
                    t.is_buyer_maker = json_bool(item, "isBuyerMaker");
                    t.time = Timestamp{json_int64(item, "time") * 1000LL};
                    trades.push_back(std::move(t));
                }
            }
            return trades;
        } catch (const std::exception& e) {
            return QUANT_ERR_T(std::vector<RecentTrade>,
                ErrorCode::ConfigParseFailed)
                .with_context("error", e.what());
        }
    }

    Result<FundingRate> get_funding_rate(std::string_view symbol) {
        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", std::string{symbol});

        auto result = request_with_retry("GET", "/fapi/v1/premiumIndex",
            params, {}, false, 1);
        if (result.is_err()) {
            return QUANT_ERR_T(FundingRate, result.error().code);
        }

        try {
            const auto j = json::parse(result.value());
            FundingRate fr;
            fr.symbol = json_string(j, "symbol");
            fr.funding_rate = json_double(j, "lastFundingRate");
            fr.funding_time_us = json_int64(j, "nextFundingTime") * 1000LL;
            return fr;
        } catch (const std::exception& e) {
            return QUANT_ERR_T(FundingRate, ErrorCode::ConfigParseFailed)
                .with_context("error", e.what());
        }
    }

    Result<ExchangeInfo> get_exchange_info() {
        auto result = request_with_retry("GET", "/fapi/v1/exchangeInfo",
            {}, {}, false, 1);
        if (result.is_err()) {
            return QUANT_ERR_T(ExchangeInfo, result.error().code);
        }

        try {
            const auto j = json::parse(result.value());
            ExchangeInfo info;
            info.server_time_us = json_int64(j, "serverTime") * 1000LL;

            if (j.contains("symbols") && j["symbols"].is_array()) {
                for (const auto& s : j["symbols"]) {
                    ExchangeInfo::Symbol sym;
                    sym.symbol = json_string(s, "symbol");
                    sym.base_asset = json_string(s, "baseAsset");
                    sym.quote_asset = json_string(s, "quoteAsset");
                    sym.price_precision = json_double(s, "pricePrecision");
                    sym.quantity_precision = json_double(s, "quantityPrecision");

                    if (s.contains("filters") && s["filters"].is_array()) {
                        for (const auto& f : s["filters"]) {
                            const auto type = json_string(f, "filterType");
                            if (type == "PRICE_FILTER") {
                                sym.tick_size = json_double(f, "tickSize");
                            } else if (type == "LOT_SIZE") {
                                sym.min_qty = json_double(f, "minQty");
                                sym.max_qty = json_double(f, "maxQty");
                                sym.step_size = json_double(f, "stepSize");
                            } else if (type == "MIN_NOTIONAL") {
                                sym.min_notional = json_double(f, "notional");
                            }
                        }
                    }
                    info.symbols.push_back(std::move(sym));
                }
            }

            return info;
        } catch (const std::exception& e) {
            return QUANT_ERR_T(ExchangeInfo, ErrorCode::ConfigParseFailed)
                .with_context("error", e.what());
        }
    }

    Result<ExchangeInfo::Symbol> get_symbol_info(std::string_view symbol) {
        auto info = get_exchange_info();
        if (info.is_err()) {
            return QUANT_ERR_T(ExchangeInfo::Symbol, info.error().code);
        }

        for (const auto& s : info.value().symbols) {
            if (s.symbol == symbol) {
                return s;
            }
        }

        return QUANT_ERR_T(ExchangeInfo::Symbol,
            ErrorCode::ExchangeInvalidSymbol)
            .with_context("symbol", std::string{symbol});
    }

    // =========================================================================
    // 私有接口
    // =========================================================================
    Result<AccountInfo> get_account_info() {
        auto result = request_with_retry("GET", "/fapi/v2/account",
            {}, {}, true, 5);
        if (result.is_err()) {
            return QUANT_ERR_T(AccountInfo, result.error().code);
        }

        try {
            const auto j = json::parse(result.value());
            AccountInfo acc;
            acc.total_wallet_balance = json_double(j, "totalWalletBalance");
            acc.total_unrealized_profit = json_double(j, "totalUnrealizedProfit");
            acc.total_margin_balance = json_double(j, "totalMarginBalance");
            acc.total_initial_margin = json_double(j, "totalInitialMargin");
            acc.total_maint_margin = json_double(j, "totalMaintMargin");
            acc.available_balance = json_double(j, "availableBalance");
            acc.max_withdraw_amount = json_double(j, "maxWithdrawAmount");

            if (j.contains("assets") && j["assets"].is_array()) {
                for (const auto& a : j["assets"]) {
                    AccountInfo::Asset asset;
                    asset.asset = json_string(a, "asset");
                    asset.wallet_balance = json_double(a, "walletBalance");
                    asset.available_balance = json_double(a, "availableBalance");
                    asset.unrealized_profit = json_double(a, "unrealizedProfit");
                    asset.margin_balance = json_double(a, "marginBalance");
                    acc.assets.push_back(std::move(asset));
                }
            }

            if (j.contains("positions") && j["positions"].is_array()) {
                for (const auto& p : j["positions"]) {
                    AccountInfo::Position pos;
                    pos.symbol = json_string(p, "symbol");
                    pos.position_amount = json_double(p, "positionAmt");
                    pos.entry_price = json_double(p, "entryPrice");
                    pos.mark_price = json_double(p, "markPrice");
                    pos.unrealized_profit = json_double(p, "unrealizedProfit");
                    pos.liquidation_price = json_double(p, "liquidationPrice");
                    pos.isolated_margin = json_double(p, "isolatedMargin");
                    pos.leverage = static_cast<int>(json_int64(p, "leverage"));
                    pos.margin_type = json_string(p, "marginType");
                    pos.position_side = json_string(p, "positionSide");
                    acc.positions.push_back(std::move(pos));
                }
            }

            return acc;
        } catch (const std::exception& e) {
            return QUANT_ERR_T(AccountInfo, ErrorCode::ConfigParseFailed)
                .with_context("error", e.what());
        }
    }

    Result<std::vector<PositionRisk>> get_position_risk(
        std::optional<std::string_view> symbol)
    {
        std::vector<std::pair<std::string, std::string>> params;
        if (symbol) {
            params.emplace_back("symbol", std::string{*symbol});
        }

        auto result = request_with_retry("GET", "/fapi/v2/positionRisk",
            params, {}, true, 5);
        if (result.is_err()) {
            return QUANT_ERR_T(std::vector<PositionRisk>, result.error().code);
        }

        try {
            const auto j = json::parse(result.value());
            std::vector<PositionRisk> positions;

            if (j.is_array()) {
                positions.reserve(j.size());
                for (const auto& p : j) {
                    PositionRisk pr;
                    pr.symbol = json_string(p, "symbol");
                    pr.position_amount = json_double(p, "positionAmt");
                    pr.entry_price = json_double(p, "entryPrice");
                    pr.mark_price = json_double(p, "markPrice");
                    pr.unrealized_profit = json_double(p, "unRealizedProfit");
                    pr.liquidation_price = json_double(p, "liquidationPrice");
                    pr.isolated_margin = json_double(p, "isolatedMargin");
                    pr.leverage = static_cast<int>(json_int64(p, "leverage"));
                    pr.margin_type = json_string(p, "marginType");
                    pr.position_side = json_string(p, "positionSide");
                    pr.notional = json_double(p, "notional");
                    pr.initial_margin = json_double(p, "initialMargin");
                    pr.maint_margin = json_double(p, "maintMargin");
                    positions.push_back(std::move(pr));
                }
            }

            return positions;
        } catch (const std::exception& e) {
            return QUANT_ERR_T(std::vector<PositionRisk>,
                ErrorCode::ConfigParseFailed)
                .with_context("error", e.what());
        }
    }

    Result<OrderResponse> place_order(const OrderRequest& req) {
        if (req.symbol.empty()) {
            return QUANT_ERR_T(OrderResponse, ErrorCode::UserInputInvalid)
                .with_context("field", "symbol");
        }

        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", req.symbol);

        // Side
        switch (req.side) {
            case Side::BUY:  params.emplace_back("side", "BUY"); break;
            case Side::SELL: params.emplace_back("side", "SELL"); break;
            default:
                return QUANT_ERR_T(OrderResponse, ErrorCode::UserInputInvalid)
                    .with_context("field", "side");
        }

        // Type
        auto type_str = std::string{};
        switch (req.type) {
            case OrderType::MARKET:      type_str = "MARKET"; break;
            case OrderType::LIMIT:       type_str = "LIMIT"; break;
            case OrderType::STOP_MARKET: type_str = "STOP_MARKET"; break;
            case OrderType::STOP_LIMIT:  type_str = "STOP"; break;
            case OrderType::TAKE_PROFIT: type_str = "TAKE_PROFIT_MARKET"; break;
            case OrderType::TRAILING:    type_str = "TRAILING_STOP_MARKET"; break;
            default:
                return QUANT_ERR_T(OrderResponse, ErrorCode::UserInputInvalid)
                    .with_context("field", "type");
        }
        params.emplace_back("type", type_str);

        // TimeInForce
        if (req.type == OrderType::LIMIT) {
            switch (req.time_in_force) {
                case TimeInForce::GTC: params.emplace_back("timeInForce", "GTC"); break;
                case TimeInForce::IOC: params.emplace_back("timeInForce", "IOC"); break;
                case TimeInForce::FOK: params.emplace_back("timeInForce", "FOK"); break;
                case TimeInForce::GTD: params.emplace_back("timeInForce", "GTD"); break;
                default: break;
            }
        }

        if (req.quantity) {
            params.emplace_back("quantity", format_double(*req.quantity));
        }
        if (req.quote_order_qty) {
            params.emplace_back("quoteOrderQty",
                format_double(*req.quote_order_qty));
        }
        if (req.price) {
            params.emplace_back("price", format_double(*req.price));
        }
        if (req.stop_price) {
            params.emplace_back("stopPrice", format_double(*req.stop_price));
        }
        if (req.callback_rate) {
            params.emplace_back("callbackRate",
                format_double(*req.callback_rate));
        }
        if (req.reduce_only) {
            params.emplace_back("reduceOnly",
                *req.reduce_only ? "true" : "false");
        }
        if (req.post_only) {
            params.emplace_back("postOnly",
                *req.post_only ? "true" : "false");
        }
        if (req.client_order_id) {
            params.emplace_back("newClientOrderId", *req.client_order_id);
        }

        auto result = request_with_retry("POST", "/fapi/v1/order",
            {}, params, true, 1);
        if (result.is_err()) {
            return QUANT_ERR_T(OrderResponse, result.error().code);
        }

        try {
            const auto j = json::parse(result.value());
            OrderResponse resp;
            resp.order_id = static_cast<std::uint64_t>(json_int64(j, "orderId"));
            resp.client_order_id = json_string(j, "clientOrderId");
            resp.symbol = json_string(j, "symbol");
            resp.transact_time_us = json_int64(j, "transactTime") * 1000LL;
            resp.price = json_double(j, "price");
            resp.orig_qty = json_double(j, "origQty");
            resp.executed_qty = json_double(j, "executedQty");
            resp.avg_price = json_double(j, "avgPrice");
            resp.status = json_string(j, "status");
            resp.type = json_string(j, "type");
            resp.side = json_string(j, "side");
            resp.time_in_force = json_string(j, "timeInForce");
            resp.reduce_only = json_bool(j, "reduceOnly");
            resp.post_only = json_bool(j, "postOnly");
            return resp;
        } catch (const std::exception& e) {
            return QUANT_ERR_T(OrderResponse, ErrorCode::ConfigParseFailed)
                .with_context("error", e.what());
        }
    }

    Result<void> cancel_order(std::string_view symbol, std::uint64_t order_id) {
        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", std::string{symbol});
        params.emplace_back("orderId", std::to_string(order_id));

        auto result = request_with_retry("DELETE", "/fapi/v1/order",
            params, {}, true, 1);
        if (result.is_err()) {
            return QUANT_ERR(result.error().code);
        }
        return {};
    }

    Result<void> cancel_all_orders(std::string_view symbol) {
        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", std::string{symbol});

        auto result = request_with_retry("DELETE", "/fapi/v1/allOpenOrders",
            params, {}, true, 1);
        if (result.is_err()) {
            return QUANT_ERR(result.error().code);
        }
        return {};
    }

    Result<OrderResponse> get_order(std::string_view symbol,
                                     std::uint64_t order_id)
    {
        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", std::string{symbol});
        params.emplace_back("orderId", std::to_string(order_id));

        auto result = request_with_retry("GET", "/fapi/v1/order",
            params, {}, true, 1);
        if (result.is_err()) {
            return QUANT_ERR_T(OrderResponse, result.error().code);
        }

        try {
            const auto j = json::parse(result.value());
            OrderResponse resp;
            resp.order_id = static_cast<std::uint64_t>(json_int64(j, "orderId"));
            resp.client_order_id = json_string(j, "clientOrderId");
            resp.symbol = json_string(j, "symbol");
            resp.price = json_double(j, "price");
            resp.orig_qty = json_double(j, "origQty");
            resp.executed_qty = json_double(j, "executedQty");
            resp.avg_price = json_double(j, "avgPrice");
            resp.status = json_string(j, "status");
            resp.type = json_string(j, "type");
            resp.side = json_string(j, "side");
            resp.time_in_force = json_string(j, "timeInForce");
            return resp;
        } catch (const std::exception& e) {
            return QUANT_ERR_T(OrderResponse, ErrorCode::ConfigParseFailed)
                .with_context("error", e.what());
        }
    }

    Result<std::vector<OrderResponse>> get_open_orders(
        std::optional<std::string_view> symbol)
    {
        std::vector<std::pair<std::string, std::string>> params;
        if (symbol) {
            params.emplace_back("symbol", std::string{*symbol});
        }

        auto result = request_with_retry("GET", "/fapi/v1/openOrders",
            params, {}, true, 40);
        if (result.is_err()) {
            return QUANT_ERR_T(std::vector<OrderResponse>, result.error().code);
        }

        try {
            const auto j = json::parse(result.value());
            std::vector<OrderResponse> orders;
            if (j.is_array()) {
                orders.reserve(j.size());
                for (const auto& item : j) {
                    OrderResponse resp;
                    resp.order_id = static_cast<std::uint64_t>(
                        json_int64(item, "orderId"));
                    resp.client_order_id = json_string(item, "clientOrderId");
                    resp.symbol = json_string(item, "symbol");
                    resp.price = json_double(item, "price");
                    resp.orig_qty = json_double(item, "origQty");
                    resp.executed_qty = json_double(item, "executedQty");
                    resp.avg_price = json_double(item, "avgPrice");
                    resp.status = json_string(item, "status");
                    resp.type = json_string(item, "type");
                    resp.side = json_string(item, "side");
                    resp.time_in_force = json_string(item, "timeInForce");
                    orders.push_back(std::move(resp));
                }
            }
            return orders;
        } catch (const std::exception& e) {
            return QUANT_ERR_T(std::vector<OrderResponse>,
                ErrorCode::ConfigParseFailed)
                .with_context("error", e.what());
        }
    }

    Result<void> set_leverage(std::string_view symbol, int leverage) {
        if (leverage < 1 || leverage > 125) {
            return QUANT_ERR_MSG(ErrorCode::UserInputOutOfRange,
                "杠杆超出范围 [1, 125]")
                .with_context("leverage", std::to_string(leverage));
        }

        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", std::string{symbol});
        params.emplace_back("leverage", std::to_string(leverage));

        auto result = request_with_retry("POST", "/fapi/v1/leverage",
            {}, params, true, 1);
        if (result.is_err()) {
            return QUANT_ERR(result.error().code);
        }
        return {};
    }

    Result<void> set_margin_type(std::string_view symbol,
                                   std::string_view margin_type)
    {
        if (margin_type != "ISOLATED" && margin_type != "CROSSED") {
            return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
                "margin_type 必须为 ISOLATED 或 CROSSED");
        }

        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", std::string{symbol});
        params.emplace_back("marginType", std::string{margin_type});

        auto result = request_with_retry("POST", "/fapi/v1/marginType",
            {}, params, true, 1);
        if (result.is_err()) {
            return QUANT_ERR(result.error().code);
        }
        return {};
    }

    // =========================================================================
    // 诊断
    // =========================================================================
    std::string dump() const {
        std::ostringstream oss;
        oss << "BinanceRest Dump:\n";
        oss << "  base_url:          " << config_.get_base_url() << "\n";
        oss << "  testnet:           " << config_.use_testnet << "\n";
        oss << "  initialized:       " << initialized_.load() << "\n";
        oss << "  time_offset_us:    " << time_offset_us() << "\n";
        oss << "  current_weight:    " << current_weight() << "\n";
        oss << "  weight_limit:      "
            << config_.rate_limit_weight_per_minute << "\n";
        oss << "  requests_total:    "
            << stats_.requests_total.load() << "\n";
        oss << "  requests_succeeded:"
            << stats_.requests_succeeded.load() << "\n";
        oss << "  requests_failed:   "
            << stats_.requests_failed.load() << "\n";
        oss << "  requests_retried:  "
            << stats_.requests_retried.load() << "\n";
        oss << "  rate_limit_hits:   "
            << stats_.rate_limit_hits.load() << "\n";
        oss << "  success_rate:      "
            << (stats_.success_rate() * 100.0) << "%\n";
        oss << "  avg_latency_ms:    "
            << stats_.avg_latency_ms() << "\n";
        oss << "  max_latency_us:    "
            << stats_.max_latency_us.load() << "\n";
        return oss.str();
    }

    // 暴露统计
    const RestStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_.reset(); }

private:
    BinanceRestConfig config_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<std::int64_t> time_offset_us_{0};
    std::atomic<std::size_t> current_weight_{0};

    RestStats stats_;

    // 时间同步
    std::thread sync_thread_;
    std::mutex sync_mutex_;
    std::condition_variable sync_cv_;

    // 速率限制
    std::mutex rate_limit_mutex_;
    std::condition_variable rate_limit_cv_;
};

// ==============================================================================
// BinanceRest 转发
// ==============================================================================
BinanceRest::BinanceRest(const BinanceRestConfig& config)
    : impl_{std::make_unique<Impl>(config)}
{}

BinanceRest::~BinanceRest() = default;

Result<void> BinanceRest::initialize() {
    return impl_->initialize();
}

void BinanceRest::shutdown() noexcept {
    impl_->shutdown();
}

Result<void> BinanceRest::sync_time() {
    return impl_->sync_time();
}

Timestamp BinanceRest::server_time() const noexcept {
    return impl_->server_time();
}

std::int64_t BinanceRest::time_offset_us() const noexcept {
    return impl_->time_offset_us();
}

Result<std::vector<Candle>>
BinanceRest::get_klines(const KlineRequest& request) {
    return impl_->get_klines(request);
}

Result<std::unordered_map<std::string, std::vector<Candle>>>
BinanceRest::get_klines_batch(std::span<const KlineRequest> requests) {
    return impl_->get_klines_batch(requests);
}

Result<std::pair<std::vector<std::pair<double, double>>,
                 std::vector<std::pair<double, double>>>>
BinanceRest::get_depth(std::string_view symbol, std::size_t limit) {
    return impl_->get_depth(symbol, limit);
}

Result<std::vector<BinanceRest::RecentTrade>>
BinanceRest::get_recent_trades(std::string_view symbol, std::size_t limit) {
    return impl_->get_recent_trades(symbol, limit);
}

Result<FundingRate>
BinanceRest::get_funding_rate(std::string_view symbol) {
    return impl_->get_funding_rate(symbol);
}

Result<ExchangeInfo> BinanceRest::get_exchange_info() {
    return impl_->get_exchange_info();
}

Result<ExchangeInfo::Symbol>
BinanceRest::get_symbol_info(std::string_view symbol) {
    return impl_->get_symbol_info(symbol);
}

Result<AccountInfo> BinanceRest::get_account_info() {
    return impl_->get_account_info();
}

Result<std::vector<PositionRisk>>
BinanceRest::get_position_risk(std::optional<std::string_view> symbol) {
    return impl_->get_position_risk(symbol);
}

Result<OrderResponse>
BinanceRest::place_order(const OrderRequest& request) {
    return impl_->place_order(request);
}

Result<void> BinanceRest::cancel_order(std::string_view symbol,
                                        std::uint64_t order_id) {
    return impl_->cancel_order(symbol, order_id);
}

Result<void> BinanceRest::cancel_all_orders(std::string_view symbol) {
    return impl_->cancel_all_orders(symbol);
}

Result<OrderResponse>
BinanceRest::get_order(std::string_view symbol, std::uint64_t order_id) {
    return impl_->get_order(symbol, order_id);
}

Result<std::vector<OrderResponse>>
BinanceRest::get_open_orders(std::optional<std::string_view> symbol) {
    return impl_->get_open_orders(symbol);
}

Result<void> BinanceRest::set_leverage(std::string_view symbol, int leverage) {
    return impl_->set_leverage(symbol, leverage);
}

Result<void> BinanceRest::set_margin_type(std::string_view symbol,
                                            std::string_view margin_type) {
    return impl_->set_margin_type(symbol, margin_type);
}

const RestStats& BinanceRest::stats() const noexcept {
    return impl_->stats();
}

void BinanceRest::reset_stats() noexcept {
    impl_->reset_stats();
}

std::size_t BinanceRest::current_weight() const noexcept {
    return impl_->current_weight();
}

bool BinanceRest::is_rate_limited() const noexcept {
    return impl_->is_rate_limited();
}

std::string BinanceRest::dump() const {
    return impl_->dump();
}

}  // namespace data
}  // namespace quant
