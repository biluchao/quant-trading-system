#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
币安 BTC/ETH 3分钟量化交易系统 - 环境变量校验脚本
================================================================================
@file    scripts/check_env.py
@version 1.0.2
@author  quant-team
@brief   启动前校验 .env 文件，确保所有必要变量已配置且合法
         (第二轮修复：18 个运行时问题)

使用方法:
    python scripts/check_env.py
    python scripts/check_env.py --env prod --strict
    python scripts/check_env.py --json             # JSON 输出
    python scripts/check_env.py --quiet            # 静默模式
    python scripts/check_env.py --log check.log    # 日志文件

退出码:
    0 - 校验通过
    1 - 校验失败
    2 - 文件不存在或不可读
    3 - 内部错误
    4 - 被信号中断
    5 - 并发执行冲突
================================================================================
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import io
import json
import os
import re
import signal
import stat
import sys
import tempfile
import time
from contextlib import contextmanager
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Dict, Iterator, List, Optional, Tuple
from urllib.parse import urlparse

# ==============================================================================
# 依赖检查
# ==============================================================================

try:
    from dotenv import load_dotenv
except ImportError:
    sys.stderr.write("❌ 缺少 python-dotenv 依赖\n")
    sys.stderr.write("   安装: pip install python-dotenv\n")
    sys.exit(3)


# ==============================================================================
# 常量
# ==============================================================================

VERSION = "1.0.2"

# 文件限制
MAX_ENV_FILE_SIZE = 1 * 1024 * 1024     # 1 MB
MAX_ENV_LINES = 10000                    # 最多 10000 行
MAX_VALUE_LENGTH = 65536                 # 单值最大 64KB

# 环境变量名正则（POSIX 标准）
ENV_NAME_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

# UTF-8 BOM
UTF8_BOM = b"\xef\xbb\xbf"

# 中断标志
_interrupted = False


# ==============================================================================
# 颜色
# ==============================================================================

def _supports_color() -> bool:
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("CI"):
        return False
    if not hasattr(sys.stdout, "isatty"):
        return False
    if not sys.stdout.isatty():
        return False
    if sys.platform == "win32":
        try:
            import ctypes
            kernel32 = ctypes.windll.kernel32
            kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
            return True
        except Exception:
            return False
    return True


class Color:
    _enabled = _supports_color()
    RESET = "\033[0m" if _enabled else ""
    RED = "\033[0;31m" if _enabled else ""
    GREEN = "\033[0;32m" if _enabled else ""
    YELLOW = "\033[0;33m" if _enabled else ""
    BLUE = "\033[0;34m" if _enabled else ""
    CYAN = "\033[0;36m" if _enabled else ""
    BOLD = "\033[1m" if _enabled else ""

    @classmethod
    def disable(cls) -> None:
        for attr in ("RESET", "RED", "GREEN", "YELLOW", "BLUE", "CYAN", "BOLD"):
            setattr(cls, attr, "")


# ==============================================================================
# 敏感关键字
# ==============================================================================

SECRET_KEYWORDS = (
    "SECRET", "PASSWORD", "TOKEN", "KEY", "PHRASE",
    "CREDENTIAL", "PRIVATE",
)

INSECURE_DEFAULTS = {
    "change_me", "change_me_in_production",
    "change_me_use_openssl_rand_hex_32",
    "your_api_key_here", "your_api_secret_here",
    "password", "123456", "admin", "test",
    "example", "xxx", "todo", "fixme",
}


# ==============================================================================
# 变量 Schema
# ==============================================================================

