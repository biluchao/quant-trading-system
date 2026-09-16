#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
币安 BTC/ETH 3分钟量化交易系统 - 文件元数据校验脚本
================================================================================
@file    scripts/validation/validate_metadata.py
@version 1.0.1
@author  quant-team
@brief   校验所有源文件的头部元数据块是否符合项目规范
         第二轮修复：20 类运行时问题

元数据块格式（按文件类型使用不同注释风格）:
    C++ (.hpp/.cpp):
        /**
         * ---
         * module: strategy
         * type: core
         * name: engine
         * version: "1.0.0"
         * assembly_order: 40
         * ---
         */

    Python (.py):
        \"\"\"
        ---
        module: backtest
        type: bias
        name: deflated_sharpe
        version: "1.0.0"
        assembly_order: 55
        ---
        \"\"\"

    TypeScript/TSX:
        /**
         * ---
         * module: frontend
         * type: component
         * name: param_slider
         * version: "1.0.0"
         * assembly_order: 75
         * ---
         */

必需字段:
    - module          所属模块
    - type            文件类型
    - name            文件名称
    - version         语义化版本号（YAML 中建议加引号）
    - assembly_order  组装顺序（整数）

使用方法:
    python scripts/validation/validate_metadata.py --root src/
    python scripts/validation/validate_metadata.py --root src/ --strict
    python scripts/validation/validate_metadata.py --root src/ --json
    python scripts/validation/validate_metadata.py --root src/ --summary-only

退出码:
    0 - 全部通过
    1 - 存在校验失败
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
import stat as stat_module
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Dict, Iterator, List, Optional, Tuple

# ==============================================================================
# 依赖检查
# ==============================================================================
try:
    import yaml
except ImportError:
    sys.stderr.write("❌ 缺少 PyYAML 依赖\n")
    sys.stderr.write("   安装: pip install PyYAML\n")
    sys.exit(3)


# ==============================================================================
# 常量
# ==============================================================================
VERSION = "1.0.1"

# 元数据块仅在前 N 行内搜索（避免误匹配代码中间的注释）
MAX_HEADER_LINES = 100

# 文件大小上限
MAX_FILE_SIZE = 1 * 1024 * 1024  # 1 MB

# 允许的模块
VALID_MODULES = {
    "common", "event", "data", "indicator", "strategy", "ai", "oms",
    "virtual_broker", "experience", "portfolio", "backtest", "attribution",
    "fault", "monitoring", "bootstrap", "backend", "frontend", "config",
    "deploy", "core_main", "migrations",
}

# 允许的文件类型
VALID_TYPES = {
    "core", "api", "model", "config", "test", "doc", "deploy", "schema",
    "component", "hook", "util", "handler", "service", "contract",
    "history", "approval",
}

# 扩展名 → 注释风格
COMMENT_STYLES: Dict[str, str] = {
    ".cpp": "c_block", ".hpp": "c_block", ".cc": "c_block",
    ".cxx": "c_block", ".h": "c_block", ".hh": "c_block",
    ".hxx": "c_block", ".inl": "c_block", ".ipp": "c_block",
    ".c": "c_block", ".cu": "c_block", ".cuh": "c_block",
    ".py": "python_docstring", ".pyi": "python_docstring",
    ".ts": "c_block", ".tsx": "c_block",
    ".js": "c_block", ".jsx": "c_block",
    ".vue": "c_block",
    ".sh": "shell_hash", ".bash": "shell_hash",
}

