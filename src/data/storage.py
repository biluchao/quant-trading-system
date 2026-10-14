"""---UMS-BEGIN---
spec_version: 1.0
module: data
type: core
name: storage
version: 2.0.0
level: 2
required: false
author: quant-team
created: 2026-10-14
brief: 持久化存储（异步/连接池/UPSERT/多级降级/迁移/自动清理）

provides:
  - class Storage
  - class StorageConfig
  - class StorageStats
  - class StorageBackend
  - function get_storage
  - function init_storage

requires:
  modules: [core.logger, core.timestamp, core.config, core.secrets]
  libraries:
    - psycopg>=3.2  (async)
    - psycopg_pool>=3.2  (连接池)
    - aiosqlite>=0.20  (降级)
  resources:
    memory_mb: 64
    cpu_cores: 1
    network: postgresql:5432

health:
  fn: Storage.health
  timeout_ms: 2000

assembly:
  order: 32
  parallel: false

tests:
  - tests/test_storage.py

fallback:
  - PostgreSQL 不可用 → SQLite
  - SQLite 不可用 → 内存
  - 迁移失败 → 使用现有 schema

tags: [data, storage, persistence]
---UMS-END---"""

from __future__ import annotations

import asyncio
import json
import os
import threading
import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# 延迟 logger
_logger = None


def _log() -> Any:
    global _logger
    if _logger is None:
        try:
            from core.logger import get_logger
            _logger = get_logger("storage")
        except Exception:
            import logging
            _logger = logging.getLogger("storage")
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
    import psycopg  # type: ignore
    from psycopg_pool import AsyncConnectionPool  # type: ignore
    _HAS_PSYCOPG = True
except ImportError:
    _HAS_PSYCOPG = False

try:
    import aiosqlite  # type: ignore
    _HAS_AIOSQLITE = True
except ImportError:
    _HAS_AIOSQLITE = False


# ============================================================
# 常量
# ============================================================
DEFAULT_MAX_QUERY_ROWS = 5000
DEFAULT_BATCH_SIZE = 500
DEFAULT_RETENTION_DAYS = 30
DEFAULT_POOL_MIN = 2
DEFAULT_POOL_MAX = 10
DEFAULT_POOL_TIMEOUT_S = 5.0
SCHEMA_VERSION = 1


# ============================================================
# 异常
# ============================================================
class StorageError(Exception):
    """存储基类异常"""


class StorageNotAvailable(StorageError):
    """所有后端均不可用"""


class StorageTimeout(StorageError):
    """操作超时"""


class MigrationError(StorageError):
    """迁移失败"""


# ============================================================
# 数据结构
# ============================================================
class StorageBackend(str, Enum):
    POSTGRES = "postgres"
    SQLITE = "sqlite"
    MEMORY = "memory"


@dataclass
class StorageConfig:
    # PostgreSQL
    pg_host: str = "localhost"
    pg_port: int = 5432
    pg_db: str = "quant"
    pg_user: str = "quant"
    pg_password: str = ""
    # 池
    pool_min: int = DEFAULT_POOL_MIN
    pool_max: int = DEFAULT_POOL_MAX
    pool_timeout_s: float = DEFAULT_POOL_TIMEOUT_S
    # SQLite 降级
    sqlite_path: str = "data/quant.db"
    # 通用
    max_query_rows: int = DEFAULT_MAX_QUERY_ROWS
    batch_size: int = DEFAULT_BATCH_SIZE
    retention_days: int = DEFAULT_RETENTION_DAYS
    auto_migrate: bool = True


@dataclass
class StorageStats:
    writes: int = 0
    reads: int = 0
    write_errors: int = 0
    read_errors: int = 0
    total_rows_written: int = 0
    total_rows_read: int = 0
    reconnects: int = 0
    fallbacks: int = 0
    last_write_ms: int = 0
    last_read_ms: int = 0
    started_ms: int = field(default_factory=_now_ms)

    def to_dict(self) -> Dict[str, Any]:
        return {k: getattr(self, k) for k in (
            "writes", "reads", "write_errors", "read_errors",
            "total_rows_written", "total_rows_read",
            "reconnects", "fallbacks",
            "last_write_ms", "last_read_ms", "started_ms",
        )}


