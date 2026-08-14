#!/usr/bin/env python3
"""extract_city_map.py — 从场景 JSON 提炼 maps/city_ring 独立地图数据。

生成两个文件（收敛到可提交状态，地图脱离场景独立存在）：
  maps/city_ring/map.json    — 完整独立静态道路事实源（roads + lanes + junctions + connections + landmarks）
  maps/city_ring/routes.json — 路线定义（已验证主线 + draft 匝道/高架 + 预留左右转）

只产数据，不改任何 C/C++ loader、规划、评测、3D JS。
可独立校验：python3 tools/extract_city_map.py --check maps/city_ring
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

# ── 默认源 ──────────────────────────────────────────────────
DEFAULT_SCENARIO = os.path.join(os.path.dirname(__file__), "..", "scenarios", "city_comprehensive.json")
DEFAULT_OUT = os.path.join(os.path.dirname(__file__), "..", "maps", "city_ring")

# ── 节点几何工具 ────────────────────────────────────────────

def sample_centerline(nodes):
    """返回道路中心线原始节点 [(x, y, z)]（不采样——事实源忠实于源数据，
    loader 可自行插值，避免 5m 采样膨胀成数百 KB）。"""
    return [(float(p[0]), float(p[1]), float(p[2]) if len(p) > 2 else 0.0) for p in nodes]

def right_normal(dx, dy):
    L = math.hypot(dx, dy)
    if L < 1e-9:
        return (0.0, -1.0)
    return (dy / L, -dx / L)

def offset_lane(center, offset):
    """把中心线沿右法线偏移 offset 米，返回 lane 中心线 [(x,y,z)]。"""
    lane = []
    for i, (x, y, z) in enumerate(center):
        if i < len(center) - 1:
            nx, ny = right_normal(center[i + 1][0] - x, center[i + 1][1] - y)
        else:
            nx, ny = right_normal(x - center[i - 1][0], y - center[i - 1][1])
        lane.append([round(x + nx * offset, 3), round(y + ny * offset, 3), round(z, 3)])
    return lane

# ── 车道模型 ────────────────────────────────────────────────
# OpenDRIVE 约定：lane id 0=参考线，正 id=左侧(对向)，负 id=右侧(前进方向)。
# 双向 N 车道 → 每方向 N/2 条；单向(oneway) → 全部同向（前进方向右侧）。

def _markings(road_id, idx, is_opp, per_side, oneway):
    """按车道位置推断标线（前端按类型渲染的语义数据）。
    右侧最外侧=实线白边；对向分隔(中心线)=双黄；其余=白虚线。"""
    mk = []
    if not is_opp:
        if idx == 1 and not oneway:
            mk.append({"type": "double_yellow", "side": "left"})
        if idx == per_side:
            mk.append({"type": "solid_white", "side": "right"})
        else:
            mk.append({"type": "dashed_white", "side": "right"})
    else:
        if idx == 1:
            mk.append({"type": "double_yellow", "side": "right"})
        if idx == per_side:
            mk.append({"type": "solid_white", "side": "left"})
        else:
            mk.append({"type": "dashed_white", "side": "left"})
    return mk

def build_road(edge, rid):
    nodes = edge.get("nodes") or []
    lanes_total = int(edge.get("lanes", edge.get("lane_count", 2)))
    lw = float(edge.get("lane_width", 3.5))
    oneway = bool(edge.get("oneway", False)) or "ramp" in str(edge.get("type", "")).lower()
    sp = float(edge.get("speed_limit", 13.89))
    elev = edge.get("elevation_profile", [])
    etype = str(edge.get("type", "urban"))

    center = sample_centerline(nodes)
    road = {
        "id": str(edge.get("name", "road_%d" % rid)),
        "type": etype,
        "speed_limit": sp,
        "oneway": oneway,
        "lane_width": lw,
        "centerline": [[round(p[0], 3), round(p[1], 3), round(p[2], 3)] for p in center],
        "lanes": [],
    }
    if elev:
        road["elevation_profile"] = [{"s": float(e["s"]), "z": float(e["h"])} for e in elev]

    if oneway:
        per_side = lanes_total
        lane_defs = [(i, False) for i in range(1, per_side + 1)]
    else:
        per_side = lanes_total // 2
        lane_defs = [(i, False) for i in range(1, per_side + 1)] + \
                    [(i, True) for i in range(1, per_side + 1)]

    for idx, is_opp in lane_defs:
        if not is_opp:
            offset = (idx - 0.5) * lw
            direction = 1
        else:
            offset = -(idx - 0.5) * lw
            direction = -1
        lane_id = "%s.lane.%d" % (road["id"], idx if not is_opp else 100 + idx)
        road["lanes"].append({
            "id": lane_id,
            "index": idx,
            "width": lw,
            "direction": direction,
            "centerline": offset_lane(center, offset),
            "markings": _markings(road["id"], idx, is_opp, per_side, oneway),
        })
    return road


# ── 主流程 ──────────────────────────────────────────────────

def extract(scenario_path: str, out_dir: str) -> tuple[dict, dict]:
    with open(scenario_path) as f:
        sc = json.load(f)
    rn = sc.get("road_network", {})
    edges = rn.get("edges", [])
    cross_roads = rn.get("cross_roads", [])

    roads = []
    for i, e in enumerate(edges):
        roads.append(build_road(e, i))

    # lane 级 successors：edge 顺序拼接 → 前段同方向 lane 指向后段同方向 lane
    for i in range(len(roads) - 1):
        cur, nxt = roads[i], roads[i + 1]
        for cl in cur["lanes"]:
            cl["successors"] = [nl["id"] for nl in nxt["lanes"] if nl["direction"] == cl["direction"]]
    # 终点 road 的 lane 无后继 → 显式空数组（保证字段齐全）
    for cl in roads[-1]["lanes"]:
        cl.setdefault("successors", [])

    # cross_roads → 左右转预留连接（无真实几何，仅标记 reserved）
    junctions = []
    turn_roads = []
    for idx, cr in enumerate(cross_roads):
        jid = "junction_reserved_%03d" % (idx + 1)
        junctions.append({
            "id": jid,
            "name": cr.get("name", "reserved_cross"),
            "at_s": cr.get("at_s", 0),
            "length_m": cr.get("length_m", 0),
            "lanes": cr.get("lanes", 2),
            "lane_width": cr.get("lane_width", 3.5),
            "speed_limit": cr.get("speed_limit", 8.0),
            "direction": cr.get("direction", 0),
            "reserved": True,
        })
        turn_roads.append({
            "from": "ground_signal_avenue" if idx < 2 else "curved_commercial_boulevard",
            "at_s": cr.get("at_s", 0),
            "direction": cr.get("direction", 0),
            "junction": jid,
            "reserved": True,
        })

    map_doc = {
        "schema_version": 1,
        "map_id": "city_ring",
        "name": "City Ring",
        # 注意：故意不含 source_scenario —— 地图独立存在，不依赖旧场景
        "roads": roads,
        "connections": turn_roads,
        "junctions": junctions,
        "landmarks": {
            "traffic_lights": [],
            "stop_lines": [],
            "construction_zones": [],
        },
    }

    routes = {
        "map_id": "city_ring",
        "routes": [
            {
                "id": "main",
                "name": "主线（地面主路→上匝道→高架→下匝道→滨河返回路）",
                "kind": "main",
                "road_chain": [r["id"] for r in roads],
                "lane_direction": 1,
                # 已验证主线，不含 draft 标记
            },
            {
                "id": "on_ramp",
                "name": "上匝道（商业大道→东行高架入口→高架东段）",
                "kind": "on_ramp",
                "road_chain": ["curved_commercial_boulevard", "eastbound_viaduct_entry", "elevated_ring_east"],
                "lane_direction": 1,
                "draft": True,
            },
            {
                "id": "off_ramp",
                "name": "下匝道（高架东段→西行高架出口→滨河返回路）",
                "kind": "off_ramp",
                "road_chain": ["elevated_ring_east", "westbound_viaduct_exit", "riverfront_return_road"],
                "lane_direction": 1,
                "draft": True,
            },
            {
                "id": "viaduct",
                "name": "高架（elevated_ring_east，全程 elevation=9m）",
                "kind": "viaduct",
                "road_chain": ["elevated_ring_east"],
                "lane_direction": 1,
                "draft": True,
            },
        ],
        "reserved_turns": turn_roads,
    }

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "map.json"), "w") as f:
        json.dump(map_doc, f, indent=2, ensure_ascii=False)
    with open(os.path.join(out_dir, "routes.json"), "w") as f:
        json.dump(routes, f, indent=2, ensure_ascii=False)
    return map_doc, routes


# ── 独立校验 ────────────────────────────────────────────────

def check(out_dir: str) -> int:
    map_path = os.path.join(out_dir, "map.json")
    routes_path = os.path.join(out_dir, "routes.json")
    errors = []

    # 1. JSON 合法
    try:
        with open(map_path) as f:
            m = json.load(f)
    except Exception as e:
        errors.append("map.json 非法: %s" % e)
        m = {}
    try:
        with open(routes_path) as f:
            r = json.load(f)
    except Exception as e:
        errors.append("routes.json 非法: %s" % e)
        r = {}

    if not m or not r:
        return _report(errors)

    # 2. 道路 ID 唯一
    road_ids = [rd["id"] for rd in m.get("roads", [])]
    dup = {x for x in road_ids if road_ids.count(x) > 1}
    if dup:
        errors.append("道路 ID 重复: %s" % sorted(dup))
    # 必须含 schema 关键字段
    for key in ("map_id", "name", "roads", "connections", "junctions", "landmarks"):
        if key not in m:
            errors.append("map.json 缺字段: %s" % key)
    for key in ("traffic_lights", "stop_lines", "construction_zones"):
        if key not in m.get("landmarks", {}):
            errors.append("landmarks 缺字段: %s" % key)

    # 3. lane 必含字段 + 中心线至少两个点
    all_lane_ids = set()
    for rd in m.get("roads", []):
        for lane in rd.get("lanes", []):
            for key in ("id", "index", "width", "direction", "centerline", "markings", "successors"):
                if key not in lane:
                    errors.append("lane %s 缺字段: %s" % (lane.get("id"), key))
            if len(lane.get("centerline", [])) < 2:
                errors.append("lane %s 中心线少于 2 点" % lane.get("id"))
            all_lane_ids.add(lane.get("id"))
        if len(rd.get("centerline", [])) < 2:
            errors.append("road %s 中心线少于 2 点" % rd.get("id"))
        # 6. 高程连续：elevation_profile 的 s 单调不减、z 有限
        elev = rd.get("elevation_profile", [])
        prev_s = -1.0
        for ep in elev:
            if ep["s"] < prev_s:
                errors.append("road %s 高程 s 非单调" % rd.get("id"))
            prev_s = ep["s"]
            if not math.isfinite(ep["z"]):
                errors.append("road %s 高程 z 非法" % rd.get("id"))

    # 4. 车道 successor 存在（引用的 lane id 必须在本 map 内）
    for rd in m.get("roads", []):
        for lane in rd.get("lanes", []):
            for succ in lane.get("successors", []):
                if succ not in all_lane_ids:
                    errors.append("lane %s successor %s 不存在" % (lane.get("id"), succ))

    # 5. 路线引用道路存在
    road_set = set(road_ids)
    for route in r.get("routes", []):
        for rid in route.get("road_chain", []):
            if rid not in road_set:
                errors.append("route %s 引用道路 %s 不存在" % (route.get("id"), rid))
        if route.get("id") == "main" and route.get("draft"):
            errors.append("主线 main 不应标记 draft")

    # 不依赖旧场景：map.json 不得含 source_scenario
    if "source_scenario" in m:
        errors.append("map.json 仍含 source_scenario（应脱离场景独立）")

    return _report(errors)


def _report(errors: list) -> int:
    if not errors:
        print("校验通过: 所有检查项 OK")
        return 0
    for e in errors:
        print("FAIL:", e)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", default=DEFAULT_SCENARIO,
                        help="源场景 JSON（默认 scenarios/city_comprehensive.json）")
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="输出目录（默认 maps/city_ring）")
    parser.add_argument("--check", metavar="DIR", nargs="?", const=DEFAULT_OUT,
                        help="仅校验指定目录（默认 maps/city_ring），不重新生成")
    args = parser.parse_args()

    if args.check:
        return check(args.check)
    extract(os.path.abspath(args.scenario), os.path.abspath(args.out))
    return check(os.path.abspath(args.out))


if __name__ == "__main__":
    raise SystemExit(main())
