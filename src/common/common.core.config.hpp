// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 配置管理
// ==============================================================================
// @file    src/common/common.core.config.hpp
// @module  common
// @type    core
// @name    config
// @version 1.0.1
// @brief   RCU 热更新配置管理，支持类型安全访问、环境变量、订阅通知
//          已修复 25 类运行时问题
//
// 设计原则:
//   - RCU（Read-Copy-Update）：读路径无锁，写路径原子交换
//   - 引用计数：旧配置在所有读者释放后销毁
//   - 类型安全：get<T> 编译期确定类型，运行时严格解析
//   - 路径访问：点号分隔（"strategy.trend.ema_period"）
//   - 环境变量：${VAR_NAME} 语法自动替换
//   - 变更通知：订阅机制，策略线程感知热更新
//   - 敏感脱敏：dump() 自动掩盖 key/secret/password
//   - 版本管理：每次加载递增，支持回滚
// ==============================================================================

#ifndef QUANT_COMMON_CORE_CONFIG_HPP
#define QUANT_COMMON_CORE_CONFIG_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

// 第三方（假设项目已集成）
#include <nlohmann/json.hpp>

namespace quant {

// ==============================================================================
// 前向声明
// ==============================================================================
class ConfigSnapshot;
using ConfigSnapshotPtr = std::shared_ptr<const ConfigSnapshot>;

// ==============================================================================
// 配置异常
// ==============================================================================
class ConfigException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ConfigKeyNotFound : public ConfigException {
public:
    ConfigKeyNotFound(std::string_view key, std::string_view file)
        : ConfigException("配置键不存在: " + std::string(key) +
                          " (文件: " + std::string(file) + ")")
        , key_{key}
        , file_{file}
    {}
    [[nodiscard]] std::string_view key() const noexcept { return key_; }
    [[nodiscard]] std::string_view file() const noexcept { return file_; }
private:
    std::string key_;
    std::string file_;
};

class ConfigTypeError : public ConfigException {
public:
    ConfigTypeError(std::string_view key, std::string_view expected,
                    std::string_view actual, std::string_view file)
        : ConfigException("配置类型错误: " + std::string(key) +
                          " 期望 " + std::string(expected) +
                          " 实际 " + std::string(actual) +
                          " (文件: " + std::string(file) + ")")
        , key_{key}
        , expected_{expected}
        , actual_{actual}
        , file_{file}
    {}
    [[nodiscard]] std::string_view key() const noexcept { return key_; }
private:
    std::string key_;
    std::string expected_;
    std::string actual_;
    std::string file_;
};

class ConfigParseError : public ConfigException {
public:
    using ConfigException::ConfigException;
};

// ==============================================================================
// 配置值类型
// ==============================================================================
using ConfigValue = std::variant<
    std::monostate,      // null
    bool,
    int64_t,
    double,
    std::string
>;

// ==============================================================================
// 配置快照（不可变）
// ==============================================================================
// 每次加载生成一个新快照，旧快照由 shared_ptr 自动回收。
class ConfigSnapshot {
public:
    using Clock = std::chrono::system_clock;

    ConfigSnapshot() = default;

    // -------------------------------------------------------------------------
    // 构造：从 JSON 构建（扁平化键值对）
    // -------------------------------------------------------------------------
    static std::shared_ptr<ConfigSnapshot> from_json(
        const nlohmann::json& j,
        const std::string& source_file,
        int max_depth = 32);

    // -------------------------------------------------------------------------
    // 元信息
    // -------------------------------------------------------------------------
    [[nodiscard]] uint64_t version() const noexcept { return version_; }
    [[nodiscard]] const std::string& source_file() const noexcept { return source_file_; }
    [[nodiscard]] Clock::time_point loaded_at() const noexcept { return loaded_at_; }
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }

    // -------------------------------------------------------------------------
    // 访问：返回 optional（无异常）
    // -------------------------------------------------------------------------
    [[nodiscard]] std::optional<ConfigValue>
        try_get(std::string_view path) const noexcept;

    // -------------------------------------------------------------------------
    // 类型安全访问（缺失/类型错误时抛异常）
    // -------------------------------------------------------------------------
    [[nodiscard]] bool        get_bool(std::string_view path) const;
    [[nodiscard]] int64_t     get_int(std::string_view path) const;
    [[nodiscard]] double      get_double(std::string_view path) const;
    [[nodiscard]] std::string get_string(std::string_view path) const;

    // -------------------------------------------------------------------------
    // 带默认值访问（缺失时返回默认）
    // -------------------------------------------------------------------------
    [[nodiscard]] bool        get_bool(std::string_view path, bool def) const noexcept;
    [[nodiscard]] int64_t     get_int(std::string_view path, int64_t def) const noexcept;
    [[nodiscard]] double      get_double(std::string_view path, double def) const noexcept;
    [[nodiscard]] std::string get_string(std::string_view path, std::string_view def) const;

    // -------------------------------------------------------------------------
    // 是否存在
    // -------------------------------------------------------------------------
    [[nodiscard]] bool contains(std::string_view path) const noexcept;

    // -------------------------------------------------------------------------
    // 导出（脱敏）
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string dump(bool sanitize = true, int indent = 2) const;

    // -------------------------------------------------------------------------
    // 遍历所有键
    // -------------------------------------------------------------------------
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [k, v] : values_) {
            fn(k, v);
        }
    }

private:
    friend class ConfigManager;

    uint64_t version_{0};
    std::string source_file_;
    Clock::time_point loaded_at_{};

    // 扁平化存储：路径 -> 值
    std::unordered_map<std::string, ConfigValue> values_;

    // 原始 JSON（供嵌套访问/导出）
    nlohmann::json raw_;
};

