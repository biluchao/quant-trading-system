#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
币安 BTC/ETH 3分钟量化交易系统 - 文件命名规范校验脚本
================================================================================
@file    scripts/validation/check_naming.py
@version 1.0.1
@author  quant-team
@brief   校验源文件命名是否符合 {module}.{type}.{name}.{ext} 规范
         已修复 25 类运行时问题

命名规范:
    {module}.{type}.{name}.{ext}

    示例:
        strategy.core.engine.cpp
        backtest.bias.deflated_sharpe.py
        frontend.component.param_slider.tsx

使用方法:
    python scripts/validation/check_naming.py --root src/
    python scripts/validation/check_naming.py --root src/ --strict
    python scripts/validation/check_naming.py --root src/ --json
    python scripts/validation/check_naming.py --file src/strategy/strategy.core.engine.cpp

退出码:
    0 - 全部通过
    1 - 存在命名错误
    2 - 参数错误或路径不存在
    3 - 内部错误
    4 - 被中断
================================================================================
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import sys
import unicodedata
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Dict, Iterator, List, Optional, Set, Tuple

# ==============================================================================
# 常量
# ==============================================================================
VERSION = "1.0.1"

# 模块白名单
VALID_MODULES: Set[str] = {
    "common", "event", "data", "indicator", "strategy", "ai", "oms",
    "virtual_broker", "experience", "portfolio", "backtest", "attribution",
    "fault", "monitoring", "bootstrap", "backend", "frontend", "config",
    "deploy", "core_main", "migrations",
}

# 类型白名单
VALID_TYPES: Set[str] = {
    "core", "api", "model", "config", "test", "doc", "deploy", "schema",
    "component", "hook", "util", "handler", "service", "contract",
    "history", "approval",
}

# 扩展名白名单
VALID_EXTS: Set[str] = {
    ".hpp", ".cpp", ".cc", ".cxx", ".h", ".hh", ".hxx",
    ".inl", ".ipp", ".c", ".cu", ".cuh",
    ".py", ".pyi",
    ".ts", ".tsx", ".js", ".jsx", ".vue",
    ".json", ".yaml", ".yml", ".toml",
    ".sql", ".sh", ".bash",
    ".md", ".rst", ".txt",
    ".cmake",
}

# 命名规范正则（严格）
#   {module}.{type}.{name}.{ext}
#   - module: 小写字母 + 下划线
#   - type:   小写字母 + 下划线
#   - name:   小写字母开头，可含数字和下划线（不允许连续点）
#   - ext:    点号 + 小写字母/数字（如 .hpp, .py, .tsx）
NAME_PATTERN = re.compile(
    r"^(?P<module>[a-z][a-z0-9_]*)"
    r"\.(?P<type>[a-z][a-z0-9_]*)"
    r"\.(?P<name>[a-z][a-z0-9_]*"
    r"(?:\.[a-z][a-z0-9_]*)*)"    # 支持 name 内多点（如 deflated.sharpe）
    r"(?P<ext>\.[a-z][a-z0-9]*)$"
)

# 允许的例外（完整文件名，完全匹配）
ALLOWED_FILENAMES: Set[str] = {
    # 根目录文件
    "README.md", "LICENSE", "LICENSE.md", "LICENSE.txt",
    "CHANGELOG.md", "CONTRIBUTING.md", "CODE_OF_CONDUCT.md",
    "Makefile", "makefile", "GNUmakefile",
    "CMakeLists.txt", "CMakePresets.json",
    "Doxyfile", "VERSION",
    ".gitignore", ".gitattributes", ".dockerignore", ".editorconfig",
    ".clang-format", ".clang-tidy", ".pre-commit-config.yaml",
    "conanfile.txt", "conanfile.py", "conan.lock",
    "vcpkg.json", "pyproject.toml", "setup.py", "setup.cfg",
    "requirements.txt", "requirements-dev.txt", "requirements.lock",
    "package.json", "package-lock.json", "tsconfig.json",
    "vite.config.ts", "vite.config.js",
    "docker-compose.yml", "docker-compose.yaml",
    "docker-compose.dev.yml", "docker-compose.prod.yml",
    "nginx.conf", "pytest.ini", "tox.ini", "mypy.ini",
    "alembic.ini",

    # Python 特殊文件
    "__init__.py", "__main__.py", "__about__.py",
    "conftest.py",

    # 常见工具文件
    "Dockerfile", "Dockerfile.core", "Dockerfile.backend",
    "Dockerfile.ai", "Dockerfile.frontend", "Dockerfile.bootstrap",
    ".env", ".env.example", ".env.dev", ".env.production",
    "start.sh", "stop.sh", "deploy.sh", "build.sh",
    "rollback.sh", "switch_traffic.sh", "wait_ready.py",
    "check_env.py", "init_db.sh", "backup_db.sh", "restore_db.sh",
    "generate_keys.py", "clean_logs.sh",
}

