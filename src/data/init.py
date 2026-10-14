"""---UMS-BEGIN---
spec_version: 1.0
module: data
type: package
name: __init__
version: 2.0.0
level: 2
required: true
author: quant-team
created: 2026-10-14
brief: data 包入口（惰性加载/统一初始化/优雅关闭/降级）

provides:
  - function bootstrap
  - function shutdown
  - function data_health
  - function data_stats
  - function is_ready
  - constant __version__
  - constant __all__

requires:
  modules: [core]
  libraries: []
  resources:
    memory_mb: 4
    cpu_cores: 1

health:
  fn: data_health
  timeout_ms: 2000

assembly:
  order: 22
  parallel: false

tests:
  - tests/test_data_package.py

fallback:
  - 子模块缺失 → 提供 stub
  - bootstrap 部分失败 → 继续其他
  - shutdown 异常 → 强制继续

tags: [data, package, bootstrap]
---UMS-END---"""

from __future__ import annotations

import importlib
import sys
import threading
import traceback
from types import ModuleType
from typing import Any, Callable, Dict, List, Optional, Tuple, TYPE_CHECKING

# ============================================================
# 版本
# ============================================================
__version__ = "2.0.0"
__author__ = "quant-team"
__all__ = [
    "__version__",
    # 生命周期
    "bootstrap",
    "shutdown",
    "is_ready",
    # 状态
    "data_health",
    "data_stats",
    "get_module_status",
    # 异常
    "DataError",
    "BootstrapError",
    "ShutdownError",
    "ModuleNotAvailableError",
    # 子模块（惰性）
    "binance_ws",
    "binance_rest",
    "candle_buffer",
    "cleaner",
    "gap_filler",
    "depth_cleaner",
    "source_router",
    "storage",
    "microstructure",
]

# ============================================================
# 类型提示
# ============================================================
if TYPE_CHECKING:
    from .candle_buffer import CandleBuffer as _CandleBuffer  # noqa
    from .binance_ws import BinanceWS as _BinanceWS  # noqa
    from .binance_rest import BinanceREST as _BinanceREST  # noqa
    from .storage import Storage as _Storage  # noqa


# ============================================================
# 异常层次
# ============================================================
class DataError(Exception):
    """data 包基类异常"""


class BootstrapError(DataError):
    """启动失败"""


class ShutdownError(DataError):
    """关闭失败"""


class ModuleNotAvailableError(DataError):
    """子模块不可用"""


# ============================================================
# 内部状态
# ============================================================
_lock = threading.RLock()

_module_status: Dict[str, Tuple[bool, str]] = {}
_module_cache: Dict[str, ModuleType] = {}
_singletons: Dict[str, Any] = {}

_bootstrap_done = False
_shutdown_done = False
_bootstrap_started_ms = 0
_shutdown_started_ms = 0

# 惰性映射：属性名 → 子模块名
_LAZY_MAP: Dict[str, str] = {
    "binance_ws": "binance_ws",
    "binance_rest": "binance_rest",
    "candle_buffer": "candle_buffer",
    "cleaner": "cleaner",
    "gap_filler": "gap_filler",
    "depth_cleaner": "depth_cleaner",
    "source_router": "source_router",
    "storage": "storage",
    "microstructure": "microstructure",
}

# 模块依赖顺序（供 bootstrap 参考）
_BOOTSTRAP_ORDER: List[str] = [
    "cleaner",
    "candle_buffer",
    "microstructure",
    "depth_cleaner",
    "binance_rest",
    "binance_ws",
    "gap_filler",
    "source_router",
    "storage",
]

_internal_logger = None


def _log() -> Any:
    """获取日志器（延迟，避免循环）"""
    global _internal_logger
    if _internal_logger is None:
        try:
            mod = _load_module("cleaner")
            _internal_logger = mod and None or None
        except Exception:
            pass
        try:
            from core.logger import get_logger
            _internal_logger = get_logger("data")
        except Exception:
            import logging
            _internal_logger = logging.getLogger("data")
    return _internal_logger


