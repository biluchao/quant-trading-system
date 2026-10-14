"""---UMS-BEGIN---
spec_version: 1.0
module: data
type: core
name: source_router
version: 2.0.0
level: 2
required: true
author: quant-team
created: 2026-10-14
brief: 数据源路由器（多级回退/健康探测/自动切换/一致性校验）

provides:
  - class SourceRouter
  - class DataSource
  - class SourceCategory
  - class RouterStats
  - class RouterConfig
  - function get_source_router
  - function init_source_router

requires:
  modules: [core.logger, core.timestamp, core.event_bus]
  libraries: []
  resources:
    memory_mb: 16
    cpu_cores: 1

health:
  fn: SourceRouter.health
  timeout_ms: 1000

assembly:
  order: 31
  parallel: false

tests:
  - tests/test_source_router.py

fallback:
  - 所有源失效 → 返回 None + 事件
  - 一致性校验失败 → 记录告警，使用主源
  - 健康探测失败 → 标记源为 down，切换

tags: [data, router, resilience]
---UMS-END---"""

from __future__ import annotations

import asyncio
import threading
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Awaitable, Callable, Dict, List, Optional, Tuple

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("source_router")
        except Exception:
            import logging
            _logger = logging.getLogger("source_router")
    return _logger


def _now_ms() -> int:
    try:
        from core.timestamp import now_ms
        return now_ms()
    except Exception:
        return int(time.time() * 1000)


# ============================================================
# 枚举与常量
# ============================================================
class SourceCategory(str, Enum):
    """数据源类别"""
    REALTIME = "realtime"    # WebSocket 实时
    REST = "rest"            # REST 补齐
    CACHE = "cache"          # 本地缓存
    REPLAY = "replay"        # 历史回放


DEFAULT_HEALTH_INTERVAL_S = 10.0
DEFAULT_CALL_TIMEOUT_S = 5.0
DEFAULT_MAX_FAILURES = 3          # 连续失败 N 次标记为 down
DEFAULT_RECOVERY_PROBES = 2       # 恢复需连续成功 N 次
DEFAULT_AUTO_RECOVER = True       # 主源恢复后是否自动切回


# ============================================================
# 数据源
# ============================================================
@dataclass
class DataSource:
    """注册的数据源"""
    name: str
    category: SourceCategory
    priority: int = 100                   # 数值越小优先级越高
    get_fn: Optional[Callable[..., Awaitable[Any]]] = None
    health_fn: Optional[Callable[[], Any]] = None
    timeout_s: float = DEFAULT_CALL_TIMEOUT_S
    enabled: bool = True
    # 运行时状态
    status: str = "unknown"               # ok / degraded / down / unknown
    consecutive_failures: int = 0
    consecutive_successes: int = 0
    last_ok_ms: int = 0
    last_error: str = ""
    total_calls: int = 0
    total_success: int = 0
    total_failed: int = 0

    def to_dict(self) -> Dict[str, Any]:
        return {
            "name": self.name,
            "category": self.category.value,
            "priority": self.priority,
            "enabled": self.enabled,
            "status": self.status,
            "consecutive_failures": self.consecutive_failures,
            "consecutive_successes": self.consecutive_successes,
            "last_ok_ms": self.last_ok_ms,
            "last_error": self.last_error,
            "total_calls": self.total_calls,
            "total_success": self.total_success,
            "total_failed": self.total_failed,
        }

    def success_rate(self) -> float:
        return (self.total_success / self.total_calls
                if self.total_calls else 0.0)


# ============================================================
# 配置与统计
# ============================================================
@dataclass
class RouterConfig:
    health_interval_s: float = DEFAULT_HEALTH_INTERVAL_S
    call_timeout_s: float = DEFAULT_CALL_TIMEOUT_S
    max_failures: int = DEFAULT_MAX_FAILURES
    recovery_probes: int = DEFAULT_RECOVERY_PROBES
    auto_recover: bool = DEFAULT_AUTO_RECOVER
    enable_consistency_check: bool = False
    consistency_threshold_pct: float = 0.001  # 0.1%


@dataclass
class RouterStats:
    total_requests: int = 0
    total_success: int = 0
    total_failed: int = 0
    total_fallbacks: int = 0
    switches: int = 0
    inconsistency_detected: int = 0
    started_ms: int = field(default_factory=_now_ms)

    def to_dict(self) -> Dict[str, Any]:
        return {k: getattr(self, k) for k in (
            "total_requests", "total_success", "total_failed",
            "total_fallbacks", "switches",
            "inconsistency_detected", "started_ms",
        )}

    def success_rate(self) -> float:
        return (self.total_success / self.total_requests
                if self.total_requests else 0.0)


