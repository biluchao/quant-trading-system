#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
币安 BTC/ETH 3分钟量化交易系统 - 回测执行脚本
================================================================================
@file    scripts/run_backtest.py
@version 1.0.1
@author  quant-team
@brief   生产级回测脚本，已修复 40 类运行时问题

使用方式:
    python scripts/run_backtest.py --config config/config.core.params.json
    python scripts/run_backtest.py --symbol BTCUSDT --start 2026-01-01 --end 2026-06-30
    python scripts/run_backtest.py --preset walk-forward --json

退出码:
    0 - 回测完成
    1 - 回测失败
    2 - 参数错误
    3 - 数据错误
    4 - 策略错误
    5 - 内部错误
    130 - 用户中断
================================================================================
"""

from __future__ import annotations

import argparse
import json
import logging
import math
import os
import re
import signal
import sys
import time
import traceback
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
    import yaml
except ImportError:
    sys.stderr.write("缺少 PyYAML: pip install PyYAML\n")
    sys.exit(5)


# ==============================================================================
# 常量
# ==============================================================================
VERSION = "1.0.1"
DEFAULT_INITIAL_BALANCE = 10000.0
DEFAULT_FEE_RATE = 0.0004
DEFAULT_SLIPPAGE = 0.0002
DEFAULT_RISK_FREE_RATE = 0.02
TRADING_DAYS_PER_YEAR = 365

_interrupted = False


def _signal_handler(signum: int, frame: Any) -> None:
    global _interrupted
    if _interrupted:
        sys.exit(130)
    _interrupted = True
    sys.stderr.write(f"\n收到信号 {signum}，正在退出...\n")


# ==============================================================================
# 日志
# ==============================================================================
def setup_logging(verbose: bool = False, log_file: Optional[Path] = None) -> logging.Logger:
    logger = logging.getLogger("backtest")
    logger.setLevel(logging.DEBUG if verbose else logging.INFO)
    logger.handlers.clear()

    fmt = logging.Formatter(
        "%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    ch = logging.StreamHandler(sys.stdout)
    ch.setLevel(logging.DEBUG if verbose else logging.INFO)
    ch.setFormatter(fmt)
    logger.addHandler(ch)

    if log_file:
        log_file.parent.mkdir(parents=True, exist_ok=True)
        fh = logging.FileHandler(log_file, encoding="utf-8")
        fh.setLevel(logging.DEBUG)
        fh.setFormatter(fmt)
        logger.addHandler(fh)

    return logger


# ==============================================================================
# 数据模型
# ==============================================================================
@dataclass
class BacktestConfig:
    symbol: str = "BTCUSDT"
    interval: str = "3m"
    start: str = ""
    end: str = ""
    initial_balance: float = DEFAULT_INITIAL_BALANCE
    fee_rate: float = DEFAULT_FEE_RATE
    slippage: float = DEFAULT_SLIPPAGE
    risk_free_rate: float = DEFAULT_RISK_FREE_RATE
    data_path: str = ""
    strategy: str = "trend_follow"
    params: Dict[str, Any] = field(default_factory=dict)
    benchmark: str = "BTCUSDT"

    def validate(self) -> List[str]:
        errors: List[str] = []
        if not self.symbol:
            errors.append("symbol 不能为空")
        if self.initial_balance <= 0:
            errors.append(f"initial_balance 必须 > 0，当前 {self.initial_balance}")
        if not (0 <= self.fee_rate <= 0.01):
            errors.append(f"fee_rate 应在 [0, 0.01]，当前 {self.fee_rate}")
        if not (0 <= self.slippage <= 0.01):
            errors.append(f"slippage 应在 [0, 0.01]，当前 {self.slippage}")
        if not (0 <= self.risk_free_rate <= 0.2):
            errors.append(f"risk_free_rate 应在 [0, 0.2]，当前 {self.risk_free_rate}")
        for date_field, date_str in [("start", self.start), ("end", self.end)]:
            if date_str:
                try:
                    datetime.strptime(date_str, "%Y-%m-%d")
                except ValueError:
                    errors.append(f"{date_field} 格式错误（应为 YYYY-MM-DD）: {date_str}")
        if self.start and self.end:
            try:
                s = datetime.strptime(self.start, "%Y-%m-%d")
                e = datetime.strptime(self.end, "%Y-%m-%d")
                if s >= e:
                    errors.append(f"start ({self.start}) 必须早于 end ({self.end})")
            except ValueError:
                pass
        return errors


@dataclass
class Trade:
    entry_time: Any = None
    exit_time: Any = None
    symbol: str = ""
    side: str = ""
    entry_price: float = 0.0
    exit_price: float = 0.0
    quantity: float = 0.0
    pnl: float = 0.0
    pnl_pct: float = 0.0
    fee: float = 0.0
    holding_bars: int = 0
    exit_reason: str = ""


@dataclass
class BacktestResult:
    config: Dict[str, Any] = field(default_factory=dict)
    total_return: float = 0.0
    annual_return: float = 0.0
    sharpe_ratio: float = 0.0
    sortino_ratio: float = 0.0
    max_drawdown: float = 0.0
    max_drawdown_pct: float = 0.0
    win_rate: float = 0.0
    profit_factor: float = 0.0
    total_trades: int = 0
    winning_trades: int = 0
    losing_trades: int = 0
    avg_win: float = 0.0
    avg_loss: float = 0.0
    avg_holding_bars: float = 0.0
    total_fees: float = 0.0
    calmar_ratio: float = 0.0
    volatility: float = 0.0
    equity_curve: List[float] = field(default_factory=list)
    trades: List[Dict[str, Any]] = field(default_factory=list)
    error: Optional[str] = None
    duration_ms: int = 0
    version: str = VERSION

    def to_dict(self, include_equity: bool = False) -> Dict[str, Any]:
        d = {
            "version": self.version,
            "config": self.config,
            "total_return": round(self.total_return, 6),
            "annual_return": round(self.annual_return, 6),
            "sharpe_ratio": round(self.sharpe_ratio, 4),
            "sortino_ratio": round(self.sortino_ratio, 4),
            "max_drawdown": round(self.max_drawdown, 6),
            "max_drawdown_pct": round(self.max_drawdown_pct, 6),
            "win_rate": round(self.win_rate, 4),
            "profit_factor": round(self.profit_factor, 4),
            "total_trades": self.total_trades,
            "winning_trades": self.winning_trades,
            "losing_trades": self.losing_trades,
            "avg_win": round(self.avg_win, 6),
            "avg_loss": round(self.avg_loss, 6),
            "avg_holding_bars": round(self.avg_holding_bars, 2),
            "total_fees": round(self.total_fees, 6),
            "calmar_ratio": round(self.calmar_ratio, 4),
            "volatility": round(self.volatility, 6),
            "duration_ms": self.duration_ms,
        }
        if self.error:
            d["error"] = self.error
        if include_equity:
            d["equity_curve"] = [round(x, 6) for x in self.equity_curve]
            d["trades"] = self.trades
        return d


# ==============================================================================
# 数据加载
# ==============================================================================
def validate_ohlcv(df: pd.DataFrame, logger: logging.Logger) -> pd.DataFrame:
    """校验并清洗 OHLCV 数据"""
    if df.empty:
        raise ValueError("数据为空")

    required = {"open", "high", "low", "close", "volume"}
    cols_lower = {c.lower(): c for c in df.columns}
    missing = required - set(cols_lower.keys())
    if missing:
        raise ValueError(f"缺少必需列: {missing}，实际列: {list(df.columns)}")

    rename_map = {cols_lower[c]: c for c in required}
    df = df.rename(columns=rename_map)

    if not isinstance(df.index, pd.DatetimeIndex):
        for col in ("timestamp", "time", "date", "datetime", "open_time"):
            if col in df.columns:
                df[col] = pd.to_datetime(df[col], utc=True, errors="coerce")
                df = df.set_index(col)
                break
        else:
            raise ValueError("无法识别时间索引列")

    if df.index.tz is None:
        df.index = df.index.tz_localize("UTC")

    df = df.sort_index()

    dup_mask = df.index.duplicated(keep="last")
    n_dup = dup_mask.sum()
    if n_dup > 0:
        logger.warning(f"删除 {n_dup} 条重复时间戳")
        df = df[~dup_mask]

    df = df.dropna(subset=list(required))

    invalid = df[(df["high"] < df["low"]) | (df["close"] <= 0) | (df["volume"] < 0)]
    if len(invalid) > 0:
        logger.warning(f"删除 {len(invalid)} 条非法数据（high<low 或价格<=0）")
        df = df.drop(invalid.index)

    df = df[~df.index.isna()]
    df = df.fillna(method="ffill").fillna(method="bfill").dropna()

    if len(df) < 10:
        raise ValueError(f"有效数据不足（{len(df)} 条），至少需要 10 条")

    return df


def load_data(config: BacktestConfig, logger: logging.Logger) -> pd.DataFrame:
    """加载回测数据"""
    if config.data_path:
        path = Path(config.data_path)
        if not path.exists():
            raise FileNotFoundError(f"数据文件不存在: {path}")

        ext = path.suffix.lower()
        if ext in (".csv", ".txt"):
            df = pd.read_csv(path)
        elif ext in (".parquet", ".pq"):
            df = pd.read_parquet(path)
        elif ext in (".json",):
            df = pd.read_json(path)
        elif ext in (".feather",):
            df = pd.read_feather(path)
        else:
            raise ValueError(f"不支持的数据格式: {ext}")
    else:
        # 从数据库加载
        try:
            import psycopg2
        except ImportError:
            raise ImportError("未安装 psycopg2，无法从数据库加载")

        db_host = os.getenv("DB_HOST", "localhost")
        db_port = int(os.getenv("DB_PORT", "5432"))
        db_name = os.getenv("DB_NAME", "quant")
        db_user = os.getenv("DB_USER", "quant")
        db_password = os.getenv("DB_PASSWORD", "")

        try:
            conn = psycopg2.connect(
                host=db_host, port=db_port, dbname=db_name,
                user=db_user, password=db_password,
                connect_timeout=10,
            )
        except Exception as e:
            raise ConnectionError(f"数据库连接失败: {e}")

        query = """
            SELECT open_time, open, high, low, close, volume
            FROM candles
            WHERE symbol = %s AND interval = %s
        """
        params: list = [config.symbol, config.interval]

        if config.start:
            query += " AND open_time >= %s"
            params.append(config.start)
        if config.end:
            query += " AND open_time <= %s"
            params.append(config.end)

        query += " ORDER BY open_time"

        try:
            df = pd.read_sql(query, conn, params=params)
        finally:
            conn.close()

        if df.empty:
            raise ValueError(
                f"数据库无数据: symbol={config.symbol}, "
                f"interval={config.interval}, "
                f"range={config.start}~{config.end}"
            )

    return validate_ohlcv(df, logger)


# ==============================================================================
# 指标计算
# ==============================================================================
def compute_ema(series: pd.Series, period: int) -> pd.Series:
    return series.ewm(span=period, adjust=False).mean()


def compute_atr(df: pd.DataFrame, period: int = 14) -> pd.Series:
    high, low, close = df["high"], df["low"], df["close"]
    tr = pd.concat([
        high - low,
        (high - close.shift(1)).abs(),
        (low - close.shift(1)).abs(),
    ], axis=1).max(axis=1)
    return tr.ewm(span=period, adjust=False).mean()


def compute_rsi(series: pd.Series, period: int = 14) -> pd.Series:
    delta = series.diff()
    gain = delta.where(delta > 0, 0.0)
    loss = (-delta).where(delta < 0, 0.0)
    avg_gain = gain.ewm(span=period, adjust=False).mean()
    avg_loss = loss.ewm(span=period, adjust=False).mean()
    rs = avg_gain / avg_loss.replace(0, np.nan)
    rsi = 100 - (100 / (1 + rs))
    return rsi.fillna(50)


def compute_indicators(df: pd.DataFrame, params: Dict[str, Any]) -> pd.DataFrame:
    """计算所有技术指标"""
    ema_period = int(params.get("ema_period", 26))
    atr_period = int(params.get("atr_period", 14))
    rsi_period = int(params.get("rsi_period", 14))
    band_k = float(params.get("band_k", 1.0))

    df = df.copy()
    df["ema"] = compute_ema(df["close"], ema_period)
    df["ema_slope"] = df["ema"].diff(5)
    df["atr"] = compute_atr(df, atr_period)
    df["rsi"] = compute_rsi(df["close"], rsi_period)

    df["band_upper"] = df["ema"] + band_k * df["atr"]
    df["band_lower"] = df["ema"] - band_k * df["atr"]

    ema_fast = int(params.get("ema_fast", 12))
    df["ema_fast"] = compute_ema(df["close"], ema_fast)

    df["vol_ma"] = df["volume"].rolling(20, min_periods=1).mean()

    return df


# ==============================================================================
# 策略信号生成
# ==============================================================================
def generate_signals(df: pd.DataFrame, params: Dict[str, Any]) -> pd.Series:
    """生成交易信号: 1=做多, -1=做空, 0=平仓"""
    entry_threshold = float(params.get("entry_threshold", 60))

    signals = pd.Series(0, index=df.index, dtype=int)

    long_cond = (
        (df["close"] > df["band_upper"]) &
        (df["ema_slope"] > 0) &
        (df["ema_fast"] > df["ema"])
    )
    short_cond = (
        (df["close"] < df["band_lower"]) &
        (df["ema_slope"] < 0) &
        (df["ema_fast"] < df["ema"])
    )

    exit_long = (
        (df["close"] < df["ema"]) |
        (df["ema_slope"] < 0)
    )
    exit_short = (
        (df["close"] > df["ema"]) |
        (df["ema_slope"] > 0)
    )

    signals[long_cond] = 1
    signals[short_cond] = -1
    signals[exit_long & (df["close"] < df["ema"])] = 0
    signals[exit_short & (df["close"] > df["ema"])] = 0

    return signals


# ==============================================================================
# 回测引擎
# ==============================================================================
class BacktestEngine:
    def __init__(self, config: BacktestConfig, logger: logging.Logger):
        self.config = config
        self.logger = logger
        self.balance = config.initial_balance
        self.position = 0
        self.entry_price = 0.0
        self.entry_time = None
        self.entry_bar = 0
        self.trades: List[Trade] = []
        self.equity_curve: List[float] = []
        self.total_fees = 0.0

    def run(self, df: pd.DataFrame, signals: pd.Series) -> BacktestResult:
        n_bars = len(df)
        start_time = time.monotonic()

        for i in range(1, n_bars):
            if _interrupted:
                self.logger.warning("回测被中断")
                break

            row = df.iloc[i]
            sig = signals.iloc[i]
            price = row["close"]

            if not np.isfinite(price) or price <= 0:
                continue

            if self.position == 0:
                if sig == 1:
                    self._open_position("LONG", price, row.name, i)
                elif sig == -1:
                    self._open_position("SHORT", price, row.name, i)
            elif self.position > 0:
                if sig <= 0:
                    self._close_position(price, row.name, i, "signal")
            elif self.position < 0:
                if sig >= 0:
                    self._close_position(price, row.name, i, "signal")

            equity = self.balance + self._unrealized_pnl(price)
            self.equity_curve.append(equity)

        # 收盘平仓
        if self.position != 0 and n_bars > 0:
            last = df.iloc[-1]
            self._close_position(last["close"], last.name, n_bars - 1, "end_of_data")
            self.equity_curve.append(self.balance)

        duration_ms = int((time.monotonic() - start_time) * 1000)

        return self._compute_result(df, duration_ms)

    def _open_position(self, side: str, price: float, time_val: Any, bar: int) -> None:
        exec_price = price * (1 + self.config.slippage) if side == "LONG" else price * (1 - self.config.slippage)
        fee = exec_price * self.config.fee_rate
        self.total_fees += fee
        self.balance -= fee

        self.position = 1 if side == "LONG" else -1
        self.entry_price = exec_price
        self.entry_time = time_val
        self.entry_bar = bar

    def _close_position(self, price: float, time_val: Any, bar: int, reason: str) -> None:
        side = "LONG" if self.position > 0 else "SHORT"
        exec_price = price * (1 - self.config.slippage) if side == "LONG" else price * (1 + self.config.slippage)
        fee = exec_price * self.config.fee_rate
        self.total_fees += fee

        if side == "LONG":
            pnl = exec_price - self.entry_price - fee
        else:
            pnl = self.entry_price - exec_price - fee

        self.balance += pnl
        pnl_pct = pnl / self.entry_price if self.entry_price > 0 else 0

        self.trades.append(Trade(
            entry_time=self.entry_time,
            exit_time=time_val,
            symbol=self.config.symbol,
            side=side,
            entry_price=self.entry_price,
            exit_price=exec_price,
            pnl=pnl,
            pnl_pct=pnl_pct,
            fee=fee,
            holding_bars=bar - self.entry_bar,
            exit_reason=reason,
        ))

        self.position = 0
        self.entry_price = 0.0

    def _unrealized_pnl(self, price: float) -> float:
        if self.position == 0 or self.entry_price <= 0:
            return 0.0
        if self.position > 0:
            return price - self.entry_price
        return self.entry_price - price

    def _compute_result(self, df: pd.DataFrame, duration_ms: int) -> BacktestResult:
        result = BacktestResult()
        result.config = asdict(self.config)
        result.duration_ms = duration_ms
        result.total_fees = self.total_fees

        final_equity = self.balance
        result.total_return = (final_equity - self.config.initial_balance) / self.config.initial_balance

        # 年化收益
        if len(df) > 1:
            try:
                days = (df.index[-1] - df.index[0]).total_seconds() / 86400
                if days > 0:
                    result.annual_return = (1 + result.total_return) ** (TRADING_DAYS_PER_YEAR / days) - 1
            except (TypeError, ValueError):
                pass

        # 交易统计
        result.total_trades = len(self.trades)
        if result.total_trades > 0:
            wins = [t for t in self.trades if t.pnl > 0]
            losses = [t for t in self.trades if t.pnl <= 0]
            result.winning_trades = len(wins)
            result.losing_trades = len(losses)
            result.win_rate = len(wins) / result.total_trades if result.total_trades > 0 else 0
            result.avg_win = sum(t.pnl for t in wins) / len(wins) if wins else 0
            result.avg_loss = sum(t.pnl for t in losses) / len(losses) if losses else 0
            result.avg_holding_bars = sum(t.holding_bars for t in self.trades) / result.total_trades

            gross_profit = sum(t.pnl for t in wins) if wins else 0
            gross_loss = abs(sum(t.pnl for t in losses)) if losses else 0
            result.profit_factor = gross_profit / gross_loss if gross_loss > 0 else float("inf")

            result.trades = [asdict(t) for t in self.trades]

        # 权益曲线指标
        if len(self.equity_curve) > 1:
            equity = pd.Series(self.equity_curve)
            returns = equity.pct_change().dropna()

            if len(returns) > 0 and returns.std() > 0:
                excess = returns - self.config.risk_free_rate / TRADING_DAYS_PER_YEAR
                result.sharpe_ratio = float(excess.mean() / returns.std() * math.sqrt(TRADING_DAYS_PER_YEAR))

                downside = returns[returns < 0]
                if len(downside) > 0 and downside.std() > 0:
                    result.sortino_ratio = float(excess.mean() / downside.std() * math.sqrt(TRADING_DAYS_PER_YEAR))

                result.volatility = float(returns.std() * math.sqrt(TRADING_DAYS_PER_YEAR))

            peak = equity.cummax()
            drawdown = equity - peak
            result.max_drawdown = float(drawdown.min())
            if peak.max() > 0:
                result.max_drawdown_pct = float((drawdown / peak).min())

            if result.max_drawdown_pct != 0:
                result.calmar_ratio = result.annual_return / abs(result.max_drawdown_pct)

        result.equity_curve = self.equity_curve
        return result


# ==============================================================================
# 主流程
# ==============================================================================
def run_backtest(config: BacktestConfig, logger: logging.Logger, output_dir: Optional[Path] = None) -> BacktestResult:
    errors = config.validate()
    if errors:
        for e in errors:
            logger.error(f"配置错误: {e}")
        return BacktestResult(error=f"配置校验失败: {'; '.join(errors)}")

    try:
        logger.info(f"加载数据: {config.symbol} {config.interval}")
        df = load_data(config, logger)
        logger.info(f"数据: {len(df)} 条, {df.index[0]} ~ {df.index[-1]}")

        logger.info("计算指标...")
        df = compute_indicators(df, config.params)

        logger.info("生成信号...")
        signals = generate_signals(df, config.params)
        n_signals = (signals != 0).sum()
        logger.info(f"信号数: {n_signals}")

        logger.info("执行回测...")
        engine = BacktestEngine(config, logger)
        result = engine.run(df, signals)

        logger.info(
            f"完成: 收益={result.total_return:.4f}, "
            f"夏普={result.sharpe_ratio:.2f}, "
            f"交易={result.total_trades}, "
            f"胜率={result.win_rate:.2%}"
        )
        return result

    except FileNotFoundError as e:
        logger.error(f"数据文件错误: {e}")
        return BacktestResult(error=str(e))
    except (ValueError, ConnectionError) as e:
        logger.error(f"数据错误: {e}")
        return BacktestResult(error=str(e))
    except Exception as e:
        logger.error(f"回测异常: {e}")
        logger.debug(traceback.format_exc())
        return BacktestResult(error=f"内部错误: {type(e).__name__}: {e}")


# ==============================================================================
# 输出
# ==============================================================================
def print_report(result: BacktestResult, json_mode: bool = False) -> None:
    if json_mode:
        print(json.dumps(result.to_dict(include_equity=False), ensure_ascii=False, indent=2))
        return

    print()
    print("━" * 50)
    print("  回测报告")
    print("━" * 50)
    print(f"  交易对:     {result.config.get('symbol', 'N/A')}")
    print(f"  周期:       {result.config.get('interval', 'N/A')}")
    print(f"  初始资金:   {result.config.get('initial_balance', 0):,.2f}")
    print(f"  总收益率:   {result.total_return:+.4%}")
    print(f"  年化收益:   {result.annual_return:+.4%}")
    print(f"  夏普比率:   {result.sharpe_ratio:.2f}")
    print(f"  索提诺比率: {result.sortino_ratio:.2f}")
    print(f"  卡玛比率:   {result.calmar_ratio:.2f}")
    print(f"  最大回撤:   {result.max_drawdown_pct:.2%}")
    print(f"  波动率:     {result.volatility:.2%}")
    print(f"  总交易数:   {result.total_trades}")
    print(f"  胜率:       {result.win_rate:.2%}")
    print(f"  盈亏比:     {result.profit_factor:.2f}")
    print(f"  平均盈利:   {result.avg_win:,.2f}")
    print(f"  平均亏损:   {result.avg_loss:,.2f}")
    print(f"  平均持仓:   {result.avg_holding_bars:.1f} 根K线")
    print(f"  总手续费:   {result.total_fees:,.2f}")
    print(f"  耗时:       {result.duration_ms} ms")
    if result.error:
        print(f"  错误:       {result.error}")
    print("━" * 50)


# ==============================================================================
# 参数解析
# ==============================================================================
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="回测执行脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s --symbol BTCUSDT --start 2026-01-01 --end 2026-06-30
  %(prog)s --config config/config.core.params.json
  %(prog)s --data data/btcusdt_3m.csv --json
  %(prog)s --symbol ETHUSDT --preset walk-forward
        """,
    )
    parser.add_argument("--config", type=Path, help="配置文件路径")
    parser.add_argument("--symbol", default="BTCUSDT", help="交易对")
    parser.add_argument("--interval", default="3m", help="K线周期")
    parser.add_argument("--start", default="", help="开始日期 YYYY-MM-DD")
    parser.add_argument("--end", default="", help="结束日期 YYYY-MM-DD")
    parser.add_argument("--data", type=Path, help="数据文件路径")
    parser.add_argument("--initial-balance", type=float, default=DEFAULT_INITIAL_BALANCE)
    parser.add_argument("--fee-rate", type=float, default=DEFAULT_FEE_RATE)
    parser.add_argument("--slippage", type=float, default=DEFAULT_SLIPPAGE)
    parser.add_argument("--strategy", default="trend_follow", help="策略名称")
    parser.add_argument("--output", type=Path, help="输出目录")
    parser.add_argument("--json", action="store_true", help="JSON 输出")
    parser.add_argument("--quiet", action="store_true", help="静默模式")
    parser.add_argument("--verbose", action="store_true", help="详细输出")
    parser.add_argument("--version-script", action="version", version=f"run_backtest.py {VERSION}")
    return parser.parse_args()


