"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: health
version: 2.0.0
level: 0
required: false
author: quant-team
created: 2026-10-14
brief: 健康检查注册与聚合（分级/超时/并发/TTL/依赖/事件）

provides:
  - class HealthRegistry
  - class HealthCheck
  - class HealthReport
  - class HealthStatus
  - function get_health
  - function init_health
  - function health_check
  - function register_health
  - function register_ready

requires:
  modules: [core.logger, core.timestamp]
  libraries: []
  resources:
    memory_mb: 8
    cpu_cores: 1

health:
  fn: HealthRegistry.health
  timeout_ms: 200

assembly:
  order: 20
  parallel: false

tests:
  - tests/test_health.py

fallback:
  - event_bus 缺失 → 跳过通知
  - asyncio 不可用 → 同步串行执行
  - 检查超时 → 标记 DOWN

tags: [core, health, observability]
---UMS-END---"""

from __future__ import annotations

import asyncio
import inspect
import threading
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("health")
        except Exception:
            import logging
            _logger = logging.getLogger("health")
    return _logger


def _now_ms() -> int:
    try:
        from core.timestamp import now_ms
        return now_ms()
    except Exception:
        return int(time.time() * 1000)


# ============================================================
# 状态
# ============================================================
class HealthStatus(str, Enum):
    OK = "ok"                 # 完全健康
    DEGRADED = "degraded"     # 部分降级，可继续运行
    DOWN = "down"             # 不可用
    UNKNOWN = "unknown"       # 无法检测


# 状态严重度排序（越大越严重）
_SEVERITY = {
    HealthStatus.OK: 0,
    HealthStatus.UNKNOWN: 1,
    HealthStatus.DEGRADED: 2,
    HealthStatus.DOWN: 3,
}


def _worse(a: HealthStatus, b: HealthStatus) -> HealthStatus:
    return a if _SEVERITY[a] >= _SEVERITY[b] else b


# ============================================================
# 检查定义
# ============================================================
@dataclass
class HealthCheck:
    """单个健康检查定义"""
    name: str
    fn: Callable[[], Any]
    category: str = "general"
    critical: bool = False           # 关键模块，DOWN 则整体 DOWN
    weight: float = 1.0              # 用于加权聚合
    timeout_s: float = 2.0
    ttl_s: float = 0.0               # 缓存 TTL，0 表示不缓存
    check_type: str = "health"       # "health" | "ready"
    dependencies: List[str] = field(default_factory=list)

    # 运行时状态
    last_status: HealthStatus = HealthStatus.UNKNOWN
    last_run_ms: int = 0
    last_duration_ms: int = 0
    last_error: str = ""
    last_details: Dict[str, Any] = field(default_factory=dict)
    consecutive_failures: int = 0
    last_ok_ms: int = 0


# ============================================================
# 报告
# ============================================================
@dataclass
class HealthReport:
    """聚合健康报告"""
    status: HealthStatus = HealthStatus.UNKNOWN
    timestamp_ms: int = 0
    duration_ms: int = 0
    checks: Dict[str, Dict[str, Any]] = field(default_factory=dict)
    ready: bool = False
    summary: Dict[str, int] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "status": self.status.value,
            "timestamp_ms": self.timestamp_ms,
            "duration_ms": self.duration_ms,
            "ready": self.ready,
            "summary": self.summary,
            "checks": self.checks,
        }


# ============================================================
# HealthRegistry 单例
# ============================================================
class HealthRegistry:
    """
    健康检查注册与聚合。

    特性：
    - 分级状态（OK/DEGRADED/DOWN/UNKNOWN）
    - 并发执行 + 独立超时
    - TTL 缓存（廉价/昂贵检查分离）
    - 依赖联动
    - 事件通知
    - liveness / readiness 分离
    """

    _instance: Optional["HealthRegistry"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "HealthRegistry":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self,
        default_timeout_s: float = 2.0,
        history_size: int = 100,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._checks: Dict[str, HealthCheck] = {}
            self._default_timeout_s = default_timeout_s
            self._history: List[HealthReport] = []
            self._history_size = history_size
            self._last_report: Optional[HealthReport] = None
            self._last_status: HealthStatus = HealthStatus.UNKNOWN
            self._last_change_ms: int = 0
            self._event_publisher: Optional[Callable[[str, Any], None]] = None
            self._stats = {
                "total_runs": 0,
                "total_checks": 0,
                "total_failures": 0,
                "cache_hits": 0,
                "cache_misses": 0,
                "status_changes": 0,
                "started_ms": _now_ms(),
            }
            self._initialized = True

    # ========================================================
    # 注册
    # ========================================================
    def register(
        self,
        name: str,
        fn: Callable[[], Any],
        category: str = "general",
        critical: bool = False,
        weight: float = 1.0,
        timeout_s: Optional[float] = None,
        ttl_s: float = 0.0,
        check_type: str = "health",
        dependencies: Optional[List[str]] = None,
    ) -> HealthCheck:
        if not name or not callable(fn):
            raise ValueError("name 和 fn 均不能为空")
        check = HealthCheck(
            name=name,
            fn=fn,
            category=category,
            critical=critical,
            weight=weight,
            timeout_s=timeout_s if timeout_s is not None
            else self._default_timeout_s,
            ttl_s=ttl_s,
            check_type=check_type,
            dependencies=list(dependencies or []),
        )
        with self._lock:
            self._checks[name] = check
        _log().debug(
            f"注册健康检查: {name} "
            f"(category={category}, critical={critical})")
        return check

    def unregister(self, name: str) -> bool:
        with self._lock:
            if name in self._checks:
                del self._checks[name]
                return True
            return False

    def has(self, name: str) -> bool:
        with self._lock:
            return name in self._checks

    def list_checks(self) -> List[Dict[str, Any]]:
        with self._lock:
            return [
                {
                    "name": c.name,
                    "category": c.category,
                    "critical": c.critical,
                    "check_type": c.check_type,
                    "last_status": c.last_status.value,
                    "consecutive_failures": c.consecutive_failures,
                }
                for c in self._checks.values()
            ]

    def clear(self) -> None:
        with self._lock:
            self._checks.clear()

    # ========================================================
    # 事件
    # ========================================================
    def set_event_publisher(
        self, publisher: Callable[[str, Any], None],
    ) -> None:
        self._event_publisher = publisher

    def _publish(self, topic: str, data: Any) -> None:
        if self._event_publisher is None:
            return
        try:
            self._event_publisher(topic, data)
        except Exception as exc:
            _log().debug(f"事件推送失败: {exc}")

    # ========================================================
    # 执行单检查
    # ========================================================
    def _is_cache_valid(self, check: HealthCheck) -> bool:
        if check.ttl_s <= 0:
            return False
        if check.last_run_ms == 0:
            return False
        age_s = (_now_ms() - check.last_run_ms) / 1000
        return age_s < check.ttl_s

    def _run_check_sync(
        self, check: HealthCheck,
    ) -> Tuple[HealthStatus, str, Dict[str, Any]]:
        start = time.perf_counter()
        try:
            result = check.fn()
            if inspect.iscoroutine(result):
                # 同步上下文中运行协程
                try:
                    loop = asyncio.new_event_loop()
                    try:
                        result = loop.run_until_complete(
                            asyncio.wait_for(
                                result, timeout=check.timeout_s))
                    finally:
                        loop.close()
                except asyncio.TimeoutError:
                    return (
                        HealthStatus.DOWN,
                        f"超时 ({check.timeout_s}s)",
                        {},
                    )
            status, details = self._interpret(result)
            error = details.get("error", "")
            check.last_ok_ms = (
                _now_ms() if status == HealthStatus.OK
                else check.last_ok_ms)
            return status, error, details
        except Exception as exc:
            return (
                HealthStatus.DOWN,
                f"{type(exc).__name__}: {exc}",
                {"error": str(exc)},
            )
        finally:
            elapsed = int((time.perf_counter() - start) * 1000)
            check.last_duration_ms = elapsed

    async def _run_check_async(
        self, check: HealthCheck,
    ) -> Tuple[HealthStatus, str, Dict[str, Any]]:
        start = time.perf_counter()
        try:
            result = check.fn()
            if inspect.iscoroutine(result):
                result = await asyncio.wait_for(
                    result, timeout=check.timeout_s)
            status, details = self._interpret(result)
            error = details.get("error", "")
            check.last_ok_ms = (
                _now_ms() if status == HealthStatus.OK
                else check.last_ok_ms)
            return status, error, details
        except asyncio.TimeoutError:
            return (
                HealthStatus.DOWN,
                f"超时 ({check.timeout_s}s)",
                {},
            )
        except Exception as exc:
            return (
                HealthStatus.DOWN,
                f"{type(exc).__name__}: {exc}",
                {"error": str(exc)},
            )
        finally:
            elapsed = int((time.perf_counter() - start) * 1000)
            check.last_duration_ms = elapsed

    def _interpret(
        self, result: Any,
    ) -> Tuple[HealthStatus, Dict[str, Any]]:
        """将检查函数返回值标准化"""
        if result is None:
            return HealthStatus.OK, {}
        if isinstance(result, bool):
            return (
                HealthStatus.OK if result else HealthStatus.DOWN,
                {},
            )
        if isinstance(result, HealthStatus):
            return result, {}
        if isinstance(result, str):
            try:
                return HealthStatus(result.lower()), {}
            except ValueError:
                return HealthStatus.UNKNOWN, {"raw": result}
        if isinstance(result, dict):
            status_raw = result.get("status", "ok")
            try:
                status = HealthStatus(str(status_raw).lower())
            except ValueError:
                status = HealthStatus.UNKNOWN
            details = {k: v for k, v in result.items() if k != "status"}
            return status, details
        return HealthStatus.UNKNOWN, {"raw": str(result)}

    # ========================================================
    # 聚合
    # ========================================================
    def _aggregate(
        self,
        results: Dict[str, Tuple[HealthStatus, str, Dict[str, Any]]],
    ) -> Tuple[HealthStatus, Dict[str, int]]:
        """根据所有检查结果计算整体状态"""
        summary = {
            "ok": 0, "degraded": 0, "down": 0, "unknown": 0,
        }
        overall = HealthStatus.OK
        critical_down = False

        with self._lock:
            checks_snapshot = dict(self._checks)

        for name, (status, _, _) in results.items():
            summary[status.value] = summary.get(status.value, 0) + 1
            check = checks_snapshot.get(name)
            if check is not None and check.critical \
                    and status == HealthStatus.DOWN:
                critical_down = True
            overall = _worse(overall, status)

        if critical_down:
            overall = HealthStatus.DOWN
        return overall, summary

    # ========================================================
    # 报告
    # ========================================================
    def _build_report(
        self,
        results: Dict[str, Tuple[HealthStatus, str, Dict[str, Any]]],
        ready: bool,
        duration_ms: int,
    ) -> HealthReport:
        with self._lock:
            checks_snapshot = dict(self._checks)
            checks_dict: Dict[str, Dict[str, Any]] = {}
            for name, (status, error, details) in results.items():
                c = checks_snapshot.get(name)
                checks_dict[name] = {
                    "status": status.value,
                    "duration_ms": c.last_duration_ms if c else 0,
                    "critical": c.critical if c else False,
                    "error": error,
                    "details": details,
                    "consecutive_failures":
                        c.consecutive_failures if c else 0,
                }

        overall, summary = self._aggregate(results)
        report = HealthReport(
            status=overall,
            timestamp_ms=_now_ms(),
            duration_ms=duration_ms,
            checks=checks_dict,
            ready=ready,
            summary=summary,
        )
        return report

    def _record_report(self, report: HealthReport) -> None:
        with self._lock:
            self._last_report = report
            self._history.append(report)
            if len(self._history) > self._history_size:
                self._history = self._history[-self._history_size:]
            prev = self._last_status
            if prev != report.status:
                self._last_status = report.status
                self._last_change_ms = report.timestamp_ms
                self._stats["status_changes"] += 1
                _log().warning(
                    f"健康状态变化: {prev.value} → "
                    f"{report.status.value}")
                self._publish("health.status_changed", {
                    "from": prev.value,
                    "to": report.status.value,
                    "ts_ms": report.timestamp_ms,
                })

    # ========================================================
    # 同步执行
    # ========================================================
    def run_sync(
        self,
        check_type: Optional[str] = None,
        force: bool = False,
    ) -> HealthReport:
        """
        同步执行所有检查。

        check_type: "health" | "ready" | None（全部）
        force: 忽略 TTL 缓存
        """
        start = time.perf_counter()
        with self._lock:
            checks = list(self._checks.values())

        if check_type is not None:
            checks = [c for c in checks if c.check_type == check_type]

        results: Dict[str, Tuple[HealthStatus, str, Dict[str, Any]]] = {}

        for check in checks:
            # TTL 缓存
            if not force and self._is_cache_valid(check):
                results[check.name] = (
                    check.last_status,
                    check.last_error,
                    check.last_details,
                )
                with self._lock:
                    self._stats["cache_hits"] += 1
                continue

            with self._lock:
                self._stats["cache_misses"] += 1

            # 依赖检查
            if check.dependencies:
                dep_failed = any(
                    dep in results and
                    results[dep][0] in (
                        HealthStatus.DOWN, HealthStatus.UNKNOWN)
                    for dep in check.dependencies
                )
                if dep_failed:
                    results[check.name] = (
                        HealthStatus.UNKNOWN,
                        "依赖未就绪",
                        {},
                    )
                    continue

            status, error, details = self._run_check_sync(check)
            results[check.name] = (status, error, details)

            # 更新检查状态
            with self._lock:
                check.last_status = status
                check.last_run_ms = _now_ms()
                check.last_error = error
                check.last_details = details
                if status in (HealthStatus.DOWN, HealthStatus.UNKNOWN):
                    check.consecutive_failures += 1
                    self._stats["total_failures"] += 1
                else:
                    check.consecutive_failures = 0
                self._stats["total_checks"] += 1

        duration_ms = int((time.perf_counter() - start) * 1000)
        # ready 语义：所有 ready 检查都 OK
        ready = all(
            results[name][0] == HealthStatus.OK
            for name in results
            if check_type is None or True
        ) if results else False

        report = self._build_report(results, ready, duration_ms)
        with self._lock:
            self._stats["total_runs"] += 1
        self._record_report(report)
        return report

    # ========================================================
    # 异步执行（并发）
    # ========================================================
    async def run_async(
        self,
        check_type: Optional[str] = None,
        force: bool = False,
    ) -> HealthReport:
        """
        异步并发执行所有检查。
        """
        start = time.perf_counter()
        with self._lock:
            checks = list(self._checks.values())

        if check_type is not None:
            checks = [c for c in checks if c.check_type == check_type]

        # 分离缓存与需执行的
        results: Dict[str, Tuple[HealthStatus, str, Dict[str, Any]]] = {}
        to_run: List[HealthCheck] = []

        for check in checks:
            if not force and self._is_cache_valid(check):
                results[check.name] = (
                    check.last_status,
                    check.last_error,
                    check.last_details,
                )
                with self._lock:
                    self._stats["cache_hits"] += 1
            else:
                with self._lock:
                    self._stats["cache_misses"] += 1
                to_run.append(check)

        # 并发执行
        if to_run:
            tasks = [
                asyncio.create_task(self._run_check_async(c))
                for c in to_run
            ]
            gathered = await asyncio.gather(
                *tasks, return_exceptions=True)
            for check, result in zip(to_run, gathered):
                if isinstance(result, Exception):
                    status = HealthStatus.DOWN
                    error = f"{type(result).__name__}: {result}"
                    details: Dict[str, Any] = {}
                else:
                    status, error, details = result
                results[check.name] = (status, error, details)

                with self._lock:
                    check.last_status = status
                    check.last_run_ms = _now_ms()
                    check.last_error = error
                    check.last_details = details
                    if status in (HealthStatus.DOWN,
                                  HealthStatus.UNKNOWN):
                        check.consecutive_failures += 1
                        self._stats["total_failures"] += 1
                    else:
                        check.consecutive_failures = 0
                    self._stats["total_checks"] += 1

        duration_ms = int((time.perf_counter() - start) * 1000)
        ready = all(
            results[name][0] == HealthStatus.OK for name in results
        ) if results else False

        report = self._build_report(results, ready, duration_ms)
        with self._lock:
            self._stats["total_runs"] += 1
        self._record_report(report)
        return report

    # ========================================================
    # 便捷入口
    # ========================================================
    def liveness(self) -> HealthReport:
        """存活检查：只关心进程是否活着"""
        return self.run_sync(check_type="health")

    def readiness(self) -> HealthReport:
        """就绪检查：关心是否能接收流量"""
        return self.run_sync(check_type="ready")

    # ========================================================
    # 历史与状态
    # ========================================================
    def last_report(self) -> Optional[HealthReport]:
        with self._lock:
            return self._last_report

    def history(self, n: int = 20) -> List[HealthReport]:
        with self._lock:
            return list(self._history[-n:])

    def status(self) -> HealthStatus:
        with self._lock:
            return self._last_status

    def last_change_ms(self) -> int:
        with self._lock:
            return self._last_change_ms

    # ========================================================
    # 自身健康
    # ========================================================
    def health(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "status": "ok",
                "registered": len(self._checks),
                "last_status": self._last_status.value,
                "last_run_ms": (
                    self._last_report.timestamp_ms
                    if self._last_report else 0),
                "stats": dict(self._stats),
            }

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            return dict(self._stats)

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<HealthRegistry checks={len(self._checks)} "
                f"last={self._last_status.value}>"
            )


# ============================================================
# 全局入口
# ============================================================
_registry: Optional[HealthRegistry] = None
_registry_lock = threading.Lock()


def init_health(
    default_timeout_s: float = 2.0,
    event_publisher: Optional[Callable[[str, Any], None]] = None,
) -> HealthRegistry:
    """显式初始化（启动时调用）"""
    global _registry
    with _registry_lock:
        _registry = HealthRegistry(
            default_timeout_s=default_timeout_s)
        if event_publisher is not None:
            _registry.set_event_publisher(event_publisher)
    return _registry


def get_health() -> HealthRegistry:
    """获取单例（惰性初始化）"""
    global _registry
    if _registry is None:
        with _registry_lock:
            if _registry is None:
                _registry = HealthRegistry()
    return _registry


# ============================================================
# 便捷函数
# ============================================================
def register_health(
    name: str,
    fn: Callable[[], Any],
    critical: bool = False,
    **kwargs: Any,
) -> HealthCheck:
    return get_health().register(
        name, fn, critical=critical,
        check_type="health", **kwargs)


def register_ready(
    name: str,
    fn: Callable[[], Any],
    critical: bool = False,
    **kwargs: Any,
) -> HealthCheck:
    return get_health().register(
        name, fn, critical=critical,
        check_type="ready", **kwargs)


def health_check(
    name: Optional[str] = None,
    category: str = "general",
    critical: bool = False,
    check_type: str = "health",
    timeout_s: float = 2.0,
    ttl_s: float = 0.0,
) -> Callable[[Callable[[], Any]], Callable[[], Any]]:
    """
    装饰器：注册为健康检查函数。

    用法：
        @health_check(name="db", critical=True, timeout_s=1.0)
        def check_db():
            return {"status": "ok", "latency_ms": 3}
    """
    def decorator(fn: Callable[[], Any]) -> Callable[[], Any]:
        cb_name = name or getattr(fn, "__name__", "unnamed")
        get_health().register(
            name=cb_name,
            fn=fn,
            category=category,
            critical=critical,
            check_type=check_type,
            timeout_s=timeout_s,
            ttl_s=ttl_s,
        )
        return fn
    return decorator