# ============================================================
# 抽象后端接口
# ============================================================
class _Backend:
    """后端接口基类"""

    name: str = "abstract"

    async def connect(self) -> None:
        raise NotImplementedError

    async def close(self) -> None:
        raise NotImplementedError

    async def upsert_candles(
        self, candles: List[Dict[str, Any]]) -> int:
        raise NotImplementedError

    async def query_candles(
        self, symbol: str, interval: str,
        start_ms: int, end_ms: int, limit: int,
    ) -> List[Dict[str, Any]]:
        raise NotImplementedError

    async def delete_before(
        self, cutoff_ms: int,
    ) -> int:
        raise NotImplementedError

    async def count(
        self, symbol: str, interval: str,
    ) -> int:
        raise NotImplementedError

    async def health(self) -> Dict[str, Any]:
        raise NotImplementedError

    async def migrate(self) -> None:
        raise NotImplementedError


# ============================================================
# PostgreSQL 后端
# ============================================================
class _PostgresBackend(_Backend):
    name = "postgres"

    def __init__(self, cfg: StorageConfig) -> None:
        self.cfg = cfg
        self._pool: Optional[Any] = None

    def _conninfo(self) -> str:
        return (
            f"host={self.cfg.pg_host} port={self.cfg.pg_port} "
            f"dbname={self.cfg.pg_db} user={self.cfg.pg_user} "
            f"password={self.cfg.pg_password} "
            f"connect_timeout=5 application_name=quant"
        )

    async def connect(self) -> None:
        if not _HAS_PSYCOPG:
            raise StorageNotAvailable("psycopg 未安装")
        self._pool = AsyncConnectionPool(
            conninfo=self._conninfo(),
            min_size=self.cfg.pool_min,
            max_size=self.cfg.pool_max,
            timeout=self.cfg.pool_timeout_s,
            open=False,
        )
        await self._pool.open()
        # 验证连接
        async with self._pool.connection() as conn:
            async with conn.cursor() as cur:
                await cur.execute("SELECT 1")
                await cur.fetchone()

    async def close(self) -> None:
        if self._pool is not None:
            await self._pool.close()
            self._pool = None

    async def migrate(self) -> None:
        if self._pool is None:
            raise StorageNotAvailable("未连接")
        async with self._pool.connection() as conn:
            async with conn.cursor() as cur:
                await cur.execute("""
                    CREATE TABLE IF NOT EXISTS candles (
                        symbol VARCHAR(32) NOT NULL,
                        interval VARCHAR(8) NOT NULL,
                        open_time BIGINT NOT NULL,
                        close_time BIGINT NOT NULL,
                        open NUMERIC(24, 10) NOT NULL,
                        high NUMERIC(24, 10) NOT NULL,
                        low NUMERIC(24, 10) NOT NULL,
                        close NUMERIC(24, 10) NOT NULL,
                        volume NUMERIC(24, 10) NOT NULL,
                        quote_volume NUMERIC(24, 10) DEFAULT 0,
                        trades INTEGER DEFAULT 0,
                        is_closed BOOLEAN DEFAULT TRUE,
                        created_ms BIGINT NOT NULL,
                        PRIMARY KEY (symbol, interval, open_time)
                    )
                """)
                await cur.execute("""
                    CREATE INDEX IF NOT EXISTS idx_candles_time
                    ON candles (symbol, interval, open_time DESC)
                """)
                await cur.execute("""
                    CREATE TABLE IF NOT EXISTS schema_version (
                        version INTEGER PRIMARY KEY,
                        applied_ms BIGINT NOT NULL
                    )
                """)
                await cur.execute(
                    "INSERT INTO schema_version (version, applied_ms) "
                    "VALUES (%s, %s) ON CONFLICT DO NOTHING",
                    (SCHEMA_VERSION, _now_ms()),
                )
            await conn.commit()

    async def upsert_candles(
        self, candles: List[Dict[str, Any]],
    ) -> int:
        if self._pool is None:
            raise StorageNotAvailable("未连接")
        if not candles:
            return 0
        rows = [
            (
                c.get("symbol", "").lower(),
                c.get("interval", ""),
                int(c.get("open_time", 0)),
                int(c.get("close_time", 0)),
                float(c.get("open", 0)),
                float(c.get("high", 0)),
                float(c.get("low", 0)),
                float(c.get("close", 0)),
                float(c.get("volume", 0)),
                float(c.get("quote_volume", 0)),
                int(c.get("trades", 0)),
                bool(c.get("is_closed", True)),
                _now_ms(),
            )
            for c in candles
        ]
        async with self._pool.connection() as conn:
            async with conn.cursor() as cur:
                await cur.executemany("""
                    INSERT INTO candles (
                        symbol, interval, open_time, close_time,
                        open, high, low, close, volume,
                        quote_volume, trades, is_closed, created_ms
                    ) VALUES (
                        %s, %s, %s, %s, %s, %s, %s, %s, %s,
                        %s, %s, %s, %s
                    )
                    ON CONFLICT (symbol, interval, open_time)
                    DO UPDATE SET
                        close_time = EXCLUDED.close_time,
                        open = EXCLUDED.open,
                        high = EXCLUDED.high,
                        low = EXCLUDED.low,
                        close = EXCLUDED.close,
                        volume = EXCLUDED.volume,
                        quote_volume = EXCLUDED.quote_volume,
                        trades = EXCLUDED.trades,
                        is_closed = EXCLUDED.is_closed
                """, rows)
            await conn.commit()
        return len(rows)

    async def query_candles(
        self, symbol: str, interval: str,
        start_ms: int, end_ms: int, limit: int,
    ) -> List[Dict[str, Any]]:
        if self._pool is None:
            raise StorageNotAvailable("未连接")
        async with self._pool.connection() as conn:
            async with conn.cursor() as cur:
                await cur.execute("""
                    SELECT symbol, interval, open_time, close_time,
                           open, high, low, close, volume,
                           quote_volume, trades, is_closed
                    FROM candles
                    WHERE symbol = %s AND interval = %s
                      AND open_time >= %s AND open_time <= %s
                    ORDER BY open_time ASC
                    LIMIT %s
                """, (symbol.lower(), interval,
                      start_ms, end_ms, limit))
                rows = await cur.fetchall()
        out: List[Dict[str, Any]] = []
        for r in rows:
            out.append({
                "symbol": r[0], "interval": r[1],
                "open_time": int(r[2]), "close_time": int(r[3]),
                "open": float(r[4]), "high": float(r[5]),
                "low": float(r[6]), "close": float(r[7]),
                "volume": float(r[8]), "quote_volume": float(r[9]),
                "trades": int(r[10]), "is_closed": bool(r[11]),
            })
        return out

    async def delete_before(self, cutoff_ms: int) -> int:
        if self._pool is None:
            return 0
        async with self._pool.connection() as conn:
            async with conn.cursor() as cur:
                await cur.execute(
                    "DELETE FROM candles WHERE open_time < %s",
                    (cutoff_ms,))
                affected = cur.rowcount
            await conn.commit()
        return affected

    async def count(self, symbol: str, interval: str) -> int:
        if self._pool is None:
            return 0
        async with self._pool.connection() as conn:
            async with conn.cursor() as cur:
                await cur.execute(
                    "SELECT COUNT(*) FROM candles "
                    "WHERE symbol = %s AND interval = %s",
                    (symbol.lower(), interval))
                row = await cur.fetchone()
        return int(row[0]) if row else 0

    async def health(self) -> Dict[str, Any]:
        try:
            if self._pool is None:
                return {"status": "down", "reason": "no_pool"}
            async with self._pool.connection() as conn:
                async with conn.cursor() as cur:
                    await cur.execute("SELECT 1")
                    await cur.fetchone()
            return {"status": "ok"}
        except Exception as exc:
            return {"status": "down", "error": str(exc)}


