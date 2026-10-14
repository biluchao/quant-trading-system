"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: compat
version: 2.0.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: 跨平台兼容层（事件循环/信号/CPU亲和/子进程/平台信息）

provides:
  - class Compat
  - function install
  - function is_windows
  - function is_linux
  - function is_macos
  - function pin_cpu
  - function set_default_executor
  - function platform_info
  - function check_python_version

requires:
  modules: [core.logger]
  libraries:
    - uvloop>=0.21  (可选，仅 Linux/macOS)
  resources:
    memory_mb: 8
    cpu_cores: 1

health:
  fn: Compat.health
  timeout_ms: 500

assembly:
  order: 15
  parallel: false

tests:
  - tests/test_compat.py

fallback:
  - uvloop 缺失 → asyncio 默认
  - uvloop 安装失败 → asyncio 默认
  - CPU 亲和不可用 → 跳过
  - 信号不可用 → 忽略

tags: [core, platform, compat]
---UMS-END---"""

from __future__ import annotations

import os
import sys
import signal
import platform as _platform
import threading
import asyncio
import multiprocessing as mp
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("compat")
        except Exception:
            import logging
            _logger = logging.getLogger("compat")
    return _logger


# ============================================================
# 平台常量
# ============================================================
IS_WINDOWS: bool = sys.platform == "win32"
IS_LINUX: bool = sys.platform.startswith("linux")
IS_MACOS: bool = sys.platform == "darwin"
IS_POSIX: bool = os.name == "posix"

# 子进程
HAS_FORK: bool = hasattr(os, "fork") and not IS_WINDOWS

# CPU 亲和
HAS_AFFINITY: bool = hasattr(os, "sched_setaffinity") and IS_LINUX

# Python 版本
MIN_PYTHON: Tuple[int, int] = (3, 12)


# ============================================================
# 异常
# ============================================================
class CompatError(Exception):
    """兼容层基类异常"""


class PythonVersionError(CompatError):
    """Python 版本不满足要求"""


# ============================================================
# 兼容信息
# ============================================================
@dataclass
class CompatInfo:
    python_version: str = ""
    python_impl: str = ""
    platform: str = ""
    platform_release: str = ""
    machine: str = ""
    cpu_count: int = 0
    has_uvloop: bool = False
    event_loop: str = "asyncio"
    has_fork: bool = False
    has_affinity: bool = False
    is_windows: bool = False
    is_linux: bool = False
    is_macos: bool = False
    installed_ms: int = 0


# ============================================================
# Compat 单例
# ============================================================
class Compat:
    """
    跨平台兼容层。

    - 统一事件循环策略
    - 信号兼容
    - CPU 亲和
    - 线程池配置
    - 平台信息导出
    """

    _instance: Optional["Compat"] = None
    _lock = threading.Lock()

    # 环境变量开关
    ENV_DISABLE_UVLOOP = "QUANT_NO_UVLOOP"

    def __new__(cls, *args: Any, **kwargs: Any) -> "Compat":
        with cls._lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(self) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._lock:
            if getattr(self, "_initialized", False):
                return
            self._inner_lock = threading.RLock()
            self._uvloop_installed = False
            self._install_time_ms = 0
            self._executor_configured = False
            self._info = CompatInfo()
            self._initialized = True
            self._collect_info()

    # ========================================================
    # 信息收集
    # ========================================================
    def _collect_info(self) -> None:
        try:
            self._info.python_version = (
                f"{sys.version_info.major}."
                f"{sys.version_info.minor}."
                f"{sys.version_info.micro}"
            )
            self._info.python_impl = _platform.python_implementation()
            self._info.platform = sys.platform
            self._info.platform_release = _platform.release()
            self._info.machine = _platform.machine()
            self._info.cpu_count = os.cpu_count() or 1
            self._info.has_fork = HAS_FORK
            self._info.has_affinity = HAS_AFFINITY
            self._info.is_windows = IS_WINDOWS
            self._info.is_linux = IS_LINUX
            self._info.is_macos = IS_MACOS
            self._info.installed_ms = int(__import__("time").time() * 1000)
        except Exception as exc:
            _log().warning(f"平台信息采集异常: {exc}")

    # ========================================================
    # Python 版本
    # ========================================================
    def check_python_version(
        self, min_version: Tuple[int, int] = MIN_PYTHON
    ) -> None:
        cur = (sys.version_info.major, sys.version_info.minor)
        if cur < min_version:
            msg = (f"需要 Python ≥ {min_version[0]}.{min_version[1]}，"
                   f"当前 {cur[0]}.{cur[1]}")
            _log().error(msg)
            raise PythonVersionError(msg)
        _log().info(
            f"Python 版本检查通过: "
            f"{self._info.python_version} ({self._info.python_impl})")

    # ========================================================
    # 事件循环安装
    # ========================================================
    def install(self, prefer_uvloop: bool = True) -> str:
        """
        安装最优事件循环策略，返回最终使用的实现名。
        支持环境变量 QUANT_NO_UVLOOP=1 强制禁用 uvloop。
        """
        with self._inner_lock:
            if self._uvloop_installed:
                return self._info.event_loop

            self.check_python_version()

            # 1. Windows：强制使用默认 Selector/Proactor，不做 uvloop
            if IS_WINDOWS:
                self._apply_windows_policy()
                self._info.event_loop = "asyncio-proactor"
                self._uvloop_installed = True
                _log().info("Windows 平台，使用 asyncio 默认事件循环")
                return self._info.event_loop

            # 2. 显式禁用
            if os.environ.get(self.ENV_DISABLE_UVLOOP, "0") == "1":
                self._info.event_loop = "asyncio"
                self._uvloop_installed = True
                _log().info("环境变量禁用 uvloop，使用 asyncio")
                return self._info.event_loop

            # 3. 尝试 uvloop
            if prefer_uvloop:
                try:
                    import uvloop  # type: ignore
                    # 兼容不同版本的 install API
                    if hasattr(uvloop, "install"):
                        uvloop.install()
                    else:
                        asyncio.set_event_loop_policy(
                            uvloop.EventLoopPolicy())
                    self._info.has_uvloop = True
                    self._info.event_loop = "uvloop"
                    self._uvloop_installed = True
                    _log().info("已启用 uvloop 事件循环")
                    return self._info.event_loop
                except ImportError:
                    _log().warning("uvloop 未安装，降级到 asyncio")
                except Exception as exc:
                    _log().warning(
                        f"uvloop 安装失败: {exc}，降级到 asyncio")

            # 4. 默认 asyncio
            self._info.event_loop = "asyncio"
            self._uvloop_installed = True
            return self._info.event_loop

    def _apply_windows_policy(self) -> None:
        """Windows 特定处理"""
        try:
            # Proactor 性能更好，但与子进程/部分库有兼容问题
            # 默认用 Selector，如需 Proactor 由用户指定
            if hasattr(asyncio, "WindowsSelectorEventLoopPolicy"):
                asyncio.set_event_loop_policy(
                    asyncio.WindowsSelectorEventLoopPolicy())
        except Exception as exc:
            _log().warning(f"Windows 事件循环策略设置失败: {exc}")

    @property
    def event_loop(self) -> str:
        return self._info.event_loop

    # ========================================================
    # 信号兼容
    # ========================================================
    def register_shutdown_signals(
        self, loop: asyncio.AbstractEventLoop,
        callback: Any,
    ) -> List[str]:
        """
        跨平台注册关闭信号。返回注册成功的信号名列表。
        """
        registered: List[str] = []
        sig_map = []
        if hasattr(signal, "SIGINT"):
            sig_map.append(signal.SIGINT)
        if hasattr(signal, "SIGTERM") and not IS_WINDOWS:
            sig_map.append(signal.SIGTERM)
        # Windows 上额外注册 SIGBREAK
        if IS_WINDOWS and hasattr(signal, "SIGBREAK"):
            sig_map.append(signal.SIGBREAK)

        for sig in sig_map:
            try:
                loop.add_signal_handler(sig, callback)
                registered.append(signal.Signals(sig).name)
            except (NotImplementedError, RuntimeError, ValueError) as exc:
                _log().debug(
                    f"信号 {sig} 无法注册: {exc}")

        if not registered:
            # 降级到 signal.signal
            try:
                signal.signal(signal.SIGINT, lambda *_: callback())
                registered.append("SIGINT")
            except Exception:
                pass
        return registered

    # ========================================================
    # CPU 亲和
    # ========================================================
    def pin_cpu(self, cores: List[int]) -> bool:
        """
        绑定当前进程到指定 CPU 核心。
        仅 Linux 支持。
        """
        if not HAS_AFFINITY:
            return False
        try:
            os.sched_setaffinity(0, set(cores))
            _log().info(f"已绑定 CPU 核心: {cores}")
            return True
        except Exception as exc:
            _log().warning(f"CPU 亲和绑定失败: {exc}")
            return False

    def available_cores(self) -> List[int]:
        if not HAS_AFFINITY:
            return list(range(os.cpu_count() or 1))
        try:
            return sorted(os.sched_getaffinity(0))
        except Exception:
            return list(range(os.cpu_count() or 1))

    # ========================================================
    # 线程池
    # ========================================================
    def set_default_executor(
        self,
        loop: asyncio.AbstractEventLoop,
        max_workers: Optional[int] = None,
        thread_name_prefix: str = "quant-pool",
    ) -> int:
        """
        配置默认线程池。返回实际线程数。
        """
        if max_workers is None:
            # 保留 2 个核心给 I/O 线程
            cpu = os.cpu_count() or 2
            max_workers = max(2, cpu - 2)
        try:
            from concurrent.futures import ThreadPoolExecutor
            executor = ThreadPoolExecutor(
                max_workers=max_workers,
                thread_name_prefix=thread_name_prefix,
            )
            loop.set_default_executor(executor)
            self._executor_configured = True
            _log().info(f"线程池已配置: {max_workers} 线程")
            return max_workers
        except Exception as exc:
            _log().warning(f"线程池配置失败: {exc}")
            return 0

    # ========================================================
    # 子进程
    # ========================================================
    def subprocess_start_method(self) -> str:
        """
        选择子进程启动方式。
        - Linux/macOS: fork（快速）
        - Windows: spawn（必须）
        - macOS 上 fork 与 asyncio 有兼容问题，若 asyncio 已运行则用 spawn
        """
        if IS_WINDOWS:
            return "spawn"
        if IS_MACOS:
            try:
                asyncio.get_running_loop()
                # 在 asyncio 上下文中，macOS 使用 spawn 更安全
                return "spawn"
            except RuntimeError:
                return "fork"
        return "fork"

    def configure_multiprocessing(self) -> str:
        """配置多进程启动方式，返回实际设置"""
        method = self.subprocess_start_method()
        try:
            mp.set_start_method(method, force=False)
        except RuntimeError:
            # 已设置过
            pass
        except Exception as exc:
            _log().warning(f"多进程启动方式设置失败: {exc}")
        return method

    # ========================================================
    # 睡眠精度
    # ========================================================
    @staticmethod
    def sleep_precision_ms() -> float:
        """
        Windows 默认 sleep 精度约 15ms；此函数返回经验精度。
        """
        return 15.0 if IS_WINDOWS else 1.0

    @staticmethod
    async def async_sleep_accurate(seconds: float) -> None:
        """
        高精度异步睡眠。Windows 上使用短循环减少抖动。
        """
        if seconds <= 0:
            return
        if not IS_WINDOWS:
            await asyncio.sleep(seconds)
            return
        # Windows：分片 sleep，提升精度
        end = asyncio.get_event_loop().time() + seconds
        while True:
            remaining = end - asyncio.get_event_loop().time()
            if remaining <= 0:
                break
            await asyncio.sleep(min(remaining, 0.005))

    # ========================================================
    # 路径
    # ========================================================
    @staticmethod
    def normalize_path(path: str) -> Path:
        """统一路径处理，自动展开 ~ 和 env"""
        expanded = os.path.expandvars(os.path.expanduser(path))
        return Path(expanded).resolve()

    # ========================================================
    # 信息 / 健康
    # ========================================================
    def info(self) -> Dict[str, Any]:
        with self._inner_lock:
            return {
                "python_version": self._info.python_version,
                "python_impl": self._info.python_impl,
                "platform": self._info.platform,
                "platform_release": self._info.platform_release,
                "machine": self._info.machine,
                "cpu_count": self._info.cpu_count,
                "event_loop": self._info.event_loop,
                "has_uvloop": self._info.has_uvloop,
                "has_fork": HAS_FORK,
                "has_affinity": HAS_AFFINITY,
                "is_windows": IS_WINDOWS,
                "is_linux": IS_LINUX,
                "is_macos": IS_MACOS,
                "executor_configured": self._executor_configured,
                "subprocess_method": self.subprocess_start_method(),
            }

    def health(self) -> Dict[str, Any]:
        with self._inner_lock:
            return {
                "status": "ok",
                "installed": self._uvloop_installed,
                "event_loop": self._info.event_loop,
                "python_ok": (
                    (sys.version_info.major, sys.version_info.minor)
                    >= MIN_PYTHON
                ),
                "platform": self._info.platform,
                "cpu_count": self._info.cpu_count,
            }


# ============================================================
# 全局单例
# ============================================================
_compat: Optional[Compat] = None
_compat_lock = threading.Lock()


def get_compat() -> Compat:
    global _compat
    if _compat is None:
        with _compat_lock:
            if _compat is None:
                _compat = Compat()
    return _compat


# ============================================================
# 便捷函数
# ============================================================
def install(prefer_uvloop: bool = True) -> str:
    """显式安装事件循环（启动时调用）"""
    return get_compat().install(prefer_uvloop=prefer_uvloop)


def is_windows() -> bool:
    return IS_WINDOWS


def is_linux() -> bool:
    return IS_LINUX


def is_macos() -> bool:
    return IS_MACOS


def pin_cpu(cores: List[int]) -> bool:
    return get_compat().pin_cpu(cores)


def set_default_executor(
    loop: asyncio.AbstractEventLoop,
    max_workers: Optional[int] = None,
) -> int:
    return get_compat().set_default_executor(loop, max_workers)


def platform_info() -> Dict[str, Any]:
    return get_compat().info()


def check_python_version(
    min_version: Tuple[int, int] = MIN_PYTHON,
) -> None:
    return get_compat().check_python_version(min_version)


def register_shutdown_signals(
    loop: asyncio.AbstractEventLoop,
    callback: Any,
) -> List[str]:
    return get_compat().register_shutdown_signals(loop, callback)
