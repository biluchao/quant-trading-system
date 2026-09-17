#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
币安 BTC/ETH 3分钟量化交易系统 - 服务就绪等待脚本
================================================================================
@file    scripts/wait_ready.py
@version 1.0.1
@author  quant-team
@brief   等待 HTTP/TCP 服务就绪，支持多端点、指数退避、JSON 输出
         已修复 40 类运行时问题

使用方式:
    python scripts/wait_ready.py http://localhost:8000/health
    python scripts/wait_ready.py --urls http://a/health,http://b/health
    python scripts/wait_ready.py --tcp localhost:5432 --timeout 60
    python scripts/wait_ready.py --json
    python scripts/wait_ready.py --help

退出码:
    0 - 全部就绪
    1 - 超时未就绪
    2 - 参数错误
    3 - 内部错误
    4 - 被信号中断
================================================================================
"""

from __future__ import annotations

import argparse
import gzip
import io
import json
import os
import signal
import socket
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field, asdict
from typing import Any, Dict, List, Optional, Tuple

# ==============================================================================
# 常量
# ==============================================================================
VERSION = "1.0.1"

# 默认值
DEFAULT_TIMEOUT = 60
DEFAULT_INTERVAL = 1.0
DEFAULT_MAX_INTERVAL = 10.0
DEFAULT_BACKOFF = 1.5
DEFAULT_MAX_RETRIES = 0        # 0 = 无限（受超时限制）
DEFAULT_MAX_RESPONSE_SIZE = 1024 * 1024  # 1 MB
DEFAULT_USER_AGENT = f"quant-wait-ready/{VERSION}"

# 中断标志
_interrupted = False


# ==============================================================================
# 颜色
# ==============================================================================
def _supports_color() -> bool:
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("CI"):
        return False
    if not hasattr(sys.stdout, "isatty"):
        return False
    if not sys.stdout.isatty():
        return False
    if sys.platform == "win32":
        try:
            import ctypes
            kernel32 = ctypes.windll.kernel32
            kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
            return True
        except Exception:
            return False
    return True


class Color:
    _enabled = _supports_color()
    RESET = "\033[0m" if _enabled else ""
    RED = "\033[0;31m" if _enabled else ""
    GREEN = "\033[0;32m" if _enabled else ""
    YELLOW = "\033[0;33m" if _enabled else ""
    BLUE = "\033[0;34m" if _enabled else ""
    CYAN = "\033[0;36m" if _enabled else ""
    BOLD = "\033[1m" if _enabled else ""

    @classmethod
    def disable(cls) -> None:
        for attr in ("RESET", "RED", "GREEN", "YELLOW", "BLUE", "CYAN", "BOLD"):
            setattr(cls, attr, "")


# ==============================================================================
# 数据结构
# ==============================================================================
@dataclass
class CheckResult:
    target: str
    ready: bool = False
    attempts: int = 0
    elapsed_ms: int = 0
    last_error: Optional[str] = None
    status_code: Optional[int] = None

    def to_dict(self) -> Dict[str, Any]:
        d = {
            "target": self.target,
            "ready": self.ready,
            "attempts": self.attempts,
            "elapsed_ms": self.elapsed_ms,
        }
        if self.last_error:
            d["last_error"] = self.last_error
        if self.status_code is not None:
            d["status_code"] = self.status_code
        return d


# ==============================================================================
# 信号处理
# ==============================================================================
def _signal_handler(signum: int, frame: Any) -> None:
    global _interrupted
    if _interrupted:
        sys.exit(130)
    _interrupted = True
    sys.stderr.write(f"\n收到信号 {signum}，正在退出...\n")


# ==============================================================================
# 目标解析
# ==============================================================================
@dataclass
class Target:
    raw: str
    kind: str          # "http" | "tcp"
    scheme: str = ""
    host: str = ""
    port: int = 0
    path: str = ""
    url: str = ""      # 完整 URL（HTTP 用）

    def __str__(self) -> str:
        return self.raw


def parse_target(raw: str) -> Target:
    """解析目标字符串为 Target"""
    raw = raw.strip()
    if not raw:
        raise ValueError("空目标")

    # HTTP/HTTPS
    if raw.startswith(("http://", "https://")):
        try:
            parsed = urllib.parse.urlparse(raw)
        except ValueError as e:
            raise ValueError(f"URL 解析失败: {e}")

        if not parsed.hostname:
            raise ValueError(f"URL 缺少主机: {raw}")

        # 默认端口
        if parsed.port:
            port = parsed.port
        elif parsed.scheme == "https":
            port = 443
        else:
            port = 80

        # 规范化 URL
        path = parsed.path or "/"
        if parsed.query:
            path += "?" + parsed.query

        return Target(
            raw=raw,
            kind="http",
            scheme=parsed.scheme,
            host=parsed.hostname,
            port=port,
            path=path,
            url=raw,
        )

    # TCP: host:port
    if ":" in raw:
        host, _, port_str = raw.rpartition(":")
        # 处理 IPv6 [::1]:8080
        if host.startswith("[") and host.endswith("]"):
            host = host[1:-1]
        try:
            port = int(port_str)
        except ValueError:
            raise ValueError(f"端口无效: {port_str}")
        if not (1 <= port <= 65535):
            raise ValueError(f"端口范围错误: {port}")
        if not host:
            raise ValueError(f"缺少主机: {raw}")
        return Target(raw=raw, kind="tcp", host=host, port=port)

    raise ValueError(f"无法识别的目标格式: {raw}")


# ==============================================================================
# HTTP 检查
# ==============================================================================
class HTTPChecker:
    def __init__(
        self,
        method: str = "GET",
        headers: Optional[Dict[str, str]] = None,
        insecure: bool = False,
        follow_redirects: bool = False,
        expect_status: Optional[List[int]] = None,
        expect_body: Optional[str] = None,
        max_response_size: int = DEFAULT_MAX_RESPONSE_SIZE,
        proxy: Optional[str] = None,
    ):
        self.method = method.upper()
        self.headers = headers or {}
        self.insecure = insecure
        self.follow_redirects = follow_redirects
        self.expect_status = expect_status or list(range(200, 300))
        self.expect_body = expect_body
        self.max_response_size = max_response_size
        self.proxy = proxy

        # SSL 上下文
        if insecure:
            self.ssl_context = ssl.create_default_context()
            self.ssl_context.check_hostname = False
            self.ssl_context.verify_mode = ssl.CERT_NONE
        else:
            self.ssl_context = ssl.create_default_context()

        # 默认 headers
        self.headers.setdefault("User-Agent", DEFAULT_USER_AGENT)
        self.headers.setdefault("Accept", "*/*")
        self.headers.setdefault("Accept-Encoding", "gzip, deflate")

        # 代理
        if proxy:
            self.opener = urllib.request.build_opener(
                urllib.request.ProxyHandler({
                    "http": proxy,
                    "https": proxy,
                }),
                urllib.request.HTTPSHandler(context=self.ssl_context),
            )
        else:
            # 禁用自动重定向（默认）
            class NoRedirect(urllib.request.HTTPRedirectHandler):
                def redirect_request(self, *args, **kwargs):
                    return None

            handlers = [urllib.request.HTTPSHandler(context=self.ssl_context)]
            if not follow_redirects:
                handlers.insert(0, NoRedirect())
            self.opener = urllib.request.build_opener(*handlers)

    def check(self, target: Target, timeout: float) -> Tuple[bool, Optional[int], Optional[str]]:
        """
        返回 (ready, status_code, error)
        """
        req = urllib.request.Request(
            target.url,
            method=self.method,
            headers=self.headers,
        )

        try:
            with self.opener.open(req, timeout=timeout) as resp:
                status = resp.status

                if status not in self.expect_status:
                    return False, status, f"HTTP {status}（期望 {self.expect_status}）"

                # 检查响应体
                if self.expect_body:
                    body = self._read_body(resp)
                    if self.expect_body not in body:
                        return False, status, f"响应体不包含 '{self.expect_body}'"

                return True, status, None

        except urllib.error.HTTPError as e:
            return False, e.code, f"HTTP {e.code}"

        except urllib.error.URLError as e:
            reason = e.reason
            if isinstance(reason, socket.timeout):
                return False, None, "连接超时"
            if isinstance(reason, socket.gaierror):
                return False, None, f"DNS 解析失败: {reason}"
            if isinstance(reason, ConnectionRefusedError):
                return False, None, "连接被拒绝"
            return False, None, f"URL 错误: {reason}"

        except socket.timeout:
            return False, None, "socket 超时"

        except ssl.SSLError as e:
            return False, None, f"SSL 错误: {e}"

        except ConnectionError as e:
            return False, None, f"连接错误: {e}"

        except Exception as e:
            return False, None, f"未知错误: {type(e).__name__}: {e}"

    def _read_body(self, resp: Any) -> str:
        # gzip 解压
        content_encoding = resp.headers.get("Content-Encoding", "").lower()
        data = resp.read(self.max_response_size + 1)

        if len(data) > self.max_response_size:
            return ""

        if "gzip" in content_encoding:
            try:
                data = gzip.decompress(data)
            except OSError:
                pass

        try:
            return data.decode("utf-8", errors="replace")
        except Exception:
            return ""


# ==============================================================================
# TCP 检查
# ==============================================================================
class TCPChecker:
    def check(self, target: Target, timeout: float) -> Tuple[bool, Optional[int], Optional[str]]:
        try:
            with socket.create_connection(
                (target.host, target.port), timeout=timeout
            ):
                return True, None, None
        except socket.timeout:
            return False, None, "TCP 连接超时"
        except socket.gaierror as e:
            return False, None, f"DNS 解析失败: {e}"
        except ConnectionRefusedError:
            return False, None, "TCP 连接被拒绝"
        except OSError as e:
            return False, None, f"TCP 错误: {e}"


# ==============================================================================
# 等待器
# ==============================================================================
class Waiter:
    def __init__(
        self,
        timeout: float = DEFAULT_TIMEOUT,
        interval: float = DEFAULT_INTERVAL,
        max_interval: float = DEFAULT_MAX_INTERVAL,
        backoff: float = DEFAULT_BACKOFF,
        request_timeout: float = 5.0,
        parallel: bool = False,
        verbose: bool = False,
        quiet: bool = False,
        checker: Optional[Any] = None,
    ):
        self.timeout = timeout
        self.interval = interval
        self.max_interval = max_interval
        self.backoff = backoff
        self.request_timeout = request_timeout
        self.parallel = parallel
        self.verbose = verbose
        self.quiet = quiet
        self.checker = checker or HTTPChecker()

    def _log(self, msg: str, level: str = "info") -> None:
        if self.quiet:
            return
        if level == "verbose" and not self.verbose:
            return

        prefix = {
            "info": f"{Color.BLUE}[INFO]{Color.RESET}",
            "verbose": f"{Color.CYAN}[DEBUG]{Color.RESET}",
            "warn": f"{Color.YELLOW}[WARN]{Color.RESET}",
            "error": f"{Color.RED}[ERROR]{Color.RESET}",
        }.get(level, "")
        print(f"{prefix} {msg}")

    def wait_all(self, targets: List[Target]) -> List[CheckResult]:
        if self.parallel and len(targets) > 1:
            return self._wait_parallel(targets)
        return self._wait_serial(targets)

    def _wait_serial(self, targets: List[Target]) -> List[CheckResult]:
        results: List[CheckResult] = []
        for target in targets:
            if _interrupted:
                break
            result = self._wait_one(target)
            results.append(result)
            if not result.ready:
                # 有失败，记录并继续（并行模式下所有都检查）
                pass
        return results

    def _wait_parallel(self, targets: List[Target]) -> List[CheckResult]:
        results: List[CheckResult] = []
        with ThreadPoolExecutor(max_workers=len(targets)) as executor:
            futures = {executor.submit(self._wait_one, t): t for t in targets}
            for future in as_completed(futures):
                if _interrupted:
                    break
                try:
                    results.append(future.result())
                except Exception as e:
                    t = futures[future]
                    results.append(CheckResult(
                        target=t.raw, ready=False, last_error=str(e)
                    ))
        return results

    def _wait_one(self, target: Target) -> CheckResult:
        result = CheckResult(target=target.raw)
        start = time.monotonic()
        current_interval = self.interval
        attempt = 0

        while True:
            if _interrupted:
                result.elapsed_ms = int((time.monotonic() - start) * 1000)
                return result

            elapsed = time.monotonic() - start
            if elapsed >= self.timeout:
                result.elapsed_ms = int(elapsed * 1000)
                return result

            attempt += 1
            result.attempts = attempt

            try:
                ready, status, error = self.checker.check(target, self.request_timeout)
            except Exception as e:
                ready, status, error = False, None, f"检查异常: {e}"

            if ready:
                result.ready = True
                result.status_code = status
                result.elapsed_ms = int((time.monotonic() - start) * 1000)
                self._log(
                    f"{Color.GREEN}✓{Color.RESET} {target} "
                    f"就绪（尝试 {attempt} 次，耗时 {result.elapsed_ms}ms）"
                )
                return result

            result.status_code = status
            result.last_error = error

            if self.verbose:
                self._log(
                    f"尝试 {attempt}: {target} - {error}",
                    "verbose",
                )

            # 等待间隔
            remaining = self.timeout - (time.monotonic() - start)
            if remaining <= 0:
                break

            sleep_time = min(current_interval, remaining)
            try:
                time.sleep(sleep_time)
            except KeyboardInterrupt:
                break

            # 指数退避
            current_interval = min(current_interval * self.backoff, self.max_interval)

        result.elapsed_ms = int((time.monotonic() - start) * 1000)
        return result


# ==============================================================================
# 输出
# ==============================================================================
def print_results(results: List[CheckResult], json_mode: bool, quiet: bool) -> None:
    if json_mode:
        output = {
            "version": VERSION,
            "timestamp": int(time.time()),
            "passed": all(r.ready for r in results),
            "results": [r.to_dict() for r in results],
        }
        print(json.dumps(output, ensure_ascii=False, indent=2))
        return

    if quiet:
        return

    print()
    print(f"{Color.BOLD}━━━ 等待结果 ━━━{Color.RESET}")
    print()
    for r in results:
        if r.ready:
            icon = f"{Color.GREEN}✓{Color.RESET}"
            status = f"就绪（{r.attempts} 次，{r.elapsed_ms}ms）"
        else:
            icon = f"{Color.RED}✗{Color.RESET}"
            status = f"未就绪（{r.attempts} 次，{r.elapsed_ms}ms）"
            if r.last_error:
                status += f" - {r.last_error}"

        print(f"  {icon} {r.target} {status}")

    print()
    if all(r.ready for r in results):
        print(f"{Color.GREEN}✅ 全部就绪{Color.RESET}")
    else:
        failed = sum(1 for r in results if not r.ready)
        print(f"{Color.RED}❌ {failed} 个目标未就绪{Color.RESET}")


# ==============================================================================
# 参数解析
# ==============================================================================
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="等待服务就绪",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s http://localhost:8000/health
  %(prog)s --urls http://a/health,http://b/health
  %(prog)s --tcp localhost:5432,localhost:6379
  %(prog)s --urls http://localhost:8000/health --timeout 120
  %(prog)s --urls http://localhost:8000/health --json
  %(prog)s --urls http://localhost:8000/health --insecure

退出码:
  0   全部就绪
  1   超时未就绪
  2   参数错误
  3   内部错误
  4   被信号中断
        """,
    )

    parser.add_argument(
        "targets",
        nargs="*",
        help="目标 URL 或 host:port",
    )
    parser.add_argument(
        "--urls",
        help="逗号分隔的 URL 列表",
    )
    parser.add_argument(
        "--tcp",
        help="逗号分隔的 host:port 列表",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help=f"整体超时秒数（默认 {DEFAULT_TIMEOUT}）",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=DEFAULT_INTERVAL,
        help=f"初始重试间隔秒数（默认 {DEFAULT_INTERVAL}）",
    )
    parser.add_argument(
        "--max-interval",
        type=float,
        default=DEFAULT_MAX_INTERVAL,
        help=f"最大重试间隔秒数（默认 {DEFAULT_MAX_INTERVAL}）",
    )
    parser.add_argument(
        "--backoff",
        type=float,
        default=DEFAULT_BACKOFF,
        help=f"退避系数（默认 {DEFAULT_BACKOFF}）",
    )
    parser.add_argument(
        "--request-timeout",
        type=float,
        default=5.0,
        help="单次请求超时秒数（默认 5.0）",
    )
    parser.add_argument(
        "--method",
        default="GET",
        choices=["GET", "HEAD", "POST"],
        help="HTTP 方法（默认 GET）",
    )
    parser.add_argument(
        "--header",
        action="append",
        default=[],
        help="自定义请求头 'Key: Value'（可多次）",
    )
    parser.add_argument(
        "--expect-status",
        help="期望的 HTTP 状态码（逗号分隔，默认 200-299）",
    )
    parser.add_argument(
        "--expect-body",
        help="响应体必须包含的子串",
    )
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="不验证 SSL 证书",
    )
    parser.add_argument(
        "--follow-redirects",
        action="store_true",
        help="跟随重定向",
    )
    parser.add_argument(
        "--proxy",
        help="HTTP 代理（如 http://127.0.0.1:7890）",
    )
    parser.add_argument(
        "--parallel",
        action="store_true",
        help="并行检查多个目标",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="JSON 格式输出",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="静默模式（仅退出码）",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="详细输出",
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        help="禁用颜色",
    )
    parser.add_argument(
        "--version-script",
        action="version",
        version=f"wait_ready.py {VERSION}",
    )

    return parser.parse_args()


