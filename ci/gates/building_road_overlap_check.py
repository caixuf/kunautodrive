#!/usr/bin/env python3
"""building_road_overlap_check.py — 建筑压路门禁（WorldEngine 布局 × 路面）

挡住 2026-11-23 的回归：WorldEngine 曾对**所有高架**跳过"是否挡路"判断，导致
沿地面辅路生成的高楼上穿 6m 天桥（视觉"楼建在路上/桥上"）。

判据（与 world_engine.cpp `building_clears_roads` 同源的近似实现）：
  - 对每栋建筑取中心 + 4 角 + 4 边中点（9 采样点，按 heading 旋转，podium 1.35 放大）
  - 落在**地面路段**路面半宽内 → 违规（FAIL）
  - 落在**高架路段**路面半宽内：仅当"桥底净空 >= 楼高+1m"（楼能从桥下穿过）才放行，
    否则 → 违规（FAIL）。这一条正是旧代码漏掉、导致楼穿桥的那一类。

输入（缺省为 demo/CI 运行时产物）：
  --layout   /tmp/flow_world_layout.json   （flowsim 写出的 WorldEngine 布局）
  --topology /tmp/flow_topology.json        （含 metrics.scene.road_network.edges）

退出码：0=PASS，1=FAIL（有压路），2=无法判定（缺布局且 --require-buildings）。
"""
import argparse
import json
import math
import sys

# 高架判定：路段两端 z 的最小值 >= 该阈值视为"架空"（与 world_engine 的 2.8m 一致）
ELEVATED_Z_M = 2.8
# 高楼下穿低桥时，要求桥底净空至少高出楼顶这么多
UNDER_CLEARANCE_M = 1.0


def _load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _road_segments(topology):
    """从拓扑里抽路段：[(ax, ay, bx, by, half_w, elev_z, name)]，elev_z=None 表示地面段。"""
    scene = (topology.get("metrics") or {}).get("scene") or {}
    rn = scene.get("road_network") or {}
    edges = rn.get("edges") or rn.get("roads") or []
    segs = []
    for e in edges:
        nodes = e.get("nodes") or []
        hw = float(e.get("lanes") or 1) * float(e.get("lane_width") or 3.5) / 2.0
        name = e.get("name") or e.get("id")
        for i in range(len(nodes) - 1):
            a, b = nodes[i], nodes[i + 1]
            az = float(a[2]) if len(a) > 2 else 0.0
            bz = float(b[2]) if len(b) > 2 else 0.0
            elev = min(az, bz)
            elev_z = elev if elev >= ELEVATED_Z_M else None
            segs.append((float(a[0]), float(a[1]), float(b[0]), float(b[1]), hw, elev_z, name))
    return segs


def _dist_point_seg(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    if l2 < 1e-9:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / l2))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def _footprint_samples(b):
    """建筑 9 采样点（世界坐标），含 podium 放大与 heading 旋转。"""
    x, y = float(b["x"]), float(b["y"])
    w, d, h, hd = float(b["width"]), float(b["depth"]), float(b.get("height", 0.0)), float(b.get("heading", 0.0))
    mul = 1.35 if h > 28.0 else 1.0
    ha, hc = 0.5 * w * mul, 0.5 * d * mul
    local = [(0, 0), (ha, hc), (ha, -hc), (-ha, -hc), (-ha, hc), (ha, 0), (-ha, 0), (0, hc), (0, -hc)]
    ch, sh = math.cos(hd), math.sin(hd)
    return [(x + a * ch - c * sh, y + a * sh + c * ch) for a, c in local], h


def check(layout, topology):
    buildings = layout.get("buildings") or []
    segs = _road_segments(topology)
    if not segs:
        return [], buildings, 0
    violations = []
    for idx, b in enumerate(buildings):
        samples, h = _footprint_samples(b)
        hit = None
        for (px, py) in samples:
            for (ax, ay, bx, by, hw, elev_z, name) in segs:
                if _dist_point_seg(px, py, ax, ay, bx, by) >= hw:
                    continue
                if elev_z is not None and elev_z >= h + UNDER_CLEARANCE_M:
                    continue  # 楼能从桥下穿过
                hit = {
                    "building": idx, "x": round(float(b["x"]), 1), "y": round(float(b["y"]), 1),
                    "height": round(h, 1), "road": name,
                    "elevated": elev_z is not None, "deck_z": None if elev_z is None else round(elev_z, 1),
                }
                break
            if hit:
                break
        if hit:
            violations.append(hit)
    return violations, buildings, len(segs)


def main():
    ap = argparse.ArgumentParser(description="Building-vs-road overlap gate")
    ap.add_argument("--layout", default="/tmp/flow_world_layout.json")
    ap.add_argument("--topology", default="/tmp/flow_topology.json")
    ap.add_argument("--require-buildings", action="store_true",
                    help="无建筑时判为无法判定（退出码 2），防静默假绿")
    args = ap.parse_args()

    try:
        layout = _load_json(args.layout)
    except OSError:
        if args.require_buildings:
            print(f"✗ 无布局文件 {args.layout}（--require-buildings 下判为无法判定）")
            return 2
        print(f"… 跳过：无布局文件 {args.layout}")
        return 0
    try:
        topology = _load_json(args.topology)
    except (OSError, json.JSONDecodeError) as e:
        print(f"✗ 读取拓扑失败 {args.topology}: {e}")
        return 2

    violations, buildings, nseg = check(layout, topology)
    if not buildings:
        if args.require_buildings:
            print("✗ 布局里没有建筑（--require-buildings 下判为无法判定）")
            return 2
        print("… 跳过：布局里没有建筑")
        return 0

    if violations:
        print(f"✗ building-road overlap: {len(violations)} / {len(buildings)} 栋压路（{nseg} 路段）")
        for v in violations:
            kind = "高架" if v["elevated"] else "地面"
            extra = f" deck_z={v['deck_z']}" if v["elevated"] else ""
            print(f"  - bldg#{v['building']} @({v['x']},{v['y']}) h={v['height']} "
                  f"-> {kind}路 '{v['road']}'{extra}")
        return 1

    print(f"✓ building-road overlap gate OK ({len(buildings)} 栋 / {nseg} 路段，0 压路)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
