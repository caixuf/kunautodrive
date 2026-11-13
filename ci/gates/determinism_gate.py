#!/usr/bin/env python3
"""determinism_gate.py — 方向五：同 seed 启动期确定性门禁。

背景：`demo.sh` 的**涌现指标**仍受墙钟节拍与线程调度影响（见
docs/FAILURE_MODES.md §6），不能拿整条轨迹做同 seed 相等断言。但 RNG 隔离后，
**启动期的 seed 派生结果**必须完全可复现：实体 id/类型/位置/朝向/路口转向意图/
横向目标。flowsim 把这些量的 FNV-1a 指纹随 scene/frame 发出，本 gate 用两次
独立启动验证：

  1. 同 scenario + 同 seed → `metrics.scene.init_fingerprint` 完全相同；
  2. 改 random_seed → 指纹必须变化（证明不是恒真摆设）；
  3. `traffic_density` 静态结论（spawned/route_segs）也必须一致。

它不启动 3D/前端，只用 flow_launcher + monitor 的拓扑快照，单次 ~3s。

Usage:
  python3 ci/gates/determinism_gate.py [--scenario scenarios/dense_npc.json]
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
TOPOLOGY = Path("/tmp/flow_topology.json")


def _patch_pipeline(work: Path, scenario: Path, name: str) -> Path:
    cfg = json.loads((ROOT / "config" / "pipeline.json").read_text(encoding="utf-8"))
    for proc in cfg.get("processes") or []:
        if proc.get("name") == "flowsim":
            params = json.loads(proc.get("params") or "{}")
            params["scenario_file"] = str(scenario)
            proc["params"] = json.dumps(params)
    out = work / f"pipeline_{name}.json"
    out.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    return out


def _run(pipeline: Path, log_dir: Path, duration: int) -> dict:
    TOPOLOGY.unlink(missing_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["FLOW_LOG_DIR"] = str(log_dir)
    result = subprocess.run(
        [str(LAUNCHER), str(pipeline), "--duration", str(duration)],
        cwd=ROOT, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120,
    )
    if not TOPOLOGY.is_file():
        raise SystemExit(
            f"determinism gate: no topology snapshot (rc={result.returncode})\n"
            f"{result.stdout[-800:]}\n{result.stderr[-800:]}"
        )
    return json.loads(TOPOLOGY.read_text(encoding="utf-8"))


def _scene(doc: dict) -> dict:
    return ((doc.get("metrics") or {}).get("scene") or {})


def _fingerprint(doc: dict) -> str:
    return str(_scene(doc).get("init_fingerprint") or "")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scenario",
                    default=str(ROOT / "scenarios" / "dense_route_traffic.json"))
    ap.add_argument("--duration", type=int, default=3)
    args = ap.parse_args()

    if not LAUNCHER.is_file():
        raise SystemExit(f"missing {LAUNCHER} — build first (bash build.sh release)")

    scenario_src = Path(args.scenario)
    base = json.loads(scenario_src.read_text(encoding="utf-8"))
    errors: list[str] = []

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        sc_a = work / "same_a.json"
        sc_b = work / "same_b.json"
        sc_seed = work / "diff_seed.json"
        shutil.copyfile(scenario_src, sc_a)
        shutil.copyfile(scenario_src, sc_b)
        seed_sc = json.loads(json.dumps(base))
        seed_sc["random_seed"] = int(base.get("random_seed", 42)) + 1
        sc_seed.write_text(json.dumps(seed_sc, indent=2) + "\n", encoding="utf-8")

        pipe_a = _patch_pipeline(work, sc_a, "a")
        pipe_b = _patch_pipeline(work, sc_b, "b")
        pipe_seed = _patch_pipeline(work, sc_seed, "seed")

        first = _run(pipe_a, work / "logs_a", args.duration)
        second = _run(pipe_b, work / "logs_b", args.duration)
        diff_seed = _run(pipe_seed, work / "logs_seed", args.duration)

        fp1, fp2, fp3 = _fingerprint(first), _fingerprint(second), _fingerprint(diff_seed)
        if not fp1:
            errors.append(
                "scene.init_fingerprint 缺失 —— flowsim 未发布指纹，"
                "或 monitor 未透传（确定性检查无法判定）"
            )
        if fp1 and fp1 != fp2:
            errors.append(
                f"same seed/scenario produced different init fingerprints: {fp1} != {fp2}"
            )
        if fp1 and fp3 == fp1:
            errors.append(
                "changing random_seed did NOT change init fingerprint — "
                "指纹恒真，不是真正的 seed 派生量"
            )

        td1 = _scene(first).get("traffic_density")
        td2 = _scene(second).get("traffic_density")
        if td1 != td2:
            errors.append(f"traffic_density init result not deterministic: {td1} != {td2}")
        # 指纹只覆盖 seed 派生值；全零 spawn 时它仍会"同 seed 相同、改 seed 不同"。
        # 必须额外断言 spawn 路径真的产出了 NPC，否则门禁对 spawn 回归失明。
        try:
            spawned = int((td1 or {}).get("spawned", 0) or 0)
        except (TypeError, ValueError):
            spawned = 0
        if spawned < 1:
            errors.append(
                f"traffic_density spawned={spawned} — determinism gate cannot verify "
                f"seed-derived spawn if the spawn path produced nothing"
            )
        entities = _scene(first).get("entities") or []
        if len(entities) < 2:
            errors.append(
                f"scene.entities has {len(entities)} entity(ies) — expected ego + spawned NPCs"
            )

    if errors:
        for e in errors:
            print(f"::error::{e}")
        print(f"determinism-gate FAILED ({len(errors)} issue(s)).")
        return 1
    print("✓ determinism gate OK (same seed → same init fingerprint; different seed → different)")
    return 0


if __name__ == "__main__":
    sys.exit(main())