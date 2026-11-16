#!/usr/bin/env python3
"""parking_training_suite.py — 泊车专项训练与分级评测套件。

覆盖三类核心泊车技能闭环验证：
  1. 垂直倒车入库 (Perpendicular Reverse Parking)
  2. 侧方位停车 (Parallel Parking — 标准/偏置多工况)
  3. 歪斜车位二次揉库纠偏 (Crooked-Slot Re-alignment: ±22° 偏斜初态拉正)

用法:
  python3 tools/parking_training_suite.py [--json-out out/parking_report.json]
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

THIS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(THIS_DIR))

from maneuver_tracker import TrajectoryPoint, WHEELBASE, norm_angle  # noqa: E402
from open_space_parking_sim import (  # noqa: E402
    downsample_trajectory,
    generate_parallel_parking_trajectory,
    generate_perpendicular_parking_trajectory,
    simulate_closed_loop,
)


def _generate_crooked_recovery_trajectory(
    start_pose: tuple[float, float, float],
    target_heading: float,
    dt: float = 0.05,
) -> tuple[list[TrajectoryPoint], tuple[float, float, float]]:
    """生成运动学可行的歪斜车位二次揉库轨迹：弧线前拉回正，再直行倒回库位。"""
    pts: list[TrajectoryPoint] = []
    sx, sy, sh = start_pose

    def step_steer(steer: float, v: float, n: int) -> None:
        nonlocal sx, sy, sh
        k = math.tan(steer) / WHEELBASE
        for _ in range(n):
            yaw = v / WHEELBASE * math.tan(steer)
            sh = norm_angle(sh + yaw * dt)
            sx += (v * math.cos(sh) - WHEELBASE * 0.5 * math.sin(sh) * yaw) * dt
            sy += (v * math.sin(sh) + WHEELBASE * 0.5 * math.cos(sh) * yaw) * dt
            pts.append(TrajectoryPoint(x=sx, y=sy, heading=sh, kappa=k, v=v))

    # Stage 1: Forward pull-out while steering to align heading -> target_heading
    for _ in range(45):
        dh = norm_angle(target_heading - sh)
        if abs(dh) < 0.01:
            steer = 0.0
        else:
            steer = max(-0.48, min(0.48, dh * 2.5))
        step_steer(steer, 1.0, 1)
    step_steer(0.0, 0.5, 10)
    pts.append(TrajectoryPoint(x=sx, y=sy, heading=sh, kappa=0.0, v=0.0))

    # Stage 2: Straight reverse back into slot
    step_steer(0.0, -0.8, 35)
    step_steer(0.0, -0.3, 12)
    pts.append(TrajectoryPoint(x=sx, y=sy, heading=sh, kappa=0.0, v=0.0))

    return downsample_trajectory(pts, 64), (sx, sy, sh)


def run_suite(verbose: bool = False) -> dict[str, Any]:
    cases: list[dict[str, Any]] = []

    # 1. 垂直倒车入库
    start_perp = (0.0, 2.0, 0.0)
    traj_perp, goal_perp = generate_perpendicular_parking_trajectory(start_perp)
    r_perp = simulate_closed_loop(traj_perp, start_perp, goal_perp, max_ticks=450, verbose=verbose)
    cases.append({"name": "perpendicular_reverse", **r_perp})

    # 2. 侧方位停车（标准 + 偏置初态）
    for tag, start_par in [
        ("parallel_standard", (0.0, 2.0, 0.0)),
        ("parallel_lateral_offset", (0.5, 2.35, 0.04)),
    ]:
        traj_par, goal_par = generate_parallel_parking_trajectory(start_par)
        r_par = simulate_closed_loop(traj_par, start_par, goal_par, max_ticks=480, verbose=verbose)
        cases.append({"name": tag, **r_par})

    # 3. 歪斜车位二次揉库纠偏（+22° / -22°）
    goal_rec = (10.0, -5.0, -math.pi * 0.5)
    for tag, deg_err in [
        ("crooked_recovery_pos22deg", 22.0),
        ("crooked_recovery_neg22deg", -22.0),
    ]:
        start_rec = (
            goal_rec[0] + 0.15,
            goal_rec[1] + 0.10,
            norm_angle(goal_rec[2] + math.radians(deg_err)),
        )
        traj_rec, goal_rec_actual = _generate_crooked_recovery_trajectory(start_rec, goal_rec[2])
        r_rec = simulate_closed_loop(traj_rec, start_rec, goal_rec_actual, max_ticks=450, verbose=verbose)
        cases.append({"name": tag, **r_rec})

    passed = sum(1 for c in cases if c["passed"])
    return {
        "schema": "flowengine.parking_training_suite.v1",
        "total_cases": len(cases),
        "passed_cases": passed,
        "all_pass": passed == len(cases),
        "cases": [
            {
                "name": c["name"],
                "passed": c["passed"],
                "pos_err_m": round(c["pos_err"], 4),
                "heading_err_deg": round(c["heading_err_deg"], 3),
                "reverse_triggered": c["reverse_triggered"],
                "gear_flips": c["gear_flips"],
            }
            for c in cases
        ],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json-out", default="", help="Write JSON report")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    rep = run_suite(verbose=args.verbose)
    if args.json_out:
        out = Path(args.json_out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(rep, indent=2) + "\n", encoding="utf-8")

    print(f"Parking Training Suite: {rep['passed_cases']}/{rep['total_cases']} PASSED")
    for c in rep["cases"]:
        mark = "PASS" if c["passed"] else "FAIL"
        print(
            f"  [{mark}] {c['name']:28s} pos_err={c['pos_err_m']:.3f}m  "
            f"heading_err={c['heading_err_deg']:.2f}°  flips={c['gear_flips']}"
        )
    return 0 if rep["all_pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
