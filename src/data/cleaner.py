"""---UMS-BEGIN---
spec_version: 1.0
module: data
type: core
name: cleaner
version: 2.0.0
level: 2
required: true
author: quant-team
created: 2026-10-14
brief: K线清洗器（插针过滤/有效性/低流动性/成交量异常/MAD保护）

provides:
  - class CandleCleaner
  - class CleanResult
  - class CleanerConfig
  - class CleanerStats
  - function get_cleaner
  - function init_cleaner

requires:
  modules: [core.logger, core.timestamp, core.event_bus]
  libraries: []
  resources:
    memory_mb: 16
    cpu_cores: 1

health:
  fn: CandleCleaner.health
  timeout_ms: 500

assembly:
  order: 28
  parallel: false

tests:
  - tests/test_cleaner.py

fallback:
  - event_bus 缺失 → 本地日志
  - 历史不足 → 只做有效性检查
  - MAD 异常 → 使用绝对阈值

tags: [data, cleaner, quality]
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
            _logger = get_logger("cleaner")
        except Exception:
            import logging
            _logger = logging.getLogger("cleaner")
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
DEFAULT_HISTORY_SIZE = 200
DEFAULT_MAD_K = 10.0          # MAD 倍数阈值
DEFAULT_SPIKE_K = 3.0          # 修正时回退的 MAD 倍数
MIN_MAD_RATIO = 1e-6           # MAD 相对价格的最小比例
MIN_MAD_ABS = 1e-6             # MAD 绝对下限
LOW_VOLUME_RATIO = 0.05        # 低于历史中位数的 5% 视为异常
HIGH_VOLUME_RATIO = 20.0       # 高于历史中位数的 20 倍视为异常


# ============================================================
# 结果类型
# ============================================================
class CleanAction(str, Enum):
    PASS = "pass"              # 未修改
    CORRECTED = "corrected"    # 修正了 high/low
    REJECTED = "rejected"      # 拒绝（数据非法）


@dataclass
class CleanResult:
    candle: Optional[Dict[str, Any]]   # 清洗后的 K 线；None 表示拒绝
    action: CleanAction
    reasons: List[str] = field(default_factory=list)
    original: Optional[Dict[str, Any]] = None
    corrected_fields: Dict[str, Tuple[Any, Any]] = field(
        default_factory=dict)          # field → (old, new)

    @property
    def is_valid(self) -> bool:
        return self.candle is not None and self.action != CleanAction.REJECTED

    def to_dict(self) -> Dict[str, Any]:
        return {
            "action": self.action.value,
            "reasons": list(self.reasons),
            "corrected": {
                k: {"old": v[0], "new": v[1]}
                for k, v in self.corrected_fields.items()
            },
        }


@dataclass
class CleanerConfig:
    history_size: int = DEFAULT_HISTORY_SIZE
    mad_k: float = DEFAULT_MAD_K
    spike_k: float = DEFAULT_SPIKE_K
    low_volume_ratio: float = LOW_VOLUME_RATIO
    high_volume_ratio: float = HIGH_VOLUME_RATIO
    enable_spike_filter: bool = True
    enable_volume_check: bool = True
    enable_volatility_scaling: bool = True


@dataclass
class CleanerStats:
    total: int = 0
    passed: int = 0
    corrected: int = 0
    rejected: int = 0
    spikes_high: int = 0
    spikes_low: int = 0
    low_volume: int = 0
    high_volume: int = 0
    invalid_ohlc: int = 0
    non_positive: int = 0
    nan_inf: int = 0
    started_ms: int = field(default_factory=_now_ms)

    def to_dict(self) -> Dict[str, Any]:
        return {k: getattr(self, k) for k in (
            "total", "passed", "corrected", "rejected",
            "spikes_high", "spikes_low", "low_volume",
            "high_volume", "invalid_ohlc", "non_positive",
            "nan_inf", "started_ms",
        )}

    def pass_rate(self) -> float:
        return self.passed / self.total if self.total else 0.0

    def reject_rate(self) -> float:
        return self.rejected / self.total if self.total else 0.0


# ============================================================
# 单序列历史
# ============================================================
class _SeriesHistory:
    """按 (symbol, interval) 隔离的历史窗口"""

    def __init__(self, size: int) -> None:
        self._h_highs: Deque[float] = deque(maxlen=size)
        self._h_lows: Deque[float] = deque(maxlen=size)
        self._h_closes: Deque[float] = deque(maxlen=size)
        self._h_volumes: Deque[float] = deque(maxlen=size)
        self._h_ranges: Deque[float] = deque(maxlen=size)

    def append(self, candle: Dict[str, Any]) -> None:
        h = float(candle.get("high", 0))
        l = float(candle.get("low", 0))
        c = float(candle.get("close", 0))
        v = float(candle.get("volume", 0))
        self._h_highs.append(h)
        self._h_lows.append(l)
        self._h_closes.append(c)
        self._h_volumes.append(v)
        self._h_ranges.append(h - l)

    def size(self) -> int:
        return len(self._h_closes)

    def median_range(self) -> Optional[float]:
        if not self._h_ranges:
            return None
        return _median(list(self._h_ranges))

    def median_volume(self) -> Optional[float]:
        if not self._h_volumes:
            return None
        return _median(list(self._h_volumes))

    def median_close(self) -> Optional[float]:
        if not self._h_closes:
            return None
        return _median(list(self._h_closes))

    def atr_like(self) -> Optional[float]:
        """平均波动幅度（HL 中位数）"""
        return self.median_range()


def _median(values: List[float]) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    n = len(s)
    mid = n // 2
    if n % 2:
        return s[mid]
    return (s[mid - 1] + s[mid]) / 2.0


# ============================================================
# CandleCleaner 单例
# ============================================================
class CandleCleaner:
    """
    K 线清洗器。

    流程：
      1. 有效性检查（NaN / 正值 / OHLC 关系）
      2. 低/高成交量检测
      3. 插针检测（跨 K 线 MAD + 波动率自适应）
      4. 修正后重新校验
      5. 发布事件
    """

    _instance: Optional["CandleCleaner"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "CandleCleaner":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self, config: Optional[CleanerConfig] = None,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self.config = config or CleanerConfig()
            self._history: Dict[Tuple[str, str], _SeriesHistory] = {}
            self._stats = CleanerStats()
            self._initialized = True

    # ========================================================
    # 公共 API
    # ========================================================
    def clean(
        self, candle: Dict[str, Any],
    ) -> CleanResult:
        """清洗单根 K 线，返回 CleanResult"""
        with self._lock:
            self._stats.total += 1

        # 1. 有效性检查
        reasons: List[str] = []
        if not isinstance(candle, dict):
            return self._reject(candle, ["not_a_dict"], "invalid_ohlc")

        # 字段必需性
        required = ("symbol", "interval", "open_time",
                    "open", "high", "low", "close", "volume")
        for f in required:
            if f not in candle:
                return self._reject(
                    candle, [f"missing_field:{f}"], "invalid_ohlc")

        try:
            o = float(candle["open"])
            h = float(candle["high"])
            l = float(candle["low"])
            c = float(candle["close"])
            v = float(candle["volume"])
        except (TypeError, ValueError):
            return self._reject(candle, ["non_numeric"], "invalid_ohlc")

        # NaN / Inf
        for name, val in (("open", o), ("high", h),
                          ("low", l), ("close", c),
                          ("volume", v)):
            if math.isnan(val) or math.isinf(val):
                with self._lock:
                    self._stats.nan_inf += 1
                return self._reject(
                    candle, [f"nan_inf:{name}"], "nan_inf")

        # 正数检查
        if o <= 0 or h <= 0 or l <= 0 or c <= 0:
            with self._lock:
                self._stats.non_positive += 1
            return self._reject(
                candle, ["non_positive_price"], "non_positive")
        if v < 0:
            with self._lock:
                self._stats.non_positive += 1
            return self._reject(
                candle, ["negative_volume"], "non_positive")

        # OHLC 关系
        if h < l:
            with self._lock:
                self._stats.invalid_ohlc += 1
            return self._reject(
                candle, ["high_lt_low"], "invalid_ohlc")
        if not (l <= o <= h):
            with self._lock:
                self._stats.invalid_ohlc += 1
            return self._reject(
                candle, ["open_out_of_range"], "invalid_ohlc")
        if not (l <= c <= h):
            with self._lock:
                self._stats.invalid_ohlc += 1
            return self._reject(
                candle, ["close_out_of_range"], "invalid_ohlc")

        # 2. 获取序列历史
        symbol = str(candle["symbol"]).lower()
        interval = str(candle["interval"])
        key = (symbol, interval)
        with self._lock:
            hist = self._history.get(key)
            if hist is None:
                hist = _SeriesHistory(self.config.history_size)
                self._history[key] = hist
            history_size = hist.size()
            median_range = hist.median_range()
            median_volume = hist.median_volume()
            median_close = hist.median_close()

        # 3. 成交量异常检测
        volume_flag = ""
        if self.config.enable_volume_check \
                and median_volume is not None \
                and median_volume > 0 and history_size >= 20:
            if v < median_volume * self.config.low_volume_ratio:
                volume_flag = "low_volume"
                with self._lock:
                    self._stats.low_volume += 1
            elif v > median_volume * self.config.high_volume_ratio:
                volume_flag = "high_volume"
                with self._lock:
                    self._stats.high_volume += 1

        # 4. 插针检测与修正
        corrected: Dict[str, Tuple[Any, Any]] = {}
        out = dict(candle)
        if self.config.enable_spike_filter \
                and history_size >= 20 \
                and median_range is not None:
            # 用中位数范围作为尺度
            # 波动率自适应：以历史中位数范围为基准
            scale = median_range
            if self.config.enable_volatility_scaling:
                # 单根 K 线自身范围过大时，允许放宽
                current_range = h - l
                scale = max(scale, current_range * 0.1)
            scale = max(scale, MIN_MAD_ABS,
                        (median_close or c) * MIN_MAD_RATIO)

            # 检测 high 是否过高
            threshold = scale * self.config.mad_k
            if h - l > 0:
                # high 明显偏离当前 K 线本身形态
                if (h - c) > threshold and (h - o) > threshold:
                    new_h = max(o, c, l) + scale * self.config.spike_k
                    if new_h < h:
                        corrected["high"] = (h, new_h)
                        out["high"] = new_h
                        h = new_h
                        with self._lock:
                            self._stats.spikes_high += 1
                if (l - c) < -threshold and (l - o) < -threshold:
                    new_l = min(o, c, h) - scale * self.config.spike_k
                    if new_l > l:
                        corrected["low"] = (l, new_l)
                        out["low"] = new_l
                        l = new_l
                        with self._lock:
                            self._stats.spikes_low += 1

        # 5. 修正后重新校验 OHLC
        if corrected:
            o2, h2, l2, c2 = (
                float(out["open"]), float(out["high"]),
                float(out["low"]), float(out["close"]))
            if h2 < l2 or not (l2 <= o2 <= h2) \
                    or not (l2 <= c2 <= h2):
                # 修正导致非法，退回原值并拒绝
                with self._lock:
                    self._stats.rejected += 1
                return CleanResult(
                    candle=None,
                    action=CleanAction.REJECTED,
                    reasons=["correction_invalidated_ohlc"],
                    original=dict(candle),
                )

        # 6. 更新历史
        with self._lock:
            hist.append(out)

        # 7. 发布事件
        if corrected:
            self._publish("data.cleaned", {
                "symbol": symbol,
                "interval": interval,
                "open_time": out.get("open_time"),
                "corrected": {k: {"old": v2[0], "new": v2[1]}
                              for k, v2 in corrected.items()},
                "ts_ms": _now_ms(),
            })

        # 8. 返回
        if corrected:
            with self._lock:
                self._stats.corrected += 1
            return CleanResult(
                candle=out,
                action=CleanAction.CORRECTED,
                reasons=[f"corrected:{k}" for k in corrected] +
                        ([volume_flag] if volume_flag else []),
                original=dict(candle),
                corrected_fields=corrected,
            )

        with self._lock:
            self._stats.passed += 1
        return CleanResult(
            candle=out,
            action=CleanAction.PASS,
            reasons=([volume_flag] if volume_flag else []),
            original=dict(candle),
        )

    def clean_batch(
        self, candles: List[Dict[str, Any]],
    ) -> List[Dict[str, Any]]:
        """批量清洗，返回清洗后的 K 线列表（已剔除拒绝项）"""
        if not isinstance(candles, list):
            return []
        results: List[Dict[str, Any]] = []
        for c in candles:
            r = self.clean(c)
            if r.is_valid:
                results.append(r.candle)
        return results

    def clean_with_report(
        self, candles: List[Dict[str, Any]],
    ) -> Tuple[List[Dict[str, Any]], List[CleanResult]]:
        """返回 (clean_list, all_results)"""
        if not isinstance(candles, list):
            return [], []
        cleaned: List[Dict[str, Any]] = []
        results: List[CleanResult] = []
        for c in candles:
            r = self.clean(c)
            results.append(r)
            if r.is_valid:
                cleaned.append(r.candle)
        return cleaned, results

    def _reject(
        self, candle: Any, reasons: List[str], kind: str,
    ) -> CleanResult:
        with self._lock:
            self._stats.rejected += 1
        self._publish("data.rejected", {
            "reasons": reasons,
            "kind": kind,
            "ts_ms": _now_ms(),
        })
        return CleanResult(
            candle=None,
            action=CleanAction.REJECTED,
            reasons=reasons,
            original=candle if isinstance(candle, dict) else None,
        )

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
    def stats(self) -> Dict[str, Any]:
        with self._lock:
            s = self._stats.to_dict()
            s["pass_rate"] = round(self._stats.pass_rate(), 4)
            s["reject_rate"] = round(self._stats.reject_rate(), 4)
            s["series_count"] = len(self._history)
            return s

    def health(self) -> Dict[str, Any]:
        with self._lock:
            total = self._stats.total
            reject_rate = self._stats.reject_rate()
            series_count = len(self._history)
        if total == 0:
            status = "ok"  # 尚未处理数据
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
            "series_count": series_count,
            "stats": dict(self._stats.to_dict()),
        }

    def reset_stats(self) -> None:
        with self._lock:
            self._stats = CleanerStats()

    def clear_history(self) -> None:
        with self._lock:
            self._history.clear()

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<CandleCleaner total={self._stats.total} "
                f"corrected={self._stats.corrected} "
                f"rejected={self._stats.rejected}>"
            )


# ============================================================
# 全局入口
# ============================================================
_cleaner_instance: Optional[CandleCleaner] = None
_cleaner_lock = threading.Lock()


def init_cleaner(
    config: Optional[CleanerConfig] = None,
) -> CandleCleaner:
    """显式初始化（启动时调用）"""
    global _cleaner_instance
    with _cleaner_lock:
        _cleaner_instance = CandleCleaner(config)
    return _cleaner_instance


def get_cleaner() -> CandleCleaner:
    """获取单例（惰性初始化）"""
    global _cleaner_instance
    if _cleaner_instance is None:
        with _cleaner_lock:
            if _cleaner_instance is None:
                _cleaner_instance = CandleCleaner()
    return _cleaner_instance


# ============================================================
# 便捷函数
# ============================================================
def clean_candle(candle: Dict[str, Any]) -> Dict[str, Any]:
    """便捷：清洗单根，返回清洗后的 K 线或原始 K 线"""
    r = get_cleaner().clean(candle)
    return r.candle if r.is_valid else candle


def clean_batch(
    candles: List[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    """便捷：批量清洗"""
    return get_cleaner().clean_batch(candles)
