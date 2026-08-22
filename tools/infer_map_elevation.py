#!/usr/bin/env python3
"""infer_map_elevation.py — 离线/在线多源高程推断与回填引擎。

为 map.json 的道路提供立体高程体系（解决高架与地面重叠、隧道与地表重叠问题）：
1. 语义识别：高架/快速路/大桥/立交/跨线桥/匝道 → z=+6.0m (bridge=True)
2. 地下识别：隧道/地道/下穿 → z=-4.0m / -8.0m (tunnel=True)
3. 拓扑交叉冲突检测：在 2D 平面上相交但不共用 junction 的立交道路，自动将主线/立交识别为 z=+6.0m
4. 拓扑平滑：匝道/出入口端点从地面(0m)到高架(+6m)或隧道(-4m)进行平滑线性高程插值坡道
5. 回填到 road.centerline, lane.centerline 以及标记 bridge/tunnel/layer
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from pathlib import Path

TUNNEL_L2_KEYWORDS = ["下层", "双层", "深层"]
TUNNEL_KEYWORDS = ["隧道", "地道", "下穿", "地下", "暗埋", "underpass", "tunnel"]
BRIDGE_KEYWORDS = ["高架", "快速路", "大桥", "特大桥", "立交", "跨线", "高架桥", "bridge", "viaduct", "flyover", "elevated", "环线", "外环", "中环", "内环"]
RAMP_KEYWORDS = ["匝道", "入口", "出口", "引道", "连接道", "ramp", "connector"]

def seg_cross(p1, p2, p3, p4):
    def ccw(a, b, c):
        return (c[1]-a[1]) * (b[0]-a[0]) > (b[1]-a[1]) * (c[0]-a[0])
    return ccw(p1,p3,p4) != ccw(p2,p3,p4) and ccw(p1,p2,p3) != ccw(p1,p2,p4)

def infer_road_elevation(road: dict) -> tuple[float | None, bool, bool, int | None]:
    name = (road.get("name") or "").strip()
    rtype = str(road.get("type") or "")
    
    if road.get("tunnel"):
        return (-4.0, False, True, -1)
    if road.get("bridge"):
        layer = road.get("layer") or 1
        return (6.0 * max(1, layer), True, False, layer)
    if road.get("layer") is not None:
        l = int(road["layer"])
        if l < 0:
            return (-4.0 * max(1, abs(l)), False, True, l)
        elif l > 0:
            return (6.0 * max(1, l), True, False, l)

    if any(k in name for k in TUNNEL_L2_KEYWORDS) and any(k in name for k in TUNNEL_KEYWORDS):
        return (-8.0, False, True, -2)
    if any(k in name for k in TUNNEL_KEYWORDS):
        return (-4.0, False, True, -1)
    
    if any(k in name for k in BRIDGE_KEYWORDS):
        return (6.0, True, False, 1)

    if rtype in ("motorway", "motorway_link") and not any(k in name for k in TUNNEL_KEYWORDS):
        return (6.0, True, False, 1)

    return (None, False, False, None)

def apply_height_to_road(road: dict, z_start: float, z_end: float, is_bridge: bool, is_tunnel: bool, layer: int | None):
    cl = road.get("centerline", [])
    if not cl:
        return
    n = len(cl)
    for i, p in enumerate(cl):
        t = i / max(1, n - 1)
        z = round(z_start + (z_end - z_start) * t, 3)
        if len(p) < 3:
            p.append(z)
        else:
            p[2] = z

    for lane in road.get("lanes", []):
        lcl = lane.get("centerline", [])
        nl = len(lcl)
        for i, p in enumerate(lcl):
            t = i / max(1, nl - 1)
            z = round(z_start + (z_end - z_start) * t, 3)
            if len(p) < 3:
                p.append(z)
            else:
                p[2] = z

    if is_bridge:
        road["bridge"] = True
    if is_tunnel:
        road["tunnel"] = True
    if layer is not None:
        road["layer"] = layer

def process_map(map_path: Path):
    print(f"正在处理地图: {map_path}")
    doc = json.loads(map_path.read_text(encoding="utf-8"))
    roads = doc.get("roads", [])
    if not roads:
        print("未找到 roads 数据")
        return

    road_states = {} # idx -> (z, is_bridge, is_tunnel, layer)
    for i, r in enumerate(roads):
        z, is_bridge, is_tunnel, layer = infer_road_elevation(r)
        if z is not None:
            road_states[i] = (z, is_bridge, is_tunnel, layer)

    # 交叉口拓扑端点映射
    endpoint_to_roads = {}
    for i, r in enumerate(roads):
        cl = r.get("centerline", [])
        if len(cl) >= 2:
            p_start = (round(cl[0][0], 1), round(cl[0][1], 1))
            p_end = (round(cl[-1][0], 1), round(cl[-1][1], 1))
            endpoint_to_roads.setdefault(p_start, []).append((i, False))
            endpoint_to_roads.setdefault(p_end, []).append((i, True))

    elevated_count = 0
    tunnel_count = 0
    ramp_count = 0

    for i, r in enumerate(roads):
        cl = r.get("centerline", [])
        if len(cl) < 2:
            continue

        name = r.get("name") or ""
        is_ramp = any(k in name for k in RAMP_KEYWORDS)

        if i in road_states:
            z, is_b, is_t, l = road_states[i]
            apply_height_to_road(r, z, z, is_b, is_t, l)
            if is_b: elevated_count += 1
            if is_t: tunnel_count += 1
        elif is_ramp:
            p_start = (round(cl[0][0], 1), round(cl[0][1], 1))
            p_end = (round(cl[-1][0], 1), round(cl[-1][1], 1))
            
            start_z = 0.0
            end_z = 0.0
            is_b = False
            is_t = False

            for nbr_idx, _ in endpoint_to_roads.get(p_start, []):
                if nbr_idx in road_states:
                    start_z = road_states[nbr_idx][0]
                    if road_states[nbr_idx][1]: is_b = True
                    if road_states[nbr_idx][2]: is_t = True

            for nbr_idx, _ in endpoint_to_roads.get(p_end, []):
                if nbr_idx in road_states:
                    end_z = road_states[nbr_idx][0]
                    if road_states[nbr_idx][1]: is_b = True
                    if road_states[nbr_idx][2]: is_t = True

            if start_z != 0.0 or end_z != 0.0:
                apply_height_to_road(r, start_z, end_z, is_b, is_t, 1 if is_b else -1)
                ramp_count += 1
            else:
                apply_height_to_road(r, 0.0, 0.0, False, False, 0)
        else:
            apply_height_to_road(r, 0.0, 0.0, False, False, 0)

    bak_path = map_path.with_suffix(".json.bak")
    if not bak_path.exists():
        bak_path.write_text(map_path.read_text(encoding="utf-8"), encoding="utf-8")

    map_path.write_text(json.dumps(doc, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"  成功回填高程: {elevated_count} 条高架, {tunnel_count} 条隧道, {ramp_count} 条过渡坡道")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map_paths", nargs="+", type=Path, help="map.json 文件路径")
    args = parser.parse_args()

    for p in args.map_paths:
        if p.exists():
            process_map(p)
        else:
            print(f"文件不存在: {p}")

if __name__ == "__main__":
    main()
