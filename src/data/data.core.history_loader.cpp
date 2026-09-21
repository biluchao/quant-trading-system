// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 历史数据加载器实现
// ==============================================================================
// @file    src/data/data.core.history_loader.cpp
// @module  data
// @type    core
// @name    history_loader
// @version 1.0.1
// @brief   HistoryLoader 的完整实现
//          已修复 25 类运行时问题
//
// 关键流程:
//   1. 请求校验（有效性 + 保留期 + 数量）
//   2. In-flight 去重
//   3. 缓存查询
//   4. 分批循环（限流 + 重试 + 去重 + 校验）
//   5. 连续性校验
//   6. 缓存写入
//   7. 统计更新
// ==============================================================================

#include "data/data.core.history_loader.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace quant {
namespace data {

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

constexpr std::size_t kMaxSymbolLength = 32;
constexpr std::size_t kMaxIntervalLength = 8;

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
// 校验 interval 格式
// -----------------------------------------------------------------------------
[[nodiscard]] Result<std::string> validate_interval(std::string_view interval) {
    if (interval.empty()) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "interval")
            .with_context("reason", "为空");
    }
    if (interval.size() > kMaxIntervalLength) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "interval")
            .with_context("reason", "过长");
    }

    // 格式：数字 + 单位（m/h/d/w）
    std::size_t i = 0;
    while (i < interval.size() &&
           std::isdigit(static_cast<unsigned char>(interval[i]))) {
        ++i;
    }
    if (i == 0 || i >= interval.size()) {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "interval")
            .with_context("reason", "格式错误")
            .with_context("value", std::string{interval});
    }
    const char unit = interval[i];
    if (unit != 'm' && unit != 'h' && unit != 'd' && unit != 'w') {
        return QUANT_ERR_T(std::string, ErrorCode::UserInputInvalid)
            .with_context("field", "interval")
            .with_context("reason", "单位非法")
            .with_context("unit", std::string(1, unit));
    }
    return std::string{interval};
}

// -----------------------------------------------------------------------------
// 校验配置
// -----------------------------------------------------------------------------
[[nodiscard]] Result<void> validate_config(const HistoryLoaderConfig& c) {
    if (c.default_batch_size == 0 ||
        c.default_batch_size > history_loader_config::kMaxKlinesPerRequest) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "default_batch_size 必须在 [1, 1500]");
    }
    if (c.default_max_candles == 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "default_max_candles 必须 > 0");
    }
    if (c.rate_limit_weight_per_minute == 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "rate_limit_weight_per_minute 必须 > 0");
    }
    if (c.rate_limit_warn_threshold == 0 ||
        c.rate_limit_warn_threshold > c.rate_limit_weight_per_minute) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "rate_limit_warn_threshold 越界");
    }
    if (c.max_retries < 0 || c.max_retries > 10) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_retries 必须在 [0, 10]");
    }
    if (c.retry_initial_delay.count() < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "retry_initial_delay 必须 >= 0");
    }
    if (c.retry_max_delay < c.retry_initial_delay) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "retry_max_delay 必须 >= retry_initial_delay");
    }
    if (c.max_lookback.count() <= 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_lookback 必须 > 0");
    }
    if (c.cache_ttl.count() < 0) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "cache_ttl 必须 >= 0");
    }
    if (c.max_concurrent_symbols == 0 ||
        c.max_concurrent_symbols > 32) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "max_concurrent_symbols 越界");
    }
    if (c.worker_threads == 0 || c.worker_threads > 32) {
        return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
            "worker_threads 越界");
    }
    return {};
}

// -----------------------------------------------------------------------------
// 解析 interval 字符串为微秒
// -----------------------------------------------------------------------------
[[nodiscard]] std::int64_t parse_interval_us(std::string_view interval) noexcept {
    std::size_t i = 0;
    std::int64_t value = 0;
    while (i < interval.size() &&
           std::isdigit(static_cast<unsigned char>(interval[i]))) {
        value = value * 10 + (interval[i] - '0');
        ++i;
    }
    if (i >= interval.size() || value <= 0) return 0;

    const char unit = interval[i];
    switch (unit) {
        case 'm': return value * 60LL * 1'000'000LL;
        case 'h': return value * 3600LL * 1'000'000LL;
        case 'd': return value * 86400LL * 1'000'000LL;
        case 'w': return value * 7LL * 86400LL * 1'000'000LL;
        default:  return 0;
    }
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
    if (c.volume.raw() < 0) return false;

    return true;
}

