#!/usr/bin/env python3
"""virtual_clock_audit.py — 虚拟时钟唯一时间源静态审计门禁。

确保 modules/adas_nodes/ 下的被测节点不直接调用 OS 墙钟 API
(clock_gettime / gettimeofday / std::chrono::*_clock::now)，
必须统一通过 clock_service.h (clock_now_us) 获取时间。
仅带 `/* wall-clock-prof */` 显式注解的内部耗时统计行允许豁免。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCAN_DIR = ROOT / "modules" / "adas_nodes"

FORBIDDEN_RE = re.compile(
    r"\b(clock_gettime|gettimeofday|"
    r"std::chrono::(?:steady_clock|system_clock|high_resolution_clock)::now)\s*\("
)
EXEMPT_MARKER = "wall-clock-prof"
EXTS = {".c", ".cc", ".cpp", ".h", ".hpp"}


def audit() -> list[str]:
    violations: list[str] = []
    for path in sorted(SCAN_DIR.rglob("*")):
        if not path.is_file() or path.suffix not in EXTS:
            continue
        rel = path.relative_to(ROOT)
        for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if EXEMPT_MARKER in line:
                continue
            stripped = line.lstrip()
            if stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*"):
                continue
            m = FORBIDDEN_RE.search(line)
            if m:
                violations.append(f"{rel}:{lineno}: forbidden wall-clock call `{m.group(1)}()`")
    return violations


def main() -> int:
    violations = audit()
    if violations:
        print("✗ virtual_clock_audit FAILED:")
        for v in violations:
            print(f"  {v}")
        return 1
    print("✓ virtual_clock_audit OK (0 unexempt wall-clock calls in modules/adas_nodes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
