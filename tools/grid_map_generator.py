#!/usr/bin/env python3
"""grid_map_generator.py — 程序化生成 5km×5km 城市网格地图（拆段 + 交叉口转向拓扑）。

生成两个文件（与 city_ring 同契约，完全独立，不修改任何既有文件）：
  maps/city_grid/map.json    — 网格道路事实源（roads + lanes + junctions + connections + landmarks）
  maps/city_grid/routes.json — 路线定义（main + 单路验证路线）

布局：N-S 大道（沿 +y）与 E-W 大道（沿 +x）在网格点正交交叉；每条大道在交叉口处
**拆成独立 road 段**（如 ns_avenue_00_seg_03），相邻段端点在交叉口中心相交，从而能
用 lane successors + junctions 表达每个交叉口的直行/左转/右转转向拓扑。
车道几何严格复用 extract_city_map.py 的 build_road / right_normal / offset_lane /
_markings（right-normal 偏移、lane id 方案、标线规则）。

仅输出数据，不改动任何 C/C++ loader。可独立校验：
  python3 tools/grid_map_generator.py --check maps/city_grid
"""
from __future__ import annotations

import argparse
import json
import os
import sys

# ── 复用既有车道几何数学（不重新发明） ──────────────────────────
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from extract_city_map import build_road, check  # noqa: E402

DEFAULT_OUT = os.path.join(HERE, "..", "maps", "city_grid")


def _seg(axis: str, c: int, k: int) -> str:
    """段 id：ns_avenue_XX_seg_YY（覆盖 coords[k]..coords[k+1]）。"""
    return "%s_avenue_%02d_seg_%02d" % (axis, c, k)


def _lane_id(seg_id: str, idx: int, direction: int) -> str:
    """目标 lane id：正向 direction=+1 → .lane.<idx>；对向 direction=-1 → .lane.<100+idx>。"""
    return "%s.lane.%d" % (seg_id, idx if direction == 1 else 100 + idx)


def _turn_targets(seg_id: str, idx: int, direction: int, n: int, m: int):
    """返回本 lane（段 seg_id、车道 idx、行驶方向 direction）在段末端交叉口
    的 直/左/右 三个目标 (target_seg_id, target_direction, exists) 三元组。

    direction: +1 = 沿段几何正向行驶（ns:+y / ew:+x），-1 = 反向。
    n = 每轴大道数，m = 每轴段数（= n-1）。coords 由间距推导（0..(n-1)*spacing）。
    """
    axis = seg_id.split("_")[0]  # 'ns' | 'ew'
    coord_idx = int(seg_id.split("_")[2])
    k = int(seg_id.split("_")[4])

    # 段末端交叉口索引 (i_idx, j_idx)：ns 大道索引 i、ew 大道索引 j。
    if axis == "ns":
        i_idx = coord_idx
        j_idx = k if direction == -1 else k + 1
    else:  # 'ew'
        i_idx = k if direction == -1 else k + 1
        j_idx = coord_idx

    def exists(axis2, c, segk):
        return 0 <= c < n and 0 <= segk < m

    def tgt(axis2, c, segk, d2):
        if not exists(axis2, c, segk):
            return (None, d2, False)
        return (_seg(axis2, c, segk), d2, True)

    if axis == "ns":
        if direction == 1:      # 沿 +y 到 (i_idx=i, j_idx=k+1)
            straight = tgt("ns", i_idx, k + 1, 1)      # 继续 +y
            right = tgt("ew", j_idx, i_idx, 1)         # +y → +x
            left = tgt("ew", j_idx, i_idx - 1, -1)     # +y → -x
        else:                   # 沿 -y 到 (i_idx=i, j_idx=k)
            straight = tgt("ns", i_idx, k - 1, -1)     # 继续 -y
            right = tgt("ew", j_idx, i_idx - 1, -1)    # -y → -x
            left = tgt("ew", j_idx, i_idx, 1)          # -y → +x
    else:  # 'ew'
        if direction == 1:      # 沿 +x 到 (i_idx=k+1, j_idx=j)
            straight = tgt("ew", j_idx, k + 1, 1)      # 继续 +x
            right = tgt("ns", i_idx, j_idx - 1, -1)    # +x → -y
            left = tgt("ns", i_idx, j_idx, 1)          # +x → +y
        else:                   # 沿 -x 到 (i_idx=k, j_idx=j)
            straight = tgt("ew", j_idx, k - 1, -1)     # 继续 -x
            right = tgt("ns", i_idx, j_idx, 1)         # -x → +y
            left = tgt("ns", i_idx, j_idx - 1, -1)     # -x → -y

    return straight, right, left