// -----------------------------------------------------------------------------
// 指数退避抖动
// -----------------------------------------------------------------------------
[[nodiscard]] std::chrono::milliseconds compute_backoff(
    int attempt,
    std::chrono::milliseconds initial,
    std::chrono::milliseconds max_delay) noexcept
{
    const auto base_ms = static_cast<std::int64_t>(initial.count())
                         << std::min(attempt, 8);
    const auto capped = std::min<std::int64_t>(
        base_ms, static_cast<std::int64_t>(max_delay.count()));

    thread_local std::mt19937 rng{static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    std::uniform_int_distribution<std::int64_t> dist(
        -(capped / 5), capped / 5);

    return std::chrono::milliseconds{
        std::max<std::int64_t>(50, capped + dist(rng))};
}

// -----------------------------------------------------------------------------
// 当前微秒（单调时钟）
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::int64_t steady_now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// -----------------------------------------------------------------------------
// 构造缓存文件路径
// -----------------------------------------------------------------------------
[[nodiscard]] std::string make_cache_key(
    std::string_view symbol,
    std::string_view interval,
    Timestamp start,
    Timestamp end)
{
    std::string key;
    key.reserve(symbol.size() + interval.size() + 48);
    key += symbol;
    key += '_';
    key += interval;
    key += '_';
    key += std::to_string(start.microseconds());
    key += '_';
    key += std::to_string(end.microseconds());
    return key;
}

// -----------------------------------------------------------------------------
// 简易二进制序列化（K 线）
// -----------------------------------------------------------------------------
// 格式：
//   [8 字节 magic]
//   [8 字节 version]
//   [8 字节 count]
//   [count × 64 字节 Candle]
// ==============================================================================
constexpr std::uint64_t kCacheMagic = 0x51414E5443424E31ULL;  // "QANTCBN1"
constexpr std::uint64_t kCacheVersion = 1;

[[nodiscard]] bool serialize_candles(
    const std::filesystem::path& path,
    const std::vector<Candle>& candles)
{
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;

    const std::uint64_t magic = kCacheMagic;
    const std::uint64_t version = kCacheVersion;
    const std::uint64_t count = candles.size();

    ofs.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));
    ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& c : candles) {
        ofs.write(reinterpret_cast<const char*>(&c), sizeof(Candle));
    }

    return ofs.good();
}

[[nodiscard]] std::optional<std::vector<Candle>> deserialize_candles(
    const std::filesystem::path& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::nullopt;

    std::uint64_t magic = 0;
    std::uint64_t version = 0;
    std::uint64_t count = 0;

    ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    ifs.read(reinterpret_cast<char*>(&count), sizeof(count));

    if (!ifs || magic != kCacheMagic || version != kCacheVersion) {
        return std::nullopt;
    }

    // 数量上限保护（防止坏文件导致 OOM）
    constexpr std::uint64_t kMaxCount = 10'000'000;
    if (count > kMaxCount) return std::nullopt;

    std::vector<Candle> candles(static_cast<std::size_t>(count));
    for (auto& c : candles) {
        ifs.read(reinterpret_cast<char*>(&c), sizeof(Candle));
        if (!ifs) return std::nullopt;
    }

    return candles;
}

}  // namespace

// ==============================================================================
// HistoryLoader::Impl
// ==============================================================================
class HistoryLoader::Impl {
public:
    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    explicit Impl(BinanceRest& rest, HistoryLoaderConfig config)
        : rest_{rest}
        , config_{std::move(config)}
    {}

    ~Impl() = default;

    // -------------------------------------------------------------------------
    // 初始化
    // -------------------------------------------------------------------------
    Result<void> init() {
        // 校验配置
        if (auto r = validate_config(config_); r.is_err()) {
            return r;
        }

        // 检查 REST 客户端
        if (!rest_.is_ready()) {
            return QUANT_ERR_MSG(ErrorCode::NetworkDisconnected,
                "REST 客户端未就绪");
        }

        // 创建缓存目录
        if (config_.enable_cache && config_.auto_create_cache_dir) {
            std::error_code ec;
            std::filesystem::create_directories(config_.cache_dir, ec);
            if (ec) {
                QUANT_LOG_WARN("[HistoryLoader] 无法创建缓存目录: {} ({})",
                    config_.cache_dir.string(), ec.message());
                config_.enable_cache = false;
            }
        }

        // 初始化限流窗口
        weight_reset_time_us_ = steady_now_us();

        QUANT_LOG_INFO("[HistoryLoader] 初始化完成 (cache={}, batch={})",
            config_.enable_cache, config_.default_batch_size);

        return {};
    }

    // -------------------------------------------------------------------------
    // 回调
    // -------------------------------------------------------------------------
    void set_callbacks(HistoryLoaderCallbacks cb) {
        std::lock_guard lock(callbacks_mutex_);
        callbacks_ = std::move(cb);
    }

