"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: cpu_affinity
version: 2.0.0
level: 0
required: false
author: quant-team
created: 2026-10-14
brief: CPU亲和性管理（线程绑定/工作负载规划/NUMA感知/优雅降级）

provides:
  - class CPUAffinity
  - class CoreInfo
  - class AffinityPlan
  - function get_cpu_affinity
  - function init_cpu_affinity
  - function pin_current_thread
  - function plan_workload
  - function pinned_to

requires:
  modules: [core.logger]
  libraries:
    - psutil>=5.9  (可选，NUMA/拓扑)
  resources:
    memory_mb: 4
    cpu_cores: 1

health:
  fn: CPUAffinity.health
  timeout_ms: 500

assembly:
  order: 17
  parallel: false

tests:
  - tests/test_cpu_affinity.py

fallback:
  - 平台不支持 → 静默降级（返回 False）
  - 权限不足 → 降级并告警
  - psutil 缺失 → 使用 os.cpu_count() 单 NUMA

tags: [core, performance, affinity]
---UMS-END---"""

from __future__ import annotations

import os
import sys
import threading
from contextlib import contextmanager
from dataclasses import dataclass, field
from typing import Any, Dict, Iterator, List, Optional, Set, Tuple

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("cpu_affinity")
        except Exception:
            import logging
            _logger = logging.getLogger("cpu_affinity")
    return _logger


# ============================================================
# 平台检测
# ============================================================
IS_LINUX = sys.platform.startswith("linux")
IS_WINDOWS = sys.platform == "win32"
IS_MACOS = sys.platform == "darwin"

_HAS_SCHED_AFFINITY = (
    IS_LINUX
    and hasattr(os, "sched_setaffinity")
    and hasattr(os, "sched_getaffinity")
)

try:
    import psutil  # type: ignore
    _HAS_PSUTIL = True
except ImportError:
    _HAS_PSUTIL = False


# ============================================================
# 异常
# ============================================================
class AffinityError(Exception):
    """CPU 亲和性基类异常"""


class CoreNotAvailableError(AffinityError):
    """指定核心不可用"""


# ============================================================
# 数据结构
# ============================================================
@dataclass
class CoreInfo:
    """单个核心信息"""
    core_id: int
    numa_node: int = 0
    isolated: bool = False


@dataclass
class AffinityPlan:
    """工作负载规划结果"""
    assignments: Dict[str, List[int]] = field(default_factory=dict)
    reserved_cores: List[int] = field(default_factory=list)
    numa_aware: bool = False

    def get(self, component: str) -> List[int]:
        return self.assignments.get(component, [])


# 优先级排序
_PRIORITY_ORDER = {
    "critical": 0,
    "high": 1,
    "normal": 2,
    "low": 3,
}


# ============================================================
# CPUAffinity 单例
# ============================================================
class CPUAffinity:
    """
    跨平台 CPU 亲和性管理器。

    特性：
    - 平台检测与优雅降级
    - 单例 + 线程安全
    - 线程绑定跟踪与恢复
    - 工作负载规划（NUMA 感知）
    - 上下文管理器
    """

    _instance: Optional["CPUAffinity"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "CPUAffinity":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(self, enable: bool = True) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._enable = enable and _HAS_SCHED_AFFINITY
            self._thread_cores: Dict[int, List[int]] = {}
            self._original_cores: Set[int] = set()
            self._stats = {
                "pin_success": 0,
                "pin_failed": 0,
                "reset_success": 0,
                "reset_failed": 0,
                "unsupported_calls": 0,
            }
            self._last_error = ""
            self._initialized = True
            self._detect_topology()

    # ========================================================
    # 拓扑检测
    # ========================================================
    def _detect_topology(self) -> None:
        # 可用核心
        try:
            if _HAS_SCHED_AFFINITY:
                self._available_cores: List[int] = sorted(
                    os.sched_getaffinity(0))
            else:
                self._available_cores = list(range(os.cpu_count() or 1))
        except Exception as exc:
            self._available_cores = list(range(os.cpu_count() or 1))
            _log().warning(f"获取可用核心失败: {exc}")

        # 记录原始亲和性（用于恢复）
        self._original_cores = set(self._available_cores)

        # NUMA 拓扑
        self._numa_map: Dict[int, List[int]] = {}
        self._numa_aware = False
        if _HAS_PSUTIL:
            try:
                topo = psutil.cpu_topology() or {}
                nodes = topo.get("nodes", [])
                if nodes:
                    for idx, cores in enumerate(nodes):
                        if isinstance(cores, list):
                            self._numa_map[idx] = list(cores)
                    self._numa_aware = bool(self._numa_map)
            except Exception as exc:
                _log().debug(f"NUMA 拓扑获取失败: {exc}")
        if not self._numa_map:
            self._numa_map = {0: list(self._available_cores)}

    # ========================================================
    # 查询
    # ========================================================
    def is_supported(self) -> bool:
        return self._enable

    def available_cores(self) -> List[int]:
        with self._lock:
            return list(self._available_cores)

    def numa_nodes(self) -> Dict[int, List[int]]:
        with self._lock:
            return {k: list(v) for k, v in self._numa_map.items()}

    def is_numa_aware(self) -> bool:
        with self._lock:
            return self._numa_aware

    def core_info(self) -> List[CoreInfo]:
        result: List[CoreInfo] = []
        with self._lock:
            for node, cores in self._numa_map.items():
                for c in cores:
                    result.append(CoreInfo(core_id=c, numa_node=node))
        return result

    def get_affinity(self, tid: int = 0) -> Optional[Set[int]]:
        """获取指定线程的亲和性。tid=0 表示当前线程。"""
        if not _HAS_SCHED_AFFINITY:
            return None
        try:
            return set(os.sched_getaffinity(tid))
        except Exception as exc:
            _log().debug(f"获取亲和性失败 (tid={tid}): {exc}")
            return None

    # ========================================================
    # 绑定
    # ========================================================
    def pin_current_thread(
        self,
        core_id: int,
        allow_shared: bool = False,
    ) -> bool:
        """
        绑定当前线程到指定核心。
        core_id 必须存在于可用核心中。
        """
        if not self._enable:
            with self._lock:
                self._stats["unsupported_calls"] += 1
            return False

        # 核心校验
        if core_id not in self._available_cores:
            raise CoreNotAvailableError(
                f"核心 {core_id} 不在可用列表 "
                f"{self._available_cores[:16]}...")

        try:
            os.sched_setaffinity(0, {core_id})
            tid = threading.get_ident()
            with self._lock:
                self._thread_cores[tid] = [core_id]
                self._stats["pin_success"] += 1
            _log().debug(
                f"线程 {tid} 绑定到核心 {core_id}")
            return True
        except PermissionError as exc:
            with self._lock:
                self._stats["pin_failed"] += 1
                self._last_error = f"权限不足: {exc}"
            _log().warning(
                f"绑定核心 {core_id} 失败（权限不足），降级继续")
            return False
        except OSError as exc:
            with self._lock:
                self._stats["pin_failed"] += 1
                self._last_error = f"OSError: {exc}"
            _log().warning(f"绑定核心 {core_id} 失败: {exc}")
            return False
        except Exception as exc:
            with self._lock:
                self._stats["pin_failed"] += 1
                self._last_error = str(exc)
            _log().error(f"绑定核心异常: {exc}")
            return False

    def pin_thread(self, tid: int, core_id: int) -> bool:
        """绑定指定线程（需原生 TID，来自 threading.get_native_id()）"""
        if not self._enable:
            with self._lock:
                self._stats["unsupported_calls"] += 1
            return False
        if core_id not in self._available_cores:
            raise CoreNotAvailableError(
                f"核心 {core_id} 不可用")
        try:
            os.sched_setaffinity(tid, {core_id})
            with self._lock:
                self._thread_cores[tid] = [core_id]
                self._stats["pin_success"] += 1
            return True
        except Exception as exc:
            with self._lock:
                self._stats["pin_failed"] += 1
                self._last_error = str(exc)
            _log().warning(f"绑定线程 {tid} 到核心 {core_id} 失败: {exc}")
            return False

    def pin_process(self, core_ids: List[int]) -> bool:
        """
        绑定当前线程到多个核心（Linux sched_setaffinity 语义为线程级，
        多核心集合即允许在这些核心上调度）。
        """
        if not self._enable:
            return False
        if not core_ids:
            raise AffinityError("core_ids 不能为空")
        invalid = [c for c in core_ids if c not in self._available_cores]
        if invalid:
            raise CoreNotAvailableError(
                f"无效核心: {invalid}")
        try:
            os.sched_setaffinity(0, set(core_ids))
            tid = threading.get_ident()
            with self._lock:
                self._thread_cores[tid] = list(core_ids)
                self._stats["pin_success"] += 1
            _log().debug(
                f"线程 {tid} 绑定到核心集合 {core_ids}")
            return True
        except Exception as exc:
            with self._lock:
                self._stats["pin_failed"] += 1
                self._last_error = str(exc)
            _log().warning(f"绑定核心集合失败: {exc}")
            return False

    # ========================================================
    # 恢复
    # ========================================================
    def reset_current(self) -> bool:
        """恢复当前线程的原始亲和性"""
        if not self._enable:
            return False
        try:
            os.sched_setaffinity(0, self._original_cores)
            tid = threading.get_ident()
            with self._lock:
                self._thread_cores.pop(tid, None)
                self._stats["reset_success"] += 1
            return True
        except Exception as exc:
            with self._lock:
                self._stats["reset_failed"] += 1
                self._last_error = str(exc)
            _log().warning(f"恢复亲和性失败: {exc}")
            return False

    def reset_all(self) -> int:
        """恢复所有已跟踪线程（尽力而为，仅能恢复当前线程）"""
        # 只能恢复当前线程
        ok = 1 if self.reset_current() else 0
        with self._lock:
            # 清空跟踪（其他线程由调用方自行恢复）
            self._thread_cores.clear()
        return ok

    def pinned_cores(self) -> Dict[int, List[int]]:
        with self._lock:
            return {k: list(v) for k, v in self._thread_cores.items()}

    # ========================================================
    # 工作负载规划
    # ========================================================
    def plan_workload(
        self,
        components: Dict[str, str],
        reserve_cores: int = 1,
        numa_aware: Optional[bool] = None,
    ) -> AffinityPlan:
        """
        为组件分配核心。

        components: {组件名: 优先级("critical"|"high"|"normal"|"low")}
        reserve_cores: 为系统保留的核心数
        numa_aware: 是否按 NUMA 节点分配（None 时自动判断）
        """
        plan = AffinityPlan()

        with self._lock:
            cores = list(self._available_cores)
            numa = {k: list(v) for k, v in self._numa_map.items()}
            plan.numa_aware = (
                self._numa_aware if numa_aware is None else numa_aware)

        if not cores:
            return plan

        # 保留核心
        reserve = max(0, min(reserve_cores, len(cores) - 1))
        plan.reserved_cores = cores[-reserve:] if reserve else []
        usable = cores[:-reserve] if reserve else cores[:]
        if not usable:
            usable = cores[:]

        # 按优先级排序组件
        sorted_comps = sorted(
            components.items(),
            key=lambda x: _PRIORITY_ORDER.get(x[1], 99),
        )

        assigned: Set[int] = set()
        # 先为 critical / high 分配专用核心
        for name, prio in sorted_comps:
            if prio in ("critical", "high"):
                for c in usable:
                    if c not in assigned:
                        plan.assignments[name] = [c]
                        assigned.add(c)
                        break

        # 剩余组件共享剩余核心
        shared = [c for c in usable if c not in assigned] or usable
        for name, prio in sorted_comps:
            if name in plan.assignments:
                continue
            plan.assignments[name] = list(shared)

        return plan

    # ========================================================
    # 上下文管理器
    # ========================================================
    @contextmanager
    def pinned_to(self, core_id: int) -> Iterator[bool]:
        """with affinity.pinned_to(2): ... 退出时自动恢复"""
        success = False
        try:
            success = self.pin_current_thread(core_id)
            yield success
        finally:
            if success:
                self.reset_current()

    # ========================================================
    # 状态
    # ========================================================
    def stats(self) -> Dict[str, Any]:
        with self._lock:
            return dict(self._stats)

    def health(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "status": "ok" if self._enable else "degraded",
                "supported": self._enable,
                "platform": sys.platform,
                "available_cores": len(self._available_cores),
                "numa_nodes": len(self._numa_map),
                "numa_aware": self._numa_aware,
                "pinned_threads": len(self._thread_cores),
                "psutil_available": _HAS_PSUTIL,
                "last_error": self._last_error,
                "stats": dict(self._stats),
            }

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<CPUAffinity supported={self._enable} "
                f"cores={len(self._available_cores)} "
                f"numa={len(self._numa_map)} "
                f"pinned={len(self._thread_cores)}>"
            )


# ============================================================
# 全局入口
# ============================================================
_affinity: Optional[CPUAffinity] = None
_affinity_lock = threading.Lock()


def init_cpu_affinity(enable: bool = True) -> CPUAffinity:
    """显式初始化（启动时调用一次）"""
    global _affinity
    with _affinity_lock:
        _affinity = CPUAffinity(enable=enable)
    return _affinity


def get_cpu_affinity() -> CPUAffinity:
    """获取全局单例（惰性初始化）"""
    global _affinity
    if _affinity is None:
        with _affinity_lock:
            if _affinity is None:
                _affinity = CPUAffinity()
    return _affinity


# ============================================================
# 便捷函数
# ============================================================
def pin_current_thread(core_id: int) -> bool:
    return get_cpu_affinity().pin_current_thread(core_id)


def plan_workload(
    components: Dict[str, str],
    reserve_cores: int = 1,
) -> AffinityPlan:
    return get_cpu_affinity().plan_workload(components, reserve_cores)


@contextmanager
def pinned_to(core_id: int) -> Iterator[bool]:
    with get_cpu_affinity().pinned_to(core_id) as ok:
        yield ok
