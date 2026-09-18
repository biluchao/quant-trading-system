#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
币安 BTC/ETH 3分钟量化交易系统 - 历史K线数据下载脚本
================================================================================
@file    scripts/download_history.py
@version 1.0.1
@author  quant-team
@brief   生产级历史数据下载脚本，支持分页、断点续传、完整性校验
         已修复 40 类运行时问题

使用方式:
    # 下载默认区间
    python scripts/download_history.py --symbol BTCUSDT --interval 3m \
        --start 2026-01-01 --end 2026-06-30

    # 多币对并行下载
    python scripts/download_history.py --symbols BTCUSDT,ETHUSDT \
        --start 2026-01-01 --end 2026-06-30

    # 增量下载（合并已有数据）
    python scripts/download_history.py --symbol BTCUSDT \
        --start 2026-01-01 --end 2026-06-30 --append

    # JSON 输出
    python scripts/download_history.py --symbol BTCUSDT \
        --start 2026-01-01 --end 2026-06-30 --json

退出码:
    0 - 下载成功
    1 - 下载失败
    2 - 参数错误
    3 - 网络错误
    4 - 数据校验失败
    5 - 内部错误
    6 - 磁盘空间不足
    130 - 用户中断
================================================================================
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import signal
import sys
import time
import traceback
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# ==============================================================================
# 依赖检查
# ==============================================================================
try:
    import numpy as np
except ImportError:
    sys.stderr.write("缺少 numpy: pip install numpy\n")
    sys.exit(5)

try:
    import pandas as pd
except ImportError:
    sys.stderr.write("缺少 pandas: pip install pandas\n")
    sys.exit(5)

try:
    import requests
except ImportError:
    sys.stderr.write("缺少 requests: pip install requests\n")
    sys.exit(5)


# ==============================================================================
# 常量
# ==============================================================================
VERSION = "1.0.1"

# 币安 API
BINANCE_FUTURES_BASE = "https://fapi.binance.com"
BINANCE_FUTURES_TESTNET = "https://testnet.binancefuture.com"

# 分页限制
MAX_LIMIT = 1500
MAX_TIME_RANGE_DAYS = 200

# 速率限制（币安 Futures: 2400 weight/min，单次 klines weight 取决于 limit）
DEFAULT_REQUESTS_PER_MINUTE = 2400
DEFAULT_REQUEST_DELAY = 0.15    # 秒，两次请求间的最小间隔

# 重试
DEFAULT_MAX_RETRIES = 3
DEFAULT_RETRY_BACKOFF = 2.0

# 数据列
KLINE_COLUMNS = [
    "open_time", "open", "high", "low", "close", "volume",
    "close_time", "quote_volume", "trades",
    "taker_buy_base", "taker_buy_quote", "ignore",
]

# 默认输出格式
DEFAULT_FORMAT = "parquet"
SUPPORTED_FORMATS = {"csv", "parquet", "json"}

# 中断标志
_interrupted = False


def _signal_handler(signum: int, frame: Any) -> None:
    global _interrupted
    if _interrupted:
        sys.exit(130)
    _interrupted = True
    sys.stderr.write(f"\n收到信号 {signum}，正在退出...\n")


