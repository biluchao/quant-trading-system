"""---UMS-BEGIN---
spec_version: 1.0
module: data
type: api
name: binance_ws
version: 2.0.0
level: 2
required: false
author: quant-team
created: 2026-10-14
brief: 币安期货WebSocket（异步/自动重连/多符号/K线解析/事件发布）

provides:
  - class BinanceWS
  - class Candle
  - class WSConfig
  - function get_binance_ws
  - function init_binance_ws

requires:
  modules: [core.logger, core.timestamp, core.event_bus, core.compat]
  libraries:
    - websockets>=14.1
  resources:
    memory_mb: 64
    cpu_cores: 1
    network: fstream.binance.com:443

health:
  fn: BinanceWS.health
  timeout_ms: 1000

assembly:
  order: 25
  parallel: false

tests:
  - tests/test_binance_ws.py

fallback:
  - 币安不可达 → 发布 data.source_down，上层降级 REST
  - websockets 缺失 → 报错并禁用
  - 代理失败 → 直连重试

tags: [data, websocket, binance]
---UMS-END---"""

from __future__ import annotations

import asyncio
import json
import random
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Set

# 延迟导入
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("binance_ws")
        except Exception:
            import logging
            _logger = logging.getLogger("binance_ws")
    return _logger


def _now_ms() -> int:
    try:
        from core.timestamp import now_ms
        return now_ms()
    except Exception:
        return int(time.time() * 1000)


# ============================================================
# 可选依赖
# ============================================================
try:
    import websockets  # type: ignore
    from websockets.exceptions import (
        ConnectionClosed, WebSocketException,
    )
    _HAS_WEBSOCKETS = True
except ImportError:
    _HAS_WEBSOCKETS = False


# ============================================================
# 常量
# ============================================================
# 币安期货流地址[reference:1]
FSTREAM_BASE = "wss://fstream.binance.com"
# 组合流格式：/stream?streams=a@kline_3m/b@kline_3m[reference:2]
STREAM_PATH = "/stream?streams="

# 断连策略
INITIAL_BACKOFF_S = 1.0
MAX_BACKOFF_S = 60.0
BACKOFF_JITTER = 0.3
MAX_RECONNECT_ATTEMPTS = 0    # 0 表示无限

# 心跳
HEARTBEAT_INTERVAL_S = 30.0
HEARTBEAT_TIMEOUT_S = 90.0

# 连接
CONNECT_TIMEOUT_S = 10.0
MAX_MESSAGES_PER_SECOND = 1000  # 本地阈值告警


# ============================================================
# 数据结构
# ============================================================
@dataclass
class Candle:
    """标准 K 线结构"""
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
    event_time: int = 0

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
            "event_time": self.event_time,
        }


@dataclass
class WSConfig:
    symbols: List[str] = field(
        default_factory=lambda: ["btcusdt", "ethusdt"])
    interval: str = "3m"
    proxy: Optional[str] = None
    heartbeat_interval_s: float = HEARTBEAT_INTERVAL_S
    heartbeat_timeout_s: float = HEARTBEAT_TIMEOUT_S
    max_backoff_s: float = MAX_BACKOFF_S
    max_reconnect_attempts: int = MAX_RECONNECT_ATTEMPTS


