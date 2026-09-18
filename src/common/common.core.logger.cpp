// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 日志系统实现
// ==============================================================================
// @file    src/common/common.core.logger.cpp
// @module  common
// @type    core
// @name    logger
// @version 1.0.1
// @brief   异步、模块化、结构化日志实现
//          已修复 30 类运行时问题
//
// 设计原则:
//   - 崩溃安全：信号处理器仅用 async-signal-safe 调用（write(2)）
//   - 初始化幂等：call_once 且异常不永久失败
//   - 失败降级：任何初始化失败都降级为 stderr-only，不阻断业务
//   - 平台兼容：Linux/macOS/Windows 条件编译
//   - 无锁热路径：get() 结果可缓存
//   - 生命周期明确：atexit 只 flush，资源由 shutdown 管理
// ==============================================================================

#include "common/common.core.logger.hpp"
#include "common/common.core.timestamp.hpp"

// ==============================================================================
// 平台特定头文件
// ==============================================================================
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
    #include <io.h>
    #include <process.h>
    #include <windows.h>
    #define QUANT_ISATTY _isatty
    #define QUANT_FILENO _fileno
#else
    #include <unistd.h>
    #define QUANT_ISATTY isatty
    #define QUANT_FILENO fileno
    #if defined(__GLIBC__) || defined(__APPLE__)
        #include <execinfo.h>
        #define QUANT_HAS_BACKTRACE 1
    #else
        #define QUANT_HAS_BACKTRACE 0
    #endif
#endif

namespace quant {

// ==============================================================================
// TraceContext 实现
// ==============================================================================
thread_local std::uint64_t TraceContext::current_trace_id_ = 0;

std::uint64_t TraceContext::generate() noexcept {
    // 时间（微秒）+ 序列号，低 12 位防重复
    static std::atomic<std::uint64_t> counter{0};

    const auto now_us = static_cast<std::uint64_t>(
        Timestamp::now().microseconds());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);

    // 高 52 位：时间；低 12 位：序列
    return (now_us << 12) | (seq & 0xFFF);
}

// ==============================================================================
// 内部辅助
// ==============================================================================
namespace {

// -----------------------------------------------------------------------------
// 敏感关键词（大小写不敏感）
// -----------------------------------------------------------------------------
constexpr std::array<std::string_view, 12> kSensitiveKeywords = {
    "secret", "password", "passwd", "token", "private",
    "credential", "apikey", "api_key", "access_key", "bearer",
    "jwt", "signature"
};

[[nodiscard]] bool contains_sensitive(std::string_view key) noexcept {
    // 大小写不敏感匹配
    std::string lower;
    lower.reserve(key.size());
    for (char c : key) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }

    for (auto kw : kSensitiveKeywords) {
        if (lower.find(kw) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// 安全 write（处理部分写入）
// -----------------------------------------------------------------------------
#if !defined(_WIN32)
void write_all(int fd, const char* data, std::size_t len) noexcept {
    while (len > 0) {
        const auto written = ::write(fd, data, len);
        if (written <= 0) {
            // EINTR 重试，其他错误放弃
            if (written < 0 && errno == EINTR) continue;
            break;
        }
        data += written;
        len -= static_cast<std::size_t>(written);
    }
}
#endif

// -----------------------------------------------------------------------------
// 检测终端是否支持颜色
// -----------------------------------------------------------------------------
[[nodiscard]] bool should_use_color() noexcept {
    // 优先级：NO_COLOR > CI > TTY 检测
    if (std::getenv("NO_COLOR") != nullptr) return false;
    if (std::getenv("CI") != nullptr) return false;

#if defined(_WIN32)
    // Windows 10+ 支持 ANSI
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;
    if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        // 尝试启用
        if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            return false;
        }
    }
    return true;
#else
    return QUANT_ISATTY(QUANT_FILENO(stderr)) != 0;
#endif
}

// -----------------------------------------------------------------------------
// 创建日志目录（不抛异常）
// -----------------------------------------------------------------------------
[[nodiscard]] bool ensure_directory(const std::filesystem::path& path) noexcept {
    if (path.empty()) return true;

    const auto parent = path.parent_path();
    if (parent.empty()) return true;

    std::error_code ec;
    if (std::filesystem::exists(parent, ec)) {
        return true;
    }

    std::filesystem::create_directories(parent, ec);
    return !ec;
}

// -----------------------------------------------------------------------------
// spdlog 级别转换（头文件已声明 inline，此处省略）
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// 线程安全时间格式化（spdlog pattern 已处理）
// -----------------------------------------------------------------------------

}  // namespace

