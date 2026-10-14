"""---UMS-BEGIN---
spec_version: 1.0
module: core
type: core
name: http_server
version: 2.0.0
level: 0
required: false
author: quant-team
created: 2026-10-14
brief: 健康检查HTTP服务（鉴权/优雅关闭/无副作用/降级）

provides:
  - class HTTPServer
  - function get_http_server
  - function init_http_server
  - function start_http_server
  - function stop_http_server

requires:
  modules: [core.logger, core.timestamp]
  libraries:
    - fastapi>=0.115  (可选)
    - uvicorn>=0.34  (可选)
  resources:
    memory_mb: 32
    cpu_cores: 1

health:
  fn: HTTPServer.health
  timeout_ms: 500

assembly:
  order: 18
  parallel: false

tests:
  - tests/test_http_server.py

fallback:
  - FastAPI 缺失 → 内置 asyncio HTTP
  - 端口占用 → 换端口或跳过启动
  - 鉴权失败 → 401

tags: [core, http, health]
---UMS-END---"""

from __future__ import annotations

import os
import sys
import json
import socket
import asyncio
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Tuple

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("http_server")
        except Exception:
            import logging
            _logger = logging.getLogger("http_server")
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
    from fastapi import FastAPI, Request, Response, HTTPException
    from fastapi.responses import JSONResponse, PlainTextResponse
    from fastapi.middleware.cors import CORSMiddleware
    _HAS_FASTAPI = True
except ImportError:
    _HAS_FASTAPI = False

try:
    import uvicorn  # type: ignore
    _HAS_UVICORN = True
except ImportError:
    _HAS_UVICORN = False


# ============================================================
# 异常
# ============================================================
class HTTPServerError(Exception):
    """HTTP 服务基类异常"""


class PortInUseError(HTTPServerError):
    """端口被占用"""


# ============================================================
# 配置
# ============================================================
@dataclass
class HTTPConfig:
    host: str = "127.0.0.1"          # 默认仅本地访问
    port: int = 9000
    enable_auth: bool = False
    auth_token: str = ""              # 简单 bearer token
    enable_debug: bool = False         # /debug 端点
    enable_metrics: bool = True        # /metrics 端点
    cors_origins: List[str] = field(default_factory=list)
    request_timeout_s: float = 5.0
    max_body_bytes: int = 64 * 1024
    version: str = "1.0.0"


# ============================================================
# 健康检查回调
# ============================================================
class HealthRegistry:
    """注册各模块的健康检查回调"""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._checks: Dict[str, Callable[[], Dict[str, Any]]] = {}
        self._ready_checks: Dict[str, Callable[[], bool]] = {}

    def register_health(
        self, name: str,
        fn: Callable[[], Dict[str, Any]],
    ) -> None:
        with self._lock:
            self._checks[name] = fn

    def register_ready(self, name: str,
                        fn: Callable[[], bool]) -> None:
        with self._lock:
            self._ready_checks[name] = fn

    def unregister(self, name: str) -> None:
        with self._lock:
            self._checks.pop(name, None)
            self._ready_checks.pop(name, None)

    def run_health(self) -> Dict[str, Any]:
        with self._lock:
            items = list(self._checks.items())
        result: Dict[str, Any] = {}
        for name, fn in items:
            try:
                result[name] = fn()
            except Exception as exc:
                result[name] = {
                    "status": "error",
                    "error": str(exc),
                }
        return result

    def run_ready(self) -> Tuple[bool, Dict[str, Any]]:
        with self._lock:
            items = list(self._ready_checks.items())
        result: Dict[str, Any] = {}
        all_ready = True
        for name, fn in items:
            try:
                ok = bool(fn())
            except Exception as exc:
                ok = False
                result[name] = {
                    "ready": False, "error": str(exc)}
            else:
                result[name] = {"ready": ok}
            if not ok:
                all_ready = False
        return all_ready, result