def generate(size: float, spacing: float, lanes_per_side: int,
             lane_width: float, speed_limit: float) -> tuple[dict, dict]:
    n = int(round(size / spacing)) + 1   # 每轴大道数（26@5000/200）
    m = n - 1                            # 每轴段数（25）
    coords = [i * spacing for i in range(n)]

    roads = []

    # ── 1. 拆段生成所有 road 段 ──────────────────────────────
    # ns 段：x=coords[i]，覆盖 y∈[coords[k], coords[k+1]]
    for i in range(n):
        for k in range(m):
            edge = {
                "name": _seg("ns", i, k),
                "type": "urban",
                "speed_limit": speed_limit,
                "oneway": False,
                "lane_width": lane_width,
                "lanes": lanes_per_side * 2,
                "nodes": [[coords[i], coords[k], 0.0], [coords[i], coords[k + 1], 0.0]],
            }
            roads.append(build_road(edge, i))
    # ew 段：y=coords[j]，覆盖 x∈[coords[k], coords[k+1]]
    for j in range(n):
        for k in range(m):
            edge = {
                "name": _seg("ew", j, k),
                "type": "urban",
                "speed_limit": speed_limit,
                "oneway": False,
                "lane_width": lane_width,
                "lanes": lanes_per_side * 2,
                "nodes": [[coords[k], coords[j], 0.0], [coords[k + 1], coords[j], 0.0]],
            }
            roads.append(build_road(edge, n + j))

    roads_by_id = {r["id"]: r for r in roads}

    # ── 2. 为每段每 lane 填直/左/右 successors ───────────────
    for road in roads:
        for lane in road["lanes"]:
            idx = lane["index"]
            direction = lane["direction"]
            succ = []
            straight, right, left = _turn_targets(road["id"], idx, direction, n, m)
            for tgt_seg, tgt_dir, ok in (straight, right, left):
                if ok:
                    succ.append(_lane_id(tgt_seg, idx, tgt_dir))
            lane["successors"] = succ

    # ── 3. 顶层 junctions[]（fork：每进入方向一个，connecting=直/左/右目标段）──
    junctions = []
    jid = 0
    # 遍历每个交叉口 (i,j)，对每个存在的进入段生成一个 fork junction。
    for i in range(n):
        for j in range(n):
            # 进入段 = 到达交叉口 (coords[i], coords[j]) 的段：
            #   ns_avenue_i_seg_{j-1} 正向（沿 +y 到）
            #   ns_avenue_i_seg_{j}   对向（沿 -y 到）
            #   ew_avenue_j_seg_{i-1} 正向（沿 +x 到）
            #   ew_avenue_j_seg_i     对向（沿 -x 到）
            entries = []
            if j - 1 >= 0:
                entries.append((_seg("ns", i, j - 1), 1))   # 沿 +y 到
            if j < m:
                entries.append((_seg("ns", i, j), -1))      # 沿 -y 到
            if i - 1 >= 0:
                entries.append((_seg("ew", j, i - 1), 1))   # 沿 +x 到
            if i < m:
                entries.append((_seg("ew", j, i), -1))      # 沿 -x 到

            for entry_seg, entry_dir in entries:
                # 用 lane idx=1 的转向目标代表该进入方向的直/左/右 connecting 段
                _, right, left = _turn_targets(entry_seg, 1, entry_dir, n, m)
                straight, _, _ = _turn_targets(entry_seg, 1, entry_dir, n, m)
                conns = []
                for tgt_seg, _d2, ok in (straight, right, left):
                    if ok:
                        conns.append({"id": tgt_seg})
                if not conns:
                    continue
                junctions.append({
                    "id": jid,
                    "type": "fork",
                    "incoming_road": entry_seg,
                    "connecting_roads": conns,
                })
                jid += 1

    # ── map 文档 ───────────────────────────────────────────
    map_doc = {
        "schema_version": 1,
        "map_id": "city_grid",
        "name": "City Grid",
        # 故意不含 source_scenario —— 地图独立存在
        "roads": roads,
        "connections": [],
        "junctions": junctions,
        "landmarks": {
            "traffic_lights": [],
            "stop_lines": [],
            "construction_zones": [],
        },
    }

    # ── routes 文档 ────────────────────────────────────────
    # main：南北主干道 ns_avenue_00 全程（拆段后的段链），已验证不含 draft。
    main_chain = [_seg("ns", 0, k) for k in range(m)]
    routes = {
        "map_id": "city_grid",
        "routes": [
            {
                "id": "main",
                "name": "网格主线（ns_avenue_00 全程）",
                "kind": "main",
                "road_chain": main_chain,
                "lane_direction": 1,
            },
            {
                "id": "ew_spine",
                "name": "东西主干道（ew_avenue_00 全程）",
                "kind": "spine",
                "road_chain": [_seg("ew", 0, k) for k in range(m)],
                "lane_direction": 1,
                "draft": True,
            },
            {
                "id": "ns_spine",
                "name": "南北主干道（ns_avenue_25 全程）",
                "kind": "spine",
                "road_chain": [_seg("ns", n - 1, k) for k in range(m)],
                "lane_direction": 1,
                "draft": True,
            },
        ],
        "reserved_turns": [],
    }

    return map_doc, routes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=float, default=5000.0,
                        help="网格区域边长（米），默认 5000")
    parser.add_argument("--spacing", type=float, default=200.0,
                        help="大道间距（米），默认 200")
    parser.add_argument("--lanes-per-side", type=int, default=2,
                        help="每个方向车道数，默认 2（总 4 车道/路）")
    parser.add_argument("--lane-width", type=float, default=3.5,
                        help="单车道宽（米），默认 3.5")
    parser.add_argument("--speed-limit", type=float, default=15.0,
                        help="限速（m/s），默认 15.0")
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="输出目录（默认 maps/city_grid）")
    parser.add_argument("--check", metavar="DIR", nargs="?", const=DEFAULT_OUT,
                        help="仅校验指定目录（默认 maps/city_grid），不重新生成")
    args = parser.parse_args()

    if args.check:
        return check(os.path.abspath(args.check))

    map_doc, routes = generate(args.size, args.spacing, args.lanes_per_side,
                               args.lane_width, args.speed_limit)
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "map.json"), "w") as f:
        json.dump(map_doc, f, indent=2, ensure_ascii=False)
    with open(os.path.join(out_dir, "routes.json"), "w") as f:
        json.dump(routes, f, indent=2, ensure_ascii=False)
    return check(out_dir)


if __name__ == "__main__":
    raise SystemExit(main())
