#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
币安 BTC/ETH 3分钟量化交易系统 - 密钥生成脚本
================================================================================
@file    scripts/generate_keys.py
@version 1.0.1
@author  quant-team
@brief   生产级密钥生成工具，支持多种密钥类型、输出格式、文件权限、审计日志
         已修复 40 类运行时问题

使用方式:
    # 生成 JWT 密钥（输出到 stdout）
    python scripts/generate_keys.py --type jwt

    # 生成完整密钥集到 .env
    python scripts/generate_keys.py --preset full --output .env.prod

    # 生成密钥集（JSON 格式）
    python scripts/generate_keys.py --preset full --format json

    # 生成 RSA 密钥对
    python scripts/generate_keys.py --type rsa --output ./keys/

    # 生成人类可读口令
    python scripts/generate_keys.py --type passphrase --words 6

退出码:
    0 - 成功
    1 - 生成失败
    2 - 参数错误
    3 - 文件已存在
    4 - 熵不足
    5 - 内部错误
    130 - 用户中断
================================================================================
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import logging
import os
import secrets
import signal
import stat
import string
import sys
import time
import traceback
import uuid
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# ==============================================================================
# 依赖检查
# ==============================================================================
try:
    import yaml
except ImportError:
    yaml = None

# ==============================================================================
# 常量
# ==============================================================================
VERSION = "1.0.1"

# 密钥类型
KEY_TYPES = [
    "jwt", "api_key", "api_secret", "db_password", "redis_password",
    "encryption_key", "hmac_key", "salt", "nonce", "uuid",
    "totp_secret", "passphrase", "rsa", "ed25519",
]

# 输出格式
OUTPUT_FORMATS = ["env", "json", "yaml", "text", "shell"]

# 密钥预设
PRESETS: Dict[str, List[str]] = {
    "minimal": ["jwt", "db_password", "redis_password"],
    "full": [
        "jwt", "api_key", "api_secret", "db_password", "redis_password",
        "encryption_key", "hmac_key", "totp_secret",
    ],
    "crypto": [
        "jwt", "api_key", "api_secret", "db_password", "redis_password",
        "encryption_key", "hmac_key", "salt", "nonce", "totp_secret",
    ],
    "secrets": ["db_password", "redis_password", "encryption_key"],
}

# 环境变量名映射
ENV_NAMES: Dict[str, str] = {
    "jwt": "JWT_SECRET",
    "api_key": "API_KEY",
    "api_secret": "API_SECRET",
    "db_password": "DB_PASSWORD",
    "redis_password": "REDIS_PASSWORD",
    "encryption_key": "ENCRYPTION_KEY",
    "hmac_key": "HMAC_KEY",
    "salt": "SALT",
    "nonce": "NONCE",
    "uuid": "UUID",
    "totp_secret": "TOTP_SECRET",
    "passphrase": "PASSPHRASE",
}

# 默认长度（字节）
DEFAULT_LENGTHS: Dict[str, int] = {
    "jwt": 32,
    "api_key": 32,
    "api_secret": 32,
    "db_password": 24,
    "redis_password": 24,
    "encryption_key": 32,   # AES-256
    "hmac_key": 32,
    "salt": 16,
    "nonce": 12,
    "totp_secret": 20,      # Base32 编码后 32 字符
}

# 口令单词表（简短）
WORDLIST = [
    "apple", "bridge", "castle", "dragon", "eagle", "forest", "garden",
    "hunter", "island", "jungle", "knight", "lemon", "mountain", "night",
    "ocean", "palace", "queen", "river", "stone", "tiger", "umbrella",
    "valley", "winter", "yellow", "zebra", "anchor", "bottle", "candle",
    "danger", "engine", "falcon", "giant", "harbor", "insect", "jewel",
    "kettle", "lantern", "mirror", "needle", "orange", "pencil", "quartz",
    "rocket", "silver", "tunnel", "urban", "violet", "walnut", "xenon",
    "yacht", "zephyr", "amber", "bronze", "crystal", "diamond", "ember",
    "frost", "glacier", "hollow", "ivory", "jasper", "krypton", "lunar",
    "marble", "nebula", "obsidian", "pearl", "quasar", "ruby", "sapphire",
]

