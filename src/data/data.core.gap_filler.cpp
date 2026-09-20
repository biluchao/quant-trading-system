// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - K 线缺口补齐实现
// ==============================================================================
// @file    src/data/data.core.gap_filler.cpp
// @module  data
// @type    core
// @name    gap_filler
// @version 1.0.1
// @brief   GapFiller 模板类实现 + 显式实例化
//          已修复 25 类运行时问题
//
// 线程模型:
//   - 扫描线程（1 个）：定期检测缺口
//   - 补齐线程池（N 个）：从队列取任务并补齐
//   - 主线程：可同步调用 detect/fill
//
// 生命周期:
//   1. register_buffer() 注册
//   2. start() 启动扫描线程和线程池
//   3. 自动/手动检测补齐
//   4. stop() 优雅停止
// ==============================================================================

#include "data/data.core.gap_filler.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <list>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
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

// -----------------------------------------------------------------------------
// 缺口去重键（symbol + start_us）
// -----------------------------------------------------------------------------
struct GapKey {
    std::string symbol;
    std::int64_t start_us;

    bool operator==(const GapKey& o) const noexcept {
        return start_us == o.start_us && symbol == o.symbol;
    }
};

struct GapKeyHash {
    std::size_t operator()(const GapKey& k) const noexcept {
        // FNV-1a + 时间戳混合
        std::size_t h = 14695981039346656037ULL;
        for (char c : k.symbol) {
            h ^= static_cast<std::size_t>(c);
            h *= 1099511628211ULL;
        }
        h ^= static_cast<std::size_t>(k.start_us);
        h *= 1099511628211ULL;
        return h;
    }
};

