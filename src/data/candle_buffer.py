"""---UMS-BEGIN---
spec_version: 1.0
module: data
type: core
name: candle_buffer
version: 2.0.0
level: 2
required: true
author: quant-team
created: 2026-10-14
brief: K线缓冲区（多符号/多周期/缺口检测/事件发布/线程安全）

provides:
  - class Candle
  - class CandleBuffer
  - class BufferStats
  - function get_candle_buffer
  - function init_candle_buffer

requires:
  modules: [core.logger, core.timestamp, core.event_bus]
  libraries: []
  resources:
    memory_mb: 32
    cpu_cores: 1

health:
  fn: CandleBuffer.health
  timeout_ms: 500

assembly:
  order: 27
  parallel: false

tests:
  - tests/test_candle_buffer.py

fallback:
  - 缺口检测失败 → 记录并继续
  - event_bus 缺失 → 本地日志
  - 内存超限 → 自动缩减 max_size

tags: [data, buffer, candle]
---UMS-END---"""

from __future__ import annotations

import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Callable, Deque, Dict, List, Optional, Tuple

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("candle_buffer")
        except Exception:
            import logging
            _logger = logging.getLogger("candle_buffer")
    return _logger


def _now_ms() -> int:
    try:
        from core.timestamp import now_ms
        return now_ms()
    except Exception:
        return int(time.time() * 1000)


# ============================================================
# 周期常量（毫秒）
# ============================================================
INTERVAL_MS: Dict[str, int] = {
    "1m": 60_000,
    "3m": 180_000,
    "5m": 300_000,
    "15m": 900_000,
    "30m": 1_800_000,
    "1h": 3_600_000,
    "4h": 14_400_000,
    "1d": 86_400_000,
}


# ============================================================
# 数据结构
# ============================================================
@dataclass(frozen=True)
class Candle:
    """标准 K 线结构（不可变）"""
    symbol: str
    interval: str
    open_time: int
    close_time: int
    open: float
    high: float
    low: float
    close: float
    volume: float
    quote_volume: float = 0.0
    trades: int = 0
    is_closed: bool = False

    def to_dict(self) -> Dict[str, Any]:
        return {
            "symbol": self.symbol,
            "interval": self.interval,
            "open_time": self.open_time,
            "close_time": self.close_time,
            "open": self.open,
            "high": self.high,
            "low": self.low,
            "close": self.close,
            "volume": self.volume,
            "quote_volume": self.quote_volume,
            "trades": self.trades,
            "is_closed": self.is_closed,
        }

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> "Candle":
        return cls(
            symbol=str(d.get("symbol", "")).lower(),
            interval=str(d.get("interval", "")),
            open_time=int(d.get("open_time", 0)),
            close_time=int(d.get("close_time", 0)),
            open=float(d.get("open", 0)),
            high=float(d.get("high", 0)),
            low=float(d.get("low", 0)),
            close=float(d.get("close", 0)),
            volume=float(d.get("volume", 0)),
            quote_volume=float(d.get("quote_volume", 0)),
            trades=int(d.get("trades", 0)),
            is_closed=bool(d.get("is_closed", False)),
        )

    def is_valid(self) -> bool:
        return (
            self.high >= self.low
            and self.high >= self.open
            and self.high >= self.close
            and self.low <= self.open
            and self.low <= self.close
            and self.volume >= 0
            and self.open_time > 0
        )


@dataclass
class BufferStats:
    received: int = 0
    duplicates: int = 0
    out_of_order: int = 0
    gaps: int = 0
    gap_bars_total: int = 0
    closed: int = 0
    invalid: int = 0
    started_ms: int = field(default_factory=_now_ms)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "received": self.received,
            "duplicates": self.duplicates,
            "out_of_order": self.out_of_order,
            "gaps": self.gaps,
            "gap_bars_total": self.gap_bars_total,
            "closed": self.closed,
            "invalid": self.invalid,
            "started_ms": self.started_ms,
        }