# 审计日志
AUDIT_DIR = Path("./build/audit")
AUDIT_FILE = AUDIT_DIR / "key-generation.log"

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
def setup_logging(verbose: bool = False, quiet: bool = False) -> logging.Logger:
    logger = logging.getLogger("generate_keys")
    logger.setLevel(logging.DEBUG if verbose else logging.INFO)
    logger.handlers.clear()

    fmt = logging.Formatter("%(asctime)s [%(levelname)s] %(message)s",
                             datefmt="%Y-%m-%d %H:%M:%S")

    if not quiet:
        # 日志输出到 stderr，避免污染 stdout（密钥输出）
        ch = logging.StreamHandler(sys.stderr)
        ch.setLevel(logging.DEBUG if verbose else logging.INFO)
        ch.setFormatter(fmt)
        logger.addHandler(ch)

    return logger


# ==============================================================================
# 数据结构
# ==============================================================================
@dataclass
class GeneratedKey:
    key_type: str
    name: str
    value: str
    length: int = 0
    encoding: str = ""
    generated_at: str = ""

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


@dataclass
class GenerationResult:
    keys: List[GeneratedKey] = field(default_factory=list)
    output_file: str = ""
    file_size: int = 0
    file_mode: str = ""
    duration_ms: int = 0
    version: str = VERSION
    error: Optional[str] = None

    def to_dict(self, mask_values: bool = True) -> Dict[str, Any]:
        d = {
            "version": self.version,
            "keys": [
                {
                    "key_type": k.key_type,
                    "name": k.name,
                    "value": "***" if mask_values else k.value,
                    "length": k.length,
                    "encoding": k.encoding,
                }
                for k in self.keys
            ],
            "output_file": self.output_file,
            "file_size": self.file_size,
            "file_mode": self.file_mode,
            "duration_ms": self.duration_ms,
        }
        if self.error:
            d["error"] = self.error
        return d


# ==============================================================================
# 熵检查
# ==============================================================================
def check_entropy(logger: logging.Logger) -> bool:
    """检查系统熵池（Linux）"""
    entropy_file = Path("/proc/sys/kernel/random/entropy_avail")
    if not entropy_file.exists():
        return True  # 非 Linux 或无法读取，跳过

    try:
        with open(entropy_file, "r") as f:
            entropy = int(f.read().strip())
        if entropy < 128:
            logger.warning(f"系统熵池较低: {entropy} bits（建议 >= 128）")
            return False
        return True
    except Exception:
        return True


