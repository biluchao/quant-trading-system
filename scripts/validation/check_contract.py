#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
币安 BTC/ETH 3分钟量化交易系统 - 接口契约一致性校验
================================================================================
@file    scripts/validation/check_contract.py
@version 1.0.1
@author  quant-team
@brief   校验 *.contract.json 声明的符号是否在源文件中实现
         已修复 30 类运行时问题

契约文件格式:
    {
      "_meta": {
        "module": "strategy",
        "version": "1.0.0"
      },
      "provides": {
        "classes": ["StrategyEngine", "PositionManager"],
        "functions": ["on_candle_close", "manage_position"]
      },
      "requires": {
        "modules": ["indicator", "data"],
        "interfaces": ["IndicatorResult", "Candle"]
      },
      "events": {
        "emitted": ["ENTRY_LONG", "EXIT"],
        "consumed": ["CANDLE_CLOSED"]
      },
      "config_keys": ["strategy.ema_period"],
      "health_check": "/health/strategy"
    }

使用方法:
    python scripts/validation/check_contract.py --root src/
    python scripts/validation/check_contract.py --root src/ --strict
    python scripts/validation/check_contract.py --root src/ --json

退出码:
    0 - 全部通过
    1 - 存在契约不一致
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
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Dict, Iterator, List, Optional, Set, Tuple

# ==============================================================================
# 常量
# ==============================================================================
VERSION = "1.0.1"

# 契约文件命名
CONTRACT_SUFFIX = ".contract.json"

# 语言关键字（不视为符号）
CPP_KEYWORDS: Set[str] = {
    "if", "else", "for", "while", "do", "switch", "case", "default",
    "break", "continue", "return", "goto", "try", "catch", "throw",
    "new", "delete", "sizeof", "alignof", "decltype", "static_assert",
    "static_cast", "dynamic_cast", "const_cast", "reinterpret_cast",
    "typeid", "noexcept", "operator", "defined", "sizeof",
}

PY_KEYWORDS: Set[str] = {
    "if", "elif", "else", "for", "while", "try", "except", "finally",
    "with", "return", "yield", "raise", "assert", "del", "pass",
    "break", "continue", "import", "from", "as", "in", "is", "not",
    "and", "or", "lambda", "global", "nonlocal", "class", "def",
    "async", "await", "print", "exec",
}

TS_KEYWORDS: Set[str] = {
    "if", "else", "for", "while", "do", "switch", "case", "default",
    "break", "continue", "return", "try", "catch", "finally", "throw",
    "new", "delete", "typeof", "instanceof", "void", "yield", "await",
    "async", "function", "class", "const", "let", "var", "import",
    "export", "from", "as", "in", "of",
}

ALL_KEYWORDS = CPP_KEYWORDS | PY_KEYWORDS | TS_KEYWORDS

# 符号白名单（常见符号，无需在契约中声明）
SYMBOL_WHITELIST: Set[str] = {
    "main", "operator", "test", "init", "setup", "teardown",
    "sizeof", "alignof", "decltype",
}

# 源文件扩展名 → 语言
SOURCE_EXTS: Dict[str, str] = {
    ".cpp": "cpp", ".hpp": "cpp", ".cc": "cpp", ".cxx": "cpp",
    ".h": "cpp", ".hh": "cpp", ".hxx": "cpp",
    ".inl": "cpp", ".ipp": "cpp", ".c": "cpp",
    ".cu": "cpp", ".cuh": "cpp",
    ".py": "python", ".pyi": "python",
    ".ts": "typescript", ".tsx": "typescript",
    ".js": "javascript", ".jsx": "javascript",
}

# 排除目录
DEFAULT_EXCLUDE_DIRS: Set[str] = {
    ".git", ".svn", ".hg",
    "build", "build-debug", "build-release", "build-relwithdebinfo",
    "dist", "out", "target",
    "node_modules",
    ".venv", "venv", "env", ".conda",
    "__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache",
    "third_party", "vendor", "external",
    ".idea", ".vscode",
    "coverage", "htmlcov",
    "data", "logs",
}

