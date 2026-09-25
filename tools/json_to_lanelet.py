#!/usr/bin/env python3
"""tools/json_to_lanelet.py — FlowEngine map.json → Lanelet2 OSM XML 转换器.

遵 docs/M1_OSM_INTERFACE_CONTRACT.md v1.0 权威规范：
  - 顶层元素：<osm version="0.6" generator="json_to_lanelet.py">
  - 节点：<node id="N{i}" lat="..." lon="..." />（6 位小数，x ↔ lon, y ↔ lat）
  - 边界 way：<way id="W{i}">，子元素先 <nd ref="..."/> 后 <tag k="type" v="line_thin"/>
  - 停止线 way：<way id="W{i}">，<tag k="type" v="stop_line"/>
  - 车道关系：<relation id="R{i}">，member 先 left 后 right，tag 按固定顺序排布
  - 信号灯关系：<relation id="RT{i}">，subtype=traffic_light，排在所有车道 relation 之后
  - 字节级确定性：属性顺序固定、浮点 6 位小数、字典序稳定、空字段跳过不写

用法:
  python3 tools/json_to_lanelet.py maps/osm_test/map.json -o /tmp/test.osm
  python3 tools/json_to_lanelet.py maps/osm_test/map.json   # 输出到 stdout
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# 默认车道宽度（与 json_to_xodr.py 对齐）
DEFAULT_LANE_WIDTH = 3.5

# 支持直接透传的 location 类型（其余映射为 urban，契约 §3）
VALID_LOCATIONS = {"urban", "rural", "motorway", "residential", "highway"}


@dataclass(frozen=True)
class OSMNode:
    id: str
    lat: float
    lon: float


@dataclass(frozen=True)
class OSMWay:
    id: str
    nd_refs: tuple[str, ...]
    way_type: str  # "line_thin" or "stop_line"


@dataclass(frozen=True)
class OSMLaneletRelation:
    id: str
    left_way_id: str
    right_way_id: str
    speed_limit: str | None
    location: str
    one_way: str
    lanelet_id: str


@dataclass(frozen=True)
class OSMRegulatoryRelation:
    id: str
    stop_line_way_id: str
    ref_line_way_id: str
    lanelet_id: str


def offset_left(pts: list[tuple[float, float]], d: float) -> list[tuple[float, float]]:
    """折线沿行进方向左法线偏移 d 米（d < 0 为右侧偏移）。

    端点使用相邻段法线，内部顶点使用相邻段切线的角平分线单位法向量。
    左法线向量为 (-ty, tx)。
    """
    if len(pts) < 2 or abs(d) < 1e-9:
        return list(pts)
    n = len(pts)
    out: list[tuple[float, float]] = []
    for i, (px, py) in enumerate(pts):
        txs = 0.0
        tys = 0.0
        cnt = 0
        if i > 0:
            dx = px - pts[i - 1][0]
            dy = py - pts[i - 1][1]
            ln = math.hypot(dx, dy)
            if ln > 1e-9:
                txs += dx / ln
                tys += dy / ln
                cnt += 1
        if i < n - 1:
            dx = pts[i + 1][0] - px
            dy = pts[i + 1][1] - py
            ln = math.hypot(dx, dy)
            if ln > 1e-9:
                txs += dx / ln
                tys += dy / ln
                cnt += 1
        if cnt == 0:
            out.append((px, py))
            continue
        tl = math.hypot(txs, tys)
        if tl < 1e-9:
            txs, tys = 1.0, 0.0
        else:
            txs /= tl
            tys /= tl
        # 左法线向量 = (-tys, txs)
        nx = -tys * d
        ny = txs * d
        out.append((px + nx, py + ny))
    return out


def _format_coord(val: float) -> str:
    """格式化坐标为 6 位小数，消除 -0.000000 保证字节确定性。"""
    if abs(val) < 1e-9:
        val = 0.0
    return f"{val:.6f}"


def map_location(road_type: Any) -> str:
    """map.json roads[].type → relation.location（契约 §2.4 & §3）。"""
    if not road_type:
        return "urban"
    t = str(road_type).lower().strip()
    return t if t in VALID_LOCATIONS else "urban"


def map_one_way(lane_dir: Any, road_oneway: Any) -> str:
    """map.json direction / oneway → relation.one_way（契约 §3）。"""
    if lane_dir == -1 or road_oneway is False or road_oneway == 0:
        return "no"
    return "yes"


def map_speed_limit(lane_speed: Any, road_speed: Any) -> str | None:
    """提取并格式化 speed_limit，单位 m/s，保留 2 位小数（契约 §2.4）。"""
    raw = lane_speed if lane_speed is not None else road_speed
    if raw is None or raw == "":
        return None
    try:
        val = float(raw)
        return f"{val:.2f}"
    except (ValueError, TypeError):
        return None


def convert_map_dict(data: dict) -> str:
    """将 map.json 字典转换为 Lanelet2 标准 OSM XML 字符串."""
    roads = data.get("roads", [])
    if not isinstance(roads, list):
        roads = []

    # 1. 遍历 roads 与 lanes，构建 nodes 与 boundary ways（严格递增）
    nodes: list[OSMNode] = []
    ways: list[OSMWay] = []
    lane_info_list: list[dict] = []
    node_id_counter = 0
    way_id_counter = 0

    for road_idx, road in enumerate(roads):
        road_id = str(road.get("id", f"road_{road_idx}"))
        road_type = road.get("type")
        road_speed = road.get("speed_limit")
        road_oneway = road.get("oneway")

        lanes = road.get("lanes", [])
        if not isinstance(lanes, list):
            continue

        for lane_idx, lane in enumerate(lanes):
            lane_id = lane.get("id")
            if not lane_id:
                lane_id = f"{road_id}.lane.{lane_idx + 1}"
            lane_id = str(lane_id)

            centerline = lane.get("centerline")
            if not centerline or not isinstance(centerline, list) or len(centerline) < 2:
                raise ValueError(
                    f"Invalid or missing centerline for road '{road_id}', lane '{lane_id}'"
                )

            pts: list[tuple[float, float]] = []
            for p in centerline:
                if not isinstance(p, (list, tuple)) or len(p) < 2:
                    raise ValueError(
                        f"Invalid centerline point {p} in road '{road_id}', lane '{lane_id}'"
                    )
                pts.append((float(p[0]), float(p[1])))

            width_val = lane.get("width", road.get("lane_width", DEFAULT_LANE_WIDTH))
            try:
                width = float(width_val)
            except (ValueError, TypeError):
                width = DEFAULT_LANE_WIDTH

            # TODO(M2): project ENU->WGS84 via pyproj
            # 当前 M1: lat/lon 与 map.json x/y 字段同名复用 (lon=x, lat=y)
            left_pts = offset_left(pts, width / 2.0)
            right_pts = offset_left(pts, -width / 2.0)

            # Node 生成：契约 §4.1 按 centerline 出现顺序，先 left 节点后 right 节点
            left_node_ids: list[str] = []
            for p in left_pts:
                nid = f"N{node_id_counter}"
                node_id_counter += 1
                nodes.append(OSMNode(id=nid, lat=p[1], lon=p[0]))
                left_node_ids.append(nid)

            right_node_ids: list[str] = []
            for p in right_pts:
                nid = f"N{node_id_counter}"
                node_id_counter += 1
                nodes.append(OSMNode(id=nid, lat=p[1], lon=p[0]))
                right_node_ids.append(nid)

            # Way 生成：契约 §4.2 left way 先于 right way
            left_wid = f"W{way_id_counter}"
            way_id_counter += 1
            ways.append(OSMWay(id=left_wid, nd_refs=tuple(left_node_ids), way_type="line_thin"))

            right_wid = f"W{way_id_counter}"
            way_id_counter += 1
            ways.append(OSMWay(id=right_wid, nd_refs=tuple(right_node_ids), way_type="line_thin"))

            lane_info_list.append({
                "lane_id": lane_id,
                "left_way_id": left_wid,
                "right_way_id": right_wid,
                "left_node_ids": left_node_ids,
                "right_node_ids": right_node_ids,
                "speed_limit": map_speed_limit(lane.get("speed_limit"), road_speed),
                "location": map_location(road_type),
                "one_way": map_one_way(lane.get("direction"), road_oneway),
            })

    lane_by_id = {info["lane_id"]: info for info in lane_info_list}

    # 2. 收集 traffic lights（支持 top-level 与 landmarks.traffic_lights）
    raw_tls = data.get("traffic_lights")
    if not raw_tls and isinstance(data.get("landmarks"), dict):
        raw_tls = data["landmarks"].get("traffic_lights")
    if not isinstance(raw_tls, list):
        raw_tls = []

    # 稳定排序 traffic lights
    valid_tls: list[dict] = []
    for tl_idx, tl in enumerate(raw_tls):
        if not isinstance(tl, dict):
            continue
        tl_lane = tl.get("lane") or tl.get("lane_id")
        if not tl_lane or str(tl_lane) not in lane_by_id:
            continue
        valid_tls.append({
            "lane_id": str(tl_lane),
            "tl_id": str(tl.get("id", tl_idx)),
            "orig_idx": tl_idx,
        })
    valid_tls.sort(key=lambda item: (item["lane_id"], item["tl_id"], item["orig_idx"]))

    # 3. 为 traffic lights 生成 stop_line way
    reg_relations_raw: list[dict] = []
    for tl_item in valid_tls:
        target = lane_by_id[tl_item["lane_id"]]
        stop_wid = f"W{way_id_counter}"
        way_id_counter += 1
        # 停止线跨越车道末端：连接左边界末点与右边界末点
        stop_refs = (target["left_node_ids"][-1], target["right_node_ids"][-1])
        ways.append(OSMWay(id=stop_wid, nd_refs=stop_refs, way_type="stop_line"))
        reg_relations_raw.append({
            "stop_way_id": stop_wid,
            "ref_way_id": target["left_way_id"],
            "lane_id": tl_item["lane_id"],
        })

    # 4. 生成 lanelet relations：契约 §4.3 按 lane id 字典序
    sorted_lanes = sorted(lane_info_list, key=lambda x: x["lane_id"])
    lanelet_relations: list[OSMLaneletRelation] = []
    for rel_idx, info in enumerate(sorted_lanes):
        lanelet_relations.append(OSMLaneletRelation(
            id=f"R{rel_idx}",
            left_way_id=info["left_way_id"],
            right_way_id=info["right_way_id"],
            speed_limit=info["speed_limit"],
            location=info["location"],
            one_way=info["one_way"],
            lanelet_id=info["lane_id"],
        ))

    # 5. 生成 regulatory relations（契约 §4.7 traffic_light 在所有 lane relation 之后）
    regulatory_relations: list[OSMRegulatoryRelation] = []
    for rt_idx, reg in enumerate(reg_relations_raw):
        regulatory_relations.append(OSMRegulatoryRelation(
            id=f"RT{rt_idx}",
            stop_line_way_id=reg["stop_way_id"],
            ref_line_way_id=reg["ref_way_id"],
            lanelet_id=reg["lane_id"],
        ))

    # 6. 按照字节级确定性格式组装 XML 字符串
    lines: list[str] = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<osm version="0.6" generator="json_to_lanelet.py">',
    ]

    for node in nodes:
        lines.append(f'  <node id="{node.id}" lat="{_format_coord(node.lat)}" lon="{_format_coord(node.lon)}" />')

    for way in ways:
        lines.append(f'  <way id="{way.id}">')
        for ref in way.nd_refs:
            lines.append(f'    <nd ref="{ref}" />')
        lines.append(f'    <tag k="type" v="{way.way_type}" />')
        lines.append('  </way>')

    for rel in lanelet_relations:
        lines.append(f'  <relation id="{rel.id}">')
        lines.append(f'    <member type="way" ref="{rel.left_way_id}" role="left" />')
        lines.append(f'    <member type="way" ref="{rel.right_way_id}" role="right" />')
        lines.append('    <tag k="type" v="lanelet" />')
        lines.append('    <tag k="subtype" v="road" />')
        if rel.speed_limit is not None:
            lines.append(f'    <tag k="speed_limit" v="{rel.speed_limit}" />')
        lines.append(f'    <tag k="location" v="{rel.location}" />')
        lines.append(f'    <tag k="one_way" v="{rel.one_way}" />')
        lines.append(f'    <tag k="lanelet_id" v="{rel.lanelet_id}" />')
        lines.append('  </relation>')

    for rt in regulatory_relations:
        lines.append(f'  <relation id="{rt.id}">')
        lines.append(f'    <member type="way" ref="{rt.stop_line_way_id}" role="stop_line" />')
        lines.append(f'    <member type="way" ref="{rt.ref_line_way_id}" role="ref_line" />')
        lines.append('    <tag k="type" v="regulatory_element" />')
        lines.append('    <tag k="subtype" v="traffic_light" />')
        lines.append(f'    <tag k="lanelet_id" v="{rt.lanelet_id}" />')
        lines.append('  </relation>')

    lines.append('</osm>\n')
    return '\n'.join(lines)


def convert_file(map_json_path: Path) -> str:
    """读取并转换 map.json 文件为 OSM XML."""
    content = map_json_path.read_text(encoding="utf-8")
    data = json.loads(content)
    return convert_map_dict(data)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="FlowEngine map.json → Lanelet2 OSM XML 转换器"
    )
    ap.add_argument("map_json", help="map.json 道路网络定义文件路径")
    ap.add_argument("-o", "--output", help="输出 .osm 路径 (默认 stdout)")
    args = ap.parse_args()

    map_path = Path(args.map_json).resolve()
    if not map_path.exists():
        print(f"Error: map file not found: {map_path}", file=sys.stderr)
        return 1

    try:
        xml_content = convert_file(map_path)
    except Exception as exc:
        print(f"Error during conversion: {exc}", file=sys.stderr)
        return 1

    if args.output:
        out_path = Path(args.output).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(xml_content, encoding="utf-8")
        print(
            f"✓ {args.map_json} → {args.output} ({len(xml_content.encode('utf-8'))} bytes)",
            file=sys.stderr,
        )
    else:
        sys.stdout.write(xml_content)

    return 0


if __name__ == "__main__":
    sys.exit(main())