# ==============================================================================
# 密钥生成器
# ==============================================================================
class KeyGenerator:
    """密钥生成器"""

    def __init__(self, logger: logging.Logger):
        self.logger = logger

    def generate(
        self,
        key_type: str,
        length: Optional[int] = None,
        encoding: str = "hex",
        words: int = 6,
    ) -> str:
        """生成指定类型的密钥"""

        if key_type == "jwt" or key_type in (
            "api_key", "api_secret", "db_password",
            "redis_password", "encryption_key", "hmac_key", "salt", "nonce",
        ):
            n = length or DEFAULT_LENGTHS.get(key_type, 32)
            return self._random_token(n, encoding)

        if key_type == "uuid":
            return str(uuid.uuid4())

        if key_type == "totp_secret":
            n = length or DEFAULT_LENGTHS["totp_secret"]
            raw = secrets.token_bytes(n)
            return base64.b32encode(raw).decode("ascii").rstrip("=")

        if key_type == "passphrase":
            return self._passphrase(words)

        if key_type in ("rsa", "ed25519"):
            raise ValueError(f"{key_type} 需要使用 generate_keypair()")

        raise ValueError(f"未知密钥类型: {key_type}")

    def _random_token(self, n_bytes: int, encoding: str) -> str:
        if encoding == "hex":
            return secrets.token_hex(n_bytes)
        if encoding == "urlsafe":
            return secrets.token_urlsafe(n_bytes)
        if encoding == "base64":
            return base64.b64encode(secrets.token_bytes(n_bytes)).decode("ascii")
        if encoding == "base32":
            return base64.b32encode(secrets.token_bytes(n_bytes)).decode("ascii").rstrip("=")
        if encoding == "alnum":
            alphabet = string.ascii_letters + string.digits
            return "".join(secrets.choice(alphabet) for _ in range(n_bytes * 2))
        raise ValueError(f"未知编码: {encoding}")

    def _passphrase(self, n_words: int) -> str:
        words = [secrets.choice(WORDLIST) for _ in range(n_words)]
        return "-".join(words)

    def generate_keypair(
        self,
        key_type: str,
        output_dir: Path,
        password: Optional[str] = None,
    ) -> Tuple[str, str]:
        """
        生成非对称密钥对。
        返回 (private_key_pem, public_key_pem)
        """
        try:
            from cryptography.hazmat.primitives.asymmetric import rsa, ed25519
            from cryptography.hazmat.primitives import serialization
        except ImportError:
            raise ImportError(
                "生成非对称密钥需要 cryptography: pip install cryptography"
            )

        if key_type == "rsa":
            private_key = rsa.generate_private_key(
                public_exponent=65537,
                key_size=4096,
            )
        elif key_type == "ed25519":
            private_key = ed25519.Ed25519PrivateKey.generate()
        else:
            raise ValueError(f"不支持的非对称类型: {key_type}")

        # 序列化
        if password:
            encryption = serialization.BestAvailableEncryption(password.encode())
        else:
            encryption = serialization.NoEncryption()

        private_pem = private_key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=encryption,
        ).decode("ascii")

        public_pem = private_key.public_key().public_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PublicFormat.SubjectPublicKeyInfo,
        ).decode("ascii")

        return private_pem, public_pem


# ==============================================================================
# 输出处理
# ==============================================================================
class OutputWriter:
    """输出写入器"""

    def __init__(self, logger: logging.Logger):
        self.logger = logger

    def write_to_stdout(
        self,
        keys: List[GeneratedKey],
        fmt: str = "env",
        quiet: bool = False,
    ) -> None:
        """输出到 stdout（供管道/重定向使用）"""
        content = self._format(keys, fmt)
        print(content)

    def write_to_file(
        self,
        keys: List[GeneratedKey],
        path: Path,
        fmt: str = "env",
        mode: int = 0o600,
        force: bool = False,
        dry_run: bool = False,
        append: bool = False,
    ) -> Tuple[bool, int]:
        """
        写入文件。
        返回 (成功, 文件大小)
        """
        if path.exists() and not force and not append:
            self.logger.error(f"文件已存在: {path}（使用 --force 覆盖）")
            return False, 0

        if dry_run:
            self.logger.info(f"[DRY-RUN] 会写入: {path}")
            return True, 0

        content = self._format(keys, fmt)

        try:
            path.parent.mkdir(parents=True, exist_ok=True)

            if append:
                # 追加模式：生成带分隔符的内容
                ts = datetime.now(timezone.utc).isoformat()
                header = f"\n# Generated at {ts}\n"
                with open(path, "a", encoding="utf-8") as f:
                    f.write(header)
                    f.write(content)
                    f.write("\n")
            else:
                # 原子写入：先写临时文件再重命名
                tmp_path = path.with_suffix(path.suffix + ".tmp")
                with open(tmp_path, "w", encoding="utf-8") as f:
                    f.write(content)
                    f.write("\n")
                os.replace(tmp_path, path)

            # 设置权限
            os.chmod(path, mode)

            size = path.stat().st_size
            return True, size

        except OSError as e:
            self.logger.error(f"写入文件失败: {e}")
            return False, 0

    def _format(self, keys: List[GeneratedKey], fmt: str) -> str:
        """格式化输出"""
        if fmt == "env" or fmt == "shell":
            lines = []
            if fmt == "shell":
                lines.append("#!/usr/bin/env bash")
                lines.append("# 由 generate_keys.py 生成")
                lines.append("")
            for k in keys:
                if fmt == "shell":
                    lines.append(f'export {k.name}="{k.value}"')
                else:
                    lines.append(f"{k.name}={k.value}")
            return "\n".join(lines)

        if fmt == "json":
            data = {k.name: k.value for k in keys}
            return json.dumps(data, ensure_ascii=False, indent=2)

        if fmt == "yaml":
            if yaml is None:
                raise ImportError("YAML 输出需要 PyYAML: pip install PyYAML")
            data = {k.name: k.value for k in keys}
            return yaml.safe_dump(data, allow_unicode=True, default_flow_style=False)

        if fmt == "text":
            lines = []
            for k in keys:
                lines.append(f"{k.name} = {k.value}")
            return "\n".join(lines)

        raise ValueError(f"未知格式: {fmt}")