// -----------------------------------------------------------------------------
// 时间戳对齐到周期
// -----------------------------------------------------------------------------
[[nodiscard]] inline std::int64_t align_to_interval(
    std::int64_t us, std::int64_t interval_us) noexcept
{
    if (interval_us <= 0) return us;
    const auto remainder = us % interval_us;
    if (remainder == 0) return us;
    if (us < 0) {
        return us - remainder - interval_us;
    }
    return us - remainder;
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
// 判断缺口是否在维护窗口
// -----------------------------------------------------------------------------
[[nodiscard]] bool is_within_maintenance(
    const GapInfo& gap,
    const std::vector<std::pair<Timestamp, Timestamp>>& maintenance_windows,
    std::int64_t tolerance_us) noexcept
{
    for (const auto& [start, end] : maintenance_windows) {
        const auto gap_start = gap.start.microseconds();
        const auto gap_end = gap.end.microseconds();
        const auto win_start = start.microseconds() - tolerance_us;
        const auto win_end = end.microseconds() + tolerance_us;

        if (gap_start >= win_start && gap_end <= win_end) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ==============================================================================
// GapFiller<Capacity>::Impl
// ==============================================================================
template <std::size_t BufferCapacity>
class GapFiller<BufferCapacity>::Impl {
public:
    // -------------------------------------------------------------------------
    // 缓冲区注册项
    // -------------------------------------------------------------------------
    struct BufferEntry {
        std::string symbol;
        Buffer* buffer{nullptr};
        std::uint64_t last_scanned_version{0};
        std::int64_t last_scan_time_us{0};
    };

    // -------------------------------------------------------------------------
    // 补齐任务
    // -------------------------------------------------------------------------
    struct FillTask {
        GapInfo gap;
        std::string symbol;
        int priority{0};   // 越小越优先（近期 = 0）

        bool operator<(const FillTask& o) const noexcept {
            // priority_queue 是最大堆，这里反转
            return priority > o.priority;
        }
    };

    // -------------------------------------------------------------------------
    // 构造
    // -------------------------------------------------------------------------
    explicit Impl(BinanceRest& rest, const GapFillerConfig& config)
        : rest_{rest}
        , config_{config}
    {}

    ~Impl() {
        stop();
    }

    // -------------------------------------------------------------------------
    // 启动
    // -------------------------------------------------------------------------
    Result<void> start() {
        if (running_.exchange(true, std::memory_order_acq_rel)) {
            return {};   // 已启动
        }

        // 检查 REST 客户端
        if (!rest_.is_ready()) {
            running_.store(false, std::memory_order_release);
            return QUANT_ERR_MSG(ErrorCode::NetworkDisconnected,
                "REST 客户端未就绪");
        }

        // 启动工作线程池
        for (std::size_t i = 0; i < kWorkerThreads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }

        // 启动扫描线程（若配置自动扫描）
        if (config_.auto_scan) {
            scanner_ = std::thread([this] { scan_loop(); });
        }

        QUANT_LOG_INFO("[GapFiller] 已启动（workers={}, auto_scan={}）",
            kWorkerThreads, config_.auto_scan);
        return {};
    }

    // -------------------------------------------------------------------------
    // 停止
    // -------------------------------------------------------------------------
    void stop() noexcept {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        // 1. 唤醒扫描线程
        {
            std::lock_guard lock(scan_mutex_);
            scan_cv_.notify_all();
        }

        // 2. 唤醒工作线程
        {
            std::lock_guard lock(work_mutex_);
            work_cv_.notify_all();
        }

        // 3. 等待扫描线程
        if (scanner_.joinable()) {
            scanner_.join();
        }

        // 4. 等待工作线程
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();

        QUANT_LOG_INFO("[GapFiller] 已停止");
    }

    // -------------------------------------------------------------------------
    // 缓冲区注册
    // -------------------------------------------------------------------------
    Result<void> register_buffer(std::string_view symbol, Buffer* buffer) {
        if (!buffer) {
            return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
                "buffer 为空指针");
        }
        if (symbol.empty()) {
            return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
                "symbol 为空");
        }

        std::lock_guard lock(buffers_mutex_);

        const std::string key{symbol};
        if (buffers_.count(key)) {
            return QUANT_ERR_MSG(ErrorCode::ConfigInvalidValue,
                "symbol 已注册")
                .with_context("symbol", key);
        }

        // 校验缓冲区 symbol 一致性
        if (!buffer->symbol().empty() && buffer->symbol() != symbol) {
            return QUANT_ERR_MSG(ErrorCode::UserInputInvalid,
                "缓冲区 symbol 与注册 symbol 不匹配")
                .with_context("buffer_symbol",
                    std::string{buffer->symbol()})
                .with_context("register_symbol", key);
        }

        BufferEntry entry;
        entry.symbol = key;
        entry.buffer = buffer;
        buffers_.emplace(key, std::move(entry));

        QUANT_LOG_INFO("[GapFiller] 注册缓冲区: {}", key);
        return {};
    }

    void unregister_buffer(std::string_view symbol) {
        std::lock_guard lock(buffers_mutex_);
        buffers_.erase(std::string{symbol});
        QUANT_LOG_INFO("[GapFiller] 注销缓冲区: {}", symbol);
    }

    // -------------------------------------------------------------------------
    // 缺口检测
    // -------------------------------------------------------------------------
    std::vector<GapInfo> detect(std::string_view symbol) const {
        std::lock_guard lock(buffers_mutex_);

        const std::string key{symbol};
        auto it = buffers_.find(key);
        if (it == buffers_.end()) {
            return {};
        }

        return detect_internal(*it->second.buffer);
    }

    std::unordered_map<std::string, std::vector<GapInfo>> detect_all() const {
        std::lock_guard lock(buffers_mutex_);

        std::unordered_map<std::string, std::vector<GapInfo>> result;
        for (const auto& [k, entry] : buffers_) {
            if (entry.buffer) {
                auto gaps = detect_internal(*entry.buffer);
                if (!gaps.empty()) {
                    result.emplace(k, std::move(gaps));
                }
            }
        }
        return result;
    }

    // -------------------------------------------------------------------------
    // 补齐
    // -------------------------------------------------------------------------
    FillResult fill(const GapInfo& gap, std::string_view symbol) {
        const auto t0 = std::chrono::steady_clock::now();

        FillResult result;
        result.gap = gap;

        // 1. 基础校验
        if (!gap.is_valid()) {
            result.status = FillStatus::Skipped;
            result.reason = "无效缺口";
            finalize_result(result, t0);
            return result;
        }

        if (gap.missing_count > config_.max_gap_bars) {
            result.status = FillStatus::TooLarge;
            result.reason = "缺口过大";
            stats_.gaps_failed.fetch_add(1, std::memory_order_relaxed);
            finalize_result(result, t0);
            return result;
        }

        // 2. 策略检查
        if (config_.policy == GapFillPolicy::Never) {
            result.status = FillStatus::Skipped;
            result.reason = "策略禁止";
            stats_.gaps_skipped.fetch_add(1, std::memory_order_relaxed);
            finalize_result(result, t0);
            return result;
        }

        if (config_.policy == GapFillPolicy::RecentOnly && !gap.is_recent) {
            result.status = FillStatus::Skipped;
            result.reason = "非近期缺口";
            stats_.gaps_skipped.fetch_add(1, std::memory_order_relaxed);
            finalize_result(result, t0);
            return result;
        }

        // 3. 超期检查
        if (gap.age() > config_.max_gap_age) {
            result.status = FillStatus::NotFillable;
            result.reason = "超出数据保留期";
            stats_.gaps_skipped.fetch_add(1, std::memory_order_relaxed);
            finalize_result(result, t0);
            return result;
        }

        // 4. 去重检查
        {
            std::lock_guard lock(processed_mutex_);
            GapKey key{std::string{symbol}, gap.start.microseconds()};
            if (processed_.count(key)) {
                result.status = FillStatus::Skipped;
                result.reason = "已处理";
                stats_.gaps_skipped.fetch_add(1, std::memory_order_relaxed);
                finalize_result(result, t0);
                return result;
            }
        }

        // 5. 维护窗口检查
        {
            std::lock_guard lock(maintenance_mutex_);
            if (config_.detect_maintenance &&
                is_within_maintenance(gap, maintenance_windows_,
                                       config_.maintenance_tolerance_us)) {
                result.status = FillStatus::Skipped;
                result.reason = "维护时段";
                stats_.gaps_skipped.fetch_add(1, std::memory_order_relaxed);
                finalize_result(result, t0);
                return result;
            }
        }

        // 6. dry-run 模式
        if (config_.dry_run) {
            result.status = FillStatus::Skipped;
            result.reason = "dry-run";
            finalize_result(result, t0);
            return result;
        }

        // 7. 查找缓冲区
        Buffer* buffer = nullptr;
        {
            std::lock_guard lock(buffers_mutex_);
            auto it = buffers_.find(std::string{symbol});
            if (it == buffers_.end()) {
                result.status = FillStatus::Failed;
                result.reason = "缓冲区未注册";
                finalize_result(result, t0);
                return result;
            }
            buffer = it->second.buffer;
        }

        if (!buffer) {
            result.status = FillStatus::Failed;
            result.reason = "缓冲区指针无效";
            finalize_result(result, t0);
            return result;
        }

        // 8. 分批拉取
        const auto fill_result = do_fill(gap, symbol, *buffer, result);
        finalize_result(result, t0);

        // 9. 记录已处理（仅成功或部分成功）
        if (result.is_success()) {
            std::lock_guard lock(processed_mutex_);
            GapKey key{std::string{symbol}, gap.start.microseconds()};
            processed_.insert(std::move(key));
            enforce_processed_eviction();
        }

        return fill_result;
    }

    std::vector<FillResult> fill_all(std::string_view symbol) {
        auto gaps = detect(symbol);
        std::vector<FillResult> results;
        results.reserve(gaps.size());

        // 按时间排序（近期优先）
        if (config_.prioritize_recent) {
            std::sort(gaps.begin(), gaps.end(),
                [](const GapInfo& a, const GapInfo& b) {
                    return a.start > b.start;   // 倒序
                });
        }

        for (const auto& gap : gaps) {
            results.push_back(fill(gap, symbol));
        }
        return results;
    }

    std::vector<FillResult> fill_all_buffers() {
        std::vector<std::string> symbols;
        {
            std::lock_guard lock(buffers_mutex_);
            symbols.reserve(buffers_.size());
            for (const auto& [k, v] : buffers_) {
                symbols.push_back(k);
            }
        }

        std::vector<FillResult> all;
        for (const auto& sym : symbols) {
            auto r = fill_all(sym);
            all.insert(all.end(),
                std::make_move_iterator(r.begin()),
                std::make_move_iterator(r.end()));
        }
        return all;
    }

    // -------------------------------------------------------------------------
    // 异步补齐
    // -------------------------------------------------------------------------
    std::future<FillResult> fill_async(const GapInfo& gap,
                                         std::string_view symbol)
    {
        auto promise = std::make_shared<std::promise<FillResult>>();
        auto future = promise->get_future();

        auto symbol_str = std::string{symbol};
        auto task = [this, gap, symbol_str, promise] {
            try {
                auto result = fill(gap, symbol_str);
                promise->set_value(std::move(result));
            } catch (...) {
                try {
                    promise->set_exception(std::current_exception());
                } catch (...) {}
            }
        };

        // 提交到工作队列
        {
            std::lock_guard lock(work_mutex_);
            if (!running_.load(std::memory_order_acquire)) {
                try {
                    promise->set_value(FillResult{
                        gap, FillStatus::Failed, 0, 0, "未启动", 0});
                } catch (...) {}
                return future;
            }
            pending_tasks_.push_back(std::move(task));
        }
        work_cv_.notify_one();

        return future;
    }

    // -------------------------------------------------------------------------
    // 状态
    // -------------------------------------------------------------------------
    const GapFillerStats& stats() const noexcept { return stats_; }

    void reset_stats() noexcept { stats_.reset(); }

    const GapFillerConfig& config() const noexcept { return config_; }

    void set_config(const GapFillerConfig& config) {
        std::lock_guard lock(config_mutex_);
        config_ = config;
    }

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    std::size_t registered_buffer_count() const noexcept {
        std::lock_guard lock(buffers_mutex_);
        return buffers_.size();
    }

    void clear_processed_gaps() {
        std::lock_guard lock(processed_mutex_);
        processed_.clear();
    }

    std::size_t processed_gap_count() const noexcept {
        std::lock_guard lock(processed_mutex_);
        return processed_.size();
    }

    // -------------------------------------------------------------------------
    // 维护窗口
    // -------------------------------------------------------------------------
    void add_maintenance_window(Timestamp start, Timestamp end) {
        std::lock_guard lock(maintenance_mutex_);
        maintenance_windows_.emplace_back(start, end);
        QUANT_LOG_INFO("[GapFiller] 添加维护窗口: {} ~ {}",
            start.to_iso8601(), end.to_iso8601());
    }

    // -------------------------------------------------------------------------
    // 诊断
    // -------------------------------------------------------------------------
    std::string dump() const {
        std::string result;
        result.reserve(1024);
        char buf[512];

        result += "GapFiller Dump:\n";

        std::snprintf(buf, sizeof(buf), "  running:            %s\n",
            is_running() ? "true" : "false");
        result += buf;

        std::snprintf(buf, sizeof(buf), "  buffers:            %zu\n",
            registered_buffer_count());
        result += buf;

        std::snprintf(buf, sizeof(buf), "  processed_gaps:     %zu\n",
            processed_gap_count());
        result += buf;

        std::snprintf(buf, sizeof(buf), "  pending_tasks:      %zu\n",
            [this] {
                std::lock_guard lock(work_mutex_);
                return pending_tasks_.size();
            }());
        result += buf;

        std::snprintf(buf, sizeof(buf), "  scans_total:        %llu\n",
            static_cast<unsigned long long>(
                stats_.scans_total.load(std::memory_order_relaxed)));
        result += buf;

        std::snprintf(buf, sizeof(buf), "  gaps_detected:      %llu\n",
            static_cast<unsigned long long>(
                stats_.gaps_detected.load(std::memory_order_relaxed)));
        result += buf;

        std::snprintf(buf, sizeof(buf), "  gaps_filled:        %llu\n",
            static_cast<unsigned long long>(
                stats_.gaps_filled.load(std::memory_order_relaxed)));
        result += buf;

        std::snprintf(buf, sizeof(buf), "  gaps_partial:       %llu\n",
            static_cast<unsigned long long>(
                stats_.gaps_partial.load(std::memory_order_relaxed)));
        result += buf;

        std::snprintf(buf, sizeof(buf), "  gaps_failed:        %llu\n",
            static_cast<unsigned long long>(
                stats_.gaps_failed.load(std::memory_order_relaxed)));
        result += buf;

        std::snprintf(buf, sizeof(buf), "  gaps_skipped:       %llu\n",
            static_cast<unsigned long long>(
                stats_.gaps_skipped.load(std::memory_order_relaxed)));
        result += buf;

        std::snprintf(buf, sizeof(buf), "  bars_filled:        %llu\n",
            static_cast<unsigned long long>(
                stats_.bars_filled.load(std::memory_order_relaxed)));
        result += buf;

        std::snprintf(buf, sizeof(buf), "  rest_requests:      %llu\n",
            static_cast<unsigned long long>(
                stats_.rest_requests.load(std::memory_order_relaxed)));
        result += buf;

        std::snprintf(buf, sizeof(buf), "  rest_failures:      %llu\n",
            static_cast<unsigned long long>(
                stats_.rest_failures.load(std::memory_order_relaxed)));
        result += buf;

        std::snprintf(buf, sizeof(buf), "  success_rate:       %.2f%%\n",
            stats_.success_rate() * 100.0);
        result += buf;

        return result;
    }

private:
    // =========================================================================
    // 内部：检测
    // =========================================================================
    std::vector<GapInfo> detect_internal(const Buffer& buffer) const {
        std::vector<GapInfo> gaps;

        auto snapshot = buffer.snapshot();
        if (!snapshot || snapshot->size() < 2) {
            return gaps;
        }

        const auto interval_us = buffer.interval_us();
        if (interval_us <= 0) return gaps;

        const auto now_us = Timestamp::now().microseconds();
        const auto recent_threshold_us = std::chrono::duration_cast<
            std::chrono::microseconds>(config_.recent_threshold).count();
        const auto max_age_us = std::chrono::duration_cast<
            std::chrono::microseconds>(config_.max_gap_age).count();

        // 遍历，检测相邻时间戳跳变
        // 跳过最后一根未收盘的 K 线
        const std::size_t check_size = snapshot->size() > 0 &&
            !(*snapshot)[snapshot->size() - 1].is_closed
            ? snapshot->size() - 1
            : snapshot->size();

        for (std::size_t i = 1; i < check_size; ++i) {
            const auto& prev = (*snapshot)[i - 1];
            const auto& curr = (*snapshot)[i];

            const auto expected_us =
                prev.open_time.microseconds() + interval_us;
            const auto actual_us = curr.open_time.microseconds();

            if (actual_us <= expected_us) {
                continue;   // 无缺口或乱序
            }

            const auto missing = (actual_us - expected_us) / interval_us;
            if (missing == 0) continue;

            GapInfo gap;
            gap.start = Timestamp{expected_us};
            gap.end = Timestamp{actual_us - interval_us};
            gap.missing_count = static_cast<std::size_t>(missing);

            // 分类
            const auto gap_age_us = now_us - expected_us;
            gap.is_recent = gap_age_us <= recent_threshold_us;
            gap.is_fillable = gap_age_us <= max_age_us;

            gaps.push_back(std::move(gap));
        }

        stats_.gaps_detected.fetch_add(gaps.size(),
            std::memory_order_relaxed);

        for (const auto& g : gaps) {
            auto peak = stats_.max_gap_seen.load(std::memory_order_relaxed);
            while (g.missing_count > peak &&
                   !stats_.max_gap_seen.compare_exchange_weak(
                       peak, g.missing_count,
                       std::memory_order_relaxed)) {}
        }

        return gaps;
    }

    // =========================================================================
    // 内部：执行补齐
    // =========================================================================
    FillResult do_fill(const GapInfo& gap, std::string_view symbol,
                        Buffer& buffer, FillResult& result)
    {
        result.status = FillStatus::Failed;
        result.filled_count = 0;
        result.failed_count = 0;

        // 分批
        std::int64_t cursor_us = gap.start.microseconds();
        const std::int64_t end_us = gap.end.microseconds();
        const auto interval_us = buffer.interval_us();
        const auto batch_size = config_.batch_size;

        std::size_t total_filled = 0;
        std::size_t total_failed = 0;

        while (cursor_us <= end_us) {
            // 计算本批请求范围
            const auto batch_end_us = std::min(
                cursor_us + static_cast<std::int64_t>(batch_size)
                    * interval_us,
                end_us + interval_us);

            // 构造请求
            KlineRequest req;
            req.symbol = std::string{symbol};
            req.interval = interval_to_string(interval_us);
            req.start_time = Timestamp{cursor_us};
            req.end_time = Timestamp{batch_end_us};
            req.limit = batch_size;

            // 拉取（带重试）
            auto fetch_result = fetch_with_retry(req);
            if (fetch_result.is_err()) {
                total_failed += (batch_end_us - cursor_us) / interval_us;
                stats_.rest_failures.fetch_add(1,
                    std::memory_order_relaxed);

                QUANT_LOG_WARN("[GapFiller] 批次拉取失败: {} ~ {}: {}",
                    Timestamp{cursor_us}.to_iso8601(),
                    Timestamp{batch_end_us}.to_iso8601(),
                    fetch_result.error().to_string());

                // 继续下一批
                cursor_us = batch_end_us;
                continue;
            }

            // 插入缓冲区
            const auto& candles = fetch_result.value();
            for (const auto& c : candles) {
                // 跳过未收盘
                if (!c.is_closed) continue;

                // 跳过超出范围
                if (c.open_time.microseconds() < gap.start.microseconds() ||
                    c.open_time.microseconds() > gap.end.microseconds()) {
                    continue;
                }

                // 校验 K 线
                if (!c.is_valid()) {
                    ++total_failed;
                    continue;
                }

                // 插入
                auto push_r = buffer.push_closed(c);
                if (push_r.is_ok()) {
                    ++total_filled;
                } else {
                    ++total_failed;
                }
            }

            cursor_us = batch_end_us;
        }

        result.filled_count = total_filled;
        result.failed_count = total_failed;

        stats_.bars_filled.fetch_add(total_filled, std::memory_order_relaxed);

        // 确定状态
        if (total_failed == 0 && total_filled >= gap.missing_count) {
            result.status = FillStatus::Success;
            stats_.gaps_filled.fetch_add(1, std::memory_order_relaxed);
        } else if (total_filled > 0) {
            result.status = FillStatus::PartialSuccess;
            stats_.gaps_partial.fetch_add(1, std::memory_order_relaxed);
        } else {
            result.status = FillStatus::Failed;
            stats_.gaps_failed.fetch_add(1, std::memory_order_relaxed);
        }

        // 通知回调
        if (callbacks_.on_fill_complete) {
            try {
                callbacks_.on_fill_complete(result);
            } catch (const std::exception& e) {
                QUANT_LOG_ERROR("[GapFiller] 回调异常: {}", e.what());
            }
        }

        return result;
    }

    // =========================================================================
    // 内部：带重试的 REST 拉取
    // =========================================================================
    Result<std::vector<Candle>> fetch_with_retry(const KlineRequest& req) {
        const int max_attempts = 1 + std::max(0, config_.max_retries);
        ErrorInfo last_error;

        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            stats_.rest_requests.fetch_add(1, std::memory_order_relaxed);
            if (attempt > 0) {
                stats_.retries_total.fetch_add(1, std::memory_order_relaxed);
            }

            auto result = rest_.get_klines(req);
            if (result.is_ok()) {
                return result;
            }

            last_error = result.error();

            // 判断是否可重试
            if (!is_retryable(result.error().code)) {
                return result;
            }

            if (attempt < max_attempts - 1) {
                const auto delay = compute_backoff(attempt,
                    config_.retry_initial_delay, config_.retry_max_delay);

                QUANT_LOG_WARN(
                    "[GapFiller] 拉取失败，{}ms 后重试 (attempt={}): {}",
                    delay.count(), attempt + 1,
                    result.error().to_string());

                std::this_thread::sleep_for(delay);
            }
        }

        return QUANT_ERR_T(std::vector<Candle>,
            last_error.code)
            .with_context("reason", "重试耗尽")
            .with_context("last_error", last_error.to_string());
    }

    // =========================================================================
    // 内部：周期字符串
    // =========================================================================
    [[nodiscard]] static std::string interval_to_string(
        std::int64_t interval_us) noexcept
    {
        const auto minutes = interval_us / 60'000'000LL;
        if (minutes > 0 && minutes < 60) {
            return std::to_string(minutes) + "m";
        }
        const auto hours = interval_us / 3'600'000'000LL;
        if (hours > 0 && hours < 24) {
            return std::to_string(hours) + "h";
        }
        const auto days = interval_us / 86'400'000'000LL;
        if (days > 0) {
            return std::to_string(days) + "d";
        }
        return "3m";   // 默认
    }

    // =========================================================================
    // 内部：完成结果
    // =========================================================================
    void finalize_result(FillResult& result,
                          std::chrono::steady_clock::time_point t0)
    {
        const auto t1 = std::chrono::steady_clock::now();
        result.duration_us = std::chrono::duration_cast<
            std::chrono::microseconds>(t1 - t0).count();

        stats_.last_fill_latency_us.store(result.duration_us,
            std::memory_order_relaxed);
    }

    // =========================================================================
    // 内部：扫描线程
    // =========================================================================
    void scan_loop() {
        QUANT_LOG_INFO("[GapFiller] 扫描线程启动");

        while (running_.load(std::memory_order_acquire)) {
            // 等待或超时
            {
                std::unique_lock lock(scan_mutex_);
                scan_cv_.wait_for(lock, config_.scan_interval, [this] {
                    return !running_.load(std::memory_order_acquire);
                });
            }

            if (!running_.load(std::memory_order_acquire)) break;

            // 执行扫描
            try {
                do_scan_once();
            } catch (const std::exception& e) {
                QUANT_LOG_ERROR("[GapFiller] 扫描异常: {}", e.what());
            }
        }

        QUANT_LOG_INFO("[GapFiller] 扫描线程退出");
    }

    void do_scan_once() {
        stats_.scans_total.fetch_add(1, std::memory_order_relaxed);

        // 收集所有缺口
        std::vector<std::pair<std::string, GapInfo>> all_gaps;
        {
            std::lock_guard lock(buffers_mutex_);
            for (const auto& [sym, entry] : buffers_) {
                if (!entry.buffer) continue;
                auto gaps = detect_internal(*entry.buffer);
                for (auto& g : gaps) {
                    all_gaps.emplace_back(sym, std::move(g));
                }
            }
        }

        if (all_gaps.empty()) return;

        // 排序（近期优先）
        if (config_.prioritize_recent) {
            std::sort(all_gaps.begin(), all_gaps.end(),
                [](const auto& a, const auto& b) {
                    return a.second.start > b.second.start;
                });
        }

        // 提交任务（自动补齐开关）
        if (config_.policy != GapFillPolicy::Never && !config_.dry_run) {
            for (auto& [sym, gap] : all_gaps) {
                // 触发检测回调
                if (callbacks_.on_gap_detected) {
                    try {
                        callbacks_.on_gap_detected(gap);
                    } catch (const std::exception& e) {
                        QUANT_LOG_ERROR(
                            "[GapFiller] 检测回调异常: {}", e.what());
                    }
                }

                // 提交异步补齐
                submit_fill_task(gap, sym);
            }
        }
    }

    void submit_fill_task(const GapInfo& gap, const std::string& symbol) {
        auto task = [this, gap, symbol] {
            try {
                fill(gap, symbol);
            } catch (const std::exception& e) {
                QUANT_LOG_ERROR("[GapFiller] 异步补齐异常: {}", e.what());
            }
        };

        {
            std::lock_guard lock(work_mutex_);
            if (!running_.load(std::memory_order_acquire)) return;
            pending_tasks_.push_back(std::move(task));
        }
        work_cv_.notify_one();
    }

    // =========================================================================
    // 内部：工作线程
    // =========================================================================
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(work_mutex_);
                work_cv_.wait(lock, [this] {
                    return !running_.load(std::memory_order_acquire) ||
                           !pending_tasks_.empty();
                });

                if (!running_.load(std::memory_order_acquire) &&
                    pending_tasks_.empty()) {
                    return;
                }

                if (pending_tasks_.empty()) continue;

                task = std::move(pending_tasks_.front());
                pending_tasks_.pop_front();
            }

            try {
                task();
            } catch (const std::exception& e) {
                QUANT_LOG_ERROR("[GapFiller] 任务异常: {}", e.what());
            } catch (...) {
                QUANT_LOG_ERROR("[GapFiller] 任务未知异常");
            }
        }
    }

    // =========================================================================
    // 内部：已处理集合淘汰
    // =========================================================================
    void enforce_processed_eviction() {
        constexpr std::size_t kMaxProcessed = 10'000;
        while (processed_.size() > kMaxProcessed) {
            processed_.erase(processed_.begin());
        }
    }

    // =========================================================================
    // 成员
    // =========================================================================
    static constexpr std::size_t kWorkerThreads = 2;

    BinanceRest& rest_;
    GapFillerConfig config_;
    mutable std::mutex config_mutex_;

    // 缓冲区注册表
    mutable std::mutex buffers_mutex_;
    std::unordered_map<std::string, BufferEntry> buffers_;

    // 已处理缺口
    mutable std::mutex processed_mutex_;
    std::unordered_set<GapKey, GapKeyHash> processed_;

    // 维护窗口
    mutable std::mutex maintenance_mutex_;
    std::vector<std::pair<Timestamp, Timestamp>> maintenance_windows_;

    // 运行状态
    std::atomic<bool> running_{false};

    // 扫描线程
    std::thread scanner_;
    mutable std::mutex scan_mutex_;
    std::condition_variable scan_cv_;

    // 工作线程池
    std::vector<std::thread> workers_;
    std::mutex work_mutex_;
    std::condition_variable work_cv_;
    std::deque<std::function<void()>> pending_tasks_;

    // 回调
    GapFillerCallbacks callbacks_;

    // 统计
    GapFillerStats stats_;

    friend class GapFiller;
};