VAR_SCHEMA: Dict[str, Dict[str, Dict[str, Any]]] = {
    "common": {
        "ENV": {"type": "enum", "required": True,
                "choices": ["dev", "test", "staging", "prod"], "desc": "运行环境"},
        "LOG_LEVEL": {"type": "enum", "required": False, "default": "INFO",
                      "choices": ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]},
        "TZ": {"type": "string", "required": False, "default": "UTC"},
        "DEBUG": {"type": "bool", "required": False, "default": False},
    },
    "exchange": {
        "BINANCE_API_KEY": {"type": "string", "required": True, "secret": True,
                            "min_length": 10},
        "BINANCE_API_SECRET": {"type": "string", "required": True, "secret": True,
                               "min_length": 10},
        "BINANCE_TESTNET": {"type": "bool", "required": False, "default": True},
        "BINANCE_API_TIMEOUT_MS": {"type": "int", "required": False,
                                   "default": 5000, "min": 100, "max": 60000},
    },
    "trading": {
        "TRADING_SYMBOLS": {"type": "list", "required": True, "min_items": 1,
                            "item_pattern": r"^[A-Z]{2,10}USDT$"},
        "CONTRACT_TYPE": {"type": "enum", "required": False, "default": "PERPETUAL",
                          "choices": ["PERPETUAL", "CURRENT_QUARTER", "NEXT_QUARTER"]},
    },
    "database": {
        "DB_TYPE": {"type": "enum", "required": False, "default": "timescale",
                    "choices": ["postgres", "timescale", "sqlite"]},
        "DB_HOST": {"type": "string", "required": True},
        "DB_PORT": {"type": "int", "required": False, "default": 5432,
                    "min": 1, "max": 65535},
        "DB_NAME": {"type": "string", "required": True},
        "DB_USER": {"type": "string", "required": True},
        "DB_PASSWORD": {"type": "string", "required": True, "secret": True,
                        "min_length": 8},
        "DB_POOL_SIZE": {"type": "int", "required": False, "default": 10,
                         "min": 1, "max": 200},
    },
    "redis": {
        "REDIS_HOST": {"type": "string", "required": True},
        "REDIS_PORT": {"type": "int", "required": False, "default": 6379,
                       "min": 1, "max": 65535},
    },
    "execution": {
        "EXECUTION_MODE": {"type": "enum", "required": True,
                           "choices": ["VIRTUAL", "LIVE"]},
        "ALLOW_LIVE_TRADING": {"type": "bool", "required": False, "default": False},
        "VIRTUAL_INITIAL_BALANCE": {"type": "float", "required": False,
                                    "default": 1000.0, "min": 10.0, "max": 1e9},
    },
    "risk": {
        "MAX_RISK_PER_TRADE": {"type": "float", "required": False,
                               "default": 0.02, "min": 0.001, "max": 0.10},
        "MAX_DAILY_LOSS": {"type": "float", "required": False,
                           "default": 0.05, "min": 0.01, "max": 0.50},
        "MAX_LEVERAGE": {"type": "int", "required": False,
                         "default": 5, "min": 1, "max": 125},
        "MIN_RISK_REWARD_RATIO": {"type": "float", "required": False,
                                  "default": 1.5, "min": 1.0, "max": 10.0},
    },
    "ai": {
        "AI_ENABLED": {"type": "bool", "required": False, "default": True},
        "AI_MODEL_PATH": {"type": "path", "required": False, "default": "./models",
                          "must_exist": False},
        "AI_CONFIDENCE_THRESHOLD": {"type": "float", "required": False,
                                    "default": 0.55, "min": 0.0, "max": 1.0},
    },
    "backend": {
        "BACKEND_PORT": {"type": "int", "required": False,
                         "default": 8000, "min": 1024, "max": 65535},
        "BOOTSTRAP_PORT": {"type": "int", "required": False,
                           "default": 8080, "min": 1024, "max": 65535},
        "JWT_SECRET": {"type": "string", "required": True, "secret": True,
                       "min_length": 32},
        "CORS_ORIGINS": {"type": "list", "required": False,
                         "default": ["http://localhost:3000"]},
    },
}


# ==============================================================================
# 结果数据类
# ==============================================================================