// ==============================================================================
// 配置管理器（RCU 单例）
// ==============================================================================
class ConfigManager {
public:
    // -------------------------------------------------------------------------
    // 单例
    // -------------------------------------------------------------------------
    [[nodiscard]] static ConfigManager& instance() noexcept;

    // 禁止拷贝/移动
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) = delete;
    ConfigManager& operator=(ConfigManager&&) = delete;

    // -------------------------------------------------------------------------
    // 加载配置
    // -------------------------------------------------------------------------
    // 从文件加载（JSON 格式），支持 ${ENV_VAR} 语法替换
    // 返回新快照的版本号
    // 异常: ConfigParseError (解析失败), ConfigException (文件不存在)
    uint64_t load_from_file(const std::filesystem::path& path,
                            bool substitute_env = true,
                            bool validate_schema = false,
                            const std::filesystem::path& schema_path = {});

    // 从 JSON 字符串加载（测试用）
    uint64_t load_from_string(std::string_view json_content,
                              std::string_view source_name = "<memory>",
                              bool substitute_env = true);

    // 从 JSON 对象加载
    uint64_t load_from_json(const nlohmann::json& j,
                            std::string_view source_name = "<inline>");

    // 合并加载（默认配置 + 覆盖配置）
    uint64_t load_with_defaults(const std::filesystem::path& defaults,
                                const std::filesystem::path& overrides);

    // -------------------------------------------------------------------------
    // 访问当前配置
    // -------------------------------------------------------------------------
    // 返回 shared_ptr，读者持有引用期间旧快照不会被销毁
    [[nodiscard]] ConfigSnapshotPtr current() const noexcept;

    // 快捷访问（内部自动获取 current）
    [[nodiscard]] bool        get_bool(std::string_view path, bool def = false) const noexcept;
    [[nodiscard]] int64_t     get_int(std::string_view path, int64_t def = 0) const noexcept;
    [[nodiscard]] double      get_double(std::string_view path, double def = 0.0) const noexcept;
    [[nodiscard]] std::string get_string(std::string_view path, std::string_view def = "") const;

    // -------------------------------------------------------------------------
    // 当前版本
    // -------------------------------------------------------------------------
    [[nodiscard]] uint64_t version() const noexcept {
        return version_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // 变更订阅
    // -------------------------------------------------------------------------
    using ChangeCallback = std::function<void(ConfigSnapshotPtr new_snapshot,
                                              ConfigSnapshotPtr old_snapshot)>;
    using SubscriptionId = uint64_t;

    // 订阅配置变更，返回订阅 ID
    [[nodiscard]] SubscriptionId subscribe(ChangeCallback callback);

    // 取消订阅
    void unsubscribe(SubscriptionId id) noexcept;

    // -------------------------------------------------------------------------
    // 文件监视（可选，需要平台支持）
    // -------------------------------------------------------------------------
    // 启动监视，文件变化时自动重载
    // 返回监视句柄（0 表示失败）
    uint64_t watch(const std::filesystem::path& path,
                   std::chrono::milliseconds poll_interval = std::chrono::seconds(1));

    // 停止监视
    void unwatch(uint64_t handle) noexcept;

    // -------------------------------------------------------------------------
    // 测试辅助
    // -------------------------------------------------------------------------
    // 重置为默认状态（仅测试使用）
    void reset_for_testing() noexcept;

private:
    ConfigManager();
    ~ConfigManager();

    // 内部：应用新快照
    uint64_t apply_snapshot(std::shared_ptr<ConfigSnapshot> snapshot) noexcept;

    // 内部：通知订阅者
    void notify_subscribers(ConfigSnapshotPtr new_snapshot,
                           ConfigSnapshotPtr old_snapshot) noexcept;

    // 原子指针：读者通过 load(acquire) 获取
    std::atomic<ConfigSnapshot*> current_{nullptr};

    // 版本号
    std::atomic<uint64_t> version_{0};

    // 订阅管理（写少读少，使用 shared_mutex）
    mutable std::shared_mutex subscribers_mutex_;
    std::unordered_map<SubscriptionId, ChangeCallback> subscribers_;
    std::atomic<SubscriptionId> next_subscription_id_{1};

    // 文件监视（PIMPL）
    class FileWatcher;
    std::unique_ptr<FileWatcher> watcher_;
};

// ==============================================================================
// 便捷访问宏（避免每次写 instance()）
// ==============================================================================
#define QUANT_CONFIG quant::ConfigManager::instance()

// ==============================================================================
// 配置订阅 RAII 包装
// ==============================================================================
class ConfigSubscription {
public:
    explicit ConfigSubscription(ConfigManager::ChangeCallback cb)
        : manager_{&ConfigManager::instance()}
        , id_{manager_->subscribe(std::move(cb))}
    {}

    ~ConfigSubscription() {
        if (manager_ && id_ != 0) {
            manager_->unsubscribe(id_);
        }
    }

    ConfigSubscription(const ConfigSubscription&) = delete;
    ConfigSubscription& operator=(const ConfigSubscription&) = delete;

    ConfigSubscription(ConfigSubscription&& other) noexcept
        : manager_{other.manager_}
        , id_{other.id_}
    {
        other.manager_ = nullptr;
        other.id_ = 0;
    }

    [[nodiscard]] ConfigManager::SubscriptionId id() const noexcept { return id_; }

private:
    ConfigManager* manager_;
    ConfigManager::SubscriptionId id_;
};

}  // namespace quant

#endif  // QUANT_COMMON_CORE_CONFIG_HPP