// ==============================================================================
// GapFiller<Capacity> 转发实现
// ==============================================================================

template <std::size_t BufferCapacity>
GapFiller<BufferCapacity>::GapFiller(
    BinanceRest& rest_client,
    const GapFillerConfig& config)
    : impl_{std::make_unique<Impl>(rest_client, config)}
{}

template <std::size_t BufferCapacity>
GapFiller<BufferCapacity>::~GapFiller() = default;

template <std::size_t BufferCapacity>
void GapFiller<BufferCapacity>::set_callbacks(GapFillerCallbacks callbacks) {
    impl_->callbacks_ = std::move(callbacks);
}

template <std::size_t BufferCapacity>
Result<void> GapFiller<BufferCapacity>::start() {
    return impl_->start();
}

template <std::size_t BufferCapacity>
void GapFiller<BufferCapacity>::stop() noexcept {
    impl_->stop();
}

template <std::size_t BufferCapacity>
Result<void> GapFiller<BufferCapacity>::register_buffer(
    std::string_view symbol, Buffer* buffer)
{
    return impl_->register_buffer(symbol, buffer);
}

template <std::size_t BufferCapacity>
void GapFiller<BufferCapacity>::unregister_buffer(std::string_view symbol) {
    impl_->unregister_buffer(symbol);
}

