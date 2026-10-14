/* ---UMS-BEGIN---
spec_version: 1.0
module: kernel
type: core
name: config
version: 1.0.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: L0 内核配置管理 (文件+环境变量+热重载+观察者)

provides:
  - class Config
  - struct ConfigSnapshot

requires:
  modules: [kernel.core.types, kernel.core.logger]
  libraries: [nlohmann_json]

interface:
  contract: kernel.L0.contract.config.json

health:
  fn: Config::health
  timeout_ms: 100

assembly:
  order: 2
  parallel: false

tests:
  - tests/test_kernel_config.cpp

tags: [kernel, config, hot-reload]
---UMS-END--- */

#include "kernel.L0.core.config.hpp"
#include "kernel.L0.core.logger.hpp"

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace quant::kernel {

// ═══════════════════════════════════════════════════
// 内部状态（PIMPL 风格，隐藏实现）
// ═══════════════════════════════════════════════════
namespace {

constexpr int    MIN_EMA_PERIOD   = 5;
constexpr int    MAX_EMA_PERIOD   = 200;
constexpr double MIN_BAND_K       = 0.1;
constexpr double MAX_BAND_K       = 5.0;
constexpr double MIN_ENTRY_THRESH = 0.0;
constexpr double MAX_ENTRY_THRESH = 100.0;
constexpr double MIN_STOP_ATR     = 0.1;
constexpr double MAX_STOP_ATR     = 10.0;
constexpr double MIN_RISK_PCT     = 0.001;
constexpr double MAX_RISK_PCT     = 0.10;
constexpr int    MIN_DB_PORT      = 1;
constexpr int    MAX_DB_PORT      = 65535;

// 敏感键（禁止出现在日志）
const std::vector<std::string> SENSITIVE_KEYS = {
    "api_key", "api_secret", "password", "token", "secret",
    "private_key", "passphrase"
};

bool is_sensitive(const std::string& key) {
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& s : SENSITIVE_KEYS) {
        if (lower.find(s) != std::string::npos) return true;
    }
    return false;
}

// 去除 UTF-8 BOM
std::string strip_bom(const std::string& content) {
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        return content.substr(3);
    }
    return content;
}

// 环境变量读取（带默认值）
std::optional<std::string> get_env(const std::string& key) {
    const char* v = std::getenv(key.c_str());
    if (v == nullptr) return std::nullopt;
    return std::string(v);
}