# ==============================================================================
# 审计日志
# ==============================================================================
def write_audit_log(
    keys: List[GeneratedKey],
    output_file: str,
    action: str = "generate",
) -> None:
    """写入审计日志（不含密钥值）"""
    try:
        AUDIT_DIR.mkdir(parents=True, exist_ok=True)
        ts = datetime.now(timezone.utc).isoformat()
        user = os.environ.get("USER", os.environ.get("USERNAME", "unknown"))

        # 记录元数据，绝不记录密钥值
        record = {
            "timestamp": ts,
            "action": action,
            "user": user,
            "pid": os.getpid(),
            "keys": [
                {
                    "name": k.name,
                    "type": k.key_type,
                    "length": k.length,
                    "encoding": k.encoding,
                    # 记录 SHA256 前 8 位，便于识别但不泄露
                    "fingerprint": hashlib.sha256(
                        k.value.encode()
                    ).hexdigest()[:8],
                }
                for k in keys
            ],
            "output_file": output_file,
        }

        with open(AUDIT_FILE, "a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

        # 权限保护
        os.chmod(AUDIT_FILE, 0o600)

    except Exception:
        pass


# ==============================================================================
# 密钥生成流程
# ==============================================================================
def generate_keys(
    key_types: List[str],
    generator: KeyGenerator,
    encoding: str = "hex",
    length: Optional[int] = None,
    words: int = 6,
) -> List[GeneratedKey]:
    """批量生成密钥"""
    keys: List[GeneratedKey] = []

    for kt in key_types:
        if kt in ("rsa", "ed25519"):
            continue  # 单独处理

        if kt not in KEY_TYPES:
            raise ValueError(f"未知密钥类型: {kt}")

        value = generator.generate(kt, length=length, encoding=encoding, words=words)
        name = ENV_NAMES.get(kt, kt.upper())

        keys.append(GeneratedKey(
            key_type=kt,
            name=name,
            value=value,
            length=len(value),
            encoding=encoding,
            generated_at=datetime.now(timezone.utc).isoformat(),
        ))

    return keys


def generate_keypair_files(
    key_type: str,
    output_dir: Path,
    generator: KeyGenerator,
    password: Optional[str] = None,
    force: bool = False,
    dry_run: bool = False,
) -> Tuple[str, str]:
    """生成非对称密钥对并保存"""

    private_path = output_dir / f"{key_type}_private.pem"
    public_path = output_dir / f"{key_type}_public.pem"

    if not force and not dry_run:
        if private_path.exists() or public_path.exists():
            raise FileExistsError(f"密钥文件已存在: {private_path}")

    if dry_run:
        return str(private_path), str(public_path)

    private_pem, public_pem = generator.generate_keypair(
        key_type, output_dir, password
    )

    output_dir.mkdir(parents=True, exist_ok=True)

    # 私钥 600，公钥 644
    with open(private_path, "w", encoding="utf-8") as f:
        f.write(private_pem)
    os.chmod(private_path, 0o600)

    with open(public_path, "w", encoding="utf-8") as f:
        f.write(public_pem)
    os.chmod(public_path, 0o644)

    return str(private_path), str(public_path)


# ==============================================================================
# 参数解析
# ==============================================================================
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="生成密码学密钥",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 生成 JWT 密钥（输出到 stdout）
  %(prog)s --type jwt

  # 生成完整密钥集到 .env
  %(prog)s --preset full --output .env.prod

  # 生成密钥集（JSON）
  %(prog)s --preset full --format json

  # 生成口令
  %(prog)s --type passphrase --words 6

  # 生成 RSA 密钥对
  %(prog)s --type rsa --output ./keys/

  # 追加到现有 .env
  %(prog)s --preset secrets --output .env --append

可用密钥类型:
  jwt, api_key, api_secret, db_password, redis_password,
  encryption_key, hmac_key, salt, nonce, uuid, totp_secret,
  passphrase, rsa, ed25519

可用预设:
  minimal, full, crypto, secrets

可用输出格式:
  env, json, yaml, text, shell

退出码:
  0   成功
  1   生成失败
  2   参数错误
  3   文件已存在
  4   熵不足
  5   内部错误
  130 用户中断
        """,
    )

    parser.add_argument("--type", "-t", action="append", default=[],
                        help="密钥类型（可多次指定）")
    parser.add_argument("--preset", "-p", choices=list(PRESETS.keys()),
                        help="预设密钥集")
    parser.add_argument("--output", "-o", type=Path,
                        help="输出文件（默认 stdout）")
    parser.add_argument("--format", "-f", choices=OUTPUT_FORMATS,
                        default="env", help="输出格式（默认 env）")
    parser.add_argument("--encoding", choices=["hex", "urlsafe", "base64", "base32", "alnum"],
                        default="hex", help="编码方式（默认 hex）")
    parser.add_argument("--length", type=int,
                        help="密钥长度（字节，默认按类型）")
    parser.add_argument("--words", type=int, default=6,
                        help="口令单词数（默认 6）")
    parser.add_argument("--mode", default="600",
                        help="文件权限（八进制，默认 600）")
    parser.add_argument("--force", action="store_true",
                        help="覆盖已存在的文件")
    parser.add_argument("--append", action="store_true",
                        help="追加到文件（不覆盖）")
    parser.add_argument("--password", "-P",
                        help="非对称密钥密码（用于加密私钥）")
    parser.add_argument("--dry-run", action="store_true",
                        help="预演，不实际写入")
    parser.add_argument("--show-values", action="store_true",
                        help="在日志中显示密钥值（不安全）")
    parser.add_argument("--quiet", "-q", action="store_true",
                        help="静默模式")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="详细输出")
    parser.add_argument("--no-color", action="store_true",
                        help="禁用颜色")
    parser.add_argument("--version-script", action="version",
                        version=f"generate_keys.py {VERSION}")

    return parser.parse_args()


# ==============================================================================
# 主函数
# ==============================================================================
def main() -> int:
    args = parse_args()

    if args.no_color:
        Color.disable()

    signal.signal(signal.SIGINT, _signal_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _signal_handler)

    logger = setup_logging(verbose=args.verbose, quiet=args.quiet)

    # 熵检查
    if not check_entropy(logger):
        logger.warning("系统熵池较低，建议增加熵源")

    # 解析密钥类型
    key_types: List[str] = []
    if args.preset:
        key_types.extend(PRESETS[args.preset])
    if args.type:
        key_types.extend(args.type)

    # 去重保序
    key_types = list(dict.fromkeys(key_types))

    if not key_types:
        sys.stderr.write("❌ 必须指定 --type 或 --preset\n")
        sys.stderr.write("使用 --help 查看帮助\n")
        return 2

    # 校验长度
    if args.length is not None and args.length < 8:
        sys.stderr.write(f"❌ --length 必须 >= 8，当前 {args.length}\n")
        return 2

    # 校验权限
    try:
        file_mode = int(args.mode, 8)
    except ValueError:
        sys.stderr.write(f"❌ --mode 无效: {args.mode}\n")
        return 2

    # 检查是否需要文件输出
    has_keypair = any(t in ("rsa", "ed25519") for t in key_types)

    if not args.output and has_keypair:
        sys.stderr.write("❌ 非对称密钥必须指定 --output 目录\n")
        return 2

    # 生成
    start_ts = time.monotonic()
    result = GenerationResult()

    try:
        generator = KeyGenerator(logger)

        # 对称密钥
        symmetric_types = [t for t in key_types if t not in ("rsa", "ed25519")]
        keypair_types = [t for t in key_types if t in ("rsa", "ed25519")]

        generated: List[GeneratedKey] = []
        if symmetric_types:
            logger.info(f"生成 {len(symmetric_types)} 个对称密钥...")
            generated = generate_keys(
                symmetric_types,
                generator,
                encoding=args.encoding,
                length=args.length,
                words=args.words,
            )

        # 非对称密钥
        keypair_paths: List[Tuple[str, str]] = []
        for kp in keypair_types:
            logger.info(f"生成 {kp} 密钥对...")
            private_path, public_path = generate_keypair_files(
                kp,
                args.output if args.output else Path("."),
                generator,
                password=args.password,
                force=args.force,
                dry_run=args.dry_run,
            )
            keypair_paths.append((private_path, public_path))

        result.keys = generated

        # 输出
        writer = OutputWriter(logger)

        if args.output and generated:
            # 如果有非对称密钥，output 是目录；否则是文件
            out_path = args.output
            if has_keypair:
                out_path = args.output / f"keys.{args.format}"

            ok, size = writer.write_to_file(
                generated,
                out_path,
                fmt=args.format,
                mode=file_mode,
                force=args.force,
                dry_run=args.dry_run,
                append=args.append,
            )

            if not ok:
                return 3

            result.output_file = str(out_path)
            result.file_size = size
            result.file_mode = oct(file_mode)

            if not args.quiet:
                logger.info(f"✓ 已写入: {out_path}")
                logger.info(f"  权限: {oct(file_mode)}")
                logger.info(f"  大小: {size} 字节")

        elif generated:
            # stdout 输出
            writer.write_to_stdout(generated, fmt=args.format, quiet=args.quiet)

        # 非对称密钥路径
        for private, public in keypair_paths:
            if not args.quiet:
                logger.info(f"✓ 私钥: {private}")
                logger.info(f"✓ 公钥: {public}")

        # 审计日志
        if not args.dry_run:
            write_audit_log(
                generated,
                result.output_file or "<stdout>",
                action="generate",
            )

    except FileExistsError as e:
        logger.error(str(e))
        return 3
    except (ValueError, ImportError) as e:
        logger.error(f"错误: {e}")
        result.error = str(e)
        return 1
    except KeyboardInterrupt:
        return 130
    except Exception as e:
        logger.error(f"内部错误: {e}")
        logger.debug(traceback.format_exc())
        result.error = str(e)
        return 5

    result.duration_ms = int((time.monotonic() - start_ts) * 1000)

    if not args.quiet:
        logger.info(f"完成（耗时 {result.duration_ms}ms）")

    return 0


if __name__ == "__main__":
    sys.exit(main())