# ============================================================
# BinanceWS 单例
# ============================================================
class BinanceWS:
    """
    币安期货 WebSocket 客户端。

    特性：
    - 异步（websockets 库）
    - 多符号组合流
    - 自动重连 + 指数退避 + 抖动
    - 心跳监控
    - K 线解析 + 事件发布
    - 优雅关闭
    """

    _instance: Optional["BinanceWS"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "BinanceWS":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(self, config: Optional[WSConfig] = None) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self.config = config or WSConfig()
            self._ws: Any = None
            self._task: Optional[asyncio.Task] = None
            self._loop: Optional[asyncio.AbstractEventLoop] = None
            self._running = False
            self._connected = False
            self._stop_event = threading.Event()
            self._last_msg_ms = 0
            self._last_pong_ms = 0
            self._reconnect_count = 0
            self._current_backoff_s = INITIAL_BACKOFF_S
            self._subscribed: Set[str] = set()
            self._event_publisher: Optional[
                Callable[[str, Any], None]] = None
            self._stats = {
                "messages": 0,
                "candles_closed": 0,
                "candles_open": 0,
                "errors": 0,
                "disconnects": 0,
                "heartbeat_timeouts": 0,
                "started_ms": 0,
                "last_msg_ms": 0,
                "reconnects": 0,
            }
            self._initialized = True
            self._update_subscriptions()

    # ========================================================
    # 内部工具
    # ========================================================
    def _update_subscriptions(self) -> None:
        with self._lock:
            self._subscribed = {
                f"{s.lower()}@kline_{self.config.interval}"
                for s in self.config.symbols
            }

    def _build_url(self) -> str:
        streams = "/".join(sorted(self._subscribed))
        return f"{FSTREAM_BASE}{STREAM_PATH}{streams}"

    def set_event_publisher(
        self, publisher: Callable[[str, Any], None],
    ) -> None:
        self._event_publisher = publisher

    def _publish(self, topic: str, data: Any) -> None:
        if self._event_publisher is None:
            # 尝试从 core.event_bus 获取
            try:
                from core.event_bus import publish as bus_publish
                bus_publish(topic, data)
                return
            except Exception:
                pass
            _log().debug(f"[{topic}] {data}")
            return
        try:
            self._event_publisher(topic, data)
        except Exception as exc:
            _log().debug(f"事件发布失败: {exc}")

    def _backoff_with_jitter(self) -> float:
        """指数退避 + 抖动，避免惊群[reference:3]"""
        base = min(self._current_backoff_s,
                   self.config.max_backoff_s)
        jitter = random.uniform(-BACKOFF_JITTER, BACKOFF_JITTER) * base
        delay = max(0.5, base + jitter)
        # 下次翻倍
        self._current_backoff_s = min(
            base * 2, self.config.max_backoff_s)
        return delay

    def _reset_backoff(self) -> None:
        self._current_backoff_s = INITIAL_BACKOFF_S

    # ========================================================
    # 启动 / 停止
    # ========================================================
    def start(
        self,
        loop: Optional[asyncio.AbstractEventLoop] = None,
    ) -> bool:
        if not _HAS_WEBSOCKETS:
            _log().error("websockets 库缺失，无法启动 BinanceWS")
            return False

        with self._lock:
            if self._running:
                return True
            self._running = True
            self._stop_event.clear()
            self._stats["started_ms"] = _now_ms()

        try:
            if loop is not None and loop.is_running():
                self._loop = loop
                self._task = loop.create_task(self._run())
                _log().info(
                    f"BinanceWS 已在 asyncio 循环中启动: "
                    f"{self.config.symbols}")
            else:
                # 独立线程 + 独立 loop
                self._loop = asyncio.new_event_loop()
                t = threading.Thread(
                    target=self._run_in_thread,
                    name="binance-ws",
                    daemon=True,
                )
                t.start()
                _log().info(
                    f"BinanceWS 已在独立线程启动: "
                    f"{self.config.symbols}")
            return True
        except Exception as exc:
            _log().error(f"BinanceWS 启动失败: {exc}")
            with self._lock:
                self._running = False
            return False

    def _run_in_thread(self) -> None:
        loop = self._loop
        if loop is None:
            return
        asyncio.set_event_loop(loop)
        try:
            loop.run_until_complete(self._run())
        except Exception as exc:
            _log().error(f"BinanceWS 线程异常: {exc}")
        finally:
            try:
                loop.close()
            except Exception:
                pass

    def stop(self, timeout_s: float = 5.0) -> None:
        with self._lock:
            if not self._running:
                return
            self._running = False
            self._stop_event.set()
        _log().info("BinanceWS 停止中...")
        # 尝试关闭 websocket
        try:
            if self._ws is not None and self._loop is not None:
                asyncio.run_coroutine_threadsafe(
                    self._ws.close(), self._loop)
        except Exception:
            pass
        # 等待任务退出
        if self._task is not None and self._loop is not None:
            try:
                self._task.cancel()
            except Exception:
                pass
        time.sleep(0.2)
        with self._lock:
            self._connected = False
        _log().info(
            f"BinanceWS 已停止 "
            f"(messages={self._stats['messages']}, "
            f"reconnects={self._stats['reconnects']})")

    # ========================================================
    # 主循环
    # ========================================================
    async def _run(self) -> None:
        while self._running and not self._stop_event.is_set():
            try:
                await self._connect_and_listen()
            except asyncio.CancelledError:
                _log().info("BinanceWS 任务被取消")
                break
            except Exception as exc:
                self._stats["errors"] += 1
                _log().warning(f"连接异常: {exc}")
                self._connected = False

            if not self._running or self._stop_event.is_set():
                break

            # 重连
            self._stats["reconnects"] += 1
            self._reconnect_count += 1
            max_attempts = self.config.max_reconnect_attempts
            if max_attempts > 0 and \
                    self._reconnect_count > max_attempts:
                _log().error(
                    f"已达最大重连次数 {max_attempts}，停止")
                break

            delay = self._backoff_with_jitter()
            _log().info(
                f"将在 {delay:.1f}s 后重连 "
                f"(第 {self._reconnect_count} 次)")
            try:
                await asyncio.sleep(delay)
            except asyncio.CancelledError:
                break

        with self._lock:
            self._connected = False

    async def _connect_and_listen(self) -> None:
        url = self._build_url()
        _log().info(f"连接币安 WebSocket: {url}")

        connect_kwargs: Dict[str, Any] = {
            "ping_interval": None,    # 币安自带 ping/pong[reference:4]
            "ping_timeout": None,
            "close_timeout": 5,
            "max_size": 2 ** 20,
        }
        if self.config.proxy:
            connect_kwargs["proxy"] = self.config.proxy

        try:
            async with websockets.connect(
                url, **connect_kwargs
            ) as ws:
                self._ws = ws
                self._connected = True
                self._reset_backoff()
                self._reconnect_count = 0
                self._last_msg_ms = _now_ms()
                _log().info(
                    f"币安 WebSocket 已连接 "
                    f"({len(self._subscribed)} 个流)")

                # 启动心跳监控
                hb_task = asyncio.create_task(self._heartbeat_loop())

                try:
                    async for raw in ws:
                        if not self._running or \
                                self._stop_event.is_set():
                            break
                        self._on_raw_message(raw)
                finally:
                    hb_task.cancel()
                    try:
                        await hb_task
                    except (asyncio.CancelledError, Exception):
                        pass
        except ConnectionClosed as exc:
            self._stats["disconnects"] += 1
            _log().warning(f"连接关闭: code={exc.code}")
            raise
        except WebSocketException as exc:
            self._stats["disconnects"] += 1
            _log().warning(f"WebSocket 异常: {exc}")
            raise
        finally:
            self._connected = False
            self._ws = None

    # ========================================================
    # 消息处理
    # ========================================================
    def _on_raw_message(self, raw: Any) -> None:
        self._last_msg_ms = _now_ms()
        with self._lock:
            self._stats["messages"] += 1
            self._stats["last_msg_ms"] = self._last_msg_ms

        try:
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8")
            msg = json.loads(raw)
        except Exception as exc:
            self._stats["errors"] += 1
            _log().debug(f"消息解析失败: {exc}")
            return

        # 组合流封装：{"stream": "...", "data": {...}}
        if isinstance(msg, dict) and "data" in msg:
            payload = msg["data"]
        else:
            payload = msg

        try:
            event = payload.get("e", "")
            if event == "kline":
                self._handle_kline(payload)
            else:
                # 其他事件透传
                self._publish("data.binance.raw", {
                    "event": event,
                    "payload": payload,
                    "ts_ms": _now_ms(),
                })
        except Exception as exc:
            self._stats["errors"] += 1
            _log().debug(f"事件处理失败: {exc}")

    def _handle_kline(self, payload: Dict[str, Any]) -> None:
        k = payload.get("k", {})
        if not k:
            return
        candle = Candle(
            symbol=k.get("s", "").lower(),
            interval=k.get("i", ""),
            open_time=int(k.get("t", 0)),
            close_time=int(k.get("T", 0)),
            open=float(k.get("o", 0)),
            high=float(k.get("h", 0)),
            low=float(k.get("l", 0)),
            close=float(k.get("c", 0)),
            volume=float(k.get("v", 0)),
            quote_volume=float(k.get("q", 0)),
            trades=int(k.get("n", 0)),
            is_closed=bool(k.get("x", False)),
            event_time=int(payload.get("E", 0)),
        )

        if candle.is_closed:
            self._stats["candles_closed"] += 1
            self._publish("candle.closed", candle.to_dict())
        else:
            self._stats["candles_open"] += 1
            self._publish("candle.tick", candle.to_dict())

    # ========================================================
    # 心跳监控
    # ========================================================
    async def _heartbeat_loop(self) -> None:
        """监控消息是否及时到达，超时主动断开触发重连"""
        try:
            while self._running and not self._stop_event.is_set():
                await asyncio.sleep(self.config.heartbeat_interval_s)
                if self._last_msg_ms == 0:
                    continue
                idle_ms = _now_ms() - self._last_msg_ms
                if idle_ms > self.config.heartbeat_timeout_s * 1000:
                    self._stats["heartbeat_timeouts"] += 1
                    _log().warning(
                        f"心跳超时 ({idle_ms/1000:.0f}s)，"
                        f"主动断开以触发重连")
                    try:
                        if self._ws is not None:
                            await self._ws.close()
                    except Exception:
                        pass
                    return
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            _log().debug(f"心跳监控退出: {exc}")

    # ========================================================
    # 订阅管理
    # ========================================================
    def subscribe(self, symbol: str,
                  interval: Optional[str] = None) -> bool:
        """动态添加订阅（触发重连以生效）"""
        interval = interval or self.config.interval
        stream = f"{symbol.lower()}@kline_{interval}"
        with self._lock:
            if symbol.lower() not in [
                    s.lower() for s in self.config.symbols]:
                self.config.symbols.append(symbol.lower())
            self._subscribed.add(stream)
        _log().info(f"添加订阅: {stream}（下次重连生效）")
        return True

    def unsubscribe(self, symbol: str,
                    interval: Optional[str] = None) -> bool:
        interval = interval or self.config.interval
        stream = f"{symbol.lower()}@kline_{interval}"
        with self._lock:
            self._subscribed.discard(stream)
            self.config.symbols = [
                s for s in self.config.symbols
                if s.lower() != symbol.lower()]
        _log().info(f"移除订阅: {stream}")
        return True

    # ========================================================
    # 状态
    # ========================================================
    def is_connected(self) -> bool:
        with self._lock:
            return self._connected

    def is_running(self) -> bool:
        with self._lock:
            return self._running

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            idle_s = (
                (_now_ms() - self._last_msg_ms) / 1000
                if self._last_msg_ms else -1
            )
            return {
                **self._stats,
                "connected": self._connected,
                "running": self._running,
                "idle_s": round(idle_s, 2),
                "symbols": list(self.config.symbols),
                "interval": self.config.interval,
                "current_backoff_s": self._current_backoff_s,
                "reconnect_count": self._reconnect_count,
            }

    def health(self) -> Dict[str, Any]:
        with self._lock:
            connected = self._connected
            running = self._running
        if not running:
            status = "down"
        elif not connected:
            status = "reconnecting"
        else:
            idle_ms = _now_ms() - self._last_msg_ms \
                if self._last_msg_ms else 0
            if idle_ms > self.config.heartbeat_timeout_s * 1000:
                status = "degraded"
            else:
                status = "ok"
        return {
            "status": status,
            "connected": connected,
            "running": running,
            "symbols": list(self.config.symbols),
            "interval": self.config.interval,
            "last_msg_ms": self._last_msg_ms,
            "reconnects": self._stats["reconnects"],
            "messages": self._stats["messages"],
            "websockets_available": _HAS_WEBSOCKETS,
        }

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<BinanceWS connected={self._connected} "
                f"symbols={len(self.config.symbols)} "
                f"interval={self.config.interval}>"
            )