# ============================================================
# 单个序列（symbol + interval）
# ============================================================
class _CandleSeries:
    """单个 (symbol, interval) 的 K 线序列"""

    def __init__(self, max_size: int) -> None:
        self._lock = threading.RLock()
        self._buffer: Deque[Candle] = deque(maxlen=max_size)
        self._index: Dict[int, Candle] = {}   # open_time → Candle
        self._max_size = max_size
        self._last_open_time: int = 0

    def push(
        self, candle: Candle,
    ) -> Tuple[str, Optional[Tuple[int, int]]]:
        """
        返回 (状态, 缺口区间)。
        状态：added / updated / duplicate / out_of_order / invalid
        缺口区间：(missing_start, missing_end)，None 表示无缺口
        """
        if not candle.is_valid():
            return "invalid", None

        with self._lock:
            # 重复：更新现有（同一根 K 线的 tick 更新）
            if candle.open_time in self._index:
                old = self._index[candle.open_time]
                # 若旧的是 closed，新的非 closed，忽略
                if old.is_closed and not candle.is_closed:
                    return "duplicate", None
                self._index[candle.open_time] = candle
                # 更新 deque 中的对应项
                for i in range(len(self._buffer) - 1, -1, -1):
                    if self._buffer[i].open_time == candle.open_time:
                        self._buffer[i] = candle
                        break
                return "updated", None

            # 乱序：比最后的 open_time 早
            if self._last_open_time > 0 and \
                    candle.open_time < self._last_open_time:
                return "out_of_order", None

            # 缺口检测
            gap: Optional[Tuple[int, int]] = None
            if self._last_open_time > 0:
                expected = self._last_open_time + \
                    INTERVAL_MS.get(candle.interval, 0)
                if candle.open_time > expected and expected > 0:
                    gap = (expected, candle.open_time)

            # 加入
            self._buffer.append(candle)
            self._index[candle.open_time] = candle
            self._last_open_time = candle.open_time

            # 溢出清理索引
            if len(self._buffer) == self._max_size:
                # deque 自动弹出最早项，需要同步删除索引
                pass  # 稍后清理

            # 索引清理：只保留在 buffer 中的
            if len(self._index) > self._max_size * 2:
                valid_times = {c.open_time for c in self._buffer}
                self._index = {
                    t: c for t, c in self._index.items()
                    if t in valid_times
                }

            return "added", gap

    def latest(self, n: int = 1) -> List[Candle]:
        with self._lock:
            if n <= 0:
                return []
            size = len(self._buffer)
            if size == 0:
                return []
            n = min(n, size)
            return list(self._buffer)[-n:]

    def latest_price(self) -> Optional[float]:
        with self._lock:
            if not self._buffer:
                return None
            return self._buffer[-1].close

    def latest_candle(self) -> Optional[Candle]:
        with self._lock:
            if not self._buffer:
                return None
            return self._buffer[-1]

    def range(self, start_ms: int, end_ms: int) -> List[Candle]:
        with self._lock:
            return [
                c for c in self._buffer
                if start_ms <= c.open_time <= end_ms
            ]

    def size(self) -> int:
        with self._lock:
            return len(self._buffer)

    def clear(self) -> None:
        with self._lock:
            self._buffer.clear()
            self._index.clear()
            self._last_open_time = 0


