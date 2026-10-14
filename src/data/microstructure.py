"""---UMS-BEGIN---
spec_version: 1.0
module: data
type: core
name: microstructure
version: 2.0.0
level: 2
required: false
author: quant-team
created: 2026-10-14
brief: 市场微观结构（OBI/CVD/VWAP/大单/成交失衡/时间窗口）

provides:
  - class Microstructure
  - class Trade
  - class MicroSnapshot
  - class MicroConfig
  - class MicroStats
  - function get_microstructure
  - function init_microstructure

requires:
  modules: [core.logger, core.timestamp, core.event_bus]
  libraries: []
  resources:
    memory_mb: 32
    cpu_cores: 1

health:
  fn: Microstructure.health
  timeout_ms: 500

assembly:
  order: 33
  parallel: false

tests:
  - tests/test_microstructure.py

fallback:
  - event_bus 缺失 → 本地日志
  - 空数据 → 返回 None
  - 异常输入 → 丢弃 + 计数

tags: [data, microstructure, orderflow]
---UMS-END---"""

from __future__ import annotations

import math
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Deque, Dict, List, Optional, Tuple

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("microstructure")
        except Exception:
            import logging
            _logger = logging.getLogger("microstructure")
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
DEFAULT_WINDOW_MS = 300_000          # 5 分钟窗口
DEFAULT_MAX_TRADES = 10_000          # 每个序列最多保留
DEFAULT_LARGE_TRADE_PERCENTILE = 0.95
DEFAULT_MIN_TRADES_FOR_LARGE = 50
DEFAULT_EPSILON = 1e-12


# ============================================================
# 数据结构
# ============================================================
class TradeSide(str, Enum):
    BUY = "buy"
    SELL = "sell"
    UNKNOWN = "unknown"


@dataclass(frozen=True)
class Trade:
    ts_ms: int
    price: float
    qty: float
    side: TradeSide

    def notional(self) -> float:
        return self.price * self.qty


@dataclass
class MicroSnapshot:
    """市场微观结构快照"""
    symbol: str
    interval: str
    ts_ms: int

    # 订单簿
    best_bid: float = 0.0
    best_ask: float = 0.0
    mid_price: float = 0.0
    spread: float = 0.0
    spread_pct: float = 0.0
    obi_l1: float = 0.0
    obi_l5: float = 0.0
    obi_full: float = 0.0
    obi_weighted: float = 0.0
    bid_depth_5: float = 0.0
    ask_depth_5: float = 0.0
    bid_depth_full: float = 0.0
    ask_depth_full: float = 0.0

    # 成交
    trade_count: int = 0
    buy_volume: float = 0.0
    sell_volume: float = 0.0
    total_volume: float = 0.0
    total_notional: float = 0.0
    vwap: float = 0.0
    trade_imbalance: float = 0.0     # [-1, 1]
    cvd: float = 0.0                  # 累计成交量差
    cvd_delta: float = 0.0            # 窗口内变化
    large_trade_count: int = 0
    large_trade_ratio: float = 0.0
    trades_per_second: float = 0.0
    avg_trade_size: float = 0.0

    # 信号
    buy_pressure_score: float = 0.0   # [0, 1]

    def to_dict(self) -> Dict[str, Any]:
        return {k: getattr(self, k) for k in self.__dataclass_fields__}


@dataclass
class MicroConfig:
    window_ms: int = DEFAULT_WINDOW_MS
    max_trades: int = DEFAULT_MAX_TRADES
    large_trade_percentile: float = DEFAULT_LARGE_TRADE_PERCENTILE
    min_trades_for_large: int = DEFAULT_MIN_TRADES_FOR_LARGE
    epsilon: float = DEFAULT_EPSILON


@dataclass
class MicroStats:
    updates_depth: int = 0
    updates_trade: int = 0
    invalid_inputs: int = 0
    na_n_inf: int = 0
    started_ms: int = field(default_factory=_now_ms)

    def to_dict(self) -> Dict[str, Any]:
        return {k: getattr(self, k) for k in (
            "updates_depth", "updates_trade", "invalid_inputs",
            "na_n_inf", "started_ms",
        )}