def _now_ms() -> int:
    try:
        from core.timestamp import now_ms
        return now_ms()
    except Exception:
        import time
        return int(time.time() * 1000)


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
    try:
        return _load_module(name)
    except Exception as exc:
        _log().warning(f"子模块 {name} 不可用: {exc}")
        return None


def __getattr__(name: str) -> Any:
    """
    PEP 562 模块级惰性属性。
    访问 data.storage 等时按需加载。
    """
    if name in _LAZY_MAP:
        return _load_module(_LAZY_MAP[name])
    raise AttributeError(
        f"module {__name__!r} has no attribute {name!r}")


def __dir__() -> List[str]:
    return sorted(set(
        list(globals().keys()) + list(_LAZY_MAP.keys())))


# ============================================================
# 单例注册表
# ============================================================
def _get_singleton(
    key: str, factory: Callable[[], Any],
) -> Any:
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


def get_cleaner() -> Any:
    """获取 cleaner 单例"""
    def _f() -> Any:
        mod = _load_module("cleaner")
        return mod.get_cleaner()
    return _get_singleton("cleaner", _f)


def get_candle_buffer() -> Any:
    def _f() -> Any:
        mod = _load_module("candle_buffer")
        return mod.get_candle_buffer()
    return _get_singleton("candle_buffer", _f)


def get_binance_ws() -> Any:
    def _f() -> Any:
        mod = _load_module("binance_ws")
        return mod.get_binance_ws()
    return _get_singleton("binance_ws", _f)


def get_binance_rest() -> Any:
    def _f() -> Any:
        mod = _load_module("binance_rest")
        return mod.get_binance_rest()
    return _get_singleton("binance_rest", _f)


def get_gap_filler() -> Any:
    def _f() -> Any:
        mod = _load_module("gap_filler")
        return mod.get_gap_filler()
    return _get_singleton("gap_filler", _f)


def get_depth_cleaner() -> Any:
    def _f() -> Any:
        mod = _load_module("depth_cleaner")
        return mod.get_depth_cleaner()
    return _get_singleton("depth_cleaner", _f)


def get_source_router() -> Any:
    def _f() -> Any:
        mod = _load_module("source_router")
        return mod.get_source_router()
    return _get_singleton("source_router", _f)


def get_storage() -> Any:
    def _f() -> Any:
        mod = _load_module("storage")
        return mod.get_storage()
    return _get_singleton("storage", _f)


def get_microstructure() -> Any:
    def _f() -> Any:
        mod = _load_module("microstructure")
        return mod.get_microstructure()
    return _get_singleton("microstructure", _f)


# ============================================================
# 启动阶段
# ============================================================
def _step_init_cleaner() -> None:
    mod = _try_load("cleaner")
    if mod is None:
        _log().warning("cleaner 模块不可用")
        return
    fn = getattr(mod, "init_cleaner", None)
    if fn is not None:
        fn()


def _step_init_candle_buffer() -> None:
    mod = _try_load("candle_buffer")
    if mod is None:
        _log().warning("candle_buffer 模块不可用")
        return
    fn = getattr(mod, "init_candle_buffer", None)
    if fn is not None:
        fn(auto_subscribe=True)


def _step_init_microstructure() -> None:
    mod = _try_load("microstructure")
    if mod is None:
        _log().warning("microstructure 模块不可用")
        return
    fn = getattr(mod, "init_microstructure", None)
    if fn is not None:
        fn()


def _step_init_depth_cleaner() -> None:
    mod = _try_load("depth_cleaner")
    if mod is None:
        _log().warning("depth_cleaner 模块不可用")
        return
    fn = getattr(mod, "init_depth_cleaner", None)
    if fn is not None:
        fn()