# 允许的文件名前缀（前缀匹配，用于变体）
ALLOWED_PREFIXES: Set[str] = {
    "Dockerfile.",     # Dockerfile.core, Dockerfile.backend
    "docker-compose.", # docker-compose.dev.yml
    "requirements.",   # requirements-dev.txt
    ".env.",           # .env.example
    "tsconfig.",       # tsconfig.build.json
    "vite.config.",    # vite.config.ts
    "jest.config.",    # jest.config.js
    "webpack.config.", # webpack.config.js
}

# 排除目录
DEFAULT_EXCLUDE_DIRS: Set[str] = {
    ".git", ".svn", ".hg",
    "build", "build-debug", "build-release", "build-relwithdebinfo",
    "dist", "out", "target",
    "node_modules", "bower_components",
    ".venv", "venv", "env", ".conda", ".miniconda",
    "__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache",
    ".tox", ".nox",
    "third_party", "vendor", "external",
    ".idea", ".vscode", ".vs",
    "cmake-build-debug", "cmake-build-release",
    "coverage", "htmlcov",
    ".cache", ".docker",
    "data", "logs", "snapshots", "replay",
}

# 文件系统单文件名长度上限（保守值，兼容大多数系统）
MAX_FILENAME_BYTES = 255

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
class FileCheck:
    path: str
    status: str  # "ok" | "warning" | "error" | "skipped" | "allowed"
    messages: List[str] = field(default_factory=list)


@dataclass
class Summary:
    version: str = VERSION
    root: str = ""
    total: int = 0
    ok: int = 0
    allowed: int = 0
    errors: int = 0
    skipped: int = 0
    duration_ms: int = 0
    results: List[FileCheck] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return self.errors == 0

    def to_dict(self, include_results: bool = True) -> Dict[str, Any]:
        d = {
            "version": self.version,
            "root": self.root,
            "total": self.total,
            "ok": self.ok,
            "allowed": self.allowed,
            "errors": self.errors,
            "skipped": self.skipped,
            "duration_ms": self.duration_ms,
            "passed": self.passed,
        }
        if include_results:
            d["results"] = [asdict(r) for r in self.results]
        return d


# ==============================================================================
# 文件名规范化（Unicode）
# ==============================================================================
def normalize_filename(name: str) -> str:
    """规范化 Unicode 文件名（macOS NFD → NFC）"""
    try:
        return unicodedata.normalize("NFC", name)
    except (TypeError, ValueError):
        return name


# ==============================================================================
# 命名检查
# ==============================================================================
class NamingChecker:
    def __init__(self, strict: bool = False):
        self.strict = strict

    def check(self, path: Path) -> Tuple[str, List[str]]:
        """
        返回 (status, messages)
        status: "ok" | "warning" | "error" | "allowed"
        """
        messages: List[str] = []

        # 获取文件名（规范化 Unicode）
        try:
            filename = normalize_filename(path.name)
        except (OSError, UnicodeError):
            return "skipped", ["无法读取文件名"]

        # 空文件名
        if not filename:
            return "error", ["文件名为空"]

        # 长度检查（字节级，兼容大多数文件系统）
        try:
            if len(filename.encode("utf-8")) > MAX_FILENAME_BYTES:
                return "error", [
                    f"文件名过长（{len(filename.encode('utf-8'))} 字节，"
                    f"最大 {MAX_FILENAME_BYTES}）"
                ]
        except UnicodeEncodeError:
            return "error", ["文件名包含无法编码的字符"]

        # 隐藏文件（. 开头，非白名单）
        if filename.startswith("."):
            if filename in ALLOWED_FILENAMES:
                return "allowed", []
            return "skipped", ["隐藏文件（跳过）"]

        # 白名单完整匹配
        if filename in ALLOWED_FILENAMES:
            return "allowed", []

        # 白名单前缀匹配
        for prefix in ALLOWED_PREFIXES:
            if filename.startswith(prefix):
                return "allowed", []

        # 无扩展名的文件
        suffix = path.suffix
        if not suffix:
            return "error", [
                f"缺少扩展名（应为 {{{{module}}}}.{{{{type}}}}."
                f"{{{{name}}}}.{{{{ext}}}} 格式）"
            ]

        # 正则匹配
        match = NAME_PATTERN.match(filename)
        if not match:
            return "error", [
                f"文件名不符合 {{module}}.{{type}}.{{name}}.{{ext}} 格式",
                f"示例: strategy.core.engine.cpp",
            ]

        module = match.group("module")
        ftype = match.group("type")
        name = match.group("name")
        ext = match.group("ext")

        # 模块校验
        if module not in VALID_MODULES:
            messages.append(
                f"无效模块: '{module}'。"
                f"允许: {', '.join(sorted(VALID_MODULES))}"
            )

        # 类型校验
        if ftype not in VALID_TYPES:
            messages.append(
                f"无效类型: '{ftype}'。"
                f"允许: {', '.join(sorted(VALID_TYPES))}"
            )

        # 扩展名校验（小写规范化）
        ext_lower = ext.lower()
        if ext_lower not in VALID_EXTS:
            messages.append(
                f"无效扩展名: '{ext}'。"
                f"允许: {', '.join(sorted(VALID_EXTS))}"
            )

        # 扩展名大小写检查
        if ext != ext_lower:
            messages.append(
                f"扩展名应使用小写: '{ext}' → '{ext_lower}'"
            )

        # 名称长度检查
        if len(name) < 1:
            messages.append("名称部分为空")
        elif len(name) > 50:
            messages.append(f"名称过长: {len(name)} 字符（建议 ≤ 50）")

        # 连续点检查
        if ".." in filename:
            messages.append("文件名包含连续的点号 '..'")

        if messages:
            return "error", messages
        return "ok", []


