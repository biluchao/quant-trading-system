"""---UMS-BEGIN---
spec_version: 1.0
module: data
type: core
name: gap_filler
version: 2.0.0
level: 2
required: true
author: quant-team
created: 2026-10-14
brief: K线缺口补齐（分片/去重/幂等/限流/事件/可取消）

provides:
  - class GapFiller
  - class GapRange
  - class FillResult
  - class GapStats
  - function get_gap_filler
  - function init_gap_filler

requires:
  modules:
    - data.binance_rest
    - data.candle_buffer
    - data.cleaner
    - core.logger
    - core.timestamp
    - core.event_bus
  libraries: []
  resources:
    memory_mb: 32
    cpu_cores: 1
    network: fapi.binance.com:443

health:
  fn: GapFiller.health
  timeout_ms: 2000

assembly:
  order: 29
  parallel: false

tests:
  - tests/test_gap_filler.py

fallback:
  - REST 不可用 → 挂起到 pending 队列，定时重试
  - 缺口过大 → 分片请求
  - 补回数据非法 → 丢弃并记录
  - 连续失败 → 发布 data.gap_unfilled 事件

tags: [data, gap, recovery]
---UMS-END---"""

from __future__ import annotations

import asyncio
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Tuple

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("gap_filler")
        except Exception:
            import logging
            _logger = logging.getLogger("gap_filler")
    return _logger


def _now_ms() -> int:
    try:
        from core.timestamp import now_ms
        return now_ms()
    except Exception:
        return int(time.time() * 1000)


# ============================================================
# 常量
# ============================================================
INTERVAL_MS: Dict[str, int] = {
    "1m": 60_000,
    "3m": 180_000,
    "5m": 300_000,
    "15m": 900_000,
    "30m": 1_800_000,
    "1h": 3_600_000,
}

# 单次分片最大 K 线数（币安单次返回上限 1500）
MAX_BARS_PER_REQUEST = 1000
# 单次补齐的最大 K 线总数（防止内存爆炸）
DEFAULT_MAX_GAP_BARS = 2000
# 未补成功缺口的重试间隔（秒）
PENDING_RETRY_INTERVAL_S = 60.0
# 最大挂起缺口数量
MAX_PENDING = 100


# ============================================================
# 数据结构
# ============================================================
@dataclass
class GapRange:
    """一个缺口区间（毫秒）"""
    symbol: str
    interval: str
    start_ms: int
    end_ms: int
    created_ms: int = field(default_factory=_now_ms)
    attempts: int = 0
    last_error: str = ""

    @property
    def bars(self) -> int:
        ms = INTERVAL_MS.get(self.interval, 0)
        if ms <= 0 or self.end_ms <= self.start_ms:
            return 0
        return (self.end_ms - self.start_ms) // ms

    def to_dict(self) -> Dict[str, Any]:
        return {
            "symbol": self.symbol,
            "interval": self.interval,
            "start_ms": self.start_ms,
            "end_ms": self.end_ms,
            "bars": self.bars,
            "attempts": self.attempts,
            "last_error": self.last_error,
        }


@dataclass
class FillResult:
    """一次补齐操作的结果"""
    gap: GapRange
    filled: int = 0
    skipped: int = 0
    rejected: int = 0
    success: bool = False
    error: str = ""
    duration_ms: int = 0

    def to_dict(self) -> Dict[str, Any]:
        return {
            "gap": self.gap.to_dict(),
            "filled": self.filled,
            "skipped": self.skipped,
            "rejected": self.rejected,
            "success": self.success,
            "error": self.error,
            "duration_ms": self.duration_ms,
        }


@dataclass
class GapStats:
    total_gaps: int = 0
    filled_gaps: int = 0
    failed_gaps: int = 0
    total_bars_filled: int = 0
    total_bars_skipped: int = 0
    total_bars_rejected: int = 0
    pending_gaps: int = 0
    retry_count: int = 0
    started_ms: int = field(default_factory=_now_ms)

    def to_dict(self) -> Dict[str, Any]:
        return {k: getattr(self, k) for k in (
            "total_gaps", "filled_gaps", "failed_gaps",
            "total_bars_filled", "total_bars_skipped",
            "total_bars_rejected", "pending_gaps",
            "retry_count", "started_ms",
        )}

    def success_rate(self) -> float:
        return (self.filled_gaps / self.total_gaps
                if self.total_gaps else 0.0)


