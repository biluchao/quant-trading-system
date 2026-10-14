"""---UMS-BEGIN---
spec_version: 1.0
module: data
type: api
name: binance_rest
version: 2.0.0
level: 2
required: false
author: quant-team
created: 2026-10-14
brief: 币安期货REST客户端（异步/签名/限流/重试/缓存/权重感知）

provides:
  - class BinanceREST
  - class RESTSigning
  - class RateLimiter
  - class ResponseCache
  - function get_binance_rest
  - function init_binance_rest

requires:
  modules: [core.logger, core.timestamp, core.secrets]
  libraries:
    - httpx>=0.28
    - tenacity>=9.0
  resources:
    memory_mb: 32
    cpu_cores: 1
    network: fapi.binance.com:443

health:
  fn: BinanceREST.health
  timeout_ms: 3000

assembly:
  order: 26
  parallel: false

tests:
  - tests/test_binance_rest.py

fallback:
  - fapi.binance.com 不可达 → 尝试 fapi2/fapi3
  - 429 限流 → 指数退避重试
  - 418 封禁 → 立即停止所有请求
  - 5xx 服务错误 → 重试 3 次

tags: [data, rest, binance]
---UMS-END---"""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import threading
import time
import urllib.parse
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

# 延迟导入
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("binance_rest")
        except Exception:
            import logging
            _logger = logging.getLogger("binance_rest")
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
    import httpx  # type: ignore
    _HAS_HTTPX = True
except ImportError:
    _HAS_HTTPX = False

try:
    from tenacity import (  # type: ignore
        retry, stop_after_attempt, wait_exponential,
        retry_if_exception_type, RetryError,
    )
    _HAS_TENACITY = True
except ImportError:
    _HAS_TENACITY = False


# ============================================================
# 常量
# ============================================================
# 多 base URL 降级
BASE_URLS = [
    "https://fapi.binance.com",
    "https://fapi2.binance.com",
    "https://fapi3.binance.com",
]

# 默认权重（每请求，按端点实际权重调整）
DEFAULT_WEIGHT = 1
WEIGHT_LIMIT_PER_MINUTE = 2400
ORDER_LIMIT_PER_MINUTE = 1200
ORDER_LIMIT_PER_10S = 300

# 超时
CONNECT_TIMEOUT_S = 5.0
READ_TIMEOUT_S = 10.0

# 重试
MAX_RETRIES = 3
BACKOFF_BASE_S = 0.5
BACKOFF_MAX_S = 8.0

# 缓存
DEFAULT_CACHE_TTL_S = 5.0


# ============================================================
# 异常
# ============================================================
class BinanceRESTError(Exception):
    """REST 客户端基类异常"""


class BinanceAPIError(BinanceRESTError):
    """交易所返回业务错误"""
    def __init__(self, code: int, msg: str,
                 status_code: int = 0) -> None:
        super().__init__(f"[{code}] {msg} (http={status_code})")
        self.code = code
        self.msg = msg
        self.status_code = status_code


class RateLimitExceeded(BinanceRESTError):
    """触发币安限流"""


class IPBanned(BinanceRESTError):
    """IP 被封禁（418）"""


class SigningError(BinanceRESTError):
    """签名错误"""


class NetworkError(BinanceRESTError):
    """网络层异常"""


# ============================================================
# 签名
# ============================================================
class RESTSigning:
    """HMAC-SHA256 签名工具"""

    @staticmethod
    def sign(secret: str, query: str) -> str:
        return hmac.new(
            secret.encode("utf-8"),
            query.encode("utf-8"),
            hashlib.sha256,
        ).hexdigest()

    @staticmethod
    def build_query(params: Dict[str, Any]) -> str:
        """URL 编码 + 排序，避免签名歧义"""
        clean = {k: v for k, v in params.items() if v is not None}
        # 币安签名要求 key 顺序与传递一致，不排序
        return urllib.parse.urlencode(clean)

    @staticmethod
    def signed_params(
        params: Dict[str, Any], secret: str,
        timestamp_ms: int, recv_window: int = 5000,
    ) -> str:
        """生成签名后的 query 字符串"""
        params = dict(params)
        params["timestamp"] = timestamp_ms
        params["recvWindow"] = recv_window
        query = RESTSigning.build_query(params)
        sig = RESTSigning.sign(secret, query)
        return f"{query}&signature={sig}"


