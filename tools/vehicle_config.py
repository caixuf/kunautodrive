#!/usr/bin/env python3
"""Build and validate a vehicle-specific FlowEngine pipeline."""

import argparse
import json
import os
import sys
from pathlib import Path


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{path}: root must be an object")
    return value


def parse_params(node: dict) -> dict:
    value = node.get("params", "{}")
    if isinstance(value, str):
        parsed = json.loads(value)
    elif isinstance(value, dict):
        parsed = value
    else:
        raise ValueError(f'node {node.get("name", "?")}: params must be JSON object/string')
    if not isinstance(parsed, dict):
        raise ValueError(f'node {node.get("name", "?")}: params must decode to an object')
    return parsed


def build_pipeline(base: dict, profile: dict, plugin_dir: str) -> dict:
    if profile.get("status", "ready") != "ready":
        raise ValueError(f'profile is not deployable: {profile.get("id", "?")} '
                         f'({profile.get("reason", "placeholder")})')
    pipeline = json.loads(json.dumps(base))
    nodes = pipeline.get("processes")
    if not isinstance(nodes, list):
        raise ValueError("base pipeline must contain a processes array")
    by_name = {node.get("name"): node for node in nodes if isinstance(node, dict)}
    if len(by_name) != len(nodes):
        raise ValueError("base pipeline contains missing or duplicate process names")

    for name, override in profile.get("nodes", {}).items():
        if name not in by_name:
            raise ValueError(f"profile references unknown node: {name}")
        if not isinstance(override, dict):
            raise ValueError(f"profile node override must be an object: {name}")
        node = by_name[name]
        if "enabled" in override:
            node["auto_start"] = bool(override["enabled"])
        if "library_path" in override:
            node["library_path"] = override["library_path"]
        params = override.get("params")
        if params is not None:
            if not isinstance(params, dict):
                raise ValueError(f"profile params must be an object: {name}")
            merged = parse_params(node)
            merged.update(params)
            merged = {
                key: os.path.expandvars(value) if isinstance(value, str) else value
                for key, value in merged.items()
            }
            unresolved = [key for key, value in merged.items()
                          if isinstance(value, str) and "${" in value]
            if unresolved:
                raise ValueError(f"{name}: unresolved environment variables in "
                                 f"{', '.join(unresolved)}")
            node["params"] = json.dumps(merged, separators=(",", ":"))

    pipeline["vehicle"] = {
        key: profile[key] for key in ("id", "platform", "variant")
        if key in profile
    }
    for node in nodes:
        library = node.get("library_path")
        if isinstance(library, str) and library.startswith("build/lib/"):
            node["library_path"] = plugin_dir.rstrip("/") + "/" + Path(library).name
    return pipeline


def validate_pipeline(pipeline: dict, plugin_dir: Path | None = None) -> list[str]:
    errors = []
    nodes = pipeline.get("processes", [])
    active_publishers: dict[str, list[str]] = {}
    for node in nodes:
        name = node.get("name", "?")
        try:
            parse_params(node)
        except (ValueError, json.JSONDecodeError) as error:
            errors.append(str(error))
        if not node.get("auto_start", False):
            continue
        library = node.get("library_path")
        if not isinstance(library, str) or not library:
            errors.append(f"{name}: missing library_path")
        elif plugin_dir:
            candidate = plugin_dir / Path(library).name
            if not candidate.is_file():
                errors.append(f"{name}: plugin not found: {candidate}")
        for output in node.get("publish", []):
            topic = output if isinstance(output, str) else output.get("topic")
            if topic:
                active_publishers.setdefault(topic, []).append(name)
    for topic, publishers in active_publishers.items():
        if len(publishers) > 1 and topic in {"planning/trajectory", "control/cmd"}:
            errors.append(f"{topic}: multiple active publishers: {', '.join(publishers)}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--plugin-dir", type=Path)
    parser.add_argument("--plugin-rel-dir", required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        pipeline = build_pipeline(load_json(args.base), load_json(args.profile),
                                  args.plugin_rel_dir)
        errors = validate_pipeline(pipeline, args.plugin_dir)
        if errors:
            raise ValueError("\n".join(errors))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(pipeline, indent=2, ensure_ascii=False) + "\n",
                                   encoding="utf-8")
        if args.check or not args.output:
            active = sum(bool(node.get("auto_start")) for node in pipeline["processes"])
            print(f'OK profile={pipeline.get("vehicle", {}).get("id", "?")} '
                  f"active_nodes={active}")
    except ValueError as error:
        print(f"vehicle_config: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
