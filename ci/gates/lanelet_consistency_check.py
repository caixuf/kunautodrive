#!/usr/bin/env python3
"""D2-09 consistency gate — map.json ↔ lanelet.osm 双向覆盖校验.

背景（按 M1 接口契约 §4.3 FR-CI-01 + §7.1 A5）：HDMap 落地链路是
`map.json → tools/json_to_lanelet.py → lanelet.osm`，两端的**双向覆盖**必须
钉死，否则下游 Lanelet2 加载/规划链路会静默丢失车道或坐标漂移而无人察觉：

1. 每个 map.json lane 在 .osm 里有对应 relation（按 lanelet_id 一致）
2. 每个 .osm lanelet relation 在 map.json 里有对应 lane
3. centerline 起点坐标偏差 ≤ 1.0m（map.json x/y 直比 .osm lat/lon 当作
   同一坐标——M1 期内不投影；详见 M1_OSM_INTERFACE_CONTRACT.md §2.2）

退码语义（按契约 §5）：
  0 = 全绿
  1 = 有 mismatch（双向覆盖或坐标偏差超 1.0m）
  2 = 缺少 map.json 或缺少对应的 lanelet.osm

CLI:
  python3 ci/gates/lanelet_consistency_check.py
      # 默认扫 maps/*/，每个目录找 map.json + lanelet.osm
  python3 ci/gates/lanelet_consistency_check.py --map maps/<n>/map.json --osm maps/<n>/lanelet.osm
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# M1 期内坐标阈值：契约 §4.3 FR-CI-01 写 1.0m，本期 map.json x/y 与 .osm lat/lon
# 当作同一坐标系（不做 ENU↔WGS84 投影），所以直接欧氏距离即可。
COORD_TOLERANCE_M = 1.0


def parse_lanelet_osm(osm_path: Path) -> dict[str, dict]:
    """解析 lanelet.osm，返回 {lanelet_id: {lat, lon, attrs, ...}}.

    按 M1_OSM_INTERFACE_CONTRACT.md §1/§2 的字节级确定性结构：
      - `<node id="N{i}" lat="..." lon="..." />`：centerline 点（§2.2）
      - `<way id="W{i}"><nd ref="..."/>*N <tag.../></way>`：边界 way（§2.3）
      - `<relation id="R{i}"><member role="left|right" type="way" ref="W..."/>*
                            <tag k="lanelet_id" v="..."/></relation>`：lanelet（§2.4）

    返回 dict 的每个 entry 包含契约 §5 约定的字段：
      lat, lon               — centerline 起点坐标（取 left way 第 1 个 nd ref）
      left_node_ids          — left way 的 nd ref 列表
      right_node_ids         — right way 的 nd ref 列表
      attrs                  — relation 的全部 <tag> 字典
      relation_id            — `<relation id="...">` 便于排查

    仅处理 `type=lanelet` 的 relation（`type=regulatory_element` 是红绿灯，
    不是车到，§2.5，不在本 gate 覆盖范围）。
    """
    tree = ET.parse(osm_path)
    root = tree.getroot()

    # Pass 1: nodes by id
    nodes: dict[str, tuple[float, float]] = {}
    for node in root.findall("node"):
        nid = node.get("id")
        if nid is None:
            continue
        try:
            lat = float(node.get("lat", "nan"))
            lon = float(node.get("lon", "nan"))
        except ValueError:
            continue
        nodes[nid] = (lat, lon)

    # Pass 2: ways by id (nd ref 列表 + tag 字典)
    ways: dict[str, dict] = {}
    for way in root.findall("way"):
        wid = way.get("id")
        if wid is None:
            continue
        nd_refs = [nd.get("ref") for nd in way.findall("nd") if nd.get("ref")]
        tags = {t.get("k"): t.get("v") for t in way.findall("tag") if t.get("k")}
        ways[wid] = {"nd_refs": nd_refs, "tags": tags}

    # Pass 3: relations — 只挑 type=lanelet 的，按 §2.4 顺序 left → right 收成员
    lanelets: dict[str, dict] = {}
    for rel in root.findall("relation"):
        tags = {t.get("k"): t.get("v") for t in rel.findall("tag") if t.get("k")}
        if tags.get("type") != "lanelet":
            continue  # 跳过 regulatory_element 等
        lanelet_id = tags.get("lanelet_id")
        if not lanelet_id:
            continue

        left_ref = right_ref = None
        for member in rel.findall("member"):
            role = member.get("role")
            ref = member.get("ref")
            if role == "left":
                left_ref = ref
            elif role == "right":
                right_ref = ref

        left_node_ids = list(ways.get(left_ref, {}).get("nd_refs", [])) if left_ref else []
        right_node_ids = list(ways.get(right_ref, {}).get("nd_refs", [])) if right_ref else []

        # centerline 起点：取 left way 的第 1 个 nd ref 对应节点的 lat/lon
        # （契约 §2.3："nd ref 按 centerline 顺序"，第 1 个就是起点）
        lat = lon = None
        if left_node_ids:
            first = left_node_ids[0]
            if first in nodes:
                lat, lon = nodes[first]

        lanelets[lanelet_id] = {
            "lat": lat,
            "lon": lon,
            "left_node_ids": left_node_ids,
            "right_node_ids": right_node_ids,
            "attrs": tags,
            "relation_id": rel.get("id"),
            "left_way_id": left_ref,
            "right_way_id": right_ref,
        }

    return lanelets


def _map_lanes(map_json_path: Path) -> dict[str, dict]:
    """从 map.json 抽 {lane_id: {x, y, z, ...}}，中心点 = centerline[0].

    按 M1_OSM_INTERFACE_CONTRACT.md §3：map.json 的
    `roads[].lanes[].centerline[][0]` (x) ↔ .osm node.lon，
    `roads[].lanes[].centerline[][1]` (y) ↔ .osm node.lat。
    """
    data = json.loads(map_json_path.read_text(encoding="utf-8"))
    lanes: dict[str, dict] = {}
    for road in data.get("roads", []) or []:
        for lane in road.get("lanes", []) or []:
            lid = lane.get("id")
            if not lid:
                continue
            centerline = lane.get("centerline") or []
            if not centerline:
                continue
            first = centerline[0]
            try:
                x = float(first[0])
                y = float(first[1])
            except (TypeError, ValueError, IndexError):
                continue
            z = float(first[2]) if len(first) > 2 else 0.0
            lanes[lid] = {
                "x": x,
                "y": y,
                "z": z,
                "road_id": road.get("id"),
                "direction": lane.get("direction"),
            }
    return lanes


def check_consistency(map_json_path: Path, osm_path: Path) -> list[str]:
    """双向覆盖 + 起点坐标校验，返回错误列表（空 = 通过）.

    规则（按契约 §5）：
      1. 每个 map.json lane 在 .osm 里有对应 relation
      2. 每个 .osm lanelet relation 在 map.json 里有对应 lane
      3. centerline 起点坐标偏差 ≤ COORD_TOLERANCE_M（本期不投影）
    """
    errors: list[str] = []

    map_lanes = _map_lanes(map_json_path)
    osm_lanelets = parse_lanelet_osm(osm_path)

    # Rule 1: map.json → .osm
    for lid, lane in map_lanes.items():
        if lid not in osm_lanelets:
            errors.append(
                f"map.json 多 lane {lid}, .osm 找不到对应 llt_id"
            )
            continue
        osm = osm_lanelets[lid]
        osm_lat, osm_lon = osm.get("lat"), osm.get("lon")
        if osm_lat is None or osm_lon is None:
            errors.append(
                f"lanelet_id {lid}: .osm 关系 {osm.get('relation_id')} "
                f"找不到 centerline 起点坐标（left way={osm.get('left_way_id')}）"
            )
            continue
        # Rule 3: centerline 起点偏差
        # 契约 §2.2 + §4.3：本期不投影，map.x ↔ .osm.lon，map.y ↔ .osm.lat
        dx = osm_lon - lane["x"]
        dy = osm_lat - lane["y"]
        dist = math.hypot(dx, dy)
        if dist > COORD_TOLERANCE_M:
            errors.append(
                f"坐标偏差 {dist:.3f} 米 @ lanelet_id {lid} "
                f"(map.x={lane['x']:.6f},map.y={lane['y']:.6f} vs "
                f"osm.lat={osm_lat:.6f},osm.lon={osm_lon:.6f})"
            )

    # Rule 2: .osm → map.json
    for lid in osm_lanelets:
        if lid not in map_lanes:
            errors.append(
                f".osm 多 lanelet_id {lid}, map.json 找不到对应 lane"
            )

    return errors


def _collect_pairs(args: argparse.Namespace) -> tuple[list[tuple[Path, Path]], str | None]:
    """根据 CLI 参数决定要检查的 (map.json, lanelet.osm) 对.

    返回 (pairs, error_msg)。error_msg 非 None 表示 CLI 用法错误（→ exit 2）。
    """
    if args.map or args.osm:
        if not (args.map and args.osm):
            return [], "--map 和 --osm 必须同时指定或不指定"
        return [(Path(args.map), Path(args.osm))], None

    # 默认：扫 ROOT/maps/*/，把每个目录下的 (map.json, lanelet.osm) 收集起来
    maps_dir = ROOT / "maps"
    pairs: list[tuple[Path, Path]] = []
    if not maps_dir.is_dir():
        return [], f"maps 目录不存在: {maps_dir}"
    for d in sorted(maps_dir.iterdir()):
        if not d.is_dir():
            continue
        mp = d / "map.json"
        if not mp.exists():
            continue  # 没有 map.json 的目录不归本 gate 管
        pairs.append((mp, d / "lanelet.osm"))
    return pairs, None


def main() -> int:
    ap = argparse.ArgumentParser(
        description="D2-09: map.json ↔ lanelet.osm 双向覆盖校验 gate",
    )
    ap.add_argument("--map", help="map.json 路径（与 --osm 一起指定）")
    ap.add_argument("--osm", help="lanelet.osm 路径（与 --map 一起指定）")
    args = ap.parse_args()

    pairs, cli_err = _collect_pairs(args)
    if cli_err is not None:
        print(f"::error::{cli_err}", file=sys.stderr)
        return 2

    if not pairs:
        print("::error::没有可检查的 maps/*/map.json（全部目录都缺 map.json）",
              file=sys.stderr)
        return 2

    # Phase 1：检查文件存在性 —— 任一缺失 → exit 2（契约 §5）
    missing: list[str] = []
    existing: list[tuple[Path, Path]] = []
    for mp, op in pairs:
        if not mp.exists():
            missing.append(str(mp))
            continue
        if not op.exists():
            missing.append(str(op))
            continue
        existing.append((mp, op))

    if missing:
        for f in missing:
            print(f"::error::missing: {f}", file=sys.stderr)
        print(
            f"lanelet-consistency-gate MISSING ({len(missing)} file(s)); "
            f"D2-02 必须先为对应地图生成 lanelet.osm。",
            file=sys.stderr,
        )
        return 2

    # Phase 2：一致性检查 —— 任一失败 → exit 1
    errors: list[str] = []
    for mp, op in existing:
        try:
            errs = check_consistency(mp, op)
        except (OSError, json.JSONDecodeError, ET.ParseError) as exc:
            errs = [f"无法解析 {mp.name} 或 {op.name}: {exc}"]
        for e in errs:
            print(f"::error::{mp.parent.name}/{mp.name}: {e}")
        errors.extend(errs)

    if errors:
        print(f"lanelet-consistency-gate FAILED ({len(errors)} issue(s)).")
        return 1

    print(f"✓ lanelet consistency OK ({len(existing)} map(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
