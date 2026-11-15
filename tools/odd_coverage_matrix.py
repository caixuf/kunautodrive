#!/usr/bin/env python3
"""odd_coverage_matrix.py — ODD × 场景 × 判定 三维覆盖矩阵工具。

扫描 scenarios/*.json（支持显式 odd_tags 与结构化推断），
生成 ODD 维度 × 场景 × 判定准则 覆盖矩阵，高亮“有 ODD 无场景”或“有场景缺判定”的盲区。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
SCENARIO_DIR = ROOT / "scenarios"

ODD_AXES = {
    "weather": ["clear", "rain", "fog", "snow"],
    "road_topology": ["urban_intersection", "highway", "curve", "parking", "roundabout"],
    "interaction": ["cut_in", "car_following", "oncoming", "merge", "pedestrian", "traffic_light"],
}

ORACLE_CHECKS = ["safety_ttc_drac", "rule_line_cross", "rule_traffic_light", "comfort_jerk_lat"]


def infer_tags(doc: dict[str, Any], name: str) -> dict[str, Any]:
    explicit = doc.get("odd_tags") or {}
    lower = name.lower()

    weather = explicit.get("weather")
    if not weather:
        env = doc.get("environment") or {}
        weather = str(env.get("weather", "clear")).lower()
        if weather not in ODD_AXES["weather"]:
            weather = "rain" if "rain" in lower else ("fog" if "fog" in lower else "clear")

    topo = explicit.get("road_topology")
    if not topo:
        if "park" in lower:
            topo = "parking"
        elif "curve" in lower:
            topo = "curve"
        elif "ring" in lower or "roundabout" in lower:
            topo = "roundabout"
        elif "highway" in lower:
            topo = "highway"
        else:
            topo = "urban_intersection"

    interactions = list(explicit.get("interaction") or [])
    if not interactions:
        if "cutin" in lower or "cut_in" in lower:
            interactions.append("cut_in")
        if "oncoming" in lower:
            interactions.append("oncoming")
        if "merge" in lower:
            interactions.append("merge")
        if "light" in lower or "intersection" in lower or "city" in lower:
            interactions.append("traffic_light")
        if not interactions:
            interactions.append("car_following")

    oracles = list(explicit.get("oracles") or doc.get("expected_oracles") or [])
    if not oracles:
        oracles = ["safety_ttc_drac", "rule_line_cross", "comfort_jerk_lat"]
        if "traffic_light" in interactions:
            oracles.append("rule_traffic_light")

    return {
        "weather": weather,
        "road_topology": topo,
        "interaction": interactions,
        "oracles": oracles,
    }


def build_matrix(scenario_dir: Path = SCENARIO_DIR) -> dict[str, Any]:
    scenarios = []
    coverage: dict[str, dict[str, list[str]]] = {
        axis: {val: [] for val in vals} for axis, vals in ODD_AXES.items()
    }
    oracle_cov: dict[str, list[str]] = {o: [] for o in ORACLE_CHECKS}

    for path in sorted(scenario_dir.glob("*.json")):
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            continue
        if not isinstance(doc, dict):
            continue
        tags = infer_tags(doc, path.stem)
        scenarios.append({"name": path.stem, **tags})

        w = tags["weather"]
        if w in coverage["weather"]:
            coverage["weather"][w].append(path.stem)
        t = tags["road_topology"]
        if t in coverage["road_topology"]:
            coverage["road_topology"][t].append(path.stem)
        for inter in tags["interaction"]:
            if inter in coverage["interaction"]:
                coverage["interaction"][inter].append(path.stem)
        for o in tags["oracles"]:
            if o in oracle_cov:
                oracle_cov[o].append(path.stem)

    gaps: list[str] = []
    for axis, buckets in coverage.items():
        for val, names in buckets.items():
            if not names:
                gaps.append(f"missing_odd_scenario:{axis}={val}")
    for o, names in oracle_cov.items():
        if not names:
            gaps.append(f"missing_oracle_coverage:{o}")

    total_cells = sum(len(v) for v in ODD_AXES.values())
    covered_cells = sum(1 for buckets in coverage.values() for names in buckets.values() if names)

    return {
        "schema": "flowengine.odd_coverage_matrix.v1",
        "scenario_count": len(scenarios),
        "odd_cell_coverage": f"{covered_cells}/{total_cells}",
        "gaps": gaps,
        "coverage": coverage,
        "oracle_coverage": {k: len(v) for k, v in oracle_cov.items()},
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scenario-dir", default=str(SCENARIO_DIR))
    ap.add_argument("--json-out", help="Write matrix JSON to path")
    ap.add_argument("--check", action="store_true", help="Print summary and verify >=1 scenario")
    args = ap.parse_args()

    mat = build_matrix(Path(args.scenario_dir))
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(mat, indent=2) + "\n", encoding="utf-8")

    print(
        f"✓ ODD×Scenario×Oracle matrix: {mat['scenario_count']} scenarios, "
        f"ODD cells {mat['odd_cell_coverage']}, {len(mat['gaps'])} gap(s)"
    )
    if mat["gaps"]:
        print("  Gaps:", ", ".join(mat["gaps"]))
    if args.check and mat["scenario_count"] == 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
