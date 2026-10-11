#!/usr/bin/env python3
"""L3 EDR 门禁（L3-P2 方向六）：**真起管线、真写 PEM、读懂事件**。

P2 要求「EDR：在现有 PEM 流上增加 L3 法规事件：激活/退出、TOR、MRM、碰撞前后
窗口，复用 CRC / fsync / 轮转」。本门禁是它的有牙实现，断言：

  Rule 1  声明的事件分类可被 C 侧观测（tools/l3_edr_sim.py 的 EDR_TAXONOMY
          与 include/l3_edr.h 一致，先过分类自洽）
  Rule 2  PEM 里**真的**写出了 l3_* 事件（不是"代码里有 emit 调用"就算）
  Rule 3  L3 场景（ODD 退出→TOR 超时→MRM）必须集齐 activation / tor / mrm /
          odd_exit 四类 —— 这正是该场景的法规取证要求
  Rule 4  所有 l3_* 事件都是 critical（走 fsync，掉电不可丢）

为什么必须真跑：EDR 的实现横跨 monitor 的 embedded 模式分支与 PEM 写出路径，
"代码里有 emit" 与 "PEM 里有记录" 之间隔着好几层（本轮就实测到：EDR 源订阅
恰好被关在 `if (!g.embedded_mode)` 里，写 PEM 的模式收不到要记的事件）。

用法：
  python3 ci/gates/l3_edr_gate.py                    # 默认场景
  python3 ci/gates/l3_edr_gate.py --scenario scenarios/l3_odd_exit_timeout_mrm.json
  python3 ci/gates/l3_edr_gate.py --duration 30
退出码 0 = 全绿；非 0 = 失败断言数。
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PEM_DUMP = ROOT / "tools" / "pem_dump.py"
LAUNCHER = ROOT / "build" / "bin" / "flow_launcher"
DEFAULT_SCENARIO = "scenarios/l3_odd_exit_timeout_mrm.json"

# L3 场景应集齐的事件（法规取证：激活/退出、TOR、MRM、ODD 退出）
REQUIRED_FOR_L3_SCENARIO = ("l3_activation", "l3_tor", "l3_mrm", "l3_odd_exit")


def load_taxonomy():
    """复用 tools/l3_edr_sim.py 的分类（单一事实源，勿抄第二份）。"""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "l3_edr_sim", ROOT / "tools" / "l3_edr_sim.py")
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


def build_production_pipeline(scenario: str, work: Path) -> Path:
    """基于 config/pipeline.json 造一份"带 L3 节点 + 写 PEM"的临时管线。

    为什么不能直接用某个现成配置：EDR 事件源自 odd_monitor / tor_manager（只在
    仿真编排里有），而 PEM 写出只在 `monitor.params.mode == "production"` 时开。
    两者此前从未同时成立，所以 l3_* 事件一条都记不下来。本门禁显式把这个组合
    造出来 —— 这也正是"EDR 要复用现有 PEM 流"的落地方式。
    """
    src = json.loads((ROOT / "config" / "pipeline.json").read_text(encoding="utf-8"))
    for p in src.get("processes", []):
        params = p.get("params")
        if not isinstance(params, str):
            continue
        try:
            pr = json.loads(params)
        except json.JSONDecodeError:
            continue
        if "scenario_file" in pr:
            pr["scenario_file"] = scenario
        if p.get("name") == "monitor":
            pr["mode"] = "production"          # 打开 PEM 写出
            pr.setdefault("rotate_sec", 300)
            pr.setdefault("rotate_mb", 100)
            pr.setdefault("retain_segments", 96)
            pr.setdefault("retain_mb", 1024)
        p["params"] = json.dumps(pr)
    out = work / "pipeline_edr.json"
    out.write_text(json.dumps(src, indent=2), encoding="utf-8")
    return out


def run_pipeline(pipeline: Path, duration: int, work: Path) -> tuple[int, Path]:
    """起一次管线；PEM 写到 work 目录（用 TMPDIR 隔离，不污染 /tmp）。"""
    env = os.environ.copy()
    # PEM 路径由 flow_temp_path() 决定，它读 FLOWENGINE_TEMP_DIR（**不是** TMPDIR）
    # —— 见 include/platform_paths.h::flow_temp_dir。设错变量会让 PEM 写去 /tmp
    # 而门禁在自己的 work 目录里找不到文件（本轮实测踩过）。
    env["FLOWENGINE_TEMP_DIR"] = str(work)
    env["FLOW_LOG_DIR"] = str(work / "logs")
    env["FLOW_TOPOLOGY_FILE"] = str(work / "topology.json")
    try:
        proc = subprocess.run(
            [str(LAUNCHER), str(pipeline), "--duration", str(duration)],
            cwd=ROOT, env=env, capture_output=True, text=True,
            timeout=duration + 120)
    except subprocess.TimeoutExpired:
        return 124, work
    return proc.returncode, work


def collect_events(work: Path) -> tuple[dict, list, str]:
    """解析 work 目录下所有 PEM 的 event 记录。返回 (计数, 原始记录, 错误)。"""
    pems = sorted(work.glob("kunautodrive_pem_*.pem"))
    if not pems:
        return {}, [], f"no PEM written under {work} (PEM production mode off?)"
    counts: dict[str, int] = {}
    raw: list = []
    err = ""
    for pem in pems:
        try:
            out = subprocess.run(
                [sys.executable, str(PEM_DUMP), "--jsonl", str(pem)],
                capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            err = f"pem_dump timed out on {pem.name}"
            continue
        if out.returncode != 0:
            err = f"pem_dump failed on {pem.name}: {out.stderr.strip()[:200]}"
            continue
        for line in out.stdout.splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("type") != "event":
                continue
            name = str(rec.get("name", ""))
            counts[name] = counts.get(name, 0) + 1
            raw.append(rec)
    return counts, raw, err


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", default=DEFAULT_SCENARIO)
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--keep", action="store_true", help="保留临时工作目录")
    args = ap.parse_args()

    failures: list[str] = []

    # ── Rule 1：分类自洽（Python 侧单一事实源）──
    tax = load_taxonomy()
    for f in tax.check_taxonomy():
        failures.append(f"[taxonomy] {f}")
    declared = {e["event"] for e in tax.EDR_TAXONOMY}
    l3_declared = {n for n in declared if n.startswith("l3_")}
    print(f"declared L3 EDR events: {sorted(l3_declared)}")

    if not LAUNCHER.is_file():
        print(f"FAIL: launcher not built at {LAUNCHER}", file=sys.stderr)
        return 1

    work = Path(tempfile.mkdtemp(prefix="l3_edr_gate_"))
    try:
        pipeline = build_production_pipeline(args.scenario, work)
        print(f"running pipeline ({args.duration}s) scenario={args.scenario} …",
              flush=True)
        rc, _ = run_pipeline(pipeline, args.duration, work)
        if rc != 0:
            failures.append(f"pipeline exited {rc} (expected 0)")

        counts, raw, err = collect_events(work)
        if err:
            failures.append(err)
        print(f"PEM event counts: {counts or '(none)'}")

        # ── Rule 2：PEM 里真的写出了 l3_* 事件 ──
        seen_l3 = sorted(n for n in counts if n.startswith("l3_"))
        if not seen_l3:
            failures.append(
                "no l3_* event in PEM — EDR not actually recorded "
                f"(PEM event names seen: {sorted(counts) or ['none']})")
        else:
            # 只允许声明过的事件名（防止改名后门禁与文档漂移）
            undeclared = [n for n in seen_l3 if n not in declared]
            if undeclared:
                failures.append(f"PEM contains undeclared L3 events {undeclared} "
                                f"(taxonomy: {sorted(l3_declared)})")

        # ── Rule 3：L3 场景必须集齐四类 ──
        if "l3_odd_exit" in args.scenario or "l3_" in args.scenario:
            missing = [n for n in REQUIRED_FOR_L3_SCENARIO if counts.get(n, 0) == 0]
            if missing:
                failures.append(
                    f"L3 scenario must record {list(REQUIRED_FOR_L3_SCENARIO)} "
                    f"but missing {missing} (got: {counts})")

        # ── Rule 4：所有 l3_* 事件必须 critical（fsync，掉电不可丢）──
        non_critical = [r.get("name") for r in raw
                        if str(r.get("name", "")).startswith("l3_")
                        and r.get("critical") is not True]
        if non_critical:
            failures.append(f"these L3 events were not marked critical/fsync: "
                            f"{sorted(set(non_critical))}")

        if args.json:
            print(json.dumps({"counts": counts, "failures": failures},
                             ensure_ascii=False, indent=2))
    finally:
        if args.keep:
            print(f"work dir kept: {work}")
        else:
            subprocess.run(["rm", "-rf", str(work)], check=False)

    if failures:
        print("\nFAIL:")
        for f in failures:
            print(f"  ✗ {f}")
        print(f"\n{len(failures)} assertion(s) violated")
        return len(failures)

    print("\n✓ L3 EDR events recorded, all critical/fsync")
    return 0


if __name__ == "__main__":
    sys.exit(main())
