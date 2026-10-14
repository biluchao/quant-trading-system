"""---UMS-BEGIN---
spec_version: 1.0
module: data
type: core
name: depth_cleaner
version: 2.0.0
level: 2
required: true
author: quant-team
created: 2026-10-14
brief: 订单簿深度清洗器（异常过滤/交叉检测/分层OBI/事件发布）

provides:
  - class DepthCleaner
  - class DepthSnapshot
  - class CleanResult
  - class DepthStats
  - class DepthConfig
  - function get_depth_cleaner
  - function init_depth_cleaner

requires:
  modules: [core.logger, core.timestamp, core.event_bus]
  libraries: []
  resources:
    memory_mb: 16
    cpu_cores: 1

health:
  fn: DepthCleaner.health
  timeout_ms: 500

assembly:
  order: 30
  parallel: false

tests:
  - tests/test_depth_cleaner.py

fallback:
  - event_bus 缺失 → 本地日志
  - 输入非法 → 整份拒绝
  - 空数据 → 返回 None

tags: [data, orderbook, obi]
---UMS-END---"""

from __future__ import annotations

import math
import threading
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional, Tuple

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("depth_cleaner")
        except Exception:
            import logging
            _logger = logging.getLogger("depth_cleaner")
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
DEFAULT_MAX_DISTANCE_PCT = 0.02     # 距中间价 2% 外的档位剔除
DEFAULT_ANOMALY_MULTIPLE = 10.0     # 中位数的 10 倍视为异常
DEFAULT_MIN_LEVELS = 3              # 每侧最少档位数
DEFAULT_MAX_LEVELS = 50             # 每侧最多档位数
DEFAULT_EPSILON = 1e-12


# ============================================================
# 结果类型
# ============================================================
class CleanAction(str, Enum):
    PASS = "pass"
    CORRECTED = "corrected"
    REJECTED = "rejected"


@dataclass
class DepthSnapshot:
    """清洗后的订单簿快照"""
    symbol: str
    ts_ms: int
    bids: List[List[float]] = field(default_factory=list)
    asks: List[List[float]] = field(default_factory=list)
    best_bid: float = 0.0
    best_ask: float = 0.0
    mid_price: float = 0.0
    spread: float = 0.0
    spread_pct: float = 0.0
    # 各层级 OBI
    obi_l1: float = 0.0
    obi_l5: float = 0.0
    obi_full: float = 0.0
    obi_weighted: float = 0.0
    # 各层级总深度
    bid_depth_5: float = 0.0
    ask_depth_5: float = 0.0
    bid_depth_full: float = 0.0
    ask_depth_full: float = 0.0
    # 元数据
    bid_levels_raw: int = 0
    ask_levels_raw: int = 0
    bid_levels_kept: int = 0
    ask_levels_kept: int = 0

    def to_dict(self) -> Dict[str, Any]:
        return {
            "symbol": self.symbol,
            "ts_ms": self.ts_ms,
            "bids": self.bids,
            "asks": self.asks,
            "best_bid": self.best_bid,
            "best_ask": self.best_ask,
            "mid_price": self.mid_price,
            "spread": self.spread,
            "spread_pct": self.spread_pct,
            "obi_l1": self.obi_l1,
            "obi_l5": self.obi_l5,
            "obi_full": self.obi_full,
            "obi_weighted": self.obi_weighted,
            "bid_depth_5": self.bid_depth_5,
            "ask_depth_5": self.ask_depth_5,
            "bid_depth_full": self.bid_depth_full,
            "ask_depth_full": self.ask_depth_full,
            "bid_levels_raw": self.bid_levels_raw,
            "ask_levels_raw": self.ask_levels_raw,
            "bid_levels_kept": self.bid_levels_kept,
            "ask_levels_kept": self.ask_levels_kept,
        }


