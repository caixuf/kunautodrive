#!/usr/bin/env python3
"""Validate and fingerprint the existing JSON scenario library.

This is intentionally a small standard-library tool. It validates the contract
already consumed by FlowSim and scenario_regression.py; it does not introduce a
second scenario format or silently rewrite files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCENARIO_DIR = ROOT / "scenarios"
SCENARIO_TYPES = {"car", "truck", "suv", "pedestrian", "bicycle", "ego"}


def load_json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"{path}: cannot read JSON: {exc}") from exc


def _number(value: object, label: str, errors: list[str]) -> None:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        errors.append(f"{label} must be numeric")


def validate_scenario(data: object, path: Path) -> list[str]:
    errors: list[str] = []
    if not isinstance(data, dict):
        return [f"{path}: root must be an object"]
    for key in ("name", "description", "ego", "road_network", "actors", "pass_criteria"):
        if key not in data:
            errors.append(f"{path}: missing required field '{key}'")
    if not isinstance(data.get("name"), str) or not data.get("name"):
        errors.append(f"{path}: name must be a non-empty string")
    if not isinstance(data.get("random_seed"), int):
        errors.append(f"{path}: random_seed must be an integer")

    ego = data.get("ego")
    if not isinstance(ego, dict):
        errors.append(f"{path}: ego must be an object")
    else:
        for key in ("x", "y", "heading", "init_speed"):
            if key in ego:
                _number(ego[key], f"{path}: ego.{key}", errors)

    road = data.get("road_network")
    if not isinstance(road, dict):
        errors.append(f"{path}: road_network must be an object")
    else:
        edges = road.get("edges")
        if not isinstance(edges, list) or not edges:
            errors.append(f"{path}: road_network.edges must be a non-empty array")
        else:
            for index, edge in enumerate(edges):
                label = f"{path}: road_network.edges[{index}]"
                if not isinstance(edge, dict):
                    errors.append(f"{label} must be an object")
                    continue
                for key in ("id", "lanes", "lane_width", "speed_limit"):
                    if key not in edge:
                        errors.append(f"{label} missing '{key}'")
                for key in ("lanes", "lane_width", "speed_limit"):
                    if key in edge:
                        _number(edge[key], f"{label}.{key}", errors)
                if isinstance(edge.get("lanes"), (int, float)) and edge["lanes"] <= 0:
                    errors.append(f"{label}.lanes must be positive")
                if isinstance(edge.get("lane_width"), (int, float)) and edge["lane_width"] <= 0:
                    errors.append(f"{label}.lane_width must be positive")
                nodes = edge.get("nodes")
                has_geometry = isinstance(nodes, list) and len(nodes) >= 2
                has_parametric_geometry = (
                    isinstance(edge.get("length_m"), (int, float)) and edge["length_m"] > 0
                ) or isinstance(edge.get("curvature_profile"), list)
                if not has_geometry and not has_parametric_geometry:
                    errors.append(
                        f"{label} needs nodes or positive length_m/curvature_profile geometry"
                    )

    actors = data.get("actors")
    if not isinstance(actors, list):
        errors.append(f"{path}: actors must be an array")
    else:
        for index, actor in enumerate(actors):
            label = f"{path}: actors[{index}]"
            if not isinstance(actor, dict):
                errors.append(f"{label} must be an object")
                continue
            actor_type = str(actor.get("type", "")).lower()
            if actor_type not in SCENARIO_TYPES:
                errors.append(f"{label}.type '{actor.get('type')}' is unsupported")
            if "x" in actor:
                _number(actor["x"], f"{label}.x", errors)
            if "y" in actor:
                _number(actor["y"], f"{label}.y", errors)

    if not isinstance(data.get("pass_criteria"), dict):
        errors.append(f"{path}: pass_criteria must be an object")
    return errors


def validate_suite(path: Path) -> list[str]:
    errors: list[str] = []
    suite = load_json(path)
    if not isinstance(suite, dict):
        return [f"{path}: suite root must be an object"]
    entries = suite.get("scenarios")
    if not isinstance(entries, list) or not entries:
        return [f"{path}: scenarios must be a non-empty array"]
    seen: set[str] = set()
    for index, entry in enumerate(entries):
        label = f"{path}: scenarios[{index}]"
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            errors.append(f"{label}.file must be a string")
            continue
        scenario_file = ROOT / entry["file"]
        if entry["file"] in seen:
            errors.append(f"{label}: duplicate scenario file {entry['file']}")
        seen.add(entry["file"])
        if not scenario_file.is_file():
            errors.append(f"{label}: missing scenario file {entry['file']}")
    return errors


def fingerprint(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_paths(paths: list[Path]) -> list[dict]:
    results = []
    for path in paths:
        if path.name == "suite.json":
            errors = validate_suite(path)
        else:
            try:
                errors = validate_scenario(load_json(path), path)
            except ValueError as exc:
                errors = [str(exc)]
        results.append({"path": str(path), "ok": not errors,
                        "sha256": fingerprint(path) if path.is_file() else None,
                        "errors": errors})
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    validate = sub.add_parser("validate", help="validate scenarios or the suite")
    validate.add_argument("paths", nargs="*", type=Path,
                          help="scenario JSON paths (default: all scenarios + suite)")
    validate.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args()

    if args.command == "validate":
        paths = args.paths or sorted(SCENARIO_DIR.glob("*.json"))
        results = validate_paths([p if p.is_absolute() else ROOT / p for p in paths])
        if args.as_json:
            print(json.dumps(results, indent=2, ensure_ascii=False))
        else:
            for result in results:
                status = "PASS" if result["ok"] else "FAIL"
                print(f"{status} {result['path']} sha256={result['sha256']}")
                for error in result["errors"]:
                    print(f"  - {error}")
        return 0 if all(result["ok"] for result in results) else 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