# ============================================================
# CandleBuffer 单例
# ============================================================
class CandleBuffer:
    """
    多符号、多周期 K 线缓冲区。

    特性：
    - 每个 (symbol, interval) 独立序列
    - 缺口自动检测并发布 `data.gap`
    - 重复/乱序/非法过滤
    - 收盘事件发布 `candle.closed`
    - 线程安全 + 单例
    - 时间范围查询
    """

    _instance: Optional["CandleBuffer"] = None
    _instance_lock = threading.Lock()

    DEFAULT_MAX_SIZE = 2000

    def __new__(cls, *args: Any, **kwargs: Any) -> "CandleBuffer":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self,
        max_size: int = DEFAULT_MAX_SIZE,
        auto_subscribe: bool = True,
        enable_gap_check: bool = True,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._series: Dict[Tuple[str, str], _CandleSeries] = {}
            self._max_size = max_size
            self._stats = BufferStats()
            self._sub: Any = None
            self._enable_gap_check = enable_gap_check
            self._gap_listeners: List[
                Callable[[str, str, int, int], None]] = []
            self._initialized = True

            if auto_subscribe:
                self._auto_subscribe()

    # ========================================================
    # 与事件总线集成
    # ========================================================
    def _auto_subscribe(self) -> None:
        """订阅 candle.closed / candle.tick 事件"""
        try:
            from core.event_bus import subscribe
            self._sub = subscribe("candle.*", self._on_candle_event)
            _log().debug("CandleBuffer 已订阅 candle.* 事件")
        except Exception as exc:
            _log().warning(f"订阅事件失败: {exc}")

    def _on_candle_event(self, event: Any) -> None:
        try:
            data = getattr(event, "data", None) or {}
            if isinstance(data, dict):
                self.push_dict(data)
        except Exception as exc:
            _log().debug(f"事件处理失败: {exc}")

    # ========================================================
    # 写入
    # ========================================================
    def push(self, candle: Candle) -> bool:
        """写入单个 K 线"""
        return self.push_dict(candle.to_dict())

    def push_dict(self, data: Dict[str, Any]) -> bool:
        """从 dict 写入"""
        try:
            candle = Candle.from_dict(data)
        except Exception as exc:
            with self._lock:
                self._stats.invalid += 1
            _log().debug(f"K线解析失败: {exc}")
            return False
        return self._push_candle(candle)

    def _push_candle(self, candle: Candle) -> bool:
        if not candle.symbol or not candle.interval:
            with self._lock:
                self._stats.invalid += 1
            return False

        key = (candle.symbol.lower(), candle.interval)
        with self._lock:
            series = self._series.get(key)
            if series is None:
                series = _CandleSeries(self._max_size)
                self._series[key] = series

        status, gap = series.push(candle)

        with self._lock:
            self._stats.received += 1
            if status == "invalid":
                self._stats.invalid += 1
                return False
            elif status == "duplicate":
                self._stats.duplicates += 1
                return False
            elif status == "out_of_order":
                self._stats.out_of_order += 1
                _log().debug(
                    f"乱序 K 线: {candle.symbol} "
                    f"open_time={candle.open_time}")
                return False
            elif status == "updated":
                # tick 更新
                return True

            # added
            if candle.is_closed:
                self._stats.closed += 1

            if gap is not None and self._enable_gap_check:
                self._stats.gaps += 1
                ms = INTERVAL_MS.get(candle.interval, 0)
                bars = (gap[1] - gap[0]) // ms if ms > 0 else 0
                self._stats.gap_bars_total += bars

        # 缺口处理
        if gap is not None and self._enable_gap_check:
            self._notify_gap(candle.symbol, candle.interval,
                             gap[0], gap[1])

        # 收盘事件
        if candle.is_closed:
            self._publish_closed(candle)

        return True

    # ========================================================
    # 缺口
    # ========================================================
    def add_gap_listener(
        self, fn: Callable[[str, str, int, int], None],
    ) -> None:
        """注册缺口监听：fn(symbol, interval, missing_start, missing_end)"""
        with self._lock:
            if fn not in self._gap_listeners:
                self._gap_listeners.append(fn)

    def _notify_gap(
        self, symbol: str, interval: str,
        start_ms: int, end_ms: int,
    ) -> None:
        _log().warning(
            f"检测到 K 线缺口: {symbol}/{interval} "
            f"[{start_ms} → {end_ms}]")
        # 发布事件
        try:
            from core.event_bus import publish
            publish("data.gap", {
                "symbol": symbol,
                "interval": interval,
                "missing_start": start_ms,
                "missing_end": end_ms,
                "ts_ms": _now_ms(),
            })
        except Exception:
            pass
        # 通知监听器
        for fn in list(self._gap_listeners):
            try:
                fn(symbol, interval, start_ms, end_ms)
            except Exception as exc:
                _log().debug(f"缺口监听失败: {exc}")

    # ========================================================
    # 收盘事件
    # ========================================================
    def _publish_closed(self, candle: Candle) -> None:
        try:
            from core.event_bus import publish
            publish("candle.closed", candle.to_dict())
        except Exception:
            pass

    # ========================================================
    # 读取
    # ========================================================
    def _get_series(
        self, symbol: str, interval: str,
    ) -> Optional[_CandleSeries]:
        with self._lock:
            return self._series.get(
                (symbol.lower(), interval))

    def latest(
        self, symbol: str, interval: str, n: int = 1,
    ) -> List[Candle]:
        series = self._get_series(symbol, interval)
        if series is None:
            return []
        return series.latest(n)

    def latest_candle(
        self, symbol: str, interval: str,
    ) -> Optional[Candle]:
        series = self._get_series(symbol, interval)
        if series is None:
            return None
        return series.latest_candle()

    def latest_price(
        self, symbol: str, interval: str,
    ) -> Optional[float]:
        series = self._get_series(symbol, interval)
        if series is None:
            return None
        return series.latest_price()

    def range(
        self, symbol: str, interval: str,
        start_ms: int, end_ms: int,
    ) -> List[Candle]:
        series = self._get_series(symbol, interval)
        if series is None:
            return []
        return series.range(start_ms, end_ms)

    def size(self, symbol: str, interval: str) -> int:
        series = self._get_series(symbol, interval)
        if series is None:
            return 0
        return series.size()

    def symbols(self) -> List[Tuple[str, str]]:
        with self._lock:
            return list(self._series.keys())

    # ========================================================
    # 兼容旧接口
    # ========================================================
    def get_latest(self, n: int = 100,
                   symbol: Optional[str] = None,
                   interval: str = "3m") -> List[Candle]:
        if symbol is None:
            # 返回第一个序列
            with self._lock:
                if not self._series:
                    return []
                first_key = next(iter(self._series))
            symbol, interval = first_key
        return self.latest(symbol, interval, n)

    # ========================================================
    # 清理
    # ========================================================
    def clear(
        self,
        symbol: Optional[str] = None,
        interval: Optional[str] = None,
    ) -> None:
        with self._lock:
            if symbol and interval:
                key = (symbol.lower(), interval)
                if key in self._series:
                    self._series[key].clear()
            else:
                for s in self._series.values():
                    s.clear()
                if not symbol and not interval:
                    self._series.clear()

    # ========================================================
    # 统计与健康
    # ========================================================
    def stats(self) -> Dict[str, Any]:
        with self._lock:
            series_info = {
                f"{s}/{i}": s.size()
                for (s, i), s in self._series.items()
            }
            return {
                **self._stats.to_dict(),
                "series_count": len(self._series),
                "series_sizes": series_info,
                "max_size": self._max_size,
            }

    def health(self) -> Dict[str, Any]:
        with self._lock:
            series_count = len(self._series)
            total_candles = sum(
                s.size() for s in self._series.values())
            gap_ratio = (
                self._stats.gap_bars_total /
                max(self._stats.received, 1)
            )
        if series_count == 0:
            status = "down"
        elif gap_ratio > 0.1:
            status = "degraded"
        elif self._stats.out_of_order > 100:
            status = "degraded"
        else:
            status = "ok"
        return {
            "status": status,
            "series_count": series_count,
            "total_candles": total_candles,
            "stats": dict(self._stats.to_dict()),
            "gap_ratio": round(gap_ratio, 4),
            "subscribed": self._sub is not None,
        }

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<CandleBuffer series={len(self._series)} "
                f"received={self._stats.received} "
                f"gaps={self._stats.gaps}>"
            )


# ============================================================
# 全局入口
# ============================================================
_buffer_instance: Optional[CandleBuffer] = None
_buffer_lock = threading.Lock()


def init_candle_buffer(
    max_size: int = CandleBuffer.DEFAULT_MAX_SIZE,
    auto_subscribe: bool = True,
) -> CandleBuffer:
    """显式初始化（启动时调用）"""
    global _buffer_instance
    with _buffer_lock:
        _buffer_instance = CandleBuffer(
            max_size=max_size, auto_subscribe=auto_subscribe)
    return _buffer_instance


def get_candle_buffer() -> CandleBuffer:
    """获取单例（惰性初始化）"""
    global _buffer_instance
    if _buffer_instance is None:
        with _buffer_lock:
            if _buffer_instance is None:
                _buffer_instance = CandleBuffer()
    return _buffer_instance
