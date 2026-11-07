#!/usr/bin/env python3
"""npc_multi_route_check.py — 方向三：OSM 多 OD 路线分配门禁。

背景：`traffic_density` 自动补给此前只沿 ego 的**单条中央 route** spawn。方向三
要求"用 OSM 路网做路线分配"。本仓实现：scenario 的 `npc_routes` 指定
routes.json 的额外链，scenario_loader 把这些链的 roads 并入 road_network，
flowsim 为每条链建独立 `Route`，auto-spawn 轮转交织分配 NPC；这些 NPC 不挂中央
route（route_dir=0），由 road_pos.advance 沿真实拓扑行驶。

本 gate 只验证**分配真的发生**（`scene.traffic_density.npc_route_spawned >= 1`）
且启动期没有 invariant 爆炸/碰撞 —— 城市大图上的长期行为回归仍由 scenario
矩阵负责（该场景暂不进 suite：大图 + 跨链车流会放大 ego 行为方差）。

Usage:
  python3 ci/gates/npc_multi_route_check.py [--duration 6]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "build" / "bin" / "flow_launcher"
SCENARIO = ROOT / "scenarios" / "city_multiroute.json"
TOPOLOGY = Path("/tmp/flow_topology.json")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--duration", type=int, default=6)
    args = ap.parse_args()

    if not LAUNCHER.is_file():
        raise SystemExit(f"missing {LAUNCHER} — build first (bash build.sh release)")
    if not SCENARIO.is_file():
        raise SystemExit(f"missing {SCENARIO}")

    errors: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        cfg = json.loads((ROOT / "config" / "pipeline.json").read_text(encoding="utf-8"))
        for proc in cfg.get("processes") or []:
            if proc.get("name") == "flowsim":
                params = json.loads(proc.get("params") or "{}")
                params["scenario_file"] = str(SCENARIO)
                proc["params"] = json.dumps(params)
        pipeline = work / "pipeline.json"
        pipeline.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")

        TOPOLOGY.unlink(missing_ok=True)
        logs = work / "logs"
        logs.mkdir()
        env = dict(os.environ)
        env["FLOW_LOG_DIR"] = str(logs)
        result = subprocess.run(
            [str(LAUNCHER), str(pipeline), "--duration", str(args.duration)],
            cwd=ROOT, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90,
        )
        if not TOPOLOGY.is_file():
            raise SystemExit(
                f"npc-multi-route gate: no topology snapshot (rc={result.returncode})\n"
                f"{result.stdout[-800:]}\n{result.stderr[-800:]}"
            )
        scene = ((json.loads(TOPOLOGY.read_text(encoding="utf-8")).get("metrics") or {})
                 .get("scene") or {})
        td = scene.get("traffic_density") or {}
        spawned = int(td.get("spawned", 0) or 0)
        npc_route_spawned = int(td.get("npc_route_spawned", 0) or 0)

        if spawned < 1:
            errors.append("traffic_density spawned 0 NPCs (auto-spawn path never ran)")
        if npc_route_spawned < 1:
            errors.append(
                "no NPC was assigned to npc_routes chains (npc_route_spawned=0) — "
                "multi-OD route assignment did not happen"
            )

        # flowsim 的 invariant 汇总走 stderr（`[INV] summary total=N`），
        # 节点日志文件里没有这句。旧实现 grep "invariant failed" 是死检查。
        combined = (result.stdout or "") + "\n" + (result.stderr or "")
        for path in (logs / "flowsim.log", logs / "launcher.log"):
            if path.is_file():
                combined += "\n" + path.read_text(encoding="utf-8", errors="ignore")
        m = re.search(r"\[INV\] summary total=(\d+)", combined)
        if m and int(m.group(1)) > 0:
            errors.append(f"flowsim invariant failures during multi-route check "
                          f"(total={m.group(1)})")
        if "COLLISION ego" in combined:
            errors.append("ego collided during multi-route check")

    if errors:
        for e in errors:
            print(f"::error::{e}")
        print(f"npc-multi-route-gate FAILED ({len(errors)} issue(s)).")
        return 1
    print(f"✓ npc multi-route OK (spawned, npc_route_spawned={npc_route_spawned})")
    return 0


if __name__ == "__main__":
    sys.exit(main())