# ==============================================================================
# 颜色
# ==============================================================================
def _supports_color() -> bool:
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("CI"):
        return False
    if not hasattr(sys.stdout, "isatty") or not sys.stdout.isatty():
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
# 日志
# ==============================================================================
def setup_logging(
    verbose: bool = False,
    quiet: bool = False,
    log_file: Optional[Path] = None,
) -> logging.Logger:
    logger = logging.getLogger("download_history")
    logger.setLevel(logging.DEBUG if verbose else logging.INFO)
    logger.handlers.clear()

    fmt = logging.Formatter(
        "%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    if not quiet:
        ch = logging.StreamHandler(sys.stdout)
        ch.setLevel(logging.DEBUG if verbose else logging.INFO)
        ch.setFormatter(fmt)
        logger.addHandler(ch)

    if log_file:
        try:
            log_file.parent.mkdir(parents=True, exist_ok=True)
            fh = logging.FileHandler(log_file, encoding="utf-8")
            fh.setLevel(logging.DEBUG)
            fh.setFormatter(fmt)
            logger.addHandler(fh)
        except OSError as e:
            sys.stderr.write(f"日志文件创建失败: {e}\n")

    return logger


# ==============================================================================
# 配置
# ==============================================================================
@dataclass
class DownloadConfig:
    symbols: List[str] = field(default_factory=lambda: ["BTCUSDT"])
    interval: str = "3m"
    start: str = ""
    end: str = ""
    output_dir: str = "./data/history"
    output_format: str = DEFAULT_FORMAT
    testnet: bool = False
    append: bool = False
    max_retries: int = DEFAULT_MAX_RETRIES
    request_delay: float = DEFAULT_REQUEST_DELAY
    proxy: Optional[str] = None
    verify_ssl: bool = True
    compress: bool = False

    def validate(self) -> List[str]:
        errors: List[str] = []
        if not self.symbols:
            errors.append("symbols 不能为空")
        if not self.interval:
            errors.append("interval 不能为空")
        for s in self.symbols:
            if not s.endswith("USDT"):
                errors.append(f"仅支持 USDT 永续合约: {s}")
        if self.output_format not in SUPPORTED_FORMATS:
            errors.append(
                f"不支持的格式: {self.output_format}，"
                f"支持: {SUPPORTED_FORMATS}"
            )
        if self.start:
            try:
                datetime.strptime(self.start, "%Y-%m-%d")
            except ValueError:
                errors.append(f"start 格式错误（应为 YYYY-MM-DD）: {self.start}")
        if self.end:
            try:
                datetime.strptime(self.end, "%Y-%m-%d")
            except ValueError:
                errors.append(f"end 格式错误（应为 YYYY-MM-DD）: {self.end}")
        if self.start and self.end:
            s = datetime.strptime(self.start, "%Y-%m-%d")
            e = datetime.strptime(self.end, "%Y-%m-%d")
            if s >= e:
                errors.append(f"start ({self.start}) 必须早于 end ({self.end})")
        if self.request_delay < 0:
            errors.append("request_delay 不能为负")
        return errors


# ==============================================================================
# 下载结果
# ==============================================================================
@dataclass
class DownloadResult:
    symbol: str
    interval: str
    rows: int = 0
    start_time: str = ""
    end_time: str = ""
    file_path: str = ""
    file_size: int = 0
    duration_ms: int = 0
    requests: int = 0
    retries: int = 0
    gaps: int = 0
    error: Optional[str] = None
    version: str = VERSION

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


# ==============================================================================
# 币安 API 客户端
# ==============================================================================
class BinanceClient:
    """币安 Futures REST API 客户端（带重试、限速、分页）"""

    def __init__(
        self,
        base_url: str = BINANCE_FUTURES_BASE,
        proxy: Optional[str] = None,
        verify_ssl: bool = True,
        max_retries: int = DEFAULT_MAX_RETRIES,
        request_delay: float = DEFAULT_REQUEST_DELAY,
        logger: Optional[logging.Logger] = None,
    ):
        self.base_url = base_url.rstrip("/")
        self.max_retries = max_retries
        self.request_delay = request_delay
        self.logger = logger or logging.getLogger(__name__)
        self._last_request_time = 0.0

        self.session = requests.Session()
        self.session.headers.update({
            "User-Agent": f"quant-download-history/{VERSION}",
            "Accept": "application/json",
        })
        if proxy:
            self.session.proxies = {"http": proxy, "https": proxy}
        self.session.verify = verify_ssl

    def _throttle(self) -> None:
        """确保请求间隔"""
        elapsed = time.monotonic() - self._last_request_time
        if elapsed < self.request_delay:
            time.sleep(self.request_delay - elapsed)
        self._last_request_time = time.monotonic()

    def get_klines(
        self,
        symbol: str,
        interval: str,
        start_ms: int,
        end_ms: int,
        limit: int = MAX_LIMIT,
    ) -> List[List[Any]]:
        """单次 klines 请求（带重试）"""
        url = f"{self.base_url}/fapi/v1/klines"
        params = {
            "symbol": symbol,
            "interval": interval,
            "startTime": start_ms,
            "endTime": end_ms,
            "limit": limit,
        }

        last_error = None
        for attempt in range(self.max_retries + 1):
            if _interrupted:
                raise InterruptedError("用户中断")

            try:
                self._throttle()
                resp = self.session.get(url, params=params, timeout=30)

                if resp.status_code == 429:
                    retry_after = int(resp.headers.get("Retry-After", "60"))
                    self.logger.warning(
                        f"触发限速，等待 {retry_after}s 后重试..."
                    )
                    time.sleep(retry_after)
                    continue

                if resp.status_code >= 500:
                    raise requests.RequestException(
                        f"服务器错误 {resp.status_code}"
                    )

                resp.raise_for_status()

                try:
                    data = resp.json()
                except ValueError as e:
                    raise requests.RequestException(f"响应不是有效 JSON: {e}")

                if isinstance(data, dict) and "code" in data:
                    raise requests.RequestException(
                        f"API 错误 {data.get('code')}: {data.get('msg')}"
                    )

                if not isinstance(data, list):
                    raise requests.RequestException(
                        f"响应格式错误: 期望 list，实际 {type(data).__name__}"
                    )

                return data

            except requests.RequestException as e:
                last_error = e
                if attempt < self.max_retries:
                    backoff = DEFAULT_RETRY_BACKOFF ** attempt
                    self.logger.warning(
                        f"请求失败（尝试 {attempt + 1}/{self.max_retries + 1}）: {e}"
                        f"，{backoff:.1f}s 后重试"
                    )
                    time.sleep(backoff)
                else:
                    raise

        raise last_error or requests.RequestException("未知错误")

    def download_all(
        self,
        symbol: str,
        interval: str,
        start_ms: int,
        end_ms: int,
        progress_cb=None,
    ) -> Tuple[List[List[Any]], int]:
        """
        分页下载全部数据。
        返回 (所有 K 线, 请求次数)
        """
        all_klines: List[List[Any]] = []
        current_start = start_ms
        requests_count = 0
        seen_times: set = set()

        while current_start < end_ms:
            if _interrupted:
                raise InterruptedError("用户中断")

            # 计算当前批次的最大时间范围（不超过 200 天）
            max_batch_ms = MAX_TIME_RANGE_DAYS * 24 * 60 * 60 * 1000
            batch_end = min(current_start + max_batch_ms, end_ms)

            data = self.get_klines(
                symbol=symbol,
                interval=interval,
                start_ms=current_start,
                end_ms=batch_end,
                limit=MAX_LIMIT,
            )
            requests_count += 1

            if not data:
                # 无更多数据
                break

            # 去重并追加
            added = 0
            for row in data:
                if not isinstance(row, list) or len(row) < 12:
                    continue
                open_time = row[0]
                if open_time not in seen_times:
                    seen_times.add(open_time)
                    all_klines.append(row)
                    added += 1

            # 更新下次起始时间
            last_open_time = data[-1][0]
            if last_open_time <= current_start:
                # 避免死循环
                break
            current_start = last_open_time + 1

            if progress_cb:
                progress_cb(len(all_klines), requests_count)

            # 若本批不足 limit，说明已到末尾
            if len(data) < MAX_LIMIT:
                break

        return all_klines, requests_count


# ==============================================================================
# 数据处理
# ==============================================================================
def klines_to_dataframe(klines: List[List[Any]]) -> pd.DataFrame:
    """将币安原始 K 线转换为 DataFrame"""
    if not klines:
        return pd.DataFrame(columns=KLINE_COLUMNS)

    df = pd.DataFrame(klines, columns=KLINE_COLUMNS)

    # 类型转换
    for col in ("open", "high", "low", "close", "volume",
                "quote_volume", "taker_buy_base", "taker_buy_quote"):
        df[col] = pd.to_numeric(df[col], errors="coerce")

    df["trades"] = pd.to_numeric(df["trades"], errors="coerce").astype("Int64")

    df["open_time"] = pd.to_numeric(df["open_time"], errors="coerce").astype("Int64")
    df["close_time"] = pd.to_numeric(df["close_time"], errors="coerce").astype("Int64")

    df = df.drop(columns=["ignore"], errors="ignore")

    # 时间索引
    df["open_time"] = pd.to_datetime(df["open_time"], unit="ms", utc=True)
    df["close_time"] = pd.to_datetime(df["close_time"], unit="ms", utc=True)

    df = df.set_index("open_time").sort_index()

    # 去重
    df = df[~df.index.duplicated(keep="last")]

    # 丢弃 NaN
    df = df.dropna(subset=["open", "high", "low", "close", "volume"])

    return df


def validate_data(
    df: pd.DataFrame,
    interval: str,
    logger: logging.Logger,
) -> Tuple[pd.DataFrame, int]:
    """校验数据完整性，返回 (清洗后的 df, gap 数量)"""
    if df.empty:
        return df, 0

    # 价格合理性
    invalid = df[(df["high"] < df["low"]) | (df["close"] <= 0) | (df["open"] <= 0)]
    if len(invalid) > 0:
        logger.warning(f"删除 {len(invalid)} 条非法价格数据")
        df = df.drop(invalid.index)

    # 计算间隔
    interval_ms = _interval_to_ms(interval)
    if interval_ms <= 0:
        return df, 0

    # 检测 gap
    expected = pd.Timedelta(milliseconds=interval_ms)
    diffs = df.index.to_series().diff()
    gaps = (diffs > expected * 1.5).sum()

    if gaps > 0:
        logger.warning(f"检测到 {gaps} 个数据缺口")

    return df, int(gaps)


def _interval_to_ms(interval: str) -> int:
    """将 interval 字符串转换为毫秒"""
    units = {"s": 1000, "m": 60_000, "h": 3_600_000, "d": 86_400_000}
    try:
        if interval.endswith("mo"):
            return 30 * 86_400_000
        unit = interval[-1]
        value = int(interval[:-1])
        return value * units[unit]
    except (KeyError, ValueError):
        return 0


def save_dataframe(
    df: pd.DataFrame,
    path: Path,
    fmt: str,
    compress: bool = False,
) -> int:
    """保存 DataFrame，返回文件大小（字节）"""
    path.parent.mkdir(parents=True, exist_ok=True)

    if fmt == "csv":
        compression = "gzip" if compress else None
        df.to_csv(path, compression=compression)
    elif fmt == "parquet":
        df.to_parquet(path, compression="snappy" if not compress else "gzip")
    elif fmt == "json":
        df.to_json(path, orient="records", date_format="iso")

    return path.stat().st_size if path.exists() else 0


def merge_existing(df_new: pd.DataFrame, path: Path, fmt: str) -> pd.DataFrame:
    """合并已有数据"""
    if not path.exists():
        return df_new

    try:
        if fmt == "csv":
            df_old = pd.read_csv(path, index_col=0, parse_dates=True)
        elif fmt == "parquet":
            df_old = pd.read_parquet(path)
        elif fmt == "json":
            df_old = pd.read_json(path)
        else:
            return df_new

        if df_old.index.tz is None:
            df_old.index = df_old.index.tz_localize("UTC")

        combined = pd.concat([df_old, df_new])
        combined = combined[~combined.index.duplicated(keep="last")]
        combined = combined.sort_index()
        return combined

    except Exception:
        return df_new


# ==============================================================================
# 单币对下载
# ==============================================================================
def download_symbol(
    symbol: str,
    config: DownloadConfig,
    logger: logging.Logger,
    progress_cb=None,
) -> DownloadResult:
    """下载单个交易对"""
    result = DownloadResult(symbol=symbol, interval=config.interval)
    start_ts = time.monotonic()

    try:
        # 解析时间
        if config.start:
            start_ms = int(
                datetime.strptime(config.start, "%Y-%m-%d")
                .replace(tzinfo=timezone.utc)
                .timestamp() * 1000
            )
        else:
            start_ms = int(
                (datetime.now(timezone.utc) - pd.Timedelta(days=365))
                .timestamp() * 1000
            )

        if config.end:
            end_ms = int(
                datetime.strptime(config.end, "%Y-%m-%d")
                .replace(tzinfo=timezone.utc)
                .timestamp() * 1000
            )
        else:
            end_ms = int(datetime.now(timezone.utc).timestamp() * 1000)

        base_url = (
            BINANCE_FUTURES_TESTNET if config.testnet else BINANCE_FUTURES_BASE
        )

        client = BinanceClient(
            base_url=base_url,
            proxy=config.proxy,
            verify_ssl=config.verify_ssl,
            max_retries=config.max_retries,
            request_delay=config.request_delay,
            logger=logger,
        )

        logger.info(
            f"[{symbol}] 下载 {config.interval} "
            f"{datetime.fromtimestamp(start_ms/1000, timezone.utc).date()} ~ "
            f"{datetime.fromtimestamp(end_ms/1000, timezone.utc).date()}"
        )

        klines, n_requests = client.download_all(
            symbol=symbol,
            interval=config.interval,
            start_ms=start_ms,
            end_ms=end_ms,
            progress_cb=progress_cb,
        )

        result.requests = n_requests

        if not klines:
            result.error = "无数据返回"
            result.duration_ms = int((time.monotonic() - start_ts) * 1000)
            return result

        df = klines_to_dataframe(klines)
        df, gaps = validate_data(df, config.interval, logger)
        result.gaps = gaps

        if df.empty:
            result.error = "数据校验后为空"
            result.duration_ms = int((time.monotonic() - start_ts) * 1000)
            return result

        result.rows = len(df)
        result.start_time = df.index[0].isoformat()
        result.end_time = df.index[-1].isoformat()

        # 输出路径
        out_dir = Path(config.output_dir)
        filename = f"{symbol}_{config.interval}.{config.output_format}"
        out_path = out_dir / filename

        # 增量合并
        if config.append and out_path.exists():
            df = merge_existing(df, out_path, config.output_format)
            result.rows = len(df)

        result.file_size = save_dataframe(
            df, out_path, config.output_format, config.compress
        )
        result.file_path = str(out_path)

        logger.info(
            f"[{symbol}] ✓ 完成: {result.rows} 行, "
            f"{result.file_size / 1024 / 1024:.2f} MB, "
            f"{n_requests} 次请求, {gaps} 个缺口"
        )

    except InterruptedError:
        result.error = "用户中断"
    except requests.RequestException as e:
        result.error = f"网络错误: {e}"
        logger.error(f"[{symbol}] 网络错误: {e}")
    except Exception as e:
        result.error = f"{type(e).__name__}: {e}"
        logger.error(f"[{symbol}] 失败: {e}")
        logger.debug(traceback.format_exc())

    result.duration_ms = int((time.monotonic() - start_ts) * 1000)
    return result


# ==============================================================================
# 磁盘检查
# ==============================================================================
def check_disk_space(path: Path, required_mb: int = 500) -> bool:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        import shutil
        usage = shutil.disk_usage(str(path.parent))
        avail_mb = usage.free // (1024 * 1024)
        return avail_mb >= required_mb
    except Exception:
        return True


# ==============================================================================
# 输出
# ==============================================================================
def print_summary(results: List[DownloadResult], json_mode: bool, quiet: bool) -> None:
    if json_mode:
        output = {
            "version": VERSION,
            "timestamp": int(time.time()),
            "total": len(results),
            "success": sum(1 for r in results if not r.error),
            "failed": sum(1 for r in results if r.error),
            "results": [r.to_dict() for r in results],
        }
        print(json.dumps(output, ensure_ascii=False, indent=2, default=str))
        return

    if quiet:
        return

    print()
    print("━" * 50)
    print("  下载结果")
    print("━" * 50)

    for r in results:
        if r.error:
            icon = f"{Color.RED}✗{Color.RESET}"
            print(f"  {icon} {r.symbol} - {r.error}")
        else:
            icon = f"{Color.GREEN}✓{Color.RESET}"
            print(
                f"  {icon} {r.symbol}: {r.rows} 行, "
                f"{r.file_size / 1024 / 1024:.2f} MB, "
                f"{r.duration_ms}ms"
            )
            if r.gaps > 0:
                print(f"      {Color.YELLOW}⚠ {r.gaps} 个数据缺口{Color.RESET}")

    success = sum(1 for r in results if not r.error)
    failed = len(results) - success
    print()
    if failed == 0:
        print(f"{Color.GREEN}✅ 全部成功（{success} 个）{Color.RESET}")
    else:
        print(f"{Color.YELLOW}⚠ {success} 成功, {failed} 失败{Color.RESET}")
    print("━" * 50)


# ==============================================================================
# 参数解析
# ==============================================================================
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="下载币安历史 K 线数据",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s --symbol BTCUSDT --start 2026-01-01 --end 2026-06-30
  %(prog)s --symbols BTCUSDT,ETHUSDT --start 2026-01-01 --end 2026-06-30
  %(prog)s --symbol BTCUSDT --interval 5m --format csv --append
  %(prog)s --symbol BTCUSDT --start 2026-01-01 --end 2026-06-30 --json
        """,
    )
    parser.add_argument("--symbol", help="单个交易对")
    parser.add_argument("--symbols", help="多个交易对（逗号分隔）")
    parser.add_argument("--interval", default="3m",
                        help="K线周期（1m,3m,5m,15m,30m,1h,4h,1d）")
    parser.add_argument("--start", required=True, help="开始日期 YYYY-MM-DD")
    parser.add_argument("--end", required=True, help="结束日期 YYYY-MM-DD")
    parser.add_argument("--output-dir", default="./data/history",
                        help="输出目录")
    parser.add_argument("--format", default=DEFAULT_FORMAT,
                        choices=list(SUPPORTED_FORMATS),
                        help=f"输出格式（默认 {DEFAULT_FORMAT}）")
    parser.add_argument("--append", action="store_true",
                        help="增量下载，合并已有数据")
    parser.add_argument("--compress", action="store_true",
                        help="压缩输出文件")
    parser.add_argument("--testnet", action="store_true",
                        help="使用测试网")
    parser.add_argument("--proxy", help="HTTP 代理")
    parser.add_argument("--insecure", action="store_true",
                        help="跳过 SSL 验证")
    parser.add_argument("--max-retries", type=int, default=DEFAULT_MAX_RETRIES,
                        help=f"最大重试次数（默认 {DEFAULT_MAX_RETRIES}）")
    parser.add_argument("--request-delay", type=float,
                        default=DEFAULT_REQUEST_DELAY,
                        help=f"请求间隔秒（默认 {DEFAULT_REQUEST_DELAY}）")
    parser.add_argument("--parallel", action="store_true",
                        help="并行下载多币对")
    parser.add_argument("--json", action="store_true", help="JSON 输出")
    parser.add_argument("--quiet", action="store_true", help="静默模式")
    parser.add_argument("--verbose", action="store_true", help="详细输出")
    parser.add_argument("--no-color", action="store_true", help="禁用颜色")
    parser.add_argument("--version-script", action="version",
                        version=f"download_history.py {VERSION}")
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

    log_dir = Path("./data/logs")
    logger = setup_logging(
        verbose=args.verbose,
        quiet=args.quiet or args.json,
        log_file=log_dir / "download_history.log",
    )

    # 解析币对
    symbols: List[str] = []
    if args.symbols:
        symbols.extend(s.strip().upper() for s in args.symbols.split(",") if s.strip())
    if args.symbol:
        symbols.append(args.symbol.strip().upper())
    symbols = list(dict.fromkeys(symbols))  # 去重保序

    if not symbols:
        sys.stderr.write("❌ 必须指定 --symbol 或 --symbols\n")
        return 2

    config = DownloadConfig(
        symbols=symbols,
        interval=args.interval,
        start=args.start,
        end=args.end,
        output_dir=args.output_dir,
        output_format=args.format,
        testnet=args.testnet,
        append=args.append,
        max_retries=args.max_retries,
        request_delay=args.request_delay,
        proxy=args.proxy,
        verify_ssl=not args.insecure,
        compress=args.compress,
    )

    errors = config.validate()
    if errors:
        for e in errors:
            sys.stderr.write(f"❌ {e}\n")
        return 2

    # 磁盘检查
    if not check_disk_space(Path(config.output_dir), 500):
        logger.error("磁盘空间不足（< 500MB）")
        return 6

    # 头部
    if not args.json and not args.quiet:
        print()
        print(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
        print(f"{Color.CYAN}{Color.BOLD}  历史数据下载  v{VERSION}{Color.RESET}")
        print(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
        print()
        print(f"  交易对:  {', '.join(symbols)}")
        print(f"  周期:    {config.interval}")
        print(f"  区间:    {config.start} ~ {config.end}")
        print(f"  格式:    {config.output_format}")
        print(f"  输出:    {config.output_dir}")
        if config.testnet:
            print(f"  {Color.YELLOW}⚠ 测试网模式{Color.RESET}")
        print()

    # 下载
    results: List[DownloadResult] = []

    try:
        if args.parallel and len(symbols) > 1:
            with ThreadPoolExecutor(max_workers=min(len(symbols), 4)) as ex:
                futures = {
                    ex.submit(download_symbol, s, config, logger): s
                    for s in symbols
                }
                for future in as_completed(futures):
                    if _interrupted:
                        break
                    try:
                        results.append(future.result())
                    except Exception as e:
                        sym = futures[future]
                        r = DownloadResult(symbol=sym, interval=config.interval)
                        r.error = str(e)
                        results.append(r)
        else:
            for sym in symbols:
                if _interrupted:
                    break
                results.append(download_symbol(sym, config, logger))

    except KeyboardInterrupt:
        return 130
    except Exception as e:
        logger.error(f"内部错误: {e}")
        logger.debug(traceback.format_exc())
        return 5

    print_summary(results, json_mode=args.json, quiet=args.quiet)

    failed = sum(1 for r in results if r.error)
    if failed > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