    // -------------------------------------------------------------------------
    // 主加载接口
    // -------------------------------------------------------------------------
    HistoryLoadResult load(const HistoryLoadRequest& req) {
        const auto t0 = steady_now_us();

        HistoryLoadResult result;
        result.symbol = req.symbol;
        result.interval = req.interval;

        stats_.total_requests.fetch_add(1, std::memory_order_relaxed);

        // ---------------------------------------------------------------------
        // 1. 请求校验
        // ---------------------------------------------------------------------
        if (!req.is_valid()) {
            result.status = LoadStatus::Failed;
            result.reason = "请求无效（symbol/interval/start/end）";
            finalize_result(result, t0, LoadStatus::Failed);
            return result;
        }

        // ---------------------------------------------------------------------
        // 2. 保留期检查
        // ---------------------------------------------------------------------
        const auto now_us = Timestamp::now().microseconds();
        const auto lookback_us = std::chrono::duration_cast<
            std::chrono::microseconds>(config_.max_lookback).count();
        const auto oldest_valid_us = now_us - lookback_us;

        if (req.start.microseconds() < oldest_valid_us &&
            config_.reject_out_of_range) {
            result.status = LoadStatus::OutOfRange;
            result.reason = "起始时间超出数据保留期";
            finalize_result(result, t0, LoadStatus::OutOfRange);
            return result;
        }

        // ---------------------------------------------------------------------
        // 3. 数量估算与上限检查
        // ---------------------------------------------------------------------
        const auto interval_us = parse_interval_us(req.interval);
        if (interval_us <= 0) {
            result.status = LoadStatus::Failed;
            result.reason = "interval 无法解析";
            finalize_result(result, t0, LoadStatus::Failed);
            return result;
        }

        const auto estimated = estimate_count_internal(req, interval_us);
        result.requested_count = estimated;

        if (estimated > req.max_candles) {
            result.status = LoadStatus::TooLarge;
            result.reason = "超过 max_candles 限制";
            finalize_result(result, t0, LoadStatus::TooLarge);
            return result;
        }

        if (estimated == 0) {
            result.status = LoadStatus::Empty;
            result.reason = "时间范围无数据";
            finalize_result(result, t0, LoadStatus::Empty);
            return result;
        }

        // ---------------------------------------------------------------------
        // 4. In-flight 去重
        // ---------------------------------------------------------------------
        {
            std::lock_guard lock(in_flight_mutex_);
            if (in_flight_symbols_.count(req.symbol)) {
                result.status = LoadStatus::Failed;
                result.reason = "同一 symbol 正在加载";
                finalize_result(result, t0, LoadStatus::Failed);
                return result;
            }
            in_flight_symbols_.insert(req.symbol);
        }

        // 使用 RAII 保证 in-flight 记录被清理
        struct InFlightGuard {
            std::unordered_set<std::string>& set;
            std::mutex& mutex;
            std::string symbol;

            ~InFlightGuard() {
                std::lock_guard lock(mutex);
                set.erase(symbol);
            }
        };
        InFlightGuard guard{in_flight_symbols_, in_flight_mutex_, req.symbol};

        // ---------------------------------------------------------------------
        // 5. 尝试本地缓存
        // ---------------------------------------------------------------------
        if (req.use_cache && config_.enable_cache && config_.prefer_cache) {
            if (auto cached = try_load_from_cache(req); cached.has_value()) {
                auto& r = *cached;
                r.duration_us = steady_now_us() - t0;
                r.from_cache = true;
                stats_.cache_hits.fetch_add(1, std::memory_order_relaxed);
                stats_.candles_from_cache.fetch_add(r.loaded_count,
                    std::memory_order_relaxed);
                stats_.succeeded.fetch_add(1, std::memory_order_relaxed);

                // 通知完成
                notify_complete(r);
                finalize_result(r, t0, r.status);
                return r;
            }
            stats_.cache_misses.fetch_add(1, std::memory_order_relaxed);
        }

        // ---------------------------------------------------------------------
        // 6. 分批加载
        // ---------------------------------------------------------------------
        const std::size_t batch_size = std::min(
            req.batch_size,
            history_loader_config::kMaxKlinesPerRequest);

        std::vector<Candle> all_candles;
        all_candles.reserve(std::min(
            static_cast<std::size_t>(estimated),
            req.max_candles));

        std::unordered_set<std::int64_t> seen_timestamps;
        if (config_.deduplicate) {
            seen_timestamps.reserve(all_candles.capacity());
        }

        std::int64_t cursor_us = req.start.microseconds();
        const std::int64_t end_us = req.end.microseconds();
        std::size_t batch_index = 0;

        while (cursor_us <= end_us && all_candles.size() < req.max_candles) {
            // 6.1 取消检查
            if (is_cancelled(req.symbol)) {
                result.status = LoadStatus::Cancelled;
                result.reason = "用户取消";
                stats_.cancelled.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            // 6.2 计算本批结束时间
            const std::int64_t batch_span = static_cast<std::int64_t>(batch_size - 1)
                * interval_us;
            const std::int64_t batch_end_us = std::min(
                cursor_us + batch_span, end_us);

            // 6.3 拉取
            auto fetch_result = fetch_klines_with_retry(
                req.symbol, req.interval,
                Timestamp{cursor_us}, Timestamp{batch_end_us},
                batch_size);

            if (fetch_result.is_err()) {
                // 记录错误，继续下一批
                if (callbacks_.on_error) {
                    safe_invoke([&] {
                        callbacks_.on_error(req.symbol, fetch_result.error());
                    });
                }
                cursor_us = batch_end_us + interval_us;
                continue;
            }

            const auto& batch = fetch_result.value();
            if (batch.empty()) {
                // 空批：可能已到数据边界
                cursor_us = batch_end_us + interval_us;
                if (batch_index > 0) {
                    // 已有数据，视为完成
                    break;
                }
                // 首批空，可能超出范围
                if (cursor_us > end_us) break;
                continue;
            }

            // 6.4 处理 K 线
            for (const auto& c : batch) {
                const auto ts_us = c.open_time.microseconds();

                // 边界检查
                if (ts_us < req.start.microseconds() ||
                    ts_us > end_us) {
                    continue;
                }

                // 去重
                if (config_.deduplicate) {
                    if (!seen_timestamps.insert(ts_us).second) {
                        ++result.duplicate_count;
                        stats_.duplicates_skipped.fetch_add(1,
                            std::memory_order_relaxed);
                        continue;
                    }
                }

                // 有效性校验
                if (!is_valid_candle(c)) {
                    ++result.invalid_count;
                    stats_.invalid_skipped.fetch_add(1,
                        std::memory_order_relaxed);
                    continue;
                }

                all_candles.push_back(c);
            }

            ++result.batch_count;
            ++batch_index;

            // 6.5 回调
            if (callbacks_.on_batch_complete) {
                safe_invoke([&] {
                    callbacks_.on_batch_complete(
                        req.symbol, batch_index, batch.size(),
                        Timestamp{cursor_us}, Timestamp{batch_end_us});
                });
            }

            if (callbacks_.on_progress) {
                const auto progress_total = std::max(
                    static_cast<std::size_t>(estimated), all_candles.size());
                safe_invoke([&] {
                    callbacks_.on_progress(req.symbol,
                        all_candles.size(), progress_total);
                });
            }

            // 6.6 推进游标
            cursor_us = batch_end_us + interval_us;
        }

        // ---------------------------------------------------------------------
        // 7. 排序
        // ---------------------------------------------------------------------
        if (!all_candles.empty()) {
            std::sort(all_candles.begin(), all_candles.end(),
                [](const Candle& a, const Candle& b) {
                    return a.open_time < b.open_time;
                });
        }

        // ---------------------------------------------------------------------
        // 8. 连续性校验
        // ---------------------------------------------------------------------
        if (config_.validate_continuity && req.validate_continuity &&
            all_candles.size() >= 2) {
            validate_gaps(all_candles, interval_us, result.gaps);
        }

        // ---------------------------------------------------------------------
        // 9. 设置结果
        // ---------------------------------------------------------------------
        result.loaded_count = all_candles.size();
        result.actual_start = all_candles.empty()
            ? req.start : all_candles.front().open_time;
        result.actual_end = all_candles.empty()
            ? req.end : all_candles.back().open_time;

        // 确定状态
        if (all_candles.empty()) {
            result.status = LoadStatus::Empty;
        } else if (result.status == LoadStatus::Cancelled) {
            // 保持 Cancelled
        } else if (!result.gaps.empty()) {
            result.status = LoadStatus::PartialSuccess;
        } else if (all_candles.size() < estimated) {
            result.status = LoadStatus::PartialSuccess;
        } else {
            result.status = LoadStatus::Success;
        }

        // ---------------------------------------------------------------------
        // 10. 缓存写入
        // ---------------------------------------------------------------------
        if (req.write_cache && config_.enable_cache &&
            !all_candles.empty() &&
            result.status != LoadStatus::Cancelled) {
            save_to_cache(req, all_candles);
        }

        // ---------------------------------------------------------------------
        // 11. 移动并完成
        // ---------------------------------------------------------------------
        result.candles = std::move(all_candles);
        finalize_result(result, t0, result.status);
        notify_complete(result);

        return result;
    }

    // -------------------------------------------------------------------------
    // 状态管理
    // -------------------------------------------------------------------------
    void cancel_all() noexcept {
        cancel_all_.store(true, std::memory_order_release);
        {
            std::lock_guard lock(cancel_mutex_);
            cancel_set_.clear();
        }
        QUANT_LOG_INFO("[HistoryLoader] 取消所有加载");
    }

    void cancel(std::string_view symbol) noexcept {
        std::lock_guard lock(cancel_mutex_);
        cancel_set_.insert(std::string{symbol});
        QUANT_LOG_INFO("[HistoryLoader] 取消加载: {}", symbol);
    }

    bool is_loading(std::string_view symbol) const noexcept {
        std::lock_guard lock(in_flight_mutex_);
        return in_flight_symbols_.count(std::string{symbol}) > 0;
    }

    std::vector<std::string> active_symbols() const {
        std::lock_guard lock(in_flight_mutex_);
        return std::vector<std::string>(
            in_flight_symbols_.begin(), in_flight_symbols_.end());
    }

    // -------------------------------------------------------------------------
    // 统计
    // -------------------------------------------------------------------------
    const HistoryLoaderStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_.reset(); }

