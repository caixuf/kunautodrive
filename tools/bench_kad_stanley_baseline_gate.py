#!/usr/bin/env python3
"""KAD Prefer — Stanley/baseline fail-closed gate over product_suite_pack_v1 ×6.

Thin sibling of tools/bench_kad_product_suite_cte.py. Same frozen pack IDs;
documents controller=Stanley (default product lateral, use_ltv_mpc=0).
Plant cold-pack (tools/bench_kad_baseline_cte.py) stays UNTOUCHED.

Distinct from #115 product-suite lock: that CI-locks the product suite runner;
this Prefer adds a separate Stanley/baseline regression gate on the same pack×6.

Hard non-claims (dual-ceiling lock):
  - Does NOT claim cellular beat Stanley
  - Does NOT invent ≥12W / paste cellular W/L as product score
  - Cellular ADAS remains PARK
  - Pack CTE ≠ cellular win / beat-Stanley
  - No invent-scenario / 7th scene / control retune / plant rewrite

Usage:
  python3 tools/bench_kad_stanley_baseline_gate.py --dry-run
  FLOW_SKIP_BUILD=1 python3 tools/bench_kad_stanley_baseline_gate.py
  FLOW_SKIP_BUILD=1 python3 tools/bench_kad_stanley_baseline_gate.py \\
      --out artifacts/kad_stanley_baseline_gate.json
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK_DEFAULT = ROOT / "scenarios" / "packs" / "product_suite_pack_v1.json"
PRODUCT_SUITE = ROOT / "tools" / "bench_kad_product_suite_cte.py"
SCHEMA = "kad.stanley_baseline_gate.v1"
OUT_DEFAULT = ROOT / "artifacts" / "kad_stanley_baseline_gate.json"
CONTROLLER = (
    "Stanley (default product lateral; use_ltv_mpc=0 per control_node; "
    "no retune; no plant cold-pack rewrite)"
)

FROZEN_IDS = (
    "straight_road",
    "curve_road",
    "lane_change_traffic",
    "r157_cut_in",
    "oncoming",
    "dense_npc",
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
        "This Stanley/baseline gate is ready for later compare only — "
        "no beat-Stanley claim."
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


def load_pack(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("pack_id") != "product_suite_pack_v1":
        raise SystemExit(f"invalid pack (expect pack_id=product_suite_pack_v1): {path}")
    scenes = data.get("scenes")
    if not isinstance(scenes, list) or len(scenes) != 6:
        raise SystemExit(
            f"pack must freeze exactly 6 scenes, got "
            f"{len(scenes) if isinstance(scenes, list) else type(scenes)}"
        )
    ids = [s.get("pack_scene_id") for s in scenes if isinstance(s, dict)]
    if ids != list(FROZEN_IDS):
        raise SystemExit(f"pack freeze broken: got {ids} want {list(FROZEN_IDS)}")
    return data


def load_product_suite():
    if not PRODUCT_SUITE.is_file():
        raise RuntimeError(f"missing {PRODUCT_SUITE}")
    spec = importlib.util.spec_from_file_location("bench_kad_product_suite_cte", PRODUCT_SUITE)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {PRODUCT_SUITE}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["bench_kad_product_suite_cte"] = mod
    spec.loader.exec_module(mod)
    return mod


def stamp_stanley_artifact(product_payload: dict, pack: dict) -> dict:
    """Re-stamp product suite measure as Stanley/baseline gate artifact."""
    status = product_payload.get("status")
    scenes = product_payload.get("scenes") or []
    # Ensure every scene row documents Stanley controller.
    stamped_scenes = []
    for row in scenes:
        if not isinstance(row, dict):
            continue
        stamped = dict(row)
        stamped["controller"] = "Stanley"
        stamped_scenes.append(stamped)

    out = {
        "schema": SCHEMA,
        "status": status,
        "tip_hash": tip_hash(),
        "pack_id": pack["pack_id"],
        "pack_path": "scenarios/packs/product_suite_pack_v1.json",
        "controller": "Stanley",
        "controller_label": CONTROLLER,
        "gate_kind": "stanley_baseline_regression",
        "metric_primary": "cte_m",
        "metric_mapping": (
            "demo_evaluator lane_error meters ↔ ControlRaw.cte "
            "(docs/contracts/ADAS_CELLULAR_TRANSFER_INTERFACE_CONTRACT.md §5); "
            "product lateral = Stanley (use_ltv_mpc=0)"
        ),
        "dual_ceiling_cite": DUAL_CEILING,
        "plant_cold_pack": {
            "untouched": True,
            "harness": "tools/bench_kad_baseline_cte.py",
            "note": (
                "Plant cold-pack IDs (straight/lane_change/…) stay; "
                "this gate covers product_suite pack IDs only"
            ),
        },
        "distinct_from_product_suite_lock": {
            "pr": 115,
            "gate": "ci/gates/kad_product_suite_lock_check.py",
            "job": "kad-product-suite-lock-gate",
            "note": (
                "#115 CI-locks product suite runner ×6; this Prefer adds a "
                "separate fail-closed Stanley/baseline regression gate on the same pack×6"
            ),
        },
        "frozen_scene_count": 6,
        "frozen_ids": list(FROZEN_IDS),
        "frozen_scenes": pack["scenes"],
        "scenes": stamped_scenes,
        "ready_for_later_compare": True,
        "numbers_claimed": bool(product_payload.get("numbers_claimed"))
        and status == "ok",
        "non_claims": [
            "Does not claim cellular beat Stanley",
            "Does not invent ≥12W from ceilings",
            "Does not paste cellular W/L as KAD product score",
            "Cellular ADAS PARK — no champ / P0 / P1 in this harness",
            "Soft ≥10/16 ≠ closeout; gate ≥12W/4L never green from ceilings",
            "evaluator PASS/FAIL is regression-envelope — CTE digest ≠ ship win",
            "Plant cold-pack frozen 6 untouched",
            "Does not claim pack CTE = cellular win / beat-Stanley",
            "No invent-scenario / 7th scene / control retune",
        ],
    }
    if status != "ok":
        out["reason"] = product_payload.get("reason") or "product_suite_measure_not_ok"
        out["numbers_claimed"] = False
    return out


def build_blocked_dossier(pack: dict, reason: str) -> dict:
    return {
        "schema": SCHEMA,
        "status": "deliver_or_reason",
        "tip_hash": tip_hash(),
        "pack_id": pack["pack_id"],
        "pack_path": "scenarios/packs/product_suite_pack_v1.json",
        "controller": "Stanley",
        "controller_label": CONTROLLER,
        "gate_kind": "stanley_baseline_regression",
        "dual_ceiling_cite": DUAL_CEILING,
        "plant_cold_pack": {
            "untouched": True,
            "harness": "tools/bench_kad_baseline_cte.py",
        },
        "frozen_scene_count": 6,
        "frozen_ids": list(FROZEN_IDS),
        "frozen_scenes": pack["scenes"],
        "scenes": [],
        "reason": reason,
        "ready_for_later_compare": True,
        "numbers_claimed": False,
        "non_claims": [
            "Does not claim cellular beat Stanley",
            "Does not invent green JSON / gate greenwash",
            "Plant cold-pack untouched",
            "Pack freeze held — deliver-or-reason Prefer-closes",
        ],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="KAD Stanley/baseline gate over product_suite_pack_v1 ×6"
    )
    parser.add_argument("--pack", type=Path, default=PACK_DEFAULT)
    parser.add_argument("--out", type=Path, default=OUT_DEFAULT)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument(
        "--duration-override",
        type=int,
        default=None,
        help="optional short duration probe (NOT for claiming pack numbers)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="list frozen pack scenes + Stanley controller without running demo",
    )
    parser.add_argument(
        "--only",
        type=str,
        default=None,
        help="run only this pack_scene_id (probe; pack freeze file unchanged)",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=Path("/tmp/kad_stanley_baseline_gate"),
        help="per-scene evaluator json-out workspace",
    )
    args = parser.parse_args(argv)

    pack = load_pack(args.pack)
    tip = tip_hash()
    print(f"pack={pack['pack_id']} scenes=6 tip={tip[:12]}")
    print(f"controller=Stanley ({CONTROLLER})")
    print("dual-ceiling: #3@2922599a 9W/7L ceiling NOT win; "
          "#15@50431c48 7W/9L A6 CEILING PARK STOP+ask")
    print("plant cold-pack UNTOUCHED; cellular ADAS PARK; no beat-Stanley")

    if args.dry_run:
        for s in pack["scenes"]:
            print(
                f"  - {s['pack_scene_id']:<22} {s['duration_s']}s  "
                f"{s['file']}  controller=Stanley"
            )
        return 0

    ps = load_product_suite()
    run_pack = dict(pack)
    if args.only:
        run_pack["scenes"] = [
            s for s in pack["scenes"] if s["pack_scene_id"] == args.only
        ]
        if not run_pack["scenes"]:
            raise SystemExit(f"no pack scene matches --only {args.only!r}")
        print(f"--only filter → {args.only} (pack freeze file unchanged)")

    blockers = ps.preflight()
    args.results_dir.mkdir(parents=True, exist_ok=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("FLOW_SKIP_BUILD", "1")

    if blockers:
        dossier = build_blocked_dossier(
            pack, "preflight_blocked: " + "; ".join(blockers)
        )
        args.out.write_text(
            json.dumps(dossier, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        print(f"deliver-or-reason → {args.out}")
        for b in blockers:
            print(f"  BLOCK: {b}")
        return 0

    rows = []
    for entry in run_pack["scenes"]:
        rows.append(
            ps.run_one_scene(entry, args.interval, args.results_dir, args.duration_override)
        )

    if args.only is not None or args.duration_override is not None:
        product_payload = ps.build_dossier(
            pack,
            rows,
            (
                f"probe_only only={args.only!r} "
                f"duration_override={args.duration_override} — not full pack claim"
            ),
        )
    else:
        product_payload = ps.build_payload(pack, rows, full_pack_count=6)

    payload = stamp_stanley_artifact(product_payload, pack)
    args.out.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    print(f"\nschema={SCHEMA} status={payload['status']} tip={payload['tip_hash'][:12]}")
    print(f"controller={payload['controller']} → {args.out}")
    for row in rows:
        st = row.get("status")
        print(
            f"  [{st}] {row['pack_scene_id']}: "
            f"cte_mean={row.get('cte_mean')} cte_p95={row.get('cte_p95')} "
            f"cte_max={row.get('cte_max')} n={row.get('n_samples')} "
            f"eval={row.get('evaluator_result')} controller=Stanley"
        )
    if payload["status"] != "ok":
        print(f"deliver-or-reason: {payload.get('reason')}")
        print("numbers_claimed=false — no fake JSON wins / no gate greenwash")
    else:
        print(
            "numbers_claimed=true — ready_for_later_compare; "
            "no beat-Stanley / no ≥12W invent"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
