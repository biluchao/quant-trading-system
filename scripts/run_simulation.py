#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
币安 BTC/ETH 3分钟量化交易系统 - 模拟盘运行脚本
================================================================================
@file    scripts/run_simulation.py
@version 1.0.1
@author  quant-team
@brief   使用虚拟券商运行模拟盘，真实行情 + 虚拟结算
         已修复 40 类运行时问题

使用方式:
    python scripts/run_simulation.py --env dev
    python scripts/run_simulation.py --env dev --balance 100000 --duration 3600
    python scripts/run_simulation.py --env dev --config config/config.core.params.json
    python scripts/run_simulation.py --env dev --once --json

退出码:
    0 - 正常退出
    1 - 运行失败
    2 - 参数错误
    3 - 环境检查失败
    4 - 依赖未就绪
    5 - 内部错误
    6 - 风控触发停止
    130 - 用户中断
================================================================================
"""

from __future__ import annotations

import argparse
import atexit
import json
import logging
import os
import signal
import sys
import time
import traceback
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

# ==============================================================================
# 依赖检查
# ==============================================================================
try:
    import yaml
except ImportError:
    sys.stderr.write("缺少 PyYAML: pip install PyYAML\n")
    sys.exit(5)


# ==============================================================================
# 常量
# ==============================================================================
VERSION = "1.0.1"

DEFAULT_BALANCE = 100000.0
DEFAULT_DURATION = 0          # 0 = 无限
DEFAULT_TICK_INTERVAL = 5     # 秒
DEFAULT_MAX_DRAWDOWN = 0.20
DEFAULT_MAX_DAILY_LOSS = 0.05
DEFAULT_MAX_TRADES_PER_HOUR = 60
DEFAULT_HEALTHCHECK_INTERVAL = 30

_interrupted = False
_stop_reason = ""


# ==============================================================================
# 信号处理
# ==============================================================================
def _signal_handler(signum: int, frame: Any) -> None:
    global _interrupted, _stop_reason
    if _interrupted:
        sys.exit(130)
    _interrupted = True
    _stop_reason = f"signal {signum}"
    sys.stderr.write(f"\n收到信号 {signum}，正在优雅退出...\n")


# ==============================================================================
# 日志
# ==============================================================================
def setup_logging(
    verbose: bool = False,
    log_file: Optional[Path] = None,
    quiet: bool = False,
) -> logging.Logger:
    logger = logging.getLogger("simulation")
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
class SimulationConfig:
    env: str = "dev"
    symbols: List[str] = field(default_factory=lambda: ["BTCUSDT", "ETHUSDT"])
    interval: str = "3m"
    initial_balance: float = DEFAULT_BALANCE
    duration_sec: int = DEFAULT_DURATION
    tick_interval: float = DEFAULT_TICK_INTERVAL
    max_drawdown: float = DEFAULT_MAX_DRAWDOWN
    max_daily_loss: float = DEFAULT_MAX_DAILY_LOSS
    max_trades_per_hour: int = DEFAULT_MAX_TRADES_PER_HOUR
    fee_rate: float = 0.0004
    slippage: float = 0.0002
    strategy: str = "trend_follow"
    params: Dict[str, Any] = field(default_factory=dict)
    once: bool = False
    json_output: bool = False

    def validate(self) -> List[str]:
        errors: List[str] = []
        if self.env not in ("dev", "test", "staging", "prod"):
            errors.append(f"无效环境: {self.env}")
        if self.initial_balance <= 0:
            errors.append(f"initial_balance 必须 > 0")
        if self.duration_sec < 0:
            errors.append(f"duration_sec 必须 >= 0")
        if self.tick_interval <= 0:
            errors.append(f"tick_interval 必须 > 0")
        if not (0 < self.max_drawdown < 1):
            errors.append(f"max_drawdown 应在 (0,1)")
        if not (0 < self.max_daily_loss < 1):
            errors.append(f"max_daily_loss 应在 (0,1)")
        if not self.symbols:
            errors.append("symbols 不能为空")
        return errors


# ==============================================================================
# 运行状态
# ==============================================================================
@dataclass
class SimulationState:
    run_id: str = ""
    start_time: str = ""
    end_time: str = ""
    env: str = ""
    symbols: List[str] = field(default_factory=list)
    initial_balance: float = 0.0
    current_balance: float = 0.0
    equity: float = 0.0
    peak_equity: float = 0.0
    max_drawdown: float = 0.0
    daily_pnl: float = 0.0
    total_trades: int = 0
    winning_trades: int = 0
    losing_trades: int = 0
    total_fees: float = 0.0
    tick_count: int = 0
    stop_reason: str = ""
    duration_sec: float = 0.0
    error: Optional[str] = None
    version: str = VERSION

    def to_dict(self) -> Dict[str, Any]:
        d = asdict(self)
        d["win_rate"] = (
            self.winning_trades / self.total_trades
            if self.total_trades > 0 else 0.0
        )
        return d


# ==============================================================================
# 环境检查
# ==============================================================================
def check_dependencies(logger: logging.Logger) -> bool:
    """检查运行依赖"""
    ok = True

    # PostgreSQL
    try:
        import psycopg2
        db_host = os.getenv("DB_HOST", "localhost")
        db_port = int(os.getenv("DB_PORT", "5432"))
        db_name = os.getenv("DB_NAME", "quant_dev")
        db_user = os.getenv("DB_USER", "quant")
        db_password = os.getenv("DB_PASSWORD", "")

        try:
            conn = psycopg2.connect(
                host=db_host, port=db_port, dbname=db_name,
                user=db_user, password=db_password,
                connect_timeout=5,
            )
            conn.close()
            logger.info(f"✓ PostgreSQL: {db_host}:{db_port}/{db_name}")
        except Exception as e:
            logger.warning(f"⚠ PostgreSQL 不可用: {e}")
            # 不阻塞，模拟盘可离线运行
    except ImportError:
        logger.warning("⚠ psycopg2 未安装")

    # Redis
    try:
        import redis
        redis_host = os.getenv("REDIS_HOST", "localhost")
        redis_port = int(os.getenv("REDIS_PORT", "6379"))
        redis_password = os.getenv("REDIS_PASSWORD", "")

        try:
            r = redis.Redis(
                host=redis_host, port=redis_port,
                password=redis_password or None,
                socket_timeout=5,
            )
            r.ping()
            logger.info(f"✓ Redis: {redis_host}:{redis_port}")
        except Exception as e:
            logger.warning(f"⚠ Redis 不可用: {e}")
    except ImportError:
        logger.warning("⚠ redis 未安装")

    return ok


# ==============================================================================
# 模拟盘引擎（骨架）
# ==============================================================================
class SimulationEngine:
    """模拟盘引擎：真实行情 + 虚拟券商"""

    def __init__(
        self,
        config: SimulationConfig,
        state: SimulationState,
        logger: logging.Logger,
    ):
        self.config = config
        self.state = state
        self.logger = logger
        self._last_hour_trades: List[float] = []
        self._last_daily_reset = datetime.now(timezone.utc).date()

    def initialize(self) -> bool:
        """初始化引擎"""
        try:
            # 加载虚拟券商
            self.logger.info(
                f"初始化虚拟券商: 余额 {self.config.initial_balance:,.2f}"
            )
            self.state.current_balance = self.config.initial_balance
            self.state.equity = self.config.initial_balance
            self.state.peak_equity = self.config.initial_balance
            return True
        except Exception as e:
            self.logger.error(f"初始化失败: {e}")
            return False

    def tick(self) -> bool:
        """
        单次 tick。
        返回 True 继续，False 停止。
        """
        # 检查停止条件
        if _interrupted:
            self.state.stop_reason = _stop_reason or "interrupted"
            return False

        # 检查运行时长
        if self.config.duration_sec > 0:
            if self.state.duration_sec >= self.config.duration_sec:
                self.state.stop_reason = "duration_reached"
                return False

        # 检查回撤
        if self.state.peak_equity > 0:
            dd = (self.state.peak_equity - self.state.equity) / self.state.peak_equity
            if dd > self.config.max_drawdown:
                self.logger.warning(
                    f"触发最大回撤限制: {dd:.2%} > {self.config.max_drawdown:.2%}"
                )
                self.state.stop_reason = "max_drawdown"
                return False

        # 检查每日亏损
        if self.state.daily_pnl < 0:
            daily_loss_pct = abs(self.state.daily_pnl) / self.config.initial_balance
            if daily_loss_pct > self.config.max_daily_loss:
                self.logger.warning(
                    f"触发每日亏损限制: {daily_loss_pct:.2%} "
                    f"> {self.config.max_daily_loss:.2%}"
                )
                self.state.stop_reason = "max_daily_loss"
                return False

        # 检查交易频率
        now = time.monotonic()
        self._last_hour_trades = [
            t for t in self._last_hour_trades if now - t < 3600
        ]
        if len(self._last_hour_trades) >= self.config.max_trades_per_hour:
            self.logger.debug("交易频率达到上限，等待...")
            return True

        # 重置每日统计
        today = datetime.now(timezone.utc).date()
        if today != self._last_daily_reset:
            self.logger.info(
                f"新交易日: 昨日 PnL = {self.state.daily_pnl:+.2f}"
            )
            self.state.daily_pnl = 0.0
            self._last_daily_reset = today

        # TODO: 实际策略 tick
        # 1. 获取最新 K 线数据（从 WebSocket 或数据库）
        # 2. 计算指标
        # 3. 策略决策
        # 4. 虚拟券商执行
        # 5. 更新状态

        self.state.tick_count += 1
        return True

    def shutdown(self) -> None:
        """优雅关闭"""
        try:
            self.logger.info("执行优雅关闭...")

            # 保存状态快照
            if not self.config.once:
                snapshot_dir = Path("./data/snapshots")
                snapshot_dir.mkdir(parents=True, exist_ok=True)
                snapshot_file = snapshot_dir / f"sim-{self.state.run_id}.json"

                try:
                    with open(snapshot_file, "w", encoding="utf-8") as f:
                        json.dump(self.state.to_dict(), f, ensure_ascii=False, indent=2)
                    self.logger.info(f"状态已保存: {snapshot_file}")
                except OSError as e:
                    self.logger.warning(f"状态保存失败: {e}")

            # TODO: 平仓逻辑（可选）
            # 若配置要求关闭时平仓，在此执行

        except Exception as e:
            self.logger.error(f"关闭时异常: {e}")


# ==============================================================================
# 主流程
# ==============================================================================
def run_simulation(
    config: SimulationConfig,
    logger: logging.Logger,
) -> SimulationState:
    """运行模拟盘"""

    run_id = f"sim-{datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S')}"
    state = SimulationState(
        run_id=run_id,
        start_time=datetime.now(timezone.utc).isoformat(),
        env=config.env,
        symbols=list(config.symbols),
        initial_balance=config.initial_balance,
        current_balance=config.initial_balance,
        equity=config.initial_balance,
        peak_equity=config.initial_balance,
    )

    logger.info(f"运行 ID: {run_id}")
    logger.info(f"环境: {config.env}")
    logger.info(f"交易对: {', '.join(config.symbols)}")
    logger.info(f"初始余额: {config.initial_balance:,.2f}")
    if config.duration_sec > 0:
        logger.info(f"运行时长: {config.duration_sec}s")

    engine = SimulationEngine(config, state, logger)

    if not engine.initialize():
        state.error = "初始化失败"
        state.end_time = datetime.now(timezone.utc).isoformat()
        return state

    # 注册关闭钩子
    def _shutdown():
        try:
            engine.shutdown()
        except Exception as e:
            logger.error(f"关闭钩子异常: {e}")

    atexit.register(_shutdown)

    start_ts = time.monotonic()
    last_log_ts = start_ts

    try:
        while True:
            if not engine.tick():
                break

            state.duration_sec = time.monotonic() - start_ts

            if config.once:
                break

            # 定期日志
            now = time.monotonic()
            if now - last_log_ts >= 60:
                logger.info(
                    f"运行中: {state.duration_sec:.0f}s, "
                    f"ticks={state.tick_count}, "
                    f"equity={state.equity:,.2f}"
                )
                last_log_ts = now

            # 等待下一个 tick（分段睡眠以响应信号）
            try:
                time.sleep(config.tick_interval)
            except InterruptedError:
                break

    except KeyboardInterrupt:
        logger.info("用户中断")
        state.stop_reason = "keyboard_interrupt"
    except Exception as e:
        logger.error(f"运行异常: {e}")
        logger.debug(traceback.format_exc())
        state.error = f"{type(e).__name__}: {e}"
    finally:
        state.duration_sec = time.monotonic() - start_ts
        state.end_time = datetime.now(timezone.utc).isoformat()
        if not state.stop_reason:
            state.stop_reason = "normal"

        try:
            engine.shutdown()
        except Exception as e:
            logger.error(f"关闭异常: {e}")

        try:
            atexit.unregister(_shutdown)
        except Exception:
            pass

    return state


# ==============================================================================
# 输出
# ==============================================================================
def print_report(state: SimulationState, json_mode: bool = False, quiet: bool = False) -> None:
    if json_mode:
        print(json.dumps(state.to_dict(), ensure_ascii=False, indent=2, default=str))
        return

    if quiet:
        return

    print()
    print("━" * 50)
    print("  模拟盘报告")
    print("━" * 50)
    print(f"  运行 ID:    {state.run_id}")
    print(f"  环境:       {state.env}")
    print(f"  交易对:     {', '.join(state.symbols)}")
    print(f"  初始余额:   {state.initial_balance:,.2f}")
    print(f"  最终权益:   {state.equity:,.2f}")
    print(f"  收益率:     {((state.equity - state.initial_balance) / state.initial_balance):+.4%}")
    print(f"  最大回撤:   {state.max_drawdown:.2%}")
    print(f"  总交易:     {state.total_trades}")
    print(f"  胜率:       {(state.winning_trades / state.total_trades if state.total_trades else 0):.2%}")
    print(f"  总手续费:   {state.total_fees:,.2f}")
    print(f"  Tick 次数:  {state.tick_count}")
    print(f"  运行时长:   {state.duration_sec:.0f}s")
    print(f"  停止原因:   {state.stop_reason}")
    if state.error:
        print(f"  错误:       {state.error}")
    print("━" * 50)


# ==============================================================================
# 参数解析
# ==============================================================================
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="运行模拟盘（真实行情 + 虚拟券商）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s --env dev
  %(prog)s --env dev --balance 100000 --duration 3600
  %(prog)s --env dev --symbols BTCUSDT,ETHUSDT
  %(prog)s --env dev --once --json
  %(prog)s --env dev --config config/config.core.params.json

退出码:
  0   正常退出
  1   运行失败
  2   参数错误
  3   环境检查失败
  4   依赖未就绪
  5   内部错误
  6   风控触发停止
  130 用户中断
        """,
    )
    parser.add_argument("--env", default=os.getenv("ENV", "dev"),
                        choices=["dev", "test", "staging", "prod"],
                        help="运行环境（默认 dev）")
    parser.add_argument("--balance", type=float, default=DEFAULT_BALANCE,
                        help=f"虚拟初始余额（默认 {DEFAULT_BALANCE}）")
    parser.add_argument("--symbols", default="BTCUSDT,ETHUSDT",
                        help="交易对（逗号分隔）")
    parser.add_argument("--interval", default="3m", help="K线周期")
    parser.add_argument("--duration", type=int, default=DEFAULT_DURATION,
                        help="运行时长秒（0=无限）")
    parser.add_argument("--tick-interval", type=float, default=DEFAULT_TICK_INTERVAL,
                        help=f"tick 间隔秒（默认 {DEFAULT_TICK_INTERVAL}）")
    parser.add_argument("--max-drawdown", type=float, default=DEFAULT_MAX_DRAWDOWN,
                        help=f"最大回撤（默认 {DEFAULT_MAX_DRAWDOWN}）")
    parser.add_argument("--max-daily-loss", type=float, default=DEFAULT_MAX_DAILY_LOSS,
                        help=f"每日最大亏损（默认 {DEFAULT_MAX_DAILY_LOSS}）")
    parser.add_argument("--max-trades-per-hour", type=int,
                        default=DEFAULT_MAX_TRADES_PER_HOUR,
                        help="每小时最大交易数")
    parser.add_argument("--fee-rate", type=float, default=0.0004, help="手续费率")
    parser.add_argument("--slippage", type=float, default=0.0002, help="滑点")
    parser.add_argument("--strategy", default="trend_follow", help="策略名称")
    parser.add_argument("--config", type=Path, help="配置文件路径")
    parser.add_argument("--once", action="store_true",
                        help="运行一次 tick 后退出（用于调试）")
    parser.add_argument("--json", action="store_true", help="JSON 输出")
    parser.add_argument("--quiet", action="store_true", help="静默模式")
    parser.add_argument("--verbose", action="store_true", help="详细输出")
    parser.add_argument("--no-color", action="store_true", help="禁用颜色")
    parser.add_argument("--version-script", action="version",
                        version=f"run_simulation.py {VERSION}")
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