# ============================================================
# SourceRouter 单例
# ============================================================
class SourceRouter:
    """
    数据源路由器。

    特性：
    - 优先级回退链（realtime → rest → cache）
    - 健康探测 + 自动切换
    - 主源恢复自动切回
    - 一致性校验（可选）
    - 全异步 + 超时保护
    - 事件发布
    """

    _instance: Optional["SourceRouter"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "SourceRouter":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self, config: Optional[RouterConfig] = None,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._async_lock: Optional[asyncio.Lock] = None
            self.config = config or RouterConfig()
            self._sources: Dict[str, DataSource] = {}
            self._active: Dict[SourceCategory, str] = {}  # category → name
            self._stats = RouterStats()
            self._bg_task: Optional[asyncio.Task] = None
            self._initialized = True

    # ========================================================
    # 注册
    # ========================================================
    def register(
        self,
        name: str,
        category: SourceCategory,
        get_fn: Callable[..., Awaitable[Any]],
        priority: int = 100,
        health_fn: Optional[Callable[[], Any]] = None,
        timeout_s: Optional[float] = None,
        enabled: bool = True,
    ) -> DataSource:
        if not name or not callable(get_fn):
            raise ValueError("name 和 get_fn 不能为空")
        src = DataSource(
            name=name,
            category=category,
            priority=priority,
            get_fn=get_fn,
            health_fn=health_fn,
            timeout_s=timeout_s if timeout_s is not None
            else self.config.call_timeout_s,
            enabled=enabled,
            status="unknown" if health_fn else "ok",
        )
        with self._lock:
            self._sources[name] = src
            self._reselect_category(category)
        _log().debug(
            f"注册数据源: {name} ({category.value}, "
            f"priority={priority})")
        return src

    def unregister(self, name: str) -> bool:
        with self._lock:
            src = self._sources.pop(name, None)
            if src is None:
                return False
            # 若该源是某类别的当前活动源，重新选择
            self._reselect_category(src.category)
            return True

    def enable(self, name: str) -> None:
        with self._lock:
            if name in self._sources:
                self._sources[name].enabled = True
                self._reselect_category(
                    self._sources[name].category)

    def disable(self, name: str) -> None:
        with self._lock:
            if name in self._sources:
                self._sources[name].enabled = False
                self._reselect_category(
                    self._sources[name].category)

    # ========================================================
    # 选择
    # ========================================================
    def _reselect_category(self, category: SourceCategory) -> None:
        """按优先级+状态选择该类别的活动源"""
        candidates = [
            s for s in self._sources.values()
            if s.category == category and s.enabled
        ]
        if not candidates:
            self._active.pop(category, None)
            return

        # 优先选择状态 ok 的，其次 degraded，最后 unknown
        def sort_key(s: DataSource) -> Tuple[int, int]:
            status_rank = {
                "ok": 0, "unknown": 1, "degraded": 2, "down": 3,
            }.get(s.status, 4)
            return (status_rank, s.priority)

        candidates.sort(key=sort_key)
        chosen = candidates[0]
        old = self._active.get(category)
        self._active[category] = chosen.name
        if old != chosen.name:
            _log().info(
                f"数据源切换 [{category.value}]: "
                f"{old or 'none'} → {chosen.name}")
            self._publish("data.source_switched", {
                "category": category.value,
                "from": old,
                "to": chosen.name,
                "ts_ms": _now_ms(),
            })
            with self._lock:
                self._stats.switches += 1

    def active(self, category: SourceCategory) -> Optional[str]:
        with self._lock:
            return self._active.get(category)

    def source(self, name: str) -> Optional[DataSource]:
        with self._lock:
            return self._sources.get(name)

    # ========================================================
    # 获取 K 线（主入口）
    # ========================================================
    async def get_candle(
        self,
        symbol: str,
        interval: str,
        open_time: int,
        category: Optional[SourceCategory] = None,
    ) -> Optional[Dict[str, Any]]:
        """
        按优先级尝试多个源，返回第一个成功的 K 线。
        """
        with self._lock:
            self._stats.total_requests += 1

        # 选择候选源
        if category is not None:
            order = self._candidates_by_category(category)
        else:
            order = self._candidates_all()

        if not order:
            _log().warning("无可用数据源")
            with self._lock:
                self._stats.total_failed += 1
            return None

        last_error = ""
        for i, src in enumerate(order):
            try:
                result = await self._call_source(
                    src, symbol, interval, open_time)
                if result is not None:
                    if i > 0:
                        with self._lock:
                            self._stats.total_fallbacks += 1
                        _log().info(
                            f"数据源回退成功: {src.name} "
                            f"(第 {i + 1} 个候选)")
                    with self._lock:
                        self._stats.total_success += 1
                    self._mark_success(src)
                    return result
                else:
                    last_error = f"{src.name}: empty result"
                    self._mark_failure(src, last_error)
            except asyncio.TimeoutError:
                last_error = f"{src.name}: timeout"
                self._mark_failure(src, last_error)
                _log().debug(f"数据源超时: {src.name}")
            except Exception as exc:
                last_error = f"{src.name}: {type(exc).__name__}: {exc}"
                self._mark_failure(src, last_error)
                _log().debug(f"数据源失败: {last_error}")

        with self._lock:
            self._stats.total_failed += 1
        _log().warning(
            f"所有数据源失败 {symbol}/{interval}: {last_error}")
        self._publish("data.source_all_failed", {
            "symbol": symbol,
            "interval": interval,
            "open_time": open_time,
            "last_error": last_error,
            "ts_ms": _now_ms(),
        })
        return None

    def _candidates_by_category(
        self, category: SourceCategory,
    ) -> List[DataSource]:
        with self._lock:
            sources = [
                s for s in self._sources.values()
                if s.category == category and s.enabled
                and s.status != "down"
            ]
            # 若活动源在列表中，置于最前
            active_name = self._active.get(category)
            sources.sort(key=lambda s: (
                0 if s.name == active_name else 1,
                s.priority,
            ))
            return sources

    def _candidates_all(self) -> List[DataSource]:
        """按类别优先级排列：realtime → rest → cache → replay"""
        category_order = [
            SourceCategory.REALTIME,
            SourceCategory.REST,
            SourceCategory.CACHE,
            SourceCategory.REPLAY,
        ]
        result: List[DataSource] = []
        seen: set = set()
        for cat in category_order:
            for s in self._candidates_by_category(cat):
                if s.name not in seen:
                    result.append(s)
                    seen.add(s.name)
        return result

    async def _call_source(
        self,
        src: DataSource,
        symbol: str,
        interval: str,
        open_time: int,
    ) -> Optional[Dict[str, Any]]:
        with self._lock:
            src.total_calls += 1
        result = await asyncio.wait_for(
            src.get_fn(symbol, interval, open_time),
            timeout=src.timeout_s,
        )
        return result

    # ========================================================
    # 状态标记
    # ========================================================
    def _mark_success(self, src: DataSource) -> None:
        with self._lock:
            src.total_success += 1
            src.consecutive_successes += 1
            src.consecutive_failures = 0
            src.last_ok_ms = _now_ms()
            src.last_error = ""
            # 恢复判断
            if src.status in ("down", "degraded") and \
                    src.consecutive_successes >= \
                    self.config.recovery_probes:
                old_status = src.status
                src.status = "ok"
                _log().info(
                    f"数据源 {src.name} 已恢复: "
                    f"{old_status} → ok")
                if self.config.auto_recover:
                    self._reselect_category(src.category)

    def _mark_failure(self, src: DataSource, error: str) -> None:
        with self._lock:
            src.total_failed += 1
            src.consecutive_failures += 1
            src.consecutive_successes = 0
            src.last_error = error
            if src.consecutive_failures >= self.config.max_failures:
                old_status = src.status
                src.status = "down"
                if old_status != "down":
                    _log().warning(
                        f"数据源 {src.name} 标记为 down: {error}")
                    self._publish("data.source_down", {
                        "name": src.name,
                        "category": src.category.value,
                        "error": error,
                        "ts_ms": _now_ms(),
                    })
                    self._reselect_category(src.category)
            elif src.consecutive_failures >= 2:
                src.status = "degraded"

    # ========================================================
    # 健康探测
    # ========================================================
    async def probe_health(self) -> Dict[str, str]:
        """主动探测所有源的 health_fn"""
        results: Dict[str, str] = {}
        with self._lock:
            sources = list(self._sources.values())

        for src in sources:
            if not src.enabled:
                results[src.name] = "disabled"
                continue
            if src.health_fn is None:
                results[src.name] = src.status
                continue
            try:
                fn_result = src.health_fn()
                if asyncio.iscoroutine(fn_result):
                    fn_result = await asyncio.wait_for(
                        fn_result, timeout=src.timeout_s)
                # 支持 bool / str / dict
                if isinstance(fn_result, bool):
                    ok = fn_result
                    status = "ok" if ok else "down"
                elif isinstance(fn_result, str):
                    status = fn_result
                    ok = status in ("ok", "healthy")
                elif isinstance(fn_result, dict):
                    status = str(fn_result.get("status", "unknown"))
                    ok = status in ("ok", "healthy", "degraded")
                else:
                    ok = True
                    status = "unknown"
                results[src.name] = status
                if ok:
                    self._mark_success(src)
                else:
                    self._mark_failure(src, f"probe:{status}")
            except Exception as exc:
                results[src.name] = "down"
                self._mark_failure(src, f"probe_error:{exc}")

        return results

    async def run_health_loop(self) -> None:
        """后台定时探测"""
        _log().info("SourceRouter 健康探测循环启动")
        try:
            while True:
                await asyncio.sleep(self.config.health_interval_s)
                try:
                    await self.probe_health()
                except Exception as exc:
                    _log().debug(f"健康探测异常: {exc}")
        except asyncio.CancelledError:
            _log().info("SourceRouter 健康探测循环取消")
            raise

    def start_health_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        if self._bg_task is not None and not self._bg_task.done():
            return
        self._bg_task = loop.create_task(self.run_health_loop())

    def stop_health_loop(self) -> None:
        if self._bg_task is not None and not self._bg_task.done():
            self._bg_task.cancel()
        self._bg_task = None

    # ========================================================
    # 一致性校验
    # ========================================================
    async def get_candle_verified(
        self,
        symbol: str,
        interval: str,
        open_time: int,
    ) -> Optional[Dict[str, Any]]:
        """
        从多个源获取并对比，返回多数一致的结果。
        仅在 enable_consistency_check=True 时有效。
        """
        if not self.config.enable_consistency_check:
            return await self.get_candle(
                symbol, interval, open_time)

        candidates = self._candidates_all()[:2]
        if len(candidates) < 2:
            return await self.get_candle(
                symbol, interval, open_time)

        results = await asyncio.gather(
            *[self._call_source(s, symbol, interval, open_time)
              for s in candidates],
            return_exceptions=True,
        )

        values: List[Tuple[DataSource, Dict[str, Any]]] = []
        for src, r in zip(candidates, results):
            if isinstance(r, Exception) or r is None:
                continue
            values.append((src, r))

        if not values:
            return None

        if len(values) == 1:
            return values[0][1]

        # 对比 close 价格
        closes = [float(v[1].get("close", 0)) for v in values]
        max_close = max(closes)
        min_close = min(closes)
        if max_close > 0 and \
                (max_close - min_close) / max_close > \
                self.config.consistency_threshold_pct:
            with self._lock:
                self._stats.inconsistency_detected += 1
            _log().warning(
                f"跨源不一致: {symbol}/{interval} "
                f"closes={closes}")
            self._publish("data.inconsistency", {
                "symbol": symbol,
                "interval": interval,
                "values": [
                    {"source": s.name, "close": c}
                    for (s, _), c in zip(values, closes)
                ],
                "ts_ms": _now_ms(),
            })

        # 返回高优先级的
        values.sort(key=lambda v: v[0].priority)
        return values[0][1]

    # ========================================================
    # 事件
    # ========================================================
    def _publish(self, topic: str, data: Any) -> None:
        try:
            from core.event_bus import publish
            publish(topic, data)
        except Exception:
            _log().debug(f"[{topic}] {data}")

    # ========================================================
    # 状态
    # ========================================================
    def list_sources(self) -> List[Dict[str, Any]]:
        with self._lock:
            return [s.to_dict() for s in self._sources.values()]

    def active_map(self) -> Dict[str, Optional[str]]:
        with self._lock:
            return {c.value: self._active.get(c)
                    for c in SourceCategory}

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            s = self._stats.to_dict()
            s["success_rate"] = round(
                self._stats.success_rate(), 4)
            s["sources"] = len(self._sources)
            s["active"] = self.active_map()
            return s

    def health(self) -> Dict[str, Any]:
        with self._lock:
            total = self._stats.total_requests
            failed = self._stats.total_failed
            sources_down = sum(
                1 for s in self._sources.values()
                if s.status == "down")
            sources_ok = sum(
                1 for s in self._sources.values()
                if s.status == "ok")
        if total == 0:
            status = "ok"
        elif sources_ok == 0 and sources_down > 0:
            status = "down"
        elif failed / total > 0.3:
            status = "degraded"
        else:
            status = "ok"
        return {
            "status": status,
            "total_requests": total,
            "sources_ok": sources_ok,
            "sources_down": sources_down,
            "active": self.active_map(),
            "stats": dict(self._stats.to_dict()),
        }

    def reset_stats(self) -> None:
        with self._lock:
            self._stats = RouterStats()

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<SourceRouter sources={len(self._sources)} "
                f"requests={self._stats.total_requests} "
                f"fallbacks={self._stats.total_fallbacks}>"
            )


# ============================================================
# 全局入口
# ============================================================
_router_instance: Optional[SourceRouter] = None
_router_lock = threading.Lock()


def init_source_router(
    config: Optional[RouterConfig] = None,
) -> SourceRouter:
    """显式初始化（启动时调用）"""
    global _router_instance
    with _router_lock:
        _router_instance = SourceRouter(config)
    return _router_instance


def get_source_router() -> SourceRouter:
    """获取单例（惰性初始化）"""
    global _router_instance
    if _router_instance is None:
        with _router_lock:
            if _router_instance is None:
                _router_instance = SourceRouter()
    return _router_instance
