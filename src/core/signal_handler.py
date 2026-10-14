"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: signal_handler
version: 2.0.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: 优雅关闭（多阶段/异步感知/幂等/超时兜底）

provides:
  - class SignalHandler
  - class ShutdownCallback
  - class ShutdownPhase
  - class ReloadCallback
  - function get_signal_handler
  - function init_signal_handler
  - function on_shutdown
  - function on_reload

requires:
  modules: [core.logger, core.compat]
  libraries: []
  resources:
    memory_mb: 4
    cpu_cores: 1

health:
  fn: SignalHandler.health
  timeout_ms: 500

assembly:
  order: 19
  parallel: false

tests:
  - tests/test_signal_handler.py

fallback:
  - 信号不可用 → 只支持外部 trigger
  - asyncio 不可用 → 同步模式
  - 清理挂死 → 超时后强制退出

tags: [core, lifecycle, shutdown]
---UMS-END---"""

from __future__ import annotations

import os
import sys
import signal
import asyncio
import threading
import time
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Any, Callable, Dict, List, Optional

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("signal_handler")
        except Exception:
            import logging
            _logger = logging.getLogger("signal_handler")
    return _logger


def _now_ms() -> int:
    try:
        from core.timestamp import now_ms
        return now_ms()
    except Exception:
        return int(time.time() * 1000)


# ============================================================
# 平台
# ============================================================
IS_WINDOWS = sys.platform == "win32"
IS_POSIX = os.name == "posix"


def _supported_signals() -> List[int]:
    sigs: List[int] = []
    if hasattr(signal, "SIGINT"):
        sigs.append(int(signal.SIGINT))
    if hasattr(signal, "SIGTERM") and not IS_WINDOWS:
        sigs.append(int(signal.SIGTERM))
    if IS_WINDOWS and hasattr(signal, "SIGBREAK"):
        sigs.append(int(signal.SIGBREAK))
    return sigs


def _reload_signals() -> List[int]:
    """触发 reload 的信号（不退出）"""
    if IS_POSIX and hasattr(signal, "SIGHUP"):
        return [int(signal.SIGHUP)]
    return []


def _signal_name(signum: int) -> str:
    try:
        return signal.Signals(signum).name
    except Exception:
        return f"SIG{signum}"


# ============================================================
# 关闭阶段
# ============================================================
class ShutdownPhase(IntEnum):
    STOP_ACCEPT = 0    # 停止接收新工作
    DRAIN = 1          # 排空进行中的任务
    FLUSH = 2          # 刷新缓冲区/落盘
    CLEANUP = 3        # 释放资源、断开连接
    FORCE = 4          # 强制清理（可能丢数据）


# ============================================================
# 数据结构
# ============================================================
@dataclass
class ShutdownCallback:
    name: str
    phase: ShutdownPhase
    fn: Callable[[], Any]
    priority: int = 100           # 数字越小越早
    timeout_s: float = 5.0
    required: bool = False        # 失败是否影响整体
    executed: bool = False
    failed: bool = False
    error: str = ""
    duration_ms: int = 0


@dataclass
class ReloadCallback:
    name: str
    fn: Callable[[], Any]
    priority: int = 100
    timeout_s: float = 3.0


# ============================================================
# 全局默认超时
# ============================================================
DEFAULT_TOTAL_TIMEOUT_S = 30.0
FORCE_EXIT_CODE = 130   # 128 + SIGINT


# ============================================================
# SignalHandler 单例
# ============================================================
class SignalHandler:
    """
    优雅关闭管理器。

    特性：
    - 惰性安装，import 无副作用
    - asyncio 感知（自动使用 loop.add_signal_handler）
    - 多阶段、多回调、有序执行
    - 幂等（第二次信号强制退出）
    - 全阶段超时兜底
    """

    _instance: Optional["SignalHandler"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "SignalHandler":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self,
        total_timeout_s: float = DEFAULT_TOTAL_TIMEOUT_S,
        force_exit_code: int = FORCE_EXIT_CODE,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._callbacks: List[ShutdownCallback] = []
            self._reload_callbacks: List[ReloadCallback] = []
            self._installed = False
            self._installed_signals: List[int] = []
            self._original_handlers: Dict[int, Any] = {}
            self._shutdown_started = False
            self._shutdown_done = threading.Event()
            self._reload_in_progress = False
            self._loop: Optional[asyncio.AbstractEventLoop] = None
            self._total_timeout_s = total_timeout_s
            self._force_exit_code = force_exit_code
            self._stats = {
                "triggered": 0,
                "signal_num": 0,
                "second_signal": 0,
                "callbacks_run": 0,
                "callbacks_failed": 0,
                "started_ms": 0,
                "completed_ms": 0,
                "reloads": 0,
            }
            self._initialized = True

    # ========================================================
    # 安装 / 卸载
    # ========================================================
    def install(
        self,
        loop: Optional[asyncio.AbstractEventLoop] = None,
    ) -> bool:
        """安装信号处理器。若在 asyncio 上下文中，建议传入 loop。"""
        with self._lock:
            if self._installed:
                return True
            self._loop = loop
            installed_any = False

            # asyncio 优先
            if loop is not None and loop.is_running():
                for sig in _supported_signals() + _reload_signals():
                    try:
                        loop.add_signal_handler(
                            sig,
                            self._make_loop_handler(sig),
                        )
                        self._installed_signals.append(sig)
                        installed_any = True
                    except (NotImplementedError, RuntimeError,
                            ValueError):
                        # 回退到 signal.signal
                        if self._install_sync(sig):
                            installed_any = True
            else:
                for sig in _supported_signals() + _reload_signals():
                    if self._install_sync(sig):
                        installed_any = True

            self._installed = installed_any
            if installed_any:
                _log().info(
                    f"信号处理器已安装: "
                    f"{[_signal_name(s) for s in self._installed_signals]}")
            else:
                _log().warning("未能安装任何信号处理器")
            return installed_any

    def _install_sync(self, sig: int) -> bool:
        try:
            old = signal.signal(sig, self._sync_dispatch)
            self._original_handlers[sig] = old
            if sig not in self._installed_signals:
                self._installed_signals.append(sig)
            return True
        except (ValueError, OSError) as exc:
            _log().debug(f"信号 {sig} 安装失败: {exc}")
            return False

    def uninstall(self) -> None:
        """还原原始信号处理器（测试用）"""
        with self._lock:
            if not self._installed:
                return
            if self._loop is not None and self._loop.is_running():
                for sig in self._installed_signals:
                    try:
                        self._loop.remove_signal_handler(sig)
                    except Exception:
                        pass
            for sig, old in self._original_handlers.items():
                try:
                    signal.signal(sig, old)
                except Exception:
                    pass
            self._installed_signals.clear()
            self._original_handlers.clear()
            self._installed = False
            _log().info("信号处理器已卸载")

    # ========================================================
    # 信号分发
    # ========================================================
    def _make_loop_handler(self, sig: int) -> Callable[[], None]:
        def _handler() -> None:
            self._dispatch(sig)
        return _handler

    def _sync_dispatch(self, signum: int, frame: Any) -> None:
        # 信号处理器内不做重活，仅分发
        try:
            self._dispatch(signum)
        except Exception as exc:
            # 防止异常在信号上下文中丢失
            try:
                _log().error(f"信号分发异常: {exc}")
            except Exception:
                pass

    def _dispatch(self, signum: int) -> None:
        # reload 信号
        if signum in _reload_signals():
            self._trigger_reload(signum)
            return

        with self._lock:
            if self._shutdown_started:
                # 第二次信号：强制退出
                self._stats["second_signal"] += 1
                _log().warning(
                    f"收到第二次信号 {_signal_name(signum)}，"
                    f"强制退出")
                self._force_exit()
                return
            self._shutdown_started = True
            self._stats["triggered"] += 1
            self._stats["signal_num"] = signum
            self._stats["started_ms"] = _now_ms()

        _log().info(
            f"收到信号 {_signal_name(signum)}，开始优雅关闭")

        # 异步环境：调度任务
        if self._loop is not None and self._loop.is_running():
            try:
                self._loop.call_soon_threadsafe(
                    lambda: asyncio.ensure_future(
                        self._run_shutdown_async(), loop=self._loop
                    )
                )
                return
            except Exception as exc:
                _log().warning(f"异步调度失败: {exc}，改用线程")

        # 同步环境：启动线程执行关闭
        threading.Thread(
            target=self._run_shutdown_sync,
            name="shutdown-runner",
            daemon=True,
        ).start()

    # ========================================================
    # 关闭执行
    # ========================================================
    async def _run_shutdown_async(self) -> None:
        try:
            await asyncio.wait_for(
                self._run_shutdown_impl_async(),
                timeout=self._total_timeout_s,
            )
        except asyncio.TimeoutError:
            _log().error(
                f"优雅关闭超时 ({self._total_timeout_s}s)，强制退出")
            self._force_exit()
        except Exception as exc:
            _log().error(f"异步关闭异常: {exc}")
            self._force_exit()
        finally:
            self._mark_done()

    def _run_shutdown_sync(self) -> None:
        deadline = time.time() + self._total_timeout_s
        try:
            for phase in ShutdownPhase:
                if time.time() >= deadline:
                    _log().error("总超时，跳过剩余阶段")
                    break
                self._run_phase_sync(phase, deadline)
        except Exception as exc:
            _log().error(f"同步关闭异常: {exc}")
        finally:
            self._mark_done()

    async def _run_shutdown_impl_async(self) -> None:
        for phase in ShutdownPhase:
            await self._run_phase_async(phase)

    def _sorted_callbacks(
        self, phase: ShutdownPhase,
    ) -> List[ShutdownCallback]:
        with self._lock:
            cbs = [c for c in self._callbacks if c.phase == phase]
        cbs.sort(key=lambda c: (c.priority, c.name))
        return cbs

    def _run_phase_sync(
        self, phase: ShutdownPhase, deadline: float,
    ) -> None:
        cbs = self._sorted_callbacks(phase)
        if not cbs:
            return
        _log().info(
            f"[关闭阶段 {phase.name}] 执行 {len(cbs)} 个回调")
        for cb in cbs:
            if time.time() >= deadline:
                _log().warning(
                    f"总超时，跳过 {cb.name}")
                continue
            self._invoke_sync(cb)

    async def _run_phase_async(self, phase: ShutdownPhase) -> None:
        cbs = self._sorted_callbacks(phase)
        if not cbs:
            return
        _log().info(
            f"[关闭阶段 {phase.name}] 执行 {len(cbs)} 个回调")
        for cb in cbs:
            await self._invoke_async(cb)

    def _invoke_sync(self, cb: ShutdownCallback) -> None:
        start = time.perf_counter()
        try:
            result = cb.fn()
            if asyncio.iscoroutine(result):
                # 同步上下文中运行协程
                try:
                    asyncio.run(
                        asyncio.wait_for(result, timeout=cb.timeout_s))
                except RuntimeError:
                    # 已有 loop，新建线程运行
                    self._run_coro_in_new_loop(result, cb.timeout_s)
            cb.executed = True
        except Exception as exc:
            cb.failed = True
            cb.error = f"{type(exc).__name__}: {exc}"
            with self._lock:
                self._stats["callbacks_failed"] += 1
            _log().error(
                f"关闭回调 {cb.name} 失败: {exc}")
        finally:
            cb.duration_ms = int(
                (time.perf_counter() - start) * 1000)
            with self._lock:
                self._stats["callbacks_run"] += 1

    async def _invoke_async(self, cb: ShutdownCallback) -> None:
        start = time.perf_counter()
        try:
            result = cb.fn()
            if asyncio.iscoroutine(result):
                await asyncio.wait_for(result, timeout=cb.timeout_s)
            cb.executed = True
        except asyncio.TimeoutError:
            cb.failed = True
            cb.error = f"超时 ({cb.timeout_s}s)"
            with self._lock:
                self._stats["callbacks_failed"] += 1
            _log().error(f"关闭回调 {cb.name} 超时")
        except Exception as exc:
            cb.failed = True
            cb.error = f"{type(exc).__name__}: {exc}"
            with self._lock:
                self._stats["callbacks_failed"] += 1
            _log().error(
                f"关闭回调 {cb.name} 失败: {exc}")
        finally:
            cb.duration_ms = int(
                (time.perf_counter() - start) * 1000)
            with self._lock:
                self._stats["callbacks_run"] += 1

    def _run_coro_in_new_loop(
        self, coro: Any, timeout_s: float,
    ) -> None:
        def _run() -> None:
            try:
                asyncio.run(
                    asyncio.wait_for(coro, timeout=timeout_s))
            except Exception as exc:
                _log().warning(f"协程执行异常: {exc}")

        t = threading.Thread(target=_run, daemon=True)
        t.start()
        t.join(timeout=timeout_s + 0.5)

    def _mark_done(self) -> None:
        with self._lock:
            self._stats["completed_ms"] = _now_ms()
        self._shutdown_done.set()
        _log().info(
            f"优雅关闭完成 "
            f"(callbacks={self._stats['callbacks_run']} "
            f"failed={self._stats['callbacks_failed']})")

    def _force_exit(self) -> None:
        """强制退出，绕过 Python 清理"""
        try:
            # 尝试最后一次 flush
            for h in list(getattr(sys, "stdout", None), ) or []:
                pass
        except Exception:
            pass
        try:
            sys.stdout.flush()
            sys.stderr.flush()
        except Exception:
            pass
        os._exit(self._force_exit_code)

    # ========================================================
    # Reload
    # ========================================================
    def _trigger_reload(self, signum: int) -> None:
        with self._lock:
            if self._reload_in_progress:
                _log().warning("reload 已在进行中，忽略重复信号")
                return
            self._reload_in_progress = True
        _log().info(f"收到 {_signal_name(signum)}，开始 reload")
        try:
            cbs = sorted(
                self._reload_callbacks,
                key=lambda c: (c.priority, c.name),
            )
            for cb in cbs:
                try:
                    result = cb.fn()
                    if asyncio.iscoroutine(result):
                        try:
                            asyncio.run(asyncio.wait_for(
                                result, timeout=cb.timeout_s))
                        except RuntimeError:
                            self._run_coro_in_new_loop(
                                result, cb.timeout_s)
                except Exception as exc:
                    _log().error(
                        f"reload 回调 {cb.name} 失败: {exc}")
            with self._lock:
                self._stats["reloads"] += 1
            _log().info("reload 完成")
        finally:
            with self._lock:
                self._reload_in_progress = False

    # ========================================================
    # 注册
    # ========================================================
    def register(
        self,
        phase: ShutdownPhase,
        name: str,
        fn: Callable[[], Any],
        priority: int = 100,
        timeout_s: float = 5.0,
        required: bool = False,
    ) -> ShutdownCallback:
        if not callable(fn):
            raise ValueError("fn 必须可调用")
        cb = ShutdownCallback(
            name=name,
            phase=phase,
            fn=fn,
            priority=priority,
            timeout_s=timeout_s,
            required=required,
        )
        with self._lock:
            # 同名覆盖
            self._callbacks = [
                c for c in self._callbacks if c.name != name]
            self._callbacks.append(cb)
        _log().debug(
            f"注册关闭回调: {name} (phase={phase.name}, "
            f"priority={priority})")
        return cb

    def unregister(self, name: str) -> bool:
        with self._lock:
            before = len(self._callbacks)
            self._callbacks = [
                c for c in self._callbacks if c.name != name]
            return len(self._callbacks) < before

    def register_reload(
        self,
        name: str,
        fn: Callable[[], Any],
        priority: int = 100,
        timeout_s: float = 3.0,
    ) -> ReloadCallback:
        if not callable(fn):
            raise ValueError("fn 必须可调用")
        cb = ReloadCallback(
            name=name, fn=fn, priority=priority, timeout_s=timeout_s)
        with self._lock:
            self._reload_callbacks = [
                c for c in self._reload_callbacks if c.name != name]
            self._reload_callbacks.append(cb)
        return cb

    # ========================================================
    # 外部触发
    # ========================================================
    def trigger(self, signal_num: int = int(signal.SIGINT)) -> None:
        """外部触发关闭（测试/程序化调用）"""
        self._dispatch(signal_num)

    def wait(self, timeout_s: Optional[float] = None) -> bool:
        """阻塞等待关闭完成"""
        return self._shutdown_done.wait(timeout=timeout_s)

    def is_shutting_down(self) -> bool:
        with self._lock:
            return self._shutdown_started

    # ========================================================
    # 状态
    # ========================================================
    def stats(self) -> Dict[str, Any]:
        with self._lock:
            return {
                **self._stats,
                "installed": self._installed,
                "signals": [
                    _signal_name(s) for s in self._installed_signals],
                "callbacks": len(self._callbacks),
                "shutdown_started": self._shutdown_started,
                "shutdown_done": self._shutdown_done.is_set(),
            }

    def health(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "status": "ok",
                "installed": self._installed,
                "shutdown_started": self._shutdown_started,
                "shutdown_done": self._shutdown_done.is_set(),
                "callbacks": len(self._callbacks),
                "reload_callbacks": len(self._reload_callbacks),
                "signals": [
                    _signal_name(s) for s in self._installed_signals],
                "platform": sys.platform,
            }

    def callbacks_report(self) -> List[Dict[str, Any]]:
        with self._lock:
            cbs = list(self._callbacks)
        return [
            {
                "name": c.name,
                "phase": c.phase.name,
                "priority": c.priority,
                "executed": c.executed,
                "failed": c.failed,
                "duration_ms": c.duration_ms,
                "error": c.error,
            }
            for c in cbs
        ]

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<SignalHandler installed={self._installed} "
                f"shutting_down={self._shutdown_started} "
                f"callbacks={len(self._callbacks)}>"
            )


# ============================================================
# 全局入口
# ============================================================
_handler: Optional[SignalHandler] = None
_handler_lock = threading.Lock()


def init_signal_handler(
    total_timeout_s: float = DEFAULT_TOTAL_TIMEOUT_S,
    install: bool = True,
    loop: Optional[asyncio.AbstractEventLoop] = None,
) -> SignalHandler:
    """显式初始化（启动时调用）"""
    global _handler
    with _handler_lock:
        _handler = SignalHandler(total_timeout_s=total_timeout_s)
        if install:
            _handler.install(loop=loop)
    return _handler


def get_signal_handler() -> SignalHandler:
    """获取单例（惰性初始化，不自动 install）"""
    global _handler
    if _handler is None:
        with _handler_lock:
            if _handler is None:
                _handler = SignalHandler()
    return _handler


# ============================================================
# 便捷函数
# ============================================================
def on_shutdown(
    phase: ShutdownPhase = ShutdownPhase.CLEANUP,
    name: Optional[str] = None,
    priority: int = 100,
    timeout_s: float = 5.0,
    required: bool = False,
) -> Callable[[Callable[[], Any]], Callable[[], Any]]:
    """装饰器：注册关闭回调"""
    def decorator(fn: Callable[[], Any]) -> Callable[[], Any]:
        cb_name = name or getattr(fn, "__name__", "unnamed")
        get_signal_handler().register(
            phase=phase,
            name=cb_name,
            fn=fn,
            priority=priority,
            timeout_s=timeout_s,
            required=required,
        )
        return fn
    return decorator


def on_reload(
    name: Optional[str] = None,
    priority: int = 100,
    timeout_s: float = 3.0,
) -> Callable[[Callable[[], Any]], Callable[[], Any]]:
    """装饰器：注册 reload 回调"""
    def decorator(fn: Callable[[], Any]) -> Callable[[], Any]:
        cb_name = name or getattr(fn, "__name__", "unnamed")
        get_signal_handler().register_reload(
            name=cb_name,
            fn=fn,
            priority=priority,
            timeout_s=timeout_s,
        )
        return fn
    return decorator


def trigger_shutdown() -> None:
    get_signal_handler().trigger()


def wait_shutdown(timeout_s: Optional[float] = None) -> bool:
    return get_signal_handler().wait(timeout_s)
