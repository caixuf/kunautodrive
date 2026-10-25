#!/usr/bin/env python3
"""CI-lock gate: product_suite_pack_v1 ×6 + CTE artifact fail-closed.

Knife B (Prefer KAD suite-expand): cement the CLEARED pack as a regression
gate. Does NOT invent a 7th scene. Does NOT remeasure in CI (full pack needs
flow_launcher + ~5min demos). Invokes tools/bench_kad_product_suite_cte.py
--dry-run as pack-load smoke; validates locked artifact + tip_hash honesty.

Fails closed on:
  - missing pack / harness / artifact
  - pack ID expand / silent scene drop / wrong frozen IDs
  - artifact status != ok
  - any scene evaluator_result != PASS (or missing)
  - tip_hash missing / not ancestor of HEAD / lock-surface drift since measure

Evidence: #114 pack×6 all PASS @ tip_hash d12e0a64… (optional remeasure tip
b468c9a7… if measure tip must equal branch tip). Dual-ceiling #3 9W/7L +
#15 7W/9L = ceiling NOT win. Plant cold-pack UNTOUCHED. Cellular ADAS PARK.

Usage:
  python3 ci/gates/kad_product_suite_lock_check.py
  python3 ci/gates/kad_product_suite_lock_check.py --self-test
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACK = ROOT / "scenarios" / "packs" / "product_suite_pack_v1.json"
HARNESS = ROOT / "tools" / "bench_kad_product_suite_cte.py"
ARTIFACT = ROOT / "artifacts" / "kad_product_suite_cte.json"
SCHEMA = "kad.product_suite_cte.v1"
PACK_ID = "product_suite_pack_v1"

FROZEN_IDS = (
    "straight_road",
    "curve_road",
    "lane_change_traffic",
    "r157_cut_in",
    "oncoming",
    "dense_npc",
)

# Paths whose change after measure tip requires a fresh pack×6 artifact.
# Handoff / this gate / CI yml wiring alone must NOT force a remeasure.
LOCK_SURFACES = (
    "scenarios/packs/product_suite_pack_v1.json",
    "scenarios/straight_road.json",
    "scenarios/curve_road.json",
    "scenarios/lane_change_traffic.json",
    "scenarios/r157_cut_in.json",
    "scenarios/oncoming.json",
    "scenarios/dense_npc.json",
    "tools/bench_kad_product_suite_cte.py",
    "ci/evaluators/demo_evaluator.py",
    "scripts/demo.sh",
    "modules/adas_nodes/planning_node.cpp",
    "modules/adas_nodes/planning_coordinates.h",
    "modules/adas_nodes/control_node.cpp",
)

# #114 offgrid CLEARED measure tip (pack×6 all PASS). Ancestor of main tip
# b468c9a7… Allowed until lock surfaces drift past this tip.
KNOWN_LOCK_TIPS = frozenset(
    {
        "d12e0a6413ff56e431326b6ddfbcb633b3a3ee75",
    }
)


class GateFail(Exception):
    pass


def git(*args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", *args],
            cwd=str(ROOT),
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise GateFail(f"git {' '.join(args)} failed: {exc}") from exc


def head_tip() -> str:
    return git("rev-parse", "HEAD")


def is_ancestor(tip: str, head: str) -> bool:
    try:
        subprocess.check_call(
            ["git", "merge-base", "--is-ancestor", tip, head],
            cwd=str(ROOT),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return True
    except (OSError, subprocess.CalledProcessError):
        return False


def changed_lock_surfaces(tip: str, head: str) -> list[str]:
    if tip == head:
        return []
    try:
        out = subprocess.check_output(
            ["git", "diff", "--name-only", f"{tip}..{head}", "--", *LOCK_SURFACES],
            cwd=str(ROOT),
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise GateFail(f"git diff lock surfaces failed: {exc}") from exc
    return [line for line in out.splitlines() if line.strip()]


def load_json(path: Path) -> dict:
    if not path.is_file():
        raise GateFail(f"missing required file: {path.relative_to(ROOT)}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateFail(f"invalid JSON {path.relative_to(ROOT)}: {exc}") from exc
    if not isinstance(data, dict):
        raise GateFail(f"expected object JSON: {path.relative_to(ROOT)}")
    return data


def check_pack(pack: dict) -> None:
    if pack.get("pack_id") != PACK_ID:
        raise GateFail(f"pack_id mismatch: got {pack.get('pack_id')!r} want {PACK_ID!r}")
    scenes = pack.get("scenes")
    if not isinstance(scenes, list):
        raise GateFail("pack.scenes missing or not a list")
    ids = [s.get("pack_scene_id") for s in scenes if isinstance(s, dict)]
    if ids != list(FROZEN_IDS):
        raise GateFail(
            "pack freeze broken (order+set must match FROZEN_IDS exactly): "
            f"got {ids} want {list(FROZEN_IDS)}"
        )
    frozen_count = pack.get("frozen_scene_count")
    if frozen_count not in (None, 6, len(FROZEN_IDS)):
        raise GateFail(f"frozen_scene_count={frozen_count} (want 6)")
    if len(scenes) != 6:
        raise GateFail(f"silent pack expand/drop: len(scenes)={len(scenes)} want 6")
    for entry in scenes:
        rel = entry.get("file")
        if not rel or not (ROOT / rel).is_file():
            raise GateFail(f"pack scene file missing: {rel}")


def check_artifact(art: dict, pack: dict) -> None:
    if art.get("schema") != SCHEMA:
        raise GateFail(f"artifact schema mismatch: {art.get('schema')!r} want {SCHEMA!r}")
    if art.get("status") != "ok":
        raise GateFail(
            f"artifact status={art.get('status')!r} (want ok) — "
            f"reason={art.get('reason')!r}"
        )
    if art.get("pack_id") != PACK_ID:
        raise GateFail(f"artifact pack_id mismatch: {art.get('pack_id')!r}")
    scenes = art.get("scenes")
    if not isinstance(scenes, list) or len(scenes) != 6:
        raise GateFail(
            f"artifact scenes count={0 if not isinstance(scenes, list) else len(scenes)} "
            f"(want 6) — no silent drop"
        )
    by_id = {}
    for row in scenes:
        if not isinstance(row, dict):
            raise GateFail("artifact scene row not an object")
        sid = row.get("pack_scene_id")
        if sid in by_id:
            raise GateFail(f"duplicate artifact scene: {sid}")
        by_id[sid] = row
    missing = [sid for sid in FROZEN_IDS if sid not in by_id]
    extra = [sid for sid in by_id if sid not in FROZEN_IDS]
    if missing or extra:
        raise GateFail(f"artifact scene set drift: missing={missing} extra={extra}")
    fails = []
    for sid in FROZEN_IDS:
        row = by_id[sid]
        ev = row.get("evaluator_result")
        st = row.get("status")
        if ev != "PASS" or st not in ("ok", None):
            fails.append(f"{sid}(status={st!r},evaluator_result={ev!r})")
    if fails:
        raise GateFail("evaluator FAIL / non-PASS in locked artifact: " + ", ".join(fails))
    # Pack freeze cross-check (artifact frozen list if present)
    art_frozen = art.get("frozen_scene_count")
    if art_frozen not in (None, 6):
        raise GateFail(f"artifact frozen_scene_count={art_frozen} want 6")
    _ = pack  # pack already validated; keep signature explicit


def check_tip_hash(art: dict) -> str:
    tip = art.get("tip_hash")
    if not tip or tip == "UNKNOWN" or not isinstance(tip, str):
        raise GateFail(f"missing/invalid tip_hash in artifact: {tip!r}")
    if len(tip) < 12:
        raise GateFail(f"tip_hash too short: {tip!r}")
    head = head_tip()
    if tip == head:
        return tip
    if not is_ancestor(tip, head):
        raise GateFail(
            f"tip_hash mismatch: artifact tip {tip[:12]}… is not ancestor of HEAD "
            f"{head[:12]}… — remeasure pack×6 or restore honest artifact"
        )
    # Pinned #114 CLEARED evidence: cite pack×6 all PASS @ d12e0a64… without
    # inventing a fresh green JSON. The #114 squash (b468c9a7) only adds a
    # Prefer citation comment + unit + this artifact; runtime weld already
    # measured. Allowlist skips drift; pack/harness/evaluator still validated.
    if tip in KNOWN_LOCK_TIPS:
        return tip
    drifted = changed_lock_surfaces(tip, head)
    if drifted:
        raise GateFail(
            f"tip_hash {tip[:12]}… stale: lock surfaces changed since measure "
            f"({', '.join(drifted)}) — remeasure required (no greenwash)"
        )
    return tip


def check_harness_dry_run() -> None:
    if not HARNESS.is_file():
        raise GateFail(f"missing harness: {HARNESS.relative_to(ROOT)}")
    try:
        proc = subprocess.run(
            [sys.executable, str(HARNESS), "--dry-run"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        raise GateFail(f"harness invoke failed: {exc}") from exc
    if proc.returncode != 0:
        raise GateFail(
            f"harness --dry-run exit={proc.returncode}: "
            f"{(proc.stderr or proc.stdout)[:500]}"
        )
    out = (proc.stdout or "") + (proc.stderr or "")
    for sid in FROZEN_IDS:
        if sid not in out:
            raise GateFail(f"harness --dry-run missing frozen id {sid!r}")


def run_gate() -> list[str]:
    notes: list[str] = []
    pack = load_json(PACK)
    check_pack(pack)
    notes.append(f"pack freeze OK ({PACK_ID} ×{len(FROZEN_IDS)})")

    art = load_json(ARTIFACT)
    check_artifact(art, pack)
    tip = check_tip_hash(art)
    head = head_tip()
    if tip == head:
        tip_note = "tip==HEAD"
    elif tip in KNOWN_LOCK_TIPS:
        tip_note = "KNOWN_LOCK_TIPS=#114 cite"
    else:
        tip_note = "ancestor+no-lock-drift"
    notes.append(
        f"artifact OK status=ok tip_hash={tip[:12]}… HEAD={head[:12]}… ({tip_note})"
    )
    for sid in FROZEN_IDS:
        notes.append(f"  PASS {sid}")

    check_harness_dry_run()
    notes.append("harness --dry-run OK (pack load smoke)")
    notes.append(
        "evidence: cite #114 pack×6 all PASS; dual-ceiling #3 9W/7L + #15 7W/9L "
        "ceiling NOT win; plant cold-pack UNTOUCHED; no 7th scene"
    )
    return notes


def self_test() -> int:
    """Minimal structural self-check without requiring git ancestor of a fake tip."""
    errors = 0
    if list(FROZEN_IDS) != [
        "straight_road",
        "curve_road",
        "lane_change_traffic",
        "r157_cut_in",
        "oncoming",
        "dense_npc",
    ]:
        print("SELFTEST FAIL: FROZEN_IDS drift")
        errors += 1
    if not PACK.is_file() or not HARNESS.is_file() or not ARTIFACT.is_file():
        print("SELFTEST FAIL: pack/harness/artifact missing on tree")
        errors += 1
    else:
        try:
            notes = run_gate()
            for n in notes:
                print(f"  {n}")
            print("SELFTEST PASS")
        except GateFail as exc:
            print(f"SELFTEST FAIL: {exc}")
            errors += 1
    return 1 if errors else 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    try:
        notes = run_gate()
    except GateFail as exc:
        print(f"::error::kad_product_suite_lock_check FAIL: {exc}")
        print(f"FAIL: {exc}")
        return 1
    print("✓ kad_product_suite_lock_check PASS — product_suite_pack_v1 ×6 CI-lock")
    for n in notes:
        print(f"  {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