    const HistoryLoaderConfig& config() const noexcept { return config_; }

    void set_config(const HistoryLoaderConfig& config) {
        if (auto r = validate_config(config); r.is_err()) {
            QUANT_LOG_WARN("[HistoryLoader] 新配置非法，忽略: {}",
                r.error().to_string());
            return;
        }
        std::lock_guard lock(config_mutex_);
        config_ = config;
    }

    // -------------------------------------------------------------------------
    // 估算
    // -------------------------------------------------------------------------
    std::size_t estimate_count(const HistoryLoadRequest& req) const noexcept {
        const auto interval_us = parse_interval_us(req.interval);
        if (interval_us <= 0) return 0;
        return estimate_count_internal(req, interval_us);
    }

    std::chrono::milliseconds estimate_duration(
        const HistoryLoadRequest& req) const noexcept
    {
        const auto count = estimate_count(req);
        if (count == 0) return std::chrono::milliseconds{0};

        const auto batch_size = std::min(
            req.batch_size, history_loader_config::kMaxKlinesPerRequest);
        const auto batches = (count + batch_size - 1) / batch_size;

        // 每批 ~50ms 网络 + 每批 ~10 权重
        const auto network_ms = batches * 50;
        const auto rate_limit_ms = batches * 10 * 250;   // 4 请求/秒

        return std::chrono::milliseconds{
            static_cast<std::int64_t>(network_ms + rate_limit_ms)};
    }

    // -------------------------------------------------------------------------
    // 缓存管理
    // -------------------------------------------------------------------------
    void clear_cache() {
        std::lock_guard lock(cache_mutex_);
        if (!config_.enable_cache) return;

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(
                config_.cache_dir, ec)) {
            if (ec) break;
            std::filesystem::remove(entry.path(), ec);
        }
        cache_index_.clear();

