"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: config
version: 2.0.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: 系统配置管理器（密码校验/热更新/Schema/降级/脱敏）

provides:
  - class ConfigManager
  - class ConfigChange
  - class ConfigSnapshot
  - function get_config
  - function init_config

requires:
  modules: []
  libraries:
    - pyyaml>=6.0
    - bcrypt>=4.2  (可选)
    - filelock>=3.16  (可选)
  resources:
    memory_mb: 32
    cpu_cores: 1

health:
  fn: ConfigManager.health
  timeout_ms: 500

assembly:
  order: 10
  parallel: false

tests:
  - tests/test_config.py

fallback:
  - 文件缺失 → 默认值
  - Schema 缺失 → 跳过校验
  - bcrypt 缺失 → SHA256 降级
  - filelock 缺失 → 单进程模式

tags: [core, config, security]
---UMS-END---"""

from __future__ import annotations

import os
import json
import copy
import hashlib
import threading
import time
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional
from dataclasses import dataclass

# -------- 可选依赖降级 --------
try:
    import yaml
    _HAS_YAML = True
except ImportError:
    _HAS_YAML = False

try:
    import bcrypt
    _HAS_BCRYPT = True
except ImportError:
    _HAS_BCRYPT = False

try:
    from filelock import FileLock
    _HAS_FILELOCK = True
except ImportError:
    _HAS_FILELOCK = False


# ============================================================
# 异常层次
# ============================================================
class ConfigError(Exception):
    """配置模块基类异常"""

class ConfigValidationError(ConfigError):
    """参数校验失败"""

class ConfigPasswordError(ConfigError):
    """密码校验失败"""

class ConfigNotFoundError(ConfigError):
    """配置键不存在"""


# ============================================================
# 数据结构
# ============================================================
class ChangeType(str, Enum):
    SET = "set"
    UPDATE = "update"
    RELOAD = "reload"
    ROLLBACK = "rollback"


@dataclass(frozen=True)
class ConfigChange:
    """配置变更事件"""
    key: str
    old_value: Any
    new_value: Any
    change_type: ChangeType
    timestamp_ms: int
    operator: str = "system"
    version: int = 0


@dataclass(frozen=True)
class ConfigSnapshot:
    """配置快照"""
    version: int
    timestamp_ms: int
    data: Dict[str, Any]
    checksum: str


# ============================================================
# 配置管理器（单例）
# ============================================================
class ConfigManager:
    """
    线程安全、支持热更新、密码保护、Schema 校验、多级降级的配置管理器。

    使用：
        cfg = get_config()
        cfg.get("strategy.ema_period")
        cfg.set("risk.stop_atr", 2.0, password="xxx")
        cfg.subscribe(lambda c: print(c))
    """

    _instance: Optional["ConfigManager"] = None
    _instance_lock = threading.Lock()

    # 敏感字段（导出时脱敏）
    SENSITIVE_KEYWORDS = frozenset({
        "api_key", "api_secret", "apikey", "secret", "password",
        "passwd", "token", "private_key", "db_password",
        "redis_password", "vault_token",
    })

    # 需要密码保护的顶层分类
    PROTECTED_CATEGORIES = frozenset({"risk", "order", "system", "ai"})

    # 必需参数（启动时校验）
    REQUIRED_KEYS = ("strategy.ema_period", "risk.stop_atr")

    def __new__(cls, *args, **kwargs):
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self,
        config_path: Optional[Path] = None,
        schema_path: Optional[Path] = None,
        password_hash: Optional[str] = None,
        auto_reload: bool = False,
    ):
        # 单例：仅首次初始化
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._data: Dict[str, Any] = {}
            self._defaults: Dict[str, Any] = {}
            self._schema: Dict[str, Any] = {}
            self._version: int = 0
            self._history: List[ConfigSnapshot] = []
            self._max_history: int = 50
            self._subscribers: List[Callable[[ConfigChange], None]] = []
            self._password_hash: Optional[str] = password_hash
            self._operator: str = "system"
            self._audit: List[Dict[str, Any]] = []
            self._max_audit: int = 1000
            self._auto_reload: bool = auto_reload
            self._initialized = True

            # 路径解析（基于项目根）
            project_root = Path(__file__).resolve().parents[2]
            self._config_path = Path(config_path) if config_path else \
                project_root / "config" / "params.json"
            self._schema_path = Path(schema_path) if schema_path else \
                project_root / "config" / "schema.json"
            self._lock_path = self._config_path.with_suffix(".lock")

            self._bootstrap()

    # ========================================================
    # 启动加载
    # ========================================================
    def _bootstrap(self) -> None:
        """启动加载：多级降级，绝不抛异常"""
        try:
            self._load_defaults()
            self._load_schema()
            self._load_from_file()
            self._apply_env_overrides()
            self._validate_required()
            self._record_snapshot()
            self._audit_log("INFO", "配置加载成功")
        except Exception as exc:
            self._audit_log("ERROR",
                            f"配置加载失败: {exc}，降级到默认值")
            self._data = copy.deepcopy(self._defaults) or {}
            try:
                self._apply_env_overrides()
            except Exception as env_exc:
                self._audit_log("ERROR", f"环境变量覆盖失败: {env_exc}")

    def _load_defaults(self) -> None:
        """硬编码默认值，保证无文件也能启动"""
        self._defaults = {
            "strategy": {
                "ema_period": 26,
                "ema_fast": 12,
                "band_k": 1.0,
                "entry_threshold": 60,
                "escape_speed_fast": 1.5,
                "escape_speed_slow": 0.5,
                "escape_relax_factor": 1.5,
                "escape_tighten_factor": 0.7,
                "slow_mode_time_bars": 8,
            },
            "risk": {
                "stop_atr": 1.5,
                "max_risk_per_trade": 0.02,
                "max_leverage": 5,
                "daily_max_loss": 0.05,
            },
            "ai": {
                "enabled": True,
                "ollama_host": "http://localhost:11434",
                "model": "deepseek-r1:14b",
                "quantization": "q4_K_M",
                "suggestion_mode": "record_only",
            },
            "order": {
                "execution_mode": "VIRTUAL",
                "leverage": 3,
                "order_type": "LIMIT",
            },
            "display": {
                "theme": "light",
                "show_ai_suggestions": True,
                "chart_ratio": 0.667,
            },
            "system": {
                "log_level": "INFO",
                "report_weekly": True,
                "report_monthly": True,
            },
        }
        self._data = copy.deepcopy(self._defaults)

    def _load_schema(self) -> None:
        if not self._schema_path.exists():
            self._schema = {}
            return
        try:
            self._schema = json.loads(
                self._schema_path.read_text(encoding="utf-8"))
        except Exception as exc:
            self._audit_log("WARN", f"Schema 加载失败: {exc}")
            self._schema = {}

    def _load_from_file(self) -> None:
        if not self._config_path.exists():
            self._audit_log("WARN",
                            f"配置文件不存在: {self._config_path}")
            return
        text = self._config_path.read_text(encoding="utf-8")
        suffix = self._config_path.suffix.lower()
        if suffix in (".yaml", ".yml"):
            if not _HAS_YAML:
                raise ConfigError("缺少 pyyaml，无法解析 YAML")
            loaded = yaml.safe_load(text) or {}
        else:
            loaded = json.loads(text) if text.strip() else {}
        # 深度合并到默认值
        self._data = self._deep_merge(
            copy.deepcopy(self._defaults), loaded)

    def _apply_env_overrides(self) -> None:
        """环境变量覆盖：QUANT_STRATEGY__EMA_PERIOD=30 → strategy.ema_period"""
        prefix = "QUANT_"
        for key, raw in os.environ.items():
            if not key.startswith(prefix):
                continue
            # 排除系统自身环境变量
            sub = key[len(prefix):]
            if not sub or "__" not in sub and sub in (
                    "ENV", "CONFIG_PASSWORD_HASH"):
                continue
            path = [p.lower() for p in sub.split("__")]
            if not path:
                continue
            value = self._coerce(raw)
            self._set_nested(self._data, path, value)

    # ========================================================
    # 工具
    # ========================================================
    @staticmethod
    def _coerce(value: str) -> Any:
        v = value.strip()
        if v.lower() in ("true", "false"):
            return v.lower() == "true"
        if v.lower() in ("null", "none"):
            return None
        try:
            if "." in v or "e" in v.lower():
                return float(v)
            return int(v)
        except ValueError:
            return value

    @staticmethod
    def _deep_merge(base: Dict[str, Any],
                    override: Dict[str, Any]) -> Dict[str, Any]:
        for k, v in override.items():
            if k in base and isinstance(base[k], dict) and isinstance(v, dict):
                base[k] = ConfigManager._deep_merge(base[k], v)
            else:
                base[k] = v
        return base

    @staticmethod
    def _set_nested(node: Dict[str, Any], path: List[str],
                    value: Any) -> None:
        cur = node
        for p in path[:-1]:
            if p not in cur or not isinstance(cur[p], dict):
                cur[p] = {}
            cur = cur[p]
        cur[path[-1]] = value

    @staticmethod
    def _get_nested(node: Dict[str, Any], path: List[str],
                    default: Any = None) -> Any:
        cur: Any = node
        for p in path:
            if not isinstance(cur, dict) or p not in cur:
                return default
            cur = cur[p]
        return cur

    @classmethod
    def _mask_value(cls, key: str, value: Any) -> Any:
        if not isinstance(value, str):
            return value
        k_lower = key.lower()
        if any(s in k_lower for s in cls.SENSITIVE_KEYWORDS):
            if len(value) <= 8:
                return "****"
            return f"{value[:4]}****{value[-4:]}"
        return value

    def _audit_log(self, level: str, msg: str) -> None:
        entry = {
            "ts": int(time.time() * 1000),
            "level": level,
            "msg": msg,
            "version": self._version,
        }
        self._audit.append(entry)
        if len(self._audit) > self._max_audit:
            self._audit = self._audit[-self._max_audit:]
        # 使用标准 logging 而非打印，避免循环依赖
        try:
            import logging
            logging.getLogger("quant.config").log(
                {"INFO": logging.INFO, "WARN": logging.WARNING,
                 "ERROR": logging.ERROR}.get(level, logging.INFO),
                msg,
            )
        except Exception:
            pass

    # ========================================================
    # 读写
    # ========================================================
    def get(self, path: str, default: Any = None) -> Any:
        with self._lock:
            return self._get_nested(self._data, path.split("."), default)

    def set(
        self,
        path: str,
        value: Any,
        password: Optional[str] = None,
        operator: str = "system",
    ) -> bool:
        if not isinstance(path, str) or not path:
            raise ConfigValidationError("path 不能为空")

        with self._lock:
            category = path.split(".")[0]

            # 1. 密码校验（受保护分类）
            if category in self.PROTECTED_CATEGORIES:
                if not self._verify_password(password):
                    self._audit_log("WARN", f"密码校验失败: {path}")
                    raise ConfigPasswordError("密码校验失败")

            # 2. 类型与边界校验
            self._validate_value(path, value)

            # 3. 读取旧值
            old_value = self._get_nested(self._data, path.split("."))

            # 4. 写入新值
            self._set_nested(self._data, path.split("."), value)

            # 5. 版本递增 + 通知
            self._version += 1
            change = ConfigChange(
                key=path,
                old_value=old_value,
                new_value=value,
                change_type=ChangeType.SET,
                timestamp_ms=int(time.time() * 1000),
                operator=operator,
                version=self._version,
            )
            self._notify(change)
            self._record_snapshot()
            self._audit_log(
                "INFO",
                f"配置更新: {path} = "
                f"{self._mask_value(path, value)} (by {operator})",
            )
            return True

    def update(
        self,
        changes: Dict[str, Any],
        password: Optional[str] = None,
        operator: str = "system",
    ) -> int:
        if not isinstance(changes, dict):
            raise ConfigValidationError("changes 必须是 dict")
        count = 0
        for path, value in changes.items():
            try:
                if self.set(path, value, password, operator):
                    count += 1
            except ConfigError as exc:
                self._audit_log("WARN", f"更新 {path} 失败: {exc}")
        return count

    def has(self, path: str) -> bool:
        with self._lock:
            marker = object()
            return self._get_nested(
                self._data, path.split("."), marker) is not marker

    # ========================================================
    # 密码
    # ========================================================
    def _verify_password(self, password: Optional[str]) -> bool:
        # 延迟从环境变量加载
        if self._password_hash is None:
            self._password_hash = os.environ.get(
                "QUANT_CONFIG_PASSWORD_HASH")
            if self._password_hash is None:
                # 无密码时，仅开发环境放行
                return os.environ.get("QUANT_ENV", "prod") == "dev"

        if not password:
            return False

        if _HAS_BCRYPT and self._password_hash.startswith("$2"):
            try:
                return bcrypt.checkpw(
                    password.encode("utf-8"),
                    self._password_hash.encode("utf-8"),
                )
            except Exception:
                return False

        # SHA256 降级
        digest = hashlib.sha256(password.encode("utf-8")).hexdigest()
        # 使用 hmac 常数时间比较
        import hmac
        return hmac.compare_digest(digest, self._password_hash)

    def set_password(self, password: str,
                     operator: str = "system") -> None:
        if not password or len(password) < 8:
            raise ConfigValidationError("密码长度至少 8 位")
        if _HAS_BCRYPT:
            self._password_hash = bcrypt.hashpw(
                password.encode("utf-8"),
                bcrypt.gensalt(rounds=12),
            ).decode("utf-8")
        else:
            self._password_hash = hashlib.sha256(
                password.encode("utf-8")).hexdigest()
        self._audit_log("INFO", f"密码已更新 by {operator}")

    @property
    def has_password(self) -> bool:
        return self._password_hash is not None or \
            os.environ.get("QUANT_CONFIG_PASSWORD_HASH") is not None

    # ========================================================
    # 校验
    # ========================================================
    def _validate_value(self, path: str, value: Any) -> None:
        node: Any = self._schema
        for p in path.split("."):
            if not isinstance(node, dict) or p not in node:
                node = None
                break
            node = node[p]
        if node is None:
            return  # 无 schema 定义，放行

        expected = node.get("type")
        if expected == "int" and not isinstance(value, int):
            raise ConfigValidationError(f"{path} 需要 int")
        if expected == "float" and not isinstance(value, (int, float)):
            raise ConfigValidationError(f"{path} 需要 float")
        if expected == "bool" and not isinstance(value, bool):
            raise ConfigValidationError(f"{path} 需要 bool")
        if expected == "str" and not isinstance(value, str):
            raise ConfigValidationError(f"{path} 需要 str")

        if "min" in node and isinstance(value, (int, float)) \
                and value < node["min"]:
            raise ConfigValidationError(
                f"{path}={value} 低于最小值 {node['min']}")
        if "max" in node and isinstance(value, (int, float)) \
                and value > node["max"]:
            raise ConfigValidationError(
                f"{path}={value} 高于最大值 {node['max']}")
        if "options" in node and value not in node["options"]:
            raise ConfigValidationError(
                f"{path}={value} 不在允许选项 {node['options']}")

    def _validate_required(self) -> None:
        for path in self.REQUIRED_KEYS:
            marker = object()
            v = self._get_nested(self._data, path.split("."), marker)
            if v is marker:
                raise ConfigValidationError(f"缺少必需参数: {path}")

    # ========================================================
    # 事件订阅
    # ========================================================
    def subscribe(self, callback: Callable[[ConfigChange], None]) -> None:
        if not callable(callback):
            raise ConfigError("callback 必须可调用")
        with self._lock:
            if callback not in self._subscribers:
                self._subscribers.append(callback)

    def unsubscribe(self, callback: Callable[[ConfigChange], None]) -> None:
        with self._lock:
            if callback in self._subscribers:
                self._subscribers.remove(callback)

    def _notify(self, change: ConfigChange) -> None:
        for cb in list(self._subscribers):
            try:
                cb(change)
            except Exception as exc:
                self._audit_log("WARN", f"订阅回调失败: {exc}")

    # ========================================================
    # 持久化
    # ========================================================
    def save(self, create_backup: bool = True,
             operator: str = "system") -> None:
        with self._lock:
            self._config_path.parent.mkdir(parents=True, exist_ok=True)

            # 备份
            if create_backup and self._config_path.exists():
                backup = self._config_path.with_suffix(
                    f".bak.{self._version}.json")
                try:
                    backup.write_text(
                        self._config_path.read_text(encoding="utf-8"),
                        encoding="utf-8",
                    )
                except Exception as exc:
                    self._audit_log("WARN", f"备份失败: {exc}")

            # 原子写
            tmp = self._config_path.with_suffix(".tmp")
            tmp.write_text(
                json.dumps(self._data, indent=2, ensure_ascii=False),
                encoding="utf-8",
            )

            try:
                if _HAS_FILELOCK:
                    with FileLock(str(self._lock_path)):
                        tmp.replace(self._config_path)
                else:
                    tmp.replace(self._config_path)
            except Exception as exc:
                self._audit_log("ERROR", f"写入失败: {exc}")
                raise ConfigError(f"配置保存失败: {exc}") from exc

            self._audit_log(
                "INFO",
                f"配置已保存 (version={self._version}, by {operator})")

    def reload(self) -> bool:
        with self._lock:
            try:
                before = copy.deepcopy(self._data)
                self._load_from_file()
                self._apply_env_overrides()
                self._validate_required()
                if before != self._data:
                    self._version += 1
                    self._notify(ConfigChange(
                        key="*",
                        old_value=before,
                        new_value=copy.deepcopy(self._data),
                        change_type=ChangeType.RELOAD,
                        timestamp_ms=int(time.time() * 1000),
                        version=self._version,
                    ))
                    self._record_snapshot()
                    self._audit_log("INFO", "配置已热更新")
                return True
            except Exception as exc:
                self._audit_log("ERROR", f"热更新失败: {exc}")
                return False

    # ========================================================
    # 快照与回滚
    # ========================================================
    def snapshot(self) -> ConfigSnapshot:
        with self._lock:
            data = copy.deepcopy(self._data)
            checksum = hashlib.sha256(
                json.dumps(data, sort_keys=True,
                           ensure_ascii=False).encode("utf-8")
            ).hexdigest()
            return ConfigSnapshot(
                version=self._version,
                timestamp_ms=int(time.time() * 1000),
                data=data,
                checksum=checksum,
            )

    def _record_snapshot(self) -> None:
        self._history.append(self.snapshot())
        if len(self._history) > self._max_history:
            self._history = self._history[-self._max_history:]

    def rollback(self, version: int,
                 operator: str = "system") -> bool:
        with self._lock:
            for snap in reversed(self._history):
                if snap.version == version:
                    self._data = copy.deepcopy(snap.data)
                    self._version += 1
                    self._notify(ConfigChange(
                        key="*",
                        old_value=None,
                        new_value=copy.deepcopy(self._data),
                        change_type=ChangeType.ROLLBACK,
                        timestamp_ms=int(time.time() * 1000),
                        operator=operator,
                        version=self._version,
                    ))
                    self._record_snapshot()
                    self._audit_log(
                        "INFO", f"已回滚到版本 {version}")
                    return True
            self._audit_log("WARN", f"未找到版本 {version}")
            return False

    # ========================================================
    # 导出
    # ========================================================
    def to_dict(self, mask: bool = True) -> Dict[str, Any]:
        with self._lock:
            def walk(node: Any, prefix: str = "") -> Any:
                if isinstance(node, dict):
                    return {k: walk(v, f"{prefix}{k}." if prefix else k)
                            for k, v in node.items()}
                if mask:
                    return self._mask_value(prefix, node)
                return node
            return walk(copy.deepcopy(self._data))

    def to_json(self, mask: bool = True) -> str:
        return json.dumps(
            self.to_dict(mask=mask), indent=2, ensure_ascii=False)

    # ========================================================
    # 健康与状态
    # ========================================================
    def health(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "status": "ok",
                "version": self._version,
                "path": str(self._config_path),
                "has_password": self.has_password,
                "subscribers": len(self._subscribers),
                "audit_entries": len(self._audit),
                "yaml_available": _HAS_YAML,
                "bcrypt_available": _HAS_BCRYPT,
                "filelock_available": _HAS_FILELOCK,
            }

    def audit_tail(self, n: int = 20) -> List[Dict[str, Any]]:
        with self._lock:
            return list(self._audit[-n:])

    @property
    def version(self) -> int:
        with self._lock:
            return self._version


# ============================================================
# 全局入口
# ============================================================
_config_manager: Optional[ConfigManager] = None
_global_lock = threading.Lock()


def get_config() -> ConfigManager:
    """获取全局配置管理器（惰性初始化）"""
    global _config_manager
    if _config_manager is None:
        with _global_lock:
            if _config_manager is None:
                _config_manager = ConfigManager()
    return _config_manager


def init_config(**kwargs: Any) -> ConfigManager:
    """显式初始化（启动时调用）"""
    global _config_manager
    with _global_lock:
        _config_manager = ConfigManager(**kwargs)
    return _config_manager
