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
brief: L0 内核配置管理（默认值/环境变量/schema校验/热重载/线程安全/密钥脱敏）

provides:
  - class Config
  - struct ConfigSnapshot

requires:
  modules: [kernel.core.types, kernel.core.logger]
  libraries: [nlohmann_json]
  resources:
    memory_mb: 16

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

fallback:
  - module: kernel
    name: config_default

tags: [kernel, config, foundation]
---UMS-END--- */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <optional>
#include <functional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <algorithm>

#include <nlohmann/json.hpp>

#include "kernel.L0.core.types.hpp"

namespace quant::kernel {

// ═══════════════════════════════════════════════════
// 配置版本
// ═══════════════════════════════════════════════════
constexpr int CONFIG_VERSION = 1;

// ═══════════════════════════════════════════════════
// 敏感键（日志脱敏）
// ═══════════════════════════════════════════════════
inline const std::vector<std::string> SENSITIVE_KEYS = {
    "api_key", "api_secret", "password", "secret", "token",
    "private_key", "db_password", "redis_password"
};

// ═══════════════════════════════════════════════════
// 配置快照（用于回滚）
// ═══════════════════════════════════════════════════
struct ConfigSnapshot {
    int64_t     timestamp_ms;
    int         version;
    nlohmann::json data;
    std::string  reason;
};

// ═══════════════════════════════════════════════════
// Config 主类
// ═══════════════════════════════════════════════════
class Config {
public:
    // ─────────── 单例（线程安全） ───────────
    static Config& instance() {
        static Config inst;
        return inst;
    }

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // ─────────── 加载 ───────────
    // 优先级：默认值 < 文件 < 环境变量
    bool load(const std::string& path,
              bool allow_missing = true) {
        std::unique_lock lock(mutex_);

        // 1. 默认值
        json_ = defaults();

        // 2. 文件覆盖
        if (!path.empty()) {
            if (std::filesystem::exists(path)) {
                try {
                    std::ifstream f(path);
                    if (!f.is_open()) {
                        last_error_ = "无法打开文件: " + path;
                        if (!allow_missing) return false;
                    } else {
                        auto file_json = nlohmann::json::parse(f);
                        merge_json(json_, file_json);
                        source_path_ = path;
                    }
                } catch (const std::exception& e) {
                    last_error_ = "配置解析失败: " + std::string(e.what());
                    if (!allow_missing) return false;
                }
            } else {
                last_error_ = "配置文件不存在: " + path;
                if (!allow_missing) return false;
            }
        }

        // 3. 环境变量覆盖（QUANT_* 前缀）
        apply_env_overrides();

        // 4. 版本检查
        if (json_.contains("config_version")) {
            int v = json_["config_version"].get<int>();
            if (v > CONFIG_VERSION) {
                last_error_ = "配置版本过高: " + std::to_string(v);
                return false;
            }
        } else {
            json_["config_version"] = CONFIG_VERSION;
        }

        // 5. 必填项检查
        if (!validate_required()) {
            return false;
        }

        // 6. 生成快照
        save_snapshot("load");

        last_error_.clear();
        return true;
    }

    // 热重载
    bool reload() {
        return load(source_path_, false);
    }

    // ─────────── 读取（模板 + 默认值） ───────────
    template<typename T>
    T get(const std::string& path, const T& default_value = T{}) const {
        std::shared_lock lock(mutex_);
        try {
            auto it = resolve_path(json_, path);
            if (it == nullptr) return default_value;
            if (it->is_null()) return default_value;
            return it->get<T>();
        } catch (const std::exception&) {
            // 类型不匹配时尝试转换
            try {
                return coerce<T>(json_, path, default_value);
            } catch (...) {
                return default_value;
            }
        }
    }

    // 特化：字符串
    std::string get_string(const std::string& path,
                           const std::string& def = "") const {
        return get<std::string>(path, def);
    }

    // 特化：整型
    int get_int(const std::string& path, int def = 0) const {
        auto v = get<double>(path, static_cast<double>(def));
        return static_cast<int>(v);
    }

    // 特化：浮点
    double get_double(const std::string& path, double def = 0.0) const {
        return get<double>(path, def);
    }

    // 特化：布尔
    bool get_bool(const std::string& path, bool def = false) const {
        return get<bool>(path, def);
    }

    // 范围校验的整型
    int get_int_range(const std::string& path, int def,
                      int min_val, int max_val) const {
        int v = get_int(path, def);
        return std::clamp(v, min_val, max_val);
    }

    double get_double_range(const std::string& path, double def,
                            double min_val, double max_val) const {
        double v = get_double(path, def);
        if (!std::isfinite(v)) return def;
        return std::clamp(v, min_val, max_val);
    }

    // 字符串枚举
    std::string get_enum(const std::string& path,
                         const std::vector<std::string>& allowed,
                         const std::string& def) const {
        auto v = get<std::string>(path, def);
        if (std::find(allowed.begin(), allowed.end(), v) == allowed.end()) {
            return def;
        }
        return v;
    }