@dataclass
class CleanResult:
    snapshot: Optional[DepthSnapshot]
    action: CleanAction
    reasons: List[str] = field(default_factory=list)

    @property
    def is_valid(self) -> bool:
        return (self.snapshot is not None
                and self.action != CleanAction.REJECTED)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "action": self.action.value,
            "reasons": list(self.reasons),
            "snapshot": self.snapshot.to_dict()
            if self.snapshot else None,
        }


@dataclass
class DepthConfig:
    max_distance_pct: float = DEFAULT_MAX_DISTANCE_PCT
    anomaly_multiple: float = DEFAULT_ANOMALY_MULTIPLE
    min_levels: int = DEFAULT_MIN_LEVELS
    max_levels: int = DEFAULT_MAX_LEVELS
    enable_anomaly_filter: bool = True
    enable_distance_filter: bool = True
    epsilon: float = DEFAULT_EPSILON


@dataclass
class DepthStats:
    total: int = 0
    passed: int = 0
    corrected: int = 0
    rejected: int = 0
    rejected_crossed: int = 0
    rejected_empty: int = 0
    rejected_bad_price: int = 0
    rejected_bad_qty: int = 0
    levels_removed_anomaly: int = 0
    levels_removed_distance: int = 0
    nan_inf_count: int = 0
    started_ms: int = field(default_factory=_now_ms)

    def to_dict(self) -> Dict[str, Any]:
        return {k: getattr(self, k) for k in (
            "total", "passed", "corrected", "rejected",
            "rejected_crossed", "rejected_empty",
            "rejected_bad_price", "rejected_bad_qty",
            "levels_removed_anomaly", "levels_removed_distance",
            "nan_inf_count", "started_ms",
        )}

    def pass_rate(self) -> float:
        return self.passed / self.total if self.total else 0.0

    def reject_rate(self) -> float:
        return self.rejected / self.total if self.total else 0.0


