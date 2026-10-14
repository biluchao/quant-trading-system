"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: package
name: __init__
version: 2.0.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: core 包入口（惰性加载/统一初始化/优雅关闭/降级）

provides:
  - function bootstrap
  - function shutdown
  - function core_health
  - function core_stats
  - function platform_info
  - constant __version__
  - constant __all__

requires:
  modules: []
  libraries: []
  resources:
    memory_mb: 4
    cpu_cores: 1

health:
  fn: core_health
  timeout_ms: 1000

assembly:
  order: 5
  parallel: false

tests:
  - tests/test_core_package.py

fallback:
  - 子模块缺失 → 提供 stub 对象
  - bootstrap 部分失败 → 继续加载其他模块
  - shutdown 异常 → 强制继续

tags: [core, package, bootstrap]
---UMS-END---"""

from __future__ import annotations

import sys
import threading
import importlib
import traceback
from types import ModuleType
from typing import Any, Callable, Dict, List, Optional, Tuple, TYPE_CHECKING

# ============================================================
# 版本
# ============================================================
__version__ = "2.0.0"
__author__ = "quant-team"
__all__ = [
    # 常量
    "__version__",
    # 生命周期
    "bootstrap",
    "shutdown",
    "is_ready",
    # 状态
    "core_health",
    "core_stats",
    "platform_info",
    "get_module_status",
    # 异常
    "CoreError",
    "BootstrapError",
    "ShutdownError",
    # 子模块（惰性）
    "config",
    "logger",
    "timestamp",
    "event_bus",
    "secrets",
    "compat",
    "process_pool",
    "cpu_affinity",
    "http_server",
    "signal_handler",
    "health",
]

# ============================================================
# 类型提示（仅类型检查时导入，避免运行时循环依赖）
# ============================================================
if TYPE_CHECKING:
    from .config import ConfigManager as _ConfigManager  # noqa: F401
    from .logger import Logger as _Logger  # noqa: F401
    from .event_bus import EventBus as _EventBus  # noqa: F401
    from .health import HealthRegistry as _HealthRegistry  # noqa: F401


# ============================================================
# 异常层次
# ============================================================
class CoreError(Exception):
    """core 包基类异常"""


class BootstrapError(CoreError):
    """启动失败"""


class ShutdownError(CoreError):
    """关闭失败"""


class ModuleNotAvailableError(CoreError):
    """子模块不可用"""


# ============================================================
# 内部状态
# ============================================================
_lock = threading.RLock()

# 模块加载状态：name -> (ok, error)
_module_status: Dict[str, Tuple[bool, str]] = {}

# 已加载模块缓存：name -> module
_module_cache: Dict[str, ModuleType] = {}

# 初始化过的单例
_singletons: Dict[str, Any] = {}

# 启动/关闭记录
_bootstrap_done = False
_shutdown_done = False
_bootstrap_started_ms = 0
_shutdown_started_ms = 0

# 惰性映射：属性名 → 子模块名
_LAZY_MAP: Dict[str, str] = {
    "config": "config",
    "logger": "logger",
    "timestamp": "timestamp",
    "event_bus": "event_bus",
    "secrets": "secrets",
    "compat": "compat",
    "process_pool": "process_pool",
    "cpu_affinity": "cpu_affinity",
    "http_server": "http_server",
    "signal_handler": "signal_handler",
    "health": "health",
}

# 内部日志（延迟初始化，避免循环依赖）
_internal_logger = None


def _log() -> Any:
    """获取日志器（延迟，避免循环）"""
    global _internal_logger
    if _internal_logger is None:
        try:
            mod = _load_module("logger")
            _internal_logger = mod.get_logger("core")
        except Exception:
            import logging
            _internal_logger = logging.getLogger("core")
    return _internal_logger


# ============================================================
# 惰性模块加载
# ============================================================
def _load_module(name: str) -> ModuleType:
    """
    惰性加载并缓存子模块。失败时抛 ModuleNotAvailableError。
    """
    if name in _module_cache:
        return _module_cache[name]
    with _lock:
        if name in _module_cache:
            return _module_cache[name]
        try:
            full = f"{__name__}.{name}"
            mod = importlib.import_module(full)
            _module_cache[name] = mod
            _module_status[name] = (True, "")
            return mod
        except Exception as exc:
            _module_status[name] = (
                False, f"{type(exc).__name__}: {exc}")
            raise ModuleNotAvailableError(
                f"子模块 {name} 加载失败: {exc}") from exc


def _try_load(name: str) -> Optional[ModuleType]:
    """尝试加载，失败返回 None（不抛异常）"""
    try:
        return _load_module(name)
    except Exception as exc:
        _log().warning(f"子模块 {name} 不可用: {exc}")
        return None


def __getattr__(name: str) -> Any:
    """
    PEP 562 模块级惰性属性。
    访问 core.config 等时按需加载。
    """
    if name in _LAZY_MAP:
        return _load_module(_LAZY_MAP[name])
    raise AttributeError(
        f"module {__name__!r} has no attribute {name!r}")


def __dir__() -> List[str]:
    return sorted(set(list(globals().keys()) + list(_LAZY_MAP.keys())))


# ============================================================
# 单例注册表
# ============================================================
def _get_singleton(
    key: str, factory: Callable[[], Any],
) -> Any:
    """获取或创建单例"""
    if key in _singletons:
        return _singletons[key]
    with _lock:
        if key in _singletons:
            return _singletons[key]
        try:
            obj = factory()
            _singletons[key] = obj
            return obj
        except Exception as exc:
            _log().error(f"创建单例 {key} 失败: {exc}")
            raise


def get_config() -> Any:
    """获取配置管理器"""
    def _f() -> Any:
        mod = _load_module("config")
        fn = getattr(mod, "get_config", None)
        if fn is None:
            raise ModuleNotAvailableError("config.get_config 缺失")
        return fn()
    return _get_singleton("config", _f)


def get_logger(name: str = "") -> Any:
    """获取日志器"""
    def _f() -> Any:
        mod = _load_module("logger")
        return mod.get_logger(name)
    # 日志器不带单例（按名区分）
    return _f()


def get_event_bus() -> Any:
    """获取事件总线"""
    def _f() -> Any:
        mod = _load_module("event_bus")
        return mod.get_event_bus()
    return _get_singleton("event_bus", _f)


def get_secrets() -> Any:
    """获取密钥管理"""
    def _f() -> Any:
        mod = _load_module("secrets")
        return mod.get_secrets()
    return _get_singleton("secrets", _f)


def get_health() -> Any:
    """获取健康注册中心"""
    def _f() -> Any:
        mod = _load_module("health")
        return mod.get_health()
    return _get_singleton("health", _f)


# ============================================================
# 启动流程
# ============================================================
def _step_env_check() -> None:
    """阶段 1：环境检查"""
    mod = _try_load("compat")
    if mod is None:
        raise BootstrapError("compat 模块不可用")
    mod.check_python_version()
    _log().info(
        f"环境检查通过: Python {sys.version.split()[0]}, "
        f"platform={sys.platform}")


def _step_install_event_loop(prefer_uvloop: bool = True) -> str:
    """阶段 2：事件循环安装"""
    mod = _try_load("compat")
    if mod is None:
        return "asyncio"
    return mod.install(prefer_uvloop=prefer_uvloop)


def _step_init_config() -> None:
    """阶段 3：配置"""
    mod = _try_load("config")
    if mod is None:
        _log().warning("config 模块不可用，使用默认参数")
        return
    init_fn = getattr(mod, "init_config", None)
    if init_fn is not None:
        init_fn()
    else:
        # 兼容 get_config 惰性初始化
        _get_singleton(
            "config", lambda: getattr(mod, "get_config")())


def _step_init_logger() -> None:
    """阶段 4：日志"""
    mod = _try_load("logger")
    if mod is None:
        return
    init_fn = getattr(mod, "init_logger", None)
    if init_fn is not None:
        try:
            init_fn()
        except Exception as exc:
            _log().warning(f"日志初始化失败: {exc}")


def _step_init_timestamp() -> None:
    """阶段 5：时间同步"""
    mod = _try_load("timestamp")
    if mod is None:
        _log().warning("timestamp 模块不可用，使用本地时间")
        return
    init_fn = getattr(mod, "init_timestamp", None)
    if init_fn is not None:
        try:
            init_fn(auto_start=True)
        except Exception as exc:
            _log().warning(f"时间同步失败: {exc}")


def _step_init_secrets(strict_mode: bool = True) -> None:
    """阶段 6：密钥"""
    mod = _try_load("secrets")
    if mod is None:
        _log().warning("secrets 模块不可用")
        return
    init_fn = getattr(mod, "init_secrets", None)
    if init_fn is not None:
        try:
            init_fn(strict_mode=strict_mode)
        except Exception as exc:
            _log().warning(f"密钥加载失败: {exc}")


def _step_init_event_bus(
    loop: Any = None, auto_start: bool = True,
) -> None:
    """阶段 7：事件总线"""
    mod = _try_load("event_bus")
    if mod is None:
        _log().warning("event_bus 模块不可用")
        return
    init_fn = getattr(mod, "init_event_bus", None)
    if init_fn is not None:
        try:
            init_fn(async_loop=loop, auto_start=auto_start)
        except Exception as exc:
            _log().warning(f"事件总线初始化失败: {exc}")


def _step_init_process_pool(
    cpu_workers: Optional[int] = None,
    warmup: bool = True,
) -> None:
    """阶段 8：进程池"""
    mod = _try_load("process_pool")
    if mod is None:
        _log().warning("process_pool 模块不可用")
        return
    init_fn = getattr(mod, "init_process_pool", None)
    if init_fn is not None:
        try:
            init_fn(cpu_workers=cpu_workers, warmup=warmup)
        except Exception as exc:
            _log().warning(f"进程池初始化失败: {exc}")


def _step_init_affinity() -> None:
    """阶段 9：CPU 亲和性（可选）"""
    mod = _try_load("cpu_affinity")
    if mod is None:
        return
    init_fn = getattr(mod, "init_cpu_affinity", None)
    if init_fn is not None:
        try:
            init_fn(enable=True)
        except Exception as exc:
            _log().debug(f"CPU 亲和性未启用: {exc}")


def _step_init_signal_handler(loop: Any = None) -> None:
    """阶段 10：信号处理"""
    mod = _try_load("signal_handler")
    if mod is None:
        _log().warning("signal_handler 模块不可用")
        return
    init_fn = getattr(mod, "init_signal_handler", None)
    if init_fn is not None:
        try:
            init_fn(install=True, loop=loop)
        except Exception as exc:
            _log().warning(f"信号处理器安装失败: {exc}")


def _step_init_health() -> None:
    """阶段 11：健康注册中心"""
    mod = _try_load("health")
    if mod is None:
        _log().warning("health 模块不可用")
        return
    try:
        init_fn = getattr(mod, "init_health", None)
        if init_fn is not None:
            # 尝试接上 event_bus
            publisher = None
            try:
                bus = get_event_bus()
                publisher = lambda topic, data: bus.publish(topic, data)
            except Exception:
                pass
            init_fn(event_publisher=publisher)
    except Exception as exc:
        _log().warning(f"健康中心初始化失败: {exc}")


def _step_init_http(
    host: Optional[str] = None,
    port: Optional[int] = None,
) -> None:
    """阶段 12：HTTP 服务"""
    mod = _try_load("http_server")
    if mod is None:
        _log().warning("http_server 模块不可用")
        return
    init_fn = getattr(mod, "init_http_server", None)
    if init_fn is None:
        return
    try:
        cfg_cls = getattr(mod, "HTTPConfig", None)
        cfg = None
        if cfg_cls is not None:
            kwargs: Dict[str, Any] = {}
            if host is not None:
                kwargs["host"] = host
            if port is not None:
                kwargs["port"] = port
            cfg = cfg_cls(**kwargs)
        init_fn(config=cfg, autostart=True)
    except Exception as exc:
        _log().warning(f"HTTP 服务初始化失败: {exc}")


# ============================================================
# 统一入口
# ============================================================
def bootstrap(
    *,
    prefer_uvloop: bool = True,
    loop: Any = None,
    secrets_strict: bool = True,
    cpu_workers: Optional[int] = None,
    pool_warmup: bool = True,
    http_host: Optional[str] = None,
    http_port: Optional[int] = None,
    fail_fast: bool = False,
) -> Dict[str, Any]:
    """
    统一启动入口。按序初始化所有核心模块。

    fail_fast=True 时任一阶段失败立即抛 BootstrapError；
    fail_fast=False 时记录警告并继续（生产推荐）。
    """
    global _bootstrap_done, _bootstrap_started_ms

    with _lock:
        if _bootstrap_done:
            _log().info("bootstrap 已执行，跳过")
            return core_stats()
        _bootstrap_started_ms = _now_ms()
        _bootstrap_done = False

    _log().info("=== core.bootstrap 开始 ===")

    steps: List[Tuple[str, Callable[[], Any]]] = [
        ("env_check", _step_env_check),
        ("install_loop",
         lambda: _step_install_event_loop(prefer_uvloop)),
        ("init_config", _step_init_config),
        ("init_logger", _step_init_logger),
        ("init_timestamp", _step_init_timestamp),
        ("init_secrets", lambda: _step_init_secrets(secrets_strict)),
        ("init_event_bus",
         lambda: _step_init_event_bus(loop=loop, auto_start=True)),
        ("init_process_pool",
         lambda: _step_init_process_pool(cpu_workers, pool_warmup)),
        ("init_affinity", _step_init_affinity),
        ("init_signal_handler",
         lambda: _step_init_signal_handler(loop=loop)),
        ("init_health", _step_init_health),
        ("init_http",
         lambda: _step_init_http(http_host, http_port)),
    ]

    results: Dict[str, Any] = {}
    for name, fn in steps:
        try:
            r = fn()
            results[name] = {"status": "ok", "result": r}
        except Exception as exc:
            tb = traceback.format_exc(limit=3)
            _log().error(f"bootstrap 阶段 {name} 失败: {exc}")
            results[name] = {
                "status": "failed",
                "error": f"{type(exc).__name__}: {exc}",
                "traceback": tb,
            }
            if fail_fast:
                raise BootstrapError(
                    f"阶段 {name} 失败: {exc}") from exc

    with _lock:
        _bootstrap_done = True

    _log().info(
        f"=== core.bootstrap 完成 "
        f"({_now_ms() - _bootstrap_started_ms}ms) ===")

    results["ready"] = is_ready()
    return results


def shutdown(
    *,
    total_timeout_s: float = 30.0,
    force: bool = False,
) -> Dict[str, Any]:
    """
    统一关闭入口。反序关闭所有核心模块。
    """
    global _shutdown_done, _shutdown_started_ms

    with _lock:
        if _shutdown_done:
            return {"status": "already_done"}
        _shutdown_started_ms = _now_ms()

    _log().info("=== core.shutdown 开始 ===")

    results: Dict[str, Any] = {}

    # 关闭顺序：HTTP → 信号 → 进程池 → event_bus → timestamp
    #           → secrets → logger → config
    steps: List[Tuple[str, Callable[[], Any]]] = [
        ("http_server", _shutdown_http),
        ("signal_handler", _shutdown_signal),
        ("process_pool", _shutdown_pool),
        ("event_bus", _shutdown_bus),
        ("timestamp", _shutdown_timestamp),
        ("logger", _shutdown_logger),
    ]

    for name, fn in steps:
        try:
            r = fn()
            results[name] = {"status": "ok", "result": r}
        except Exception as exc:
            _log().warning(f"关闭 {name} 失败: {exc}")
            results[name] = {
                "status": "failed",
                "error": f"{type(exc).__name__}: {exc}",
            }

    with _lock:
        _shutdown_done = True
        _singletons.clear()

    _log().info(
        f"=== core.shutdown 完成 "
        f"({_now_ms() - _shutdown_started_ms}ms) ===")
    results["forced"] = force
    return results


def _shutdown_http() -> Any:
    mod = _module_cache.get("http_server")
    if mod is None:
        return None
    fn = getattr(mod, "stop_http_server", None)
    return fn() if fn else None


def _shutdown_signal() -> Any:
    mod = _module_cache.get("signal_handler")
    if mod is None:
        return None
    handler = _singletons.get("signal_handler")
    if handler is None:
        return None
    try:
        handler.uninstall()
    except Exception:
        pass
    return None


def _shutdown_pool() -> Any:
    mod = _module_cache.get("process_pool")
    if mod is None:
        return None
    fn = getattr(mod, "shutdown_process_pool", None)
    return fn() if fn else None


def _shutdown_bus() -> Any:
    mod = _module_cache.get("event_bus")
    if mod is None:
        return None
    fn = getattr(mod, "shutdown_event_bus", None)
    return fn() if fn else None


def _shutdown_timestamp() -> Any:
    mod = _module_cache.get("timestamp")
    if mod is None:
        return None
    ts = mod.get_timestamp() if hasattr(mod, "get_timestamp") else None
    if ts and hasattr(ts, "stop_background_sync"):
        try:
            ts.stop_background_sync()
        except Exception:
            pass
    return None


def _shutdown_logger() -> Any:
    mod = _module_cache.get("logger")
    if mod is None:
        return None
    mgr_fn = getattr(mod, "get_logger_manager", None)
    if mgr_fn is None:
        return None
    try:
        mgr = mgr_fn()
        if hasattr(mgr, "shutdown"):
            mgr.shutdown()
    except Exception:
        pass
    return None


def is_ready() -> bool:
    """系统是否已 bootstrap 完成"""
    with _lock:
        return _bootstrap_done and not _shutdown_done


# ============================================================
# 状态
# ============================================================
def _now_ms() -> int:
    try:
        mod = _module_cache.get("timestamp")
        if mod is not None:
            return mod.now_ms()
    except Exception:
        pass
    import time
    return int(time.time() * 1000)


def get_module_status() -> Dict[str, Dict[str, Any]]:
    """返回各子模块加载状态"""
    with _lock:
        return {
            name: {
                "loaded": ok,
                "error": err,
                "cached": name in _module_cache,
            }
            for name, (ok, err) in _module_status.items()
        }


def core_stats() -> Dict[str, Any]:
    """核心状态统计"""
    with _lock:
        return {
            "version": __version__,
            "bootstrap_done": _bootstrap_done,
            "shutdown_done": _shutdown_done,
            "bootstrap_started_ms": _bootstrap_started_ms,
            "shutdown_started_ms": _shutdown_started_ms,
            "singletons": list(_singletons.keys()),
            "modules_loaded": list(_module_cache.keys()),
            "modules_status": dict(_module_status),
        }


def core_health() -> Dict[str, Any]:
    """
    聚合核心健康。
    返回 {status, modules, ready, details}
    """
    modules: Dict[str, Dict[str, Any]] = {}
    overall = "ok"

    # 遍历已加载模块的 health()
    for name, mod in list(_module_cache.items()):
        health_fn = getattr(mod, "health", None)
        if health_fn is None:
            continue
        try:
            result = health_fn()
            if isinstance(result, dict):
                modules[name] = result
            else:
                modules[name] = {"status": "unknown",
                                 "raw": str(result)}
        except Exception as exc:
            modules[name] = {
                "status": "error",
                "error": f"{type(exc).__name__}: {exc}",
            }
            overall = "degraded"

    # 未加载模块标记
    for name in _LAZY_MAP:
        if name in modules:
            continue
        if name in _module_status:
            ok, err = _module_status[name]
            modules[name] = {
                "status": "ok" if ok else "unavailable",
                "error": err,
            }
            if not ok:
                overall = "degraded"

    ready = is_ready()
    return {
        "status": overall,
        "ready": ready,
        "version": __version__,
        "modules": modules,
        "stats": core_stats(),
        "ts_ms": _now_ms(),
    }


def platform_info() -> Dict[str, Any]:
    """平台信息"""
    try:
        mod = _load_module("compat")
        return mod.platform_info()
    except Exception:
        import platform
        return {
            "python_version": sys.version.split()[0],
            "platform": sys.platform,
            "machine": platform.machine(),
            "cpu_count": getattr(os, "cpu_count", lambda: 0)(),
        }


# ============================================================
# repr
# ============================================================
def __repr__() -> str:
    return (
        f"<core v{__version__} "
        f"bootstrap={_bootstrap_done} "
        f"shutdown={_shutdown_done} "
        f"modules={len(_module_cache)}>"
    )


# 兼容 os 引用（platform_info 使用）
import os  # noqa: E402


# ============================================================
# 模块退出时不做任何副作用（确保 import 无害）
# ============================================================
# 注：不在 __init__ 中注册 atexit，交由 signal_handler / http_server
#     各自的 shutdown 机制处理，避免重复清理。