# ==============================================================================
# 主函数
# ==============================================================================
def main() -> int:
    args = parse_args()

    signal.signal(signal.SIGINT, _signal_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _signal_handler)

    # 日志
    log_dir = Path("./data/logs")
    log_file = log_dir / f"simulation-{datetime.now(timezone.utc).strftime('%Y%m%d')}.log"
    logger = setup_logging(
        verbose=args.verbose,
        log_file=log_file,
        quiet=args.quiet or args.json,
    )

    # 配置
    try:
        config_data: Dict[str, Any] = {}
        if args.config:
            config_data = load_config_file(args.config)

        symbols = [
            s.strip().upper()
            for s in (args.symbols or ",".join(config_data.get("symbols", ["BTCUSDT"]))).split(",")
            if s.strip()
        ]

        config = SimulationConfig(
            env=args.env,
            symbols=symbols,
            interval=args.interval or config_data.get("interval", "3m"),
            initial_balance=args.balance,
            duration_sec=args.duration,
            tick_interval=args.tick_interval,
            max_drawdown=args.max_drawdown,
            max_daily_loss=args.max_daily_loss,
            max_trades_per_hour=args.max_trades_per_hour,
            fee_rate=args.fee_rate,
            slippage=args.slippage,
            strategy=args.strategy or config_data.get("strategy", "trend_follow"),
            params=config_data.get("params", {}),
            once=args.once,
            json_output=args.json,
        )

    except FileNotFoundError as e:
        sys.stderr.write(f"❌ {e}\n")
        return 2
    except ValueError as e:
        sys.stderr.write(f"❌ 配置错误: {e}\n")
        return 2

    # 校验
    errors = config.validate()
    if errors:
        for e in errors:
            sys.stderr.write(f"❌ {e}\n")
        return 2

    # 环境检查
    logger.info(f"模拟盘启动 v{VERSION}")
    check_dependencies(logger)

    # 运行
    try:
        state = run_simulation(config, logger)
    except KeyboardInterrupt:
        return 130
    except Exception as e:
        logger.error(f"内部错误: {e}")
        logger.debug(traceback.format_exc())
        return 5

    # 输出
    print_report(state, json_mode=args.json, quiet=args.quiet)

    # 退出码
    if state.error:
        return 1
    if state.stop_reason in ("max_drawdown", "max_daily_loss"):
        return 6
    return 0


if __name__ == "__main__":
    sys.exit(main())