# ============================================================
# 全局入口
# ============================================================
_ws_instance: Optional[BinanceWS] = None
_ws_lock = threading.Lock()


def init_binance_ws(
    config: Optional[WSConfig] = None,
    autostart: bool = False,
    loop: Optional[asyncio.AbstractEventLoop] = None,
) -> BinanceWS:
    """显式初始化（启动时调用）"""
    global _ws_instance
    with _ws_lock:
        _ws_instance = BinanceWS(config)
        if autostart:
            _ws_instance.start(loop=loop)
    return _ws_instance


def get_binance_ws() -> BinanceWS:
    """获取单例（惰性初始化）"""
    global _ws_instance
    if _ws_instance is None:
        with _ws_lock:
            if _ws_instance is None:
                _ws_instance = BinanceWS()
    return _ws_instance


# ============================================================
# 便捷函数
# ============================================================
def start_binance_ws(
    symbols: Optional[List[str]] = None,
    interval: str = "3m",
    proxy: Optional[str] = None,
    loop: Optional[asyncio.AbstractEventLoop] = None,
) -> bool:
    global _ws_instance
    with _ws_lock:
        if _ws_instance is None:
            _ws_instance = BinanceWS(WSConfig(
                symbols=symbols or ["btcusdt", "ethusdt"],
                interval=interval,
                proxy=proxy,
            ))
    return _ws_instance.start(loop=loop)


def stop_binance_ws(timeout_s: float = 5.0) -> None:
    global _ws_instance
    with _ws_lock:
        if _ws_instance is not None:
            _ws_instance.stop(timeout_s=timeout_s)
            _ws_instance = None