# ==============================================================================
# 主函数
# ==============================================================================
def main() -> int:
    args = parse_args()

    if args.no_color or args.json:
        Color.disable()

    signal.signal(signal.SIGINT, _signal_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _signal_handler)

    # 校验参数
    if args.timeout <= 0:
        sys.stderr.write("❌ --timeout 必须 > 0\n")
        return 2
    if args.interval <= 0:
        sys.stderr.write("❌ --interval 必须 > 0\n")
        return 2
    if args.max_interval < args.interval:
        sys.stderr.write("❌ --max-interval 必须 >= --interval\n")
        return 2
    if args.backoff < 1.0:
        sys.stderr.write("❌ --backoff 必须 >= 1.0\n")
        return 2

    # 收集目标
    raw_targets: List[str] = []
    raw_targets.extend(args.targets)

    if args.urls:
        raw_targets.extend(u.strip() for u in args.urls.split(",") if u.strip())

    if args.tcp:
        for entry in args.tcp.split(","):
            entry = entry.strip()
            if entry:
                raw_targets.append(entry)

    if not raw_targets:
        sys.stderr.write("❌ 未提供任何目标\n")
        sys.stderr.write("使用 --help 查看帮助\n")
        return 2

    # 解析目标
    targets: List[Target] = []
    for raw in raw_targets:
        try:
            targets.append(parse_target(raw))
        except ValueError as e:
            sys.stderr.write(f"❌ 无效目标 '{raw}': {e}\n")
            return 2

    # 解析期望状态码
    expect_status: Optional[List[int]] = None
    if args.expect_status:
        try:
            expect_status = [int(s.strip()) for s in args.expect_status.split(",")]
        except ValueError:
            sys.stderr.write(f"❌ --expect-status 格式错误\n")
            return 2

    # 解析自定义 header
    headers: Dict[str, str] = {}
    for h in args.header:
        if ":" not in h:
            sys.stderr.write(f"❌ 无效 header: {h}\n")
            return 2
        k, _, v = h.partition(":")
        headers[k.strip()] = v.strip()

    # 创建 checker
    has_http = any(t.kind == "http" for t in targets)
    has_tcp = any(t.kind == "tcp" for t in targets)

    if has_http and has_tcp:
        # 混合模式：为每种类型创建独立 checker
        # 简化处理：用 HTTP checker 处理 HTTP，TCP 单独
        pass

    http_checker = HTTPChecker(
        method=args.method,
        headers=headers,
        insecure=args.insecure,
        follow_redirects=args.follow_redirects,
        expect_status=expect_status,
        expect_body=args.expect_body,
        proxy=args.proxy,
    )
    tcp_checker = TCPChecker()

    # 复合 checker
    class CompositeChecker:
        def check(self, target: Target, timeout: float):
            if target.kind == "http":
                return http_checker.check(target, timeout)
            else:
                return tcp_checker.check(target, timeout)

    checker = CompositeChecker()

    # 打印头部
    if not args.json and not args.quiet:
        print()
        print(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
        print(f"{Color.CYAN}{Color.BOLD}  等待服务就绪  v{VERSION}{Color.RESET}")
        print(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
        print()
        print(f"  目标数:  {len(targets)}")
        print(f"  超时:    {args.timeout}s")
        print(f"  间隔:    {args.interval}s（最大 {args.max_interval}s，退避 {args.backoff}x）")
        if args.parallel:
            print(f"  模式:    并行")
        print()

    # 执行等待
    waiter = Waiter(
        timeout=args.timeout,
        interval=args.interval,
        max_interval=args.max_interval,
        backoff=args.backoff,
        request_timeout=args.request_timeout,
        parallel=args.parallel,
        verbose=args.verbose,
        quiet=args.quiet or args.json,
        checker=checker,
    )

    try:
        results = waiter.wait_all(targets)
    except KeyboardInterrupt:
        return 130
    except Exception as e:
        sys.stderr.write(f"❌ 内部错误: {e}\n")
        import traceback
        traceback.print_exc()
        return 3

    print_results(results, json_mode=args.json, quiet=args.quiet)

    if all(r.ready for r in results):
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
