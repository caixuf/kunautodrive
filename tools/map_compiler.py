#!/usr/bin/env python3
"""Compile the small declarative map DSL into the runtime map JSON contract."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


TOKEN = re.compile(r'([A-Za-z_][\w]*|"[^"]*"|\[[^\]]*\]|-?\d+(?:\.\d+)?)')


def value(text: str):
    text = text.strip().rstrip(";")
    if text.startswith('"'):
        return json.loads(text)
    if text.startswith("["):
        return json.loads(text.replace("'", '"'))
    if text in ("true", "false"):
        return text == "true"
    try:
        return float(text) if "." in text else int(text)
    except ValueError:
        return text


def compile_map(source: Path) -> dict:
    result = {
        "schema_version": 1,
        "map_id": source.stem,
        "name": source.stem,
        "roads": [],
        "connections": [],
        "junctions": [],
        "landmarks": {
            "traffic_lights": [],
            "stop_lines": [],
            "construction_zones": [],
        },
        "validation": {"default_route": "mainline", "default_demo_enabled": False},
    }
    current = None
    block = None
    for number, raw in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        if line == "}":
            if block == "road":
                result["roads"].append(current)
            elif block == "connection":
                result["connections"].append(current)
            current = None
            block = None
            continue
        if line.startswith("Map") and line.endswith("{"):
            block = "map"
            continue
        match = re.match(r"Road\s+(\w+)\s*\{$", line)
        if match:
            current = {"id": match.group(1), "nodes": []}
            block = "road"
            continue
        if line == "Connection {":
            current = {}
            block = "connection"
            continue
        match = re.match(r"Point\s*\{([^}]*)\}", line)
        if match and block == "road":
            fields = dict(re.findall(r"(\w+)\s*:\s*([^;]+)", match.group(1)))
            current["nodes"].append([
                value(fields.get("x", "0")),
                value(fields.get("y", "0")),
                value(fields.get("z", "0")),
            ])
            continue
        match = re.match(r"Connection\s*\{([^}]*)\}", line)
        if match:
            fields = dict(re.findall(r"(\w+)\s*:\s*([^;]+)", match.group(1)))
            result["connections"].append({
                "from_road": value(fields["from"]),
                "to_road": value(fields["to"]),
                "type": value(fields.get("type", "continue")),
            })
            continue
        if ":" in line:
            key, raw_value = (part.strip() for part in line.rstrip(";").split(":", 1))
            target = current if block in ("road", "connection") else result
            target[key] = value(raw_value)
            if block == "road" and key == "speedLimit":
                target["speed_limit"] = target.pop(key)
            elif block == "road" and key == "laneWidth":
                target["lane_width"] = target.pop(key)
            elif block == "road" and key == "oneWay":
                target["oneway"] = target.pop(key)
            elif block == "connection" and key == "from":
                target["from_road"] = target.pop(key)
            elif block == "connection" and key == "to":
                target["to_road"] = target.pop(key)
            continue
        raise ValueError(f"{source}:{number}: unsupported declaration: {raw}")
    if block is not None:
        raise ValueError(f"{source}: unclosed {block} block")
    if not result["roads"]:
        raise ValueError(f"{source}: Map must contain at least one Road")
    map_id = result.pop("id", None)
    if isinstance(map_id, str):
        result["map_id"] = map_id
    if "name" not in result:
        result["name"] = result["map_id"]
    for road in result["roads"]:
        for key in ("type", "lanes", "lane_width", "speed_limit"):
            if key not in road:
                raise ValueError(f"{source}: road {road['id']} missing {key}")
        if len(road["nodes"]) < 2:
            raise ValueError(f"{source}: road {road['id']} needs two Point declarations")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    compiled = compile_map(args.source)
    args.output.write_text(json.dumps(compiled, indent=2) + "\n", encoding="utf-8")
    print(f"compiled {args.source} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
