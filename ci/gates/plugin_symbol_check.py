#!/usr/bin/env python3
"""Catch plugin shared libraries that reference a project symbol they never linked.

Background
----------
Node plugins are `.so` files dlopen'd **into the launcher process**. Undefined
symbols are therefore normal and expected — they get resolved either from the
launcher's own exports (`transport_publish`, `param_get_float`, …) or from
system libraries (libc/libm/libstdc++/cJSON/esmini).

The dangerous case is a symbol that is defined **by a project source file** but
was not actually linked into the plugin that calls it. The linker does not
complain (undefined symbols are allowed in a shared object), so the mistake
surfaces only at runtime: `flow_launcher: symbol lookup error:
build/lib/libplanning_node.so: undefined symbol: traj_swept_check` — the whole
launcher dies at dlopen, and the evaluator reports it as a generic
"run truncated / no topology samples collected" (2026-09-21: adding a new
`traj_safety.c` to the test target but forgetting the `planning_node` target).

This gate therefore looks for exactly that: a symbol that some build artifact
defines, the plugin references, the plugin does not define, and the launcher
does not export.

Usage:
  python3 ci/gates/plugin_symbol_check.py
  python3 ci/gates/plugin_symbol_check.py --verbose
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"

# Artifacts that make up the runtime: the launcher (which exports core symbols to
# plugins) plus every plugin library.
LAUNCHER = BUILD / "bin" / "flow_launcher"

# Symbols provided by the C++/C runtime or third-party libraries; never our problem.
SYSTEM_PREFIXES = (
    "_ITM_", "__", "std::", "_ZSt", "_ZN", "_Zd", "_Unwind_", "operator new", "operator delete",
    "cJSON_", "RM_", "osg", "pthread_", "dl", "pow", "sqrt", "log", "exp", "sin", "cos",
    "atan", "fmod", "hypot", "fabs", "fmin", "fmax", "isnan", "isfinite", "round", "floor",
    "ceil", "strto", "snprintf", "sprintf", "printf", "fprintf", "memcpy", "memset", "memcmp",
    "strlen", "strcmp", "strncmp", "strn", "strc", "strs", "strt", "malloc", "calloc", "free",
    "realloc", "qsort", "abort", "exit", "usleep", "nanosleep", "clock_gettime", "fopen",
    "fclose", "fread", "fwrite", "fgets", "fputs", "fflush", "fseek", "ftell", "rename",
    "remove", "time", "localtime", "strftime", "getenv", "setenv", "unlink", "read", "write",
    "open", "close", "ioctl", "select", "socket", "fcntl", "getsockopt", "setsockopt",
    "htons", "ntohs", "inet_", "gai_", "getaddrinfo", "freeaddrinfo", "bind", "listen",
    "accept", "connect", "send", "recv", "shutdown", "epoll_", "poll", "sigaction", "signal",
    "raise", "kill", "getpid", "sleep", "socketpair", "pipe", "dup", "dup2", "waitpid",
    "fork", "execl", "execv", "system", "realpath", "getcwd", "chdir", "mkdir", "stat",
    "lstat", "access", "isdigit", "isspace", "isdigit", "toupper", "tolower", "gettimeofday",
    "clock", "srand", "rand", "cosf", "sinf", "sqrtf", "fabsf", "logf", "expf", "powf",
    "fmodf", "hypotf", "atan2", "atan2f", "roundf", "floorf", "ceilf", "exp10", "log10",
    "log2", "atanf", "tan", "tanf", "asin", "acos", "lround", "llround", "strtod", "strtol",
    "strtoul", "atoi", "atof", "sscanf", "vsnprintf", "vfprintf", "puts", "putchar",
)


def nm_all_defined(path: Path) -> set[str]:
    """All *defined* symbols from the full (non-dynamic) symbol table.

    Plugins/bins are not stripped, so this also surfaces symbols that were linked
    statically into an executable (e.g. a pure module only wired into a test
    binary) — exactly what we need to recognise "the plugin forgot to link it".
    """
    if not path.exists() or not path.is_file():
        return set()
    try:
        out = subprocess.run(["nm", "--defined-only", str(path)],
                             capture_output=True, text=True, check=False).stdout
    except OSError:
        return set()
    names: set[str] = set()
    for line in out.splitlines():
        parts = line.split()
        # 定义为 "<addr> <type> <name>"；未定义是 "U <name>"（2 段）已被过滤
        if len(parts) >= 3 and parts[1] != "U":
            names.add(parts[-1])
    return names


def nm(path: Path, mode: str) -> set[str]:
    """Return the symbol names from `nm -D <mode>`; empty set if nm/unreadable."""
    if not path.exists():
        return set()
    try:
        out = subprocess.run(["nm", "-D", mode, str(path)],
                             capture_output=True, text=True, check=False).stdout
    except OSError:
        return set()
    names: set[str] = set()
    for line in out.splitlines():
        parts = line.split()
        if not parts:
            continue
        sym = parts[-1]
        if "@" in sym:            # versioned system symbol
            continue
        names.add(sym)
    return names


def is_system(sym: str) -> bool:
    return any(sym.startswith(p) for p in SYSTEM_PREFIXES)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not BUILD.is_dir():
        print("::error::build/ missing — build first (plugin symbol gate needs artifacts)")
        return 1

    plugins = sorted(BUILD.glob("lib/lib*_node.so"))
    if not plugins and not LAUNCHER.exists():
        print("::error::no plugin libraries and no flow_launcher found under build/")
        return 1

    # 项目自己定义的全部动态符号（任何 build 产物里出现即算"项目符号"）
    # 用完整（非动态）符号表：这样连"只被静态链进测试可执行文件"的纯模块也能被认出来
    project_defined: set[str] = set()
    for art in list(plugins) + [LAUNCHER] + sorted(BUILD.glob("lib/*.so")) \
            + sorted(BUILD.glob("lib/*.a")) + sorted(BUILD.glob("bin/*")):
        project_defined |= nm_all_defined(art)

    provided_by_launcher = nm(LAUNCHER, "--defined-only")

    errors: list[str] = []
    for so in plugins:
        defined = nm(so, "--defined-only")
        for sym in sorted(nm(so, "--undefined-only")):
            if is_system(sym) or sym in provided_by_launcher or sym in defined:
                continue
            if sym in project_defined:
                errors.append(
                    f"{so.name}: 引用了项目符号 '{sym}'，但既没链进自己、也没被 launcher 导出 "
                    f"— dlopen 时会 'symbol lookup error' 直接弄死 launcher。"
                    f"检查该模块的 .c 是否漏加进 CMake 目标。"
                )
            elif args.verbose:
                print(f"  [info] {so.name}: 未解析 '{sym}'（不在任何项目产物里，视为外部依赖）")

    if errors:
        for e in errors:
            print(f"::error::{e}")
        print(f"plugin-symbol-gate FAILED ({len(errors)} issue(s)).")
        return 1

    print(f"✓ plugin symbol gate OK ({len(plugins)} plugins, "
          f"{len(provided_by_launcher)} launcher-exported symbols)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