// 安全解析 int
std::optional<int> parse_int(const std::string& s) {
    try {
        size_t pos = 0;
        int v = std::stoi(s, &pos);
        if (pos != s.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

// 安全解析 double
std::optional<double> parse_double(const std::string& s) {
    try {
        size_t pos = 0;
        double v = std::stod(s, &pos);
        if (pos != s.size()) return std::nullopt;
        if (!std::isfinite(v)) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════
// ConfigSnapshot 实现
// ═══════════════════════════════════════════════════
struct ConfigSnapshot {
    int    version = 0;

    // strategy
    int    ema_period      = 26;
    double band_k          = 1.0;
    double entry_threshold = 60.0;
    double escape_speed_fast = 1.5;
    double escape_speed_slow = 0.5;

    // risk
    double stop_atr         = 1.5;
    double risk_per_trade   = 0.02;
    double daily_max_loss   = 0.05;

    // database
    std::string db_host = "localhost";
    int         db_port = 5432;
    std::string db_name = "quant";

    // redis
    std::string redis_host = "localhost";
    int         redis_port = 6379;

    // binance
    std::string api_key;
    std::string api_secret;
    bool        testnet = true;

    // ai
    std::string ollama_host = "http://localhost:11434";
    std::string ai_model    = "deepseek-r1:14b";
    bool        ai_enabled  = true;

    // 序列化（敏感字段脱敏）
    nlohmann::json to_json_safe() const {
        nlohmann::json j;
        j["version"] = version;
        j["strategy"]["ema_period"]      = ema_period;
        j["strategy"]["band_k"]          = band_k;
        j["strategy"]["entry_threshold"] = entry_threshold;
        j["strategy"]["escape_speed_fast"] = escape_speed_fast;
        j["strategy"]["escape_speed_slow"] = escape_speed_slow;
        j["risk"]["stop_atr"]       = stop_atr;
        j["risk"]["risk_per_trade"] = risk_per_trade;
        j["risk"]["daily_max_loss"] = daily_max_loss;
        j["database"]["host"] = db_host;
        j["database"]["port"] = db_port;
        j["database"]["name"] = db_name;
        j["redis"]["host"] = redis_host;
        j["redis"]["port"] = redis_port;
        j["binance"]["api_key"]    = api_key.empty()    ? "" : "***";
        j["binance"]["api_secret"] = api_secret.empty() ? "" : "***";
        j["binance"]["testnet"]    = testnet;
        j["ai"]["ollama_host"] = ollama_host;
        j["ai"]["model"]       = ai_model;
        j["ai"]["enabled"]     = ai_enabled;
        return j;
    }
};

// ═══════════════════════════════════════════════════
// Config 单例（Meyers Singleton，C++11 线程安全）
// ═══════════════════════════════════════════════════
class ConfigImpl {
public:
    ConfigImpl() = default;
    ~ConfigImpl() = default;

    bool load_from_file(const std::string& path) {
        namespace fs = std::filesystem;

        if (!fs::exists(path)) {
            log_error("配置文件不存在: " + path);
            return false;
        }

        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) {
            log_error("配置文件无法打开: " + path);
            return false;
        }

        std::stringstream ss;
        ss << f.rdbuf();
        std::string content = strip_bom(ss.str());

        if (content.empty()) {
            log_error("配置文件为空: " + path);
            return false;
        }

        try {
            auto j = nlohmann::json::parse(content);
            return apply_json(j);
        } catch (const nlohmann::json::exception& e) {
            log_error("配置JSON解析失败: " + std::string(e.what()));
            return false;
        }
    }

    bool apply_json(const nlohmann::json& j) {
        ConfigSnapshot snap;

        // 使用 value() 提供默认值，防止字段缺失崩溃
        if (j.contains("strategy")) {
            auto& s = j["strategy"];
            snap.ema_period        = s.value("ema_period", snap.ema_period);
            snap.band_k            = s.value("band_k", snap.band_k);
            snap.entry_threshold   = s.value("entry_threshold", snap.entry_threshold);
            snap.escape_speed_fast = s.value("escape_speed_fast", snap.escape_speed_fast);
            snap.escape_speed_slow = s.value("escape_speed_slow", snap.escape_speed_slow);
        }
        if (j.contains("risk")) {
            auto& r = j["risk"];
            snap.stop_atr       = r.value("stop_atr", snap.stop_atr);
            snap.risk_per_trade = r.value("risk_per_trade", snap.risk_per_trade);
            snap.daily_max_loss = r.value("daily_max_loss", snap.daily_max_loss);
        }
        if (j.contains("database")) {
            auto& d = j["database"];
            snap.db_host = d.value("host", snap.db_host);
            snap.db_port = d.value("port", snap.db_port);
            snap.db_name = d.value("name", snap.db_name);
        }
        if (j.contains("redis")) {
            auto& r = j["redis"];
            snap.redis_host = r.value("host", snap.redis_host);
            snap.redis_port = r.value("port", snap.redis_port);
        }
        if (j.contains("binance")) {
            auto& b = j["binance"];
            snap.api_key    = b.value("api_key", "");
            snap.api_secret = b.value("api_secret", "");
            snap.testnet    = b.value("testnet", true);
        }
        if (j.contains("ai")) {
            auto& a = j["ai"];
            snap.ollama_host = a.value("ollama_host", snap.ollama_host);
            snap.ai_model    = a.value("model", snap.ai_model);
            snap.ai_enabled  = a.value("enabled", snap.ai_enabled);
        }

        apply_env_overrides(snap);

        if (!validate(snap)) return false;

        // 原子替换
        {
            std::unique_lock lock(mutex_);
            snap.version = ++version_counter_;
            snapshot_ = snap;
        }

        notify_observers();
        return true;
    }

    void apply_env_overrides(ConfigSnapshot& snap) {
        // 环境变量优先级最高（云端部署关键）
        if (auto v = get_env("BINANCE_API_KEY"))    snap.api_key = *v;
        if (auto v = get_env("BINANCE_API_SECRET")) snap.api_secret = *v;
        if (auto v = get_env("BINANCE_TESTNET"))    snap.testnet = (*v == "true" || *v == "1");

        if (auto v = get_env("DB_HOST")) snap.db_host = *v;
        if (auto v = get_env("DB_PORT")) {
            if (auto p = parse_int(*v)) snap.db_port = *p;
            else log_warning("环境变量 DB_PORT 无效: " + *v);
        }
        if (auto v = get_env("DB_NAME")) snap.db_name = *v;

        if (auto v = get_env("REDIS_HOST")) snap.redis_host = *v;
        if (auto v = get_env("REDIS_PORT")) {
            if (auto p = parse_int(*v)) snap.redis_port = *p;
            else log_warning("环境变量 REDIS_PORT 无效: " + *v);
        }

        if (auto v = get_env("OLLAMA_HOST")) snap.ollama_host = *v;
        if (auto v = get_env("AI_MODEL"))    snap.ai_model    = *v;
        if (auto v = get_env("AI_ENABLED"))  snap.ai_enabled  = (*v == "true" || *v == "1");
    }

    bool validate(const ConfigSnapshot& s) {
        bool ok = true;

        auto check_range = [&](const std::string& name, double v,
                                double lo, double hi) {
            if (v < lo || v > hi) {
                log_error("配置 " + name + " 超出范围 [" +
                          std::to_string(lo) + ", " + std::to_string(hi) +
                          "], 当前=" + std::to_string(v));
                ok = false;
            }
        };

        check_range("ema_period",      s.ema_period,      MIN_EMA_PERIOD,   MAX_EMA_PERIOD);
        check_range("band_k",          s.band_k,          MIN_BAND_K,       MAX_BAND_K);
        check_range("entry_threshold", s.entry_threshold, MIN_ENTRY_THRESH, MAX_ENTRY_THRESH);
        check_range("stop_atr",        s.stop_atr,        MIN_STOP_ATR,     MAX_STOP_ATR);
        check_range("risk_per_trade",  s.risk_per_trade,  MIN_RISK_PCT,     MAX_RISK_PCT);
        check_range("db_port",         s.db_port,         MIN_DB_PORT,      MAX_DB_PORT);
        check_range("redis_port",      s.redis_port,      MIN_DB_PORT,      MAX_DB_PORT);

        // 必填项（实盘模式必须有密钥）
        if (!s.testnet) {
            if (s.api_key.empty()) {
                log_error("实盘模式必须提供 BINANCE_API_KEY");
                ok = false;
            }
            if (s.api_secret.empty()) {
                log_error("实盘模式必须提供 BINANCE_API_SECRET");
                ok = false;
            }
        }

        return ok;
    }

    ConfigSnapshot get() const {
        std::shared_lock lock(mutex_);
        return snapshot_;
    }

    bool set(const std::string& key, double value) {
        auto snap = get();
        if (key == "ema_period")           snap.ema_period = static_cast<int>(value);
        else if (key == "band_k")          snap.band_k = value;
        else if (key == "entry_threshold") snap.entry_threshold = value;
        else if (key == "stop_atr")        snap.stop_atr = value;
        else if (key == "risk_per_trade")  snap.risk_per_trade = value;
        else if (key == "escape_speed_fast") snap.escape_speed_fast = value;
        else if (key == "escape_speed_slow") snap.escape_speed_slow = value;
        else {
            log_warning("未知配置键: " + key);
            return false;
        }

        if (!validate(snap)) return false;

        {
            std::unique_lock lock(mutex_);
            snap.version = ++version_counter_;
            snapshot_ = snap;
        }
        notify_observers();
        return true;
    }

    void subscribe(std::function<void(const ConfigSnapshot&)> cb) {
        std::unique_lock lock(observer_mutex_);
        observers_.push_back(std::move(cb));
    }

    bool health() const {
        std::shared_lock lock(mutex_);
        return snapshot_.version > 0;
    }

private:
    void notify_observers() {
        auto snap = get();
        std::shared_lock lock(observer_mutex_);
        for (auto& cb : observers_) {
            try {
                cb(snap);
            } catch (const std::exception& e) {
                log_error("配置观察者回调异常: " + std::string(e.what()));
            }
        }
    }

    mutable std::shared_mutex mutex_;
    ConfigSnapshot snapshot_;
    std::atomic<int> version_counter_{0};

    mutable std::shared_mutex observer_mutex_;
    std::vector<std::function<void(const ConfigSnapshot&)>> observers_;
};

// ═══════════════════════════════════════════════════
// Config 公共接口（转发到 Impl）
// ═══════════════════════════════════════════════════
namespace {
    ConfigImpl& impl() {
        static ConfigImpl instance;  // Meyers Singleton: C++11 线程安全
        return instance;
    }
}

Config& Config::instance() {
    static Config instance;  // 同样线程安全
    return instance;
}

bool Config::load(const std::string& path) {
    config_path_ = path;
    bool ok = impl().load_from_file(path);
    if (ok) {
        log_info("配置加载成功: " + path);
        auto snap = impl().get();
        // 脱敏输出（不打印密钥）
        log_info("Config: ema_period=" + std::to_string(snap.ema_period) +
                 " band_k=" + std::to_string(snap.band_k) +
                 " entry_threshold=" + std::to_string(snap.entry_threshold) +
                 " testnet=" + (snap.testnet ? "true" : "false"));
    }
    return ok;
}

bool Config::reload() {
    if (config_path_.empty()) {
        log_error("配置路径未设置，无法 reload");
        return false;
    }
    return load(config_path_);
}

int Config::version() const {
    return impl().get().version;
}

nlohmann::json Config::snapshot_json() const {
    return impl().get().to_json_safe();
}

// 强类型 getter
int    Config::ema_period()      const { return impl().get().ema_period; }
double Config::band_k()          const { return impl().get().band_k; }
double Config::entry_threshold() const { return impl().get().entry_threshold; }
double Config::stop_atr()        const { return impl().get().stop_atr; }
double Config::risk_per_trade()  const { return impl().get().risk_per_trade; }
double Config::daily_max_loss()  const { return impl().get().daily_max_loss; }
std::string Config::db_host()    const { return impl().get().db_host; }
int    Config::db_port()         const { return impl().get().db_port; }
std::string Config::api_key()    const { return impl().get().api_key; }
std::string Config::api_secret() const { return impl().get().api_secret; }
bool   Config::testnet()         const { return impl().get().testnet; }

// 强类型 setter（带校验）
bool Config::set_band_k(double v) {
    if (v < MIN_BAND_K || v > MAX_BAND_K) {
        log_error("band_k 超出范围: " + std::to_string(v));
        return false;
    }
    return impl().set("band_k", v);
}

bool Config::set_ema_period(int v) {
    if (v < MIN_EMA_PERIOD || v > MAX_EMA_PERIOD) {
        log_error("ema_period 超出范围: " + std::to_string(v));
        return false;
    }
    return impl().set("ema_period", static_cast<double>(v));
}

bool Config::set_entry_threshold(double v) {
    if (v < MIN_ENTRY_THRESH || v > MAX_ENTRY_THRESH) {
        log_error("entry_threshold 超出范围: " + std::to_string(v));
        return false;
    }
    return impl().set("entry_threshold", v);
}

bool Config::set_stop_atr(double v) {
    if (v < MIN_STOP_ATR || v > MAX_STOP_ATR) {
        log_error("stop_atr 超出范围: " + std::to_string(v));
        return false;
    }
    return impl().set("stop_atr", v);
}

bool Config::set_risk_per_trade(double v) {
    if (v < MIN_RISK_PCT || v > MAX_RISK_PCT) {
        log_error("risk_per_trade 超出范围: " + std::to_string(v));
        return false;
    }
    return impl().set("risk_per_trade", v);
}

void Config::subscribe(std::function<void(const ConfigSnapshot&)> cb) {
    impl().subscribe(std::move(cb));
}

bool Config::health() const {
    return impl().health();
}

} // namespace quant::kernel