# ============================================================
# 单序列状态
# ============================================================
class _SeriesState:
    """每个 (symbol, interval) 独立的微观结构状态"""

    def __init__(self, cfg: MicroConfig) -> None:
        self._cfg = cfg
        self._lock = threading.RLock()
        self._trades: Deque[Trade] = deque(maxlen=cfg.max_trades)
        # 订单簿最近快照
        self._bids: List[Tuple[float, float]] = []
        self._asks: List[Tuple[float, float]] = []
        self._best_bid: float = 0.0
        self._best_ask: float = 0.0
        self._last_depth_ms: int = 0
        # CVD 累计
        self._cvd: float = 0.0
        # 窗口内 CVD 基准
        self._window_cvd_base: float = 0.0
        self._window_start_ms: int = 0

    # ---------- 更新 ----------
    def update_depth(
        self,
        bids: List[Tuple[float, float]],
        asks: List[Tuple[float, float]],
        ts_ms: int,
    ) -> None:
        with self._lock:
            self._bids = list(bids)
            self._asks = list(asks)
            if bids:
                self._best_bid = bids[0][0]
            if asks:
                self._best_ask = asks[0][0]
            self._last_depth_ms = ts_ms

    def update_trade(
        self, price: float, qty: float,
        side: TradeSide, ts_ms: int,
    ) -> None:
        if not (math.isfinite(price) and math.isfinite(qty)):
            return
        if price <= 0 or qty <= 0:
            return
        with self._lock:
            self._trades.append(Trade(ts_ms, price, qty, side))
            # CVD
            if side == TradeSide.BUY:
                self._cvd += qty
            elif side == TradeSide.SELL:
                self._cvd -= qty
            # 窗口滚动
            self._evict_old(ts_ms)

    def _evict_old(self, now_ms: int) -> None:
        cutoff = now_ms - self._cfg.window_ms
        # 只保留窗口内的成交
        while self._trades and self._trades[0].ts_ms < cutoff:
            self._trades.popleft()

    # ---------- 计算 ----------
    def snapshot(
        self, symbol: str, interval: str, ts_ms: int,
    ) -> MicroSnapshot:
        with self._lock:
            self._evict_old(ts_ms)

            s = MicroSnapshot(
                symbol=symbol, interval=interval, ts_ms=ts_ms)

            # 订单簿
            s.best_bid = self._best_bid
            s.best_ask = self._best_ask
            if s.best_bid > 0 and s.best_ask > 0:
                s.mid_price = (s.best_bid + s.best_ask) / 2
                s.spread = s.best_ask - s.best_bid
                s.spread_pct = (
                    s.spread / s.mid_price
                    if s.mid_price > 0 else 0.0)

            # OBI 分层
            bid5 = self._bids[:5]
            ask5 = self._asks[:5]
            bid_v1 = self._bids[0][1] if self._bids else 0.0
            ask_v1 = self._asks[0][1] if self._asks else 0.0
            bid_v5 = sum(q for _, q in bid5)
            ask_v5 = sum(q for _, q in ask5)
            bid_vf = sum(q for _, q in self._bids)
            ask_vf = sum(q for _, q in self._asks)

            s.obi_l1 = self._obi(bid_v1, ask_v1)
            s.obi_l5 = self._obi(bid_v5, ask_v5)
            s.obi_full = self._obi(bid_vf, ask_vf)
            s.bid_depth_5 = bid_v5
            s.ask_depth_5 = ask_v5
            s.bid_depth_full = bid_vf
            s.ask_depth_full = ask_vf

            # 加权 OBI
            mid = s.mid_price if s.mid_price > 0 else 0.0
            if mid > 0:
                bid_w = sum(
                    q / (1.0 + abs(p - mid) / mid)
                    for p, q in self._bids)
                ask_w = sum(
                    q / (1.0 + abs(p - mid) / mid)
                    for p, q in self._asks)
                s.obi_weighted = self._obi(bid_w, ask_w)

            # 成交统计
            trades = list(self._trades)
            s.trade_count = len(trades)
            if trades:
                buy_v = sum(t.qty for t in trades
                            if t.side == TradeSide.BUY)
                sell_v = sum(t.qty for t in trades
                             if t.side == TradeSide.SELL)
                total_v = sum(t.qty for t in trades)
                total_n = sum(t.notional() for t in trades)

                s.buy_volume = buy_v
                s.sell_volume = sell_v
                s.total_volume = total_v
                s.total_notional = total_n
                s.vwap = (total_n / total_v
                          if total_v > 0 else 0.0)
                s.trade_imbalance = self._obi(buy_v, sell_v)
                s.avg_trade_size = (
                    total_v / s.trade_count
                    if s.trade_count else 0.0)

                # CVD
                s.cvd = self._cvd
                # 窗口内 CVD 基准：第一次计算时记录
                if self._window_start_ms == 0:
                    self._window_start_ms = ts_ms
                    self._window_cvd_base = self._cvd
                else:
                    # 如果窗口已滚动超过一个 window
                    if ts_ms - self._window_start_ms > \
                            self._cfg.window_ms:
                        # 重新校准基准
                        self._window_cvd_base = self._cvd - \
                            (buy_v - sell_v)
                        self._window_start_ms = ts_ms
                s.cvd_delta = self._cvd - self._window_cvd_base

                # 大单检测
                if len(trades) >= self._cfg.min_trades_for_large:
                    sizes = sorted(t.qty for t in trades)
                    idx = int(len(sizes) *
                              self._cfg.large_trade_percentile)
                    idx = min(idx, len(sizes) - 1)
                    threshold = sizes[idx]
                    large_count = sum(
                        1 for t in trades if t.qty >= threshold)
                    s.large_trade_count = large_count
                    s.large_trade_ratio = (
                        large_count / len(trades))

                # 成交速率
                if len(trades) >= 2:
                    span_ms = (trades[-1].ts_ms -
                               trades[0].ts_ms)
                    if span_ms > 0:
                        s.trades_per_second = (
                            len(trades) / (span_ms / 1000.0))

                # 综合买压评分
                s.buy_pressure_score = self._buy_pressure(
                    s.obi_l5, s.trade_imbalance, s.cvd_delta)

            return s

    def _obi(self, bid: float, ask: float) -> float:
        denom = bid + ask
        if denom <= self._cfg.epsilon:
            return 0.0
        v = (bid - ask) / denom
        return max(-1.0, min(1.0, v))

    @staticmethod
    def _buy_pressure(
        obi: float, trade_imb: float, cvd_delta: float,
    ) -> float:
        """买压综合评分 [0, 1]"""
        # 归一化 CVD delta（无参考尺度时使用 sigmoid）
        cvd_component = 1.0 / (1.0 + math.exp(-cvd_delta / 100.0))
        # obi 和 trade_imb 映射到 [0, 1]
        o = (obi + 1.0) / 2.0
        t = (trade_imb + 1.0) / 2.0
        score = 0.4 * o + 0.4 * t + 0.2 * cvd_component
        return max(0.0, min(1.0, score))

    def reset(self) -> None:
        with self._lock:
            self._trades.clear()
            self._bids = []
            self._asks = []
            self._best_bid = 0.0
            self._best_ask = 0.0
            self._cvd = 0.0
            self._window_cvd_base = 0.0
            self._window_start_ms = 0

    def trade_count(self) -> int:
        with self._lock:
            return len(self._trades)