def _step_init_binance_rest() -> None:
    mod = _try_load("binance_rest")
    if mod is None:
        _log().warning("binance_rest 模块不可用")
        return
    fn = getattr(mod, "init_binance_rest", None)
    if fn is not None:
        fn()


def _step_init_binance_ws(autostart: bool) -> None:
    mod = _try_load("binance_ws")
    if mod is None:
        _log().warning("binance_ws 模块不可用")
        return
    fn = getattr(mod, "init_binance_ws", None)
    if fn is not None:
        fn(autostart=autostart)


def _step_init_gap_filler() -> None:
    mod = _try_load("gap_filler")
    if mod is None:
        _log().warning("gap_filler 模块不可用")
        return
    fn = getattr(mod, "init_gap_filler", None)
    if fn is not None:
        fn(auto_subscribe=True)


def _step_init_source_router() -> None:
    mod = _try_load("source_router")
    if mod is None:
        _log().warning("source_router 模块不可用")
        return
    fn = getattr(mod, "init_source_router", None)
    if fn is not None:
        fn()


def _step_init_storage() -> None:
    mod = _try_load("storage")
    if mod is None:
        _log().warning("storage 模块不可用")
        return
    fn = getattr(mod, "init_storage", None)
    if fn is not None:
        fn()


# ============================================================
# 统一入口
# ============================================================
def bootstrap(
    *,
    start_ws: bool = True,
    fail_fast: bool = False,
) -> Dict[str, Any]:
    """
    data 层统一启动入口。

    fail_fast=True 时任一阶段失败立即抛 BootstrapError；
    fail_fast=False 时记录警告并继续（生产推荐）。
    """
    global _bootstrap_done, _bootstrap_started_ms

    with _lock:
        if _bootstrap_done:
            _log().info("data.bootstrap 已执行，跳过")
            return data_stats()
        _bootstrap_started_ms = _now_ms()

    _log().info("=== data.bootstrap 开始 ===")

    steps: List[Tuple[str, Callable[[], Any]]] = [
        ("init_cleaner", _step_init_cleaner),
        ("init_candle_buffer", _step_init_candle_buffer),
        ("init_microstructure", _step_init_microstructure),
        ("init_depth_cleaner", _step_init_depth_cleaner),
        ("init_binance_rest", _step_init_binance_rest),
        ("init_binance_ws",
         lambda: _step_init_binance_ws(autostart=start_ws)),
        ("init_gap_filler", _step_init_gap_filler),
        ("init_source_router", _step_init_source_router),
        ("init_storage", _step_init_storage),
    ]

    results: Dict[str, Any] = {}
    for name, fn in steps:
        try:
            fn()
            results[name] = {"status": "ok"}
        except Exception as exc:
            tb = traceback.format_exc(limit=3)
            _log().error(f"data.bootstrap 阶段 {name} 失败: {exc}")
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
        f"=== data.bootstrap 完成 "
        f"({_now_ms() - _bootstrap_started_ms}ms) ===")

    results["ready"] = is_ready()
    return results


def shutdown(*, timeout_s: float = 10.0) -> Dict[str, Any]:
    """
    data 层统一关闭。反序关闭所有模块。
    """
    global _shutdown_done, _shutdown_started_ms

    with _lock:
        if _shutdown_done:
            return {"status": "already_done"}
        _shutdown_started_ms = _now_ms()

    _log().info("=== data.shutdown 开始 ===")

    results: Dict[str, Any] = {}

    # 关闭顺序（反序）：
    # storage → source_router → gap_filler → binance_ws
    #   → binance_rest → depth_cleaner → microstructure
    #   → candle_buffer → cleaner
    steps: List[Tuple[str, Callable[[], Any]]] = [
        ("storage", _shutdown_storage),
        ("source_router", _shutdown_router),
        ("gap_filler", _shutdown_gap_filler),
        ("binance_ws", _shutdown_ws),
        ("binance_rest", _shutdown_rest),
        ("depth_cleaner", _shutdown_depth),
        ("microstructure", _shutdown_micro),
        ("candle_buffer", _shutdown_buffer),
        ("cleaner", _shutdown_cleaner),
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
        f"=== data.shutdown 完成 "
        f"({_now_ms() - _shutdown_started_ms}ms) ===")
    return results