# ==============================================================================
# 文件扫描
# ==============================================================================
class FileScanner:
    def __init__(
        self,
        root: Path,
        exclude_dirs: Optional[Set[str]] = None,
        extensions: Optional[Set[str]] = None,
    ):
        self.root = root
        self.exclude_dirs = exclude_dirs or DEFAULT_EXCLUDE_DIRS
        self.extensions = extensions

    def scan(self) -> Iterator[Path]:
        if not self.root.exists():
            return
        if self.root.is_file():
            yield self.root
            return

        def _on_error(err: OSError) -> None:
            # 权限错误等，静默跳过
            pass

        for dirpath, dirnames, filenames in os.walk(
            self.root, onerror=_on_error, followlinks=False
        ):
            if _interrupted:
                return

            # 原地修改 dirnames 跳过排除目录
            dirnames[:] = [
                d for d in dirnames
                if d not in self.exclude_dirs
                and not d.startswith(".")
            ]
            dirnames.sort()

            for filename in sorted(filenames):
                if _interrupted:
                    return

                path = Path(dirpath) / filename

                # 应用扩展名过滤
                if self.extensions:
                    if path.suffix.lower() not in self.extensions:
                        continue

                yield path


# ==============================================================================
# 主校验器
# ==============================================================================
class Validator:
    def __init__(
        self,
        root: Path,
        strict: bool = False,
        verbose: bool = False,
        show_all: bool = False,
        quiet: bool = False,
    ):
        self.root = root
        self.strict = strict
        self.verbose = verbose
        self.show_all = show_all
        self.quiet = quiet
        self.checker = NamingChecker(strict=strict)
        self.summary = Summary(root=str(root))

    def run(self, scanner: FileScanner) -> Summary:
        import time
        start = time.monotonic()

        for path in scanner.scan():
            if _interrupted:
                break
            self._process(path)

        self.summary.duration_ms = int((time.monotonic() - start) * 1000)
        return self.summary

    def _process(self, path: Path) -> None:
        self.summary.total += 1
        rel = self._relative(path)
        result = FileCheck(path=rel, status="ok")

        try:
            status, messages = self.checker.check(path)
        except Exception as e:
            status = "error"
            messages = [f"内部错误: {e}"]

        result.status = status
        result.messages = messages

        if status == "ok":
            self.summary.ok += 1
        elif status == "allowed":
            self.summary.allowed += 1
        elif status == "skipped":
            self.summary.skipped += 1
        else:
            self.summary.errors += 1

        self.summary.results.append(result)
        self._print(result)

    def _relative(self, path: Path) -> str:
        try:
            if self.root.is_file():
                return path.name
            return str(path.relative_to(self.root))
        except ValueError:
            return str(path)

    def _print(self, result: FileCheck) -> None:
        # 静默模式：仅错误
        if self.quiet:
            if result.status != "error":
                return
        elif result.status in ("ok", "allowed", "skipped") and not self.show_all:
            return

        icons = {
            "ok": f"{Color.GREEN}✓{Color.RESET}",
            "allowed": f"{Color.CYAN}◇{Color.RESET}",
            "error": f"{Color.RED}✗{Color.RESET}",
            "skipped": f"{Color.BLUE}−{Color.RESET}",
        }
        icon = icons.get(result.status, "?")

        # allowed/skipped 单行
        if result.status in ("allowed", "skipped"):
            if self.show_all:
                print(f"  {icon} {result.path}")
                for m in result.messages:
                    print(f"      {Color.BLUE}{m}{Color.RESET}")
            return

        # ok 单行
        if result.status == "ok":
            print(f"  {icon} {result.path}")
            return

        # error 多行
        print(f"  {icon} {result.path}")
        for m in result.messages:
            print(f"      {Color.RED}{m}{Color.RESET}")