# ============================================================
# Microstructure 单例
# ============================================================
class Microstructure:
    """
    市场微观结构聚合器。

    特性：
    - 多符号/多周期隔离
    - 滑动时间窗口
    - 订单簿 OBI（L1/L5/Full/Weighted）
    - 成交失衡 / VWAP / CVD
    - 大单检测（分位数）
    - 买压综合评分
    """

    _instance: Optional["Microstructure"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "Microstructure":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self, config: Optional[MicroConfig] = None,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self.config = config or MicroConfig()
            self._series: Dict[
                Tuple[str, str], _SeriesState] = {}
            self._stats = MicroStats()
            self._initialized = True

    def _get_series(
        self, symbol: str, interval: str,
    ) -> _SeriesState:
        key = (symbol.lower(), interval)
        with self._lock:
            s = self._series.get(key)
            if s is None:
                s = _SeriesState(self.config)
                self._series[key] = s
            return s

    # ========================================================
    # 更新
    # ========================================================
    def update_depth(
        self,
        symbol: str,
        interval: str,
        bids: List[Any],
        asks: List[Any],
        ts_ms: Optional[int] = None,
    ) -> bool:
        """更新订单簿快照"""
        try:
            b = self._parse_levels(bids)
            a = self._parse_levels(asks)
        except Exception as exc:
            with self._lock:
                self._stats.invalid_inputs += 1
            _log().debug(f"depth 解析失败: {exc}")
            return False

        if not b or not a:
            with self._lock:
                self._stats.invalid_inputs += 1
            return False

        # 排序（bids 降序，asks 升序）
        b.sort(key=lambda x: x[0], reverse=True)
        a.sort(key=lambda x: x[0])

        ts = ts_ms or _now_ms()
        series = self._get_series(symbol, interval)
        series.update_depth(b, a, ts)

        with self._lock:
            self._stats.updates_depth += 1
        return True

    def update_trade(
        self,
        symbol: str,
        interval: str,
        price: float,
        qty: float,
        side: str,
        ts_ms: Optional[int] = None,
    ) -> bool:
        """更新成交"""
        try:
            p = float(price)
            q = float(qty)
        except (TypeError, ValueError):
            with self._lock:
                self._stats.invalid_inputs += 1
            return False

        if not (math.isfinite(p) and math.isfinite(q)):
            with self._lock:
                self._stats.na_n_inf += 1
            return False
        if p <= 0 or q <= 0:
            with self._lock:
                self._stats.invalid_inputs += 1
            return False

        try:
            sd = TradeSide(str(side).lower())
        except ValueError:
            sd = TradeSide.UNKNOWN

        ts = ts_ms or _now_ms()
        series = self._get_series(symbol, interval)
        series.update_trade(p, q, sd, ts)

        with self._lock:
            self._stats.updates_trade += 1
        return True

    @staticmethod
    def _parse_levels(
        levels: List[Any],
    ) -> List[Tuple[float, float]]:
        out: List[Tuple[float, float]] = []
        for lv in levels:
            try:
                if isinstance(lv, dict):
                    p = float(lv.get("price", 0))
                    q = float(lv.get("qty",
                                     lv.get("quantity", 0)))
                else:
                    p = float(lv[0])
                    q = float(lv[1])
            except (TypeError, ValueError, IndexError):
                continue
            if not (math.isfinite(p) and math.isfinite(q)):
                continue
            if p <= 0 or q <= 0:
                continue
            out.append((p, q))
        return out

    # ========================================================
    # 查询
    # ========================================================
    def snapshot(
        self,
        symbol: str,
        interval: str,
        ts_ms: Optional[int] = None,
    ) -> Optional[MicroSnapshot]:
        series = self._get_series(symbol, interval)
        if series.trade_count() == 0 and \
                series._best_bid <= 0:
            return None
        return series.snapshot(
            symbol.lower(), interval, ts_ms or _now_ms())

    def obi(
        self, symbol: str, interval: str, level: str = "l5",
    ) -> Optional[float]:
        s = self.snapshot(symbol, interval)
        if s is None:
            return None
        return {
            "l1": s.obi_l1,
            "l5": s.obi_l5,
            "full": s.obi_full,
            "weighted": s.obi_weighted,
        }.get(level, s.obi_l5)

    def trade_imbalance(
        self, symbol: str, interval: str,
    ) -> Optional[float]:
        s = self.snapshot(symbol, interval)
        return s.trade_imbalance if s else None

    def cvd(self, symbol: str, interval: str) -> Optional[float]:
        s = self.snapshot(symbol, interval)
        return s.cvd if s else None

    def vwap(self, symbol: str, interval: str) -> Optional[float]:
        s = self.snapshot(symbol, interval)
        return s.vwap if s else None

    def reset(
        self, symbol: str, interval: str,
    ) -> None:
        series = self._get_series(symbol, interval)
        series.reset()

    def reset_all(self) -> None:
        with self._lock:
            for s in self._series.values():
                s.reset()

    # ========================================================
    # 状态
    # ========================================================
    def stats(self) -> Dict[str, Any]:
        with self._lock:
            return {
                **self._stats.to_dict(),
                "series_count": len(self._series),
            }

    def health(self) -> Dict[str, Any]:
        with self._lock:
            series_count = len(self._series)
            invalid = self._stats.invalid_inputs
            updates = self._stats.updates_depth + \
                self._stats.updates_trade
        if updates == 0:
            status = "ok"
        elif invalid / max(updates, 1) > 0.3:
            status = "degraded"
        else:
            status = "ok"
        return {
            "status": status,
            "series_count": series_count,
            "stats": dict(self._stats.to_dict()),
        }

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<Microstructure series={len(self._series)} "
                f"depth={self._stats.updates_depth} "
                f"trades={self._stats.updates_trade}>"
            )


# ============================================================
# 全局入口
# ============================================================
_micro_instance: Optional[Microstructure] = None
_micro_lock = threading.Lock()


def init_microstructure(
    config: Optional[MicroConfig] = None,
) -> Microstructure:
    """显式初始化（启动时调用）"""
    global _micro_instance
    with _micro_lock:
        _micro_instance = Microstructure(config)
    return _micro_instance


def get_microstructure() -> Microstructure:
    """获取单例（惰性初始化）"""
    global _micro_instance
    if _micro_instance is None:
        with _micro_lock:
            if _micro_instance is None:
                _micro_instance = Microstructure()
    return _micro_instance
