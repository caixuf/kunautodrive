#!/usr/bin/env python3
"""D2-09 consistency gate — map.json ↔ lanelet.osm 双向覆盖校验.

背景（按 M1 接口契约 §4.3 FR-CI-01 + §7.1 A5）：HDMap 落地链路是
`map.json → tools/json_to_lanelet.py → lanelet.osm`，两端的**双向覆盖**必须
钉死，否则下游 Lanelet2 加载/规划链路会静默丢失车道或坐标漂移而无人察觉：

1. 每个 map.json lane 在 .osm 里有对应 relation（按 lanelet_id 一致）
2. 每个 .osm lanelet relation 在 map.json 里有对应 lane
3. centerline 起点坐标偏差 ≤ 1.0m（取 .osm 中 left/right way 第 1 个 nd ref 几何中点，map.json x/y 直比当
   同一坐标——M1 期内不投影；详见 M1_OSM_INTERFACE_CONTRACT.md §2.2）

4. speed_limit regulatory_element 双向覆盖（D2-08 新增）：
   - Forward: 每个 map.json 有 `speed_limit` 的 lane 在 .osm 里能找到 subtype=speed_limit
     regulatory，**speed_limit 数值偏差 ≤ 0.01 m/s**（契约 §2.5.2 精度 2 位小数）
   - Backward: 反向亦然
5. 独立 stop_line 双向覆盖（D2-08 新增）：
   - Forward: 每个 map.json.landmarks.stop_lines[] item（按 `lane` 字段聚合到 lane_id 计数）
     在 .osm 里能找到 subtype=stop_line regulatory；**数量一致**
   - Backward: 反向亦然
退码语义（按契约 §5）：
  0 = 全绿
  1 = 有 mismatch（双向覆盖或坐标偏差 / 数值偏差超阈值）
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

# D2-08 speed_limit 数值容差：契约 §2.5.2 "单位 m/s，2 位小数"，所以两个 2 位小数值比较阈值
# 取 0.005 + epsilon = 0.01（避免浮点 0.005 边界误差把 round-trip 错误标 FAIL）
SPEED_LIMIT_TOLERANCE_MPS = 0.01


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

        # centerline 起点：v1.0-clarify-2 取 left/right way 第 1 个 nd ref 的几何中点
        # 契约 §3 要求左/右 way 各偏移 width/2（独立节点），单取 left 会天然偏差 width/2
        # （契约 §7.1 修正：v1.0 原方案"共用节点"是错的，零面积车道 Lanelet2 拒绝加载）
        lat = lon = None
        if left_node_ids and right_node_ids                 and left_node_ids[0] in nodes and right_node_ids[0] in nodes:
            left_lat, left_lon = nodes[left_node_ids[0]]
            right_lat, right_lon = nodes[right_node_ids[0]]
            lat = (left_lat + right_lat) / 2.0
            lon = (left_lon + right_lon) / 2.0
        elif left_node_ids and left_node_ids[0] in nodes:
            # regulatory_element 等无 right 的 relation 兜底（本期不触发，仅防御）
            lat, lon = nodes[left_node_ids[0]]

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


def parse_regulatory_elements(osm_path: Path) -> dict[str, dict[str, object]]:
    """解析 .osm 的 regulatory_element relations（D2-08 新增 Rule 4/5 数据源）.

    按 M1_OSM_INTERFACE_CONTRACT.md §2.5：`<relation id="RT{i}">` 且 `type=regulatory_element`，
    按 `subtype` 分桶。本函数只取 D2-08 §2.5.2 (speed_limit) + §2.5.3 (stop_line) 两桶：

    返回结构：
      {
        "speed_limit": {lanelet_id: float, ...},   # Rule 4 双向覆盖 + 数值校验
        "stop_line":   {lanelet_id: int_count, ...},# Rule 5 双向覆盖 + 数量校验
      }

    注意：
    - 同一个 lane 可以有多个 subtype=stop_line regulatory（D2-08 契约 §2.5.3：每个
      landmarks.stop_lines[] item → 1 个 regulatory）。所以 stop_line 桶以 lane_id 聚合到 count
    - speed_limit 每 lane 至多 1 个（map.json 的 speed_limit 在 road 级，被所有 lane 继承）
    - 解析失败的 numeric speed_limit → 该 regulatory 跳过，不影响其它 lane
    - 解析时**不**校验 subtype 合法性（契约 §2.5.4 列举 3 种；本期不拦非法 subtype，由 D2-02
      转换器自测守；D2-09 守"双向覆盖 + 数值/数量"，不替 D2-02 验语法）

    一并更新 `parse_lanelet_osm` 文档：原 docstring 写"仅处理 type=lanelet"——这是指
    Rule 1/2/3（车道 relation），本函数处理 Rule 4/5（regulatory_element），**两条解析路径并存不冲突**。
    """
    tree = ET.parse(osm_path)
    root = tree.getroot()

    speed_limits: dict[str, float] = {}
    stop_lines: dict[str, int] = {}

    for rel in root.findall("relation"):
        tags = {t.get("k"): t.get("v") for t in rel.findall("tag") if t.get("k")}
        if tags.get("type") != "regulatory_element":
            continue  # 只挑 regulatory_element；type=lanelet 在 parse_lanelet_osm 处理
        subtype = tags.get("subtype")
        lanelet_id = tags.get("lanelet_id")
        if not lanelet_id:
            continue

        if subtype == "speed_limit":
            sl_raw = tags.get("speed_limit")
            if sl_raw is None:
                # 缺 speed_limit tag 的 regulatory_element 视为 D2-02 bug，跳过不报
                continue
            try:
                sl_float = float(sl_raw)
            except ValueError:
                # tag 写了非数字字符串（如 "13.89m/s"）→ D2-02 bug，跳过不报
                continue
            speed_limits[lanelet_id] = sl_float

        elif subtype == "stop_line":
            stop_lines[lanelet_id] = stop_lines.get(lanelet_id, 0) + 1
        # 其它 subtype（traffic_light、未知）由 Rule 1/2/3 不覆盖，D2-09 不守

    return {
        "speed_limit": speed_limits,
        "stop_line": stop_lines,
    }


def _map_speed_limits(map_json_path: Path) -> dict[str, float]:
    """从 map.json 抽 {lane_id: speed_limit_mps}（按 D2-08 数据源：roads[].speed_limit）.

    优先级：lane.speed_limit 覆盖 road.speed_limit（与 json_to_lanelet.py `map_speed_limit` 一致）。
    没 speed_limit 的 lane 不入桶（D2-02 不写 tag，D2-09 Rule 4 Forward 不应有此 lane）。

    返回空 dict = 当前 map.json 没有任何 lane 带 speed_limit → Rule 4 Forward vacuously 满足，
    但 Rule 4 Backward 仍要校验（osm 写了 regulatory 但 map.json 没声明 → 反向漏）。
    """
    data = json.loads(map_json_path.read_text(encoding="utf-8"))
    out: dict[str, float] = {}
    for road in data.get("roads", []) or []:
        road_sl = road.get("speed_limit")
        for lane in road.get("lanes", []) or []:
            lid = lane.get("id")
            if not lid:
                continue
            lane_sl = lane.get("speed_limit")
            effective = lane_sl if lane_sl is not None else road_sl
            if effective is None:
                continue
            try:
                out[str(lid)] = float(effective)
            except (TypeError, ValueError):
                continue
    return out


def _map_stop_lines(map_json_path: Path) -> dict[str, int]:
    """从 map.json 抽 {lane_id: stop_line_count}（按 D2-08 数据源：landmarks.stop_lines[]）.

    数据位置契约 §2.5.3：map.json.landmarks.stop_lines[] 在**顶层**（不在 roads[] 下）；
    每条 item 形如 `{lane: "...", x: ..., y: ..., ...}`（坐标可选），按 `lane`/`lane_id` 字段聚合。

    与 parse_regulatory_elements 的 stop_line 桶**口径对齐**：
    - Forward: map.json 的 stop_line 桶 = osm 的 stop_line 桶（lane_id → count）
    - 没 landmarks.stop_lines[] 的 map.json → 返回空 dict（vacuously satisfied if osm 也没写）
    """
    data = json.loads(map_json_path.read_text(encoding="utf-8"))
    landmarks = data.get("landmarks") or {}
    raw = landmarks.get("stop_lines") if isinstance(landmarks, dict) else None
    if not isinstance(raw, list):
        return {}
    out: dict[str, int] = {}
    for item in raw:
        if not isinstance(item, dict):
            continue
        lane_id = item.get("lane") or item.get("lane_id")
        if not lane_id:
            continue
        out[str(lane_id)] = out.get(str(lane_id), 0) + 1
    return out


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

    # Rule 4 / Rule 5（D2-08 新增，gate v2 之前未实现 → 见 _reports/D2_08_design.md §5.2）
    map_speed_limits = _map_speed_limits(map_json_path)
    map_stop_lines = _map_stop_lines(map_json_path)
    reg = parse_regulatory_elements(osm_path)
    osm_speed_limits = reg["speed_limit"]
    osm_stop_lines = reg["stop_line"]

    # Rule 4 Forward: 每个 map.json 有 speed_limit 的 lane 在 .osm 里有 subtype=speed_limit
    # regulatory + speed_limit 数值偏差 ≤ SPEED_LIMIT_TOLERANCE_MPS（契约 §2.5.2：2 位小数）
    for lid, sl in map_speed_limits.items():
        if lid not in osm_speed_limits:
            errors.append(
                f"map.json lane {lid} 声明 speed_limit={sl:.2f} m/s, "
                f".osm 找不到 subtype=speed_limit regulatory"
            )
            continue
        osm_sl = osm_speed_limits[lid]
        if abs(osm_sl - sl) > SPEED_LIMIT_TOLERANCE_MPS:
            errors.append(
                f"speed_limit 数值偏差 @ lane {lid}: "
                f"map.json={sl:.4f} m/s vs .osm={osm_sl:.4f} m/s "
                f"（容差 ±{SPEED_LIMIT_TOLERANCE_MPS:.2f} m/s）"
            )

    # Rule 4 Backward: 反向——osm 写了 regulatory 但 map.json 没声明 → 漏转换/漏声明
    for lid in osm_speed_limits:
        if lid not in map_speed_limits:
            errors.append(
                f".osm lane {lid} 有 subtype=speed_limit regulatory, "
                f"map.json 找不到对应 speed_limit 声明"
            )

    # Rule 5 Forward: 每个 map.json.landmarks.stop_lines[] item（按 lane 聚合）→
    # .osm 有对应 subtype=stop_line regulatory 数量一致
    for lid, count in map_stop_lines.items():
        osm_count = osm_stop_lines.get(lid, 0)
        if osm_count != count:
            errors.append(
                f"stop_line 数量偏差 @ lane {lid}: "
                f"map.json={count} vs .osm={osm_count}"
            )

    # Rule 5 Backward: 反向——osm 写了 stop_line 但 map.json 没声明
    for lid, osm_count in osm_stop_lines.items():
        if lid not in map_stop_lines:
            errors.append(
                f".osm lane {lid} 有 {osm_count} 个 subtype=stop_line regulatory, "
                f"map.json 找不到对应 landmarks.stop_lines[] item"
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
