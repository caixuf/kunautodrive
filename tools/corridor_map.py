#!/usr/bin/env python3
"""corridor_map.py — 按路线走廊裁剪 map.json（阶段2 瓦片化/带宽优化）

问题：maps/osm_zhengdong/map.json 73MB，/api/map/preview 整文件发给前端，
前端 MapData.selectRoadsForPreview 才做 800m 走廊过滤 —— 下载带宽浪费。

方案：本工具在预处理侧复刻前端 selectRoadsForPreview 的走廊语义
（route.road_chain 上的 road + 800m 空间格邻域内 road + 同走廊 buildings），
生成裁剪版 maps/<map_id>/map_corridor.json。C 端 /api/map/preview 优先读它，
前端零改动（仍 POST /api/map/preview）。

走廊语义（与 MapData.js 逐字对齐，保证前后端一致）：
  - LARGE_MAP_ROAD_LIMIT=5000：roads 数不超阈值 → 全量（小图零变化）
  - PREVIEW_CORRIDOR_CELL_M=800：800m 空间格，road_chain road 占格，取 3x3 邻域
  - buildings 用 footprint 质心 / b.x,b.y 判走廊（同前端 selectBuildingsForPreview）

用法：
  python3 tools/corridor_map.py maps/osm_zhengdong
  输出：maps/osm_zhengdong/map_corridor.json
"""

import argparse
import json
import math
import os
import sys

LARGE_MAP_ROAD_LIMIT = 5000
PREVIEW_CORRIDOR_CELL_M = 800
BUILDING_CORRIDOR_M = 800


def is_internal_road(road):
    """SUMO 内部连接器不进可见道路（与 MapData.isInternalRoad 同语义）"""
    rid = str(road.get("id") or "")
    sumo_id = str(road.get("sumo_id") or "")
    return bool(road.get("internal")) or sumo_id.startswith(":") or rid.startswith("road_j")


def select_roads_for_preview(roads, route):
    """复刻 MapData.selectRoadsForPreview"""
    if not isinstance(roads, list) or len(roads) <= LARGE_MAP_ROAD_LIMIT:
        return roads
    road_chain = (route or {}).get("road_chain") or []
    if not road_chain:
        return roads
    route_ids = set(road_chain)
    cell = PREVIEW_CORRIDOR_CELL_M
    cells = set()
    for road in roads:
        if road.get("id") not in route_ids:
            continue
        for p in (road.get("centerline") or road.get("nodes") or []):
            if isinstance(p, (list, tuple)) and p:
                cells.add((math.floor((p[0] or 0) / cell), math.floor((p[1] or 0) / cell)))
    if not cells:
        return roads
    out = []
    for road in roads:
        if road.get("id") in route_ids:
            out.append(road)
            continue
        hit = False
        for p in (road.get("centerline") or road.get("nodes") or []):
            if not isinstance(p, (list, tuple)) or not p:
                continue
            cx = math.floor((p[0] or 0) / cell)
            cy = math.floor((p[1] or 0) / cell)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    if (cx + dx, cy + dy) in cells:
                        hit = True
                        break
                if hit:
                    break
            if hit:
                break
        if hit:
            out.append(road)
    return out


def _segment_grid(roads, cell=100):
    """把 road centerline 折线段装入 100m 网格桶（复刻前端 selectBuildingsForPreview）"""
    grid = {}
    for road in roads:
        pts = road.get("centerline") or road.get("nodes") or []
        for i in range(1, len(pts)):
            a, b = pts[i - 1], pts[i]
            if not isinstance(a, (list, tuple)) or not isinstance(b, (list, tuple)):
                continue
            x0, y0 = float(a[0] or 0), float(a[1] or 0)
            x1, y1 = float(b[0] or 0), float(b[1] or 0)
            minx, maxx = min(x0, x1), max(x0, x1)
            miny, maxy = min(y0, y1), max(y0, y1)
            for cx in range(math.floor(minx / cell), math.floor(maxx / cell) + 1):
                for cy in range(math.floor(miny / cell), math.floor(maxy / cell) + 1):
                    grid.setdefault((cx, cy), []).append((x0, y0, x1, y1))
    return grid


def select_buildings_for_preview(buildings, roads):
    """复刻 MapData.selectBuildingsForPreview：footprint 质心优先，缺失用 b.x/b.y"""
    if not isinstance(buildings, list) or not buildings:
        return buildings
    if not isinstance(roads, list) or not roads:
        return buildings
    cell = 100
    grid = _segment_grid(roads, cell)

    def near(x, y):
        cx, cy = math.floor(x / cell), math.floor(y / cell)
        best = float("inf")
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for (x0, y0, x1, y1) in grid.get((cx + dx, cy + dy), []):
                    abx, aby = x1 - x0, y1 - y0
                    length2 = abx * abx + aby * aby or 1e-9
                    t = max(0.0, min(1.0, ((x - x0) * abx + (y - y0) * aby) / length2))
                    d = math.hypot(x - (x0 + abx * t), y - (y0 + aby * t))
                    if d < best:
                        best = d
        return best < BUILDING_CORRIDOR_M

    out = []
    for b in buildings:
        fp = b.get("footprint")
        if isinstance(fp, list) and fp:
            bx = sum(float(p[0] or 0) for p in fp) / len(fp)
            by = sum(float(p[1] or 0) for p in fp) / len(fp)
        else:
            bx = float(b.get("x") or 0)
            by = float(b.get("y") or 0)
        if near(bx, by):
            out.append(b)
    return out


def corridor_map(map_path, routes_path, out_path):
    with open(map_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    routes = {}
    if routes_path and os.path.exists(routes_path):
        with open(routes_path, "r", encoding="utf-8") as f:
            routes = json.load(f)

    rn = data.get("road_network") if isinstance(data.get("road_network"), dict) else data
    route = None
    if isinstance(routes, dict):
        rl = routes.get("routes")
        if isinstance(rl, list) and rl:
            route = rl[0]

    roads = rn.get("roads") or []
    before = len(roads)
    sel_roads = select_roads_for_preview(roads, route)
    if sel_roads is not roads:
        rn["roads"] = sel_roads
    after_roads = len(rn["roads"])

    if "buildings" in rn:
        bbefore = len(rn["buildings"])
        rn["buildings"] = select_buildings_for_preview(rn["buildings"], rn["roads"])
        bafter = len(rn["buildings"])
    else:
        bbefore = bafter = 0

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)

    src_mb = os.path.getsize(map_path) / 1e6
    out_mb = os.path.getsize(out_path) / 1e6
    print(f"roads: {before} -> {after_roads}")
    if "buildings" in rn:
        print(f"buildings: {bbefore} -> {bafter}")
    print(f"size: {src_mb:.1f}MB -> {out_mb:.1f}MB ({100*out_mb/src_mb:.0f}%)")


def main():
    ap = argparse.ArgumentParser(description="按路线走廊裁剪 map.json")
    ap.add_argument("map_dir", help="maps/<map_id> 目录")
    ap.add_argument("--out", help="输出路径（默认 <map_dir>/map_corridor.json）")
    args = ap.parse_args()

    map_path = os.path.join(args.map_dir, "map.json")
    routes_path = os.path.join(args.map_dir, "routes.json")
    out_path = args.out or os.path.join(args.map_dir, "map_corridor.json")
    if not os.path.exists(map_path):
        print(f"error: {map_path} 不存在", file=sys.stderr)
        sys.exit(1)
    corridor_map(map_path, routes_path, out_path)


if __name__ == "__main__":
    main()