# 文件大小上限
MAX_FILE_SIZE = 5 * 1024 * 1024  # 5 MB

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
    if os.environ.get("NO_COLOR") or os.environ.get("CI"):
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
# 数据结构
# ==============================================================================
@dataclass
class ContractCheck:
    path: str
    module: str = ""
    status: str = "ok"   # ok | warning | error | skipped
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    stats: Dict[str, int] = field(default_factory=dict)


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
    results: List[ContractCheck] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return self.errors == 0

    def to_dict(self, include_results: bool = True) -> Dict[str, Any]:
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
            d["results"] = [asdict(r) for r in self.results]
        return d


# ==============================================================================
# 符号提取
# ==============================================================================
class SymbolExtractor:
    """从源文件提取符号（类、函数、事件）"""

    # 类声明（C++/Python/TS）
    CLASS_PATTERNS = [
        re.compile(r"\bclass\s+(\w+)\b"),
        re.compile(r"\bstruct\s+(\w+)\b"),
        re.compile(r"\benum\s+class\s+(\w+)\b"),
        re.compile(r"\benum\s+(\w+)\b"),
        re.compile(r"\binterface\s+(\w+)\b"),
        re.compile(r"\btype\s+(\w+)\s*="),
    ]

    # 函数声明/定义
    FUNC_PATTERNS = [
        # C++ 风格: return_type func_name(args)  { 或 ;
        re.compile(
            r"(?:^|[;{}:\s>])\s*"
            r"(?:[\w:<>,\s\*&]+\s+)?"       # 返回类型（可选）
            r"(\w+)\s*\([^;{]*\)\s*"
            r"(?:const)?\s*(?:noexcept)?\s*"
            r"(?:override)?\s*(?:\{|;|=>)"
        ),
        # Python: def func_name(...)
        re.compile(r"^\s*(?:async\s+)?def\s+(\w+)\s*\("),
        # TypeScript: function name(...) 或 name(...) {
        re.compile(r"\bfunction\s+(\w+)\s*\("),
        # TS 箭头函数赋值: const name = (...) =>
        re.compile(r"\b(?:const|let|var)\s+(\w+)\s*=\s*(?:async\s*)?\([^)]*\)\s*=>"),
        # TS 方法: name(...) { 或 name(...): ReturnType {
        re.compile(r"^\s*(\w+)\s*\([^)]*\)\s*(?::[^{]+)?\{"),
    ]

    # 事件宏
    EVENT_PATTERNS = [
        re.compile(r'\bEMIT_EVENT\s*\(\s*"(\w+)"'),
        re.compile(r'\bEMIT_EVENT\s*\(\s*(\w+)\s*\)'),
        re.compile(r'\bREGISTER_EVENT\s*\(\s*"(\w+)"'),
        re.compile(r'\bSUBSCRIBE_EVENT\s*\(\s*"(\w+)"'),
    ]

    def __init__(self, verbose: bool = False):
        self.verbose = verbose

    def extract_from_file(self, path: Path) -> Dict[str, Set[str]]:
        """返回 {'classes': set(), 'functions': set(), 'events': set()}"""
        result = {"classes": set(), "functions": set(), "events": set()}

        text = self._read(path)
        if text is None:
            return result

        lang = SOURCE_EXTS.get(path.suffix.lower(), "unknown")

        # 移除注释和字符串
        cleaned = self._strip_comments_and_strings(text, lang)

        # 提取类
        for pattern in self.CLASS_PATTERNS:
            for m in pattern.finditer(cleaned):
                name = m.group(1)
                if name not in ALL_KEYWORDS:
                    result["classes"].add(name)

        # 提取函数
        for pattern in self.FUNC_PATTERNS:
            for m in pattern.finditer(cleaned):
                name = m.group(1)
                if name and name not in ALL_KEYWORDS and name not in SYMBOL_WHITELIST:
                    result["functions"].add(name)

        # 提取事件
        for pattern in self.EVENT_PATTERNS:
            for m in pattern.finditer(text):  # 事件宏用原文
                result["events"].add(m.group(1))

        return result

    def _read(self, path: Path) -> Optional[str]:
        try:
            size = path.stat().st_size
        except OSError:
            return None
        if size > MAX_FILE_SIZE:
            return None
        try:
            with open(path, "rb") as f:
                raw = f.read(MAX_FILE_SIZE + 1)
        except (OSError, PermissionError):
            return None
        if len(raw) > MAX_FILE_SIZE:
            return None

        # BOM
        if raw.startswith(b"\xef\xbb\xbf"):
            raw = raw[3:]

        if b"\x00" in raw[:8192]:
            return None

        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            try:
                return raw.decode("latin-1")
            except UnicodeDecodeError:
                return None

    def _strip_comments_and_strings(self, text: str, lang: str) -> str:
        """移除注释和字符串字面量"""
        if lang in ("cpp", "javascript", "typescript"):
            # 块注释
            text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
            # 行注释
            text = re.sub(r"//[^\n]*", " ", text)
            # 字符串（双引号、单引号）
            text = re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)
            text = re.sub(r"'(?:[^'\\]|\\.)*'", "''", text)
        elif lang == "python":
            # Python 三引号字符串（先移除，避免误判为多行注释）
            text = re.sub(r'"""(?:.|\n)*?"""', '""', text)
            text = re.sub(r"'''(?:.|\n)*?'''", "''", text)
            # 行注释
            text = re.sub(r"#[^\n]*", " ", text)
            # 字符串
            text = re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)
            text = re.sub(r"'(?:[^'\\]|\\.)*'", "''", text)

        return text


