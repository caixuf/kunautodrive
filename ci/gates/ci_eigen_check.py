#!/usr/bin/env python3
"""ci-eigen-gate — 凡会跑 CMake configure 的 CI job，必须装 libeigen3-dev。

## 为什么存在（2026-10-22 实测）

`CMakeLists.txt` 在找不到 Eigen 时**故意 FATAL_ERROR**（不静默退化，见 L3-P0③）。
Eigen 的三条获取路径是：系统包 → `third_party/eigen` → **联网 git clone**。

当时 `build-release` / `build-asan` / `viz` 三个 job 的 apt 列表里**没有**
`libeigen3-dev`，于是每次 CI 都退到联网 clone：
`Eigen download failed (network unavailable)` → `CMake Error ... refusing to build`。
一次 push 三个 job 齐红，而 `build-release` 在同一提交重跑时又转绿 ——
**红绿取决于 runner 当时的网络，而不是代码**。

`CMakeLists.txt` 里原本用注释声称「CI 三个 job 都已装」，但那条注释写死了
`ci.yml:278/312/412` 行号，随 ci.yml 增删行腐烂，且描述本身与事实相反。
本门禁把该不变量变成可执行检查：**不装就红**，不靠注释自律。

## 判据

解析 `.github/workflows/ci.yml`，对每个 job：

- 若该 job 的任一 `run:` 里出现 `cmake -S`（= 会 configure），则
  - 该 job 必须至少有一条 `apt-get install` 含 `libeigen3-dev`，**或**
  - 该 job 显式传了 `-DFLOWENGINE_ALLOW_NO_EIGEN=ON`（明示的、可 grep 的豁免）

豁免只认显式开关：那是 CMakeLists 里定义的「我知道自己在干什么」通道，
而不是"恰好没装"。

## 用法

    python3 ci/gates/ci_eigen_check.py            # 校验（CI 与本地同一入口）
    python3 ci/gates/ci_eigen_check.py --self-test # 门禁有效性自检（喂坏样本必须报红）
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"

# job 行：两空格缩进的 `name:`（YAML 顶层键）
_JOB_RE = re.compile(r"^  ([A-Za-z0-9_][A-Za-z0-9_-]*):\s*$")
_CONFIGURE_RE = re.compile(r"\bcmake\s+-S\b")
_INSTALL_RE = re.compile(r"apt-get\s+install\b")
_EIGEN_PKG = "libeigen3-dev"
_ESCAPE_HATCH = "FLOWENGINE_ALLOW_NO_EIGEN=ON"


def parse_jobs(text: str) -> dict[str, list[str]]:
    """把 workflow 切成 {job 名: [该 job 的所有行]}。"""
    jobs: dict[str, list[str]] = {}
    current: str | None = None
    for line in text.splitlines():
        m = _JOB_RE.match(line)
        if m:
            current = m.group(1)
            jobs[current] = []
            continue
        if current is not None:
            jobs[current].append(line)
    return jobs


def check(text: str) -> list[str]:
    """返回违规说明列表；空列表 = 通过。"""
    problems: list[str] = []
    for name, lines in parse_jobs(text).items():
        configures = [ln for ln in lines if _CONFIGURE_RE.search(ln)]
        if not configures:
            continue  # 不 configure 的 job（纯静态 gate 等）不受约束
        if any(_ESCAPE_HATCH in ln for ln in lines):
            continue  # 显式豁免
        installs = [ln for ln in lines if _INSTALL_RE.search(ln)]
        if any(_EIGEN_PKG in ln for ln in installs):
            continue
        if installs:
            detail = "apt 装了 [" + "; ".join(
                s.strip() for ln in installs for s in [ln.strip()]
            ) + "] 但缺 " + _EIGEN_PKG
        else:
            detail = "该 job 没有任何 apt-get install 步骤"
        problems.append(f"job '{name}' 会跑 cmake -S 但未装 {_EIGEN_PKG}：{detail}")
    return problems


def _self_test() -> int:
    """门禁有效性自检：坏样本必须被抓住，好样本必须放过。"""
    bad_no_install = """
jobs:
  build-x:
    steps:
      - run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
"""
    bad_missing_pkg = """
jobs:
  build-y:
    steps:
      - run: sudo apt-get install -y -qq libcjson-dev python3
      - run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
"""
    good_with_pkg = """
jobs:
  build-z:
    steps:
      - run: sudo apt-get install -y -qq libcjson-dev libeigen3-dev python3
      - run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
"""
    good_escape = """
jobs:
  build-w:
    steps:
      - run: cmake -S . -B build -DFLOWENGINE_ALLOW_NO_EIGEN=ON
"""
    good_no_configure = """
jobs:
  static-gate:
    steps:
      - run: python3 ci/gates/topic_contract_check.py
"""
    cases = [
        ("无 apt 步骤", bad_no_install, 1),
        ("装了包但缺 eigen", bad_missing_pkg, 1),
        ("装了 eigen", good_with_pkg, 0),
        ("显式豁免 ALLOW_NO_EIGEN", good_escape, 0),
        ("不跑 configure 的静态 gate", good_no_configure, 0),
    ]
    failed = 0
    for label, text, expect in cases:
        got = len(check(text))
        ok = got == expect
        print(f"  {'PASS' if ok else 'FAIL'}  {label}: 期望 {expect} 处违规，实际 {got}")
        if not ok:
            failed += 1
    print(f"--- self-test: {len(cases) - failed} pass, {failed} fail ---")
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return _self_test()
    if not WORKFLOW.exists():
        print(f"FAIL  找不到 {WORKFLOW}")
        return 1
    problems = check(WORKFLOW.read_text(encoding="utf-8"))
    if problems:
        print("FAIL  ci-eigen-gate：以下 job 会 CMake configure 却没装 libeigen3-dev，")
        print("      它们的 configure 会退到联网 git clone Eigen，网络一抖就整片红：")
        for p in problems:
            print(f"  ✗ {p}")
        print("      修法：在该 job 的 apt-get install 里加 libeigen3-dev；")
        print("      确实要无 Eigen 构建时显式传 -DFLOWENGINE_ALLOW_NO_EIGEN=ON。")
        return 1
    n = sum(1 for lines in parse_jobs(WORKFLOW.read_text(encoding='utf-8')).values()
            if any(_CONFIGURE_RE.search(ln) for ln in lines))
    print(f"PASS  ci-eigen-gate：{n} 个会 configure 的 job 都已装 libeigen3-dev（或有显式豁免）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