# ============================================================
# SQLite 后端
# ============================================================
class _SQLiteBackend(_Backend):
    name = "sqlite"

    def __init__(self, cfg: StorageConfig) -> None:
        self.cfg = cfg
        self._path = Path(cfg.sqlite_path)
        self._conn: Optional[Any] = None
        self._lock: Optional[asyncio.Lock] = None

    async def connect(self) -> None:
        if not _HAS_AIOSQLITE:
            raise StorageNotAvailable("aiosqlite 未安装")
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._conn = await aiosqlite.connect(str(self._path))
        self._conn.row_factory = aiosqlite.Row
        await self._conn.execute("PRAGMA journal_mode=WAL")
        await self._conn.execute("PRAGMA synchronous=NORMAL")
        self._lock = asyncio.Lock()

    async def close(self) -> None:
        if self._conn is not None:
            await self._conn.close()
            self._conn = None

    async def migrate(self) -> None:
        if self._conn is None:
            raise StorageNotAvailable("未连接")
        await self._conn.execute("""
            CREATE TABLE IF NOT EXISTS candles (
                symbol TEXT NOT NULL,
                interval TEXT NOT NULL,
                open_time INTEGER NOT NULL,
                close_time INTEGER NOT NULL,
                open REAL NOT NULL,
                high REAL NOT NULL,
                low REAL NOT NULL,
                close REAL NOT NULL,
                volume REAL NOT NULL,
                quote_volume REAL DEFAULT 0,
                trades INTEGER DEFAULT 0,
                is_closed INTEGER DEFAULT 1,
                created_ms INTEGER NOT NULL,
                PRIMARY KEY (symbol, interval, open_time)
            )
        """)
        await self._conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_candles_time
            ON candles (symbol, interval, open_time DESC)
        """)
        await self._conn.commit()

    async def upsert_candles(
        self, candles: List[Dict[str, Any]],
    ) -> int:
        if self._conn is None or self._lock is None:
            raise StorageNotAvailable("未连接")
        if not candles:
            return 0
        rows = [
            (
                c.get("symbol", "").lower(),
                c.get("interval", ""),
                int(c.get("open_time", 0)),
                int(c.get("close_time", 0)),
                float(c.get("open", 0)),
                float(c.get("high", 0)),
                float(c.get("low", 0)),
                float(c.get("close", 0)),
                float(c.get("volume", 0)),
                float(c.get("quote_volume", 0)),
                int(c.get("trades", 0)),
                1 if c.get("is_closed", True) else 0,
                _now_ms(),
            )
            for c in candles
        ]
        async with self._lock:
            await self._conn.executemany("""
                INSERT INTO candles (
                    symbol, interval, open_time, close_time,
                    open, high, low, close, volume,
                    quote_volume, trades, is_closed, created_ms
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT (symbol, interval, open_time)
                DO UPDATE SET
                    close_time = excluded.close_time,
                    open = excluded.open,
                    high = excluded.high,
                    low = excluded.low,
                    close = excluded.close,
                    volume = excluded.volume,
                    quote_volume = excluded.quote_volume,
                    trades = excluded.trades,
                    is_closed = excluded.is_closed
            """, rows)
            await self._conn.commit()
        return len(rows)

    async def query_candles(
        self, symbol: str, interval: str,
        start_ms: int, end_ms: int, limit: int,
    ) -> List[Dict[str, Any]]:
        if self._conn is None:
            raise StorageNotAvailable("未连接")
        async with self._conn.execute("""
            SELECT symbol, interval, open_time, close_time,
                   open, high, low, close, volume,
                   quote_volume, trades, is_closed
            FROM candles
            WHERE symbol = ? AND interval = ?
              AND open_time >= ? AND open_time <= ?
            ORDER BY open_time ASC
            LIMIT ?
        """, (symbol.lower(), interval,
              start_ms, end_ms, limit)) as cur:
            rows = await cur.fetchall()
        return [
            {
                "symbol": r["symbol"], "interval": r["interval"],
                "open_time": r["open_time"],
                "close_time": r["close_time"],
                "open": r["open"], "high": r["high"],
                "low": r["low"], "close": r["close"],
                "volume": r["volume"],
                "quote_volume": r["quote_volume"],
                "trades": r["trades"],
                "is_closed": bool(r["is_closed"]),
            }
            for r in rows
        ]

    async def delete_before(self, cutoff_ms: int) -> int:
        if self._conn is None:
            return 0
        async with self._conn.execute(
            "DELETE FROM candles WHERE open_time < ?",
            (cutoff_ms,)) as cur:
            await self._conn.commit()
            return cur.rowcount or 0

    async def count(self, symbol: str, interval: str) -> int:
        if self._conn is None:
            return 0
        async with self._conn.execute(
            "SELECT COUNT(*) FROM candles "
            "WHERE symbol = ? AND interval = ?",
            (symbol.lower(), interval)) as cur:
            row = await cur.fetchone()
        return int(row[0]) if row else 0

    async def health(self) -> Dict[str, Any]:
        try:
            if self._conn is None:
                return {"status": "down", "reason": "no_conn"}
            async with self._conn.execute("SELECT 1") as cur:
                await cur.fetchone()
            return {"status": "ok"}
        except Exception as exc:
            return {"status": "down", "error": str(exc)}


# ============================================================
# 内存后端（最后降级）
# ============================================================
class _MemoryBackend(_Backend):
    name = "memory"

    def __init__(self, cfg: StorageConfig) -> None:
        self.cfg = cfg
        # key: (symbol, interval, open_time) → candle
        self._data: Dict[Tuple[str, str, int], Dict[str, Any]] = {}
        self._lock = threading.RLock()

    async def connect(self) -> None:
        pass

    async def close(self) -> None:
        with self._lock:
            self._data.clear()

    async def migrate(self) -> None:
        pass

    async def upsert_candles(
        self, candles: List[Dict[str, Any]],
    ) -> int:
        with self._lock:
            for c in candles:
                k = (c.get("symbol", "").lower(),
                     c.get("interval", ""),
                     int(c.get("open_time", 0)))
                self._data[k] = dict(c)
        return len(candles)

    async def query_candles(
        self, symbol: str, interval: str,
        start_ms: int, end_ms: int, limit: int,
    ) -> List[Dict[str, Any]]:
        sym = symbol.lower()
        with self._lock:
            hits = [
                c for (s, i, t), c in self._data.items()
                if s == sym and i == interval
                and start_ms <= t <= end_ms
            ]
        hits.sort(key=lambda x: x["open_time"])
        return hits[:limit]

    async def delete_before(self, cutoff_ms: int) -> int:
        with self._lock:
            keys = [
                k for k, c in self._data.items()
                if c.get("open_time", 0) < cutoff_ms
            ]
            for k in keys:
                self._data.pop(k, None)
            return len(keys)

    async def count(self, symbol: str, interval: str) -> int:
        sym = symbol.lower()
        with self._lock:
            return sum(
                1 for (s, i, _) in self._data
                if s == sym and i == interval)

    async def health(self) -> Dict[str, Any]:
        with self._lock:
            return {"status": "ok",
                    "rows": len(self._data)}


# ============================================================
# Storage 单例
# ============================================================
class Storage:
    """
    持久化存储。

    后端优先级：PostgreSQL → SQLite → Memory
    """

    _instance: Optional["Storage"] = None
    _instance_lock = threading.Lock()

    def __new__(cls, *args: Any, **kwargs: Any) -> "Storage":
        with cls._instance_lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(
        self, config: Optional[StorageConfig] = None,
    ) -> None:
        if getattr(self, "_initialized", False):
            return
        with self._instance_lock:
            if getattr(self, "_initialized", False):
                return
            self._lock = threading.RLock()
            self._async_lock: Optional[asyncio.Lock] = None
            self.config = config or self._load_config()
            self._backend: Optional[_Backend] = None
            self._backend_name: str = "none"
            self._stats = StorageStats()
            self._initialized = True

    # ========================================================
    # 配置加载
    # ========================================================
    def _load_config(self) -> StorageConfig:
        cfg = StorageConfig()
        try:
            from core.config import get_config
            c = get_config()
            cfg.pg_host = c.get("storage.pg_host", cfg.pg_host)
            cfg.pg_port = int(c.get("storage.pg_port", cfg.pg_port))
            cfg.pg_db = c.get("storage.pg_db", cfg.pg_db)
            cfg.pg_user = c.get("storage.pg_user", cfg.pg_user)
            cfg.sqlite_path = c.get(
                "storage.sqlite_path", cfg.sqlite_path)
        except Exception:
            pass
        try:
            from core.secrets import get_secrets
            s = get_secrets()
            cfg.pg_password = s.get(
                "db_password",
                os.environ.get("DB_PASSWORD", ""))
        except Exception:
            cfg.pg_password = os.environ.get("DB_PASSWORD", "")
        return cfg

    # ========================================================
    # 连接（多级降级）
    # ========================================================
    async def connect(self, migrate: bool = True) -> str:
        """尝试连接各后端，返回使用的后端名"""
        if self._async_lock is None:
            self._async_lock = asyncio.Lock()

        async with self._async_lock:
            if self._backend is not None:
                return self._backend_name

            backends: List[_Backend] = [
                _PostgresBackend(self.config),
                _SQLiteBackend(self.config),
                _MemoryBackend(self.config),
            ]

            last_err = ""
            for be in backends:
                try:
                    await be.connect()
                    if migrate and self.config.auto_migrate:
                        await be.migrate()
                    self._backend = be
                    self._backend_name = be.name
                    if be.name != "postgres":
                        self._stats.fallbacks += 1
                    _log().info(
                        f"存储后端已连接: {be.name}")
                    return be.name
                except Exception as exc:
                    last_err = f"{be.name}: {exc}"
                    _log().warning(
                        f"后端 {be.name} 不可用: {exc}")
                    try:
                        await be.close()
                    except Exception:
                        pass

            raise StorageNotAvailable(
                f"所有存储后端失败: {last_err}")

    async def close(self) -> None:
        if self._backend is not None:
            try:
                await self._backend.close()
            except Exception:
                pass
            self._backend = None
            self._backend_name = "none"

    async def reconnect(self) -> str:
        await self.close()
        self._stats.reconnects += 1
        return await self.connect()

    def _ensure(self) -> _Backend:
        if self._backend is None:
            raise StorageNotAvailable("存储未连接")
        return self._backend

    # ========================================================
    # 写入
    # ========================================================
    async def save_candle(
        self, candle: Dict[str, Any],
    ) -> bool:
        return await self.save_candles([candle]) > 0

    async def save_candles(
        self, candles: List[Dict[str, Any]],
    ) -> int:
        if not candles:
            return 0
        try:
            be = self._ensure()
        except StorageNotAvailable:
            # 尝试重连一次
            try:
                await self.reconnect()
                be = self._ensure()
            except Exception as exc:
                _log().error(f"存储不可用: {exc}")
                self._stats.write_errors += 1
                return 0

        total = 0
        bs = max(1, self.config.batch_size)
        for i in range(0, len(candles), bs):
            chunk = candles[i:i + bs]
            try:
                n = await be.upsert_candles(chunk)
                total += n
                self._stats.total_rows_written += n
            except Exception as exc:
                _log().warning(f"批量写入失败: {exc}")
                self._stats.write_errors += 1
                # 尝试重连
                try:
                    await self.reconnect()
                except Exception:
                    pass
                return total

        self._stats.writes += 1
        self._stats.last_write_ms = _now_ms()
        return total

    # ========================================================
    # 读取
    # ========================================================
    async def get_candles(
        self,
        symbol: str,
        interval: str,
        start_ms: int,
        end_ms: int,
        limit: Optional[int] = None,
    ) -> List[Dict[str, Any]]:
        try:
            be = self._ensure()
        except StorageNotAvailable:
            try:
                await self.reconnect()
                be = self._ensure()
            except Exception:
                self._stats.read_errors += 1
                return []

        lim = min(
            limit or self.config.max_query_rows,
            self.config.max_query_rows)
        try:
            rows = await be.query_candles(
                symbol, interval, start_ms, end_ms, lim)
            self._stats.reads += 1
            self._stats.total_rows_read += len(rows)
            self._stats.last_read_ms = _now_ms()
            return rows
        except Exception as exc:
            _log().warning(f"查询失败: {exc}")
            self._stats.read_errors += 1
            return []

    async def count_candles(
        self, symbol: str, interval: str,
    ) -> int:
        try:
            be = self._ensure()
            return await be.count(symbol, interval)
        except Exception:
            return 0

    # ========================================================
    # 清理
    # ========================================================
    async def cleanup_old(self, days: Optional[int] = None) -> int:
        d = days if days is not None else self.config.retention_days
        if d <= 0:
            return 0
        cutoff = _now_ms() - d * 86_400_000
        try:
            be = self._ensure()
            n = await be.delete_before(cutoff)
            _log().info(f"清理 {n} 条 {d} 天前数据")
            return n
        except Exception as exc:
            _log().warning(f"清理失败: {exc}")
            return 0

    # ========================================================
    # 状态
    # ========================================================
    def backend(self) -> str:
        return self._backend_name

    def stats(self) -> Dict[str, Any]:
        with self._lock:
            s = self._stats.to_dict()
            s["backend"] = self._backend_name
            return s

    async def health(self) -> Dict[str, Any]:
        if self._backend is None:
            return {"status": "down", "backend": "none"}
        try:
            h = await self._backend.health()
            return {
                "status": h.get("status", "unknown"),
                "backend": self._backend_name,
                "detail": h,
                "stats": self._stats.to_dict(),
            }
        except Exception as exc:
            return {
                "status": "degraded",
                "backend": self._backend_name,
                "error": str(exc),
            }

    def __repr__(self) -> str:
        return (
            f"<Storage backend={self._backend_name} "
            f"writes={self._stats.writes} "
            f"reads={self._stats.reads}>"
        )


# ============================================================
# 全局入口
# ============================================================
_storage_instance: Optional[Storage] = None
_storage_lock = threading.Lock()


def init_storage(
    config: Optional[StorageConfig] = None,
) -> Storage:
    """显式初始化（启动时调用）"""
    global _storage_instance
    with _storage_lock:
        _storage_instance = Storage(config)
    return _storage_instance


def get_storage() -> Storage:
    """获取单例（惰性初始化）"""
    global _storage_instance
    if _storage_instance is None:
        with _storage_lock:
            if _storage_instance is None:
                _storage_instance = Storage()
    return _storage_instance