template <std::size_t BufferCapacity>
std::vector<GapInfo> GapFiller<BufferCapacity>::detect(
    std::string_view symbol) const
{
    return impl_->detect(symbol);
}

template <std::size_t BufferCapacity>
std::unordered_map<std::string, std::vector<GapInfo>>
GapFiller<BufferCapacity>::detect_all() const
{
    return impl_->detect_all();
}

template <std::size_t BufferCapacity>
FillResult GapFiller<BufferCapacity>::fill(
    const GapInfo& gap, std::string_view symbol)
{
    return impl_->fill(gap, symbol);
}

template <std::size_t BufferCapacity>
std::vector<FillResult> GapFiller<BufferCapacity>::fill_all(
    std::string_view symbol)
{
    return impl_->fill_all(symbol);
}

template <std::size_t BufferCapacity>
std::vector<FillResult> GapFiller<BufferCapacity>::fill_all_buffers() {
    return impl_->fill_all_buffers();
}

template <std::size_t BufferCapacity>
std::future<FillResult> GapFiller<BufferCapacity>::fill_async(
    const GapInfo& gap, std::string_view symbol)
{
    return impl_->fill_async(gap, symbol);
}

template <std::size_t BufferCapacity>
const GapFillerStats& GapFiller<BufferCapacity>::stats() const noexcept {
    return impl_->stats();
}

