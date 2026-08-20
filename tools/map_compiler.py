#!/usr/bin/env python3
"""Compile the declarative map DSL (.kmap) into the runtime map JSON contract.

第二阶段的 DSL 完整支持：Road + Point + Lane + Marking + Elevation + Connection。
lane 的 centerline 与 markings 由编译器从 Road 几何自动派生（与
tools/extract_city_map.py 同一套几何逻辑），DSL 里只需声明 lane 的
index/direction（及可选 successors/markings 覆盖）。

DSL 语法示例（见 maps/examples/city_ring_full.kmap）：

    Map {
        id: city_ring
        name: "City Ring"

        Road ground_signal_avenue {
            type: urban
            lanes: 4
            laneWidth: 3.5
            speedLimit: 15.0
            oneWay: false
            Point { x: 0; y: 0; z: 0 }
            Point { x: 180; y: 0; z: 0 }
            Elevation { s: 0; z: 0 }        // 可选；不写则全程贴地
        }

        Connection { from: ground_signal_avenue; to: curved_boulevard; type: continue }
    }

Road 支持两种 lane 声明方式：
  1) 简洁：只写 lanes: N + oneWay，编译器自动派生全部 lane（centerline/markings）
  2) 显式：在 Road 内写 Lane { ... } 块覆盖（index/direction/successors/Markings）
两种可混合——未显式声明的 lane 自动派生，显式的按 DSL 覆盖。

继承自 extract_city_map 的几何派生（单一事实源），保证 DSL 编译结果与
旧场景提取结果一致。
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

# 复用 extract_city_map 的纯几何工具（避免第二份 lane 派生实现）
sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_city_map import right_normal, offset_lane, _markings  # noqa: E402


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


def _block_fields(body: str) -> dict:
    """解析 'key: value; key2: value2' 形式的字段（用于单行块 Marking/Elevation）。"""
    return dict(re.findall(r"(\w+)\s*:\s*([^;]+)", body))


def _derive_lane(road_id: str, center, index: int, is_opp: bool,
                 per_side: int, oneway: bool, lw: float) -> dict:
    """从 road 中心线派生一条 lane（与 extract_city_map.build_road 一致）。"""
    if not is_opp:
        offset = -(index - 0.5) * lw
        direction = 1
    else:
        offset = (index - 0.5) * lw
        direction = -1
    return {
        "id": "%s.lane.%d" % (road_id, index if not is_opp else 100 + index),
        "index": index,
        "width": lw,
        "direction": direction,
        "centerline": offset_lane(center, offset),
        "markings": _markings(road_id, index, is_opp, per_side, oneway),
        "successors": [],
    }


def compile_map(source: Path) -> dict:
    result = {
        "schema_version": 1,
        "map_id": source.stem,
        "name": source.stem,
        "generator": "map_compiler.py (DSL hub — single writer)",
        "roads": [],
        "connections": [],
        "junctions": [],
        "landmarks": {
            "traffic_lights": [],
            "stop_lines": [],
            "construction_zones": [],
        },
        "buildings": [],
    }
    map_id = None

    def close_road(road):
        center = [(float(p[0]), float(p[1]), float(p[2]) if len(p) > 2 else 0.0)
                  for p in road["nodes"]]
        if len(center) < 2:
            raise ValueError(f"{source}: road {road['id']} needs two Point declarations")
        # 派生 lanes：显式 Lane 块按其 index/direction 覆盖，否则按 lanes 数量自动生成
        lanes_total = int(road.get("lanes", road.get("lane_count", 2)))
        lw = float(road.get("lane_width", 3.5))
        oneway = bool(road.get("oneway", False)) or "ramp" in str(road.get("type", "")).lower()
        per_side = lanes_total if oneway else lanes_total // 2
        if oneway:
            defs = [(i, False) for i in range(1, per_side + 1)]
        else:
            defs = [(i, False) for i in range(1, per_side + 1)] + \
                   [(i, True) for i in range(1, per_side + 1)]
        explicit = road.get("_explicit_lanes", [])
        lane_ids = {(l.get("index"), l.get("direction", 1)): l for l in explicit}
        lanes = []
        for idx, is_opp in defs:
            base = _derive_lane(road["id"], center, idx, is_opp, per_side, oneway, lw)
            over = lane_ids.get((idx, base["direction"]))
            if over is not None:
                if "width" in over:
                    base["width"] = over["width"]
                if "_centerline" in over:           # 显式每车道几何（osm2kmap/SUMO 精确形状）
                    base["centerline"] = [
                        [round(float(p[0]), 3), round(float(p[1]), 3),
                         round(float(p[2]) if len(p) > 2 else 0.0, 3)]
                        for p in over["_centerline"]]
                    if len(base["centerline"]) < 2:
                        raise ValueError(f"{source}: road {road['id']} lane needs two Points")
                if "direction" in over:
                    base["direction"] = over["direction"]
                if "successors" in over:
                    base["successors"] = over["successors"]
                if "markings" in over:
                    base["markings"] = over["markings"]
            lanes.append(base)
        road["lanes"] = lanes
        road["centerline"] = [[round(p[0], 3), round(p[1], 3), round(p[2], 3)] for p in center]
        road.pop("nodes", None)   # nodes 是编译中间量，运行时契约只认 centerline/lanes
        if road.get("_elevation"):
            road["elevation_profile"] = road.pop("_elevation")
        for key in ("type", "speed_limit"):
            if key not in road:
                raise ValueError(f"{source}: road {road['id']} missing {key}")
        road.pop("_explicit_lanes", None)
        road.pop("_elevation", None)
        result["roads"].append(road)

    # 解析栈：每项为 (block_type, target_dict)。支持 Map/Road/Lane 任意嵌套。
    stack = []  # [('map'|'road'|'lane', dict)]

    def cur():
        return stack[-1][1] if stack else None

    def cur_block():
        return stack[-1][0] if stack else None

    for number, raw in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        if line == "}":
            if not stack:
                raise ValueError(f"{source}:{number}: unmatched '}}'")
            kind, obj = stack.pop()
            if kind == "road":
                close_road(obj)
            elif kind == "lane":
                parent = cur()
                if parent is not None and cur_block() == "road":
                    parent.setdefault("_explicit_lanes", []).append(obj)
            elif kind == "connection":
                result["connections"].append({
                    "from_road": obj.get("from"),
                    "to_road": obj.get("to"),
                    "type": obj.get("type", "continue"),
                })
            elif kind == "junction":
                result["junctions"].append(obj)
            elif kind == "trafficlight":
                result["landmarks"]["traffic_lights"].append(obj)
            elif kind == "building":
                result["buildings"].append(obj)
            continue
        if line.startswith("Map") and line.endswith("{"):
            stack.append(("map", result))
            continue
        match = re.match(r"Road\s+(\w+)\s*\{$", line)
        if match:
            road = {"id": match.group(1), "nodes": [], "_explicit_lanes": [], "_elevation": []}
            stack.append(("road", road))
            continue
        if line == "Lane {" and cur_block() == "road":
            stack.append(("lane", {}))
            continue
        if line == "Connection {" and cur_block() == "map":
            stack.append(("connection", {}))
            continue
        if line == "Junction {" and cur_block() == "map":
            stack.append(("junction", {"id": None, "connecting_roads": [], "shape": []}))
            continue
        if line == "TrafficLight {" and cur_block() == "map":
            stack.append(("trafficlight", {}))
            continue
        if line == "Building {" and cur_block() == "map":
            stack.append(("building", {"footprint": []}))
            continue
        match = re.match(r"Point\s*\{([^}]*)\}", line)
        if match and cur_block() == "road":
            fields = dict(re.findall(r"(\w+)\s*:\s*([^;]+)", match.group(1)))
            cur()["nodes"].append([
                value(fields.get("x", "0")),
                value(fields.get("y", "0")),
                value(fields.get("z", "0")),
            ])
            continue
        match = re.match(r"Point\s*\{([^}]*)\}", line)
        if match and cur_block() == "lane":
            fields = dict(re.findall(r"(\w+)\s*:\s*([^;]+)", match.group(1)))
            cur().setdefault("_centerline", []).append([
                value(fields.get("x", "0")),
                value(fields.get("y", "0")),
                value(fields.get("z", "0")),
            ])
            continue
        match = re.match(r"ConnectingRoad\s*\{([^}]*)\}", line)
        if match and cur_block() == "junction":
            fields = _block_fields(match.group(1))
            cur()["connecting_roads"].append({
                "id": value(fields["id"]),
                "turn": value(fields.get("turn", "straight")),
            })
            continue
        match = re.match(r"ShapePoint\s*\{([^}]*)\}", line)
        if match and cur_block() == "junction":
            fields = _block_fields(match.group(1))
            cur()["shape"].append([value(fields["x"]), value(fields["y"])])
            continue
        match = re.match(r"FootprintPoint\s*\{([^}]*)\}", line)
        if match and cur_block() == "building":
            fields = _block_fields(match.group(1))
            cur()["footprint"].append([value(fields["x"]), value(fields["y"])])
            continue
        match = re.match(r"Elevation\s*\{([^}]*)\}", line)
        if match and cur_block() == "road":
            fields = _block_fields(match.group(1))
            cur()["_elevation"].append({
                "s": value(fields.get("s", "0")),
                "z": value(fields.get("z", "0")),
            })
            continue
        match = re.match(r"Marking\s*\{([^}]*)\}", line)
        if match and cur_block() == "lane":
            fields = _block_fields(match.group(1))
            cur().setdefault("markings", []).append({
                "type": value(fields["type"]),
                "side": value(fields.get("side", "both")),
            })
            continue
        match = re.match(r"Connection\s*\{([^}]*)\}", line)
        if match:
            fields = _block_fields(match.group(1))
            result["connections"].append({
                "from_road": value(fields["from"]),
                "to_road": value(fields["to"]),
                "type": value(fields.get("type", "continue")),
            })
            continue
        if ":" in line:
            key, raw_value = (part.strip() for part in line.rstrip(";").split(":", 1))
            target = cur()
            val = value(raw_value)
            block_kind = cur_block()
            if block_kind == "map" and key == "id":
                map_id = val
            elif block_kind == "road" and key == "speedLimit":
                target["speed_limit"] = val
            elif block_kind == "road" and key == "laneWidth":
                target["lane_width"] = val
            elif block_kind == "road" and key == "oneWay":
                target["oneway"] = val
            elif block_kind == "road" and key == "lanes":
                target["lanes"] = val
            elif block_kind == "lane" and key == "successors":
                target[key] = val
            else:
                target[key] = val
            continue
        raise ValueError(f"{source}:{number}: unsupported declaration: {raw}")

    if stack:
        raise ValueError(f"{source}: unclosed {stack[-1][0]} block")
    if not result["roads"]:
        raise ValueError(f"{source}: Map must contain at least one Road")
    if map_id:
        result["map_id"] = map_id
        if result["name"] == source.stem:
            result["name"] = map_id
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    compiled = compile_map(args.source)
    # 端点吸附（SUMO fork 拓扑）：相连路段端点精确重合，根治 OSM 断口
    try:
        from fix_map_endpoints import snap_endpoints
        stats = snap_endpoints(compiled)
        if stats["snapped"]:
            print(f"  snapped {stats['snapped']} endpoints / {stats['groups']} junctions")
    except Exception as exc:  # 吸附失败不阻断编译（数据无 fork 拓扑时跳过）
        print(f"  [warn] endpoint snap skipped: {exc}")
    args.output.write_text(json.dumps(compiled, indent=2) + "\n", encoding="utf-8")
    print(f"compiled {args.source} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
