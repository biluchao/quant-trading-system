/* ---UMS-BEGIN---
spec_version: 1.0
module: kernel
type: core
name: config
version: 1.1.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: Config 实现（默认值/schema校验/环境变量/脱敏/快照/线程安全）

provides:
  - Config::instance
  - Config::load
  - Config::reload
  - Config::set
  - Config::rollback
  - Config::health

requires:
  modules: [kernel.core.config, kernel.core.types]
  libraries: [nlohmann_json]

assembly:
  order: 3
  parallel: false

tests:
  - tests/test_kernel_config.cpp

tags: [kernel, config, impl]
---UMS-END--- */

#include "kernel.L0.core.config.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iostream>

#if defined(__linux__) || defined(__APPLE__)
#  include <unistd.h>
#  if !defined(environ)
extern char** environ;
#  endif
#endif

namespace quant::kernel {

// ═══════════════════════════════════════════════════
// 单例（唯一实现，header 只声明）
// ═══════════════════════════════════════════════════
Config& Config::instance() {
    static Config inst;
    return inst;
}

// ═══════════════════════════════════════════════════
// 加载配置（默认值 < 文件 < 环境变量）
// ═══════════════════════════════════════════════════
bool Config::load(const std::string& path, bool allow_missing) {
    std::unique_lock lock(mutex_);
    last_error_.clear();

    // 1. 重置为默认值
    json_ = defaults();

    // 2. 文件覆盖
    if (!path.empty()) {
        if (!std::filesystem::exists(path)) {
            last_error_ = "配置文件不存在: " + path;
            if (!allow_missing) return false;
            // allow_missing 时用默认继续
        } else {
            try {
                std::ifstream f(path);
                if (!f.is_open()) {
                    last_error_ = "无法打开配置文件: " + path;
                    if (!allow_missing) return false;
                } else {
                    auto file_json = nlohmann::json::parse(f, nullptr, true, true);
                    if (!file_json.is_object()) {
                        last_error_ = "配置根节点必须是对象: " + path;
                        if (!allow_missing) return false;
                    } else {
                        merge_json(json_, file_json);
                        source_path_ = path;
                    }
                }
            } catch (const nlohmann::json::exception& e) {
                last_error_ = std::string("JSON解析失败: ") + e.what();
                if (!allow_missing) return false;
            } catch (const std::exception& e) {
                last_error_ = std::string("配置加载异常: ") + e.what();
                if (!allow_missing) return false;
            }
        }
    }

    // 3. 环境变量覆盖
    try {
        apply_env_overrides();
    } catch (const std::exception& e) {
        // 环境变量失败不影响主流程
        std::cerr << "[WARN] 环境变量覆盖失败: " << e.what() << "\n";
    }

    // 4. 版本检查
    int file_version = json_.value("config_version", CONFIG_VERSION);
    if (file_version > CONFIG_VERSION) {
        last_error_ = "配置版本过高: " + std::to_string(file_version) +
                      " > " + std::to_string(CONFIG_VERSION);
        return false;
    }
    json_["config_version"] = CONFIG_VERSION;

    // 5. 必填项校验
    if (!validate_required()) {
        // last_error_ 已在 validate_required 中设置
        return false;
    }

    // 6. 保存快照
    save_snapshot("load");

    // 7. 通知变更监听
    for (auto& cb : change_callbacks_) {
        if (cb) {
            try { cb("__reload__"); }
            catch (...) { /* 回调异常不影响主流程 */ }
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════
// 热重载
// ═══════════════════════════════════════════════════
bool Config::reload() {
    // 拷贝路径（避免持锁时调用 load 死锁）
    std::string path;
    {
        std::shared_lock lock(mutex_);
        path = source_path_;
    }
    return load(path, false);
}

// ═══════════════════════════════════════════════════
// 写入（运行时热更新）
// ═══════════════════════════════════════════════════
bool Config::set(const std::string& path, const nlohmann::json& value) {
    std::unique_lock lock(mutex_);
    try {
        set_by_path(json_, path, value);
        save_snapshot("set:" + path);

        for (auto& cb : change_callbacks_) {
            if (cb) {
                try { cb(path); }
                catch (...) { /* 忽略回调异常 */ }
            }
        }
        return true;
    } catch (const std::exception& e) {
        last_error_ = std::string("设置失败[") + path + "]: " + e.what();
        return false;
    }
}

// ═══════════════════════════════════════════════════
// 环境变量覆盖
//   QUANT_STRATEGY__BAND_K=1.2  → strategy.band_k = 1.2
//   QUANT_BINANCE__API_KEY=xxx  → binance.api_key = "xxx"
// ═══════════════════════════════════════════════════
void Config::apply_env_overrides() {
#if defined(__linux__) || defined(__APPLE__)
    if (environ == nullptr) return;

    for (char** env = environ; *env != nullptr; ++env) {
        std::string entry(*env);
        auto eq = entry.find('=');
        if (eq == std::string::npos) continue;

        std::string key = entry.substr(0, eq);
        std::string val = entry.substr(eq + 1);

        if (key.rfind("QUANT_", 0) != 0) continue;
        if (key.size() <= 6) continue;

        // 转为小写路径
        std::string path = key.substr(6);
        std::transform(path.begin(), path.end(), path.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // __  → .    单 _ → _（保持）
        // 用哨兵替换法避免误伤
        std::string normalized;
        normalized.reserve(path.size());
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '_' && i + 1 < path.size() && path[i + 1] == '_') {
                normalized += '.';
                ++i;  // 跳过下一个 _
            } else {
                normalized += path[i];
            }
        }

        // 值类型推断：先尝试 JSON，失败按字符串
        nlohmann::json parsed_value;
        try {
            parsed_value = nlohmann::json::parse(val);
        } catch (...) {
            parsed_value = val;  // 字符串
        }

        set_by_path(json_, normalized, parsed_value);
    }
#elif defined(_WIN32)
    // Windows 通过 _environ
    char** env_ptr = _environ;
    // 同逻辑...
    (void)env_ptr;
#endif
}

// ═══════════════════════════════════════════════════
// 必填项校验（支持点号路径）
// ═══════════════════════════════════════════════════
bool Config::validate_required() const {
    for (const auto& key : required_keys_) {
        const auto* val = resolve_path(json_, key);
        if (val == nullptr || val->is_null()) {
            // 尝试环境变量兜底
            std::string env_name = "QUANT_" + key;
            std::transform(env_name.begin(), env_name.end(), env_name.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (std::getenv(env_name.c_str()) == nullptr) {
                last_error_ = "缺少必填配置: " + key;
                return false;
            }
        }
    }
    return true;
}

void Config::add_required(const std::string& key) {
    std::unique_lock lock(mutex_);
    required_keys_.push_back(key);
}

// ═══════════════════════════════════════════════════
// 日志安全导出（脱敏）
// ═══════════════════════════════════════════════════
nlohmann::json Config::to_safe_json() const {
    std::shared_lock lock(mutex_);
    auto copy = json_;
    mask_json(copy);
    return copy;
}

std::string Config::dump_safe() const {
    return to_safe_json().dump(2);
}

// ═══════════════════════════════════════════════════
// 快照与回滚
// ═══════════════════════════════════════════════════
void Config::save_snapshot(const std::string& reason) {
    // 调用方须持有 unique_lock
    ConfigSnapshot snap;
    snap.timestamp_ms = [] {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
    }();
    snap.version = json_.value("config_version", CONFIG_VERSION);
    snap.data = json_;
    snap.reason = reason;

    snapshots_.push_back(std::move(snap));

    // 容量限制
    while (snapshots_.size() > MAX_SNAPSHOTS) {
        snapshots_.erase(snapshots_.begin());
    }
}

bool Config::rollback(size_t steps_back) {
    if (steps_back == 0) {
        last_error_ = "rollback steps_back 不能为 0";
        return false;
    }

    std::unique_lock lock(mutex_);
    if (snapshots_.size() < steps_back + 1) {
        last_error_ = "快照数量不足: 现有 " +
                      std::to_string(snapshots_.size()) +
                      ", 请求回滚 " + std::to_string(steps_back);
        return false;
    }

    size_t idx = snapshots_.size() - 1 - steps_back;
    json_ = snapshots_[idx].data;

    for (auto& cb : change_callbacks_) {
        if (cb) {
            try { cb("__rollback__"); }
            catch (...) {}
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════
// 健康检查
// ═══════════════════════════════════════════════════
bool Config::health() const {
    std::shared_lock lock(mutex_);
    if (json_.empty()) return false;
    if (!json_.contains("config_version")) return false;
    if (json_["config_version"].get<int>() > CONFIG_VERSION) return false;
    // 必填项仍可访问
    return last_error_.empty();
}

std::string Config::last_error() const {
    std::shared_lock lock(mutex_);
    return last_error_;
}

std::string Config::source_path() const {
    std::shared_lock lock(mutex_);
    return source_path_;
}

// ═══════════════════════════════════════════════════
// 变更监听
// ═══════════════════════════════════════════════════
void Config::on_change(ChangeCallback cb) {
    std::unique_lock lock(mutex_);
    change_callbacks_.push_back(std::move(cb));
}

// ═══════════════════════════════════════════════════
// 私有：路径解析
// ═══════════════════════════════════════════════════
const nlohmann::json* Config::resolve_path(
    const nlohmann::json& j, const std::string& path) noexcept {

    if (path.empty()) return &j;
    const nlohmann::json* cur = &j;
    size_t start = 0;

    while (start < path.size()) {
        size_t dot = path.find('.', start);
        std::string key = (dot == std::string::npos)
            ? path.substr(start)
            : path.substr(start, dot - start);

        if (key.empty()) return nullptr;
        if (!cur->is_object() || !cur->contains(key)) return nullptr;

        cur = &(*cur)[key];

        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return cur;
}

// ═══════════════════════════════════════════════════
// 私有：路径写入（自动创建中间层）
// ═══════════════════════════════════════════════════
void Config::set_by_path(nlohmann::json& j,
                         const std::string& path,
                         const nlohmann::json& value) {
    if (path.empty()) {
        if (!value.is_object()) {
            throw std::invalid_argument("空路径写入必须为对象");
        }
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

        if (key.empty()) {
            throw std::invalid_argument("路径中包含连续的点: " + path);
        }

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

// ═══════════════════════════════════════════════════
// 私有：深度合并（对象递归，数组整体替换）
// ═══════════════════════════════════════════════════
void Config::merge_json(nlohmann::json& base,
                        const nlohmann::json& overlay) {
    if (!overlay.is_object()) {
        base = overlay;
        return;
    }

    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        const std::string& key = it.key();
        if (base.contains(key)
            && base[key].is_object()
            && it.value().is_object()) {
            merge_json(base[key], it.value());
        } else {
            base[key] = it.value();
        }
    }
}

// ═══════════════════════════════════════════════════
// 私有：脱敏
// ═══════════════════════════════════════════════════
void Config::mask_json(nlohmann::json& j) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string key_lower = it.key();
            std::transform(key_lower.begin(), key_lower.end(),
                           key_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            bool sensitive = false;
            for (const auto& s : SENSITIVE_KEYS) {
                if (key_lower.find(s) != std::string::npos) {
                    sensitive = true;
                    break;
                }
            }

            if (sensitive && it.value().is_string()) {
                const auto& v = it.value().get_ref<const std::string&>();
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
        for (auto& item : j) {
            mask_json(item);
        }
    }
}

// ═══════════════════════════════════════════════════
// 私有：默认配置
// ═══════════════════════════════════════════════════
nlohmann::json Config::defaults() {
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
            {"escape_tighten_factor", 0.7},
            {"slow_mode_time_bars", 8}
        }},

        {"risk", {
            {"stop_atr", 1.5},
            {"max_risk_per_trade", 0.02},
            {"daily_max_loss", 0.05},
            {"max_leverage", 5},
            {"max_position_size", 1000}
        }},

        {"order", {
            {"execution_mode", "VIRTUAL"},
            {"leverage", 3},
            {"maker_fee", 0.0002},
            {"taker_fee", 0.0004},
            {"cancel_timeout_sec", 30}
        }},

        {"ai", {
            {"enabled", true},
            {"ollama_host", "http://localhost:11434"},
            {"model", "deepseek-r1:14b"},
            {"quantization", "q4_K_M"},
            {"suggestion_mode", "record_only"},
            {"timeout_ms", 5000}
        }},

        {"binance", {
            {"ws_host", "fstream.binance.com"},
            {"rest_host", "https://fapi.binance.com"},
            {"testnet", true},
            {"api_key", ""},
            {"api_secret", ""}
        }},

        {"database", {
            {"host", "localhost"},
            {"port", 5432},
            {"name", "quant"},
            {"user", "quant"},
            {"password", ""}
        }},

        {"redis", {
            {"host", "localhost"},
            {"port", 6379},
            {"password", ""}
        }},

        {"monitoring", {
            {"prometheus_port", 9090},
            {"log_retention_days", 30}
        }}
    };
}

} // namespace quant::kernel
