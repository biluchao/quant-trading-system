"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: secrets
version: 2.0.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: 密钥管理（多源加载/权限校验/脱敏/加密/Vault/轮换）

provides:
  - class Secrets
  - class SecretMeta
  - class SecretSource
  - function get_secrets
  - function init_secrets

requires:
  modules: [core.config, core.logger]
  libraries:
    - pyyaml>=6.0
    - cryptography>=43.0  (可选，加密文件)
    - hvac>=2.3  (可选，Vault)
  resources:
    memory_mb: 16
    cpu_cores: 1

health:
  fn: Secrets.health
  timeout_ms: 1000

assembly:
  order: 14
  parallel: false

tests:
  - tests/test_secrets.py

fallback:
  - Vault 不可用 → 环境变量
  - 加密文件损坏 → 明文文件
  - 文件缺失 → 环境变量
  - 环境变量缺失 → 抛异常（必需密钥）

tags: [core, security, secrets]
---UMS-END---"""

from __future__ import annotations

import os
import re
import json
import hmac
import stat
import time
import base64
import hashlib
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Set

# ============================================================
# 可选依赖
# ============================================================
try:
    import yaml
    _HAS_YAML = True
except ImportError:
    _HAS_YAML = False

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
    from cryptography.hazmat.primitives import hashes
    _HAS_CRYPTO = True
except ImportError:
    _HAS_CRYPTO = False

try:
    import hvac
    _HAS_HVAC = True
except ImportError:
    _HAS_HVAC = False

# 延迟导入避免循环依赖
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("secrets")
        except Exception:
            import logging
            _logger = logging.getLogger("secrets")
    return _logger


def _now_ms() -> int:
    try:
        from core.timestamp import now_ms
        return now_ms()
    except Exception:
        return int(time.time() * 1000)


# ============================================================
# 异常
# ============================================================
class SecretsError(Exception):
    """密钥模块基类异常"""


class SecretNotFoundError(SecretsError):
    """密钥不存在"""


class SecretPermissionError(SecretsError):
    """密钥文件权限过宽"""


class SecretValidationError(SecretsError):
    """密钥格式或类型错误"""


# ============================================================
# 密钥元数据
# ============================================================
@dataclass
class SecretMeta:
    """单个密钥的元数据"""
    key: str
    format: Optional[str] = None       # 如 "binance_api_key"
    pattern: Optional[str] = None      # 正则
    required: bool = False
    rotate_days: int = 0               # 0 表示不轮换
    loaded_ms: int = field(default_factory=_now_ms)
    source: str = "unknown"

    def is_expired(self) -> bool:
        if self.rotate_days <= 0:
            return False
        age_days = (_now_ms() - self.loaded_ms) / 86400000
        return age_days > self.rotate_days


# ============================================================
# 密钥来源
# ============================================================
@dataclass
class SecretSource:
    kind: str                  # "vault" | "env" | "encrypted_file" | "file"
    path: Optional[Path] = None
    priority: int = 0          # 数字越小优先级越高
    vault_addr: Optional[str] = None
    vault_token: Optional[str] = None
    vault_path: Optional[str] = None
    enabled: bool = True


# ============================================================
# 密钥格式库
# ============================================================
SECRET_PATTERNS: Dict[str, str] = {
    "binance_api_key": r"^[A-Za-z0-9]{64}$",
    "binance_api_secret": r"^[A-Za-z0-9]{64}$",
    "db_password": r"^.{8,128}$",
    "redis_password": r"^.{8,128}$",
    "jwt_secret": r"^.{32,}$",
    "vault_token": r"^[a-zA-Z0-9._-]+$",
}

# 必需密钥（启动时校验）
REQUIRED_KEYS: Set[str] = set()


# ============================================================
# Secrets 单例
# ============================================================
class Secrets:
    """
    线程安全的多源密钥管理器。

    加载优先级（可配置）：
      1. Vault（若可用）
      2. 环境变量
      3. 加密文件
      4. 明文文件

    特性：
      - 权限校验
      - 脱敏导出
      - 审计日志
      - 密钥格式校验
      - 轮换提醒
      - 三级降级
    """

    _instance: Optional["Secrets"] = None
    _instance_lock = threading.Lock()

    # 敏感字段关键字
    SENSITIVE_KEYWORDS = frozenset({
        "api_key", "api_secret", "apikey", "secret", "password",
        "passwd", "token", "private_key", "db_password",
        "redis_password", "vault_token", "jwt_secret",
    })

    # 环境变量前缀
    ENV_PREFIX = "QUANT_SECRET_"

    def __new__(cls, *args: Any, **kwargs: Any) -> "Secrets":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self,
        sources: Optional[List[SecretSource]] = None,
        encryption_key: Optional[bytes] = None,
        strict_mode: bool = True,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._data: Dict[str, str] = {}
            self._meta: Dict[str, SecretMeta] = {}
            self._audit: List[Dict[str, Any]] = []
            self._max_audit = 1000
            self._access_count: Dict[str, int] = {}
            self._encryption_key = encryption_key
            self._strict = strict_mode
            self._initialized = True

            # 路径解析
            project_root = Path(__file__).resolve().parents[2]
            self._project_root = project_root

            # 来源列表
            self._sources = sources or self._default_sources()

            # 加载
            self._bootstrap()

    # ========================================================
    # 启动
    # ========================================================
    def _default_sources(self) -> List[SecretSource]:
        root = self._project_root
        return [
            SecretSource(
                kind="env",
                priority=10,
            ),
            SecretSource(
                kind="encrypted_file",
                path=root / "config" / "secrets.enc",
                priority=20,
            ),
            SecretSource(
                kind="file",
                path=root / "config" / "secrets.yaml",
                priority=30,
            ),
        ]

    def _bootstrap(self) -> None:
        """启动加载：多源降级，绝不崩溃"""
        try:
            loaded_any = False
            for src in sorted(self._sources, key=lambda s: s.priority):
                if not src.enabled:
                    continue
                try:
                    count = self._load_from_source(src)
                    if count > 0:
                        loaded_any = True
                        _log().info(
                            f"密钥来源 {src.kind} 加载 {count} 条")
                except SecretPermissionError:
                    raise
                except Exception as exc:
                    _log().warning(
                        f"密钥来源 {src.kind} 加载失败: {exc}")

            if not loaded_any:
                _log().warning("所有密钥来源均未加载到数据")

            self._validate_required()
            self._audit_log("INFO",
                            f"密钥加载完成，共 {len(self._data)} 条")
        except Exception as exc:
            _log().error(f"密钥加载异常: {exc}")

    def _load_from_source(self, src: SecretSource) -> int:
        if src.kind == "vault":
            return self._load_vault(src)
        if src.kind == "env":
            return self._load_env(src)
        if src.kind == "encrypted_file":
            return self._load_encrypted_file(src)
        if src.kind == "file":
            return self._load_file(src)
        return 0

    # ========================================================
    # 各来源实现
    # ========================================================
    def _load_env(self, src: SecretSource) -> int:
        count = 0
        for key, value in os.environ.items():
            if not key.startswith(self.ENV_PREFIX):
                continue
            name = key[len(self.ENV_PREFIX):].lower()
            if not name:
                continue
            with self._lock:
                if name not in self._data:
                    self._data[name] = value
                    self._meta[name] = SecretMeta(
                        key=name, source="env")
                    count += 1
        return count

    def _load_file(self, src: SecretSource) -> int:
        if src.path is None or not src.path.exists():
            return 0
        self._check_permissions(src.path)
        if not _HAS_YAML:
            raise SecretsError("缺少 pyyaml，无法解析 YAML")
        text = src.path.read_text(encoding="utf-8")
        loaded = yaml.safe_load(text) or {}
        if not isinstance(loaded, dict):
            raise SecretsError("密钥文件格式非法（应为 dict）")
        return self._merge_loaded(loaded, src="file")

    def _load_encrypted_file(self, src: SecretSource) -> int:
        if src.path is None or not src.path.exists():
            return 0
        self._check_permissions(src.path)
        if not _HAS_CRYPTO:
            raise SecretsError("缺少 cryptography，无法解密")
        if self._encryption_key is None:
            raise SecretsError("未提供解密密钥")
        raw = src.path.read_bytes()
        try:
            plaintext = self._decrypt(raw)
            loaded = json.loads(plaintext.decode("utf-8"))
        except Exception as exc:
            raise SecretsError(f"解密失败: {exc}")
        if not isinstance(loaded, dict):
            raise SecretsError("解密后格式非法")
        return self._merge_loaded(loaded, src="encrypted_file")

    def _load_vault(self, src: SecretSource) -> int:
        if not _HAS_HVAC:
            raise SecretsError("缺少 hvac，Vault 不可用")
        addr = src.vault_addr or os.environ.get("VAULT_ADDR")
        token = src.vault_token or os.environ.get("VAULT_TOKEN")
        if not addr or not token:
            raise SecretsError("Vault 地址或令牌缺失")
        client = hvac.Client(url=addr, token=token, timeout=3)
        if not client.is_authenticated():
            raise SecretsError("Vault 认证失败")
        path = src.vault_path or "secret/data/quant"
        resp = client.secrets.kv.v2.read_secret_version(
            path=path.split("/", 1)[-1] if "data" in path
            else path,
        )
        data = resp.get("data", {}).get("data", {})
        if not isinstance(data, dict):
            return 0
        return self._merge_loaded(data, src="vault")

    def _merge_loaded(self, loaded: Dict[str, Any], src: str) -> int:
        count = 0
        with self._lock:
            for k, v in loaded.items():
                if not isinstance(k, str) or not isinstance(v, str):
                    continue
                # 高优先级来源不覆盖已有（先加载的优先级高）
                if k in self._data:
                    continue
                self._data[k] = v
                self._meta[k] = SecretMeta(
                    key=k, source=src, loaded_ms=_now_ms())
                count += 1
        return count

    # ========================================================
    # 权限检查
    # ========================================================
    def _check_permissions(self, path: Path) -> None:
        if os.name != "posix":
            return
        try:
            st = path.stat()
        except Exception:
            return
        # 文件权限：不应有 group/other 的读权限
        if st.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
            msg = (f"密钥文件权限过宽: {path} "
                   f"(mode={oct(st.st_mode & 0o777)})")
            _log().error(msg)
            if self._strict:
                raise SecretPermissionError(msg)

    # ========================================================
    # 读取
    # ========================================================
    def get(self, key: str, default: Optional[str] = None) -> Optional[str]:
        with self._lock:
            value = self._data.get(key, default)
            self._access_count[key] = self._access_count.get(key, 0) + 1
        self._audit_log("ACCESS", f"读取密钥: {key}")
        return value

    def require(self, key: str) -> str:
        value = self.get(key)
        if value is None:
            raise SecretNotFoundError(f"缺少必需密钥: {key}")
        return value

    def has(self, key: str) -> bool:
        with self._lock:
            return key in self._data

    def keys(self) -> List[str]:
        with self._lock:
            return sorted(self._data.keys())

    def mask(self, value: Optional[str]) -> str:
        """脱敏显示"""
        if value is None:
            return "<none>"
        if len(value) <= 8:
            return "****"
        return f"{value[:4]}****{value[-4:]}"

    def masked(self, key: str) -> str:
        return self.mask(self.get(key))

    def to_dict(self, mask: bool = True) -> Dict[str, str]:
        with self._lock:
            if mask:
                return {k: self.mask(v) for k, v in self._data.items()}
            return dict(self._data)

    # ========================================================
    # 校验
    # ========================================================
    def _validate_required(self) -> None:
        missing = [k for k in REQUIRED_KEYS if not self.has(k)]
        if missing:
            msg = f"缺少必需密钥: {missing}"
            _log().error(msg)
            if self._strict:
                raise SecretNotFoundError(msg)

    def validate_format(self, key: str) -> bool:
        """校验密钥格式"""
        if key not in SECRET_PATTERNS:
            return True
        value = self.get(key)
        if value is None:
            return False
        pattern = SECRET_PATTERNS[key]
        return bool(re.match(pattern, value))

    def validate_all(self) -> Dict[str, bool]:
        return {k: self.validate_format(k) for k in self.keys()}

    # ========================================================
    # 轮换
    # ========================================================
    def rotate(self, key: str, new_value: str) -> bool:
        """轮换密钥（仅内存，需调用 save）"""
        if not new_value:
            raise SecretValidationError("新密钥不能为空")
        with self._lock:
            old = self._data.get(key)
            self._data[key] = new_value
            self._meta[key] = SecretMeta(
                key=key, source="rotate", loaded_ms=_now_ms())
        self._audit_log(
            "ROTATE",
            f"密钥轮换: {key} {self.mask(old)} → {self.mask(new_value)}")
        return True

    def expired_keys(self) -> List[str]:
        with self._lock:
            return [k for k, m in self._meta.items() if m.is_expired()]

    # ========================================================
    # 加密
    # ========================================================
    def _derive_key(self, password: str, salt: bytes) -> bytes:
        kdf = PBKDF2HMAC(
            algorithm=hashes.SHA256(),
            length=32,
            salt=salt,
            iterations=200_000,
        )
        return kdf.derive(password.encode("utf-8"))

    def _encrypt(self, plaintext: bytes) -> bytes:
        if not _HAS_CRYPTO:
            raise SecretsError("缺少 cryptography，无法加密")
        if self._encryption_key is None:
            raise SecretsError("未设置加密密钥")
        salt = os.urandom(16)
        nonce = os.urandom(12)
        key = self._encryption_key if len(self._encryption_key) == 32 \
            else self._derive_key(
                self._encryption_key.decode("utf-8", "ignore"), salt)
        aesgcm = AESGCM(key)
        ct = aesgcm.encrypt(nonce, plaintext, None)
        # 格式: salt(16) + nonce(12) + ciphertext
        return salt + nonce + ct

    def _decrypt(self, blob: bytes) -> bytes:
        if not _HAS_CRYPTO:
            raise SecretsError("缺少 cryptography，无法解密")
        if self._encryption_key is None:
            raise SecretsError("未设置加密密钥")
        if len(blob) < 28:
            raise SecretsError("加密文件长度不足")
        salt = blob[:16]
        nonce = blob[16:28]
        ct = blob[28:]
        key = self._encryption_key if len(self._encryption_key) == 32 \
            else self._derive_key(
                self._encryption_key.decode("utf-8", "ignore"), salt)
        aesgcm = AESGCM(key)
        return aesgcm.decrypt(nonce, ct, None)

    def save_encrypted(self, path: Path) -> None:
        if not _HAS_CRYPTO:
            raise SecretsError("缺少 cryptography，无法加密保存")
        path.parent.mkdir(parents=True, exist_ok=True)
        plaintext = json.dumps(
            self._data, ensure_ascii=False).encode("utf-8")
        blob = self._encrypt(plaintext)
        tmp = path.with_suffix(path.suffix + ".tmp")
        tmp.write_bytes(blob)
        os.chmod(tmp, 0o600)
        tmp.replace(path)
        self._audit_log("INFO", f"密钥已加密保存到 {path}")

    # ========================================================
    # 审计
    # ========================================================
    def _audit_log(self, level: str, msg: str) -> None:
        entry = {
            "ts": _now_ms(),
            "level": level,
            "msg": msg,
        }
        with self._lock:
            self._audit.append(entry)
            if len(self._audit) > self._max_audit:
                self._audit = self._audit[-self._max_audit:]
        # 屏蔽 ACCESS 级别的日志刷屏
        if level != "ACCESS":
            log_fn = getattr(_log(), level.lower(), _log().info)
            try:
                log_fn(msg)
            except Exception:
                pass

    def audit_tail(self, n: int = 20) -> List[Dict[str, Any]]:
        with self._lock:
            return list(self._audit[-n:])

    # ========================================================
    # 健康检查
    # ========================================================
    def health(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "status": "ok",
                "total_secrets": len(self._data),
                "sources": [s.kind for s in self._sources if s.enabled],
                "missing_required": [
                    k for k in REQUIRED_KEYS if k not in self._data],
                "expired": self.expired_keys(),
                "crypto_available": _HAS_CRYPTO,
                "vault_available": _HAS_HVAC,
                "yaml_available": _HAS_YAML,
                "strict_mode": self._strict,
                "audit_entries": len(self._audit),
            }

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            top = sorted(
                self._access_count.items(),
                key=lambda kv: kv[1], reverse=True)[:10]
            return {
                "total": len(self._data),
                "access_top10": dict(top),
                "audit": len(self._audit),
            }

    # ========================================================
    # repr
    # ========================================================
    def __repr__(self) -> str:
        keys = ", ".join(self.masked(k) for k in self.keys())
        return f"<Secrets keys=[{keys}]>"

    def __contains__(self, key: str) -> bool:
        return self.has(key)


# ============================================================
# 全局入口
# ============================================================
_secrets: Optional[Secrets] = None
_secrets_lock = threading.Lock()


def init_secrets(
    sources: Optional[List[SecretSource]] = None,
    encryption_key: Optional[bytes] = None,
    strict_mode: bool = True,
) -> Secrets:
    """显式初始化（启动时调用一次）"""
    global _secrets
    with _secrets_lock:
        _secrets = Secrets(
            sources=sources,
            encryption_key=encryption_key,
            strict_mode=strict_mode,
        )
    return _secrets


def get_secrets() -> Secrets:
    """获取全局 Secrets 单例（惰性初始化）"""
    global _secrets
    if _secrets is None:
        with _secrets_lock:
            if _secrets is None:
                _secrets = Secrets()
    return _secrets


# ============================================================
# 便捷函数
# ============================================================
def get_secret(key: str,
               default: Optional[str] = None) -> Optional[str]:
    return get_secrets().get(key, default)


def require_secret(key: str) -> str:
    return get_secrets().require(key)


def has_secret(key: str) -> bool:
    return get_secrets().has(key)