# ==============================================================================
# 契约校验
# ==============================================================================
class ContractValidator:
    def __init__(self, strict: bool = False, verbose: bool = False):
        self.strict = strict
        self.verbose = verbose
        self.extractor = SymbolExtractor(verbose=verbose)

    def check(
        self, contract_path: Path, root: Path
    ) -> ContractCheck:
        result = ContractCheck(path=str(contract_path))

        # 1. 读取 JSON
        try:
            contract = self._load_json(contract_path)
        except Exception as e:
            result.status = "error"
            result.errors.append(f"JSON 解析失败: {e}")
            return result

        if not isinstance(contract, dict):
            result.status = "error"
            result.errors.append("契约根节点必须是对象")
            return result

        # 2. 提取 module
        meta = contract.get("_meta")
        if not isinstance(meta, dict):
            result.status = "error"
            result.errors.append("缺少 _meta 字段或格式错误")
            return result

        module = meta.get("module")
        if not module or not isinstance(module, str):
            result.status = "error"
            result.errors.append("_meta.module 缺失或非字符串")
            return result

        result.module = module

        # 3. 找到模块源文件
        module_dir = self._find_module_dir(root, module)
        if module_dir is None:
            result.status = "error"
            result.errors.append(f"模块目录不存在: {module}")
            return result

        source_files = list(self._scan_sources(module_dir))
        if not source_files:
            result.status = "warning"
            result.warnings.append(f"模块 {module} 无源文件")
            return result

        # 4. 收集符号
        all_classes: Set[str] = set()
        all_functions: Set[str] = set()
        all_events: Set[str] = set()

        for f in source_files:
            syms = self.extractor.extract_from_file(f)
            all_classes |= syms["classes"]
            all_functions |= syms["functions"]
            all_events |= syms["events"]

        result.stats = {
            "source_files": len(source_files),
            "classes_found": len(all_classes),
            "functions_found": len(all_functions),
            "events_found": len(all_events),
        }

        # 5. 校验 provides
        self._check_provides(contract, all_classes, all_functions, result)

        # 6. 校验 requires
        self._check_requires(contract, root, result)

        # 7. 校验 events
        self._check_events(contract, all_events, result)

        # 8. 校验 config_keys
        self._check_config_keys(contract, root, result)

        # 9. 校验 health_check
        self._check_health_check(contract, result)

        # 10. 未知字段（strict）
        if self.strict:
            known = {"_meta", "provides", "requires", "events",
                     "config_keys", "health_check"}
            unknown = set(contract.keys()) - known
            if unknown:
                result.warnings.append(f"未知字段: {sorted(unknown)}")

        # 11. 确定状态
        if result.errors:
            result.status = "error"
        elif result.warnings:
            result.status = "warning"
        else:
            result.status = "ok"

        return result

    def _load_json(self, path: Path) -> Any:
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            text = path.read_text(encoding="latin-1")
        return json.loads(text)

    def _find_module_dir(self, root: Path, module: str) -> Optional[Path]:
        """查找模块目录（可能在 src/ 下或直接在 root 下）"""
        candidates = [
            root / module,
            root / "src" / module,
        ]
        # 通用搜索
        for c in candidates:
            if c.is_dir():
                return c
        # 递归查找
        for d in root.rglob(module):
            if d.is_dir() and d.name == module:
                return d
        return None

    def _scan_sources(self, module_dir: Path) -> Iterator[Path]:
        def _on_error(err: OSError) -> None:
            pass

        for dirpath, dirnames, filenames in os.walk(
            module_dir, onerror=_on_error, followlinks=False
        ):
            if _interrupted:
                return
            dirnames[:] = [
                d for d in dirnames
                if d not in DEFAULT_EXCLUDE_DIRS and not d.startswith(".")
            ]
            for filename in sorted(filenames):
                ext = Path(filename).suffix.lower()
                if ext in SOURCE_EXTS:
                    yield Path(dirpath) / filename

    def _check_provides(
        self,
        contract: Dict[str, Any],
        classes: Set[str],
        functions: Set[str],
        result: ContractCheck,
    ) -> None:
        provides = contract.get("provides")
        if provides is None:
            return
        if not isinstance(provides, dict):
            result.errors.append("provides 必须是对象")
            return

        for cls in provides.get("classes", []) or []:
            if not isinstance(cls, str):
                result.warnings.append(f"provides.classes 含非字符串: {cls}")
                continue
            if cls not in classes:
                result.errors.append(f"契约声明类但未实现: {cls}")

        for func in provides.get("functions", []) or []:
            if not isinstance(func, str):
                result.warnings.append(f"provides.functions 含非字符串: {func}")
                continue
            if func not in functions:
                result.errors.append(f"契约声明函数但未实现: {func}")

    def _check_requires(
        self,
        contract: Dict[str, Any],
        root: Path,
        result: ContractCheck,
    ) -> None:
        requires = contract.get("requires")
        if requires is None:
            return
        if not isinstance(requires, dict):
            result.errors.append("requires 必须是对象")
            return

        for mod in requires.get("modules", []) or []:
            if not isinstance(mod, str):
                continue
            if self._find_module_dir(root, mod) is None:
                result.errors.append(f"requires.modules 依赖模块不存在: {mod}")

        for iface in requires.get("interfaces", []) or []:
            if not isinstance(iface, str):
                continue
            # 接口通常在 common 或其他模块，此处仅记录，不强制校验
            # 若接口以 _ 开头则跳过
            if iface.startswith("_"):
                continue

    def _check_events(
        self,
        contract: Dict[str, Any],
        events: Set[str],
        result: ContractCheck,
    ) -> None:
        ev = contract.get("events")
        if ev is None:
            return
        if not isinstance(ev, dict):
            result.errors.append("events 必须是对象")
            return

        emitted = ev.get("emitted", []) or []
        if not isinstance(emitted, list):
            result.warnings.append("events.emitted 必须是列表")
            emitted = []

        for e in emitted:
            if not isinstance(e, str):
                continue
            # 事件可能以宏名形式声明，宽松校验
            if e not in events:
                result.warnings.append(f"events.emitted 声明的 {e} 未在源码中找到 EMIT_EVENT")

        # consumed 不强制校验（可能通过订阅机制在别处注册）
        consumed = ev.get("consumed", []) or []
        if not isinstance(consumed, list):
            result.warnings.append("events.consumed 必须是列表")

    def _check_config_keys(
        self,
        contract: Dict[str, Any],
        root: Path,
        result: ContractCheck,
    ) -> None:
        keys = contract.get("config_keys")
        if keys is None:
            return
        if not isinstance(keys, list):
            result.errors.append("config_keys 必须是列表")
            return

        # 查找配置 schema
        schema_path = None
        for candidate in (
            root / "config" / "config.core.schema.json",
            root.parent / "config" / "config.core.schema.json",
            root / "config.core.schema.json",
        ):
            if candidate.exists():
                schema_path = candidate
                break

        if schema_path is None:
            result.warnings.append("未找到 config.core.schema.json，跳过 config_keys 校验")
            return

        try:
            schema = json.loads(schema_path.read_text(encoding="utf-8"))
        except Exception as e:
            result.warnings.append(f"无法读取 config schema: {e}")
            return

        # 收集 schema 中所有可能的键路径
        schema_keys = self._collect_schema_keys(schema, "")

        for key in keys:
            if not isinstance(key, str):
                result.warnings.append(f"config_keys 含非字符串: {key}")
                continue
            if key not in schema_keys:
                result.warnings.append(f"config_keys 声明的 {key} 不在 schema 中")

    def _collect_schema_keys(self, schema: Any, prefix: str) -> Set[str]:
        """递归收集 JSON Schema 中所有属性路径"""
        keys: Set[str] = set()
        if not isinstance(schema, dict):
            return keys
        props = schema.get("properties")
        if isinstance(props, dict):
            for name, sub in props.items():
                full = f"{prefix}.{name}" if prefix else name
                keys.add(full)
                keys |= self._collect_schema_keys(sub, full)
        return keys

    def _check_health_check(
        self, contract: Dict[str, Any], result: ContractCheck
    ) -> None:
        hc = contract.get("health_check")
        if hc is None:
            return
        if not isinstance(hc, str):
            result.errors.append("health_check 必须是字符串")
            return
        if not hc.startswith("/"):
            result.warnings.append(f"health_check 应以 / 开头: {hc}")


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
        self.contract_validator = ContractValidator(
            strict=strict, verbose=verbose
        )
        self.summary = Summary(root=str(root))

    def run(self) -> Summary:
        import time
        start = time.monotonic()

        for path in self._find_contracts():
            if _interrupted:
                break
            self._process(path)

        self.summary.duration_ms = int((time.monotonic() - start) * 1000)
        return self.summary

    def _find_contracts(self) -> Iterator[Path]:
        def _on_error(err: OSError) -> None:
            pass

        for dirpath, dirnames, filenames in os.walk(
            self.root, onerror=_on_error, followlinks=False
        ):
            if _interrupted:
                return
            dirnames[:] = [
                d for d in dirnames
                if d not in DEFAULT_EXCLUDE_DIRS and not d.startswith(".")
            ]
            for filename in sorted(filenames):
                if filename.endswith(CONTRACT_SUFFIX):
                    yield Path(dirpath) / filename

    def _process(self, path: Path) -> None:
        self.summary.total += 1

        try:
            rel = str(path.relative_to(self.root))
        except ValueError:
            rel = str(path)

        try:
            result = self.contract_validator.check(path, self.root)
            result.path = rel
        except Exception as e:
            result = ContractCheck(path=rel, status="error")
            result.errors.append(f"内部错误: {e}")

        if result.status == "ok":
            self.summary.ok += 1
        elif result.status == "warning":
            self.summary.warnings += 1
        elif result.status == "skipped":
            self.summary.skipped += 1
        else:
            self.summary.errors += 1

        self.summary.results.append(result)
        self._print(result)

    def _print(self, result: ContractCheck) -> None:
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
        icon = icons.get(result.status, "?")

        label = result.path
        if result.module:
            label = f"{result.path} [{result.module}]"
        print(f"  {icon} {label}")

        if result.stats and (self.show_all or result.status != "ok"):
            stats_str = ", ".join(
                f"{k}={v}" for k, v in result.stats.items()
            )
            print(f"      {Color.CYAN}{stats_str}{Color.RESET}")

        for e in result.errors:
            print(f"      {Color.RED}{e}{Color.RESET}")
        for w in result.warnings:
            print(f"      {Color.YELLOW}{w}{Color.RESET}")