# ============================================================
# GapFiller 单例
# ============================================================
class GapFiller:
    """
    K 线缺口补齐器。

    特性：
    - 自动分片（单次 ≤ MAX_BARS_PER_REQUEST）
    - 与 candle_buffer 联动去重（幂等）
    - 补齐数据经过 cleaner 校验
    - 缺口合并与挂起重试
    - 全异步 + 可取消
    - 事件发布：data.filling / data.filled / data.gap_unfilled
    """

    _instance: Optional["GapFiller"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "GapFiller":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self,
        max_gap_bars: int = DEFAULT_MAX_GAP_BARS,
        pending_retry_s: float = PENDING_RETRY_INTERVAL_S,
        auto_subscribe: bool = True,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._async_lock: Optional[asyncio.Lock] = None
            self._max_gap_bars = max_gap_bars
            self._pending_retry_s = pending_retry_s
            self._stats = GapStats()
            self._pending: List[GapRange] = []
            self._filling: bool = False
            self._sub: Any = None
            self._initialized = True

            if auto_subscribe:
                self._auto_subscribe()

    # ========================================================
    # 事件订阅
    # ========================================================
    def _auto_subscribe(self) -> None:
        """订阅 candle_buffer 的 data.gap 事件"""
        try:
            from core.event_bus import subscribe
            self._sub = subscribe("data.gap", self._on_gap_event)
            _log().debug("GapFiller 已订阅 data.gap 事件")
        except Exception as exc:
            _log().warning(f"订阅 data.gap 失败: {exc}")

    def _on_gap_event(self, event: Any) -> None:
        try:
            data = getattr(event, "data", None) or {}
            if isinstance(data, dict):
                self.enqueue_gap(
                    symbol=data.get("symbol", ""),
                    interval=data.get("interval", ""),
                    start_ms=int(data.get("missing_start", 0)),
                    end_ms=int(data.get("missing_end", 0)),
                )
        except Exception as exc:
            _log().debug(f"缺口事件处理失败: {exc}")

    # ========================================================
    # 缺口入队
    # ========================================================
    def enqueue_gap(
        self, symbol: str, interval: str,
        start_ms: int, end_ms: int,
    ) -> Optional[GapRange]:
        """将一个缺口加入队列；相邻缺口会合并"""
        if not symbol or not interval:
            return None
        if start_ms <= 0 or end_ms <= start_ms:
            return None

        ms = INTERVAL_MS.get(interval, 0)
        if ms <= 0:
            _log().warning(f"未支持的周期: {interval}")
            return None

        # 对齐周期边界
        start_ms = (start_ms // ms) * ms
        end_ms = ((end_ms + ms - 1) // ms) * ms

        with self._lock:
            # 尝试合并
            for existing in self._pending:
                if existing.symbol == symbol and \
                        existing.interval == interval:
                    if start_ms <= existing.end_ms and \
                            end_ms >= existing.start_ms:
                        existing.start_ms = min(
                            existing.start_ms, start_ms)
                        existing.end_ms = max(
                            existing.end_ms, end_ms)
                        _log().debug(
                            f"缺口合并: {existing.to_dict()}")
                        return existing

            if len(self._pending) >= MAX_PENDING:
                _log().warning("挂起缺口已达上限，丢弃新缺口")
                return None

            gap = GapRange(
                symbol=symbol, interval=interval,
                start_ms=start_ms, end_ms=end_ms,
            )
            self._pending.append(gap)
            self._stats.total_gaps += 1
            self._stats.pending_gaps = len(self._pending)
            return gap

    # ========================================================
    # 主补齐流程
    # ========================================================
    async def run_pending(self) -> List[FillResult]:
        """
        处理所有挂起缺口。
        返回本轮补齐的结果列表。
        """
        if self._async_lock is None:
            self._async_lock = asyncio.Lock()

        if self._async_lock.locked():
            _log().debug("已有补齐任务运行中，跳过")
            return []

        async with self._async_lock:
            with self._lock:
                if self._filling:
                    return []
                self._filling = True
                pending = list(self._pending)
                self._pending.clear()
                self._stats.pending_gaps = 0

            self._publish("data.filling", {
                "count": len(pending),
                "ts_ms": _now_ms(),
            })

            results: List[FillResult] = []
            for gap in pending:
                try:
                    result = await self.fill_gap(gap)
                    results.append(result)
                except Exception as exc:
                    _log().error(f"补齐异常: {exc}")
                    results.append(FillResult(
                        gap=gap, success=False, error=str(exc)))

            with self._lock:
                self._filling = False

            self._publish("data.filling_done", {
                "results": len(results),
                "success": sum(1 for r in results if r.success),
                "ts_ms": _now_ms(),
            })

            return results

    async def fill_gap(self, gap: GapRange) -> FillResult:
        """补齐单个缺口（自动分片）"""
        start = time.perf_counter()
        result = FillResult(gap=gap)
        gap.attempts += 1

        ms = INTERVAL_MS.get(gap.interval, 0)
        if ms <= 0:
            result.error = f"unknown_interval:{gap.interval}"
            self._on_fill_failed(gap, result)
            return result

        # 获取依赖
        rest = self._get_rest()
        buffer = self._get_buffer()
        cleaner = self._get_cleaner()
        if rest is None or buffer is None:
            result.error = "dependencies_missing"
            self._on_fill_failed(gap, result)
            return result

        # 缺口大小限制
        total_bars = gap.bars
        if total_bars > self._max_gap_bars:
            _log().warning(
                f"缺口过大 ({total_bars} 根) > 上限 "
                f"({self._max_gap_bars})，截断处理")
            # 只补最近的 N 根
            gap.start_ms = gap.end_ms - \
                self._max_gap_bars * ms

        # 分片
        cursor = gap.start_ms
        total_filled = 0
        total_skipped = 0
        total_rejected = 0

        while cursor < gap.end_ms:
            chunk_end = min(
                cursor + MAX_BARS_PER_REQUEST * ms,
                gap.end_ms,
            )
            try:
                candles = await rest.klines(
                    symbol=gap.symbol,
                    interval=gap.interval,
                    limit=MAX_BARS_PER_REQUEST,
                    start_time=cursor,
                    end_time=chunk_end,
                )
            except Exception as exc:
                result.error = f"rest_error:{exc}"
                _log().warning(
                    f"拉取失败 {gap.symbol}/{gap.interval} "
                    f"[{cursor},{chunk_end}]: {exc}")
                self._on_fill_failed(gap, result)
                return result

            if not candles:
                cursor = chunk_end
                continue

            for k in candles:
                try:
                    open_time = int(k[0])
                except Exception:
                    total_rejected += 1
                    continue

                # 幂等：已在 buffer 中则跳过
                if buffer.latest_candle(
                        gap.symbol, gap.interval):
                    # 简单的存在性检查：用 range 判断
                    existing = buffer.range(
                        gap.symbol, gap.interval,
                        open_time, open_time)
                    if existing:
                        total_skipped += 1
                        continue

                candle = {
                    "symbol": gap.symbol.lower(),
                    "interval": gap.interval,
                    "open_time": open_time,
                    "close_time": int(k[6]) if len(k) > 6 else 0,
                    "open": float(k[1]),
                    "high": float(k[2]),
                    "low": float(k[3]),
                    "close": float(k[4]),
                    "volume": float(k[5]),
                    "quote_volume": float(k[7]) if len(k) > 7 else 0.0,
                    "trades": int(k[8]) if len(k) > 8 else 0,
                    "is_closed": True,
                }

                # 清洗
                if cleaner is not None:
                    cr = cleaner.clean(candle)
                    if not cr.is_valid:
                        total_rejected += 1
                        continue
                    candle = cr.candle

                # 写入 buffer
                ok = buffer.push_dict(candle)
                if ok:
                    total_filled += 1
                else:
                    total_skipped += 1

            cursor = chunk_end

        result.filled = total_filled
        result.skipped = total_skipped
        result.rejected = total_rejected
        result.success = (total_filled > 0 or
                          total_skipped > 0)
        result.duration_ms = int(
            (time.perf_counter() - start) * 1000)

        if result.success:
            self._on_fill_success(result)
        else:
            self._on_fill_failed(gap, result)

        return result

    # ========================================================
    # 结果处理
    # ========================================================
    def _on_fill_success(self, result: FillResult) -> None:
        with self._lock:
            self._stats.filled_gaps += 1
            self._stats.total_bars_filled += result.filled
            self._stats.total_bars_skipped += result.skipped
            self._stats.total_bars_rejected += result.rejected
        _log().info(
            f"缺口已补: {result.gap.symbol}/"
            f"{result.gap.interval} "
            f"[{result.gap.start_ms}→{result.gap.end_ms}] "
            f"filled={result.filled} skipped={result.skipped} "
            f"rejected={result.rejected} "
            f"({result.duration_ms}ms)")
        self._publish("data.filled", result.to_dict())

    def _on_fill_failed(
        self, gap: GapRange, result: FillResult,
    ) -> None:
        with self._lock:
            self._stats.failed_gaps += 1
            self._stats.retry_count += 1
        gap.last_error = result.error
        _log().warning(
            f"缺口补齐失败: {gap.symbol}/{gap.interval} "
            f"[{gap.start_ms}→{gap.end_ms}] "
            f"attempts={gap.attempts} error={result.error}")

        # 挂起重试（不超过尝试上限）
        if gap.attempts < 5:
            with self._lock:
                self._pending.append(gap)
                self._stats.pending_gaps = len(self._pending)
            self._publish("data.gap_retry", gap.to_dict())
        else:
            _log().error(
                f"缺口放弃（超过重试上限）: {gap.to_dict()}")
            self._publish("data.gap_unfilled", {
                **gap.to_dict(),
                "final_error": result.error,
            })

    # ========================================================
    # 依赖获取
    # ========================================================
    def _get_rest(self) -> Any:
        try:
            from data.binance_rest import get_binance_rest
            return get_binance_rest()
        except Exception as exc:
            _log().debug(f"REST 不可用: {exc}")
            return None

    def _get_buffer(self) -> Any:
        try:
            from data.candle_buffer import get_candle_buffer
            return get_candle_buffer()
        except Exception as exc:
            _log().debug(f"Buffer 不可用: {exc}")
            return None

    def _get_cleaner(self) -> Any:
        try:
            from data.cleaner import get_cleaner
            return get_cleaner()
        except Exception as exc:
            _log().debug(f"Cleaner 不可用: {exc}")
            return None

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
    # 后台循环
    # ========================================================
    async def run_forever(self) -> None:
        """定时重试挂起缺口"""
        _log().info("GapFiller 后台循环启动")
        try:
            while True:
                await asyncio.sleep(self._pending_retry_s)
                with self._lock:
                    has_pending = bool(self._pending)
                if has_pending:
                    await self.run_pending()
        except asyncio.CancelledError:
            _log().info("GapFiller 后台循环取消")
            raise

    def cancel(self) -> None:
        """外部取消入口（供信号处理调用）"""
        with self._lock:
            self._filling = False
            self._pending.clear()
            self._stats.pending_gaps = 0
        _log().info("GapFiller 已取消")

    # ========================================================
    # 状态
    # ========================================================
    def pending(self) -> List[GapRange]:
        with self._lock:
            return list(self._pending)

    def is_filling(self) -> bool:
        with self._lock:
            return self._filling

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            return {
                **self._stats.to_dict(),
                "success_rate": round(self._stats.success_rate(), 4),
                "max_gap_bars": self._max_gap_bars,
            }

    def health(self) -> Dict[str, Any]:
        with self._lock:
            pending = len(self._pending)
            filling = self._filling
            total = self._stats.total_gaps
            failed = self._stats.failed_gaps
        if filling:
            status = "ok"
        elif pending > 10:
            status = "degraded"
        elif total > 0 and failed / total > 0.5:
            status = "degraded"
        else:
            status = "ok"
        return {
            "status": status,
            "pending": pending,
            "filling": filling,
            "stats": dict(self._stats.to_dict()),
        }

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<GapFiller pending={len(self._pending)} "
                f"total={self._stats.total_gaps} "
                f"filled={self._stats.filled_gaps}>"
            )


# ============================================================
# 全局入口
# ============================================================
_filler_instance: Optional[GapFiller] = None
_filler_lock = threading.Lock()


def init_gap_filler(
    max_gap_bars: int = DEFAULT_MAX_GAP_BARS,
    pending_retry_s: float = PENDING_RETRY_INTERVAL_S,
    auto_subscribe: bool = True,
) -> GapFiller:
    """显式初始化（启动时调用）"""
    global _filler_instance
    with _filler_lock:
        _filler_instance = GapFiller(
            max_gap_bars=max_gap_bars,
            pending_retry_s=pending_retry_s,
            auto_subscribe=auto_subscribe,
        )
    return _filler_instance


def get_gap_filler() -> GapFiller:
    """获取单例（惰性初始化）"""
    global _filler_instance
    if _filler_instance is None:
        with _filler_lock:
            if _filler_instance is None:
                _filler_instance = GapFiller()
    return _filler_instance
