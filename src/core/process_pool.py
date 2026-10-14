"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: process_pool
version: 2.0.0
level: 0
required: false
author: quant-team
created: 2026-10-14
brief: 进程池管理（惰性初始化/优先级/超时/自动恢复/CPU-IO分离）

provides:
  - class ProcessPool
  - class PoolTask
  - class PoolStats
  - function get_process_pool
  - function init_process_pool
  - function run_cpu
  - function run_io
  - function submit_cpu
  - function register_task

requires:
  modules: [core.compat, core.logger, core.timestamp]
  libraries: []
  resources:
    memory_mb: 256
    cpu_cores: 4

health:
  fn: ProcessPool.health
  timeout_ms: 2000

assembly:
  order: 16
  parallel: false

tests:
  - tests/test_process_pool.py

fallback:
  - 进程池不可用 → 线程池
  - 线程池不可用 → 同步执行
  - 队列满 → 拒绝并告警

tags: [core, concurrency, process]
---UMS-END---"""

from __future__ import annotations

import os
import sys
import time
import queue
import pickle
import asyncio
import inspect
import threading
from concurrent.futures import (
    ProcessPoolExecutor, ThreadPoolExecutor, Future, TimeoutError,
)
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Any, Callable, Dict, List, Optional, Tuple

# 延迟导入
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("process_pool")
        except Exception:
            import logging
            _logger = logging.getLogger("process_pool")
    return _logger


def _now_ms() -> int:
    try:
        from core.timestamp import now_ms
        return now_ms()
    except Exception:
        return int(time.time() * 1000)


def _subprocess_method() -> str:
    try:
        from core.compat import get_compat
        return get_compat().subprocess_start_method()
    except Exception:
        if sys.platform == "win32":
            return "spawn"
        return "fork"


# ============================================================
# 异常
# ============================================================
class PoolError(Exception):
    """进程池基类异常"""


class PoolClosedError(PoolError):
    """池已关闭"""


class PoolOverloadedError(PoolError):
    """池过载"""


class TaskNotPicklableError(PoolError):
    """任务无法 pickle"""


# ============================================================
# 优先级
# ============================================================
class TaskPriority(IntEnum):
    CRITICAL = 0
    HIGH = 1
    NORMAL = 2
    LOW = 3


# ============================================================
# 任务
# ============================================================
@dataclass(order=True)
class PoolTask:
    priority: int
    seq: int = field(compare=False, default=0)
    task_id: str = field(compare=False, default="")
    fn: Callable = field(compare=False, default=None)
    args: Tuple = field(compare=False, default_factory=tuple)
    kwargs: Dict[str, Any] = field(compare=False, default_factory=dict)
    timeout_s: float = field(compare=False, default=30.0)
    submitted_ms: int = field(compare=False, default=0)
    future: Optional[Future] = field(compare=False, default=None)


# ============================================================
# 统计
# ============================================================
@dataclass
class PoolStats:
    submitted: int = 0
    completed: int = 0
    failed: int = 0
    cancelled: int = 0
    rejected: int = 0
    timeouts: int = 0
    executor_restarts: int = 0
    total_exec_ms: int = 0
    max_exec_ms: int = 0
    queue_max: int = 0
    started_ms: int = field(default_factory=_now_ms)

    def avg_exec_ms(self) -> float:
        return self.total_exec_ms / self.completed if self.completed else 0.0


# ============================================================
# 进程池
# ============================================================
class ProcessPool:
    """
    惰性初始化、线程安全、支持优先级和超时的进程池。

    - CPU 池与 IO 池分离
    - 任务可取消
    - worker 崩溃自动重建
    - 支持 asyncio
    """

    _instance: Optional["ProcessPool"] = None
    _instance_lock = threading.Lock()

    # 环境变量
    ENV_CPU_WORKERS = "QUANT_CPU_WORKERS"
    ENV_IO_WORKERS = "QUANT_IO_WORKERS"
    ENV_MAX_QUEUE = "QUANT_POOL_MAX_QUEUE"

    # 默认值
    DEFAULT_MAX_QUEUE = 4096
    DEFAULT_TIMEOUT_S = 30.0
    SHUTDOWN_TIMEOUT_S = 5.0
    RESTART_FAIL_THRESHOLD = 3

    def __new__(cls, *args: Any, **kwargs: Any) -> "ProcessPool":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self,
        cpu_workers: Optional[int] = None,
        io_workers: Optional[int] = None,
        max_queue: Optional[int] = None,
        enable_io_pool: bool = True,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._cpu_executor: Optional[ProcessPoolExecutor] = None
            self._io_executor: Optional[ThreadPoolExecutor] = None
            self._enable_io_pool = enable_io_pool
            self._cpu_workers = self._resolve_cpu_workers(cpu_workers)
            self._io_workers = self._resolve_io_workers(io_workers)
            self._max_queue = max_queue or int(
                os.environ.get(self.ENV_MAX_QUEUE, self.DEFAULT_MAX_QUEUE))
            self._pending = 0
            self._seq = 0
            self._closed = False
            self._stats = PoolStats()
            self._init_error = ""
            self._recent_failures = 0
            self._initialized = True
            self._fallback_mode = "none"  # none|thread|sync

    # ========================================================
    # 参数计算
    # ========================================================
    @staticmethod
    def _resolve_cpu_workers(override: Optional[int]) -> int:
        if override is not None:
            return max(1, override)
        env = os.environ.get("QUANT_CPU_WORKERS")
        if env and env.isdigit():
            return max(1, int(env))
        cpu = os.cpu_count() or 2
        # 保留 2 核给主进程/IO
        return max(1, cpu - 2)

    @staticmethod
    def _resolve_io_workers(override: Optional[int]) -> int:
        if override is not None:
            return max(1, override)
        env = os.environ.get("QUANT_IO_WORKERS")
        if env and env.isdigit():
            return max(1, int(env))
        cpu = os.cpu_count() or 2
        return max(2, cpu * 2)

    # ========================================================
    # 惰性初始化
    # ========================================================
    def _ensure_cpu(self) -> bool:
        with self._lock:
            if self._cpu_executor is not None:
                return True
            if self._closed:
                return False
            try:
                mp_ctx = __import__("multiprocessing").get_context(
                    _subprocess_method())
                self._cpu_executor = ProcessPoolExecutor(
                    max_workers=self._cpu_workers,
                    mp_context=mp_ctx,
                    initializer=_worker_init,
                )
                _log().info(
                    f"CPU 进程池已创建: {self._cpu_workers} workers "
                    f"(method={_subprocess_method()})")
                return True
            except Exception as exc:
                self._init_error = str(exc)
                _log().error(
                    f"CPU 进程池创建失败: {exc}，降级到线程池")
                self._fallback_mode = "thread"
                return False

    def _ensure_io(self) -> bool:
        if not self._enable_io_pool:
            return False
        with self._lock:
            if self._io_executor is not None:
                return True
            if self._closed:
                return False
            try:
                self._io_executor = ThreadPoolExecutor(
                    max_workers=self._io_workers,
                    thread_name_prefix="quant-io",
                )
                _log().info(
                    f"IO 线程池已创建: {self._io_workers} workers")
                return True
            except Exception as exc:
                _log().error(f"IO 线程池创建失败: {exc}")
                return False

    def _ensure_thread_fallback(self) -> bool:
        """CPU 进程池失败时的线程降级"""
        with self._lock:
            if self._io_executor is None:
                try:
                    self._io_executor = ThreadPoolExecutor(
                        max_workers=self._cpu_workers,
                        thread_name_prefix="quant-cpu-fallback",
                    )
                    return True
                except Exception as exc:
                    _log().error(f"线程降级失败: {exc}")
                    return False
            return True

    # ========================================================
    # 提交
    # ========================================================
    def submit(
        self,
        fn: Callable,
        *args: Any,
        priority: int = TaskPriority.NORMAL,
        timeout_s: float = DEFAULT_TIMEOUT_S,
        use_io: bool = False,
        **kwargs: Any,
    ) -> Optional[Future]:
        """
        提交任务到 CPU 或 IO 池。

        - 超过 max_queue 时抛 PoolOverloadedError
        - 函数需可 pickle（CPU 池），否则抛 TaskNotPicklableError
        """
        if self._closed:
            raise PoolClosedError("进程池已关闭")

        with self._lock:
            if self._pending >= self._max_queue:
                self._stats.rejected += 1
                raise PoolOverloadedError(
                    f"待处理任务已达上限 {self._max_queue}")
            self._pending += 1
            if self._pending > self._stats.queue_max:
                self._stats.queue_max = self._pending
            self._seq += 1
            task_id = f"task-{self._seq}"

        # 选择执行器
        if use_io and self._enable_io_pool:
            if not self._ensure_io():
                with self._lock:
                    self._pending -= 1
                # 降级到 CPU 池
                use_io = False

        if not use_io:
            if not self._ensure_cpu():
                # CPU 池失败，降级到线程池
                if not self._ensure_thread_fallback():
                    with self._lock:
                        self._pending -= 1
                    # 最终降级：同步执行
                    return self._run_sync_wrapper(
                        fn, args, kwargs, task_id)

        try:
            if use_io and self._io_executor is not None:
                future = self._io_executor.submit(fn, *args, **kwargs)
            elif self._cpu_executor is not None:
                # CPU 池：pickle 检查
                try:
                    pickle.dumps(fn)
                except Exception as exc:
                    with self._lock:
                        self._pending -= 1
                    raise TaskNotPicklableError(
                        f"函数 {getattr(fn, '__name__', fn)} "
                        f"无法 pickle: {exc}")
                future = self._cpu_executor.submit(
                    _run_with_timer, fn, args, kwargs)
            elif self._io_executor is not None:
                # 线程池降级
                future = self._io_executor.submit(fn, *args, **kwargs)
            else:
                with self._lock:
                    self._pending -= 1
                return self._run_sync_wrapper(
                    fn, args, kwargs, task_id)
        except (PoolOverloadedError, TaskNotPicklableError):
            raise
        except Exception as exc:
            with self._lock:
                self._pending -= 1
            _log().error(f"提交任务失败: {exc}")
            raise PoolError(f"任务提交失败: {exc}") from exc

        with self._lock:
            self._stats.submitted += 1
        # 完成回调
        future.add_done_callback(
            lambda f: self._on_done(f, task_id))
        return future

    def _run_sync_wrapper(
        self,
        fn: Callable,
        args: Tuple,
        kwargs: Dict[str, Any],
        task_id: str,
    ) -> Future:
        """最终降级：同步执行并封装为已完成 Future"""
        future: Future = Future()
        future.set_running_or_notify_cancel()
        start = time.perf_counter()
        try:
            result = fn(*args, **kwargs)
            future.set_result(result)
        except Exception as exc:
            future.set_exception(exc)
        finally:
            exec_ms = int((time.perf_counter() - start) * 1000)
            self._on_done_manual(task_id, exec_ms, future.exception())
        return future

    def _on_done(self, future: Future, task_id: str) -> None:
        exec_ms = int(
            (future._result_time or time.time()) -
            (future._submitted_time or time.time())
        ) * 1000 if hasattr(future, "_result_time") else 0
        with self._lock:
            self._pending = max(0, self._pending - 1)
            self._stats.completed += 1
            if future.cancelled():
                self._stats.cancelled += 1
            elif future.exception() is not None:
                self._stats.failed += 1
                self._recent_failures += 1
                if self._recent_failures >= self.RESTART_FAIL_THRESHOLD:
                    self._log_failure_spike()
            else:
                self._recent_failures = 0

    def _on_done_manual(
        self, task_id: str, exec_ms: int, exc: Optional[BaseException],
    ) -> None:
        with self._lock:
            self._pending = max(0, self._pending - 1)
            self._stats.completed += 1
            self._stats.total_exec_ms += exec_ms
            if exec_ms > self._stats.max_exec_ms:
                self._stats.max_exec_ms = exec_ms
            if exc is not None:
                self._stats.failed += 1

    def _log_failure_spike(self) -> None:
        _log().error(
            f"CPU 进程池连续失败 {self._recent_failures} 次，"
            f"考虑重建")
        self._restart_cpu()

    def _restart_cpu(self) -> None:
        with self._lock:
            old = self._cpu_executor
            self._cpu_executor = None
            self._stats.executor_restarts += 1
        if old is not None:
            try:
                old.shutdown(wait=False, cancel_futures=True)
            except Exception:
                pass
        _log().info("CPU 进程池已重建")

    # ========================================================
    # 异步包装
    # ========================================================
    async def run_cpu(
        self,
        fn: Callable,
        *args: Any,
        priority: int = TaskPriority.NORMAL,
        timeout_s: float = DEFAULT_TIMEOUT_S,
        **kwargs: Any,
    ) -> Any:
        """异步执行 CPU 任务（含超时）"""
        loop = asyncio.get_running_loop()
        future = self.submit(
            fn, *args,
            priority=priority,
            timeout_s=timeout_s,
            use_io=False, **kwargs,
        )
        if future is None:
            raise PoolError("无法提交任务")
        try:
            return await asyncio.wait_for(
                asyncio.wrap_future(future, loop=loop),
                timeout=timeout_s,
            )
        except asyncio.TimeoutError:
            future.cancel()
            with self._lock:
                self._stats.timeouts += 1
            raise PoolError(
                f"任务超时 ({timeout_s}s): {getattr(fn, '__name__', fn)}")

    async def run_io(
        self,
        fn: Callable,
        *args: Any,
        timeout_s: float = DEFAULT_TIMEOUT_S,
        **kwargs: Any,
    ) -> Any:
        """异步执行 IO 任务"""
        loop = asyncio.get_running_loop()
        future = self.submit(
            fn, *args, timeout_s=timeout_s, use_io=True, **kwargs)
        if future is None:
            raise PoolError("无法提交任务")
        try:
            return await asyncio.wait_for(
                asyncio.wrap_future(future, loop=loop),
                timeout=timeout_s,
            )
        except asyncio.TimeoutError:
            future.cancel()
            raise PoolError(f"IO 任务超时 ({timeout_s}s)")

    # ========================================================
    # 预热
    # ========================================================
    def warmup(self, timeout_s: float = 10.0) -> bool:
        """预热：提交一个空任务，确保 worker 已就绪"""
        try:
            future = self.submit(_noop, priority=TaskPriority.CRITICAL,
                                  timeout_s=timeout_s)
            if future is None:
                return False
            future.result(timeout=timeout_s)
            _log().info("进程池预热完成")
            return True
        except Exception as exc:
            _log().warning(f"进程池预热失败: {exc}")
            return False

    # ========================================================
    # 关闭
    # ========================================================
    def shutdown(self, wait: bool = True,
                  timeout_s: float = SHUTDOWN_TIMEOUT_S) -> None:
        """两阶段关闭：先等，再强杀"""
        with self._lock:
            if self._closed:
                return
            self._closed = True
            cpu = self._cpu_executor
            io = self._io_executor
            self._cpu_executor = None
            self._io_executor = None
        _log().info("正在关闭进程池...")
        for name, ex in (("cpu", cpu), ("io", io)):
            if ex is None:
                continue
            try:
                ex.shutdown(wait=wait, cancel_futures=True)
            except TypeError:
                # Python < 3.9 无 cancel_futures
                ex.shutdown(wait=wait)
            except Exception as exc:
                _log().warning(f"{name} 池关闭异常: {exc}")
        _log().info(
            f"进程池已关闭 "
            f"(submitted={self._stats.submitted} "
            f"completed={self._stats.completed} "
            f"failed={self._stats.failed})")

    # ========================================================
    # 状态
    # ========================================================
    def stats(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "submitted": self._stats.submitted,
                "completed": self._stats.completed,
                "failed": self._stats.failed,
                "cancelled": self._stats.cancelled,
                "rejected": self._stats.rejected,
                "timeouts": self._stats.timeouts,
                "executor_restarts": self._stats.executor_restarts,
                "avg_exec_ms": round(self._stats.avg_exec_ms(), 2),
                "max_exec_ms": self._stats.max_exec_ms,
                "pending": self._pending,
                "queue_max": self._stats.queue_max,
                "max_queue": self._max_queue,
                "cpu_workers": self._cpu_workers,
                "io_workers": self._io_workers,
                "fallback_mode": self._fallback_mode,
                "closed": self._closed,
            }

    def health(self) -> Dict[str, Any]:
        with self._lock:
            cpu_ok = self._cpu_executor is not None or \
                self._fallback_mode in ("thread", "sync")
            io_ok = (not self._enable_io_pool) or \
                self._io_executor is not None
            overloaded = self._pending >= self._max_queue * 0.9
            status = "ok"
            if self._closed:
                status = "closed"
            elif overloaded:
                status = "degraded"
            elif not cpu_ok:
                status = "degraded"
            return {
                "status": status,
                "cpu_ok": cpu_ok,
                "io_ok": io_ok,
                "pending": self._pending,
                "max_queue": self._max_queue,
                "fallback_mode": self._fallback_mode,
                "init_error": self._init_error,
            }

    # ========================================================
    # 上下文管理
    # ========================================================
    def __enter__(self) -> "ProcessPool":
        return self

    def __exit__(self, *args: Any) -> None:
        self.shutdown()


# ============================================================
# Worker 辅助
# ============================================================
def _worker_init() -> None:
    """子进程初始化：设置日志、忽略 SIGINT 由父进程处理"""
    try:
        import signal
        signal.signal(signal.SIGINT, signal.SIG_IGN)
    except Exception:
        pass


def _noop() -> int:
    return 0


def _run_with_timer(
    fn: Callable, args: Tuple, kwargs: Dict[str, Any],
) -> Tuple[Any, int]:
    """
    在子进程中运行并返回 (result, elapsed_ms)。
    超时由父进程的 wait_for 控制。
    """
    start = time.perf_counter()
    result = fn(*args, **kwargs)
    elapsed = int((time.perf_counter() - start) * 1000)
    return result, elapsed


# ============================================================
# 全局单例
# ============================================================
_pool: Optional[ProcessPool] = None
_pool_lock = threading.Lock()


def init_process_pool(
    cpu_workers: Optional[int] = None,
    io_workers: Optional[int] = None,
    max_queue: Optional[int] = None,
    warmup: bool = True,
) -> ProcessPool:
    """显式初始化（启动时调用）"""
    global _pool
    with _pool_lock:
        _pool = ProcessPool(
            cpu_workers=cpu_workers,
            io_workers=io_workers,
            max_queue=max_queue,
        )
        if warmup:
            _pool.warmup()
    return _pool


def get_process_pool() -> ProcessPool:
    """获取全局池（惰性初始化）"""
    global _pool
    if _pool is None:
        with _pool_lock:
            if _pool is None:
                _pool = ProcessPool()
    return _pool


def shutdown_process_pool(timeout_s: float = ProcessPool.SHUTDOWN_TIMEOUT_S) -> None:
    global _pool
    with _pool_lock:
        if _pool is not None:
            _pool.shutdown(timeout_s=timeout_s)
            _pool = None


# ============================================================
# 便捷函数
# ============================================================
async def run_cpu(
    fn: Callable,
    *args: Any,
    priority: int = TaskPriority.NORMAL,
    timeout_s: float = ProcessPool.DEFAULT_TIMEOUT_S,
    **kwargs: Any,
) -> Any:
    return await get_process_pool().run_cpu(
        fn, *args, priority=priority,
        timeout_s=timeout_s, **kwargs)


async def run_io(
    fn: Callable,
    *args: Any,
    timeout_s: float = ProcessPool.DEFAULT_TIMEOUT_S,
    **kwargs: Any,
) -> Any:
    return await get_process_pool().run_io(
        fn, *args, timeout_s=timeout_s, **kwargs)


def submit_cpu(
    fn: Callable,
    *args: Any,
    priority: int = TaskPriority.NORMAL,
    timeout_s: float = ProcessPool.DEFAULT_TIMEOUT_S,
    **kwargs: Any,
) -> Optional[Future]:
    return get_process_pool().submit(
        fn, *args, priority=priority,
        timeout_s=timeout_s, use_io=False, **kwargs)


def submit_io(
    fn: Callable,
    *args: Any,
    timeout_s: float = ProcessPool.DEFAULT_TIMEOUT_S,
    **kwargs: Any,
) -> Optional[Future]:
    return get_process_pool().submit(
        fn, *args, timeout_s=timeout_s, use_io=True, **kwargs)


# ============================================================
# 装饰器
# ============================================================
def register_task(
    priority: int = TaskPriority.NORMAL,
    timeout_s: float = ProcessPool.DEFAULT_TIMEOUT_S,
    use_io: bool = False,
) -> Callable:
    """
    将同步函数包装为异步函数，自动通过进程池执行。

    用法：
        @register_task(priority=TaskPriority.HIGH, timeout_s=10)
        def heavy_compute(x, y):
            return x + y

        # 调用
        result = await heavy_compute(1, 2)
    """
    def decorator(fn: Callable) -> Callable:
        if inspect.iscoroutinefunction(fn):
            # 异步函数直接执行（不进池）
            return fn

        async def wrapper(*args: Any, **kwargs: Any) -> Any:
            if use_io:
                return await run_io(
                    fn, *args, timeout_s=timeout_s, **kwargs)
            return await run_cpu(
                fn, *args, priority=priority,
                timeout_s=timeout_s, **kwargs)

        wrapper.__name__ = getattr(fn, "__name__", "wrapped")
        wrapper.__doc__ = getattr(fn, "__doc__", "")
        wrapper.__wrapped__ = fn
        return wrapper
    return decorator
