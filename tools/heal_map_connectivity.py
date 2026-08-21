#!/usr/bin/env python3
"""heal_map_connectivity.py — 地图拓扑与车道后继自动缝合与修复工具。

用于自动诊断并修复 map.json 中的各类断连问题：
  1. 路线链（routes.json）断开：相邻 road 对之间因微小 gap 或缺失 successor 导致 A* 无法通行；
  2. 车道级孤岛：部分车道 successors 为空或外侧车道未对齐；
  3. 几何 Gap 缝合：同街道或路口相邻道路端点在容差范围内（如 15~35m）自动建立后继；
  4. 自动维护 junctions[] / fork 结构，确保渲染与仿真一致。

用法：
  python3 tools/heal_map_connectivity.py maps/osm_test --in-place
  python3 tools/heal_map_connectivity.py maps/city_center --in-place
  python3 tools/heal_map_connectivity.py --all --in-place
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)


def base_name(road_id: str) -> str:
    m = re.match(r"^(.*)_(\d+)$", road_id)
    return m.group(1) if m else road_id


def norm_angle(a: float) -> float:
    while a > math.pi:
        a -= 2 * math.pi
    while a <= -math.pi:
        a += 2 * math.pi
    return a


def get_road_geometry(road: dict) -> dict:
    """提取 road 的参考线首尾点及朝向。"""
    cl = road.get("centerline") or road.get("nodes") or []
    if not cl or len(cl) < 2:
        z = (0.0, 0.0) if not cl else (float(cl[0][0]), float(cl[0][1]))
        return {
            "start": z,
            "end": z,
            "start_h": 0.0,
            "end_h": 0.0,
            "len": 0.0,
        }
    p0 = (float(cl[0][0]), float(cl[0][1]))
    p1 = (float(cl[1][0]), float(cl[1][1]))
    pn_1 = (float(cl[-2][0]), float(cl[-2][1]))
    pn = (float(cl[-1][0]), float(cl[-1][1]))
    sh = math.atan2(p1[1] - p0[1], p1[0] - p0[0])
    eh = math.atan2(pn[1] - pn_1[1], pn[0] - pn_1[0])
    
    total_len = 0.0
    for i in range(len(cl) - 1):
        total_len += math.hypot(float(cl[i+1][0]) - float(cl[i][0]),
                                float(cl[i+1][1]) - float(cl[i][1]))
    return {
        "start": p0,
        "end": pn,
        "start_h": sh,
        "end_h": eh,
        "len": total_len,
    }


def heal_lane_successors_between_roads(from_road: dict, to_road: dict, direction: int = 1) -> int:
    """在 from_road 和 to_road 之间建立车道级的 successors 映射。
    
    direction: 1 代表正向（从 from_road 末端出来，进入 to_road 首端），
              -1 代表逆向（从 from_road 首端出来，进入 to_road 末端）。
    返回新增连接数。
    """
    from_lanes = [l for l in from_road.get("lanes", [])
                  if isinstance(l, dict) and l.get("direction", 1) == direction]
    to_lanes = [l for l in to_road.get("lanes", [])
                if isinstance(l, dict) and l.get("direction", 1) == direction]

    if not from_lanes or not to_lanes:
        # 如果 to_road 只有 direction=1 但我们以某种相对方向进入
        if not to_lanes:
            to_lanes = [l for l in to_road.get("lanes", []) if isinstance(l, dict)]
        if not from_lanes:
            from_lanes = [l for l in from_road.get("lanes", []) if isinstance(l, dict)]

    if not from_lanes or not to_lanes:
        return 0

    added = 0
    # 策略：按车道相对索引（1, 2, ...）进行智能对齐映射
    for fl in from_lanes:
        succs = fl.setdefault("successors", [])
        fl_idx = fl.get("index", 1)
        if fl.get("direction", 1) == -1 and fl_idx > 100:
            fl_idx -= 100
        
        # 寻找最匹配的 to_lane
        best_tl = None
        min_diff = 999999
        for tl in to_lanes:
            tl_idx = tl.get("index", 1)
            if tl.get("direction", 1) == -1 and tl_idx > 100:
                tl_idx -= 100
            diff = abs(tl_idx - fl_idx)
            if diff < min_diff:
                min_diff = diff
                best_tl = tl
        
        if best_tl:
            tl_id = best_tl["id"]
            if tl_id not in succs:
                succs.append(tl_id)
                added += 1

    return added


def heal_doc(doc: dict, rdoc: dict | None = None, snap: float = 35.0) -> tuple[int, list[str]]:
    """在内存中修改 doc (map.json)，返回 (修复项总数, 修复日志)。"""
    roads = doc.get("roads", [])
    if not roads:
        return 0, ["无 roads 字段，跳过"]

    road_by_id = {r["id"]: r for r in roads if "id" in r}
    logs = []
    total_fixes = 0

    # 1. 优先修复 routes 显式指定的道路链 (Route Chains)
    if rdoc:
        routes = rdoc.get("routes", [])
        for route in routes:
            chain = route.get("road_chain", [])
            dir_val = route.get("lane_direction", 1)
            for i in range(len(chain) - 1):
                r1_id, r2_id = chain[i], chain[i + 1]
                if r1_id not in road_by_id or r2_id not in road_by_id:
                    continue
                r1 = road_by_id[r1_id]
                r2 = road_by_id[r2_id]
                
                # 检查 r1 到 r2 是否已有 lane successor
                has_succ = False
                for l in r1.get("lanes", []):
                    for s in l.get("successors", []):
                        if s.startswith(r2_id + "."):
                            has_succ = True
                            break
                    if has_succ:
                        break
                
                if not has_succ:
                    added = heal_lane_successors_between_roads(r1, r2, direction=dir_val)
                    if added > 0:
                        logs.append(f"缝合 routes [{route.get('id')}] 道路链: {r1_id} -> {r2_id} ({added} 条车道连接)")
                        total_fixes += added

    # 2. 空间邻接与微小 Gap 自动缝合 (Spatial Proximity & Gap Snapping)
    geom_info = {r["id"]: get_road_geometry(r) for r in roads if "id" in r}
    
    for i, r1 in enumerate(roads):
        r1_id = r1.get("id")
        if not r1_id:
            continue
        g1 = geom_info[r1_id]
        r1_end = g1["end"]
        r1_end_h = g1["end_h"]
        
        for j, r2 in enumerate(roads):
            if i == j:
                continue
            r2_id = r2.get("id")
            if not r2_id:
                continue
            g2 = geom_info[r2_id]
            r2_start = g2["start"]
            r2_start_h = g2["start_h"]
            
            d = math.hypot(r2_start[0] - r1_end[0], r2_start[1] - r1_end[1])
            if d <= snap:
                turn = norm_angle(r2_start_h - r1_end_h)
                same_base = base_name(r1_id) == base_name(r2_id)
                if abs(turn) < math.radians(120) or same_base:
                    added = heal_lane_successors_between_roads(r1, r2, direction=1)
                    if added > 0:
                        logs.append(f"空间邻接自动缝合 (dist={d:.1f}m): {r1_id} -> {r2_id} (+{added} successors)")
                        total_fixes += added

    # 3. 补全 junctions / fork 列表
    junctions = doc.setdefault("junctions", [])
    existing_conns = set()
    for j in junctions:
        inc = j.get("incoming_road")
        for cr in j.get("connecting_roads", []):
            existing_conns.add((inc, cr.get("id")))

    for r in roads:
        r_id = r.get("id")
        succ_roads = set()
        for l in r.get("lanes", []):
            for s in l.get("successors", []):
                target_r = s.split(".lane.")[0]
                if target_r != r_id and target_r in road_by_id:
                    succ_roads.add(target_r)
        
        for tr in succ_roads:
            if (r_id, tr) not in existing_conns:
                g1 = geom_info.get(r_id)
                g2 = geom_info.get(tr)
                turn_type = "straight"
                if g1 and g2:
                    diff = norm_angle(g2["start_h"] - g1["end_h"])
                    if abs(diff) > math.radians(130):
                        turn_type = "uturn"
                    elif diff > math.radians(30):
                        turn_type = "left"
                    elif diff < -math.radians(30):
                        turn_type = "right"
                
                found_j = False
                for j in junctions:
                    if j.get("incoming_road") == r_id:
                        j.setdefault("connecting_roads", []).append({"id": tr, "turn": turn_type})
                        found_j = True
                        break
                if not found_j:
                    junctions.append({
                        "id": len(junctions),
                        "type": "fork",
                        "incoming_road": r_id,
                        "connecting_roads": [{"id": tr, "turn": turn_type}],
                    })
                existing_conns.add((r_id, tr))

    return total_fixes, logs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("map_dir", nargs="?", help="地图目录 (如 maps/osm_test)")
    ap.add_argument("--in-place", action="store_true", help="原地覆盖写入 map.json")
    ap.add_argument("--all", action="store_true", help="自动处理 maps/ 下所有地图")
    ap.add_argument("--snap", type=float, default=35.0, help="端点缝合最大容差 (m)")
    args = ap.parse_args()

    target_dirs = []
    if args.all:
        target_dirs = sorted(glob.glob(os.path.join(ROOT, "maps", "*")))
    elif args.map_dir:
        target_dirs = [args.map_dir]
    else:
        ap.print_help()
        return 1

    total_all = 0
    for mdir in target_dirs:
        if not os.path.isdir(mdir):
            continue
        mjson = os.path.join(mdir, "map.json")
        rjson = os.path.join(mdir, "routes.json")
        if not os.path.exists(mjson):
            continue
        
        with open(mjson, "r", encoding="utf-8") as f:
            doc = json.load(f)
        rdoc = None
        if os.path.exists(rjson):
            with open(rjson, "r", encoding="utf-8") as rf:
                rdoc = json.load(rf)

        print(f"=== 检查与修复: {os.path.basename(mdir)} ===")
        fixes, logs = heal_doc(doc, rdoc, snap=args.snap)
        for log in logs:
            print("  *", log)
        print(f"  修复项总数: {fixes}")
        total_all += fixes

        if args.in_place and fixes > 0:
            with open(mjson, "w", encoding="utf-8") as f:
                json.dump(doc, f, indent=2, ensure_ascii=False)
            print(f"  [已保存] {mjson}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
