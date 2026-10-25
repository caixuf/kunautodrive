#!/usr/bin/env python3
"""KAD Prefer deepen — product_suite_pack_v1 → JSON CTE digest.

Thin pack runner for the locked suite product scenes (NOT the plant cold-pack).
Invokes in-tree ci/evaluators/demo_evaluator.py headlessly (demo.sh + flow_launcher)
per frozen pack scene, harvests lane_error series (evaluator geometric CTE proxy
aligned with ControlRaw.cte / B2-a §5), emits mean/p95/max.

Hard non-claims (dual-ceiling lock):
  - Does NOT claim cellular beat Stanley
  - Does NOT invent ≥12W / paste cellular W/L as product score
  - Cellular ADAS remains PARK
  - Plant cold-pack frozen 6 UNTOUCHED (tools/bench_kad_baseline_cte.py)

Usage:
  python3 tools/bench_kad_product_suite_cte.py --dry-run
  FLOW_SKIP_BUILD=1 python3 tools/bench_kad_product_suite_cte.py
  FLOW_SKIP_BUILD=1 python3 tools/bench_kad_product_suite_cte.py \\
      --out artifacts/kad_product_suite_cte.json
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK_DEFAULT = ROOT / "scenarios" / "packs" / "product_suite_pack_v1.json"
EVALUATOR = ROOT / "ci" / "evaluators" / "demo_evaluator.py"
SCHEMA = "kad.product_suite_cte.v1"
OUT_DEFAULT = ROOT / "artifacts" / "kad_product_suite_cte.json"

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
        "This digest is ready for later compare only — no beat-Stanley claim."
    ),
}


def tip_hash() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=str(ROOT),
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "UNKNOWN"



def prepare_pack_workspace(results_dir: Path, key: str) -> tuple[Path, dict[str, str]]:
    """Isolate monitor state_file like ci/evaluators/scenario_regression.py.

    FLOW_TOPOLOGY_FILE alone is not enough — monitor writes state_file from
    pipeline params. Patch a per-scene pipeline copy so samples land where the
    evaluator polls.
    """
    workspace = results_dir / ".workers" / key
    workspace.mkdir(parents=True, exist_ok=True)
    pipeline_path = workspace / "pipeline.json"
    src = ROOT / "config" / "pipeline.json"
    pipeline = json.loads(src.read_text(encoding="utf-8"))
    if not isinstance(pipeline, dict):
        raise RuntimeError(f"invalid pipeline: {src}")
    for node in pipeline.get("processes", pipeline.get("nodes", [])):
        if not isinstance(node, dict) or node.get("name") != "monitor":
            continue
        params = node.get("params", {})
        if isinstance(params, str):
            params_obj = json.loads(params)
            params_obj["state_file"] = str(workspace / "topology.json")
            node["params"] = json.dumps(params_obj, separators=(",", ":"))
        elif isinstance(params, dict):
            params["state_file"] = str(workspace / "topology.json")
        break
    pipeline_path.write_text(json.dumps(pipeline, indent=2, ensure_ascii=False) + "\n",
                             encoding="utf-8")
    env = {
        "FLOW_PIPELINE": str(pipeline_path),
        "FLOWENGINE_TEMP_DIR": str(workspace),
        "FLOW_BUILD_LOCK": str(results_dir / ".build.lock"),
        "FLOW_TOPOLOGY_FILE": str(workspace / "topology.json"),
        "FLOW_LOG_DIR": str(workspace / "logs"),
        "FLOWENGINE_DEMO_PID_FILE": str(workspace / "demo.pids"),
        "FLOW_LAUNCHER_STDOUT": str(workspace / "launcher.stdout"),
        "FLOW_LAUNCHER_STDERR": str(workspace / "launcher.stderr"),
        "FLOWMOND_LOG": str(workspace / "flowmond.log"),
        "FOXGLOVE_LOG": str(workspace / "foxglove.log"),
        "FLOW_ENVIRONMENT_FILE": str(workspace / "flow_environment.json"),
        "FLOW_BEH_LOG": str(workspace / "behavior.log"),
        "FLOW_SKIP_SERVICES": "1",
        "FLOW_SKIP_GLOBAL_CLEANUP": "1",
        "FLOW_SKIP_BUILD": os.environ.get("FLOW_SKIP_BUILD", "1"),
    }
    Path(env["FLOW_LOG_DIR"]).mkdir(parents=True, exist_ok=True)
    return workspace, env


def load_pack(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("pack_id") != "product_suite_pack_v1":
        raise SystemExit(f"invalid pack (expect pack_id=product_suite_pack_v1): {path}")
    scenes = data.get("scenes")
    if not isinstance(scenes, list) or len(scenes) != 6:
        raise SystemExit(f"pack must freeze exactly 6 scenes, got {type(scenes)} len="
                         f"{len(scenes) if isinstance(scenes, list) else 'n/a'}")
    return data


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
    abs_s = [abs(float(x)) for x in samples]
    if not abs_s:
        return {"cte_mean": None, "cte_p95": None, "cte_max": None, "n_samples": 0}
    abs_s.sort()
    mean = sum(abs_s) / len(abs_s)
    return {
        "cte_mean": round(mean, 6),
        "cte_p95": round(_pct(abs_s, 95), 6),
        "cte_max": round(abs_s[-1], 6),
        "n_samples": len(abs_s),
    }


def load_demo_evaluator():
    if not EVALUATOR.is_file():
        raise RuntimeError(f"missing {EVALUATOR}")
    spec = importlib.util.spec_from_file_location("demo_evaluator", EVALUATOR)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {EVALUATOR}")
    mod = importlib.util.module_from_spec(spec)
    # demo_evaluator expects to live under ci/evaluators for relative imports — none today,
    # but exec from file path is enough for sample_metrics helpers.
    sys.modules["demo_evaluator"] = mod
    spec.loader.exec_module(mod)
    return mod


def harvest_lane_errors(ev, samples: list, scenario: dict | None) -> list[float]:
    """Reuse demo_evaluator.sample_metrics lane_error (geometric CTE proxy)."""
    road = None
    if isinstance(scenario, dict):
        road = scenario.get("road_network")
    out: list[float] = []
    for raw in samples:
        try:
            metric = ev.sample_metrics(
                raw, road=road, scenario_road_network=road
            )
        except TypeError:
            metric = ev.sample_metrics(raw, road=road)
        if not isinstance(metric, dict):
            continue
        le = metric.get("lane_error")
        if le is None:
            continue
        out.append(float(le))
    return out


def run_one_scene(entry: dict, interval: float, results_dir: Path,
                  duration_override: int | None) -> dict:
    sid = entry["pack_scene_id"]
    scen_file = entry["file"]
    duration = int(duration_override if duration_override is not None else entry["duration_s"])
    out_path = results_dir / f"{sid}.json"
    workspace, worker_env = prepare_pack_workspace(results_dir, sid)
    topology = Path(worker_env["FLOW_TOPOLOGY_FILE"])
    cmd = [
        sys.executable, str(EVALUATOR),
        "--scenario", scen_file,
        "--duration", str(duration),
        "--interval", str(interval),
        "--json-file", str(topology),
        "--json-out", str(out_path),
    ]
    env = os.environ.copy()
    env.update(worker_env)
    env.setdefault("FLOW_SKIP_BUILD", "1")

    print(f"\n─── pack scene '{sid}' ({duration}s) file={scen_file} ───", flush=True)
    t0 = time.time()
    try:
        proc = subprocess.run(cmd, cwd=str(ROOT), env=env, timeout=duration + 420)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        return {
            "pack_scene_id": sid,
            "file": scen_file,
            "duration_s_requested": duration,
            "status": "blocked",
            "block_reason": f"demo_evaluator timed out after {duration + 420}s",
            "cte_mean": None,
            "cte_p95": None,
            "cte_max": None,
            "n_samples": 0,
            "evaluator_result": None,
            "wall_s": round(time.time() - t0, 3),
        }

    if not out_path.is_file():
        return {
            "pack_scene_id": sid,
            "file": scen_file,
            "duration_s_requested": duration,
            "status": "blocked",
            "block_reason": f"no json-out (exit={rc})",
            "cte_mean": None,
            "cte_p95": None,
            "cte_max": None,
            "n_samples": 0,
            "evaluator_result": None,
            "wall_s": round(time.time() - t0, 3),
        }

    payload = json.loads(out_path.read_text(encoding="utf-8"))
    samples = payload.get("samples") or []
    summary = payload.get("summary") or {}
    scen_path = ROOT / scen_file
    scenario_obj = None
    if scen_path.is_file():
        try:
            scenario_obj = json.loads(scen_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            scenario_obj = None

    ev = load_demo_evaluator()
    lane_errors = harvest_lane_errors(ev, samples, scenario_obj)
    stats = cte_stats(lane_errors)

    # Fallback: if sample_metrics path yielded nothing, use summary max + ADE mean only
    # (still honest — mark incomplete p95 rather than invent).
    if stats["n_samples"] == 0:
        ade = None
        fm = summary.get("formal_metrics") if isinstance(summary, dict) else None
        if isinstance(fm, dict):
            ade = fm.get("trajectory_ade_m")
        mx = summary.get("max_lane_error_m") if isinstance(summary, dict) else None
        if ade is not None or mx is not None:
            stats = {
                "cte_mean": round(float(ade), 6) if ade is not None else None,
                "cte_p95": None,
                "cte_max": round(float(mx), 6) if mx is not None else None,
                "n_samples": 0,
            }
            harvest_note = "fallback_summary_ade_max_only (no per-sample lane_error)"
        else:
            harvest_note = "no_lane_error_series"
    else:
        harvest_note = "demo_evaluator.sample_metrics.lane_error"

    return {
        "pack_scene_id": sid,
        "file": scen_file,
        "duration_s_requested": duration,
        "duration_s_observed": summary.get("duration_s"),
        "status": "ok" if stats["cte_mean"] is not None or stats["cte_max"] is not None else "blocked",
        "block_reason": None if (stats["cte_mean"] is not None or stats["cte_max"] is not None)
        else "could not harvest CTE series or summary fallback",
        "cte_mean": stats["cte_mean"],
        "cte_p95": stats["cte_p95"],
        "cte_max": stats["cte_max"],
        "n_samples": stats["n_samples"],
        "cte_definition": (
            "demo_evaluator lane_error meters (scene road_network / nearest-lane "
            "geometric proxy) ↔ ControlRaw.cte (B2-a §5)"
        ),
        "harvest": harvest_note,
        "evaluator_result": payload.get("result"),
        "evaluator_failures": payload.get("failures") or [],
        "summary_max_lane_error_m": summary.get("max_lane_error_m"),
        "wall_s": round(time.time() - t0, 3),
        "evaluator_exit": rc,
    }


def build_dossier(pack: dict, rows: list, reason: str) -> dict:
    return {
        "schema": SCHEMA,
        "status": "deliver_or_reason",
        "tip_hash": tip_hash(),
        "pack_id": pack["pack_id"],
        "pack_path": str(PACK_DEFAULT.relative_to(ROOT)),
        "frozen_scene_count": pack["frozen_scene_count"],
        "frozen_scenes": pack["scenes"],
        "dual_ceiling_cite": DUAL_CEILING,
        "plant_cold_pack": pack.get("plant_cold_pack"),
        "reason": reason,
        "scenes": rows,
        "ready_for_later_compare": True,
        "non_claims": pack.get("non_claims") or [],
        "numbers_claimed": False,
    }


def build_payload(pack: dict, rows: list, full_pack_count: int = 6) -> dict:
    if len(rows) != full_pack_count:
        return build_dossier(
            pack,
            rows,
            f"incomplete_pack_run: harvested {len(rows)}/{full_pack_count} "
            f"(use full pack run for numbers; --only/--duration-override are probes)",
        )
    blocked = [r for r in rows if r.get("status") != "ok"]
    if blocked:
        return build_dossier(
            pack,
            rows,
            "one_or_more_pack_scenes_blocked: " + ", ".join(
                f"{r['pack_scene_id']}({r.get('block_reason')})" for r in blocked
            ),
        )
    return {
        "schema": SCHEMA,
        "status": "ok",
        "tip_hash": tip_hash(),
        "pack_id": pack["pack_id"],
        "pack_path": "scenarios/packs/product_suite_pack_v1.json",
        "controller_label": (
            "product pipeline via demo_evaluator/demo.sh/flow_launcher "
            "(no control retune; no plant cold-pack rewrite)"
        ),
        "metric_primary": "cte_m",
        "metric_mapping": (
            "demo_evaluator lane_error meters ↔ ControlRaw.cte "
            "(docs/contracts/ADAS_CELLULAR_TRANSFER_INTERFACE_CONTRACT.md §5)"
        ),
        "dual_ceiling_cite": DUAL_CEILING,
        "plant_cold_pack": pack.get("plant_cold_pack"),
        "frozen_scene_count": len(pack["scenes"]),
        "frozen_scenes": pack["scenes"],
        "scenes": rows,
        "ready_for_later_compare": True,
        "numbers_claimed": True,
        "non_claims": [
            "Does not claim cellular beat Stanley",
            "Does not invent ≥12W from ceilings",
            "Does not paste cellular W/L as KAD product score",
            "Cellular ADAS PARK — no champ / P0 / P1 in this harness",
            "Soft ≥10/16 ≠ closeout; gate ≥12W/4L never green from ceilings",
            "evaluator PASS/FAIL is regression-envelope — CTE digest ≠ ship win",
            "Plant cold-pack frozen 6 untouched",
        ],
    }


def preflight() -> list[str]:
    blockers = []
    if not EVALUATOR.is_file():
        blockers.append(f"missing {EVALUATOR}")
    demo = ROOT / "scripts" / "demo.sh"
    if not demo.is_file():
        blockers.append(f"missing {demo}")
    launcher = ROOT / "build" / "bin" / "flow_launcher"
    if not launcher.is_file():
        blockers.append(f"missing {launcher} (need built flow_launcher for headless pack run)")
    return blockers


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="KAD product_suite_pack_v1 → JSON CTE (mean/p95/max)"
    )
    parser.add_argument("--pack", type=Path, default=PACK_DEFAULT)
    parser.add_argument("--out", type=Path, default=OUT_DEFAULT)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument(
        "--duration-override",
        type=int,
        default=None,
        help="optional short duration for stack probe (NOT for claiming pack numbers)",
    )
    parser.add_argument("--dry-run", action="store_true",
                        help="list frozen pack scenes without running demo")
    parser.add_argument(
        "--only",
        type=str,
        default=None,
        help="run only this pack_scene_id (stack probe / debug; still no silent drop of pack freeze)",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=Path("/tmp/kad_product_suite_cte"),
        help="per-scene evaluator json-out workspace",
    )
    args = parser.parse_args(argv)

    pack = load_pack(args.pack)
    full_pack_count = int(pack.get("frozen_scene_count") or len(pack["scenes"]))
    tip = tip_hash()
    print(f"pack={pack['pack_id']} scenes={pack['frozen_scene_count']} tip={tip[:12]}")
    print("dual-ceiling: #3@2922599a 9W/7L ceiling NOT win; "
          "#15@50431c48 7W/9L A6 CEILING PARK STOP+ask")
    print("plant cold-pack UNTOUCHED; cellular ADAS PARK")

    if args.only:
        pack = dict(pack)
        pack["scenes"] = [s for s in pack["scenes"] if s["pack_scene_id"] == args.only]
        if not pack["scenes"]:
            raise SystemExit(f"no pack scene matches --only {args.only!r}")
        print(f"--only filter → {args.only} (pack freeze file unchanged)")

    if args.dry_run:
        for s in pack["scenes"]:
            print(f"  - {s['pack_scene_id']:<22} {s['duration_s']}s  {s['file']}")
        return 0

    blockers = preflight()
    args.results_dir.mkdir(parents=True, exist_ok=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)

    if blockers:
        dossier = build_dossier(
            pack,
            [],
            "preflight_blocked: " + "; ".join(blockers),
        )
        args.out.write_text(json.dumps(dossier, indent=2, ensure_ascii=False) + "\n",
                            encoding="utf-8")
        print(f"deliver-or-reason → {args.out}")
        for b in blockers:
            print(f"  BLOCK: {b}")
        return 0  # Prefer closes with honest dossier; pack freeze is the knife

    rows = []
    for entry in pack["scenes"]:
        rows.append(run_one_scene(entry, args.interval, args.results_dir,
                                  args.duration_override))

    if args.duration_override is not None:
        payload = build_dossier(
            pack,
            rows,
            f"duration_override={args.duration_override} probe only — not suite "
            f"duration_s numbers (no claim)",
        )
    else:
        payload = build_payload(pack, rows, full_pack_count=full_pack_count)
    args.out.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")

    print(f"\nschema={SCHEMA} status={payload['status']} tip={payload['tip_hash'][:12]}")
    print(f"→ {args.out}")
    for row in rows:
        st = row.get("status")
        print(
            f"  [{st}] {row['pack_scene_id']}: "
            f"cte_mean={row.get('cte_mean')} cte_p95={row.get('cte_p95')} "
            f"cte_max={row.get('cte_max')} n={row.get('n_samples')} "
            f"eval={row.get('evaluator_result')}"
        )
    if payload["status"] != "ok":
        print(f"deliver-or-reason: {payload.get('reason')}")
        print("numbers_claimed=false — pack freeze still lands; no fake JSON wins")
    else:
        print("numbers_claimed=true — ready_for_later_compare; no beat-Stanley / no ≥12W invent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
