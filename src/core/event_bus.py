"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: event_bus
version: 2.0.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: 异步事件总线（优先级/背压/死信/重放/trace_id/单例）

provides:
  - class EventBus
  - class Event
  - class Subscription
  - class EventPriority
  - function get_event_bus
  - function init_event_bus
  - function publish
  - function subscribe

requires:
  modules: [core.config, core.logger, core.timestamp]
  libraries: []
  resources:
    memory_mb: 64
    cpu_cores: 1

health:
  fn: EventBus.health
  timeout_ms: 500

assembly:
  order: 13
  parallel: false

tests:
  - tests/test_event_bus.py

fallback:
  - 队列满 → 按优先级丢弃
  - handler 失败 → 死信队列
  - dispatch 线程崩溃 → 自动重启

tags: [core, event, async]
---UMS-END---"""

from __future__ import annotations

import asyncio
import fnmatch
import heapq
import queue
import threading
import time
import uuid
from collections import deque
from contextvars import ContextVar
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Any, Callable, Deque, Dict, List, Optional, Set

# 延迟导入避免循环依赖
_logger = None
_ts = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("event_bus")
        except Exception:
            import logging
            _logger = logging.getLogger("event_bus")
    return _logger


def _timestamp_ms() -> int:
    global _ts
    if _ts is None:
        try:
            from core.timestamp import now_ms
            _ts = now_ms
        except Exception:
            _ts = lambda: int(time.time() * 1000)
    return _ts()


# ============================================================
# trace_id 上下文
# ============================================================
_trace_var: ContextVar[str] = ContextVar("event_trace_id", default="-")


def current_trace_id() -> str:
    return _trace_var.get()


# ============================================================
# 优先级
# ============================================================
class EventPriority(IntEnum):
    CRITICAL = 0    # 风控、熔断、时间漂移
    HIGH = 1        # 订单、持仓
    NORMAL = 2      # 信号、指标
    LOW = 3         # 日志、分析


# ============================================================
# 事件
# ============================================================
@dataclass(order=True)
class Event:
    """事件对象。priority 参与排序，其他字段不参与。"""
    priority: int = EventPriority.NORMAL
    seq: int = field(default=0, compare=False)
    topic: str = field(default="", compare=False)
    data: Any = field(default=None, compare=False)
    ts_ms: int = field(default_factory=_timestamp_ms, compare=False)
    trace_id: str = field(default="-", compare=False)
    expire_ms: int = field(default=0, compare=False)   # 0 表示不过期
    source: str = field(default="-", compare=False)
    event_id: str = field(
        default_factory=lambda: uuid.uuid4().hex[:12],
        compare=False,
    )

    def is_expired(self) -> bool:
        if self.expire_ms <= 0:
            return False
        return _timestamp_ms() > self.ts_ms + self.expire_ms


# ============================================================
# 订阅句柄
# ============================================================
@dataclass
class Subscription:
    id: str
    pattern: str
    handler: Callable[[Event], Any]
    priority: int
    filter_fn: Optional[Callable[[Event], bool]] = None
    active: bool = True
    created_ms: int = field(default_factory=_timestamp_ms)
    # 统计
    invoked: int = 0
    failed: int = 0
    total_ms: int = 0
    max_ms: int = 0

    def cancel(self) -> None:
        self.active = False

    def avg_ms(self) -> float:
        return self.total_ms / self.invoked if self.invoked else 0.0


# ============================================================
# 死信
# ============================================================
@dataclass
class DeadLetter:
    event: Event
    subscription_id: str
    error: str
    failed_at_ms: int = field(default_factory=_timestamp_ms)


# ============================================================
# EventBus 单例
# ============================================================
class EventBus:
    """
    线程安全、异步分发、支持优先级、背压、死信、重放的事件总线。
    推荐在 asyncio 环境中运行；也支持纯线程环境。
    """

    _instance: Optional["EventBus"] = None
    _instance_lock = threading.Lock()

    # 队列水位
    HWM = 8000              # 高水位，触发背压
    LWM = 2000              # 低水位，恢复接收
    MAX_QUEUE = 10000
    RING_SIZE = 10000       # 重放缓冲
    DLQ_SIZE = 1000         # 死信上限
    STATS_WINDOW = 1000     # 耗时窗口

    def __new__(cls, *args: Any, **kwargs: Any) -> "EventBus":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self,
        async_loop: Optional[asyncio.AbstractEventLoop] = None,
        dispatcher_threads: int = 1,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._subs: Dict[str, List[Subscription]] = {}
            self._subs_by_id: Dict[str, Subscription] = {}
            self._queue: "queue.PriorityQueue[Event]" = queue.PriorityQueue(
                maxsize=self.MAX_QUEUE)
            self._seq = 0
            self._running = False
            self._paused = False
            self._dispatcher_threads: List[threading.Thread] = []
            self._n_threads = max(1, dispatcher_threads)
            self._loop = async_loop
            self._ring: Deque[Event] = deque(maxlen=self.RING_SIZE)
            self._dlq: Deque[DeadLetter] = deque(maxlen=self.DLQ_SIZE)
            self._stats = {
                "published": 0,
                "dispatched": 0,
                "dropped_low": 0,
                "dropped_full": 0,
                "expired": 0,
                "dlq": 0,
                "started_ms": _timestamp_ms(),
            }
            self._initialized = True

    # ========================================================
    # 生命周期
    # ========================================================
    def start(self) -> None:
        with self._lock:
            if self._running:
                return
            self._running = True
            for i in range(self._n_threads):
                t = threading.Thread(
                    target=self._dispatch_loop,
                    name=f"evbus-{i}",
                    daemon=True,
                )
                t.start()
                self._dispatcher_threads.append(t)
        _log().info(f"EventBus 已启动，{self._n_threads} 个分发线程")

    def stop(self, drain_timeout_s: float = 5.0) -> None:
        with self._lock:
            if not self._running:
                return
            self._running = False
        deadline = time.time() + drain_timeout_s
        while not self._queue.empty() and time.time() < deadline:
            time.sleep(0.01)
        for t in self._dispatcher_threads:
            t.join(timeout=1.0)
        self._dispatcher_threads.clear()
        remaining = self._queue.qsize()
        if remaining > 0:
            _log().warning(f"关闭时仍有 {remaining} 个事件未处理")
        _log().info("EventBus 已停止")

    # ========================================================
    # 发布
    # ========================================================
    def publish(
        self,
        topic: str,
        data: Any = None,
        priority: int = EventPriority.NORMAL,
        trace_id: Optional[str] = None,
        expire_ms: int = 0,
        source: str = "-",
    ) -> bool:
        if not topic:
            _log().warning("publish 事件 topic 为空，已忽略")
            return False

        event = Event(
            priority=int(priority),
            seq=self._next_seq(),
            topic=topic,
            data=data,
            ts_ms=_timestamp_ms(),
            trace_id=trace_id or current_trace_id(),
            expire_ms=int(expire_ms),
            source=source,
        )

        with self._lock:
            self._stats["published"] += 1
            self._ring.append(event)

        # 背压：超 HWM 时丢弃 LOW
        if self._queue.qsize() >= self.HWM:
            if priority >= EventPriority.LOW:
                with self._lock:
                    self._stats["dropped_low"] += 1
                _log().warning(
                    f"背压：丢弃 LOW 事件 {topic} "
                    f"(queue={self._queue.qsize()})")
                return False

        try:
            self._queue.put_nowait(event)
        except queue.Full:
            with self._lock:
                self._stats["dropped_full"] += 1
            _log().error(
                f"队列已满，丢弃事件 {topic} (priority={priority})")
            return False

        return True

    def publish_event(self, event: Event) -> bool:
        """直接发布已有 Event 对象"""
        if event.seq == 0:
            event.seq = self._next_seq()
        if event.trace_id == "-":
            event.trace_id = current_trace_id()
        with self._lock:
            self._stats["published"] += 1
            self._ring.append(event)
        try:
            self._queue.put_nowait(event)
            return True
        except queue.Full:
            with self._lock:
                self._stats["dropped_full"] += 1
            return False

    def _next_seq(self) -> int:
        with self._lock:
            self._seq += 1
            return self._seq

    # ========================================================
    # 订阅
    # ========================================================
    def subscribe(
        self,
        pattern: str,
        handler: Callable[[Event], Any],
        priority: int = 0,
        filter_fn: Optional[Callable[[Event], bool]] = None,
    ) -> Optional[Subscription]:
        if not pattern or not callable(handler):
            _log().warning("subscribe 参数无效")
            return None
        sub = Subscription(
            id=uuid.uuid4().hex[:10],
            pattern=pattern,
            handler=handler,
            priority=int(priority),
            filter_fn=filter_fn,
        )
        with self._lock:
            self._subs.setdefault(pattern, []).append(sub)
            self._subs_by_id[sub.id] = sub
        return sub

    def unsubscribe(self, sub: Subscription) -> None:
        if sub is None:
            return
        with self._lock:
            sub.active = False
            lst = self._subs.get(sub.pattern, [])
            if sub in lst:
                lst.remove(sub)
            if not lst:
                self._subs.pop(sub.pattern, None)
            self._subs_by_id.pop(sub.id, None)

    def clear(self) -> None:
        with self._lock:
            self._subs.clear()
            self._subs_by_id.clear()

    # ========================================================
    # 分发
    # ========================================================
    def _match(self, topic: str) -> List[Subscription]:
        matched: List[Subscription] = []
        with self._lock:
            for pattern, subs in self._subs.items():
                if fnmatch.fnmatchcase(topic, pattern) or \
                        pattern == "*":
                    matched.extend(subs)
        # 按优先级排序（小的先执行）
        matched.sort(key=lambda s: s.priority)
        return matched

    def _dispatch_loop(self) -> None:
        while True:
            with self._lock:
                if not self._running:
                    return
            try:
                event = self._queue.get(timeout=0.5)
            except queue.Empty:
                continue

            # 过期检查
            if event.is_expired():
                with self._lock:
                    self._stats["expired"] += 1
                continue

            # 设置上下文 trace_id
            token = _trace_var.set(event.trace_id)
            try:
                subs = self._match(event.topic)
                for sub in subs:
                    if not sub.active:
                        continue
                    self._invoke(sub, event)
                with self._lock:
                    self._stats["dispatched"] += 1
            finally:
                _trace_var.reset(token)

    def _invoke(self, sub: Subscription, event: Event) -> None:
        # 过滤器
        if sub.filter_fn is not None:
            try:
                if not sub.filter_fn(event):
                    return
            except Exception as exc:
                _log().warning(
                    f"过滤器异常 sub={sub.id}: {exc}")
                return

        start_ms = time.perf_counter() * 1000
        try:
            result = sub.handler(event)
            # 支持异步 handler
            if asyncio.iscoroutine(result):
                if self._loop is not None and self._loop.is_running():
                    asyncio.run_coroutine_threadsafe(result, self._loop)
                else:
                    # 无 loop，直接 run 到完成
                    try:
                        asyncio.run(result)
                    except RuntimeError:
                        # 已在 loop 中，创建 task
                        try:
                            asyncio.get_event_loop().create_task(result)
                        except Exception as exc:
                            raise RuntimeError(
                                f"异步 handler 无法执行: {exc}")
            sub.invoked += 1
        except Exception as exc:
            sub.failed += 1
            self._dlq_append(DeadLetter(
                event=event,
                subscription_id=sub.id,
                error=f"{type(exc).__name__}: {exc}",
            ))
            _log().error(
                f"handler 失败 sub={sub.id} "
                f"topic={event.topic}: {exc}")
        finally:
            elapsed = time.perf_counter() * 1000 - start_ms
            sub.total_ms += int(elapsed)
            if elapsed > sub.max_ms:
                sub.max_ms = int(elapsed)

    def _dlq_append(self, letter: DeadLetter) -> None:
        with self._lock:
            self._dlq.append(letter)
            self._stats["dlq"] += 1

    # ========================================================
    # 重放
    # ========================================================
    def replay(
        self,
        from_ms: int,
        to_ms: int,
        topic_pattern: str = "*",
        trace_id: Optional[str] = None,
    ) -> int:
        """重放 ring buffer 中的事件到订阅者（不进入队列）"""
        count = 0
        with self._lock:
            events = list(self._ring)
        for ev in events:
            if ev.ts_ms < from_ms or ev.ts_ms > to_ms:
                continue
            if not fnmatch.fnmatchcase(ev.topic, topic_pattern):
                continue
            token = _trace_var.set(trace_id or ev.trace_id)
            try:
                for sub in self._match(ev.topic):
                    if sub.active:
                        self._invoke(sub, ev)
                count += 1
            finally:
                _trace_var.reset(token)
        _log().info(f"重放 {count} 个事件")
        return count

    def recent(self, n: int = 20) -> List[Event]:
        with self._lock:
            return list(self._ring)[-n:]

    # ========================================================
    # 死信
    # ========================================================
    def dead_letters(self, n: int = 20) -> List[DeadLetter]:
        with self._lock:
            return list(self._dlq)[-n:]

    def clear_dead_letters(self) -> None:
        with self._lock:
            self._dlq.clear()

    # ========================================================
    # 状态
    # ========================================================
    def qsize(self) -> int:
        return self._queue.qsize()

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            subs_count = sum(len(v) for v in self._subs.values())
            return {
                **self._stats,
                "queue_size": self._queue.qsize(),
                "subscriptions": subs_count,
                "patterns": len(self._subs),
                "running": self._running,
                "ring_size": len(self._ring),
                "dlq_size": len(self._dlq),
            }

    def health(self) -> Dict[str, Any]:
        qs = self._queue.qsize()
        with self._lock:
            running = self._running
        if not running:
            status = "down"
        elif qs >= self.HWM:
            status = "degraded"
        else:
            status = "ok"
        return {
            "status": status,
            "queue_size": qs,
            "hwm": self.HWM,
            "subscriptions": sum(len(v) for v in self._subs.values()),
            "dispatchers": len(self._dispatcher_threads),
            "stats": dict(self._stats),
        }

    def slow_subscriptions(self, threshold_ms: float = 100.0,
                          top_n: int = 10) -> List[Dict[str, Any]]:
        with self._lock:
            subs = list(self._subs_by_id.values())
        subs.sort(key=lambda s: s.avg_ms(), reverse=True)
        result: List[Dict[str, Any]] = []
        for s in subs[:top_n]:
            if s.avg_ms() >= threshold_ms:
                result.append({
                    "id": s.id,
                    "pattern": s.pattern,
                    "invoked": s.invoked,
                    "failed": s.failed,
                    "avg_ms": round(s.avg_ms(), 2),
                    "max_ms": s.max_ms,
                })
        return result


# ============================================================
# 全局入口
# ============================================================
_bus: Optional[EventBus] = None
_bus_lock = threading.Lock()


def init_event_bus(
    async_loop: Optional[asyncio.AbstractEventLoop] = None,
    dispatcher_threads: int = 1,
    auto_start: bool = True,
) -> EventBus:
    global _bus
    with _bus_lock:
        _bus = EventBus(
            async_loop=async_loop,
            dispatcher_threads=dispatcher_threads,
        )
        if auto_start:
            _bus.start()
    return _bus


def get_event_bus() -> EventBus:
    global _bus
    if _bus is None:
        with _bus_lock:
            if _bus is None:
                _bus = EventBus()
                _bus.start()
    return _bus


def publish(topic: str, data: Any = None, **kwargs: Any) -> bool:
    return get_event_bus().publish(topic, data, **kwargs)


def subscribe(
    pattern: str,
    handler: Callable[[Event], Any],
    priority: int = 0,
    filter_fn: Optional[Callable[[Event], bool]] = None,
) -> Optional[Subscription]:
    return get_event_bus().subscribe(
        pattern, handler, priority, filter_fn)


def shutdown_event_bus(drain_timeout_s: float = 5.0) -> None:
    global _bus
    with _bus_lock:
        if _bus is not None:
            _bus.stop(drain_timeout_s)
            _bus = None