    // ─────────── 写入（运行时，线程安全） ───────────
    bool set(const std::string& path, const nlohmann::json& value) {
        std::unique_lock lock(mutex_);
        try {
            set_by_path(json_, path, value);
            save_snapshot("set:" + path);
            return true;
        } catch (const std::exception& e) {
            last_error_ = std::string("设置失败: ") + e.what();
            return false;
        }
    }

    // ─────────── 环境变量覆盖 ───────────
    void apply_env_overrides() {
        // 环境变量格式：QUANT_STRATEGY__BAND_K → strategy.band_k
        // 双下划线分隔路径层级
        for (char** env = environ; *env != nullptr; env++) {
            std::string entry(*env);
            auto eq = entry.find('=');
            if (eq == std::string::npos) continue;

            std::string key = entry.substr(0, eq);
            std::string val = entry.substr(eq + 1);

            if (key.rfind("QUANT_", 0) != 0) continue;

            std::string path = key.substr(6);  // 去掉 QUANT_
            std::transform(path.begin(), path.end(), path.begin(), ::tolower);

            // __ 替换为 .
            std::string normalized;
            for (size_t i = 0; i < path.size(); i++) {
                if (i + 1 < path.size() && path[i] == '_' && path[i+1] == '_') {
                    normalized += '.';
                    i++;
                } else {
                    normalized += path[i];
                }
            }

            // 尝试解析为 JSON 值，失败则按字符串
            try {
                auto parsed = nlohmann::json::parse(val);
                set_by_path(json_, normalized, parsed);
            } catch (...) {
                set_by_path(json_, normalized, val);
            }
        }
    }

    // ─────────── 必填项校验 ───────────
    bool validate_required() const {
        for (auto& key : required_keys_) {
            auto it = resolve_path(json_, key);
            if (it == nullptr || it->is_null()) {
                last_error_ = "缺少必填配置: " + key;
                return false;
            }
        }
        return true;
    }

    void add_required(const std::string& key) {
        std::unique_lock lock(mutex_);
        required_keys_.push_back(key);
    }

    // ─────────── 日志脱敏 ───────────
    nlohmann::json to_safe_json() const {
        std::shared_lock lock(mutex_);
        auto copy = json_;
        mask_json(copy);
        return copy;
    }

    std::string dump_safe() const {
        return to_safe_json().dump(2);
    }

    // ─────────── 快照与回滚 ───────────
    void save_snapshot(const std::string& reason) {
        ConfigSnapshot snap;
        snap.timestamp_ms = now_ms();
        snap.version = json_.value("config_version", CONFIG_VERSION);
        snap.data = json_;
        snap.reason = reason;

        snapshots_.push_back(std::move(snap));
        if (snapshots_.size() > MAX_SNAPSHOTS) {
            snapshots_.erase(snapshots_.begin());
        }
    }

    bool rollback(size_t steps_back = 1) {
        std::unique_lock lock(mutex_);
        if (snapshots_.size() <= steps_back) return false;
        json_ = snapshots_[snapshots_.size() - 1 - steps_back].data;
        return true;
    }

    // ─────────── 健康检查 ───────────
    bool health() const {
        std::shared_lock lock(mutex_);
        return !json_.empty() && last_error_.empty();
    }

    std::string last_error() const {
        std::shared_lock lock(mutex_);
        return last_error_;
    }

    std::string source_path() const {
        std::shared_lock lock(mutex_);
        return source_path_;
    }

    // ─────────── 变更监听 ───────────
    using ChangeCallback = std::function<void(const std::string& path)>;
    void on_change(ChangeCallback cb) {
        std::unique_lock lock(mutex_);
        change_callbacks_.push_back(std::move(cb));
    }

private:
    // ─────────── 构造 ───────────
    Config() : json_(defaults()) {}
    ~Config() = default;

    // ─────────── 默认配置 ───────────
    static nlohmann::json defaults() {
        return nlohmann::json{
            {"config_version", CONFIG_VERSION},
            {"log_level", "INFO"},
            {"log_path", "logs/quant.log"},
            {"strategy", {
                {"ema_period", 26},
                {"ema_fast", 12},
                {"ema_slow", 26},
                {"band_k", 1.0},
                {"entry_threshold", 60.0},
                {"escape_speed_fast", 1.5},
                {"escape_speed_slow", 0.5},
                {"escape_relax_factor", 1.5},
                {"escape_tighten_factor", 0.7}
            }},
            {"risk", {
                {"stop_atr", 1.5},
                {"max_risk_per_trade", 0.02},
                {"daily_max_loss", 0.05},
                {"max_leverage", 5}
            }},
            {"order", {
                {"execution_mode", "VIRTUAL"},
                {"leverage", 3},
                {"maker_fee", 0.0002},
                {"taker_fee", 0.0004}
            }},
            {"ai", {
                {"enabled", true},
                {"ollama_host", "http://localhost:11434"},
                {"model", "deepseek-r1:14b"},
                {"quantization", "q4_K_M"},
                {"suggestion_mode", "record_only"}
            }},
            {"binance", {
                {"ws_host", "fstream.binance.com"},
                {"rest_host", "https://fapi.binance.com"},
                {"testnet", true}
            }},
            {"database", {
                {"host", "localhost"},
                {"port", 5432},
                {"name", "quant"},
                {"user", "quant"}
            }},
            {"redis", {
                {"host", "localhost"},
                {"port", 6379}
            }}
        };
    }