        QUANT_LOG_INFO("[HistoryLoader] 缓存已清空");
    }

    std::size_t cache_entry_count() const {
        std::error_code ec;
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(
                config_.cache_dir, ec)) {
            (void)entry;
            ++count;
        }
        return count;
    }

    std::uintmax_t cache_size_bytes() const {
        std::error_code ec;
        std::uintmax_t total = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                config_.cache_dir, ec)) {
            if (ec) break;
            if (entry.is_regular_file()) {
                total += entry.file_size(ec);
            }
        }
        return total;
    }

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    std::string dump() const {
        std::string out;
        out.reserve(2048);
        char buf[512];

        out += "HistoryLoader Dump:\n";

        {
            std::lock_guard lock(config_mutex_);
            std::snprintf(buf, sizeof(buf), "  batch_size:          %zu\n",
                config_.default_batch_size);
            out += buf;
            std::snprintf(buf, sizeof(buf), "  max_candles:         %zu\n",
                config_.default_max_candles);
            out += buf;
            std::snprintf(buf, sizeof(buf), "  max_retries:         %d\n",
                config_.max_retries);
            out += buf;
            std::snprintf(buf, sizeof(buf), "  cache_enabled:       %s\n",
                config_.enable_cache ? "true" : "false");
            out += buf;
            std::snprintf(buf, sizeof(buf), "  cache_dir:           %s\n",
                config_.cache_dir.string().c_str());
            out += buf;
        }

        out += "  ---\n";

        {
            std::lock_guard lock(in_flight_mutex_);
            std::snprintf(buf, sizeof(buf), "  in_flight_symbols:   %zu\n",
                in_flight_symbols_.size());
            out += buf;
        }

        {
            std::snprintf(buf, sizeof(buf), "  current_weight:      %zu\n",
                current_weight_.load(std::memory_order_relaxed));
            out += buf;
        }

        out += "  ---\n";

        std::snprintf(buf, sizeof(buf), "  total_requests:      %llu\n",
            static_cast<unsigned long long>(
                stats_.total_requests.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  succeeded:           %llu\n",
            static_cast<unsigned long long>(
                stats_.succeeded.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  partial:             %llu\n",
            static_cast<unsigned long long>(
                stats_.partial.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  failed:              %llu\n",
            static_cast<unsigned long long>(
                stats_.failed.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  cancelled:           %llu\n",
            static_cast<unsigned long long>(
                stats_.cancelled.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  candles_loaded:      %llu\n",
            static_cast<unsigned long long>(
                stats_.candles_loaded.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  candles_from_cache:  %llu\n",
            static_cast<unsigned long long>(
                stats_.candles_from_cache.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  rest_requests:       %llu\n",
            static_cast<unsigned long long>(
                stats_.rest_requests.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  rest_retries:        %llu\n",
            static_cast<unsigned long long>(
                stats_.rest_retries.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  rest_failures:       %llu\n",
            static_cast<unsigned long long>(
                stats_.rest_failures.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  rate_limit_waits:    %llu\n",
            static_cast<unsigned long long>(
                stats_.rate_limit_waits.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  cache_hits:          %llu\n",
            static_cast<unsigned long long>(
                stats_.cache_hits.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  cache_misses:        %llu\n",
            static_cast<unsigned long long>(
                stats_.cache_misses.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  cache_writes:        %llu\n",
            static_cast<unsigned long long>(
                stats_.cache_writes.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  duplicates_skipped:  %llu\n",
            static_cast<unsigned long long>(
                stats_.duplicates_skipped.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  invalid_skipped:     %llu\n",
            static_cast<unsigned long long>(
                stats_.invalid_skipped.load(std::memory_order_relaxed)));
        out += buf;

        std::snprintf(buf, sizeof(buf), "  success_rate:        %.2f%%\n",
            stats_.success_rate() * 100.0);
        out += buf;

        std::snprintf(buf, sizeof(buf), "  avg_latency_ms:      %.2f\n",
            stats_.avg_latency_ms());
        out += buf;

        return out;
    }

private:
    // =========================================================================
    // 内部辅助
    // =========================================================================

    // -------------------------------------------------------------------------
    // 估算数量
    // -------------------------------------------------------------------------
    [[nodiscard]] std::size_t estimate_count_internal(
        const HistoryLoadRequest& req,
        std::int64_t interval_us) const noexcept
    {
        if (interval_us <= 0) return 0;
        if (req.end <= req.start) return 0;

        const auto span_us = req.end.microseconds() - req.start.microseconds();
        const auto count = span_us / interval_us + 1;

        if (count <= 0) return 0;
        if (static_cast<std::uint64_t>(count) >
            std::numeric_limits<std::size_t>::max()) {
            return std::numeric_limits<std::size_t>::max();
        }

        return static_cast<std::size_t>(count);
    }

    // -------------------------------------------------------------------------
    // 分批拉取（带重试）
    // -------------------------------------------------------------------------
    Result<std::vector<Candle>> fetch_klines_with_retry(
        const std::string& symbol,
        const std::string& interval,
        Timestamp start,
        Timestamp end,
        std::size_t limit)
    {
        const int max_attempts = 1 + std::max(0, config_.max_retries);
        ErrorInfo last_error;

        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            // 取消检查
            if (is_cancelled(symbol)) {
                return QUANT_ERR_T(std::vector<Candle>,
                    ErrorCode::UserInputInvalid)
                    .with_context("reason", "已取消");
            }

            // 限流等待
            wait_for_rate_limit(
                history_loader_config::kKlinesWeightPerRequest);

            stats_.rest_requests.fetch_add(1, std::memory_order_relaxed);
            if (attempt > 0) {
                stats_.rest_retries.fetch_add(1, std::memory_order_relaxed);
            }

            KlineRequest kreq;
            kreq.symbol = symbol;
            kreq.interval = interval;
            kreq.start_time = start;
            kreq.end_time = end;
            kreq.limit = limit;

            auto result = rest_.get_klines(kreq);

            if (result.is_ok()) {
                record_weight_used(
                    history_loader_config::kKlinesWeightPerRequest);
                return result;
            }

            last_error = result.error();
            stats_.rest_failures.fetch_add(1, std::memory_order_relaxed);

            // 判断是否可重试
            if (!is_retryable_error(result.error().code)) {
                return result;
            }

            // 退避
            if (attempt < max_attempts - 1) {
                const auto delay = compute_backoff(attempt,
                    config_.retry_initial_delay, config_.retry_max_delay);

                QUANT_LOG_WARN(
                    "[HistoryLoader] {} 拉取失败，{}ms 后重试 (attempt={}): {}",
                    symbol, delay.count(), attempt + 1,
                    result.error().to_string());

                std::this_thread::sleep_for(delay);
            }
        }

        return QUANT_ERR_T(std::vector<Candle>, last_error.code)
            .with_context("reason", "重试耗尽")
            .with_context("last_error", last_error.to_string());
    }

    // -------------------------------------------------------------------------
    // 限流
    // -------------------------------------------------------------------------
    void wait_for_rate_limit(std::size_t upcoming_weight) {
        if (!config_.auto_rate_limit) return;

        std::unique_lock lock(rate_limit_mutex_);

        // 检查是否需要重置窗口（每分钟）
        const auto now = steady_now_us();
        if (now - weight_reset_time_us_ >= 60'000'000LL) {
            current_weight_.store(0, std::memory_order_relaxed);
            weight_reset_time_us_ = now;
        }

        // 检查是否超阈值
        const auto used = current_weight_.load(std::memory_order_relaxed);
        if (used + upcoming_weight < config_.rate_limit_warn_threshold) {
            return;
        }

        // 等待窗口重置
        stats_.rate_limit_waits.fetch_add(1, std::memory_order_relaxed);

        QUANT_LOG_WARN(
            "[HistoryLoader] 限流: 当前权重 {}/{}，等待窗口重置",
            used, config_.rate_limit_weight_per_minute);

        const auto wait_until_us = weight_reset_time_us_ + 60'000'000LL;
        while (steady_now_us() < wait_until_us) {
            const auto remaining_us = wait_until_us - steady_now_us();
            if (remaining_us <= 0) break;

            const auto wait_ms = std::min<std::int64_t>(
                remaining_us / 1000, 1000);
            rate_limit_cv_.wait_for(lock,
                std::chrono::milliseconds{wait_ms}, [] { return false; });

            // 检查取消
            // 简化：不主动中断等待
        }

        current_weight_.store(0, std::memory_order_relaxed);
        weight_reset_time_us_ = steady_now_us();
    }

    void record_weight_used(std::size_t weight) noexcept {
        current_weight_.fetch_add(weight, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // 取消检查
    // -------------------------------------------------------------------------
    [[nodiscard]] bool is_cancelled(std::string_view symbol) const noexcept {
        if (cancel_all_.load(std::memory_order_acquire)) return true;

        std::lock_guard lock(cancel_mutex_);
        return cancel_set_.count(std::string{symbol}) > 0;
    }

    // -------------------------------------------------------------------------
    // 缓存
    // -------------------------------------------------------------------------
    [[nodiscard]] std::optional<HistoryLoadResult>
        try_load_from_cache(const HistoryLoadRequest& req) const
    {
        if (!config_.enable_cache) return std::nullopt;

        const auto key = make_cache_key(req.symbol, req.interval,
                                          req.start, req.end);
        const auto path = config_.cache_dir / (key + ".bin");

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return std::nullopt;
        }

        // 检查 TTL
        const auto last_write = std::filesystem::last_write_time(path, ec);
        if (ec) return std::nullopt;

        const auto now = std::filesystem::file_time_type::clock::now();
        const auto age = now - last_write;

        if (age > config_.cache_ttl) {
            return std::nullopt;   // 过期
        }

        auto candles_opt = deserialize_candles(path);
        if (!candles_opt.has_value()) {
            return std::nullopt;
        }

        HistoryLoadResult result;
        result.symbol = req.symbol;
        result.interval = req.interval;
        result.candles = std::move(*candles_opt);
        result.loaded_count = result.candles.size();
        result.requested_count = result.loaded_count;
        result.status = LoadStatus::Success;
        result.from_cache = true;

        if (!result.candles.empty()) {
            result.actual_start = result.candles.front().open_time;
            result.actual_end = result.candles.back().open_time;
        }

        QUANT_LOG_INFO("[HistoryLoader] 缓存命中: {} ({} 根)",
            key, result.loaded_count);

        return result;
    }

    void save_to_cache(const HistoryLoadRequest& req,
                        const std::vector<Candle>& candles)
    {
        if (!config_.enable_cache || candles.empty()) return;

        const auto key = make_cache_key(req.symbol, req.interval,
                                          req.start, req.end);
        const auto path = config_.cache_dir / (key + ".bin");

        try {
            if (serialize_candles(path, candles)) {
                stats_.cache_writes.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (const std::exception& e) {
            QUANT_LOG_DEBUG("[HistoryLoader] 缓存写入失败: {}", e.what());
        }
    }

    // -------------------------------------------------------------------------
    // 缺口检测
    // -------------------------------------------------------------------------
    void validate_gaps(const std::vector<Candle>& candles,
                        std::int64_t interval_us,
                        std::vector<std::pair<Timestamp, Timestamp>>& gaps) const
    {
        gaps.clear();
        if (candles.size() < 2) return;

        for (std::size_t i = 1; i < candles.size(); ++i) {
            const auto prev_us = candles[i - 1].open_time.microseconds();
            const auto curr_us = candles[i].open_time.microseconds();

            if (curr_us - prev_us > interval_us) {
                gaps.emplace_back(
                    Timestamp{prev_us + interval_us},
                    Timestamp{curr_us - interval_us});
            }
        }
    }

    // -------------------------------------------------------------------------
    // 结果完成
    // -------------------------------------------------------------------------
    void finalize_result(HistoryLoadResult& result,
                          std::int64_t t0_us,
                          LoadStatus status) noexcept
    {
        result.status = status;
        result.duration_us = steady_now_us() - t0_us;

        stats_.last_latency_us.store(result.duration_us,
            std::memory_order_relaxed);
        stats_.total_latency_us.fetch_add(result.duration_us,
            std::memory_order_relaxed);

        switch (status) {
            case LoadStatus::Success:
                stats_.succeeded.fetch_add(1, std::memory_order_relaxed);
                stats_.candles_loaded.fetch_add(result.loaded_count,
                    std::memory_order_relaxed);
                break;
            case LoadStatus::PartialSuccess:
                stats_.partial.fetch_add(1, std::memory_order_relaxed);
                stats_.candles_loaded.fetch_add(result.loaded_count,
                    std::memory_order_relaxed);
                break;
            case LoadStatus::Failed:
                stats_.failed.fetch_add(1, std::memory_order_relaxed);
                break;
            case LoadStatus::Cancelled:
                // 已在调用方计数
                break;
            case LoadStatus::Empty:
                stats_.succeeded.fetch_add(1, std::memory_order_relaxed);
                break;
            case LoadStatus::TooLarge:
            case LoadStatus::OutOfRange:
                stats_.failed.fetch_add(1, std::memory_order_relaxed);
                break;
        }
    }

    void notify_complete(const HistoryLoadResult& result) {
        if (!callbacks_.on_complete) return;
        safe_invoke([&] { callbacks_.on_complete(result); });
    }

    template <typename F>
    void safe_invoke(F&& f) noexcept {
        try {
            f();
        } catch (const std::exception& e) {
            QUANT_LOG_ERROR("[HistoryLoader] 回调异常: {}", e.what());
        } catch (...) {
            QUANT_LOG_ERROR("[HistoryLoader] 回调未知异常");
        }
    }

    // =========================================================================
    // 成员
    // =========================================================================
    BinanceRest& rest_;
    HistoryLoaderConfig config_;
    mutable std::mutex config_mutex_;

    HistoryLoaderCallbacks callbacks_;
    mutable std::mutex callbacks_mutex_;

    HistoryLoaderStats stats_;

    // In-flight
    mutable std::mutex in_flight_mutex_;
    std::unordered_set<std::string> in_flight_symbols_;

    // Cancel
    std::atomic<bool> cancel_all_{false};
    mutable std::mutex cancel_mutex_;
    std::unordered_set<std::string> cancel_set_;

    // 限流
    mutable std::mutex rate_limit_mutex_;
    std::condition_variable rate_limit_cv_;
    std::atomic<std::size_t> current_weight_{0};
    std::int64_t weight_reset_time_us_{0};

    // 缓存
    mutable std::mutex cache_mutex_;
    std::unordered_map<std::string, std::filesystem::path> cache_index_;
};

// ==============================================================================
// HistoryLoader 转发实现
// ==============================================================================
HistoryLoader::HistoryLoader(BinanceRest& rest_client,
                                HistoryLoaderConfig config)
    : impl_{std::make_unique<Impl>(rest_client, std::move(config))}
{
    if (auto r = impl_->init(); r.is_err()) {
        QUANT_LOG_WARN("[HistoryLoader] 初始化警告: {}",
            r.error().to_string());
    }
}

HistoryLoader::~HistoryLoader() = default;

void HistoryLoader::set_callbacks(HistoryLoaderCallbacks callbacks) {
    impl_->set_callbacks(std::move(callbacks));
}

HistoryLoadResult HistoryLoader::load(const HistoryLoadRequest& request) {
    return impl_->load(request);
}

std::future<HistoryLoadResult> HistoryLoader::load_async(
    HistoryLoadRequest request)
{
    return std::async(std::launch::async,
        [impl = impl_.get(), req = std::move(request)]() mutable {
            try {
                return impl->load(req);
            } catch (...) {
                HistoryLoadResult err;
                err.symbol = req.symbol;
                err.status = LoadStatus::Failed;
                err.reason = "异步加载异常";
                return err;
            }
        });
}

std::vector<HistoryLoadResult> HistoryLoader::load_batch(
    std::span<const HistoryLoadRequest> requests)
{
    std::vector<HistoryLoadResult> results;
    if (requests.empty()) return results;

    results.reserve(requests.size());

    // 简化实现：串行加载（后续可扩展为并行）
    for (const auto& req : requests) {
        try {
            results.push_back(impl_->load(req));
        } catch (const std::exception& e) {
            QUANT_LOG_ERROR("[HistoryLoader] 批量加载异常: {}", e.what());
            HistoryLoadResult err;
            err.symbol = req.symbol;
            err.status = LoadStatus::Failed;
            err.reason = "加载异常";
            results.push_back(std::move(err));
        }
    }

    return results;
}

std::size_t HistoryLoader::estimate_count(
    const HistoryLoadRequest& request) const noexcept
{
    return impl_->estimate_count(request);
}

std::chrono::milliseconds HistoryLoader::estimate_duration(
    const HistoryLoadRequest& request) const noexcept
{
    return impl_->estimate_duration(request);
}

void HistoryLoader::cancel_all() noexcept {
    impl_->cancel_all();
}

void HistoryLoader::cancel(std::string_view symbol) noexcept {
    impl_->cancel(symbol);
}

bool HistoryLoader::is_loading(std::string_view symbol) const noexcept {
    return impl_->is_loading(symbol);
}

std::vector<std::string> HistoryLoader::active_symbols() const {
    return impl_->active_symbols();
}

const HistoryLoaderStats& HistoryLoader::stats() const noexcept {
    return impl_->stats();
}

void HistoryLoader::reset_stats() noexcept {
    impl_->reset_stats();
}

const HistoryLoaderConfig& HistoryLoader::config() const noexcept {
    return impl_->config();
}

void HistoryLoader::set_config(const HistoryLoaderConfig& config) {
    impl_->set_config(config);
}

void HistoryLoader::clear_cache() {
    impl_->clear_cache();
}

std::size_t HistoryLoader::cache_entry_count() const {
    return impl_->cache_entry_count();
}

std::uintmax_t HistoryLoader::cache_size_bytes() const {
    return impl_->cache_size_bytes();
}

std::string HistoryLoader::dump() const {
    return impl_->dump();
}

// ==============================================================================
// 便捷函数
// ==============================================================================
HistoryLoadRequest make_recent_request(
    std::string symbol,
    std::string interval,
    std::size_t count,
    std::int64_t interval_us)
{
    HistoryLoadRequest req;
    req.symbol = std::move(symbol);
    req.interval = std::move(interval);
    req.max_candles = count;

    const auto now_us = Timestamp::now().microseconds();
    req.end = Timestamp{now_us};
    req.start = Timestamp{now_us -
        static_cast<std::int64_t>(count) * interval_us};

    return req;
}

HistoryLoadRequest make_range_request(
    std::string symbol,
    std::string interval,
    Timestamp start,
    Timestamp end)
{
    HistoryLoadRequest req;
    req.symbol = std::move(symbol);
    req.interval = std::move(interval);
    req.start = start;
    req.end = end;
    return req;
}

// ==============================================================================
// load_into_buffer 模板实现
// ==============================================================================
template <std::size_t Capacity>
Result<void> HistoryLoader::load_into_buffer(
    const HistoryLoadRequest& request,
    CandleBuffer<Capacity>& buffer)
{
    auto result = load(request);
    if (!result.is_success()) {
        return QUANT_ERR_MSG(ErrorCode::NetworkUnknown,
            "加载失败")
            .with_context("status", std::string{to_string(result.status)})
            .with_context("reason", std::string{result.reason});
    }

    // 插入 K 线
    std::size_t inserted = 0;
    std::size_t skipped = 0;

    for (const auto& c : result.candles) {
        if (auto r = buffer.push_closed(c); r.is_ok()) {
            ++inserted;
        } else {
            ++skipped;
        }
    }

    // 若缓冲区容量不足，会有部分被覆盖
    if (inserted < result.candles.size()) {
        QUANT_LOG_WARN(
            "[HistoryLoader] 缓冲区容量不足: 插入 {} / {}",
            inserted, result.candles.size());
    }

    return {};
}

// ==============================================================================
// 显式模板实例化
// ==============================================================================
template Result<void> HistoryLoader::load_into_buffer<256>(
    const HistoryLoadRequest&, CandleBuffer<256>&);
template Result<void> HistoryLoader::load_into_buffer<512>(
    const HistoryLoadRequest&, CandleBuffer<512>&);
template Result<void> HistoryLoader::load_into_buffer<1024>(
    const HistoryLoadRequest&, CandleBuffer<1024>&);
template Result<void> HistoryLoader::load_into_buffer<2048>(
    const HistoryLoadRequest&, CandleBuffer<2048>&);
template Result<void> HistoryLoader::load_into_buffer<4096>(
    const HistoryLoadRequest&, CandleBuffer<4096>&);
template Result<void> HistoryLoader::load_into_buffer<8192>(
    const HistoryLoadRequest&, CandleBuffer<8192>&);
template Result<void> HistoryLoader::load_into_buffer<16384>(
    const HistoryLoadRequest&, CandleBuffer<16384>&);

}  // namespace data
}  // namespace quant
