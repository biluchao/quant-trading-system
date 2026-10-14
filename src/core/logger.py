"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: logger
version: 2.0.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: 结构化日志（异步/轮转/脱敏/trace_id/多进程安全）

provides:
  - class Logger
  - class LoggerConfig
  - function get_logger
  - function init_logger
  - function set_trace_id
  - function get_trace_id
  - function timed

requires:
  modules: [core.config]
  libraries:
    - python-json-logger>=2.0  (可选)
  resources:
    memory_mb: 32
    cpu_cores: 1
    disk_mb: 1024

health:
  fn: Logger.health
  timeout_ms: 500

assembly:
  order: 11
  parallel: false

tests:
  - tests/test_logger.py

fallback:
  - 文件不可写 → stdout
  - JSON 库缺失 → 文本格式
  - 磁盘满 → 仅 stderr

tags: [core, logging, observability]
---UMS-END---"""

from __future__ import annotations

import os
import re
import sys
import json
import time
import atexit
import logging
import logging.handlers
import queue
import threading
import traceback
from contextvars import ContextVar
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Callable


# ============================================================
# 可选依赖
# ============================================================
try:
    from pythonjsonlogger import jsonlogger  # type: ignore
    _HAS_JSON_LOGGER = True
except ImportError:
    _HAS_JSON_LOGGER = False


# ============================================================
# trace_id（跨协程/线程隔离）
# ============================================================
_trace_id_var: ContextVar[str] = ContextVar("trace_id", default="-")


def set_trace_id(trace_id: str) -> None:
    """设置当前上下文的 trace_id"""
    _trace_id_var.set(trace_id or "-")


def get_trace_id() -> str:
    """获取当前上下文的 trace_id"""
    return _trace_id_var.get()


# ============================================================
# 脱敏
# ============================================================
# 用于匹配常见敏感字段的正则
_PATTERNS = [
    # JSON 或 key=value 形式的 api_key, api_secret, password 等
    re.compile(
        r'(?i)("?(?:api[_-]?key|api[_-]?secret|secret|password|passwd|'
        r'token|private[_-]?key|authorization)"?\s*[:=]\s*")'
        r'([^"]{4,})(")'
    ),
    # Bearer / JWT
    re.compile(r'(?i)(Bearer\s+)([A-Za-z0-9\-._~+/]{20,})'),
    # 长 hex（可能是密钥）
    re.compile(r'\b([a-fA-F0-9]{32,})\b'),
]


def _mask_str(text: str) -> str:
    if not isinstance(text, str) or not text:
        return text
    for pat in _PATTERNS:
        try:
            if pat.groups >= 3:
                text = pat.sub(
                    lambda m: f"{m.group(1)}"
                              f"{m.group(2)[:4]}****"
                              f"{m.group(3)}",
                    text,
                )
            elif pat.groups >= 2:
                text = pat.sub(
                    lambda m: f"{m.group(1)}"
                              f"{m.group(2)[:4]}****",
                    text,
                )
        except Exception:
            pass
    return text


# ============================================================
# 配置
# ============================================================
@dataclass
class LoggerConfig:
    name: str = "quant"
    level: str = "INFO"
    log_dir: Optional[Path] = None
    filename: str = "quant.log"
    max_bytes: int = 100 * 1024 * 1024        # 100 MB
    backup_count: int = 10
    use_json: bool = False
    to_stdout: bool = True
    async_mode: bool = True
    queue_size: int = 10000
    mask_sensitive: bool = True
    sample_rate: float = 1.0                  # 1.0=全部记录
    disk_warn_pct: float = 0.9                # 磁盘使用率告警阈值


# ============================================================
# 采样过滤器
# ============================================================
class _SamplingFilter(logging.Filter):
    def __init__(self, rate: float = 1.0):
        super().__init__()
        self.rate = max(0.0, min(1.0, rate))
        self._counter = 0
        self._lock = threading.Lock()

    def filter(self, record: logging.LogRecord) -> bool:
        if self.rate >= 1.0:
            return True
        if record.levelno >= logging.WARNING:
            return True   # 警告及以上不采样
        with self._lock:
            self._counter += 1
            n = self._counter
        # 用固定间隔采样，非随机，便于重现
        return (n % max(1, int(1 / self.rate))) == 0


# ============================================================
# 脱敏过滤器
# ============================================================
class _MaskFilter(logging.Filter):
    def __init__(self, enabled: bool = True):
        super().__init__()
        self.enabled = enabled

    def filter(self, record: logging.LogRecord) -> bool:
        if not self.enabled:
            return True
        try:
            if isinstance(record.msg, str):
                record.msg = _mask_str(record.msg)
            if record.args:
                if isinstance(record.args, dict):
                    record.args = {
                        k: _mask_str(v) if isinstance(v, str) else v
                        for k, v in record.args.items()
                    }
                elif isinstance(record.args, tuple):
                    record.args = tuple(
                        _mask_str(a) if isinstance(a, str) else a
                        for a in record.args
                    )
        except Exception:
            pass
        return True


# ============================================================
# trace_id 过滤器
# ============================================================
class _TraceFilter(logging.Filter):
    def filter(self, record: logging.LogRecord) -> bool:
        if not hasattr(record, "trace_id"):
            record.trace_id = get_trace_id()
        return True


# ============================================================
# Formatter
# ============================================================
class _TextFormatter(logging.Formatter):
    DEFAULT_FMT = (
        "%(asctime)s | %(levelname)-7s | %(name)s | "
        "[%(trace_id)s] | %(filename)s:%(lineno)d | %(message)s"
    )

    def __init__(self) -> None:
        super().__init__(fmt=self.DEFAULT_FMT, datefmt="%Y-%m-%d %H:%M:%S")

    def format(self, record: logging.LogRecord) -> str:
        if not hasattr(record, "trace_id"):
            record.trace_id = "-"
        return super().format(record)


class _JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        payload: Dict[str, Any] = {
            "ts": int(record.created * 1000),
            "level": record.levelname,
            "logger": record.name,
            "msg": record.getMessage(),
            "trace_id": getattr(record, "trace_id", "-"),
            "file": record.filename,
            "line": record.lineno,
            "func": record.funcName,
            "pid": record.process,
            "thread": record.threadName,
        }
        if record.exc_info:
            payload["exc"] = self.formatException(record.exc_info)
        if hasattr(record, "extra") and isinstance(record.extra, dict):
            payload.update(record.extra)
        try:
            return json.dumps(payload, ensure_ascii=False)
        except Exception:
            return str(payload)


# ============================================================
# Logger 单例
# ============================================================
class Logger:
    """
    线程安全、异步、支持热更新、脱敏、多进程的日志管理器。
    """

    _instance: Optional["Logger"] = None
    _lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "Logger":
        with cls._lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(self, config: Optional[LoggerConfig] = None) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._lock:
            if getattr(self, "_initialized", False):
                return
            self.config = config or LoggerConfig()
            self._loggers: Dict[str, logging.Logger] = {}
            self._queue: Optional[queue.Queue] = None
            self._listener: Optional[logging.handlers.QueueListener] = None
            self._handlers: List[logging.Handler] = []
            self._running = False
            self._stats = {
                "emitted": 0,
                "dropped": 0,
                "last_error": "",
                "started_at": int(time.time() * 1000),
            }
            self._initialized = True

            self._setup_root()
            atexit.register(self.shutdown)

    # ========================================================
    # 初始化
    # ========================================================
    def _resolve_log_dir(self) -> Path:
        if self.config.log_dir:
            return Path(self.config.log_dir)
        project_root = Path(__file__).resolve().parents[2]
        return project_root / "data" / "logs"

    def _build_handlers(self, log_path: Optional[Path]) -> List[logging.Handler]:
        handlers: List[logging.Handler] = []

        # stdout
        if self.config.to_stdout:
            sh = logging.StreamHandler(stream=sys.stdout)
            handlers.append(sh)

        # file（失败时降级）
        if log_path is not None:
            try:
                log_path.parent.mkdir(parents=True, exist_ok=True)
                fh = logging.handlers.RotatingFileHandler(
                    filename=str(log_path),
                    maxBytes=self.config.max_bytes,
                    backupCount=self.config.backup_count,
                    encoding="utf-8",
                )
                handlers.append(fh)
            except Exception as exc:
                # 降级：只输出到 stderr
                sys.stderr.write(
                    f"[logger] 文件日志不可用: {exc}，降级到 stderr\n"
                )
                eh = logging.StreamHandler(stream=sys.stderr)
                handlers.append(eh)

        # 统一 formatter 与 filter
        formatter: logging.Formatter = (
            _JsonFormatter() if self.config.use_json
            else _TextFormatter()
        )
        sampling = _SamplingFilter(self.config.sample_rate)
        masking = _MaskFilter(self.config.mask_sensitive)
        tracer = _TraceFilter()

        for h in handlers:
            h.setFormatter(formatter)
            h.addFilter(sampling)
            h.addFilter(masking)
            h.addFilter(tracer)

        return handlers

    def _setup_root(self) -> None:
        log_path: Optional[Path] = None
        try:
            log_dir = self._resolve_log_dir()
            log_path = log_dir / self.config.filename
        except Exception:
            log_path = None

        self._handlers = self._build_handlers(log_path)
        level = getattr(logging, self.config.level.upper(),
                        logging.INFO)

        if self.config.async_mode:
            try:
                self._queue = queue.Queue(maxsize=self.config.queue_size)
                self._listener = logging.handlers.QueueListener(
                    self._queue, *self._handlers,
                    respect_handler_level=True,
                )
                self._listener.start()
                self._running = True
            except Exception as exc:
                sys.stderr.write(
                    f"[logger] 异步模式启动失败: {exc}，降级同步\n"
                )
                self._queue = None
                self._listener = None

        root = logging.getLogger()
        # 清理旧 handler，避免重复
        for h in list(root.handlers):
            root.removeHandler(h)
        root.setLevel(level)

        if self._running and self._queue is not None:
            qh = logging.handlers.QueueHandler(self._queue)
            qh.setLevel(level)
            root.addHandler(qh)
        else:
            for h in self._handlers:
                h.setLevel(level)
                root.addHandler(h)

        # 屏蔽第三方库噪声
        for noisy in ("urllib3", "asyncio", "httpx", "websockets"):
            logging.getLogger(noisy).setLevel(logging.WARNING)

    # ========================================================
    # 获取 logger
    # ========================================================
    def get(self, name: str = "") -> logging.Logger:
        full = f"{self.config.name}.{name}" if name else self.config.name
        with self._lock:
            if full not in self._loggers:
                self._loggers[full] = logging.getLogger(full)
            return self._loggers[full]

    # ========================================================
    # 动态调整级别
    # ========================================================
    def set_level(self, level: str) -> bool:
        try:
            lvl = getattr(logging, level.upper())
        except AttributeError:
            return False
        with self._lock:
            self.config.level = level.upper()
            logging.getLogger().setLevel(lvl)
            for h in self._handlers:
                h.setLevel(lvl)
        return True

    def get_level(self) -> str:
        return self.config.level

    # ========================================================
    # 磁盘监控
    # ========================================================
    def check_disk(self) -> Dict[str, Any]:
        try:
            log_dir = self._resolve_log_dir()
            usage = self._disk_usage(log_dir)
            return {
                "path": str(log_dir),
                "usage_pct": usage,
                "warning": usage >= self.config.disk_warn_pct,
            }
        except Exception as exc:
            return {"path": "-", "usage_pct": 0.0,
                    "warning": False, "error": str(exc)}

    @staticmethod
    def _disk_usage(path: Path) -> float:
        try:
            import shutil
            total, used, free = shutil.disk_usage(str(path))
            if total == 0:
                return 0.0
            return used / total
        except Exception:
            return 0.0

    # ========================================================
    # 健康 / 状态
    # ========================================================
    def health(self) -> Dict[str, Any]:
        return {
            "status": "ok" if self._running or self._handlers else "degraded",
            "level": self.config.level,
            "async": self._running,
            "handlers": len(self._handlers),
            "loggers": len(self._loggers),
            "stats": dict(self._stats),
            "disk": self.check_disk(),
            "json_available": _HAS_JSON_LOGGER,
        }

    def stats(self) -> Dict[str, Any]:
        return dict(self._stats)

    # ========================================================
    # 关闭
    # ========================================================
    def shutdown(self) -> None:
        with self._lock:
            if not self._initialized:
                return
            try:
                if self._listener is not None:
                    self._listener.stop()
                    self._listener = None
            except Exception:
                pass
            for h in self._handlers:
                try:
                    h.flush()
                    h.close()
                except Exception:
                    pass
            self._handlers.clear()
            self._running = False
            self._initialized = False


# ============================================================
# 全局入口
# ============================================================
_logger_manager: Optional[Logger] = None
_global_lock = threading.Lock()


def init_logger(config: Optional[LoggerConfig] = None) -> Logger:
    """显式初始化（启动时调用一次）"""
    global _logger_manager
    with _global_lock:
        _logger_manager = Logger(config)
    return _logger_manager


def get_logger(name: str = "") -> logging.Logger:
    """获取命名 logger（惰性初始化全局实例）"""
    global _logger_manager
    if _logger_manager is None:
        with _global_lock:
            if _logger_manager is None:
                _logger_manager = Logger()
    return _logger_manager.get(name)


def get_logger_manager() -> Logger:
    """获取全局 Logger 管理器"""
    global _logger_manager
    if _logger_manager is None:
        with _global_lock:
            if _logger_manager is None:
                _logger_manager = Logger()
    return _logger_manager


# ============================================================
# 性能计时装饰器
# ============================================================
def timed(
    logger_name: str = "perf",
    threshold_ms: float = 100.0,
    level: str = "INFO",
) -> Callable:
    """
    计时装饰器：同步/异步函数均支持。
    超过 threshold_ms 强制以 WARNING 输出。
    """
    log = get_logger(logger_name)
    lvl = getattr(logging, level.upper(), logging.INFO)

    def _sync_decorator(func: Callable) -> Callable:
        def wrapper(*args: Any, **kwargs: Any) -> Any:
            start = time.perf_counter()
            try:
                return func(*args, **kwargs)
            finally:
                elapsed = (time.perf_counter() - start) * 1000
                msg = f"{func.__name__} 耗时 {elapsed:.2f}ms"
                if elapsed >= threshold_ms:
                    log.warning(msg)
                else:
                    log.log(lvl, msg)
        wrapper.__name__ = func.__name__
        return wrapper

    def _async_decorator(func: Callable) -> Callable:
        async def wrapper(*args: Any, **kwargs: Any) -> Any:
            start = time.perf_counter()
            try:
                return await func(*args, **kwargs)
            finally:
                elapsed = (time.perf_counter() - start) * 1000
                msg = f"{func.__name__} 耗时 {elapsed:.2f}ms"
                if elapsed >= threshold_ms:
                    log.warning(msg)
                else:
                    log.log(lvl, msg)
        wrapper.__name__ = func.__name__
        return wrapper

    def decorator(func: Callable) -> Callable:
        if callable(func) and _is_coroutine(func):
            return _async_decorator(func)
        return _sync_decorator(func)

    return decorator


def _is_coroutine(func: Callable) -> bool:
    import inspect
    return inspect.iscoroutinefunction(func)


# ============================================================
# 上下文管理器（自动注入 trace_id）
# ============================================================
class trace_context:
    """with trace_context("trade-001"): ..."""

    def __init__(self, trace_id: str):
        self.trace_id = trace_id or "-"
        self._token = None

    def __enter__(self) -> "trace_context":
        self._token = _trace_id_var.set(self.trace_id)
        return self

    def __exit__(self, *args: Any) -> None:
        if self._token is not None:
            _trace_id_var.reset(self._token)
