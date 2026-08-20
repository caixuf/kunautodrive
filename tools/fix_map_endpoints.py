#!/usr/bin/env python3
"""fix_map_endpoints.py — 按 SUMO fork 拓扑吸附相邻路段端点（OSM 大地图断口根治）

OSM→SUMO 转换后相邻路段端点坐标不精确重合（lane shape 偏移/节点重排），
路面 ribbon 在段边界出现断口（郑东 1636 个 2 臂 gap、孤端率 32%）。

核心：SUMO 拓扑精确说明"哪段连哪段"——fork 的 incoming_road 终点（to-node）
与 connector lane successors 的起点（from-node）是**同一 SUMO 节点**，反向边
（road_X ↔ road_rX）的对应端也是同一节点。用并查集把这些"应重合"的端点
归组，每组吸附到平均坐标 → 相连段端点精确重合。

只改 road.centerline 端点坐标，不动几何形状。

用法：
  python3 tools/fix_map_endpoints.py maps/osm_zhengdong/map.json [--out ...]
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from collections import defaultdict


def road_pt(road: dict, last: bool) -> tuple[float, float] | None:
    cl = road.get("centerline") or []
    if len(cl) < 2:
        return None
    p = cl[-1] if last else cl[0]
    return float(p[0]), float(p[1])


def set_pt(road: dict, last: bool, x: float, y: float) -> None:
    cl = road["centerline"]
    if not cl:
        return
    if last:
        cl[-1] = [x, y, cl[-1][2] if len(cl[-1]) > 2 else 0.0]
    else:
        cl[0] = [x, y, cl[0][2] if len(cl[0]) > 2 else 0.0]


def reverse_id(rid: str) -> str | None:
    if rid.startswith("road_r"):
        return rid[6:]
    if rid.startswith("road_"):
        return "road_r" + rid[5:]
    return None


class UnionFind:
    def __init__(self):
        self.parent: dict = {}

    def _key(self, rid: str, last: bool) -> tuple:
        return (rid, last)

    def find(self, x):
        if x not in self.parent:
            self.parent[x] = x
        while self.parent[x] != x:
            self.parent[x] = self.parent[self.parent[x]]
            x = self.parent[x]
        return x

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.parent[rb] = ra


def build_successors(map_data: dict) -> dict[str, set[str]]:
    """incoming_road → 后继 road id 集合（经 connector lane successors）。"""
    lane_data = {}
    for r in map_data["roads"]:
        if r.get("lanes"):
            lane_data[r["id"]] = r["lanes"]
    succ = defaultdict(set)
    for j in map_data.get("junctions", []):
        if j.get("type") != "fork":
            continue
        inc = str(j.get("incoming_road", ""))
        for c in j.get("connecting_roads", []):
            for lane in lane_data.get(c.get("id"), []):
                for s in lane.get("successors", []):
                    succ[inc].add(str(s).split(".lane.")[0])
    return succ


def snap_endpoints(map_data: dict, cell: float = 2.0) -> dict:
    """按 SUMO fork 拓扑吸附相连路段端点（原地修改 map_data['roads']）。

    用并查集把「同一 SUMO node」的端点归组（incoming_road 终点 ↔ connector
    successors 起点 ↔ 反向边对应端），每组吸附到平均坐标。
    返回 {"snapped": int, "groups": int}。
    """
    roads = map_data["roads"]
    by_id = {r["id"]: r for r in roads}
    succ = build_successors(map_data)

    uf = UnionFind()

    def link(rid_a, last_a, rid_b, last_b):
        if rid_a in by_id and rid_b in by_id:
            uf.union((rid_a, last_a), (rid_b, last_b))

    for j in map_data.get("junctions", []):
        if j.get("type") != "fork":
            continue
        inc = str(j.get("incoming_road", ""))
        if inc not in by_id:
            continue
        inc_r = reverse_id(inc)
        if inc_r:
            link(inc, True, inc_r, False)
        for sid in succ.get(inc, ()):
            link(inc, True, sid, False)
            sid_r = reverse_id(sid)
            if sid_r:
                link(sid, False, sid_r, True)

    groups: dict = defaultdict(list)
    for (rid, last), root in uf.parent.items():
        p = road_pt(by_id[rid], last)
        if p is None:
            continue
        groups[uf.find((rid, last))].append((rid, last, p[0], p[1]))

    snapped = 0
    moved_groups = 0
    for members in groups.values():
        if len(members) < 2:
            continue
        ax = sum(m[2] for m in members) / len(members)
        ay = sum(m[3] for m in members) / len(members)
        if all(math.hypot(m[2] - ax, m[3] - ay) < 0.1 for m in members):
            continue
        moved_groups += 1
        for rid, last, ox, oy in members:
            if math.hypot(ox - ax, oy - ay) < 0.1:
                continue
            set_pt(by_id[rid], last, ax, ay)
            snapped += 1
    return {"snapped": snapped, "groups": moved_groups}


def main() -> int:
    ap = argparse.ArgumentParser(description="吸附 OSM 地图相连路段端点")
    ap.add_argument("map_json", help="maps/<id>/map.json")
    ap.add_argument("--out", default=None, help="输出路径（默认原地覆盖）")
    args = ap.parse_args()

    path = args.map_json
    with open(path, encoding="utf-8") as f:
        data = json.load(f)

    stats = snap_endpoints(data)

    out = args.out or path
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
    print(f"{path}: 吸附 {stats['snapped']} 端点 / {stats['groups']} 叉口组 → {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
