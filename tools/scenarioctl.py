#!/usr/bin/env python3
"""Validate and fingerprint the existing JSON scenario library.

This is intentionally a small standard-library tool. It validates the contract
already consumed by FlowSim and scenario_regression.py; it does not introduce a
second scenario format or silently rewrite files.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import random
import subprocess
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
    for key in ("name", "description", "ego", "actors", "pass_criteria"):
        if key not in data:
            errors.append(f"{path}: missing required field '{key}'")
    if "road_network" not in data and "map_file" not in data and "map_id" not in data:
        errors.append(f"{path}: requires road_network or map_file/map_id")
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
    if road is None and ("map_file" in data or "map_id" in data):
        map_ref = data.get("map_file")
        if not isinstance(map_ref, str) and isinstance(data.get("map_id"), str):
            map_ref = f"../maps/{data['map_id']}/map.json"
        if not isinstance(map_ref, str):
            errors.append(f"{path}: map_file must be a string")
        else:
            map_path = (path.parent / map_ref).resolve()
            if not map_path.is_file():
                errors.append(f"{path}: map reference does not exist: {map_ref}")
    elif not isinstance(road, dict):
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


def validate_map(path: Path) -> list[str]:
    errors: list[str] = []
    try:
        data = load_json(path)
    except ValueError as exc:
        return [str(exc)]
    if not isinstance(data, dict):
        return [f"{path}: root must be an object"]
    for key in ("schema_version", "map_id", "roads"):
        if key not in data:
            errors.append(f"{path}: missing required field '{key}'")
    map_id = data.get("map_id")
    if not isinstance(map_id, str) or not map_id:
        errors.append(f"{path}: map_id must be a non-empty string")
    roads = data.get("roads")
    road_ids: list[str] = []
    if not isinstance(roads, list) or not roads:
        errors.append(f"{path}: roads must be a non-empty array")
    else:
        for index, road in enumerate(roads):
            label = f"{path}: roads[{index}]"
            if not isinstance(road, dict):
                errors.append(f"{label} must be an object")
                continue
            road_id = road.get("id")
            if not isinstance(road_id, str) or not road_id:
                errors.append(f"{label}.id must be a non-empty string")
            else:
                road_ids.append(road_id)
            centerline = road.get("centerline", road.get("nodes"))
            if not isinstance(centerline, list) or len(centerline) < 2:
                errors.append(f"{label}.centerline must contain at least two points")
            lanes = road.get("lanes")
            if isinstance(lanes, list):
                for lane_index, lane in enumerate(lanes):
                    if not isinstance(lane, dict):
                        errors.append(f"{label}.lanes[{lane_index}] must be an object")
                    elif not isinstance(lane.get("centerline"), list):
                        errors.append(f"{label}.lanes[{lane_index}].centerline is required")
            elif not isinstance(lanes, (int, float)):
                errors.append(f"{label}.lanes must be an array or numeric")
            if "lane_width" not in road and not isinstance(lanes, list):
                errors.append(f"{label}.lane_width must be numeric")
            elif "lane_width" in road and not isinstance(road["lane_width"], (int, float)):
                errors.append(f"{label}.lane_width must be numeric")
            if not isinstance(road.get("speed_limit"), (int, float)):
                errors.append(f"{label}.speed_limit must be numeric")
    if len(set(road_ids)) != len(road_ids):
        errors.append(f"{path}: roads contains duplicate ids")
    return errors


def validate_routes(path: Path, map_data: object) -> list[str]:
    errors: list[str] = []
    try:
        data = load_json(path)
    except ValueError as exc:
        return [str(exc)]
    if not isinstance(data, dict):
        return [f"{path}: root must be an object"]
    route_map_id = data.get("map_id")
    if not isinstance(route_map_id, str):
        errors.append(f"{path}: map_id must be a string")
    elif isinstance(map_data, dict) and route_map_id != map_data.get("map_id"):
        errors.append(f"{path}: map_id does not match map")
    routes = data.get("routes")
    if not isinstance(routes, list) or not routes:
        return errors + [f"{path}: routes must be a non-empty array"]
    roads = map_data.get("roads", []) if isinstance(map_data, dict) else []
    road_ids = {road.get("id") for road in roads if isinstance(road, dict)}
    for index, route in enumerate(routes):
        label = f"{path}: routes[{index}]"
        if not isinstance(route, dict) or not isinstance(route.get("id"), str):
            errors.append(f"{label}.id must be a string")
            continue
        route_roads = route.get("roads", route.get("road_chain"))
        if not isinstance(route_roads, list) or not route_roads:
            errors.append(f"{label}.roads must be a non-empty array")
        elif any(road_id not in road_ids for road_id in route_roads):
            errors.append(f"{label}.roads contains a road absent from map")
    return errors


def fingerprint(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def resolve_scenario(value: str) -> Path:
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = ROOT / candidate
    if candidate.is_file():
        return candidate
    by_stem = SCENARIO_DIR / f"{value}.json"
    if by_stem.is_file():
        return by_stem
    raise ValueError(f"scenario not found: {value}")


def generate_variants(template: Path, output_dir: Path, count: int, seed: int,
                      speed_scale: float, position_jitter_m: float) -> list[Path]:
    if count <= 0:
        raise ValueError("count must be positive")
    if speed_scale <= 0.0:
        raise ValueError("speed_scale must be positive")
    source = load_json(template)
    if not isinstance(source, dict):
        raise ValueError(f"{template}: scenario root must be an object")
    output_dir.mkdir(parents=True, exist_ok=True)
    generated: list[Path] = []
    for index in range(count):
        rng = random.Random(seed + index)
        scenario = copy.deepcopy(source)
        scenario["random_seed"] = seed + index
        generation = {
            "template": str(template.relative_to(ROOT)),
            "seed": seed + index,
            "speed_scale": speed_scale,
            "position_jitter_m": position_jitter_m,
        }
        scenario["generation"] = generation
        ego = scenario.get("ego")
        if isinstance(ego, dict):
            for key in ("init_speed", "target_speed"):
                if isinstance(ego.get(key), (int, float)):
                    ego[key] *= speed_scale
        for actor in scenario.get("actors", []):
            if not isinstance(actor, dict):
                continue
            for key in ("vx", "vy", "speed"):
                if isinstance(actor.get(key), (int, float)):
                    actor[key] *= speed_scale
            if isinstance(actor.get("s"), (int, float)) and position_jitter_m:
                actor["s"] = max(0.0, actor["s"] + rng.uniform(
                    -position_jitter_m, position_jitter_m))
        output = output_dir / f"{template.stem}_{index:04d}.json"
        output.write_text(json.dumps(scenario, indent=2, ensure_ascii=False) + "\n",
                          encoding="utf-8")
        generated.append(output)
    return generated


def replay_result(result_path: Path, output_path: Path | None, duration: int,
                  interval: float, run_id: str | None) -> int:
    result = load_json(result_path)
    if not isinstance(result, dict):
        raise ValueError(f"{result_path}: result must be a JSON object")
    run = result.get("run", {})
    scenario_value = run.get("scenario_file") if isinstance(run, dict) else None
    if not scenario_value:
        scenario_value = result.get("scenario")
    if not scenario_value:
        raise ValueError(f"{result_path}: no source scenario_file or scenario")
    scenario = resolve_scenario(str(scenario_value))
    cmd = [
        sys.executable,
        str(ROOT / "ci/evaluators/demo_evaluator.py"),
        "--scenario", str(scenario.relative_to(ROOT)),
        "--duration", str(duration),
        "--interval", str(interval),
        "--json-out", str(output_path or result_path.with_name(
            result_path.stem + "_replay.json")),
    ]
    if run_id:
        cmd += ["--run-id", run_id]
    return subprocess.run(cmd, cwd=ROOT).returncode


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
    map_validate = sub.add_parser("validate-map", help="validate a reusable map and routes")
    map_validate.add_argument("map", type=Path)
    map_validate.add_argument("--routes", type=Path, required=True)
    generate = sub.add_parser("generate", help="generate deterministic scenario variants")
    generate.add_argument("--template", required=True,
                           help="scenario path or file stem")
    generate.add_argument("--count", type=int, default=1)
    generate.add_argument("--seed", type=int, default=1)
    generate.add_argument("--speed-scale", type=float, default=1.0)
    generate.add_argument("--position-jitter-m", type=float, default=0.0)
    generate.add_argument("--output-dir", type=Path, required=True)
    replay = sub.add_parser("replay", help="re-run a saved evaluator result")
    replay.add_argument("--result", type=Path, required=True)
    replay.add_argument("--output", type=Path)
    replay.add_argument("--duration", type=int, default=45)
    replay.add_argument("--interval", type=float, default=0.5)
    replay.add_argument("--run-id")
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
    if args.command == "validate-map":
        map_path = args.map if args.map.is_absolute() else ROOT / args.map
        routes_path = args.routes if args.routes.is_absolute() else ROOT / args.routes
        errors = validate_map(map_path)
        try:
            map_data = load_json(map_path)
        except ValueError:
            map_data = None
        errors.extend(validate_routes(routes_path, map_data))
        if errors:
            for error in errors:
                print(f"FAIL {error}")
            return 1
        print(f"PASS {map_path} routes={routes_path}")
        return 0
    if args.command == "generate":
        template = resolve_scenario(args.template)
        generated = generate_variants(
            template, args.output_dir if args.output_dir.is_absolute()
            else ROOT / args.output_dir, args.count, args.seed,
            args.speed_scale, args.position_jitter_m,
        )
        for path in generated:
            print(f"generated {path} sha256={fingerprint(path)}")
        return 0
    if args.command == "replay":
        return replay_result(args.result, args.output, args.duration,
                             args.interval, args.run_id)
    return 2


if __name__ == "__main__":
    sys.exit(main())
