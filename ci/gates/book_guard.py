#!/usr/bin/env python3
"""docs/book/ 写作风格守门 (CLAUDE.md 文档定位整治)

阻止 docs/book/ 退回"按源码逐条重建说明书"的反范式。每条规则都是可
grep 验证的硬约束，违规即 FAIL。

参照 docs/book/README.md 写作风格约束：

  ❌ 行号引用    `foo.cpp:324` / `bar.h:42-58`
  ❌ 函数符号堆叠  同段 >10 个 `foo_serialize / _deserialize / _swap`
  ❌ 章节总长 >800 行（说明书的特征）
  ❌ 行号密度    每 100 行 >5 处 `:NNN` 引用
  ❌ 大对比表    单行长度 >200 字符 + 含 `|` 多次（疑似 markdown 表格）
  ❌ 一次小步编辑 >20% 行数突变（commit-level check）

策略：
  - chapter-level: 单文件扫描规则 1-5（cheap，跑快）
  - commit-level: 规则 6 单独扫描 `--diff-against <ref>`

Usage:
  python3 ci/gates/book_guard.py
  python3 ci/gates/book_guard.py --diff-against HEAD~5  # commit-level mutation check
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOK_DIR = ROOT / "docs" / "book"

# 章节目标长度范围（参考 docs/book/README.md 字数控制）
MAX_CHAPTER_LINES = 800
MIN_CHAPTER_LINES = 80  # 低于此通常是 stub，提醒作者补内容

# 行号引用：`foo.cpp:NNN` 或 `bar.h:NNN-NNN` 或 `:NNN`
# 注意要避免把 markdown 表格分隔行 `| --- |` 误判。
LINE_REF_PATTERN = re.compile(
    r"(?<![\w/.\-])"             # 前面不是 word char / slash / dot / dash
    r"[A-Za-z_][\w./]*\.(?:cpp|c|h|hpp|cc|py|sh|md)"  # 文件名
    r":\d{2,4}(?:-\d{1,4})?"     # 行号
    r"(?![\w])"                   # 后面不是 word char
)

# 函数符号模式（典型说明书的"函数名堆叠"特征）
FUNC_SYMBOL_PATTERN = re.compile(r"\b[a-z_][a-z0-9_]{4,}\b")

# 单行长度阈值（疑似对比表）
LONG_LINE_THRESHOLD = 200

# 行号密度阈值（每 100 行 `:NNN` 引用数）
LINE_REF_DENSITY_LIMIT = 5

# 函数符号密度阈值（每段同名符号重复）
FUNC_SYMBOL_DENSITY_LIMIT = 30  # 同段出现 ≥30 个 snake_case 标识符视作堆叠


def iter_chapter_files() -> list[Path]:
    """扫描 docs/book/*.md，过滤 README 与 archive banner。"""
    if not BOOK_DIR.exists():
        return []
    files = []
    for f in sorted(BOOK_DIR.glob("*.md")):
        # README / 序 / preface 是元文件，免检
        if f.name in {"README.md", "00_preface.md"}:
            continue
        files.append(f)
    return files


def scan_chapter(path: Path) -> list[str]:
    """单章节扫描，返回违规列表（违规即 FAIL）。"""
    errors: list[str] = []
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()

    # 规则 1：章节总长
    if len(lines) > MAX_CHAPTER_LINES:
        errors.append(
            f"{path.name}: 章节 {len(lines)} 行 > 上限 {MAX_CHAPTER_LINES}（疑似说明书，"
            f"应拆章节或砍代码逐行解读 — 参 docs/book/README.md 字数控制）"
        )
    elif len(lines) < MIN_CHAPTER_LINES:
        errors.append(
            f"{path.name}: 章节 {len(lines)} 行 < 下限 {MIN_CHAPTER_LINES}（stub 状态 — "
            f"v2 重写必须 ≥80 行，避免'标题党'章节污染 BOOK.md 索引）"
        )

    # 规则 2：行号引用密度
    line_refs = LINE_REF_PATTERN.findall(text)
    if len(lines) > 0:
        density = len(line_refs) * 100 / len(lines)
        if density > LINE_REF_DENSITY_LIMIT:
            errors.append(
                f"{path.name}: 行号引用密度 {density:.1f}/100 行 > 上限 "
                f"{LINE_REF_DENSITY_LIMIT}（共 {len(line_refs)} 处 `:NNN` — "
                f"grep 比翻章节快；行号腐化后读者读到错位置）"
            )

    # 规则 3：函数符号密度（同段）
    for lineno, line in enumerate(lines, 1):
        # 跳过代码块 / 表格 / 列表
        stripped = line.strip()
        if stripped.startswith("```") or stripped.startswith("|") or stripped.startswith("-"):
            continue
        # 跳过 markdown 链接
        line_clean = re.sub(r"\[([^\]]+)\]\([^)]+\)", "", line)
        symbols = FUNC_SYMBOL_PATTERN.findall(line_clean)
        # 过滤常见词
        common_words = {"const", "static", "return", "void", "true", "false",
                        "nullptr", "this", "that", "these", "those", "which"}
        real_symbols = [s for s in symbols if s not in common_words]
        if len(real_symbols) > FUNC_SYMBOL_DENSITY_LIMIT:
            errors.append(
                f"{path.name}:{lineno}: 单段含 {len(real_symbols)} 个函数符号 > 上限 "
                f"{FUNC_SYMBOL_DENSITY_LIMIT}（疑似'符号堆叠'——应改为人话描述或拆段）"
            )
            break  # 一个章节报一次

    # 规则 4：超长对比表行
    for lineno, line in enumerate(lines, 1):
        if len(line) > LONG_LINE_THRESHOLD and line.count("|") >= 5:
            errors.append(
                f"{path.name}:{lineno}: 行长 {len(line)} > {LONG_LINE_THRESHOLD} 且含 {line.count('|')} 个 "
                f"'|' 表格分隔符（疑似 50+ 行大对比表——表在 PR 描述里能动态更新，在书里只会腐化）"
            )
            break

    return errors


def scan_diff_mutation(against: str) -> list[str]:
    """commit-level 扫描：检查 docs/book/*.md 相对 `against` 的行数突变。

    规则：单文件行数变化 >20% 且 >100 行变更，疑似 sub-agent 静默重写。
    """
    errors: list[str] = []
    try:
        out = subprocess.run(
            ["git", "diff", "--numstat", against, "--", "docs/book/"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        )
    except subprocess.CalledProcessError as e:
        errors.append(f"git diff failed: {e.stderr.strip()}")
        return errors

    for line in out.stdout.strip().splitlines():
        if not line:
            continue
        added, removed, path = line.split("\t", 2)
        if added == "-" or removed == "-":
            continue
        added_n = int(added)
        removed_n = int(removed)
        delta = abs(added_n - removed_n)
        max_change = max(added_n, removed_n)
        if max_change < 100:
            continue  # 小改动不检
        # 取最新文件总行数估算
        try:
            current_lines = int(subprocess.run(
                ["wc", "-l", path], cwd=ROOT,
                capture_output=True, text=True, check=True,
            ).stdout.split()[0])
        except (subprocess.CalledProcessError, ValueError):
            current_lines = max_change
        ratio = max_change * 100 / max(current_lines, 1)
        if ratio > 20:
            errors.append(
                f"{path}: 相对 {against} 行数变化 {max_change} 行 ({ratio:.0f}% of "
                f"current {current_lines}) > 20% 阈值（疑似一次性大改 — "
                f"docs/book 编辑纪律要求 '不在 PR 中间改；每次重写一整章'）"
            )

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--diff-against", default=None,
                        help="commit-level 行数突变扫描基线 ref（如 HEAD~5、main~1）")
    parser.add_argument("--warn-only", action="store_true",
                        help="advisory gate：v1 章节存量违规只 WARN，不 fail。")
    args = parser.parse_args()

    chapters = iter_chapter_files()
    if not chapters:
        print("book_guard: docs/book/ 暂无可扫描章节（跳过）")
        return 0

    errors: list[str] = []
    for ch in chapters:
        errors.extend(scan_chapter(ch))

    if args.diff_against:
        errors.extend(scan_diff_mutation(args.diff_against))

    if errors:
        level = "WARN" if args.warn_only else "FAIL"
        print(f"{'⚠️ ' if args.warn_only else '❌ '}book_guard {level} ({len(errors)} issue(s)):\n")
        for e in errors:
            print(f"  ::{level.lower()}::{e}")
        return 0 if args.warn_only else 1

    print(f"✓ book_guard OK ({len(chapters)} chapter(s))"
          + (f" + diff-against {args.diff_against}" if args.diff_against else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())