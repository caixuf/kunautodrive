#!/usr/bin/env python3
"""oracle_evaluator.py — FlowEngine 第 5 层分级判定器 (Oracle)。

将“未碰撞即通过”升级为可计算的分级安全/交规/舒适性判定与失效归因：
  - 安全包络：TTC / PET / DRAC / RSS 纵横向安全裕度
  - 交规符合性：实线压线、闯红灯、超速
  - 舒适性：纵向冲击度 |jerk|、横向加速度 |a_lat|、转向角速度
  - 分级输出：PASS / MARGINAL / FAIL_COMFORT / FAIL_RULE / FAIL_SAFETY
  - 失效归因：perception_miss / prediction_error / planning_late / control_overshoot
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_THRESHOLDS = ROOT / "config" / "oracle_thresholds.json"


def load_thresholds(path: Path = DEFAULT_THRESHOLDS) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def compute_ttc_drac(
    ego_x: float, ego_y: float, ego_vx: float, ego_vy: float,
    obs_x: float, obs_y: float, obs_vx: float, obs_vy: float,
    ego_len: float = 4.6, obs_len: float = 4.6,
    ego_wid: float = 1.9, obs_wid: float = 1.9,
) -> tuple[float, float]:
    """Compute instantaneous TTC (s) and DRAC (m/s^2) along relative position."""
    dx = obs_x - ego_x
    dy = obs_y - ego_y
    dist_center = math.hypot(dx, dy)
    clearance = max(0.0, dist_center - 0.5 * (ego_len + obs_len))
    if dist_center < 1e-6:
        return 0.0, 99.0

    ux, uy = dx / dist_center, dy / dist_center
    # Closing speed (> 0 means approaching)
    v_close = (ego_vx - obs_vx) * ux + (ego_vy - obs_vy) * uy
    if v_close <= 0.05:
        return math.inf, 0.0
    # Lateral separation of the obstacle centre from the relative-motion line.
    # The old expression was identically zero (it was the perpendicular of the
    # same vector it was built from), so a car in a parallel lane was scored as
    # an in-path TTC conflict.
    rvx = ego_vx - obs_vx
    rvy = ego_vy - obs_vy
    rv = math.hypot(rvx, rvy)
    if rv > 1e-6:
        vx, vy = rvx / rv, rvy / rv
        lat_sep = abs(-vy * dx + vx * dy)
        if lat_sep > 0.5 * (ego_wid + obs_wid) + 0.2:
            return math.inf, 0.0
    if clearance <= 1e-4:
        return 0.0, 99.0
    ttc = clearance / v_close
    drac = (v_close * v_close) / (2.0 * clearance)
    return ttc, drac


def evaluate_frames(frames: list[dict[str, Any]], cfg: dict[str, Any]) -> dict[str, Any]:
    """Evaluate a sequence of simulation frames and return graded verdict + attribution."""
    safety_cfg = cfg["safety"]
    rule_cfg = cfg["rules"]
    comfort_cfg = cfg["comfort"]

    min_ttc = math.inf
    max_drac = 0.0
    min_pet = math.inf
    rss_violations = 0
    line_cross_events = 0
    red_light_events = 0
    overspeed_events = 0
    comfort_events = 0
    collision_events = 0

    max_jerk = 0.0
    max_lat_acc = 0.0
    max_steer_rate = 0.0
    attribution_counts: dict[str, int] = {
        "perception_miss": 0,
        "prediction_error": 0,
        "planning_late": 0,
        "control_overshoot": 0,
    }

    prev_t: float | None = None
    prev_ax: float | None = None
    prev_steer: float | None = None

    for fr in frames:
        t = float(fr.get("time_s", 0.0))
        ego = fr.get("ego") or {}
        ex = float(ego.get("x", 0.0))
        ey = float(ego.get("y", 0.0))
        ev = float(ego.get("speed_mps", 0.0))
        eh = float(ego.get("heading_rad", 0.0))
        evx = float(ego.get("vx", ev * math.cos(eh)))
        evy = float(ego.get("vy", ev * math.sin(eh)))
        ax = float(ego.get("ax", 0.0))
        ay = float(ego.get("ay", 0.0))
        steer = float(ego.get("steer_rad", 0.0))

        dt = (t - prev_t) if (prev_t is not None and t > prev_t) else None
        if dt is not None:
            if prev_ax is not None:
                jerk = abs(ax - prev_ax) / dt
                max_jerk = max(max_jerk, jerk)
                if jerk > comfort_cfg["max_lon_jerk_mps3"]:
                    comfort_events += 1
            if prev_steer is not None:
                srate = abs(steer - prev_steer) / dt
                max_steer_rate = max(max_steer_rate, srate)
                if srate > comfort_cfg["max_steer_rate_radps"]:
                    comfort_events += 1
        max_lat_acc = max(max_lat_acc, abs(ay))
        if abs(ay) > comfort_cfg["max_lat_accel_mps2"]:
            comfort_events += 1
        prev_t, prev_ax, prev_steer = t, ax, steer

        # Traffic rules
        if ego.get("collision", False):
            collision_events += 1
        line_offset = float(fr.get("solid_line_overlap_m", 0.0))
        if line_offset > rule_cfg["solid_line_cross_tol_m"]:
            line_cross_events += 1
            attribution_counts["control_overshoot"] += 1
        if fr.get("red_light_violation", False):
            red_light_events += 1
            attribution_counts["planning_late"] += 1
        speed_limit = fr.get("speed_limit_mps")
        if speed_limit is not None and ev > float(speed_limit) + rule_cfg["overspeed_margin_mps"]:
            overspeed_events += 1
            attribution_counts["control_overshoot"] += 1

        # Optional PET directly provided by conflict-zone tracker
        if "pet_s" in fr and fr["pet_s"] is not None:
            min_pet = min(min_pet, float(fr["pet_s"]))

        # Safety envelope vs obstacles
        perceived_ids = set(fr.get("perceived_obstacle_ids") or [])
        for obs in fr.get("obstacles") or []:
            ox = float(obs.get("x", 0.0))
            oy = float(obs.get("y", 0.0))
            ovx = float(obs.get("vx", 0.0))
            ovy = float(obs.get("vy", 0.0))
            ttc, drac = compute_ttc_drac(ex, ey, evx, evy, ox, oy, ovx, ovy)
            min_ttc = min(min_ttc, ttc)
            max_drac = max(max_drac, drac)

            # Attribution on any safety-critical envelope breach (TTC/DRAC/RSS)
            dx_ego = (ox - ex) * math.cos(eh) + (oy - ey) * math.sin(eh)
            dy_ego = -(ox - ex) * math.sin(eh) + (oy - ey) * math.cos(eh)
            rss_breach = (
                0.0 < dx_ego < safety_cfg["rss_min_lon_margin_m"] + 4.6
                and abs(dy_ego) < safety_cfg["rss_min_lat_margin_m"] + 1.9
            )
            if rss_breach:
                rss_violations += 1
            if rss_breach or ttc <= safety_cfg["ttc_fail_s"] or drac >= safety_cfg["drac_fail_mps2"]:
                oid = obs.get("id")
                if perceived_ids and oid is not None and oid not in perceived_ids:
                    attribution_counts["perception_miss"] += 1
                else:
                    attribution_counts["planning_late"] += 1

    # Graded verdict (worst severity wins)
    verdict = "PASS"
    if (
        collision_events > 0
        or min_ttc <= safety_cfg["ttc_fail_s"]
        or min_pet <= safety_cfg["pet_fail_s"]
        or max_drac >= safety_cfg["drac_fail_mps2"]
    ):
        verdict = "FAIL_SAFETY"
    elif line_cross_events > 0 or red_light_events > 0 or overspeed_events > 0:
        verdict = "FAIL_RULE"
    elif comfort_events > 0:
        verdict = "FAIL_COMFORT"
    elif (
        min_ttc <= safety_cfg["ttc_marginal_s"]
        or min_pet <= safety_cfg["pet_marginal_s"]
        or max_drac >= safety_cfg["drac_marginal_mps2"]
        or rss_violations > 0
    ):
        verdict = "MARGINAL"

    top_attribution = max(attribution_counts.items(), key=lambda kv: kv[1])
    primary_cause = top_attribution[0] if top_attribution[1] > 0 else "none"

    return {
        "schema": "flowengine.oracle_verdict.v1",
        "verdict": verdict,
        "primary_attribution": primary_cause,
        "attribution_counts": attribution_counts,
        "metrics": {
            "min_ttc_s": None if math.isinf(min_ttc) else round(min_ttc, 4),
            "min_pet_s": None if math.isinf(min_pet) else round(min_pet, 4),
            "max_drac_mps2": round(max_drac, 4),
            "rss_violations": rss_violations,
            "collision_events": collision_events,
            "line_cross_events": line_cross_events,
            "red_light_events": red_light_events,
            "overspeed_events": overspeed_events,
            "comfort_events": comfort_events,
            "max_lon_jerk_mps3": round(max_jerk, 4),
            "max_lat_accel_mps2": round(max_lat_acc, 4),
            "max_steer_rate_radps": round(max_steer_rate, 4),
        },
    }


def run_self_test() -> int:
    cfg = load_thresholds()
    # 1. PASS case
    r_pass = evaluate_frames([
        {"time_s": 0.0, "ego": {"x": 0, "y": 0, "vx": 8, "vy": 0, "ax": 0, "ay": 0.2, "steer_rad": 0.01},
         "obstacles": [{"id": 1, "x": 50, "y": 0, "vx": 8, "vy": 0}]},
        {"time_s": 0.1, "ego": {"x": 0.8, "y": 0, "vx": 8, "vy": 0, "ax": 0.1, "ay": 0.2, "steer_rad": 0.01},
         "obstacles": [{"id": 1, "x": 50.8, "y": 0, "vx": 8, "vy": 0}]},
    ], cfg)
    assert r_pass["verdict"] == "PASS", r_pass

    # 2. FAIL_RULE (solid line cross) + control_overshoot attribution
    r_rule = evaluate_frames([
        {"time_s": 0.0, "ego": {"x": 0, "y": 0, "vx": 5, "vy": 0}, "solid_line_overlap_m": 0.18}
    ], cfg)
    assert r_rule["verdict"] == "FAIL_RULE", r_rule
    assert r_rule["primary_attribution"] == "control_overshoot", r_rule

    # 3. FAIL_COMFORT
    r_comf = evaluate_frames([
        {"time_s": 0.0, "ego": {"x": 0, "y": 0, "vx": 10, "vy": 0, "ax": 0.0, "ay": 4.2}},
    ], cfg)
    assert r_comf["verdict"] == "FAIL_COMFORT", r_comf

    # 4. FAIL_SAFETY + perception_miss attribution
    r_safe = evaluate_frames([
        {"time_s": 0.0, "ego": {"x": 0, "y": 0, "vx": 15, "vy": 0},
         "perceived_obstacle_ids": [99],
         "obstacles": [{"id": 7, "x": 8.0, "y": 0, "vx": 0, "vy": 0}]},
    ], cfg)
    assert r_safe["verdict"] == "FAIL_SAFETY", r_safe
    assert r_safe["primary_attribution"] == "perception_miss", r_safe

    # 5. MARGINAL
    r_marg = evaluate_frames([
        {"time_s": 0.0, "ego": {"x": 0, "y": 0, "vx": 10, "vy": 0},
         "obstacles": [{"id": 2, "x": 20.0, "y": 0, "vx": 0, "vy": 0}]},
    ], cfg)
    assert r_marg["verdict"] == "MARGINAL", r_marg

    print("✓ oracle_evaluator --self-test PASSED (5/5 verdict grades + attributions)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", help="Path to frames JSON ({'frames': [...]})")
    ap.add_argument("--thresholds", default=str(DEFAULT_THRESHOLDS))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()
    if not args.input:
        ap.error("--input or --self-test is required")
    doc = json.loads(Path(args.input).read_text(encoding="utf-8"))
    frames = doc.get("frames") if isinstance(doc, dict) else doc
    res = evaluate_frames(frames or [], load_thresholds(Path(args.thresholds)))
    print(json.dumps(res, indent=2))
    return 0 if res["verdict"] in ("PASS", "MARGINAL") else 1


if __name__ == "__main__":
    sys.exit(main())