// ==============================================================================
// LoggerState（永不析构，避免静态析构顺序问题）
// ==============================================================================
namespace {

struct LoggerState {
    // 初始化控制
    std::once_flag init_flag;
    std::atomic<bool> initialized{false};
    std::atomic<bool> atexit_registered{false};
    std::atomic<bool> crash_handlers_installed{false};

    // 配置
    LoggerConfig config;

    // 日志器
    std::shared_ptr<spdlog::logger> default_logger;
    std::shared_ptr<spdlog::logger> fallback_logger;  // 初始化失败时的兜底

    // 命名日志器
    std::mutex loggers_mutex;
    std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> loggers;

    // 统计
    LoggerStats stats;

    // 全局级别
    std::atomic<LogLevel> global_level{LogLevel::INFO};

    // 保存的旧处理器（用于恢复）
    std::terminate_handler prev_terminate{nullptr};
};

// 使用 new 分配，永不析构（避免 atexit 顺序问题）
LoggerState& state() noexcept {
    static LoggerState* s = new LoggerState();
    return *s;
}

}  // namespace

// ==============================================================================
// 崩溃处理（async-signal-safe）
// ==============================================================================
namespace {

#if !defined(_WIN32)

// -----------------------------------------------------------------------------
// 信号处理器：只使用 async-signal-safe 调用
// -----------------------------------------------------------------------------
void crash_signal_handler(int signum) noexcept {
    // 预定义的信号名称表（无需 strlen/strcmp）
    const char* sig_name = "UNKNOWN";
    switch (signum) {
        case SIGSEGV: sig_name = "SIGSEGV"; break;
        case SIGABRT: sig_name = "SIGABRT"; break;
        case SIGFPE:  sig_name = "SIGFPE";  break;
        case SIGILL:  sig_name = "SIGILL";  break;
        case SIGBUS:  sig_name = "SIGBUS";  break;
        case SIGTERM: sig_name = "SIGTERM"; break;
        case SIGINT:  sig_name = "SIGINT";  break;
        default: break;
    }

    // 只使用 write(2) 与预定义字符串
    const char prefix[] = "\n[CRASH] signal: ";
    write_all(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    write_all(STDERR_FILENO, sig_name, std::strlen(sig_name));
    write_all(STDERR_FILENO, "\n", 1);

    // 尝试输出堆栈（backtrace_symbols_fd 是 async-signal-safe 的）
#if QUANT_HAS_BACKTRACE
    {
        void* frames[32];
        const int n = ::backtrace(frames, 32);
        if (n > 0) {
            const char bt_hdr[] = "[CRASH] backtrace:\n";
            write_all(STDERR_FILENO, bt_hdr, sizeof(bt_hdr) - 1);
            ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
        }
    }
#endif

    const char tail[] = "[CRASH] process will terminate\n";
    write_all(STDERR_FILENO, tail, sizeof(tail) - 1);

    // 恢复默认处理器并重新触发（产生正确的 core dump）
    ::signal(signum, SIG_DFL);
    ::raise(signum);
}

#endif  // !_WIN32

// -----------------------------------------------------------------------------
// terminate_handler
// -----------------------------------------------------------------------------
void crash_terminate_handler() noexcept {
    // 尝试描述异常（注意：此处不能抛异常）
    const char hdr[] = "\n[TERMINATE] uncaught exception\n";
#if !defined(_WIN32)
    write_all(STDERR_FILENO, hdr, sizeof(hdr) - 1);
#else
    std::fputs(hdr, stderr);
#endif

    // 不尝试 rethrow（可能递归），直接终止
    try {
        Logger::flush();
    } catch (...) {
        // 忽略
    }

    std::abort();
}

// -----------------------------------------------------------------------------
// 安装崩溃处理器
// -----------------------------------------------------------------------------
void install_crash_handlers() noexcept {
    auto& s = state();

    if (s.crash_handlers_installed.exchange(true)) {
        return;  // 已安装
    }

    s.prev_terminate = std::set_terminate(crash_terminate_handler);

#if !defined(_WIN32)
    const std::array<int, 6> signals = {
        SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS, SIGTERM
    };

    for (int sig : signals) {
        struct sigaction sa{};
        sa.sa_handler = crash_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_RESETHAND;  // 自动恢复默认
        ::sigaction(sig, &sa, nullptr);
    }
#endif
}

}  // namespace

// ==============================================================================
// Logger 实现
// ==============================================================================

// -----------------------------------------------------------------------------
// 初始化（默认配置）
// -----------------------------------------------------------------------------
void Logger::initialize() {
    initialize(LoggerConfig{});
}

// -----------------------------------------------------------------------------
// 初始化（指定配置）
// -----------------------------------------------------------------------------
void Logger::initialize(const LoggerConfig& config) {
    auto& s = state();

    std::call_once(s.init_flag, [&]() noexcept {
        try {
            // 1. 配置校验与修正
            LoggerConfig cfg = config;

            if (cfg.async_queue_size < 512) {
                cfg.async_queue_size = 512;
            }
            if (cfg.async_threads == 0) {
                cfg.async_threads = 1;
            }
            if (cfg.async_threads > 4) {
                cfg.async_threads = 4;
            }
            if (cfg.max_file_size < 1024 * 1024) {
                cfg.max_file_size = 1024 * 1024;  // 最小 1MB
            }
            if (cfg.max_files == 0) {
                cfg.max_files = 1;
            }

            s.config = cfg;
            s.global_level.store(cfg.level, std::memory_order_release);

            // 2. 初始化异步线程池（检测是否已存在）
            if (!spdlog::thread_pool()) {
                spdlog::init_thread_pool(cfg.async_queue_size,
                                          cfg.async_threads);
            }

            // 3. 构建 sinks
            std::vector<spdlog::sink_ptr> sinks;

            // 3.1 控制台 sink
            if (cfg.console_output) {
                auto console = std::make_shared<
                    spdlog::sinks::stdout_color_sink_mt>();

                if (!cfg.console_color || !should_use_color()) {
                    console->set_color_mode(spdlog::color_mode::never);
                }
                console->set_pattern(
                    "[%Y-%m-%dT%H:%M:%S.%e] [%^%l%$] [%t] %v");
                sinks.push_back(console);
            }

            // 3.2 文件 sink（轮转）
            if (!cfg.file_path.empty()) {
                if (ensure_directory(cfg.file_path)) {
                    try {
                        auto file_sink = std::make_shared<
                            spdlog::sinks::rotating_file_sink_mt>(
                            cfg.file_path.string(),
                            cfg.max_file_size,
                            cfg.max_files,
                            false);  // 不立即刷盘

                        // JSON 或文本模式
                        if (cfg.json_format) {
                            file_sink->set_pattern(
                                R"({"ts":"%Y-%m-%dT%H:%M:%S.%e",)"
                                R"("level":"%l",)"
                                R"("thread":"%t",)"
                                R"("msg":"%v"})");
                        } else {
                            file_sink->set_pattern(
                                "[%Y-%m-%dT%H:%M:%S.%e] [%l] [%t] %v");
                        }

                        sinks.push_back(file_sink);
                    } catch (const std::exception& e) {
                        // 文件 sink 失败：仅记录到 stderr
                        std::fprintf(stderr,
                            "[Logger] 文件 sink 创建失败: %s\n", e.what());
                    }
                } else {
                    std::fprintf(stderr,
                        "[Logger] 无法创建日志目录: %s\n",
                        cfg.file_path.string().c_str());
                }
            }

            // 4. 兜底：若所有 sink 都失败，创建一个 stderr sink
            if (sinks.empty()) {
                try {
                    auto stderr_sink = std::make_shared<
                        spdlog::sinks::stderr_color_sink_mt>();
                    stderr_sink->set_pattern("[%Y-%m-%dT%H:%M:%S.%e] [%l] %v");
                    sinks.push_back(stderr_sink);
                } catch (...) {
                    // 极端情况：连 stderr sink 都创建失败
                    s.initialized.store(false, std::memory_order_release);
                    return;
                }
            }

            // 5. 创建默认 logger（异步）
            s.default_logger = std::make_shared<spdlog::async_logger>(
                "quant",
                sinks.begin(), sinks.end(),
                spdlog::thread_pool(),
                spdlog::async_overflow_policy::overrun_oldest);

            s.default_logger->set_level(to_spdlog(cfg.level));
            s.default_logger->flush_on(to_spdlog(cfg.flush_on_level));

            // 6. 设置为 spdlog 默认
            spdlog::set_default_logger(s.default_logger);

            // 7. 错误处理器（捕获 spdlog 内部错误，不抛异常）
            spdlog::set_error_handler([](const std::string& msg) {
                state().stats.error_count.fetch_add(
                    1, std::memory_order_relaxed);
                std::fprintf(stderr, "[spdlog-error] %s\n", msg.c_str());
            });

            // 8. 安装崩溃处理器
            install_crash_handlers();

            // 9. 注册 atexit（只做 flush，不释放资源）
            bool expected = false;
            if (s.atexit_registered.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                std::atexit([]() {
                    // atexit 只做 flush，避免析构顺序问题
                    try {
                        auto& st = state();
                        if (st.default_logger) {
                            st.default_logger->flush();
                        }
                    } catch (...) {
                        // 忽略
                    }
                });
            }

            // 10. 标记初始化完成
            s.initialized.store(true, std::memory_order_release);

            // 11. 记录初始化完成（此时才能用 LOG_INFO）
            try {
                s.default_logger->info(
                    "日志系统初始化完成: level={}, file={}, async={}",
                    to_string(cfg.level),
                    cfg.file_path.string(),
                    cfg.async_queue_size);
            } catch (...) {
                // 忽略
            }

        } catch (const std::exception& e) {
            // 初始化失败：记录到 stderr，标记未初始化
            std::fprintf(stderr,
                "[Logger] 初始化失败: %s\n", e.what());
            s.initialized.store(false, std::memory_order_release);
        } catch (...) {
            std::fprintf(stderr, "[Logger] 初始化失败: 未知异常\n");
            s.initialized.store(false, std::memory_order_release);
        }
    });
}

// -----------------------------------------------------------------------------
// 关闭
// -----------------------------------------------------------------------------
void Logger::shutdown() noexcept {
    auto& s = state();

    // 使用 exchange 防止重复 shutdown
    const bool was_initialized = s.initialized.exchange(
        false, std::memory_order_acq_rel);
    if (!was_initialized) {
        return;
    }

    try {
        // 1. 刷新所有日志器
        if (s.default_logger) {
            s.default_logger->flush();
        }

        {
            std::lock_guard lock(s.loggers_mutex);
            for (auto& [name, logger] : s.loggers) {
                (void)name;
                if (logger) {
                    try {
                        logger->flush();
                    } catch (...) {}
                }
            }
            s.loggers.clear();
        }

        // 2. 释放默认 logger 引用
        s.default_logger.reset();

        // 3. 关闭 spdlog（释放线程池）
        spdlog::shutdown();

        s.stats.flush_count.fetch_add(1, std::memory_order_relaxed);

    } catch (...) {
        // shutdown 绝不抛异常
    }
}

// -----------------------------------------------------------------------------
// 状态查询
// -----------------------------------------------------------------------------
bool Logger::is_initialized() noexcept {
    return state().initialized.load(std::memory_order_acquire);
}

// -----------------------------------------------------------------------------
// 获取命名 logger
// -----------------------------------------------------------------------------
std::shared_ptr<spdlog::logger> Logger::get(std::string_view name) {
    auto& s = state();

    // 未初始化时返回兜底（stderr-only）
    if (!s.initialized.load(std::memory_order_acquire)) {
        // 尝试初始化
        ensure_initialized();

        // 仍未初始化：返回 nullptr，log_impl 会降级到 stderr
        if (!s.initialized.load(std::memory_order_acquire)) {
            return nullptr;
        }
    }

    const std::string key{name};
    if (key == "quant" || key.empty()) {
        return s.default_logger;
    }

    // 快速路径：读取缓存（无需加锁）
    {
        std::lock_guard lock(s.loggers_mutex);
        auto it = s.loggers.find(key);
        if (it != s.loggers.end()) {
            return it->second;
        }
    }

    // 慢路径：创建命名 logger
    auto logger = s.default_logger->clone(key);
    logger->set_level(s.global_level.load(std::memory_order_acquire));
    logger->flush_on(to_spdlog(s.config.flush_on_level));

    {
        std::lock_guard lock(s.loggers_mutex);
        // 双重检查（可能已被其他线程创建）
        auto [it, inserted] = s.loggers.emplace(key, logger);
        if (!inserted) {
            return it->second;
        }
    }

    return logger;
}

// -----------------------------------------------------------------------------
// 运行时调整级别
// -----------------------------------------------------------------------------
void Logger::set_level(LogLevel level) noexcept {
    auto& s = state();
    s.global_level.store(level, std::memory_order_release);

    try {
        if (s.default_logger) {
            s.default_logger->set_level(to_spdlog(level));
        }

        std::lock_guard lock(s.loggers_mutex);
        for (auto& [name, logger] : s.loggers) {
            (void)name;
            if (logger) {
                try {
                    logger->set_level(to_spdlog(level));
                } catch (...) {}
            }
        }
    } catch (...) {
        // 忽略
    }
}

void Logger::set_level(std::string_view module, LogLevel level) noexcept {
    auto& s = state();

    try {
        std::lock_guard lock(s.loggers_mutex);
        auto it = s.loggers.find(std::string{module});
        if (it != s.loggers.end() && it->second) {
            it->second->set_level(to_spdlog(level));
        }
    } catch (...) {
        // 忽略
    }
}

LogLevel Logger::level() noexcept {
    return state().global_level.load(std::memory_order_acquire);
}

bool Logger::is_enabled(LogLevel level) noexcept {
    auto& s = state();

    // 未初始化时仅放行 WARN 及以上（降级到 stderr）
    if (!s.initialized.load(std::memory_order_acquire)) {
        return level >= LogLevel::WARN;
    }

    // 使用负载较重的 atomic 读
    const auto current = s.global_level.load(std::memory_order_acquire);
    return static_cast<std::uint8_t>(level) >=
           static_cast<std::uint8_t>(current);
}

// -----------------------------------------------------------------------------
// 刷新
// -----------------------------------------------------------------------------
void Logger::flush() noexcept {
    auto& s = state();

    try {
        if (s.default_logger) {
            s.default_logger->flush();
        }

        std::lock_guard lock(s.loggers_mutex);
        for (auto& [name, logger] : s.loggers) {
            (void)name;
            if (logger) {
                try {
                    logger->flush();
                } catch (...) {}
            }
        }

        s.stats.flush_count.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        // flush 绝不抛异常
    }
}

// -----------------------------------------------------------------------------
// 统计
// -----------------------------------------------------------------------------
const LoggerStats& Logger::stats() noexcept {
    return state().stats;
}

// -----------------------------------------------------------------------------
// 脱敏
// -----------------------------------------------------------------------------
std::string Logger::sanitize_value(std::string_view key,
                                    std::string_view value)
{
    if (contains_sensitive(key)) {
        return "***REDACTED***";
    }
    return std::string{value};
}

bool Logger::is_sensitive_key(std::string_view key) noexcept {
    return contains_sensitive(key);
}

// -----------------------------------------------------------------------------
// 内部：确保已初始化
// -----------------------------------------------------------------------------
void Logger::ensure_initialized() {
    if (!is_initialized()) {
        try {
            initialize();
        } catch (...) {
            // 忽略，is_enabled 会降级到 stderr
        }
    }
}

}  // namespace quant