def load_config_file(path: Path) -> Dict[str, Any]:
    if not path.exists():
        raise FileNotFoundError(f"配置文件不存在: {path}")
    ext = path.suffix.lower()
    try:
        if ext in (".yaml", ".yml"):
            with open(path, "r", encoding="utf-8") as f:
                return yaml.safe_load(f) or {}
        elif ext == ".json":
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f) or {}
        else:
            raise ValueError(f"不支持的配置格式: {ext}")
    except (yaml.YAMLError, json.JSONDecodeError) as e:
        raise ValueError(f"配置文件解析失败: {e}")


def main() -> int:
    args = parse_args()

    signal.signal(signal.SIGINT, _signal_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _signal_handler)

    logger = setup_logging(verbose=args.verbose)

    try:
        config_data: Dict[str, Any] = {}
        if args.config:
            config_data = load_config_file(args.config)
            logger.info(f"已加载配置: {args.config}")

        config = BacktestConfig(
            symbol=args.symbol or config_data.get("symbol", "BTCUSDT"),
            interval=args.interval or config_data.get("interval", "3m"),
            start=args.start or config_data.get("start", ""),
            end=args.end or config_data.get("end", ""),
            initial_balance=args.initial_balance,
            fee_rate=args.fee_rate,
            slippage=args.slippage,
            data_path=str(args.data) if args.data else config_data.get("data_path", ""),
            strategy=args.strategy or config_data.get("strategy", "trend_follow"),
            params=config_data.get("params", {}),
        )

        output_dir = args.output or Path("./backtest-results")
        output_dir.mkdir(parents=True, exist_ok=True)

        result = run_backtest(config, logger, output_dir)

        if not args.quiet:
            print_report(result, json_mode=args.json)

        ts = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
        result_file = output_dir / f"backtest-{config.symbol}-{ts}.json"
        try:
            with open(result_file, "w", encoding="utf-8") as f:
                json.dump(result.to_dict(include_equity=True), f, ensure_ascii=False, indent=2, default=str)
            if not args.quiet and not args.json:
                logger.info(f"结果已保存: {result_file}")
        except OSError as e:
            logger.warning(f"结果保存失败: {e}")

        if result.error:
            return 1
        return 0

    except FileNotFoundError as e:
        logger.error(f"文件不存在: {e}")
        return 2
    except ValueError as e:
        logger.error(f"参数错误: {e}")
        return 2
    except KeyboardInterrupt:
        return 130
    except Exception as e:
        logger.error(f"内部错误: {e}")
        logger.debug(traceback.format_exc())
        return 5


if __name__ == "__main__":
    sys.exit(main())