# ============================================================
# 限流器
# ============================================================
class RateLimiter:
    """
    权重感知的滑动窗口限流器。
    - 1 分钟权重窗口
    - 10 秒订单窗口
    """

    def __init__(
        self,
        weight_limit: int = WEIGHT_LIMIT_PER_MINUTE,
        order_limit_10s: int = ORDER_LIMIT_PER_10S,
    ) -> None:
        self._lock = threading.RLock()
        self.weight_limit = weight_limit
        self.order_limit_10s = order_limit_10s
        self._weight_used = 0
        self._weight_window_start = _now_ms()
        self._orders_10s: List[int] = []
        self._exchange_weight: int = 0
        self._last_ban_ms = 0

    def check_and_consume(
        self, weight: int = 1, is_order: bool = False,
    ) -> bool:
        with self._lock:
            now = _now_ms()
            # 窗口重置
            if now - self._weight_window_start >= 60_000:
                self._weight_used = 0
                self._weight_window_start = now
            # 10s 订单窗口
            self._orders_10s = [
                t for t in self._orders_10s if now - t < 10_000]

            if self._weight_used + weight > self.weight_limit:
                return False
            if is_order and \
                    len(self._orders_10s) + 1 > self.order_limit_10s:
                return False

            self._weight_used += weight
            if is_order:
                self._orders_10s.append(now)
            return True

    def update_from_header(self, used_weight: int) -> None:
        """从响应头更新交易所端权重"""
        with self._lock:
            self._exchange_weight = used_weight
            # 若交易所使用超限，校正本地
            if used_weight > self._weight_used:
                self._weight_used = used_weight

    def on_429(self) -> None:
        with self._lock:
            self._weight_used = self.weight_limit

    def on_418(self, ban_until_ms: int) -> None:
        with self._lock:
            self._last_ban_ms = ban_until_ms

    def is_banned(self) -> bool:
        with self._lock:
            return _now_ms() < self._last_ban_ms

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "weight_used": self._weight_used,
                "weight_limit": self.weight_limit,
                "orders_10s": len(self._orders_10s),
                "order_limit_10s": self.order_limit_10s,
                "exchange_weight": self._exchange_weight,
                "banned": self.is_banned(),
            }


# ============================================================
# 缓存
# ============================================================
class ResponseCache:
    """按 URL+params 缓存 GET 响应"""

    def __init__(self, max_size: int = 1000) -> None:
        self._lock = threading.RLock()
        self._cache: Dict[str, Tuple[float, Any]] = {}
        self._max_size = max_size
        self.hits = 0
        self.misses = 0

    def _key(self, url: str, params: Optional[Dict[str, Any]]) -> str:
        p = json.dumps(params or {}, sort_keys=True)
        return f"{url}?{p}"

    def get(self, url: str,
            params: Optional[Dict[str, Any]]) -> Optional[Any]:
        key = self._key(url, params)
        with self._lock:
            entry = self._cache.get(key)
            if entry is None:
                self.misses += 1
                return None
            expire_at, value = entry
            if _now_ms() > expire_at:
                self._cache.pop(key, None)
                self.misses += 1
                return None
            self.hits += 1
            return value

    def put(self, url: str, params: Optional[Dict[str, Any]],
            value: Any, ttl_s: float) -> None:
        key = self._key(url, params)
        with self._lock:
            if len(self._cache) >= self._max_size:
                # 简单淘汰：清理过期
                now = _now_ms()
                self._cache = {
                    k: v for k, v in self._cache.items()
                    if v[0] > now
                }
                if len(self._cache) >= self._max_size:
                    self._cache.pop(next(iter(self._cache)))
            self._cache[key] = (now_ms_add(ttl_s), value)

    def clear(self) -> None:
        with self._lock:
            self._cache.clear()

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "size": len(self._cache),
                "hits": self.hits,
                "misses": self.misses,
                "hit_rate": (
                    self.hits / (self.hits + self.misses)
                    if (self.hits + self.misses) else 0.0
                ),
            }


def now_ms_add(seconds: float) -> float:
    return _now_ms() + seconds * 1000


# ============================================================
# 配置
# ============================================================
@dataclass
class RESTConfig:
    api_key: str = ""
    api_secret: str = ""
    base_url_index: int = 0
    recv_window: int = 5000
    enable_cache: bool = True
    cache_ttl_s: float = DEFAULT_CACHE_TTL_S
    enable_rate_limit: bool = True
    max_retries: int = MAX_RETRIES