# ============================================================
# HTTPServer 单例
# ============================================================
class HTTPServer:
    """
    健康检查 HTTP 服务。

    - 无导入副作用
    - 支持鉴权
    - 优雅关闭
    - 三级降级：FastAPI/uvicorn → asyncio 内置 → 不启动
    """

    _instance: Optional["HTTPServer"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "HTTPServer":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(self, config: Optional[HTTPConfig] = None) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self.config = config or HTTPConfig()
            self.health_registry = HealthRegistry()
            self._app = None
            self._server = None
            self._server_task: Optional[asyncio.Task] = None
            self._thread: Optional[threading.Thread] = None
            self._loop: Optional[asyncio.AbstractEventLoop] = None
            self._running = False
            self._started_ms = 0
            self._stats = {
                "requests": 0,
                "errors": 0,
                "auth_failed": 0,
                "uptime_start": 0,
            }
            self._initialized = True

            # 内置 asyncio HTTP 降级处理函数表
            self._raw_routes: Dict[str, Callable] = {}

    # ========================================================
    # 启动前检查
    # ========================================================
    def _check_port_available(
        self, host: str, port: int
    ) -> bool:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((host, port))
            return True
        except OSError:
            return False
        finally:
            s.close()

    # ========================================================
    # FastAPI 应用
    # ========================================================
    def _build_fastapi_app(self) -> Any:
        app = FastAPI(
            title="Quant Health Server",
            version=self.config.version,
            docs_url=None,
            redoc_url=None,
            openapi_url=None,
        )

        # CORS
        if self.config.cors_origins:
            app.add_middleware(
                CORSMiddleware,
                allow_origins=self.config.cors_origins,
                allow_credentials=True,
                allow_methods=["GET"],
                allow_headers=["*"],
            )

        # request_id 与鉴权中间件
        @app.middleware("http")
        async def _middleware(request: Request, call_next):
            request_id = request.headers.get(
                "x-request-id", uuid.uuid4().hex[:12])
            self._stats["requests"] += 1
            try:
                # 鉴权
                if self.config.enable_auth:
                    if not self._check_auth(request):
                        self._stats["auth_failed"] += 1
                        return JSONResponse(
                            status_code=401,
                            content={"error": "unauthorized"},
                            headers={"x-request-id": request_id},
                        )
                response = await asyncio.wait_for(
                    call_next(request),
                    timeout=self.config.request_timeout_s,
                )
            except asyncio.TimeoutError:
                self._stats["errors"] += 1
                return JSONResponse(
                    status_code=504,
                    content={"error": "timeout"},
                    headers={"x-request-id": request_id},
                )
            except Exception as exc:
                self._stats["errors"] += 1
                _log().error(f"请求异常: {exc}")
                return JSONResponse(
                    status_code=500,
                    content={"error": "internal_error"},
                    headers={"x-request-id": request_id},
                )
            response.headers["x-request-id"] = request_id
            return response

        # ---- 端点 ----

        @app.get("/health")
        async def health():
            uptime_s = (
                (_now_ms() - self._started_ms) / 1000
                if self._started_ms else 0.0
            )
            modules = self.health_registry.run_health()
            overall = "ok"
            for m in modules.values():
                if isinstance(m, dict) and \
                        m.get("status") in ("error", "down"):
                    overall = "degraded"
                    break
            return {
                "status": overall,
                "version": self.config.version,
                "uptime_s": round(uptime_s, 2),
                "ts_ms": _now_ms(),
                "modules": modules,
            }

        @app.get("/ready")
        async def ready():
            all_ready, checks = self.health_registry.run_ready()
            code = 200 if all_ready else 503
            return JSONResponse(
                status_code=code,
                content={
                    "ready": all_ready,
                    "checks": checks,
                    "ts_ms": _now_ms(),
                },
            )

        @app.get("/version")
        async def version():
            return {
                "version": self.config.version,
                "pid": os.getpid(),
                "python": sys.version.split()[0],
            }

        if self.config.enable_metrics:
            @app.get("/metrics", response_class=PlainTextResponse)
            async def metrics():
                return self._render_prometheus_metrics()

        if self.config.enable_debug:
            @app.get("/debug")
            async def debug():
                return {
                    "stats": dict(self._stats),
                    "config": {
                        "host": self.config.host,
                        "port": self.config.port,
                        "auth": self.config.enable_auth,
                        "debug": self.config.enable_debug,
                    },
                    "threads": threading.active_count(),
                    "ts_ms": _now_ms(),
                }

        @app.get("/")
        async def root():
            return {
                "service": "quant-http-server",
                "version": self.config.version,
                "endpoints": [
                    "/health", "/ready", "/version",
                ] + (["/metrics"] if self.config.enable_metrics else [])
                  + (["/debug"] if self.config.enable_debug else []),
            }

        return app

    # ========================================================
    # 鉴权
    # ========================================================
    def _check_auth(self, request: Any) -> bool:
        token = self.config.auth_token
        if not token:
            return False
        header = request.headers.get("authorization", "")
        if header.startswith("Bearer "):
            provided = header[7:].strip()
        else:
            provided = request.headers.get("x-api-token", "")
        import hmac
        return hmac.compare_digest(provided, token)

    # ========================================================
    # Prometheus 指标
    # ========================================================
    def _render_prometheus_metrics(self) -> str:
        lines: List[str] = []
        lines.append(
            "# HELP quant_http_requests_total "
            "Total HTTP requests")
        lines.append("# TYPE quant_http_requests_total counter")
        lines.append(
            f"quant_http_requests_total "
            f"{self._stats['requests']}")
        lines.append(
            "# HELP quant_http_errors_total "
            "Total HTTP errors")
        lines.append("# TYPE quant_http_errors_total counter")
        lines.append(
            f"quant_http_errors_total {self._stats['errors']}")
        lines.append(
            "# HELP quant_http_auth_failed_total "
            "Total auth failures")
        lines.append(
            "# TYPE quant_http_auth_failed_total counter")
        lines.append(
            f"quant_http_auth_failed_total "
            f"{self._stats['auth_failed']}")
        if self._started_ms:
            uptime = (_now_ms() - self._started_ms) / 1000
            lines.append(
                "# HELP quant_http_uptime_seconds "
                "Uptime in seconds")
            lines.append(
                "# TYPE quant_http_uptime_seconds gauge")
            lines.append(
                f"quant_http_uptime_seconds {uptime:.2f}")
        return "\n".join(lines) + "\n"

    # ========================================================
    # 启动
    # ========================================================
    def start(self) -> bool:
        with self._lock:
            if self._running:
                return True

            # 端口占用检查
            if not self._check_port_available(
                    self.config.host, self.config.port):
                _log().error(
                    f"端口 {self.config.port} 已被占用")
                # 尝试找可用端口
                for p in range(self.config.port + 1,
                               self.config.port + 10):
                    if self._check_port_available(
                            self.config.host, p):
                        _log().warning(
                            f"使用备用端口 {p}")
                        self.config.port = p
                        break
                else:
                    _log().error("找不到可用端口，跳过 HTTP 启动")
                    return False

            if _HAS_FASTAPI and _HAS_UVICORN:
                return self._start_fastapi()
            return self._start_builtin()

    def _start_fastapi(self) -> bool:
        try:
            self._app = self._build_fastapi_app()
            cfg = uvicorn.Config(
                app=self._app,
                host=self.config.host,
                port=self.config.port,
                log_level="warning",
                access_log=False,
                lifespan="off",
                timeout_keep_alive=5,
            )
            self._server = uvicorn.Server(cfg)
            self._started_ms = _now_ms()
            self._stats["uptime_start"] = self._started_ms

            def _run() -> None:
                try:
                    self._server.run()
                except Exception as exc:
                    _log().error(f"HTTP 服务异常: {exc}")

            self._thread = threading.Thread(
                target=_run, name="http-server", daemon=True)
            self._thread.start()
            self._running = True
            _log().info(
                f"HTTP 服务已启动 "
                f"http://{self.config.host}:{self.config.port}")
            return True
        except Exception as exc:
            _log().error(f"FastAPI 启动失败: {exc}，尝试降级")
            return self._start_builtin()

    def _start_builtin(self) -> bool:
        """内置 asyncio HTTP 降级实现（仅支持基本 GET）"""
        try:
            self._loop = asyncio.new_event_loop()

            async def _serve() -> None:
                server = await asyncio.start_server(
                    self._handle_raw,
                    host=self.config.host,
                    port=self.config.port,
                )
                self._started_ms = _now_ms()
                self._stats["uptime_start"] = self._started_ms
                async with server:
                    await server.serve_forever()

            def _run() -> None:
                asyncio.set_event_loop(self._loop)
                try:
                    self._loop.run_until_complete(_serve())
                except Exception as exc:
                    _log().error(f"内置 HTTP 异常: {exc}")

            self._thread = threading.Thread(
                target=_run, name="http-server-raw", daemon=True)
            self._thread.start()
            self._running = True
            _log().info(
                f"内置 HTTP 服务已启动 "
                f"http://{self.config.host}:{self.config.port}")
            return True
        except Exception as exc:
            _log().error(f"内置 HTTP 启动失败: {exc}")
            return False

    # ========================================================
    # 内置 HTTP 请求处理（降级）
    # ========================================================
    async def _handle_raw(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        try:
            data = await asyncio.wait_for(
                reader.read(4096), timeout=3.0)
            self._stats["requests"] += 1

            # 解析请求行
            first_line = data.split(b"\r\n", 1)[0].decode(
                "utf-8", "ignore")
            parts = first_line.split(" ")
            if len(parts) < 2:
                await self._write_response(
                    writer, 400, {"error": "bad_request"})
                return
            method, path = parts[0], parts[1]
            path = path.split("?")[0]

            if method != "GET":
                await self._write_response(
                    writer, 405, {"error": "method_not_allowed"})
                return

            # 路由
            if path == "/health":
                body = {
                    "status": "ok",
                    "version": self.config.version,
                    "ts_ms": _now_ms(),
                    "modules": self.health_registry.run_health(),
                }
                await self._write_response(writer, 200, body)
            elif path == "/ready":
                ok, checks = self.health_registry.run_ready()
                await self._write_response(
                    writer, 200 if ok else 503,
                    {"ready": ok, "checks": checks})
            elif path == "/version":
                await self._write_response(writer, 200, {
                    "version": self.config.version,
                    "pid": os.getpid(),
                })
            elif path == "/metrics" and self.config.enable_metrics:
                text = self._render_prometheus_metrics()
                await self._write_response(
                    writer, 200, text, content_type="text/plain")
            else:
                await self._write_response(
                    writer, 404, {"error": "not_found"})
        except Exception as exc:
            self._stats["errors"] += 1
            _log().debug(f"原始 HTTP 处理异常: {exc}")
            try:
                await self._write_response(
                    writer, 500, {"error": "internal_error"})
            except Exception:
                pass
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

    async def _write_response(
        self,
        writer: asyncio.StreamWriter,
        code: int,
        body: Any,
        content_type: str = "application/json",
    ) -> None:
        if content_type == "application/json":
            payload = json.dumps(body).encode("utf-8")
        else:
            payload = str(body).encode("utf-8")
        headers = (
            f"HTTP/1.1 {code} OK\r\n"
            f"Content-Type: {content_type}; charset=utf-8\r\n"
            f"Content-Length: {len(payload)}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode("ascii")
        writer.write(headers + payload)
        await writer.drain()

    # ========================================================
    # 关闭
    # ========================================================
    def stop(self, timeout_s: float = 5.0) -> None:
        with self._lock:
            if not self._running:
                return
            self._running = False
            server = self._server
            loop = self._loop
            thread = self._thread

        # uvicorn 停止
        if server is not None:
            try:
                server.should_exit = True
            except Exception:
                pass

        # 内置 loop 停止
        if loop is not None:
            try:
                loop.call_soon_threadsafe(loop.stop)
            except Exception:
                pass

        if thread is not None:
            thread.join(timeout=timeout_s)

        _log().info(
            f"HTTP 服务已停止 "
            f"(requests={self._stats['requests']}, "
            f"errors={self._stats['errors']})")

        self._server = None
        self._loop = None
        self._thread = None

    # ========================================================
    # 状态
    # ========================================================
    def is_running(self) -> bool:
        with self._lock:
            return self._running

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            return {
                **self._stats,
                "running": self._running,
                "host": self.config.host,
                "port": self.config.port,
                "started_ms": self._started_ms,
            }

    def health(self) -> Dict[str, Any]:
        with self._lock:
            running = self._running
        uptime_s = (
            (_now_ms() - self._started_ms) / 1000
            if self._started_ms else 0.0
        )
        return {
            "status": "ok" if running else "down",
            "running": running,
            "host": self.config.host,
            "port": self.config.port,
            "uptime_s": round(uptime_s, 2),
            "fastapi": _HAS_FASTAPI,
            "uvicorn": _HAS_UVICORN,
            "auth_enabled": self.config.enable_auth,
            "stats": dict(self._stats),
        }

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"<HTTPServer running={self._running} "
                f"addr={self.config.host}:{self.config.port}>"
            )


# ============================================================
# 全局入口
# ============================================================
_server: Optional[HTTPServer] = None
_server_lock = threading.Lock()


def init_http_server(
    config: Optional[HTTPConfig] = None,
    autostart: bool = False,
) -> HTTPServer:
    """显式初始化"""
    global _server
    with _server_lock:
        _server = HTTPServer(config)
        if autostart:
            _server.start()
    return _server


def get_http_server() -> HTTPServer:
    """获取单例（惰性初始化）"""
    global _server
    if _server is None:
        with _server_lock:
            if _server is None:
                _server = HTTPServer()
    return _server


def start_http_server(
    config: Optional[HTTPConfig] = None,
) -> bool:
    global _server
    with _server_lock:
        if _server is None:
            _server = HTTPServer(config)
    return _server.start()


def stop_http_server(timeout_s: float = 5.0) -> None:
    global _server
    with _server_lock:
        if _server is not None:
            _server.stop(timeout_s=timeout_s)
            _server = None