    // ─────────── 路径解析（支持点号） ───────────
    static const nlohmann::json* resolve_path(
        const nlohmann::json& j, const std::string& path) {
        
        if (path.empty()) return &j;

        const nlohmann::json* cur = &j;
        size_t start = 0;

        while (start < path.size()) {
            size_t dot = path.find('.', start);
            std::string key = (dot == std::string::npos)
                ? path.substr(start)
                : path.substr(start, dot - start);

            if (!cur->is_object() || !cur->contains(key)) return nullptr;
            cur = &(*cur)[key];

            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return cur;
    }

    // ─────────── 路径写入 ───────────
    static void set_by_path(nlohmann::json& j,
                            const std::string& path,
                            const nlohmann::json& value) {
        if (path.empty()) {
            j = value;
            return;
        }

        nlohmann::json* cur = &j;
        size_t start = 0;

        while (start < path.size()) {
            size_t dot = path.find('.', start);
            std::string key = (dot == std::string::npos)
                ? path.substr(start)
                : path.substr(start, dot - start);

            if (dot == std::string::npos) {
                (*cur)[key] = value;
                return;
            }

            if (!cur->contains(key) || !(*cur)[key].is_object()) {
                (*cur)[key] = nlohmann::json::object();
            }
            cur = &(*cur)[key];
            start = dot + 1;
        }
    }

    // ─────────── 深度合并 ───────────
    static void merge_json(nlohmann::json& base,
                           const nlohmann::json& overlay) {
        if (!overlay.is_object()) {
            base = overlay;
            return;
        }
        for (auto it = overlay.begin(); it != overlay.end(); ++it) {
            if (base.contains(it.key()) && base[it.key()].is_object()
                && it.value().is_object()) {
                merge_json(base[it.key()], it.value());
            } else {
                base[it.key()] = it.value();
            }
        }
    }

    // ─────────── 类型强制转换 ───────────
    template<typename T>
    static T coerce(const nlohmann::json& j,
                    const std::string& path,
                    const T& def) {
        auto* val = resolve_path(j, path);
        if (val == nullptr) return def;

        // 数值互转
        if constexpr (std::is_arithmetic_v<T>) {
            if (val->is_number()) return val->get<T>();
            if (val->is_string()) {
                try { return static_cast<T>(std::stod(val->get<std::string>())); }
                catch (...) { return def; }
            }
        }
        // 字符串
        if constexpr (std::is_same_v<T, std::string>) {
            if (val->is_string()) return val->get<std::string>();
            return val->dump();
        }
        // 布尔
        if constexpr (std::is_same_v<T, bool>) {
            if (val->is_boolean()) return val->get<bool>();
            if (val->is_number()) return val->get<double>() != 0.0;
        }
        return def;
    }

    // ─────────── 脱敏 ───────────
    static void mask_json(nlohmann::json& j) {
        if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                std::string key = it.key();
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);

                bool sensitive = false;
                for (auto& s : SENSITIVE_KEYS) {
                    if (key.find(s) != std::string::npos) {
                        sensitive = true;
                        break;
                    }
                }

                if (sensitive && it.value().is_string()) {
                    std::string v = it.value().get<std::string>();
                    if (v.size() > 8) {
                        it.value() = v.substr(0, 4) + "****" +
                                     v.substr(v.size() - 4);
                    } else if (!v.empty()) {
                        it.value() = "****";
                    }
                } else if (it.value().is_object() || it.value().is_array()) {
                    mask_json(it.value());
                }
            }
        } else if (j.is_array()) {
            for (auto& item : j) mask_json(item);
        }
    }

    // ─────────── 时间 ───────────
    static int64_t now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
    }

    // ─────────── 成员 ───────────
    mutable std::shared_mutex mutex_;
    nlohmann::json            json_;
    std::string               source_path_;
    std::string               last_error_;
    std::vector<std::string>  required_keys_;
    std::vector<ConfigSnapshot> snapshots_;
    std::vector<ChangeCallback> change_callbacks_;

    static constexpr size_t MAX_SNAPSHOTS = 10;

    // 环境变量遍历（Linux/macOS）
    #if defined(__linux__) || defined(__APPLE__)
    extern char** environ;
    #endif
};

// ═══════════════════════════════════════════════════
// 便捷宏（读取带默认值的配置）
// ═══════════════════════════════════════════════════
#define QUANT_CFG_STR(path, def)   \
    quant::kernel::Config::instance().get_string((path), (def))
#define QUANT_CFG_INT(path, def)   \
    quant::kernel::Config::instance().get_int((path), (def))
#define QUANT_CFG_DBL(path, def)   \
    quant::kernel::Config::instance().get_double((path), (def))
#define QUANT_CFG_BOOL(path, def)  \
    quant::kernel::Config::instance().get_bool((path), (def))

} // namespace quant::kernel
