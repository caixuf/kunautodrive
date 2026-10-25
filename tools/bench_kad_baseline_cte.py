#!/usr/bin/env python3
"""KAD B2-b baseline harness stub — Stanley / product lateral × frozen scenes → JSON CTE.

Thin wrap of tools/control_sim.py (pure-Python plant aligned with product Stanley /
optional LTV-MPC). Emits ControlRaw.cte-equivalent meters (control_sim lat_error /
curve lateral_offset) for a frozen scene list. Ready for later compare.

Hard non-claims (locked by dual-ceiling):
  - Does NOT claim cellular beat Stanley
  - Does NOT invent ≥12W / paste cellular W/L as product score
  - Cellular ADAS remains PARK; this is KAD-internal baseline only

Usage:
  python3 tools/bench_kad_baseline_cte.py
  python3 tools/bench_kad_baseline_cte.py --compare-mpc
  python3 tools/bench_kad_baseline_cte.py --out artifacts/kad_baseline_cte.json
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTROL_SIM_PATH = ROOT / "tools" / "control_sim.py"
SCHEMA = "kad.baseline_cte.v1"

# Frozen scene list (B2-b lock). IDs + params frozen in handoff; do not silent-drop.
# Source: in-tree tools/control_sim.py demo scenes (not cellular 16-scene dialect).
FROZEN_SCENES = (
    {
        "scene_id": "straight",
        "source": "control_sim.run_simulation",
        "params": {
            "do_lane_change": False,
            "target_speed": 12.0,
            "duration": 20.0,
            "seed": None,
        },
        "notes": "Straight lane-keep; Stanley default product lateral path",
    },
    {
        "scene_id": "lane_change",
        "source": "control_sim.run_simulation",
        "params": {
            "do_lane_change": True,
            "target_speed": 12.0,
            "duration": 20.0,
            "seed": None,
        },
        "notes": "Step lane-change at LC_TRIGGER_TIME; lat_error vs target lane",
    },
    {
        "scene_id": "curve",
        "source": "control_sim.run_curve_scenario",
        "params": {
            "kappa": 0.005,
            "target_speed": 12.0,
            "duration": 15.0,
            "seed": None,
        },
        "notes": "Constant-curvature R≈200m; lateral_offset via circle-distance",
    },
    {
        "scene_id": "obstacle",
        "source": "control_sim.run_obstacle_avoid_scenario",
        "params": {
            "init_speed": 12.0,
            "obs_x": 60.0,
            "duration": 10.0,
            "seed": None,
        },
        "notes": "Obstacle avoid then return; CTE = lane_center_y(2) - y (start-lane proxy)",
    },
    {
        "scene_id": "merge",
        "source": "control_sim.run_merge_scenario",
        "params": {
            "init_speed": 8.0,
            "target_speed": 15.0,
            "merge_x": 50.0,
            "seed": None,
        },
        "notes": "Ramp merge; CTE = lane_center_y(1) - y (merge target)",
    },
    {
        "scene_id": "cutin",
        "source": "control_sim.run_cutin_scenario",
        "params": {
            "init_speed": 12.0,
            "cutin_speed": 10.0,
            "cutin_x": 30.0,
            "seed": None,
        },
        "notes": "Cut-in response; CTE = lane_center_y(2) - y (ego lane)",
    },
)

DUAL_CEILING = {
    "lock_w1": {
        "cellular_pr": 3,
        "tip": "2922599a",
        "wl": "9W/7L",
        "label": "ceiling evidence, NOT a win",
    },
    "a6_reopen": {
        "cellular_pr": 15,
        "tip": "50431c48",
        "wl": "7W/9L",
        "label": "A6 CEILING · PARK · STOP+ask",
        "champ_md5": "7424dd1115b60c675aded7a59efae30a",
    },
    "note": (
        "Cellular cortex vs Stanley = ceiling/PARK, not a KAD product win. "
        "Gate ≥12W/4L never green from ceilings. Soft ≥10/16 ≠ closeout. "
        "This harness is ready for later compare only — no beat-Stanley claim."
    ),
}


def load_control_sim():
    spec = importlib.util.spec_from_file_location("control_sim", CONTROL_SIM_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {CONTROL_SIM_PATH}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def tip_hash() -> str:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=str(ROOT),
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        return out
    except (OSError, subprocess.CalledProcessError):
        return "UNKNOWN"


def _pct(sorted_vals, p):
    if not sorted_vals:
        return None
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    k = (len(sorted_vals) - 1) * (p / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return sorted_vals[int(k)]
    return sorted_vals[f] * (c - k) + sorted_vals[c] * (k - f)


def cte_stats(samples):
    """samples: iterable of signed or abs CTE in meters → mean / p95 / max of abs."""
    abs_s = [abs(float(x)) for x in samples]
    if not abs_s:
        return {
            "cte_mean": None,
            "cte_p95": None,
            "cte_max": None,
            "n_samples": 0,
        }
    abs_s.sort()
    mean = sum(abs_s) / len(abs_s)
    return {
        "cte_mean": round(mean, 6),
        "cte_p95": round(_pct(abs_s, 95), 6),
        "cte_max": round(abs_s[-1], 6),
        "n_samples": len(abs_s),
    }


def _lane_center_y(cs, lane_idx):
    return cs.lane_center_y(lane_idx)


def run_straight_or_lc(cs, use_mpc: bool, do_lane_change: bool, params: dict):
    result = cs.run_simulation(
        use_mpc=use_mpc,
        do_lane_change=do_lane_change,
        target_speed=params["target_speed"],
        duration=params["duration"],
    )
    stats = cte_stats(result.lat_error)
    ok = (not result.collided) and (result.steady_state_error is not None)
    if do_lane_change:
        ok = ok and (result.settling_time is not None)
    else:
        ok = ok and bool(result.stable)
    return stats, ok, {
        "collided": bool(result.collided),
        "stable": bool(result.stable),
        "steady_state_error_m": result.steady_state_error,
        "settling_time_s": result.settling_time,
    }


def run_curve(cs, use_mpc: bool, params: dict):
    kappa = params["kappa"]
    result = cs.run_curve_scenario(
        kappa=kappa,
        target_speed=params["target_speed"],
        duration=params["duration"],
        use_mpc=use_mpc,
    )
    # Recompute lateral_offset series (same circle-distance as control_sim eval).
    R = 1.0 / kappa if abs(kappa) > 1e-9 else float("inf")
    circle_cy = R if abs(kappa) > 1e-9 else 0.0
    samples = []
    for i in range(len(result.t)):
        if abs(kappa) > 1e-9:
            dc = math.sqrt(
                result.x[i] * result.x[i]
                + (result.y[i] - circle_cy) * (result.y[i] - circle_cy)
            )
            samples.append(dc - R)
        else:
            samples.append(result.y[i])
    stats = cte_stats(samples)
    return stats, bool(result.success), {
        "off_road": bool(result.off_road),
        "summary": result.summary,
    }


def run_obstacle(cs, use_mpc: bool, params: dict):
    stock = cs.run_obstacle_avoid_scenario(
        init_speed=params["init_speed"],
        obs_x=params["obs_x"],
        duration=params["duration"],
        use_mpc=use_mpc,
    )
    # Post-hoc CTE vs start lane center (lane 2). During avoid the active
    # target moves; this reports deviation from original lane as a documented proxy.
    ref_y = _lane_center_y(cs, 2)
    samples = [ref_y - y for y in stock.y]
    stats = cte_stats(samples)
    return stats, bool(stock.success), {
        "collision": bool(stock.collision),
        "off_road": bool(stock.off_road),
        "summary": stock.summary,
        "cte_definition": "lane_center_y(2) - ego_y (start-lane proxy during avoid)",
    }


def run_merge(cs, use_mpc: bool, params: dict):
    stock = cs.run_merge_scenario(
        init_speed=params["init_speed"],
        target_speed=params["target_speed"],
        merge_x=params["merge_x"],
        use_mpc=use_mpc,
    )
    # Post-hoc: CTE vs merge target lane (default merge_lane=1).
    ref_y = _lane_center_y(cs, 1)
    samples = [ref_y - y for y in stock.y]
    stats = cte_stats(samples)
    return stats, bool(stock.success), {
        "collision": bool(stock.collision),
        "off_road": bool(stock.off_road),
        "summary": stock.summary,
        "cte_definition": "lane_center_y(1) - ego_y (merge target lane)",
    }


def run_cutin(cs, use_mpc: bool, params: dict):
    stock = cs.run_cutin_scenario(
        init_speed=params["init_speed"],
        cutin_speed=params["cutin_speed"],
        cutin_x=params["cutin_x"],
        use_mpc=use_mpc,
    )
    # Ego stays in lane 2; CTE vs lane_center_y(2).
    ref_y = _lane_center_y(cs, 2)
    samples = [ref_y - y for y in stock.y]
    stats = cte_stats(samples)
    return stats, bool(stock.success), {
        "collision": bool(stock.collision),
        "off_road": bool(stock.off_road),
        "summary": stock.summary,
        "cte_definition": "lane_center_y(2) - ego_y (ego-lane keep)",
    }


RUNNERS = {
    "straight": lambda cs, mpc, p: run_straight_or_lc(cs, mpc, False, p),
    "lane_change": lambda cs, mpc, p: run_straight_or_lc(cs, mpc, True, p),
    "curve": run_curve,
    "obstacle": run_obstacle,
    "merge": run_merge,
    "cutin": run_cutin,
}


def run_suite(cs, use_mpc: bool):
    controller = "LTV-MPC" if use_mpc else "Stanley"
    rows = []
    for spec in FROZEN_SCENES:
        sid = spec["scene_id"]
        stats, ok, extra = RUNNERS[sid](cs, use_mpc, spec["params"])
        rows.append(
            {
                "scene_id": sid,
                "source": spec["source"],
                "params": spec["params"],
                "controller": controller,
                "cte_mean": stats["cte_mean"],
                "cte_p95": stats["cte_p95"],
                "cte_max": stats["cte_max"],
                "n_samples": stats["n_samples"],
                "pass": ok,
                "extra": extra,
            }
        )
    return rows


def compare_wl(stanley_rows, mpc_rows):
    """Optional W/L vs named KAD-internal mode only (Stanley vs LTV-MPC).

    Win = lower cte_mean (abs) without failing when the other passes.
    Not a cellular scoreboard. Not a product ship claim.
    """
    by_s = {r["scene_id"]: r for r in stanley_rows}
    by_m = {r["scene_id"]: r for r in mpc_rows}
    per = []
    sw = sl = mw = ml = tie = 0
    for sid in by_s:
        s, m = by_s[sid], by_m[sid]
        s_cte, m_cte = s["cte_mean"], m["cte_mean"]
        if s_cte is None or m_cte is None:
            verdict = "incomplete"
        elif s["pass"] and not m["pass"]:
            verdict = "Stanley"
            sw += 1
            ml += 1
        elif m["pass"] and not s["pass"]:
            verdict = "LTV-MPC"
            mw += 1
            sl += 1
        elif s_cte < m_cte - 1e-9:
            verdict = "Stanley"
            sw += 1
            ml += 1
        elif m_cte < s_cte - 1e-9:
            verdict = "LTV-MPC"
            mw += 1
            sl += 1
        else:
            verdict = "tie"
            tie += 1
        per.append(
            {
                "scene_id": sid,
                "stanley_cte_mean": s_cte,
                "mpc_cte_mean": m_cte,
                "stanley_pass": s["pass"],
                "mpc_pass": m["pass"],
                "lower_cte": verdict,
            }
        )
    return {
        "baseline_mode_a": "Stanley",
        "baseline_mode_b": "LTV-MPC",
        "scope": "KAD-internal control_sim only — NOT cellular W/L",
        "stanley_wl": f"{sw}W/{sl}L",
        "mpc_wl": f"{mw}W/{ml}L",
        "ties": tie,
        "per_scene": per,
        "claim": (
            "Optional KAD-internal Stanley vs LTV-MPC compare only. "
            "Not a cellular scoreboard. Not evidence of beat-Stanley / ≥12W."
        ),
    }


def build_payload(stanley_rows, mpc_rows=None):
    payload = {
        "schema": SCHEMA,
        "tip_hash": tip_hash(),
        "controller_label": "Stanley (default product lateral in control_sim; no retune)",
        "metric_primary": "cte_m",
        "metric_mapping": (
            "control_sim lat_error / lateral_offset meters ↔ ControlRaw.cte "
            "(see docs/contracts/ADAS_CELLULAR_TRANSFER_INTERFACE_CONTRACT.md §5)"
        ),
        "dual_ceiling_cite": DUAL_CEILING,
        "frozen_scene_count": len(FROZEN_SCENES),
        "frozen_scenes": [
            {
                "scene_id": s["scene_id"],
                "source": s["source"],
                "params": s["params"],
                "notes": s["notes"],
            }
            for s in FROZEN_SCENES
        ],
        "scenes": stanley_rows,
        "ready_for_later_compare": True,
        "non_claims": [
            "Does not claim cellular beat Stanley",
            "Does not invent ≥12W from ceilings",
            "Does not paste cellular W/L as KAD product score",
            "Cellular ADAS PARK — no champ / P0 / P1 in this harness",
            "Soft ≥10/16 ≠ closeout; gate ≥12W/4L never green from ceilings",
        ],
    }
    if mpc_rows is not None:
        payload["scenes_mpc"] = mpc_rows
        payload["optional_wl_vs_named_kad_mode"] = compare_wl(stanley_rows, mpc_rows)
    return payload


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="KAD B2-b baseline CTE harness stub (Stanley × frozen scenes)"
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=ROOT / "artifacts" / "kad_baseline_cte.json",
        help="JSON output path (default: artifacts/kad_baseline_cte.json)",
    )
    parser.add_argument(
        "--compare-mpc",
        action="store_true",
        help="Also run LTV-MPC and emit optional W/L vs named KAD-internal mode",
    )
    args = parser.parse_args(argv)

    if not CONTROL_SIM_PATH.is_file():
        print(f"ERROR: missing {CONTROL_SIM_PATH}", file=sys.stderr)
        return 2

    cs = load_control_sim()
    stanley_rows = run_suite(cs, use_mpc=False)
    mpc_rows = run_suite(cs, use_mpc=True) if args.compare_mpc else None
    payload = build_payload(stanley_rows, mpc_rows)

    out: Path = args.out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(f"schema={SCHEMA} tip={payload['tip_hash'][:12]} controller=Stanley")
    print(f"frozen_scenes={len(FROZEN_SCENES)} → {out}")
    for row in stanley_rows:
        status = "PASS" if row["pass"] else "FAIL"
        print(
            f"  [{status}] {row['scene_id']}: "
            f"cte_mean={row['cte_mean']} cte_p95={row['cte_p95']} cte_max={row['cte_max']}"
        )
    if mpc_rows is not None:
        wl = payload["optional_wl_vs_named_kad_mode"]
        print(
            f"optional KAD-internal W/L: Stanley {wl['stanley_wl']} vs "
            f"LTV-MPC {wl['mpc_wl']} (NOT cellular; NOT a ship win)"
        )
    print("dual-ceiling: #3@2922599a 9W/7L ceiling NOT win; "
          "#15@50431c48 7W/9L A6 CEILING PARK STOP+ask")
    print("ready_for_later_compare=true — no beat-Stanley / no ≥12W invent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
