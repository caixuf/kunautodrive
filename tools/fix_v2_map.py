#!/usr/bin/env python3
"""fix_v2_map.py — 修复 osm_lujiazui_v2 地图的两类问题

1) 坐标偏差:road.centerline 相对车道组横向偏 6~8m(OSM/SUMO netconvert 产物)。
   重算 road.centerline = 车道组的几何中心:每条车道中心线按弧长重采样到与
   road.centerline 同点数(并处理车道方向反转),逐点求平均。修完后路/车道线
   天然对齐,laneGroupEnvelope 会走到 center≈0 的免偏移路径。

2) 方案一(主要路网):保留 primary/secondary/urban 三类主干道,并强制保留
   routes.json 中 47 段路线(其中 10 段是 residential,否则路线链会断),
   其余住宅小路删除。路数 9675 → ~2738,渲染与 esmini 加载量大幅下降。

用法:
  python3 tools/fix_v2_map.py maps/osm_lujiazui_v2/map.json \
      --routes maps/osm_lujiazui_v2/routes.json --out maps/osm_lujiazui_v2/map.json
"""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path


def _pt3(p):
    return [float(p[0]), float(p[1]), float(p[2]) if len(p) > 2 else 0.0]


def resample(pts, n):
    """把折线按弧长重采样为 n 个点(端点保形)。"""
    pts = [_pt3(p) for p in pts]
    if n <= 1:
        return [pts[0] for _ in range(max(n, 1))]
    m = len(pts)
    if m == 1:
        return [pts[0] for _ in range(n)]
    seg = [math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
           for i in range(m - 1)]
    total = sum(seg) or 1e-9
    cum = [0.0]
    for s in seg:
        cum.append(cum[-1] + s)
    out = []
    for k in range(n):
        target = total * k / (n - 1)
        i = 0
        while i < len(seg) - 1 and cum[i + 1] < target:
            i += 1
        t = (target - cum[i]) / (seg[i] if seg[i] > 0 else 1e-9)
        x = pts[i][0] + (pts[i + 1][0] - pts[i][0]) * t
        y = pts[i][1] + (pts[i + 1][1] - pts[i][1]) * t
        z = pts[i][2] + (pts[i + 1][2] - pts[i][2]) * t
        out.append([x, y, z])
    return out


def lane_group_center(road):
    """重算 road.centerline 为车道组中心;无法计算返回 None(保持原值)。"""
    lanes = road.get("lanes")
    if not isinstance(lanes, list) or not lanes:
        return None
    cls = [l.get("centerline") for l in lanes if isinstance(l, dict) and l.get("centerline")]
    cls = [c for c in cls if len(c) >= 2]
    if not cls:
        return None
    n = len(road.get("centerline", []))
    if n < 2:
        return None
    # 以 road.centerline 首尾方向为基准,统一各车道方向(防反转车道污染平均)
    r = road["centerline"]
    r0, r1 = _pt3(r[0]), _pt3(r[-1])
    rdx, rdy = r1[0] - r0[0], r1[1] - r0[1]
    aligned = []
    for cl in cls:
        c = [ _pt3(p) for p in cl ]
        c0, c1 = c[0], c[-1]
        if (c1[0] - c0[0]) * rdx + (c1[1] - c0[1]) * rdy < 0:
            c = c[::-1]
        aligned.append(resample(c, n))
    new = []
    for i in range(n):
        x = sum(a[i][0] for a in aligned) / len(aligned)
        y = sum(a[i][1] for a in aligned) / len(aligned)
        z = sum(a[i][2] for a in aligned) / len(aligned)
        new.append([x, y, z])
    return new


MAJOR_TYPES = {"primary", "secondary", "urban"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("--routes", required=True)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    out = args.out or args.map

    m = json.load(open(args.map))
    routes = json.load(open(args.routes))
    chain = set()
    for rt in routes.get("routes", []):
        for rid in rt.get("road_chain", []):
            chain.add(rid)

    roads = m.get("roads", [])
    kept = [r for r in roads
            if r.get("id") in chain or r.get("type") in MAJOR_TYPES]

    # 坐标修复:对保留的全部路重算 centerline(主干道同样有偏移,全部修;
    # 路线里 residential 段也一并修,保证路线几何与车道对齐)
    fixed = 0
    for r in kept:
        nc = lane_group_center(r)
        if nc is not None:
            r["centerline"] = nc
            fixed += 1

    # junction 仅保留 incoming_road 仍存在者,避免悬空路口 patch
    kept_ids = {r["id"] for r in kept}
    junc = m.get("junctions", [])
    kept_junc = [j for j in junc
                 if j.get("incoming_road") in kept_ids]

    new_map = dict(m)
    new_map["roads"] = kept
    new_map["junctions"] = kept_junc
    # buildings / landmarks 保留原样(不随路网删减,避免误删有效地标)

    # 校验:路线 47 段是否全在
    missing = [rid for rid in chain if rid not in kept_ids]
    print(f"roads: {len(roads)} -> {len(kept)} (kept)")
    print(f"  major+route forced: route={len(chain)}, missing_from_map={missing}")
    print(f"junctions: {len(junc)} -> {len(kept_junc)}")
    print(f"centerline re-aligned for {fixed} roads")
    print(f"buildings: {len(m.get('buildings', []))} (unchanged)")

    json.dump(new_map, open(out, "w"), ensure_ascii=False)
    print(f"written -> {out}")


if __name__ == "__main__":
    main()