# 排除目录
DEFAULT_EXCLUDE_DIRS = {
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

# 语义化版本正则
SEMVER_PATTERN = re.compile(
    r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"
)

# 名称正则
NAME_PATTERN = re.compile(r"^[a-z][a-z0-9_]*$")

# YAML 分隔符
YAML_FENCE = re.compile(r"^\s*-{3,}\s*$")


# ==============================================================================
# 中断标志
# ==============================================================================
_interrupted = False


def _signal_handler(signum: int, frame: Any) -> None:
    global _interrupted
    if _interrupted:
        # 第二次信号，强制退出
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
class FileResult:
    path: str
    status: str
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    metadata: Optional[Dict[str, Any]] = None

    def to_dict(self, include_metadata: bool = False) -> Dict[str, Any]:
        d = {
            "path": self.path,
            "status": self.status,
            "errors": self.errors,
            "warnings": self.warnings,
        }
        if include_metadata and self.metadata is not None:
            try:
                json.dumps(self.metadata)
                d["metadata"] = self.metadata
            except (TypeError, ValueError):
                d["metadata"] = "<无法序列化>"
        return d


@dataclass
class Summary:
    version: str = VERSION
    root: str = ""
    total: int = 0
    ok: int = 0
    warnings: int = 0
    errors: int = 0
    skipped: int = 0
    duration_ms: int = 0
    results: List[FileResult] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return self.errors == 0

    def to_dict(self, include_results: bool = True,
                include_metadata: bool = False) -> Dict[str, Any]:
        d = {
            "version": self.version,
            "root": self.root,
            "total": self.total,
            "ok": self.ok,
            "warnings": self.warnings,
            "errors": self.errors,
            "skipped": self.skipped,
            "duration_ms": self.duration_ms,
            "passed": self.passed,
        }
        if include_results:
            d["results"] = [r.to_dict(include_metadata) for r in self.results]
        return d


# ==============================================================================
# 文件读取
# ==============================================================================
class SafeReader:
    """安全读取文件（BOM、CRLF、编码、大小、符号链接）"""

    def __init__(self, verbose: bool = False):
        self.verbose = verbose

    def read(self, path: Path) -> Optional[str]:
        # 符号链接检查
        try:
            st = path.lstat()
        except OSError:
            return None

        if stat_module.S_ISLNK(st.st_mode):
            try:
                target = path.resolve(strict=True)
            except (OSError, RuntimeError):
                return None
            if not target.is_file():
                return None

        # 大小检查
        try:
            size = path.stat().st_size
        except OSError:
            return None

        if size == 0:
            return ""
        if size > MAX_FILE_SIZE:
            if self.verbose:
                sys.stderr.write(f"跳过超大文件: {path} ({size} bytes)\n")
            return None

        # 原子读取
        try:
            with open(path, "rb") as f:
                raw = f.read(MAX_FILE_SIZE + 1)
        except (OSError, PermissionError):
            return None

        if len(raw) > MAX_FILE_SIZE:
            return None

        # BOM 处理
        if raw.startswith(b"\xef\xbb\xbf"):
            raw = raw[3:]
        elif raw.startswith(b"\xff\xfe"):
            try:
                return raw[2:].decode("utf-16-le")
            except UnicodeDecodeError:
                return None
        elif raw.startswith(b"\xfe\xff"):
            try:
                return raw[2:].decode("utf-16-be")
            except UnicodeDecodeError:
                return None

        # UTF-16 无 BOM 检测（大量 null 字节交替）
        if len(raw) >= 4 and raw[1:2] == b"\x00" and raw[3:4] == b"\x00":
            if self.verbose:
                sys.stderr.write(f"疑似 UTF-16 无 BOM: {path}\n")
            return None

        # 通用二进制检测
        if b"\x00" in raw[:8192]:
            return None

        # 解码
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            try:
                text = raw.decode("latin-1")
            except UnicodeDecodeError:
                return None

        # CRLF → LF
        if "\r\n" in text:
            text = text.replace("\r\n", "\n")

        return text


# ==============================================================================
# 元数据提取器
# ==============================================================================
class MetadataExtractor:
    """从源文件头部提取元数据块"""

    def extract(self, text: str, style: str) -> Optional[str]:
        # 仅处理前 N 行
        lines = text.split("\n", MAX_HEADER_LINES + 1)
        header = "\n".join(lines[:MAX_HEADER_LINES])

        if style == "c_block":
            return self._extract_c_block(header)
        if style == "python_docstring":
            return self._extract_python_docstring(text)
        if style == "shell_hash":
            return self._extract_shell_hash(header)
        return None

    def _extract_c_block(self, header: str) -> Optional[str]:
        """查找含 --- 围栏的块注释"""
        # 查找所有块注释
        pattern = re.compile(r"/\*\*(.*?)\*/", re.DOTALL)
        for match in pattern.finditer(header):
            content = self._clean_block(match.group(1))
            yaml_text = self._extract_yaml_fence(content)
            if yaml_text:
                return yaml_text
        return None

    def _extract_python_docstring(self, text: str) -> Optional[str]:
        """提取模块级 docstring 中的 YAML 块"""
        lines = text.split("\n")
        i = 0
        # 跳过 shebang、coding 声明和空行
        while i < len(lines):
            stripped = lines[i].strip()
            if not stripped:
                i += 1
                continue
            if stripped.startswith("#!"):
                i += 1
                continue
            # PEP 263: coding 声明必须在前两行
            if i < 2 and re.match(r"^#.*coding[:=]\s*[-\w.]+", stripped):
                i += 1
                continue
            # 跳过其他注释行（如 pylint）
            if stripped.startswith("#"):
                i += 1
                continue
            break

        if i >= len(lines):
            return None

        # 尝试匹配三引号 docstring
        remaining = "\n".join(lines[i:i + MAX_HEADER_LINES])
        match = re.match(r'\s*("""|\'\'\')(.*?)\1', remaining, re.DOTALL)
        if not match:
            return None

        content = match.group(2)
        return self._extract_yaml_fence(content)

    def _extract_shell_hash(self, header: str) -> Optional[str]:
        """提取 shell 注释中的 YAML 块"""
        lines = header.split("\n")
        start = 0
        if lines and lines[0].startswith("#!"):
            start = 1

        # 收集连续注释行
        comment_lines: List[str] = []
        for line in lines[start:]:
            stripped = line.strip()
            if stripped.startswith("#"):
                comment_lines.append(line)
            elif stripped == "":
                if comment_lines:
                    comment_lines.append(line)
            else:
                break

        if not comment_lines:
            return None

        # 去除 # 前缀
        result = []
        for line in comment_lines:
            idx = line.find("#")
            if idx >= 0:
                line = line[idx + 1:]
            if line.startswith(" "):
                line = line[1:]
            result.append(line)

        # 去除尾部空行
        while result and not result[-1].strip():
            result.pop()

        content = "\n".join(result)
        return self._extract_yaml_fence(content)

    def _clean_block(self, content: str) -> str:
        """清理 C 块注释的 * 前缀"""
        lines = content.split("\n")
        cleaned = []
        for line in lines:
            stripped = line.lstrip()
            if stripped.startswith("*"):
                stripped = stripped[1:]
                if stripped.startswith(" "):
                    stripped = stripped[1:]
            cleaned.append(stripped)
        return "\n".join(cleaned)

    def _extract_yaml_fence(self, content: str) -> Optional[str]:
        """从注释内容中提取 --- ... --- 之间的 YAML"""
        lines = content.split("\n")

        # 找到第一个 --- 行
        start_idx = -1
        for i, line in enumerate(lines):
            if YAML_FENCE.match(line):
                start_idx = i
                break

        if start_idx < 0:
            return None

        # 找到第二个 --- 行
        end_idx = -1
        for i in range(start_idx + 1, len(lines)):
            if YAML_FENCE.match(lines[i]):
                end_idx = i
                break

        if end_idx < 0:
            return None

        # 提取中间内容
        yaml_lines = lines[start_idx + 1:end_idx]
        return "\n".join(yaml_lines)


# ==============================================================================
# 元数据校验
# ==============================================================================
class MetadataValidator:
    def __init__(self, strict: bool = False):
        self.strict = strict

    def validate(self, meta: Any) -> Tuple[List[str], List[str]]:
        errors: List[str] = []
        warnings: List[str] = []

        if meta is None:
            errors.append("元数据为空")
            return errors, warnings

        if not isinstance(meta, dict):
            errors.append(f"元数据不是字典，实际: {type(meta).__name__}")
            return errors, warnings

        if not meta:
            errors.append("元数据为空字典")
            return errors, warnings

        # 必需字段
        for name in ("module", "type", "name", "version", "assembly_order"):
            if name not in meta:
                errors.append(f"缺少必需字段: {name}")

        # module
        module = meta.get("module")
        if module is not None:
            if not isinstance(module, str):
                errors.append(f"module 必须是字符串，实际: {type(module).__name__}")
            elif module not in VALID_MODULES:
                errors.append(
                    f"module 值非法: '{module}'。"
                    f"允许: {sorted(VALID_MODULES)}"
                )

        # type
        ftype = meta.get("type")
        if ftype is not None:
            if not isinstance(ftype, str):
                errors.append(f"type 必须是字符串")
            elif ftype not in VALID_TYPES:
                errors.append(
                    f"type 值非法: '{ftype}'。"
                    f"允许: {sorted(VALID_TYPES)}"
                )

        # name
        name = meta.get("name")
        if name is not None:
            if not isinstance(name, str):
                errors.append(f"name 必须是字符串")
            elif not NAME_PATTERN.match(name):
                errors.append(
                    f"name 格式错误: '{name}'。应匹配 [a-z][a-z0-9_]*"
                )

        # version
        version = meta.get("version")
        if version is not None:
            if isinstance(version, bool):
                errors.append(f"version 不能是布尔值: {version}")
            elif isinstance(version, (int, float)):
                warnings.append(
                    f"version 被 YAML 解析为数值: {version}。"
                    f"建议加引号: version: \"{version}\""
                )
                version_str = str(version)
            elif isinstance(version, str):
                version_str = version
            else:
                errors.append(f"version 类型非法: {type(version).__name__}")
                version_str = ""

            if version_str and not SEMVER_PATTERN.match(version_str):
                errors.append(
                    f"version 格式错误: '{version_str}'。"
                    f"应为语义化版本（如 1.0.0）"
                )

        # assembly_order
        order = meta.get("assembly_order")
        if order is not None:
            if isinstance(order, bool):
                errors.append(f"assembly_order 不能是布尔值: {order}")
            elif isinstance(order, int):
                if order < 0:
                    errors.append(f"assembly_order 不能为负数: {order}")
                if order > 10000:
                    warnings.append(f"assembly_order 过大: {order}")
            elif isinstance(order, str):
                try:
                    int(order)
                    warnings.append(
                        f"assembly_order 是字符串 '{order}'，建议整数"
                    )
                except ValueError:
                    errors.append(f"assembly_order 不是有效整数: '{order}'")
            else:
                errors.append(f"assembly_order 类型非法: {type(order).__name__}")

        # dependencies
        if "dependencies" in meta:
            deps = meta["dependencies"]
            if deps is None:
                warnings.append("dependencies 为 null，建议省略该字段")
            elif not isinstance(deps, list):
                errors.append("dependencies 必须是列表")
            else:
                for i, dep in enumerate(deps):
                    if not isinstance(dep, dict):
                        errors.append(f"dependencies[{i}] 必须是字典")
                        continue
                    for req in ("module", "file"):
                        if req not in dep:
                            msg = f"dependencies[{i}] 缺少 '{req}'"
                            if self.strict:
                                errors.append(msg)
                            else:
                                warnings.append(msg)
                        elif not isinstance(dep[req], str):
                            errors.append(
                                f"dependencies[{i}].{req} 必须是字符串"
                            )

        # provides
        if "provides" in meta:
            p = meta["provides"]
            if p is not None and not isinstance(p, list):
                errors.append("provides 必须是列表")

        # health_check
        if "health_check" in meta:
            hc = meta["health_check"]
            if hc is not None:
                if not isinstance(hc, str):
                    errors.append("health_check 必须是字符串")
                elif not hc.startswith("/"):
                    warnings.append(f"health_check 应以 / 开头: '{hc}'")

        # 未知字段
        known = {
            "module", "type", "name", "version", "assembly_order",
            "author", "created_at", "created", "brief", "description",
            "dependencies", "provides", "interfaces", "config_keys",
            "health_check", "events_emitted", "events_consumed",
        }
        unknown = set(meta.keys()) - known
        if unknown:
            if self.strict:
                errors.append(f"未知字段（strict 模式）: {sorted(unknown)}")
            else:
                warnings.append(f"未知字段: {sorted(unknown)}")

        return errors, warnings


# ==============================================================================
# 文件名一致性检查
# ==============================================================================
def check_filename_consistency(path: Path, meta: Dict[str, Any]) -> Optional[str]:
    """返回警告信息或 None"""
    module = meta.get("module")
    ftype = meta.get("type")
    name = meta.get("name")

    if not (module and ftype and name):
        return None

    if not all(isinstance(x, str) for x in (module, ftype, name)):
        return None

    filename = path.stem
    expected_prefix = f"{module}.{ftype}.{name}"

    # 精确匹配或跟随分隔符
    if filename == expected_prefix:
        return None
    if filename.startswith(expected_prefix + "."):
        return None
    if filename.startswith(expected_prefix + "_"):
        return None

    # 特殊的测试文件后缀
    for suffix in (".test", ".spec", "_test", "_spec"):
        if filename.endswith(suffix):
            base = filename[:-len(suffix)]
            if base == expected_prefix:
                return None

    return (
        f"文件名与元数据不一致: 文件名 '{filename}' "
        f"期望前缀 '{expected_prefix}'"
    )


# ==============================================================================
# 文件扫描
# ==============================================================================
class FileScanner:
    def __init__(
        self,
        root: Path,
        exclude_dirs: Optional[set] = None,
        extensions: Optional[set] = None,
    ):
        self.root = root
        self.exclude_dirs = exclude_dirs or DEFAULT_EXCLUDE_DIRS
        self.extensions = extensions or set(COMMENT_STYLES.keys())

    def scan(self) -> Iterator[Path]:
        if not self.root.exists():
            return
        if self.root.is_file():
            if self._is_candidate(self.root):
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

            # 原地修改 dirnames 以跳过子目录
            # 注意：以 . 开头的目录默认跳过，但保留根目录
            dirnames[:] = [
                d for d in dirnames
                if d not in self.exclude_dirs and not d.startswith(".")
            ]
            dirnames.sort()

            for filename in sorted(filenames):
                if _interrupted:
                    return
                if filename.startswith("."):
                    continue
                path = Path(dirpath) / filename
                if self._is_candidate(path):
                    yield path

    def _is_candidate(self, path: Path) -> bool:
        return path.suffix.lower() in self.extensions


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
        self.reader = SafeReader(verbose=verbose)
        self.extractor = MetadataExtractor()
        self.meta_validator = MetadataValidator(strict=strict)
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
        result = FileResult(path=rel, status="ok")

        text = self.reader.read(path)
        if text is None:
            result.status = "skipped"
            result.warnings.append("文件不可读或为二进制")
            self.summary.skipped += 1
            self.summary.results.append(result)
            self._print(result)
            return

        style = COMMENT_STYLES.get(path.suffix.lower())
        if not style:
            result.status = "skipped"
            result.warnings.append(f"未知扩展名: {path.suffix}")
            self.summary.skipped += 1
            self.summary.results.append(result)
            self._print(result)
            return

        yaml_text = self.extractor.extract(text, style)
        if not yaml_text or not yaml_text.strip():
            result.status = "error"
            result.errors.append("未找到元数据块（缺少 --- 围栏）")
            self.summary.errors += 1
            self.summary.results.append(result)
            self._print(result)
            return

        try:
            meta = yaml.safe_load(yaml_text)
        except yaml.YAMLError as e:
            result.status = "error"
            msg = str(e).replace("\n", " ")[:200]
            result.errors.append(f"YAML 解析失败: {msg}")
            self.summary.errors += 1
            self.summary.results.append(result)
            self._print(result)
            return

        if meta is None:
            result.status = "error"
            result.errors.append("元数据为空")
            self.summary.errors += 1
            self.summary.results.append(result)
            self._print(result)
            return

        # 校验
        errs, warns = self.meta_validator.validate(meta)
        result.errors.extend(errs)
        result.warnings.extend(warns)

        # 文件名一致性
        if isinstance(meta, dict):
            result.metadata = meta
            msg = check_filename_consistency(path, meta)
            if msg:
                result.warnings.append(msg)

        if result.errors:
            result.status = "error"
            self.summary.errors += 1
        elif result.warnings:
            result.status = "warning"
            self.summary.warnings += 1
        else:
            result.status = "ok"
            self.summary.ok += 1

        self.summary.results.append(result)
        self._print(result)

    def _relative(self, path: Path) -> str:
        try:
            if self.root.is_file():
                return path.name
            return str(path.relative_to(self.root))
        except ValueError:
            return str(path)

    def _print(self, result: FileResult) -> None:
        # 静默模式：仅错误
        if self.quiet:
            if result.status != "error":
                return
        elif result.status == "ok" and not self.show_all:
            return

        icons = {
            "ok": f"{Color.GREEN}✓{Color.RESET}",
            "warning": f"{Color.YELLOW}⚠{Color.RESET}",
            "error": f"{Color.RED}✗{Color.RESET}",
            "skipped": f"{Color.BLUE}−{Color.RESET}",
        }
        print(f"  {icons[result.status]} {result.path}")
        for e in result.errors:
            print(f"      {Color.RED}{e}{Color.RESET}")
        for w in result.warnings:
            print(f"      {Color.YELLOW}{w}{Color.RESET}")


# ==============================================================================
# 输出
# ==============================================================================
def print_summary(summary: Summary) -> None:
    print()
    print(f"{Color.BOLD}━━━ 校验结果 ━━━{Color.RESET}")
    print()
    print(f"  扫描文件:  {summary.total}")
    print(f"  {Color.GREEN}通过:      {summary.ok}{Color.RESET}")
    if summary.warnings:
        print(f"  {Color.YELLOW}警告:      {summary.warnings}{Color.RESET}")
    if summary.errors:
        print(f"  {Color.RED}错误:      {summary.errors}{Color.RESET}")
    if summary.skipped:
        print(f"  {Color.BLUE}跳过:      {summary.skipped}{Color.RESET}")
    if summary.duration_ms:
        print(f"  耗时:      {summary.duration_ms} ms")
    print()

    if summary.errors:
        print(f"{Color.RED}❌ 校验失败：{summary.errors} 个错误{Color.RESET}")
    elif summary.warnings:
        print(f"{Color.YELLOW}⚠ 校验通过（{summary.warnings} 个警告）{Color.RESET}")
    else:
        print(f"{Color.GREEN}✅ 全部通过{Color.RESET}")


# ==============================================================================
# 主函数
# ==============================================================================
def main() -> int:
    parser = argparse.ArgumentParser(
        description="文件元数据校验",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--root", default="src", help="扫描根目录")
    parser.add_argument("--strict", action="store_true",
                        help="严格模式：警告视为错误，未知字段报错")
    parser.add_argument("--json", action="store_true", help="JSON 输出")
    parser.add_argument("--quiet", action="store_true", help="静默模式")
    parser.add_argument("--verbose", action="store_true", help="详细输出")
    parser.add_argument("--show-all", action="store_true",
                        help="显示所有文件")
    parser.add_argument("--summary-only", action="store_true",
                        help="仅输出汇总（JSON 模式下省略 results）")
    parser.add_argument("--no-color", action="store_true", help="禁用颜色")
    parser.add_argument("--exclude", action="append", default=[],
                        help="额外排除的目录")
    parser.add_argument("--ext", action="append", default=[],
                        help="仅扫描指定扩展名")
    parser.add_argument("--include-metadata", action="store_true",
                        help="JSON 输出包含元数据")
    parser.add_argument("--version", action="version",
                        version=f"validate_metadata.py {VERSION}")

    args = parser.parse_args()

    if args.no_color or args.json:
        Color.disable()

    # 信号
    signal.signal(signal.SIGINT, _signal_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _signal_handler)

    # 根路径
    root = Path(args.root).expanduser().resolve()
    if not root.exists():
        sys.stderr.write(f"❌ 路径不存在: {root}\n")
        return 2

    # 排除目录
    exclude = set(DEFAULT_EXCLUDE_DIRS)
    exclude.update(args.exclude)

    # 扩展名（标准化小写）
    extensions = None
    if args.ext:
        extensions = set()
        for e in args.ext:
            e = e.strip().lower()
            if not e.startswith("."):
                e = f".{e}"
            extensions.add(e)

    # 头部打印
    if not args.json and not args.quiet:
        print()
        print(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
        print(f"{Color.CYAN}{Color.BOLD}  文件元数据校验  v{VERSION}{Color.RESET}")
        print(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
        print()
        print(f"  根目录: {root}")
        print(f"  模式:   {'严格' if args.strict else '普通'}")
        print()

    # 扫描器与校验器
    scanner = FileScanner(root, exclude_dirs=exclude, extensions=extensions)
    validator = Validator(
        root=root,
        strict=args.strict,
        verbose=args.verbose,
        show_all=args.show_all and not args.quiet and not args.json,
        quiet=args.quiet or args.json,
    )

    # 运行
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
        output = summary.to_dict(
            include_results=not args.summary_only,
            include_metadata=args.include_metadata,
        )
        print(json.dumps(output, ensure_ascii=False, indent=2))
    else:
        print_summary(summary)

    # 退出码
    if summary.errors > 0:
        return 1
    if args.strict and summary.warnings > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