# ---- 各模块关闭 ----
def _shutdown_storage() -> Any:
    mod = _module_cache.get("storage")
    if mod is None:
        return None
    s = _singletons.get("storage")
    if s is None:
        return None
    try:
        import asyncio
        from ..core.compat import get_compat
        try:
            loop = asyncio.get_event_loop()
            if loop.is_running():
                asyncio.create_task(s.close())
            else:
                loop.run_until_complete(s.close())
        except RuntimeError:
            asyncio.run(s.close())
    except Exception:
        pass
    return None


def _shutdown_router() -> Any:
    mod = _module_cache.get("source_router")
    if mod is None:
        return None
    r = _singletons.get("source_router")
    if r is None:
        return None
    try:
        r.stop_health_loop()
    except Exception:
        pass
    return None


def _shutdown_gap_filler() -> Any:
    mod = _module_cache.get("gap_filler")
    if mod is None:
        return None
    f = _singletons.get("gap_filler")
    if f is None:
        return None
    try:
        f.cancel()
    except Exception:
        pass
    return None


def _shutdown_ws() -> Any:
    mod = _module_cache.get("binance_ws")
    if mod is None:
        return None
    fn = getattr(mod, "stop_binance_ws", None)
    if fn is not None:
        try:
            fn(timeout_s=5.0)
        except Exception:
            pass
    return None


def _shutdown_rest() -> Any:
    mod = _module_cache.get("binance_rest")
    if mod is None:
        return None
    fn = getattr(mod, "close_binance_rest", None)
    if fn is None:
        return None
    try:
        import asyncio
        try:
            loop = asyncio.get_event_loop()
            if loop.is_running():
                asyncio.create_task(fn())
            else:
                loop.run_until_complete(fn())
        except RuntimeError:
            asyncio.run(fn())
    except Exception:
        pass
    return None


def _shutdown_depth() -> Any:
    d = _singletons.get("depth_cleaner")
    if d is None:
        return None
    try:
        d.reset_stats()
    except Exception:
        pass
    return None


def _shutdown_micro() -> Any:
    m = _singletons.get("microstructure")
    if m is None:
        return None
    try:
        m.reset_all()
    except Exception:
        pass
    return None


def _shutdown_buffer() -> Any:
    b = _singletons.get("candle_buffer")
    if b is None:
        return None
    try:
        b.clear()
    except Exception:
        pass
    return None


def _shutdown_cleaner() -> Any:
    c = _singletons.get("cleaner")
    if c is None:
        return None
    try:
        c.clear_history()
    except Exception:
        pass
    return None


def is_ready() -> bool:
    """data 层是否已 bootstrap"""
    with _lock:
        return _bootstrap_done and not _shutdown_done


# ============================================================
# 状态
# ============================================================
def get_module_status() -> Dict[str, Dict[str, Any]]:
    with _lock:
        return {
            name: {
                "loaded": ok,
                "error": err,
                "cached": name in _module_cache,
            }
            for name, (ok, err) in _module_status.items()
        }


def data_stats() -> Dict[str, Any]:
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


def data_health() -> Dict[str, Any]:
    """
    聚合 data 层健康。
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
                modules[name] = {
                    "status": "unknown", "raw": str(result)}
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
        "stats": data_stats(),
        "ts_ms": _now_ms(),
    }


def __repr__() -> str:
    return (
        f"<data v{__version__} "
        f"bootstrap={_bootstrap_done} "
        f"shutdown={_shutdown_done} "
        f"modules={len(_module_cache)}>"
    )
