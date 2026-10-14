"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: timestamp
version: 2.0.0
level: 0
required: true
author: quant-team
created: 2026-10-14
brief: 高精度时间同步（多源中位数/RTT补偿/漂移检测/异步）

provides:
  - class Timestamp
  - class TimeSource
  - function now_ms
  - function now_us
  - function now_s
  - function now_iso
  - function monotonic_ms
  - function signed_timestamp
  - function get_timestamp
  - function init_timestamp

requires:
  modules: [core.config, core.logger]
  libraries:
    - httpx>=0.28  (或 requests>=2.32)
    - ntplib>=0.4  (可选)
  resources:
    memory_mb: 8
    cpu_cores: 1

health:
  fn: Timestamp.health
  timeout_ms: 2000

assembly:
  order: 12
  parallel: false

tests:
  - tests/test_timestamp.py

fallback:
  - 网络不通 → 使用本地时间 + 上次偏移
  - NTP 不可用 → HTTP 同步
  - 所有同步失败 → 本地时间（记录告警）

tags: [core, time, sync]
---UMS-END---"""

from __future__ import annotations

import time
import threading
import statistics
import asyncio
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# ============================================================
# 可选依赖
# ============================================================
try:
    import httpx
    _HAS_HTTPX = True
except ImportError:
    _HAS_HTTPX = False

try:
    import requests
    _HAS_REQUESTS = True
except ImportError:
    _HAS_REQUESTS = False

try:
    import ntplib
    _HAS_NTPLIB = True
except ImportError:
    _HAS_NTPLIB = False

# 延迟导入 logger，避免循环依赖
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("timestamp")
        except Exception:
            import logging
            _logger = logging.getLogger("timestamp")
    return _logger


# ============================================================
# 时间源定义
# ============================================================
@dataclass
class TimeSource:
    """单个时间源"""
    name: str
    kind: str                # "http" | "ntp"
    url: str
    weight: float = 1.0
    timeout_ms: int = 2000
    enabled: bool = True


DEFAULT_SOURCES: List[TimeSource] = [
    TimeSource(
        name="binance_futures",
        kind="http",
        url="https://fapi.binance.com/fapi/v1/time",
        weight=1.0,
    ),
    TimeSource(
        name="binance_spot",
        kind="http",
        url="https://api.binance.com/api/v3/time",
        weight=1.0,
    ),
    TimeSource(
        name="ntp_pool",
        kind="ntp",
        url="pool.ntp.org",
        weight=1.0,
    ),
]


# ============================================================
# Timestamp（单例）
# ============================================================
class Timestamp:
    """
    高精度时间同步管理器。

    - 多源同步，中位数聚合
    - RTT 补偿：offset = server - (t0 + t1) / 2
    - 后台定时同步（默认 5 分钟）
    - 漂移检测 + 告警
    - 单调时钟独立接口
    """

    _instance: Optional["Timestamp"] = None
    _lock = threading.Lock()

    # 默认配置
    DEFAULT_SYNC_INTERVAL_S: int = 300        # 5 分钟
    DEFAULT_MIN_INTERVAL_S: int = 30          # 最小同步间隔，防抖
    DRIFT_WARN_MS: int = 500                  # 漂移告警
    DRIFT_CRITICAL_MS: int = 2000             # 严重漂移，暂停交易
    OFFSET_STALE_S: int = 900                 # 偏移过期（15 分钟）

    def __new__(cls, *args: Any, **kwargs: Any) -> "Timestamp":
        with cls._lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self,
        sources: Optional[List[TimeSource]] = None,
        sync_interval_s: int = DEFAULT_SYNC_INTERVAL_S,
        auto_start: bool = False,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._lock:
            if getattr(self, "_initialized", False):
                return
            self._inner_lock = threading.RLock()
            self._sources: List[TimeSource] = sources or list(DEFAULT_SOURCES)
            self._offset_ms: int = 0
            self._offset_ts_ms: int = 0            # 上次成功同步的时刻
            self._last_sync_attempt_ms: int = 0
            self._rtt_ms: int = 0
            self._source_used: str = "local"
            self._sync_count: int = 0
            self._error_count: int = 0
            self._last_error: str = ""
            self._drift_history: List[int] = []
            self._sync_interval_s: int = max(
                sync_interval_s, self.DEFAULT_MIN_INTERVAL_S)
            self._bg_thread: Optional[threading.Thread] = None
            self._bg_stop = threading.Event()
            self._initialized = True

            # 首次同步（阻塞，但容错）
            self.sync()

            if auto_start:
                self.start_background_sync()

    # ========================================================
    # 时间获取
    # ========================================================
    def now_ms(self) -> int:
        """毫秒时间戳（含偏移）"""
        return int(time.time() * 1000) + self._offset_ms

    def now_us(self) -> int:
        """微秒时间戳（含偏移）"""
        return int(time.time() * 1_000_000) + self._offset_ms * 1000

    def now_s(self) -> int:
        """秒时间戳（含偏移）"""
        return int(time.time()) + self._offset_ms // 1000

    def now_iso(self) -> str:
        """ISO 8601 UTC 格式"""
        return datetime.fromtimestamp(
            self.now_ms() / 1000, tz=timezone.utc
        ).isoformat(timespec="milliseconds")

    @staticmethod
    def monotonic_ms() -> int:
        """单调时钟（毫秒），不受系统时间调整影响，用于测量耗时"""
        return int(time.monotonic() * 1000)

    @staticmethod
    def monotonic_us() -> int:
        """单调时钟（微秒）"""
        return int(time.monotonic() * 1_000_000)

    def signed_timestamp(self, recv_window_ms: int = 5000) -> Dict[str, int]:
        """
        生成币安签名所需的时间戳参数。
        返回：{"timestamp": ms, "recvWindow": ms}
        """
        return {
            "timestamp": self.now_ms(),
            "recvWindow": int(recv_window_ms),
        }

    # ========================================================
    # 偏移查询
    # ========================================================
    def offset_ms(self) -> int:
        with self._inner_lock:
            return self._offset_ms

    def rtt_ms(self) -> int:
        with self._inner_lock:
            return self._rtt_ms

    def source(self) -> str:
        with self._inner_lock:
            return self._source_used

    def is_stale(self) -> bool:
        """偏移量是否过期"""
        with self._inner_lock:
            if self._offset_ts_ms == 0:
                return True
            age_s = (self.now_ms() - self._offset_ts_ms) / 1000
            return age_s > self.OFFSET_STALE_S

    # ========================================================
    # 同步
    # ========================================================
    def sync(self, force: bool = False) -> bool:
        """
        执行同步：多源并发探测 + 中位数聚合。
        返回是否成功。
        """
        now_local = int(time.time() * 1000)
        with self._inner_lock:
            # 节流（除强制外）
            if not force and self._last_sync_attempt_ms > 0:
                delta_s = (now_local - self._last_sync_attempt_ms) / 1000
                if delta_s < self.DEFAULT_MIN_INTERVAL_S:
                    return False
            self._last_sync_attempt_ms = now_local

        # 并发同步各源
        results: List[Tuple[str, int, int]] = []  # (source_name, offset_ms, rtt_ms)
        threads: List[threading.Thread] = []
        results_lock = threading.Lock()

        def probe(src: TimeSource) -> None:
            try:
                if src.kind == "http":
                    offset, rtt = self._probe_http(src)
                elif src.kind == "ntp":
                    offset, rtt = self._probe_ntp(src)
                else:
                    return
                with results_lock:
                    results.append((src.name, offset, rtt))
            except Exception as exc:
                _log().warning(
                    f"时间源 {src.name} 探测失败: {exc}")

        for src in self._sources:
            if not src.enabled:
                continue
            t = threading.Thread(target=probe, args=(src,), daemon=True)
            threads.append(t)
            t.start()

        for t in threads:
            t.join(timeout=3.0)

        if not results:
            with self._inner_lock:
                self._error_count += 1
                self._last_error = "所有时间源同步失败"
            _log().warning("时间同步失败：使用本地时间 + 上次偏移")
            return False

        # 中位数聚合
        offsets = [r[1] for r in results]
        median_offset = int(statistics.median(offsets))
        rtts = [r[2] for r in results]
        median_rtt = int(statistics.median(rtts)) if rtts else 0
        # 用于标识的主要源（离中位数最近）
        best = min(results, key=lambda r: abs(r[1] - median_offset))

        with self._inner_lock:
            old_offset = self._offset_ms
            self._offset_ms = median_offset
            self._rtt_ms = median_rtt
            self._offset_ts_ms = self.now_ms()
            self._source_used = best[0]
            self._sync_count += 1

            # 漂移检测
            drift = abs(median_offset - old_offset)
            self._drift_history.append(drift)
            if len(self._drift_history) > 100:
                self._drift_history = self._drift_history[-100:]

            if drift >= self.DRIFT_CRITICAL_MS:
                _log().error(
                    f"⚠️ 时间严重漂移 {drift}ms，源={best[0]}，"
                    f"旧偏移={old_offset}ms 新偏移={median_offset}ms")
            elif drift >= self.DRIFT_WARN_MS:
                _log().warning(
                    f"时间漂移 {drift}ms，源={best[0]}")

        _log().info(
            f"时间同步成功: 源={best[0]} 偏移={median_offset}ms "
            f"RTT={median_rtt}ms (共 {len(results)} 个源)")
        return True

    async def async_sync(self, force: bool = False) -> bool:
        """异步版本"""
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, self.sync, force)

    # ========================================================
    # 单源探测
    # ========================================================
    def _probe_http(self, src: TimeSource) -> Tuple[int, int]:
        """
        HTTP 探测：RTT 补偿公式
        offset = server_time - (t0 + t1) / 2
        RTT = t1 - t0
        """
        t0 = int(time.time() * 1000)
        if _HAS_HTTPX:
            resp = httpx.get(src.url, timeout=src.timeout_ms / 1000.0)
            resp.raise_for_status()
            server_time = int(resp.json()["serverTime"])
        elif _HAS_REQUESTS:
            resp = requests.get(src.url, timeout=src.timeout_ms / 1000.0)
            resp.raise_for_status()
            server_time = int(resp.json()["serverTime"])
        else:
            raise RuntimeError("缺少 httpx 或 requests")

        t1 = int(time.time() * 1000)
        rtt = t1 - t0
        offset = server_time - (t0 + t1) // 2
        return offset, rtt

    def _probe_ntp(self, src: TimeSource) -> Tuple[int, int]:
        if not _HAS_NTPLIB:
            raise RuntimeError("缺少 ntplib")
        client = ntplib.NTPClient()
        resp = client.request(src.url, version=3,
                              timeout=src.timeout_ms / 1000.0)
        # resp.offset 是秒
        offset = int(resp.offset * 1000)
        # NTP 的 delay 是往返延迟（秒）
        rtt = int(getattr(resp, "delay", 0.0) * 1000)
        return offset, rtt

    # ========================================================
    # 后台同步
    # ========================================================
    def start_background_sync(self) -> None:
        if self._bg_thread is not None and self._bg_thread.is_alive():
            return
        self._bg_stop.clear()
        self._bg_thread = threading.Thread(
            target=self._bg_loop, daemon=True, name="ts-sync")
        self._bg_thread.start()
        _log().info(
            f"后台时间同步已启动，间隔 {self._sync_interval_s}s")

    def stop_background_sync(self) -> None:
        self._bg_stop.set()
        if self._bg_thread is not None:
            self._bg_thread.join(timeout=2.0)
            self._bg_thread = None
        _log().info("后台时间同步已停止")

    def _bg_loop(self) -> None:
        while not self._bg_stop.is_set():
            try:
                self.sync()
            except Exception as exc:
                _log().warning(f"后台同步异常: {exc}")
            self._bg_stop.wait(self._sync_interval_s)

    # ========================================================
    # 健康检查
    # ========================================================
    def health(self) -> Dict[str, Any]:
        with self._inner_lock:
            stale = self.is_stale()
            drift_recent = (self._drift_history[-1]
                            if self._drift_history else 0)
            return {
                "status": "ok" if not stale else "degraded",
                "offset_ms": self._offset_ms,
                "rtt_ms": self._rtt_ms,
                "source": self._source_used,
                "sync_count": self._sync_count,
                "error_count": self._error_count,
                "last_error": self._last_error,
                "last_sync_ms_ago": (
                    self.now_ms() - self._offset_ts_ms
                    if self._offset_ts_ms else -1
                ),
                "is_stale": stale,
                "last_drift_ms": drift_recent,
                "background": (
                    self._bg_thread is not None
                    and self._bg_thread.is_alive()
                ),
                "sources": [s.name for s in self._sources if s.enabled],
            }

    def stats(self) -> Dict[str, Any]:
        return self.health()


# ============================================================
# 全局入口
# ============================================================
_ts_instance: Optional[Timestamp] = None
_ts_lock = threading.Lock()


def init_timestamp(
    sources: Optional[List[TimeSource]] = None,
    sync_interval_s: int = Timestamp.DEFAULT_SYNC_INTERVAL_S,
    auto_start: bool = True,
) -> Timestamp:
    """显式初始化（启动时调用）"""
    global _ts_instance
    with _ts_lock:
        _ts_instance = Timestamp(
            sources=sources,
            sync_interval_s=sync_interval_s,
            auto_start=auto_start,
        )
    return _ts_instance


def get_timestamp() -> Timestamp:
    """获取全局实例（惰性初始化）"""
    global _ts_instance
    if _ts_instance is None:
        with _ts_lock:
            if _ts_instance is None:
                _ts_instance = Timestamp(auto_start=False)
    return _ts_instance


# ============================================================
# 便捷函数
# ============================================================
def now_ms() -> int:
    return get_timestamp().now_ms()


def now_us() -> int:
    return get_timestamp().now_us()


def now_s() -> int:
    return get_timestamp().now_s()


def now_iso() -> str:
    return get_timestamp().now_iso()


def monotonic_ms() -> int:
    return Timestamp.monotonic_ms()


def monotonic_us() -> int:
    return Timestamp.monotonic_us()


def signed_timestamp(recv_window_ms: int = 5000) -> Dict[str, int]:
    return get_timestamp().signed_timestamp(recv_window_ms)