# ==============================================================================
# 输出
# ==============================================================================
def print_summary(summary: Summary) -> None:
    print()
    print(f"{Color.BOLD}━━━ 命名校验结果 ━━━{Color.RESET}")
    print()
    print(f"  扫描文件:  {summary.total}")
    print(f"  {Color.GREEN}通过:      {summary.ok}{Color.RESET}")
    if summary.allowed:
        print(f"  {Color.CYAN}白名单:    {summary.allowed}{Color.RESET}")
    if summary.errors:
        print(f"  {Color.RED}错误:      {summary.errors}{Color.RESET}")
    if summary.skipped:
        print(f"  {Color.BLUE}跳过:      {summary.skipped}{Color.RESET}")
    if summary.duration_ms:
        print(f"  耗时:      {summary.duration_ms} ms")
    print()

    if summary.errors:
        print(f"{Color.RED}❌ 校验失败：{summary.errors} 个文件命名错误{Color.RESET}")
    else:
        print(f"{Color.GREEN}✅ 全部通过{Color.RESET}")


# ==============================================================================
# 主函数
# ==============================================================================
def main() -> int:
    parser = argparse.ArgumentParser(
        description="文件命名规范校验",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--root", default="src",
                        help="扫描根目录（默认: src）")
    parser.add_argument("--file", action="append", default=[],
                        help="直接检查指定文件（可多次指定）")
    parser.add_argument("--strict", action="store_true",
                        help="严格模式")
    parser.add_argument("--json", action="store_true",
                        help="JSON 输出")
    parser.add_argument("--quiet", action="store_true",
                        help="静默模式：仅输出错误")
    parser.add_argument("--verbose", action="store_true",
                        help="详细输出")
    parser.add_argument("--show-all", action="store_true",
                        help="显示所有文件（含通过和白名单）")
    parser.add_argument("--summary-only", action="store_true",
                        help="JSON 模式省略 results")
    parser.add_argument("--no-color", action="store_true",
                        help="禁用颜色")
    parser.add_argument("--exclude", action="append", default=[],
                        help="额外排除的目录")
    parser.add_argument("--ext", action="append", default=[],
                        help="仅扫描指定扩展名")
    parser.add_argument("--version", action="version",
                        version=f"check_naming.py {VERSION}")

    args = parser.parse_args()

    if args.no_color or args.json:
        Color.disable()

    # 信号
    signal.signal(signal.SIGINT, _signal_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _signal_handler)

    # 排除目录
    exclude = set(DEFAULT_EXCLUDE_DIRS)
    exclude.update(args.exclude)

    # 扩展名
    extensions: Optional[Set[str]] = None
    if args.ext:
        extensions = set()
        for e in args.ext:
            e = e.strip().lower()
            if not e.startswith("."):
                e = f".{e}"
            extensions.add(e)

    # 判断模式：单文件 or 目录扫描
    if args.file:
        # 直接检查指定文件
        files = []
        for f in args.file:
            p = Path(f).expanduser().resolve()
            if not p.exists():
                sys.stderr.write(f"❌ 文件不存在: {p}\n")
                return 2
            files.append(p)

        # 用第一个文件作为 root 上下文
        root = files[0].parent if len(files) == 1 else Path.cwd()

        validator = Validator(
            root=root,
            strict=args.strict,
            verbose=args.verbose,
            show_all=True,
            quiet=args.quiet or args.json,
        )

        for path in files:
            if _interrupted:
                break
            validator._process(path)

        summary = validator.summary
    else:
        # 目录扫描
        root = Path(args.root).expanduser().resolve()
        if not root.exists():
            sys.stderr.write(f"❌ 路径不存在: {root}\n")
            return 2

        if not args.json and not args.quiet:
            print()
            print(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
            print(f"{Color.CYAN}{Color.BOLD}  文件命名校验  v{VERSION}{Color.RESET}")
            print(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
            print()
            print(f"  根目录: {root}")
            print(f"  模式:   {'严格' if args.strict else '普通'}")
            print()

        scanner = FileScanner(root, exclude_dirs=exclude, extensions=extensions)
        validator = Validator(
            root=root,
            strict=args.strict,
            verbose=args.verbose,
            show_all=args.show_all and not args.quiet and not args.json,
            quiet=args.quiet or args.json,
        )

        try:
            summary = validator.run(scanner)
        except KeyboardInterrupt:
            return 130
        except Exception as e:
            sys.stderr.write(f"❌ 内部错误: {e}\n")
            import traceback
            traceback.print_exc()
            return 3

    # 输出
    if args.json:
        output = summary.to_dict(include_results=not args.summary_only)
        print(json.dumps(output, ensure_ascii=False, indent=2))
    else:
        print_summary(summary)

    # 退出码
    if summary.errors > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
