#!/usr/bin/env python3
"""plant_determinism_gate.py — 方向五：动态轨迹确定性（仿真底层）。

`determinism_gate.py` 只比较**启动期**指纹（seed 派生的实体布局/转向意图）。
本 gate 进一步验证**动态**确定性：只加载 flowsim 单节点（无其他节点调度/墙钟
干扰），让其内部巡航推进 6s，每 10 cycle 写一行全实体状态哈希（位置/朝向/速度/
油门/刹车/横向 offset）。断言：

  1. 同 scenario + 同 seed 跑两遍 → dump **逐字节相同**；
  2. 改 random_seed → dump 必须不同（证明不是恒真）。

范围说明：这证明的是"被控对象 + NPC AI + 场景"这一层的确定性。全链路
（perception→planning→control 异步消息）仍受线程调度影响，不在本条内 ——
那部分由 `determinism_gate.py`（启动期）与 `eval_closed_loop.py`（固定 DT）守。

Usage:
  python3 ci/gates/plant_determinism_gate.py [--duration 6]
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "build" / "bin" / "flow_launcher"
SCENARIO = ROOT / "scenarios" / "dense_route_traffic.json"


def _make_pipeline(work: Path, name: str, scenario: Path, dump: Path) -> Path:
    cfg = json.loads((ROOT / "config" / "pipeline.json").read_text(encoding="utf-8"))
    flowsim = next(p for p in cfg["processes"] if p.get("name") == "flowsim")
    params = json.loads(flowsim.get("params") or "{}")
    params["scenario_file"] = str(scenario)
    params["deterministic_dump"] = str(dump)
    flowsim["params"] = json.dumps(params)
    out = work / f"pipeline_{name}.json"
    # 只保留 flowsim：任何其他节点的墙钟/调度都不参与，测的是仿真核本身。
    out.write_text(json.dumps({
        "scheduler": cfg.get("scheduler", {}),
        "profile": "default",
        "processes": [flowsim],
    }, indent=2) + "\n", encoding="utf-8")
    return out


def _run(pipeline: Path, log_dir: Path, duration: int) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["FLOW_LOG_DIR"] = str(log_dir)
    result = subprocess.run(
        [str(LAUNCHER), str(pipeline), "--duration", str(duration)],
        cwd=ROOT, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120,
    )
    if result.returncode != 0:
        raise SystemExit(f"flowsim-only run failed (rc={result.returncode})\n"
                         f"{result.stdout[-800:]}\n{result.stderr[-800:]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--duration", type=int, default=6)
    args = ap.parse_args()
    if not LAUNCHER.is_file():
        raise SystemExit(f"missing {LAUNCHER} — build first")

    base = json.loads(SCENARIO.read_text(encoding="utf-8"))
    errors: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        sc_a = work / "same.json"
        sc_b = work / "same_copy.json"
        sc_seed = work / "diff_seed.json"
        shutil.copyfile(SCENARIO, sc_a)
        shutil.copyfile(SCENARIO, sc_b)
        seed_sc = json.loads(json.dumps(base))
        seed_sc["random_seed"] = int(base.get("random_seed", 42)) + 1
        sc_seed.write_text(json.dumps(seed_sc, indent=2) + "\n", encoding="utf-8")

        dump_a = work / "dump_a.csv"
        dump_b = work / "dump_b.csv"
        dump_seed = work / "dump_seed.csv"
        _run(_make_pipeline(work, "a", sc_a, dump_a), work / "logs_a", args.duration)
        _run(_make_pipeline(work, "b", sc_b, dump_b), work / "logs_b", args.duration)
        _run(_make_pipeline(work, "seed", sc_seed, dump_seed),
             work / "logs_seed", args.duration)

        if not dump_a.is_file() or dump_a.stat().st_size == 0:
            errors.append("deterministic dump missing/empty — flowsim did not write it")
        else:
            a = dump_a.read_bytes()
            b = dump_b.read_bytes() if dump_b.is_file() else b""
            c = dump_seed.read_bytes() if dump_seed.is_file() else b""
            if not b:
                errors.append("second same-seed run produced no dump")
            if not c:
                errors.append("different-seed run produced no dump")
            # 循环次数由**墙钟 duration** 决定：负载下可能多/少最后一个 10-cycle
            # 行。只比较两份 dump 的公共前缀，并要求至少 5 行（否则样本太少）。
            a_lines = a.splitlines()
            b_lines = b.splitlines()
            common = min(len(a_lines), len(b_lines))
            if common < 5:
                errors.append(f"dump too short ({common} lines) — determinism check weak")
            elif a_lines[:common] != b_lines[:common]:
                errors.append(
                    "same scenario + same seed produced different dynamic trajectory dumps "
                    "(common-prefix mismatch)"
                )
            if c and a_lines[:common] == c.splitlines()[:common]:
                errors.append(
                    "changing random_seed did NOT change the dynamic trajectory dump — "
                    "dump is not seed-derived"
                )

    if errors:
        for e in errors:
            print(f"::error::{e}")
        print(f"plant-determinism-gate FAILED ({len(errors)} issue(s)).")
        return 1
    print("✓ plant determinism OK (same seed → identical trajectory; different seed → differs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())