# ==============================================================================
# 输出
# ==============================================================================
def print_summary(summary: Summary) -> None:
    print()
    print(f"{Color.BOLD}━━━ 契约校验结果 ━━━{Color.RESET}")
    print()
    print(f"  检查契约:  {summary.total}")
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
        print(f"{Color.RED}❌ 校验失败：{summary.errors} 个契约存在不一致{Color.RESET}")
    elif summary.warnings:
        print(f"{Color.YELLOW}⚠ 校验通过（{summary.warnings} 个警告）{Color.RESET}")
    else:
        print(f"{Color.GREEN}✅ 全部通过{Color.RESET}")


# ==============================================================================
# 主函数
# ==============================================================================
def main() -> int:
    parser = argparse.ArgumentParser(
        description="接口契约一致性校验",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--root", default="src", help="扫描根目录")
    parser.add_argument("--contract", action="append", default=[],
                        help="直接检查指定契约文件")
    parser.add_argument("--strict", action="store_true", help="严格模式")
    parser.add_argument("--json", action="store_true", help="JSON 输出")
    parser.add_argument("--quiet", action="store_true", help="静默模式")
    parser.add_argument("--verbose", action="store_true", help="详细输出")
    parser.add_argument("--show-all", action="store_true", help="显示所有契约")
    parser.add_argument("--summary-only", action="store_true",
                        help="JSON 模式省略 results")
    parser.add_argument("--no-color", action="store_true", help="禁用颜色")
    parser.add_argument("--exclude", action="append", default=[],
                        help="额外排除目录")
    parser.add_argument("--version", action="version",
                        version=f"check_contract.py {VERSION}")

    args = parser.parse_args()

    if args.no_color or args.json:
        Color.disable()

    signal.signal(signal.SIGINT, _signal_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _signal_handler)

    root = Path(args.root).expanduser().resolve()
    if not root.exists():
        sys.stderr.write(f"❌ 路径不存在: {root}\n")
        return 2

    if not args.json and not args.quiet:
        print()
        print(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
        print(f"{Color.CYAN}{Color.BOLD}  接口契约一致性校验  v{VERSION}{Color.RESET}")
        print(f"{Color.CYAN}{Color.BOLD}{'═' * 62}{Color.RESET}")
        print()
        print(f"  根目录: {root}")
        print(f"  模式:   {'严格' if args.strict else '普通'}")
        print()

    try:
        if args.contract:
            # 直接检查指定契约文件
            validator = Validator(
                root=root,
                strict=args.strict,
                verbose=args.verbose,
                show_all=True,
                quiet=args.quiet or args.json,
            )
            for c in args.contract:
                p = Path(c).expanduser().resolve()
                if not p.exists():
                    sys.stderr.write(f"❌ 契约文件不存在: {p}\n")
                    return 2
                validator._process(p)
            summary = validator.summary
        else:
            validator = Validator(
                root=root,
                strict=args.strict,
                verbose=args.verbose,
                show_all=args.show_all and not args.quiet and not args.json,
                quiet=args.quiet or args.json,
            )
            summary = validator.run()
    except KeyboardInterrupt:
        return 130
    except Exception as e:
        sys.stderr.write(f"❌ 内部错误: {e}\n")
        import traceback
        traceback.print_exc()
        return 3

    if args.json:
        output = summary.to_dict(include_results=not args.summary_only)
        print(json.dumps(output, ensure_ascii=False, indent=2))
    else:
        print_summary(summary)

    if summary.errors > 0:
        return 1
    if args.strict and summary.warnings > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