@dataclass
class CheckResult:
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    infos: List[str] = field(default_factory=list)

    def error(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warnings.append(msg)

    def info(self, msg: str) -> None:
        self.infos.append(msg)

    @property
    def passed(self) -> bool:
        return len(self.errors) == 0

    def to_dict(self) -> Dict[str, Any]:
        return {
            "passed": self.passed,
            "errors": self.errors,
            "warnings": self.warnings,
            "infos": self.infos,
            "error_count": len(self.errors),
            "warning_count": len(self.warnings),
        }


# ==============================================================================
# 工具函数
# ==============================================================================

def mask_secret(value: str, visible: int = 4) -> str:
    if not value:
        return "(empty)"
    if len(value) <= visible * 2:
        return "*" * len(value)
    return value[:visible] + "*" * (len(value) - visible * 2) + value[-visible:]


def is_secret_key(key: str) -> bool:
    return any(kw in key.upper() for kw in SECRET_KEYWORDS)


def parse_bool(value: Any) -> Optional[bool]:
    if isinstance(value, bool):
        return value
    if value is None:
        return None
    s = str(value).strip().lower()
    if s in ("true", "1", "yes", "on"):
        return True
    if s in ("false", "0", "no", "off", ""):
        return False
    return None


def parse_int(value: Any) -> Optional[int]:
    try:
        return int(str(value).strip())
    except (ValueError, TypeError):
        return None


def parse_float(value: Any) -> Optional[float]:
    try:
        return float(str(value).strip())
    except (ValueError, TypeError):
        return None


def parse_list(value: Any, sep: str = ",") -> List[str]:
    if isinstance(value, list):
        return value
    if not value:
        return []
    return [x.strip() for x in str(value).split(sep) if x.strip()]


def clean_value(value: Optional[str]) -> str:
    """清理 CRLF 和 BOM"""
    if value is None:
        return ""
    s = str(value)
    # 移除 UTF-8 BOM
    if s.startswith("\ufeff"):
        s = s[1:]
    return s.strip().rstrip("\r\n")


# ==============================================================================
# 文件读取（安全）
# ==============================================================================

class SafeEnvReader:
    """安全读取 .env 文件，处理 BOM、大小、符号链接等"""

    def __init__(self, result: CheckResult):
        self.result = result

    def read(self, path: Path) -> Optional[Dict[str, str]]:
        """安全读取 .env 并返回键值对"""
        # 1. 检查符号链接
        if path.is_symlink():
            target = path.resolve()
            self.result.warn(
                f"{path} 是符号链接，指向 {target}"
            )
            # 检查是否指向敏感路径
            sensitive = ("/etc/", "/root/", "/home/", "/sys/", "/proc/")
            target_str = str(target)
            if any(target_str.startswith(s) for s in sensitive):
                self.result.error(f"符号链接指向敏感路径: {target}")
                return None
            path = target

        # 2. 大小检查（防 DoS）
        try:
            size = path.stat().st_size
            if size > MAX_ENV_FILE_SIZE:
                self.result.error(
                    f".env 文件过大: {size} bytes（最大 {MAX_ENV_FILE_SIZE}）"
                )
                return None
        except OSError as e:
            self.result.error(f"无法获取文件大小: {e}")
            return None

        # 3. 原子读取（避免 TOCTOU）
        try:
            with open(path, "rb") as f:
                raw = f.read(MAX_ENV_FILE_SIZE + 1)
        except FileNotFoundError:
            self.result.error(f"文件不存在: {path}")
            return None
        except PermissionError:
            self.result.error(f"文件无权限读取: {path}")
            return None
        except OSError as e:
            self.result.error(f"读取文件失败: {e}")
            return None

        if len(raw) > MAX_ENV_FILE_SIZE:
            self.result.error(f".env 文件过大（读取时超出限制）")
            return None

        # 4. BOM 处理
        if raw.startswith(UTF8_BOM):
            self.result.warn("检测到 UTF-8 BOM，已自动移除")
            raw = raw[len(UTF8_BOM):]

        # 5. 解码
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as e:
            self.result.error(f"文件编码错误（应为 UTF-8）: {e}")
            return None

        # 6. CRLF 检测
        if "\r\n" in text:
            self.result.warn("检测到 CRLF 换行，建议改为 LF")
            text = text.replace("\r\n", "\n")

        # 7. 行数检查
        lines = text.split("\n")
        if len(lines) > MAX_ENV_LINES:
            self.result.error(
                f".env 行数过多: {len(lines)}（最大 {MAX_ENV_LINES}）"
            )
            return None

        # 8. 解析
        return self._parse_lines(lines)

    def _parse_lines(self, lines: List[str]) -> Dict[str, str]:
        """解析 .env 行，处理重复键、export、行内注释等"""
        result: Dict[str, str] = {}
        seen_keys: Dict[str, int] = {}   # 记录键出现的行号
        line_no = 0

        while line_no < len(lines):
            line = lines[line_no]
            line_no += 1

            # 跳过空行和注释
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue

            # 处理 export 前缀
            if stripped.startswith("export "):
                stripped = stripped[7:].lstrip()

            # 处理多行续行（行尾 \）
            full_line = stripped
            while full_line.endswith("\\") and line_no < len(lines):
                full_line = full_line[:-1] + lines[line_no].strip()
                line_no += 1

            # 分离键值
            if "=" not in full_line:
                self.result.warn(f"第 {line_no} 行格式错误（缺少 =）: {full_line[:50]}")
                continue

            key, _, value = full_line.partition("=")
            key = key.strip()

            # 键名校验
            if not key:
                self.result.error(f"第 {line_no} 行变量名为空")
                continue

            if not ENV_NAME_PATTERN.match(key):
                self.result.error(
                    f"第 {line_no} 行变量名非法: '{key}'"
                    f"（应匹配 [A-Za-z_][A-Za-z0-9_]*）"
                )
                continue

            # 检查保留前缀
            reserved = ("LD_", "PYTHON", "PERL", "RUBY", "NODE_")
            if key.startswith(reserved):
                self.result.warn(
                    f"变量 {key} 使用了保留前缀（可能影响子进程）"
                )

            # 值清理
            value = value.strip()

            # 处理引号
            if len(value) >= 2:
                if value[0] == value[-1] == '"':
                    value = value[1:-1]
                    # 处理转义（仅双引号内）
                    value = value.replace("\\n", "\n").replace("\\t", "\t")
                    value = value.replace('\\"', '"').replace("\\\\", "\\")
                elif value[0] == value[-1] == "'":
                    value = value[1:-1]
                    # 单引号内不转义
            else:
                # 未加引号：处理行内注释
                if " #" in value:
                    value = value.split(" #", 1)[0].rstrip()
                elif "\t#" in value:
                    value = value.split("\t#", 1)[0].rstrip()

            # 值长度检查
            if len(value) > MAX_VALUE_LENGTH:
                self.result.error(
                    f"变量 {key} 的值过长（{len(value)}，最大 {MAX_VALUE_LENGTH}）"
                )
                continue

            # 重复键检查
            if key in result:
                self.result.warn(
                    f"变量 {key} 在第 {seen_keys[key]} 行和第 {line_no} 行重复定义，"
                    f"使用后者"
                )

            result[key] = value
            seen_keys[key] = line_no

        return result


# ==============================================================================
# 并发锁
# ==============================================================================

@contextmanager
def file_lock(lock_path: Path, timeout: int = 5) -> Iterator[bool]:
    """文件锁，防止并发执行"""
    lock_file = None
    try:
        lock_file = open(lock_path, "w")
        start = time.time()
        while True:
            try:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.time() - start > timeout:
                    yield False
                    return
                time.sleep(0.1)
        yield True
    except Exception:
        yield True    # 锁失败不阻断
    finally:
        if lock_file:
            try:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
                lock_file.close()
            except Exception:
                pass


# ==============================================================================
# 校验器
# ==============================================================================

class EnvValidator:
    def __init__(self, env: str = "dev", strict: bool = False, check_files: bool = True):
        self.env = env
        self.strict = strict
        self.check_files = check_files
        self.result = CheckResult()
        self.env_vars: Dict[str, str] = {}
        self.file_values: Dict[str, str] = {}

    # -------------------------------------------------------------------------
    # 加载
    # -------------------------------------------------------------------------
    def load(self, path: Path) -> bool:
        reader = SafeEnvReader(self.result)
        values = reader.read(path)
        if values is None:
            return False

        self.file_values = values

        # 检查文件权限
        if sys.platform != "win32":
            try:
                mode = path.stat().st_mode
                if mode & (stat.S_IRGRP | stat.S_IROTH):
                    self.result.warn(
                        f"{path} 权限过宽（{oct(mode)[-3:]}），建议 chmod 600"
                    )
                else:
                    self.result.info(f"文件权限正确: {oct(mode)[-3:]}")
            except OSError:
                pass

        # 合并：系统环境变量优先
        self.env_vars = {}
        for key, value in values.items():
            self.env_vars[key] = os.environ.get(key, value)

        return True

    # -------------------------------------------------------------------------
    # 校验
    # -------------------------------------------------------------------------
    def validate_var(self, name: str, schema: Dict[str, Any], value: Optional[str]) -> None:
        if value is None or value == "":
            if schema.get("required"):
                self.result.error(f"缺少必填变量: {name}")
                return
            default = schema.get("default")
            if default is not None:
                value = str(default)
            else:
                return

        var_type = schema.get("type", "string")
        checker = getattr(self, f"_check_{var_type}", None)
        if checker:
            checker(name, value, schema)

        # 敏感默认值检查
        if schema.get("secret") and self.env == "prod":
            if value in INSECURE_DEFAULTS:
                self.result.error(f"{name} 使用不安全的默认值（生产环境必须修改）")

    def _check_string(self, name, value, schema):
        min_len = schema.get("min_length")
        max_len = schema.get("max_length")
        if min_len and len(value) < min_len:
            self.result.error(f"{name} 长度不足（最小 {min_len}，当前 {len(value)}）")
        if max_len and len(value) > max_len:
            self.result.error(f"{name} 长度超限（最大 {max_len}）")
        pattern = schema.get("pattern")
        if pattern and not re.match(pattern, value):
            self.result.error(f"{name} 格式错误")

    def _check_int(self, name, value, schema):
        parsed = parse_int(value)
        if parsed is None:
            self.result.error(f"{name} 不是有效整数: {value}")
            return
        min_v, max_v = schema.get("min"), schema.get("max")
        if min_v is not None and parsed < min_v:
            self.result.error(f"{name} 小于最小值 {min_v}")
        if max_v is not None and parsed > max_v:
            self.result.error(f"{name} 大于最大值 {max_v}")

    def _check_float(self, name, value, schema):
        parsed = parse_float(value)
        if parsed is None:
            self.result.error(f"{name} 不是有效浮点数: {value}")
            return
        min_v, max_v = schema.get("min"), schema.get("max")
        if min_v is not None and parsed < min_v:
            self.result.error(f"{name} 小于最小值 {min_v}")
        if max_v is not None and parsed > max_v:
            self.result.error(f"{name} 大于最大值 {max_v}")

    def _check_bool(self, name, value, schema):
        if parse_bool(value) is None:
            self.result.error(f"{name} 不是有效布尔值: {value}")

    def _check_enum(self, name, value, schema):
        choices = schema.get("choices", [])
        if value not in choices:
            self.result.error(f"{name} 不在允许值内（应为 {choices}）")

    def _check_list(self, name, value, schema):
        items = parse_list(value)
        min_items = schema.get("min_items", 0)
        if len(items) < min_items:
            self.result.error(f"{name} 元素数不足（最少 {min_items}）")
        pattern = schema.get("item_pattern")
        if pattern:
            for item in items:
                if not re.match(pattern, item):
                    self.result.error(f"{name} 元素格式错误: {item}")

    def _check_url(self, name, value, schema):
        try:
            r = urlparse(value)
            if not r.scheme or not r.netloc:
                self.result.error(f"{name} 不是有效 URL: {value}")
        except Exception:
            self.result.error(f"{name} 不是有效 URL")

    def _check_path(self, name, value, schema):
        if not self.check_files:
            return
        if schema.get("must_exist", False):
            if not Path(value).expanduser().exists():
                self.result.error(f"{name} 路径不存在: {value}")

    def validate_all(self) -> None:
        for category, vars_schema in VAR_SCHEMA.items():
            for name, schema in vars_schema.items():
                self.validate_var(name, schema, self.env_vars.get(name))

    # -------------------------------------------------------------------------
    # 环境特定检查
    # -------------------------------------------------------------------------
    def check_env_specific(self) -> None:
        if self.env == "prod":
            self._check_production()
        elif self.env == "dev":
            self._check_development()

    def _check_production(self) -> None:
        if parse_bool(self.env_vars.get("DEBUG", "false")):
            self.result.error("生产环境 DEBUG 必须为 false")
        if self.env_vars.get("LOG_LEVEL") == "DEBUG":
            self.result.warn("生产环境 LOG_LEVEL 建议为 INFO 或 WARNING")
        if parse_bool(self.env_vars.get("BINANCE_TESTNET", "true")):
            self.result.warn("生产环境仍使用测试网")

        if self.env_vars.get("EXECUTION_MODE") == "LIVE":
            if not parse_bool(self.env_vars.get("ALLOW_LIVE_TRADING", "false")):
                self.result.error("EXECUTION_MODE=LIVE 但 ALLOW_LIVE_TRADING=false")
            else:
                self.result.info("实盘模式已启用")

        if not parse_bool(self.env_vars.get("DB_SSL", "false")):
            self.result.warn("生产环境建议 DB_SSL=true")

        jwt = self.env_vars.get("JWT_SECRET", "")
        if len(jwt) < 32:
            self.result.error("生产环境 JWT_SECRET 长度必须 ≥ 32")

    def _check_development(self) -> None:
        if self.env_vars.get("EXECUTION_MODE") == "LIVE":
            self.result.warn("开发环境使用实盘模式")

    # -------------------------------------------------------------------------
    # 交叉检查
    # -------------------------------------------------------------------------
    def check_cross_vars(self) -> None:
        ports: Dict[int, str] = {}
        for name in ("BACKEND_PORT", "BOOTSTRAP_PORT"):
            value = self.env_vars.get(name)
            if value:
                p = parse_int(value)
                if p:
                    if p in ports:
                        self.result.error(f"{name} 与 {ports[p]} 端口冲突（{p}）")
                    else:
                        ports[p] = name

        if self.env_vars.get("EXECUTION_MODE") == "VIRTUAL":
            balance = parse_float(self.env_vars.get("VIRTUAL_INITIAL_BALANCE", "1000"))
            if balance is not None and balance < 100:
                self.result.warn(f"虚拟券商余额较低（{balance}）")

        max_risk = parse_float(self.env_vars.get("MAX_RISK_PER_TRADE", "0.02"))
        max_daily = parse_float(self.env_vars.get("MAX_DAILY_LOSS", "0.05"))
        if max_risk and max_daily and max_risk > max_daily / 2:
            self.result.warn(f"单笔风险（{max_risk}）超过每日上限的一半")


# ==============================================================================
# 输出
# ==============================================================================

class Reporter:
    def __init__(self, quiet: bool = False, json_mode: bool = False, log_file: Optional[Path] = None):
        self.quiet = quiet
        self.json_mode = json_mode
        self.log_file = log_file
        self._log_buffer: List[str] = []

    def _emit(self, msg: str = "") -> None:
        if not self.quiet and not self.json_mode:
            print(msg)
        if self.log_file:
            self._log_buffer.append(msg)

    def _emit_err(self, msg: str) -> None:
        if not self.json_mode:
            sys.stderr.write(msg + "\n")
        if self.log_file:
            self._log_buffer.append(msg)

    def header(self, env: str, path: Path, strict: bool) -> None:
        self._emit()
        self._emit(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
        self._emit(f"{Color.CYAN}{Color.BOLD}  环境变量校验  v{VERSION}{Color.RESET}")
        self._emit(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
        self._emit()
        self._emit(f"  环境:  {Color.GREEN}{env}{Color.RESET}")
        self._emit(f"  文件:  {path}")
        self._emit(f"  模式:  {'严格' if strict else '普通'}")
        self._emit()

    def snapshot(self, env_vars: Dict[str, str]) -> None:
        self._emit(f"{Color.BOLD}━━━ 环境变量快照 ━━━{Color.RESET}")
        self._emit()
        for category, vars_schema in VAR_SCHEMA.items():
            self._emit(f"{Color.CYAN}[{category}]{Color.RESET}")
            for name in vars_schema:
                value = env_vars.get(name)
                if value is None:
                    display = f"{Color.YELLOW}(未设置){Color.RESET}"
                elif is_secret_key(name):
                    display = f"{Color.GREEN}{mask_secret(value)}{Color.RESET}"
                else:
                    s = str(value)
                    display = s if len(s) <= 60 else s[:57] + "..."
                self._emit(f"  {name:30s} = {display}")
            self._emit()

    def result(self, res: CheckResult) -> None:
        self._emit(f"{Color.BOLD}━━━ 校验结果 ━━━{Color.RESET}")
        self._emit()
        for msg in res.infos:
            self._emit(f"{Color.BLUE}  [INFO]{Color.RESET} {msg}")
        for msg in res.warnings:
            self._emit(f"{Color.YELLOW}  [WARN]{Color.RESET} {msg}")
        for msg in res.errors:
            self._emit(f"{Color.RED}  [ERR ]{Color.RESET} {msg}")
        self._emit()
        if res.passed:
            if res.warnings:
                self._emit(f"{Color.GREEN}✅ 校验通过（{len(res.warnings)} 个警告）{Color.RESET}")
            else:
                self._emit(f"{Color.GREEN}✅ 校验通过{Color.RESET}")
        else:
            self._emit(f"{Color.RED}❌ 校验失败：{len(res.errors)} 错误 / {len(res.warnings)} 警告{Color.RESET}")

    def json_output(self, res: CheckResult, env: str, path: Path) -> None:
        output = {
            "version": VERSION,
            "env": env,
            "file": str(path),
            "timestamp": int(time.time()),
            **res.to_dict(),
        }
        print(json.dumps(output, ensure_ascii=False, indent=2))

    def flush_log(self) -> None:
        if not self.log_file:
            return
        try:
            with open(self.log_file, "w", encoding="utf-8") as f:
                f.write("\n".join(self._log_buffer))
        except OSError as e:
            sys.stderr.write(f"写入日志失败: {e}\n")


# ==============================================================================
# 信号处理
# ==============================================================================

def _signal_handler(signum: int, frame: Any) -> None:
    global _interrupted
    _interrupted = True
    sys.stderr.write(f"\n收到信号 {signum}，正在退出...\n")
    sys.exit(4)


# ==============================================================================
# 主流程
# ==============================================================================

def run_check(
    env_file: Path,
    env: str,
    strict: bool,
    reporter: Reporter,
) -> int:
    if _interrupted:
        return 4

    reporter.header(env, env_file, strict)

    validator = EnvValidator(env=env, strict=strict)
    if not validator.load(env_file):
        reporter.result(validator.result)
        return 1 if not validator.result.passed else 2

    validator.validate_all()
    validator.check_env_specific()
    validator.check_cross_vars()

    if not reporter.json_mode:
        reporter.snapshot(validator.env_vars)
    reporter.result(validator.result)

    if reporter.json_mode:
        reporter.json_output(validator.result, env, env_file)

    reporter.flush_log()

    if strict and validator.result.warnings:
        return 1
    return 0 if validator.result.passed else 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="环境变量校验脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--env", default=os.environ.get("ENV", "dev"),
                        choices=["dev", "test", "staging", "prod"])
    parser.add_argument("--file", default=".env")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--no-color", action="store_true")
    parser.add_argument("--json", action="store_true", help="JSON 输出")
    parser.add_argument("--quiet", action="store_true", help="静默模式")
    parser.add_argument("--log", type=Path, help="日志文件")
    parser.add_argument("--no-lock", action="store_true", help="禁用并发锁")
    parser.add_argument("--version", action="version", version=f"check_env.py {VERSION}")

    args = parser.parse_args()

    if args.no_color or args.json:
        Color.disable()

    # 信号处理
    signal.signal(signal.SIGINT, _signal_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _signal_handler)

    env_file = Path(args.file).expanduser().resolve()

    reporter = Reporter(
        quiet=args.quiet,
        json_mode=args.json,
        log_file=args.log,
    )

    # 并发锁
    lock_path = Path(tempfile.gettempdir()) / "quant_check_env.lock"

    try:
        if args.no_lock:
            return run_check(env_file, args.env, args.strict, reporter)

        with file_lock(lock_path, timeout=5) as acquired:
            if not acquired:
                sys.stderr.write("❌ 另一个 check_env 进程正在运行\n")
                return 5
            return run_check(env_file, args.env, args.strict, reporter)
    except KeyboardInterrupt:
        return 130
    except Exception as e:
        sys.stderr.write(f"❌ 内部错误: {e}\n")
        import traceback
        traceback.print_exc()
        return 3


if __name__ == "__main__":
    sys.exit(main())