# ============================================================
# BinanceREST 单例
# ============================================================
class BinanceREST:
    """
    币安期货 REST 客户端。

    特性：
    - 异步 httpx + 连接池
    - HMAC-SHA256 签名 + 时间戳同步
    - 权重感知限流
    - 指数退避重试（tenacity）
    - GET 响应缓存
    - 429/418 特殊处理
    - 多 base URL 降级
    """

    _instance: Optional["BinanceREST"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "BinanceREST":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(self, config: Optional[RESTConfig] = None) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self.config = config or self._load_config()
            self._client: Optional[Any] = None
            self._limiter = RateLimiter()
            self._cache = ResponseCache()
            self._base_idx = self.config.base_url_index
            self._stats = {
                "requests": 0,
                "success": 0,
                "errors": 0,
                "retries": 0,
                "cache_hit": 0,
                "cache_miss": 0,
                "rate_limited": 0,
                "banned": 0,
                "started_ms": _now_ms(),
            }
            self._initialized = True

    # ========================================================
    # 配置加载
    # ========================================================
    def _load_config(self) -> RESTConfig:
        cfg = RESTConfig()
        try:
            from core.secrets import get_secrets
            s = get_secrets()
            cfg.api_key = s.get("binance_api_key", "") or ""
            cfg.api_secret = s.get("binance_api_secret", "") or ""
        except Exception as exc:
            _log().warning(f"密钥加载失败: {exc}")
        return cfg

    def _base_url(self) -> str:
        idx = min(self._base_idx, len(BASE_URLS) - 1)
        return BASE_URLS[idx]

    def _rotate_base_url(self) -> None:
        with self._lock:
            self._base_idx = (self._base_idx + 1) % len(BASE_URLS)
            _log().warning(
                f"切换到 base URL: {self._base_url()}")

    # ========================================================
    # HTTP 客户端
    # ========================================================
    async def _get_client(self) -> Any:
        if not _HAS_HTTPX:
            raise NetworkError("httpx 未安装")
        with self._lock:
            if self._client is None or self._client.is_closed:
                self._client = httpx.AsyncClient(
                    timeout=httpx.Timeout(
                        connect=CONNECT_TIMEOUT_S,
                        read=READ_TIMEOUT_S,
                        write=READ_TIMEOUT_S,
                        pool=None,
                    ),
                    limits=httpx.Limits(
                        max_connections=20,
                        max_keepalive_connections=10,
                        keepalive_expiry=30.0,
                    ),
                    headers={
                        "User-Agent": "quant-system/2.0",
                        "Accept": "application/json",
                    },
                )
            return self._client

    async def close(self) -> None:
        with self._lock:
            client = self._client
            self._client = None
        if client is not None and not client.is_closed:
            try:
                await client.aclose()
            except Exception:
                pass

    # ========================================================
    # 核心请求
    # ========================================================
    async def request(
        self,
        method: str,
        path: str,
        params: Optional[Dict[str, Any]] = None,
        *,
        signed: bool = False,
        weight: int = DEFAULT_WEIGHT,
        is_order: bool = False,
        cache_ttl_s: Optional[float] = None,
    ) -> Any:
        # 封禁检查
        if self._limiter.is_banned():
            raise IPBanned("IP 仍在封禁期，拒绝请求")

        # 限流
        if self.config.enable_rate_limit and \
                not self._limiter.check_and_consume(weight, is_order):
            self._stats["rate_limited"] += 1
            raise RateLimitExceeded(
                f"本地限流触发 (weight={weight})")

        # 缓存
        use_cache = (
            method.upper() == "GET"
            and self.config.enable_cache
            and not signed
            and cache_ttl_s != 0
        )
        if use_cache:
            cached = self._cache.get(path, params)
            if cached is not None:
                self._stats["cache_hit"] += 1
                return cached
            self._stats["cache_miss"] += 1

        # 签名
        url_params: Optional[Dict[str, Any]] = dict(params or {})
        if signed:
            if not self.config.api_key or not self.config.api_secret:
                raise SigningError("API 密钥未配置")
            qs = RESTSigning.signed_params(
                url_params, self.config.api_secret,
                _now_ms(), self.config.recv_window,
            )
            url = f"{self._base_url()}{path}?{qs}"
        else:
            url = f"{self._base_url()}{path}"

        headers = {}
        if self.config.api_key:
            headers["X-MBX-APIKEY"] = self.config.api_key

        # 请求
        self._stats["requests"] += 1
        try:
            resp = await self._do_request(method, url, headers)
        except IPBanned:
            self._stats["banned"] += 1
            raise
        except RateLimitExceeded:
            self._stats["rate_limited"] += 1
            raise
        except Exception as exc:
            self._stats["errors"] += 1
            raise NetworkError(str(exc)) from exc

        # 解析响应头权重
        used = resp.headers.get("X-MBX-USED-WEIGHT-1M")
        if used and used.isdigit():
            self._limiter.update_from_header(int(used))

        data = self._parse_response(resp)

        # 写缓存
        if use_cache:
            ttl = cache_ttl_s if cache_ttl_s is not None \
                else self.config.cache_ttl_s
            self._cache.put(path, params, data, ttl)

        self._stats["success"] += 1
        return data

    async def _do_request(
        self, method: str, url: str,
        headers: Dict[str, str],
    ) -> Any:
        """单次请求，含对 429/418/5xx 的处理"""
        client = await self._get_client()
        for attempt in range(self.config.max_retries + 1):
            try:
                resp = await client.request(
                    method, url, headers=headers)
            except httpx.TimeoutException as exc:
                if attempt >= self.config.max_retries:
                    raise NetworkError(f"超时: {exc}") from exc
                await self._sleep_backoff(attempt)
                self._stats["retries"] += 1
                continue
            except httpx.RequestError as exc:
                if attempt >= self.config.max_retries:
                    raise NetworkError(str(exc)) from exc
                await self._sleep_backoff(attempt)
                self._stats["retries"] += 1
                continue

            # 429 限流
            if resp.status_code == 429:
                retry_after = int(
                    resp.headers.get("Retry-After", "1"))
                self._limiter.on_429()
                _log().warning(
                    f"币安 429 限流，等待 {retry_after}s")
                await asyncio.sleep(min(retry_after, 60))
                self._stats["retries"] += 1
                if attempt >= self.config.max_retries:
                    raise RateLimitExceeded("多次 429")
                continue

            # 418 封禁
            if resp.status_code == 418:
                retry_after = int(
                    resp.headers.get("Retry-After", "120"))
                ban_until = _now_ms() + retry_after * 1000
                self._limiter.on_418(ban_until)
                _log().error(
                    f"币安 418 封禁 IP，等待 {retry_after}s")
                raise IPBanned(
                    f"IP 被封禁，解禁时间 {retry_after}s")

            # 5xx
            if 500 <= resp.status_code < 600:
                if attempt >= self.config.max_retries:
                    raise BinanceRESTError(
                        f"服务端错误 {resp.status_code}")
                await self._sleep_backoff(attempt)
                self._stats["retries"] += 1
                continue

            return resp

        raise NetworkError("重试耗尽")

    async def _sleep_backoff(self, attempt: int) -> None:
        delay = min(BACKOFF_BASE_S * (2 ** attempt), BACKOFF_MAX_S)
        await asyncio.sleep(delay)

    def _parse_response(self, resp: Any) -> Any:
        """解析响应，检查业务错误码"""
        status = resp.status_code
        try:
            data = resp.json()
        except Exception:
            raise BinanceRESTError(
                f"响应非 JSON (status={status}): "
                f"{resp.text[:200]}")

        if status >= 400:
            code = data.get("code", 0) if isinstance(data, dict) else 0
            msg = data.get("msg", "") if isinstance(data, dict) else ""
            raise BinanceAPIError(code, msg, status)

        # 币安有时 200 也返回错误码
        if isinstance(data, dict) and "code" in data and data["code"] < 0:
            raise BinanceAPIError(
                data["code"], data.get("msg", ""), status)

        return data

    # ========================================================
    # 常用 API
    # ========================================================
    async def server_time(self) -> int:
        data = await self.request("GET", "/fapi/v1/time", weight=1)
        return int(data.get("serverTime", 0))

    async def ping(self) -> bool:
        try:
            await self.request("GET", "/fapi/v1/ping", weight=1,
                               cache_ttl_s=0)
            return True
        except Exception:
            return False

    async def klines(
        self,
        symbol: str,
        interval: str = "3m",
        limit: int = 500,
        start_time: Optional[int] = None,
        end_time: Optional[int] = None,
    ) -> List[List[Any]]:
        params: Dict[str, Any] = {
            "symbol": symbol.upper(),
            "interval": interval,
            "limit": limit,
        }
        if start_time is not None:
            params["startTime"] = start_time
        if end_time is not None:
            params["endTime"] = end_time
        # klines 权重按 limit 递增
        weight = 1 if limit <= 100 else (2 if limit <= 500 else 5)
        return await self.request(
            "GET", "/fapi/v1/klines", params,
            weight=weight, cache_ttl_s=1.0)

    async def ticker_price(self, symbol: str) -> float:
        data = await self.request(
            "GET", "/fapi/v1/ticker/price",
            {"symbol": symbol.upper()},
            weight=1, cache_ttl_s=1.0)
        return float(data["price"])

    async def depth(self, symbol: str, limit: int = 20) -> Dict[str, Any]:
        return await self.request(
            "GET", "/fapi/v1/depth",
            {"symbol": symbol.upper(), "limit": limit},
            weight=2 if limit <= 50 else 5,
            cache_ttl_s=0.5)

    async def funding_rate(self, symbol: str) -> Dict[str, Any]:
        return await self.request(
            "GET", "/fapi/v1/premiumIndex",
            {"symbol": symbol.upper()},
            weight=1, cache_ttl_s=10.0)

    # ---- 签名端点 ----
    async def place_order(
        self,
        symbol: str,
        side: str,
        order_type: str,
        quantity: float,
        price: Optional[float] = None,
        time_in_force: str = "GTC",
        reduce_only: bool = False,
        client_order_id: Optional[str] = None,
        stop_price: Optional[float] = None,
    ) -> Dict[str, Any]:
        params: Dict[str, Any] = {
            "symbol": symbol.upper(),
            "side": side.upper(),
            "type": order_type.upper(),
            "quantity": quantity,
        }
        if price is not None:
            params["price"] = price
            params["timeInForce"] = time_in_force
        if reduce_only:
            params["reduceOnly"] = "true"
        if client_order_id:
            params["newClientOrderId"] = client_order_id
        if stop_price is not None:
            params["stopPrice"] = stop_price
        return await self.request(
            "POST", "/fapi/v1/order", params,
            signed=True, weight=1, is_order=True)

    async def cancel_order(
        self, symbol: str, order_id: Optional[int] = None,
        client_order_id: Optional[str] = None,
    ) -> Dict[str, Any]:
        params: Dict[str, Any] = {"symbol": symbol.upper()}
        if order_id is not None:
            params["orderId"] = order_id
        elif client_order_id is not None:
            params["origClientOrderId"] = client_order_id
        return await self.request(
            "DELETE", "/fapi/v1/order", params,
            signed=True, weight=1, is_order=True)

    async def open_orders(
        self, symbol: Optional[str] = None,
    ) -> List[Dict[str, Any]]:
        params: Dict[str, Any] = {}
        if symbol:
            params["symbol"] = symbol.upper()
        return await self.request(
            "GET", "/fapi/v1/openOrders", params,
            signed=True, weight=1 if symbol else 40,
            cache_ttl_s=0)

    async def position_risk(
        self, symbol: Optional[str] = None,
    ) -> List[Dict[str, Any]]:
        params: Dict[str, Any] = {}
        if symbol:
            params["symbol"] = symbol.upper()
        return await self.request(
            "GET", "/fapi/v2/positionRisk", params,
            signed=True, weight=5, cache_ttl_s=1.0)

    async def account(self) -> Dict[str, Any]:
        return await self.request(
            "GET", "/fapi/v2/account", {},
            signed=True, weight=5, cache_ttl_s=1.0)

    # ========================================================
    # 健康 & 状态
    # ========================================================
    def stats(self) -> Dict[str, Any]:
        with self._lock:
            return {
                **self._stats,
                "limiter": self._limiter.stats(),
                "cache": self._cache.stats(),
                "base_url": self._base_url(),
                "api_key_configured": bool(self.config.api_key),
            }

    def health(self) -> Dict[str, Any]:
        with self._lock:
            banned = self._limiter.is_banned()
            has_key = bool(self.config.api_key)
        if banned:
            status = "down"
        elif not _HAS_HTTPX:
            status = "down"
        elif not has_key:
            status = "degraded"
        else:
            status = "ok"
        return {
            "status": status,
            "banned": banned,
            "api_key_configured": has_key,
            "httpx_available": _HAS_HTTPX,
            "tenacity_available": _HAS_TENACITY,
            "base_url": self._base_url(),
            "stats": dict(self._stats),
        }

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<BinanceREST base={self._base_url()} "
                f"requests={self._stats['requests']} "
                f"errors={self._stats['errors']}>"
            )


# ============================================================
# 全局入口
# ============================================================
_rest_instance: Optional[BinanceREST] = None
_rest_lock = threading.Lock()


def init_binance_rest(
    config: Optional[RESTConfig] = None,
) -> BinanceREST:
    """显式初始化（启动时调用）"""
    global _rest_instance
    with _rest_lock:
        _rest_instance = BinanceREST(config)
    return _rest_instance


def get_binance_rest() -> BinanceREST:
    """获取单例（惰性初始化）"""
    global _rest_instance
    if _rest_instance is None:
        with _rest_lock:
            if _rest_instance is None:
                _rest_instance = BinanceREST()
    return _rest_instance


async def close_binance_rest() -> None:
    """关闭全局客户端"""
    global _rest_instance
    with _rest_lock:
        if _rest_instance is not None:
            await _rest_instance.close()
            _rest_instance = None
