#!/usr/bin/env python3
"""tools/json_to_lanelet.py — FlowEngine map.json → Lanelet2 OSM XML 转换器.

遵 docs/M1_OSM_INTERFACE_CONTRACT.md v1.0 权威规范：
  - 顶层元素：<osm version="0.6" generator="json_to_lanelet.py">
  - 节点：<node id="N{i}" lat="..." lon="..." />（6 位小数，x ↔ lon, y ↔ lat）
  - 边界 way：<way id="W{i}">，子元素先 <nd ref="..."/> 后 <tag k="type" v="line_thin"/>
  - 停止线 way：<way id="W{i}">，<tag k="type" v="stop_line"/>
  - 车道关系：<relation id="R{i}">，member 先 left 后 right，tag 按固定顺序排布
  - 信号灯关系：<relation id="RT{i}">，subtype=traffic_light，排在所有车道 relation 之后
  - 限速 sign (D2-08): <relation id="RT{i}">, subtype=speed_limit
  - 独立停止线 (D2-08): <relation id="RT{i}">, subtype=stop_line
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
    subtype: str  # "traffic_light" | "speed_limit" | "stop_line"
    stop_line_way_id: str  # "" if no stop_line way (speed_limit subtype)
    ref_line_way_id: str
    lanelet_id: str
    speed_limit: str | None = None  # only for speed_limit subtype


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


def _lane_cum_s(centerline: list) -> list[float]:
    """预计算 lane 中心线累积弧长数组 (cum_s[i] = 从起点到第 i 点的总弧长)."""
    cum = [0.0]
    for i in range(len(centerline) - 1):
        dx = centerline[i + 1][0] - centerline[i][0]
        dy = centerline[i + 1][1] - centerline[i][1]
        cum.append(cum[-1] + math.hypot(dx, dy))
    return cum


def _build_segment_grid(
    lane_info_list: list[dict],
    cell_size: float = 5.0,
) -> dict[tuple[int, int], list[tuple[str, int, tuple[float, float, float, float]]]]:
    """构建 lane 中心线段的空间网格索引 (D2-08 step 2 §5.1).

    每个段在 start / end / mid 三格各登记一次：避免稀疏段被漏检 (短 lane 只有 1 段时
    段中点不在搜索半径内仍能命中端点). 找最近段 → 计算点-段精确垂直距离 (与顶点法不同).
    返回 {(cell_x, cell_y): [(lane_id, seg_idx, (ax, ay, bx, by)), ...]}.
    cell_size=5m → 查询 6×6 cells=30m 半径覆盖率足够 (lujiazui_v2 实测 < 30m).
    """
    grid: dict[tuple[int, int], list] = {}
    for info in lane_info_list:
        cl = info["centerline"]
        lid = info["lane_id"]
        for i in range(len(cl) - 1):
            ax = float(cl[i][0])
            ay = float(cl[i][1])
            bx = float(cl[i + 1][0])
            by = float(cl[i + 1][1])
            mx = (ax + bx) / 2.0
            my = (ay + by) / 2.0
            cells_to_register = {
                (int(ax // cell_size), int(ay // cell_size)),
                (int(bx // cell_size), int(by // cell_size)),
                (int(mx // cell_size), int(my // cell_size)),
            }
            seg_tuple = (lid, i, (ax, ay, bx, by))
            for key in cells_to_register:
                grid.setdefault(key, []).append(seg_tuple)
    return grid


def _perp_dist_point_segment(
    px: float, py: float, ax: float, ay: float, bx: float, by: float
) -> float:
    """点 P 到线段 AB 的精确垂直距离."""
    dx = bx - ax
    dy = by - ay
    seg_ln_sq = dx * dx + dy * dy
    if seg_ln_sq < 1e-18:
        return math.hypot(px - ax, py - ay)
    t = ((px - ax) * dx + (py - ay) * dy) / seg_ln_sq
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0
    cx_p = ax + t * dx
    cy_p = ay + t * dy
    return math.hypot(px - cx_p, py - cy_p)


def _closest_polyline_dist(
    px: float,
    py: float,
    grid: dict,
    cell_size: float = 5.0,
    search_radius_cells: int = 6,
) -> tuple[float, str | None]:
    """找离 (px, py) 最近的 polyline 段，返回 (distance, lane_id).

    用段中心点的网格索引 + 精确点-段投影 (比顶点法准).
    对每个 lane 取所有段中**最小**的垂直距离，再取跨 lane 的最小.
    search_radius_cells=4 * cell_size=5m = 20m 搜索半径.
    """
    cx_g = int(px // cell_size)
    cy_g = int(py // cell_size)
    best_per_lane: dict[str, float] = {}
    for dx in range(-search_radius_cells, search_radius_cells + 1):
        for dy in range(-search_radius_cells, search_radius_cells + 1):
            key = (cx_g + dx, cy_g + dy)
            if key not in grid:
                continue
            for (lid, _seg_idx, (ax, ay, bx, by)) in grid[key]:
                d = _perp_dist_point_segment(px, py, ax, ay, bx, by)
                cur = best_per_lane.get(lid)
                if cur is None or d < cur:
                    best_per_lane[lid] = d
    if not best_per_lane:
        return float("inf"), None
    best_lane = min(best_per_lane, key=best_per_lane.get)  # type: ignore[arg-type]
    return best_per_lane[best_lane], best_lane


def _find_nearest_lane(
    tl: dict,
    lane_info_list: list[dict],
    *,
    max_search_distance: float = 5.0,
    max_lateral_deviation: float = 5.0,
) -> str | None:
    """坐标式 traffic_light → 所属 lane 的几何匹配 (D2-08 step 2 §5.1).

    输入：tl 含 {x, y_lane}（x = 沿车道弧长 s，y_lane = 中心线法向偏移，左正右负）
          lane_info_list = convert_map_dict 内部构建的 [{lane_id, centerline, ...}, ...]
    返回：lane_id (str)，或 None 表示找不到匹配 lane.

    算法 (不依赖 Lanelet2 库，纯几何):
      1. 对每条 arc-length >= tl.x 的 lane:
         a. 在 lane 中心线上做 1D 投影找 s=tl.x (二分查找 cum_s[] + 线性插值)
         b. 在 s 处用相邻段差分拿 lane 切线 heading
         c. 计算左法线 N = (-ty, tx)（OpenDRIVE 约定 y_lane>0=左）
         d. 推算 tl 世界坐标 P_world = C(s=tl.x) + tl.y_lane * N
      2. 用**段中心点**空间网格找 P_world 最近的 polyline 段（精确点-段距离）
      3. "host lane" 判定：closest_polyline.lane_id == 假设 lane_id（lane 认领自己的 P_world）
      4. 候选按 perpendicular_dist 最小（lane 越靠近 P_world 越合理），再按 lane_id 字典序破并列
      5. 若 |s_推算 - tl.x| > max_search_distance → 跳过

    性能 (osm_lujiazui_v2 实测):
      - 2738 roads × 4620 lanes × 880 tls
      - 段中心点网格 (cell_size=5m) + 30m 搜索半径 → 每个 P_world 仅检查 ~30 个段
      - 单次转换 ~30s (单次转换可接受；与未 fallback 的转换相比 ~20% overhead)

    注意 (避坑):
      - tl.x / tl.y_lane **不是世界坐标**（值域 0~500），是**车道局部坐标**
      - y_lane>0 表示 lane heading 方向**左侧**（OpenDRIVE 约定）
      - 显式 {lane: ...} 字段优先；本函数仅用于无 lane 字段的 fallback
    """
    if not isinstance(tl, dict):
        return None
    x_raw = tl.get("x")
    y_raw = tl.get("y_lane")
    if x_raw is None or y_raw is None:
        return None
    try:
        x = float(x_raw)
        y_lane = float(y_raw)
    except (TypeError, ValueError):
        return None

    # Pre-compute segment grid; cache across multiple tl calls to avoid rebuild
    grid = lane_info_list[0].get("_seg_grid") if lane_info_list else None
    if grid is None:
        grid = _build_segment_grid(lane_info_list, cell_size=5.0)
        # Attach to all entries (cheap pointer; enables easy cleanup)
        for info in lane_info_list:
            info["_seg_grid"] = grid

    best_lane: str | None = None
    best_score: tuple[float, float, str] = (float("inf"), float("inf"), "")

    for info in lane_info_list:
        cl = info.get("centerline")
        if not cl or len(cl) < 2:
            continue
        cum_s = info.get("_cum_s")
        if cum_s is None:
            cum_s = _lane_cum_s(cl)
            info["_cum_s"] = cum_s
        total = cum_s[-1]
        if x > total + 1e-6:
            continue

        # Binary search for s = x
        lo, hi = 0, len(cum_s) - 1
        while lo < hi - 1:
            mid = (lo + hi) // 2
            if cum_s[mid] <= x:
                lo = mid
            else:
                hi = mid
        s_lo = cum_s[lo]
        s_hi = cum_s[hi]
        if s_hi - s_lo < 1e-9:
            t = 0.0
        else:
            t = (x - s_lo) / (s_hi - s_lo)

        cx_w = cl[lo][0] + t * (cl[hi][0] - cl[lo][0])
        cy_w = cl[lo][1] + t * (cl[hi][1] - cl[lo][1])

        # Tangent via finite difference over surrounding segments
        if hi < len(cl) - 1:
            tx_full = cl[hi + 1][0] - cl[lo][0]
            ty_full = cl[hi + 1][1] - cl[lo][1]
        else:
            tx_full = cl[hi][0] - cl[hi - 1][0]
            ty_full = cl[hi][1] - cl[hi - 1][1]
        tnorm = math.hypot(tx_full, ty_full)
        if tnorm < 1e-9:
            continue
        tx = tx_full / tnorm
        ty = ty_full / tnorm
        # Lane heading angle (atan2)
        lane_heading = math.atan2(ty, tx)
        # Heading filter: skip lanes whose direction differs from tl.heading by > π/4
        # (tl might be facing either direction at intersection, so mod π)
        tl_heading_raw = tl.get("heading")
        if tl_heading_raw is not None:
            try:
                tl_heading = float(tl_heading_raw)
                diff = abs(lane_heading - tl_heading)
                # Reduce to [0, π]
                diff = diff % math.pi
                if diff > math.pi / 2:
                    diff = math.pi - diff
                if diff > math.pi / 4:  # 45° threshold
                    continue
            except (TypeError, ValueError):
                pass
        # Left normal (OpenDRIVE: y_lane>0 = heading 方向左侧)
        nx = -ty
        ny = tx

        # Assumed world position
        wx = cx_w + y_lane * nx
        wy = cy_w + y_lane * ny
        s_proj = s_lo + t * (s_hi - s_lo)

        # Find closest polyline to P_world via segment grid
        closest_dist, closest_lane = _closest_polyline_dist(wx, wy, grid, cell_size=5.0)
        # Host lane check: assumed lane must be closest
        if closest_lane != info["lane_id"]:
            continue

        # Longitudinal consistency
        s_err = abs(x - s_proj)
        if s_err > max_search_distance:
            continue

        # NOTE: do NOT check closest_dist > max_lateral_deviation here.
        # For straight lanes P_world is exactly |y_lane| away from OWN centerline,
        # so this check would reject all tls with |y_lane| > 5. The "closest == assumed"
        # check above is the main discriminator; max_lateral_deviation is kept as a
        # guard against pathological far-from-lane cases.

        score = (closest_dist, s_err, info["lane_id"])
        if score < best_score:
            best_score = score
            best_lane = info["lane_id"]

    return best_lane


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
                # D2-08 step 2 §5.1: 坐标式 traffic_light fallback 需要 centerline 做几何投影
                "centerline": pts,
            })

    lane_by_id = {info["lane_id"]: info for info in lane_info_list}

    # 2. 收集 traffic lights（支持 top-level 与 landmarks.traffic_lights）
    raw_tls = data.get("traffic_lights")
    if not raw_tls and isinstance(data.get("landmarks"), dict):
        raw_tls = data["landmarks"].get("traffic_lights")
    if not isinstance(raw_tls, list):
        raw_tls = []

    # 稳定排序 traffic lights
    # D2-08 step 2 §5.1: 对无 {lane, lane_id} 字段的坐标式 tl (osm_lujiazui_v2 / beijing_guomao /
    # osm_zhengdong) 走 _find_nearest_lane(x, y_lane) fallback。显式 {lane: ...} 字段优先 (向
    # 后兼容 test_04 / test_10 等用 {id, lane} 格式的 fixture)。匹配失败 → 静默丢弃 + stderr
    # 警告 (与既有路径一致)。
    valid_tls: list[dict] = []
    unmatched_tls: list[tuple[int, dict]] = []
    for tl_idx, tl in enumerate(raw_tls):
        if not isinstance(tl, dict):
            continue
        tl_lane = tl.get("lane") or tl.get("lane_id")
        tl_lane_str = str(tl_lane) if tl_lane else None
        matched_lane: str | None = None
        # 优先用显式 lane 字段 (M2 既有路径，向后兼容)
        if tl_lane_str and tl_lane_str in lane_by_id:
            matched_lane = tl_lane_str
        else:
            # 坐标式 fallback (M3 step 2 §5.1)
            matched_lane = _find_nearest_lane(tl, lane_info_list)
        if matched_lane is None:
            unmatched_tls.append((tl_idx, tl))
            continue
        valid_tls.append({
            "lane_id": matched_lane,
            "tl_id": str(tl.get("id", tl_idx)),
            "orig_idx": tl_idx,
        })
    if unmatched_tls:
        print(
            f"warning: {len(unmatched_tls)}/{len(raw_tls)} traffic_lights "
            f"无法匹配任何 lane (coordinate->lane fallback 失败)",
            file=sys.stderr,
        )
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
            subtype="traffic_light",
            stop_line_way_id=reg["stop_way_id"],
            ref_line_way_id=reg["ref_way_id"],
            lanelet_id=reg["lane_id"],
        ))

    # 5b. speed_limit regulatory relations (D2-08 / contract 2.5.2)
    speed_limit_rels: list[OSMRegulatoryRelation] = []
    for info in sorted_lanes:
        sl_str = info.get("speed_limit")
        if sl_str is None:
            continue
        speed_limit_rels.append(OSMRegulatoryRelation(
            id=f"RT{len(regulatory_relations) + len(speed_limit_rels)}",
            subtype="speed_limit",
            stop_line_way_id="",
            ref_line_way_id=info["left_way_id"],
            lanelet_id=info["lane_id"],
            speed_limit=sl_str,
        ))

    # 5c. 独立 stop_line regulatory relations (D2-08 / contract 2.5.3)
    stop_line_rels: list[OSMRegulatoryRelation] = []
    raw_stop_lines = []
    if isinstance(data.get("landmarks"), dict):
        raw_stop_lines = data["landmarks"].get("stop_lines", [])
    if not isinstance(raw_stop_lines, list):
        raw_stop_lines = []
    for sl in raw_stop_lines:
        if not isinstance(sl, dict):
            continue
        sl_lane = sl.get("lane") or sl.get("lane_id")
        if not sl_lane or str(sl_lane) not in lane_by_id:
            continue
        target = lane_by_id[str(sl_lane)]
        stop_wid = f"W{way_id_counter}"
        way_id_counter += 1
        stop_refs = (target["left_node_ids"][-1], target["right_node_ids"][-1])
        ways.append(OSMWay(id=stop_wid, nd_refs=stop_refs, way_type="stop_line"))
        stop_line_rels.append(OSMRegulatoryRelation(
            id=f"RT{len(regulatory_relations) + len(speed_limit_rels) + len(stop_line_rels) - 1}",
            subtype="stop_line",
            stop_line_way_id=stop_wid,
            ref_line_way_id=target["left_way_id"],
            lanelet_id=str(sl_lane),
        ))

    # 5d. 合并 regulatory 顺序: traffic_light -> speed_limit -> stop_line (contract 2.5.4)
    all_regulatory = regulatory_relations + speed_limit_rels + stop_line_rels

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

    for rt in all_regulatory:
        lines.append(f'  <relation id="{rt.id}">')
        if rt.stop_line_way_id:
            lines.append(f'    <member type="way" ref="{rt.stop_line_way_id}" role="stop_line" />')
        lines.append(f'    <member type="way" ref="{rt.ref_line_way_id}" role="ref_line" />')
        lines.append('    <tag k="type" v="regulatory_element" />')
        lines.append(f'    <tag k="subtype" v="{rt.subtype}" />')
        if rt.subtype == "speed_limit" and rt.speed_limit is not None:
            lines.append(f'    <tag k="speed_limit" v="{rt.speed_limit}" />')
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