template <std::size_t BufferCapacity>
void GapFiller<BufferCapacity>::reset_stats() noexcept {
    impl_->reset_stats();
}

template <std::size_t BufferCapacity>
const GapFillerConfig& GapFiller<BufferCapacity>::config() const noexcept {
    return impl_->config();
}

template <std::size_t BufferCapacity>
void GapFiller<BufferCapacity>::set_config(const GapFillerConfig& config) {
    impl_->set_config(config);
}

template <std::size_t BufferCapacity>
bool GapFiller<BufferCapacity>::is_running() const noexcept {
    return impl_->is_running();
}

template <std::size_t BufferCapacity>
std::size_t GapFiller<BufferCapacity>::registered_buffer_count() const noexcept {
    return impl_->registered_buffer_count();
}

template <std::size_t BufferCapacity>
void GapFiller<BufferCapacity>::clear_processed_gaps() {
    impl_->clear_processed_gaps();
}

template <std::size_t BufferCapacity>
std::size_t GapFiller<BufferCapacity>::processed_gap_count() const noexcept {
    return impl_->processed_gap_count();
}

template <std::size_t BufferCapacity>
std::string GapFiller<BufferCapacity>::dump() const {
    return impl_->dump();
}

// ==============================================================================
// 显式模板实例化（下游可直接使用）
// ==============================================================================
template class GapFiller<256>;
template class GapFiller<512>;
template class GapFiller<1024>;
template class GapFiller<2048>;
template class GapFiller<4096>;
template class GapFiller<8192>;

// ==============================================================================
// 编译期校验
// ==============================================================================
static_assert(std::is_nothrow_move_constructible_v<GapInfo>,
    "GapInfo 必须可移动构造");
static_assert(std::is_nothrow_move_constructible_v<FillResult>,
    "FillResult 必须可移动构造");

}  // namespace data
}  // namespace quant