# ============================================================
# DepthCleaner 单例
# ============================================================
class DepthCleaner:
    """
    订单簿深度清洗器。

    流程：
      1. 结构与有效性检查
      2. 价格/数量过滤（正数、有限）
      3. 交叉检测（bid >= ask 拒绝）
      4. 远价过滤（> max_distance_pct）
      5. 异常大单过滤（中位数 × 倍数）
      6. 层级 OBI 计算（L1/L5/Full/Weighted）
      7. 事件发布
    """

    _instance: Optional["DepthCleaner"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "DepthCleaner":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self, config: Optional[DepthConfig] = None,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self.config = config or DepthConfig()
            self._stats = DepthStats()
            self._last_snapshot: Dict[str, DepthSnapshot] = {}
            self._initialized = True

    # ========================================================
    # 主入口
    # ========================================================
    def clean(
        self,
        depth: Dict[str, Any],
        symbol: Optional[str] = None,
        ts_ms: Optional[int] = None,
    ) -> CleanResult:
        """清洗订单簿深度"""
        with self._lock:
            self._stats.total += 1

        # 结构检查
        if not isinstance(depth, dict):
            return self._reject(["not_a_dict"], "rejected_empty")

        sym = (symbol or depth.get("symbol") or "").lower()
        bids_raw = depth.get("bids") or depth.get("b") or []
        asks_raw = depth.get("asks") or depth.get("a") or []

        if not bids_raw or not asks_raw:
            return self._reject(
                ["empty_book"], "rejected_empty")

        ts = ts_ms or int(depth.get("ts_ms", 0)) or _now_ms()

        # 1. 逐档解析 + 价格/数量过滤
        bids, bad_price_b, bad_qty_b = self._parse_side(
            bids_raw, side="bid")
        asks, bad_price_a, bad_qty_a = self._parse_side(
            asks_raw, side="ask")

        if bad_price_b or bad_price_a:
            with self._lock:
                self._stats.rejected_bad_price += 1
        if bad_qty_b or bad_qty_a:
            with self._lock:
                self._stats.rejected_bad_qty += 1

        # 至少要有一定档位
        min_lv = max(1, self.config.min_levels)
        if len(bids) < min_lv or len(asks) < min_lv:
            return self._reject(
                [f"too_few_levels:bids={len(bids)}"
                 f",asks={len(asks)}"],
                "rejected_empty")

        # 2. 排序（bids 降序，asks 升序）
        bids.sort(key=lambda x: x[0], reverse=True)
        asks.sort(key=lambda x: x[0])

        # 3. 交叉检测
        best_bid = bids[0][0]
        best_ask = asks[0][0]
        if best_bid >= best_ask:
            with self._lock:
                self._stats.rejected_crossed += 1
            return self._reject(
                [f"crossed_book:bid={best_bid}"
                 f",ask={best_ask}"],
                "rejected_crossed")

        # 4. 中间价
        mid = (best_bid + best_ask) / 2.0
        if not (math.isfinite(mid) and mid > 0):
            return self._reject(["bad_mid_price"], "rejected_bad_price")

        # 5. 距离过滤
        removed_dist = 0
        if self.config.enable_distance_filter:
            bids, d1 = self._filter_by_distance(
                bids, mid, side="bid")
            asks, d2 = self._filter_by_distance(
                asks, mid, side="ask")
            removed_dist = d1 + d2
            with self._lock:
                self._stats.levels_removed_distance += removed_dist

        if len(bids) < min_lv or len(asks) < min_lv:
            return self._reject(
                ["too_few_after_distance"], "rejected_empty")

        # 6. 异常大单过滤
        removed_anomaly = 0
        if self.config.enable_anomaly_filter:
            bids, a1 = self._filter_anomaly(bids)
            asks, a2 = self._filter_anomaly(asks)
            removed_anomaly = a1 + a2
            with self._lock:
                self._stats.levels_removed_anomaly += removed_anomaly

        if len(bids) < min_lv or len(asks) < min_lv:
            return self._reject(
                ["too_few_after_anomaly"], "rejected_empty")

        # 7. 截断到最大档位
        max_lv = self.config.max_levels
        bids = bids[:max_lv]
        asks = asks[:max_lv]

        # 8. 计算 OBI
        snapshot = self._build_snapshot(
            sym, ts, bids, asks, mid,
            bid_levels_raw=len(bids_raw),
            ask_levels_raw=len(asks_raw),
        )

        # 9. 记录
        with self._lock:
            self._last_snapshot[sym] = snapshot
            if removed_dist > 0 or removed_anomaly > 0:
                self._stats.corrected += 1
                action = CleanAction.CORRECTED
            else:
                self._stats.passed += 1
                action = CleanAction.PASS

        reasons: List[str] = []
        if removed_dist:
            reasons.append(f"distance_removed:{removed_dist}")
        if removed_anomaly:
            reasons.append(f"anomaly_removed:{removed_anomaly}")

        # 10. 事件
        if action == CleanAction.CORRECTED:
            self._publish("depth.cleaned", {
                "symbol": sym,
                "reasons": reasons,
                "snapshot": snapshot.to_dict(),
                "ts_ms": _now_ms(),
            })

        return CleanResult(
            snapshot=snapshot, action=action, reasons=reasons)

    # ========================================================
    # 解析辅助
    # ========================================================
    def _parse_side(
        self, levels: List[Any], side: str,
    ) -> Tuple[List[List[float]], bool, bool]:
        """
        解析单侧档位，返回 (cleaned, bad_price, bad_qty)
        """
        out: List[List[float]] = []
        bad_price = False
        bad_qty = False
        for lv in levels:
            try:
                if isinstance(lv, dict):
                    price = float(lv.get("price", 0))
                    qty = float(lv.get("qty",
                                       lv.get("quantity", 0)))
                else:
                    price = float(lv[0])
                    qty = float(lv[1])
            except (TypeError, ValueError, IndexError):
                bad_price = True
                continue

            # NaN / Inf
            if not (math.isfinite(price) and math.isfinite(qty)):
                with self._lock:
                    self._stats.nan_inf_count += 1
                continue

            # 正值
            if price <= 0:
                bad_price = True
                continue
            if qty <= 0:
                bad_qty = True
                continue

            out.append([price, qty])
        return out, bad_price, bad_qty

    def _filter_by_distance(
        self, levels: List[List[float]],
        mid: float, side: str,
    ) -> Tuple[List[List[float]], int]:
        if mid <= 0:
            return levels, 0
        max_dist = mid * self.config.max_distance_pct
        filtered: List[List[float]] = []
        removed = 0
        for price, qty in levels:
            if abs(price - mid) > max_dist:
                removed += 1
                continue
            filtered.append([price, qty])
        return filtered, removed

    def _filter_anomaly(
        self, levels: List[List[float]],
    ) -> Tuple[List[List[float]], int]:
        if len(levels) < 3:
            return levels, 0
        qtys = sorted(lv[1] for lv in levels)
        median = qtys[len(qtys) // 2]
        if median <= 0:
            return levels, 0
        threshold = median * self.config.anomaly_multiple
        filtered: List[List[float]] = []
        removed = 0
        for price, qty in levels:
            if qty > threshold:
                removed += 1
                continue
            filtered.append([price, qty])
        return filtered, removed

    # ========================================================
    # 快照构建
    # ========================================================
    def _build_snapshot(
        self, symbol: str, ts_ms: int,
        bids: List[List[float]], asks: List[List[float]],
        mid: float,
        bid_levels_raw: int, ask_levels_raw: int,
    ) -> DepthSnapshot:
        s = DepthSnapshot(
            symbol=symbol,
            ts_ms=ts_ms,
            bids=[[p, q] for p, q in bids],
            asks=[[p, q] for p, q in asks],
            best_bid=bids[0][0],
            best_ask=asks[0][0],
            mid_price=mid,
            bid_levels_raw=bid_levels_raw,
            ask_levels_raw=ask_levels_raw,
            bid_levels_kept=len(bids),
            ask_levels_kept=len(asks),
        )
        s.spread = s.best_ask - s.best_bid
        s.spread_pct = (s.spread / mid) if mid > 0 else 0.0

        # 分层成交量
        bid5 = bids[:5]
        ask5 = asks[:5]

        bid_v1 = bids[0][1]
        ask_v1 = asks[0][1]
        s.obi_l1 = self._obi(bid_v1, ask_v1)

        bid_v5 = sum(q for _, q in bid5)
        ask_v5 = sum(q for _, q in ask5)
        s.bid_depth_5 = bid_v5
        s.ask_depth_5 = ask_v5
        s.obi_l5 = self._obi(bid_v5, ask_v5)

        bid_vf = sum(q for _, q in bids)
        ask_vf = sum(q for _, q in asks)
        s.bid_depth_full = bid_vf
        s.ask_depth_full = ask_vf
        s.obi_full = self._obi(bid_vf, ask_vf)

        # 加权 OBI：距离越近权重越高
        w = 1.0 / (1.0 + self.config.max_distance_pct * 100)
        bid_w = sum(
            q * (1.0 / (1.0 + abs(p - mid) / mid))
            for p, q in bids
        )
        ask_w = sum(
            q * (1.0 / (1.0 + abs(p - mid) / mid))
            for p, q in asks
        )
        s.obi_weighted = self._obi(bid_w, ask_w)
        return s

    def _obi(self, bid_vol: float, ask_vol: float) -> float:
        denom = bid_vol + ask_vol
        if denom <= self.config.epsilon:
            return 0.0
        val = (bid_vol - ask_vol) / denom
        # 归一化到 [-1, 1]
        if val > 1.0:
            val = 1.0
        elif val < -1.0:
            val = -1.0
        return val

    # ========================================================
    # 拒绝与事件
    # ========================================================
    def _reject(
        self, reasons: List[str], kind: str,
    ) -> CleanResult:
        with self._lock:
            self._stats.rejected += 1
        self._publish("depth.rejected", {
            "reasons": reasons,
            "kind": kind,
            "ts_ms": _now_ms(),
        })
        return CleanResult(
            snapshot=None,
            action=CleanAction.REJECTED,
            reasons=reasons,
        )

    def _publish(self, topic: str, data: Any) -> None:
        try:
            from core.event_bus import publish
            publish(topic, data)
        except Exception:
            _log().debug(f"[{topic}] {data}")

    # ========================================================
    # 便捷 API
    # ========================================================
    def calc_obi(
        self, depth: Dict[str, Any],
        symbol: Optional[str] = None,
        level: str = "l5",
    ) -> Optional[float]:
        """快速计算 OBI（无需完整清洗）"""
        result = self.clean(depth, symbol=symbol)
        if not result.is_valid:
            return None
        s = result.snapshot
        if level == "l1":
            return s.obi_l1
        if level == "l5":
            return s.obi_l5
        if level == "full":
            return s.obi_full
        if level == "weighted":
            return s.obi_weighted
        return s.obi_l5

    def last_snapshot(
        self, symbol: str,
    ) -> Optional[DepthSnapshot]:
        with self._lock:
            return self._last_snapshot.get(symbol.lower())

    # ========================================================
    # 状态
    # ========================================================
    def stats(self) -> Dict[str, Any]:
        with self._lock:
            s = self._stats.to_dict()
            s["pass_rate"] = round(self._stats.pass_rate(), 4)
            s["reject_rate"] = round(self._stats.reject_rate(), 4)
            return s

    def health(self) -> Dict[str, Any]:
        with self._lock:
            total = self._stats.total
            reject_rate = self._stats.reject_rate()
        if total == 0:
            status = "ok"
        elif reject_rate > 0.5:
            status = "down"
        elif reject_rate > 0.2:
            status = "degraded"
        else:
            status = "ok"
        return {
            "status": status,
            "total_processed": total,
            "reject_rate": round(reject_rate, 4),
            "stats": dict(self._stats.to_dict()),
        }

    def reset_stats(self) -> None:
        with self._lock:
            self._stats = DepthStats()

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<DepthCleaner total={self._stats.total} "
                f"corrected={self._stats.corrected} "
                f"rejected={self._stats.rejected}>"
            )


# ============================================================
# 全局入口
# ============================================================
_cleaner_instance: Optional[DepthCleaner] = None
_cleaner_lock = threading.Lock()


def init_depth_cleaner(
    config: Optional[DepthConfig] = None,
) -> DepthCleaner:
    """显式初始化（启动时调用）"""
    global _cleaner_instance
    with _cleaner_lock:
        _cleaner_instance = DepthCleaner(config)
    return _cleaner_instance


def get_depth_cleaner() -> DepthCleaner:
    """获取单例（惰性初始化）"""
    global _cleaner_instance
    if _cleaner_instance is None:
        with _cleaner_lock:
            if _cleaner_instance is None:
                _cleaner_instance = DepthCleaner()
    return _cleaner_instance


# ============================================================
# 便捷函数
# ============================================================
def clean_depth(
    depth: Dict[str, Any],
    symbol: Optional[str] = None,
) -> Optional[DepthSnapshot]:
    """便捷：清洗并返回快照"""
    r = get_depth_cleaner().clean(depth, symbol=symbol)
    return r.snapshot if r.is_valid else None


def calc_obi(
    depth: Dict[str, Any],
    level: str = "l5",
    symbol: Optional[str] = None,
) -> Optional[float]:
    """便捷：计算 OBI"""
    return get_depth_cleaner().calc_obi(depth, symbol, level)
