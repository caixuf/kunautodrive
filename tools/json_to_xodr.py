#!/usr/bin/env python3
"""
json_to_xodr.py — 场景 JSON → OpenDRIVE (.xodr) 转换器

FlowSim v2 用 esmini RoadManager 处理道路网络，esmini 吃 OpenDRIVE (.xodr)。
本工具把 FlowEngine 场景 JSON 的道路描述转成合法的 xodr，避免手写 XML。

支持两种输入格式：

1. 旧格式 (现有 14 个场景):
   "road": { "curve_start_x": 200, "curve_length_m": 120, "curve_offset_m": 8 }
   → 2 个 road：直线段 (0~curve_start_x) + 弯道段 (单 arc 近似)
   弯道曲率 k ≈ 2*offset/L² (小角度近似：横向偏移 = L²·k/2)

2. 新格式 (FlowSim v2):
   "road_network": {
     "edges": [
       { "id": 0, "type": "urban", "length_m": 200, "lanes": 3, "lane_width": 3.5,
         "speed_limit": 11.11 },
       { "id": 4, "type": "ramp_curve", "length_m": 250, "lanes": 1,
         "curvature_profile": [{"radius": 45, "arc": 130}, ...] }
     ]
   }
   → 每条 edge 一个 road，curvature_profile 拆成多个 <arc> geometry

向后兼容：无 road/road_network 字段时输出单条 1000m 直道（ego 默认场景）。

用法:
  python3 tools/json_to_xodr.py scenarios/zhongkai_road_full.json -o /tmp/zhongkai.xodr
  python3 tools/json_to_xodr.py scenarios/infinite_straight.json   # 输出到 stdout
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Optional


# ── 几何累积器 ──────────────────────────────────────────────

@dataclass
class RoadState:
    """沿 planView 累积的全局起点坐标/朝向，保证段间端点连续。"""
    x: float = 0.0
    y: float = 0.0
    hdg: float = 0.0  # 弧度

    def advance(self, length: float, curvature: float = 0.0) -> "RoadState":
        """沿当前朝向前进 length 米；curvature≠0 时为圆弧，返回段终点状态。"""
        if abs(curvature) < 1e-9:
            # 直线
            nx = self.x + length * math.cos(self.hdg)
            ny = self.y + length * math.sin(self.hdg)
            return RoadState(nx, ny, self.hdg)
        # 圆弧: 转角 dtheta = length * k, 半径 R = 1/k
        k = curvature
        dtheta = length * k
        R = 1.0 / k
        # 圆心在朝向法线方向
        cx = self.x - R * math.sin(self.hdg)
        cy = self.y + R * math.cos(self.hdg)
        nh = self.hdg + dtheta
        nx = cx + R * math.sin(nh)
        ny = cy - R * math.cos(nh)
        return RoadState(nx, ny, nh)


# ── Road 构建器 ─────────────────────────────────────────────

@dataclass
class GeometrySeg:
    s: float          # 沿 road 的起点里程
    x: float          # 全局起点 x
    y: float          # 全局起点 y
    hdg: float        # 起点朝向
    length: float
    curvature: float  # 0 = line, ≠0 = arc


@dataclass
class Road:
    id: int
    name: str
    length: float
    lane_count: int
    lane_width: float
    speed_limit: float
    geoms: list[GeometrySeg] = field(default_factory=list)
    # OpenDRIVE junction linkage (NOA Phase 1: 分叉/汇入路网支持)
    # junction_id >= 0 表示本 road 是某 junction 的 connecting road；
    # predecessor/successor >= 0 时生成 <link>，描述与 incoming/target road 的连接。
    junction_id: int = -1
    predecessor: int = -1   # elementId of predecessor road (<link><predecessor>)
    successor: int = -1     # elementId of successor road (<link><successor>)
    # 高架 elevation profile：list[{"s": float, "h": float}]，按 s 升序。
    # 空列表表示该 road 全程贴地（elevation=0），完全向后兼容。
    # 非空时生成 OpenDRIVE <elevationProfile>，相邻两点之间用线性插值（b=斜率, c=d=0）。
    # esmini 解析后通过 RM_PositionData.z 暴露给 frenet_to_world，再经 scene_pub
    # 透传成 nodes 三元组 [x, y, z]，前端 scene3d.js 用 z 做路面 Y 高度。
    elevation_profile: list = field(default_factory=list)
    oneway: bool = False  # True=单向道路(不生成对向车道)，False=双向对称
    # ── lane 级拓扑（laneLink 生成用，2026-08）──
    # lane_meta: [{id, index, direction, successors:[str]}] —— map.json lanes[]
    # 原样保留。esmini 在 junction 处按 lane 选路（Junction::GetNumberOfRoadConnections）
    # 依赖 <connection><laneLink>，而 laneLink 只能从 lane successors 推导。
    lane_meta: list = field(default_factory=list)
    # <link> 的 junction 版：本 road 起点/终点连到哪个 junction（elementType="junction"）。
    # road 级 predecessor/successor（elementType="road"）由直连 road 填充，两者独立。
    predecessor_junction: int = -1   # 起点连的 junction id
    successor_junction: int = -1     # 终点连的 junction id


@dataclass
class Junction:
    """OpenDRIVE <junction> 描述：道路分叉(fork)或汇入(merge)。
    incoming_road 是分叉前/汇入前的主路 id；connections 中每项为
    (connecting_road_id, target_road_id_or_None)，target 为 None 时
    表示该 connecting road 终点不再接续（fork 的分支末端）。
    lane_links: {connecting_road_id: [(incoming_lane_id, connecting_lane_id), ...]}
    供 <connection><laneLink> 生成（esmini 在路口按 lane 选路必需）。"""
    id: int
    name: str
    incoming_road: int
    connections: list  # list[tuple[int, Optional[int]]]
    lane_links: dict = field(default_factory=dict)  # connecting_road_id -> [(from,to)]


def build_straight_road(rid: int, name: str, length: float, lanes: int,
                        lane_width: float, speed: float, state: RoadState) -> tuple[Road, RoadState]:
    """单直线 road，从 state 起，返回 road 和终点 state。"""
    end = state.advance(length, 0.0)
    road = Road(rid, name, length, lanes, lane_width, speed, [
        GeometrySeg(0.0, state.x, state.y, state.hdg, length, 0.0)
    ])
    return road, end


def build_curve_road_from_profile(rid: int, name: str, profile: list[dict],
                                  lanes: int, lane_width: float, speed: float,
                                  state: RoadState) -> tuple[Road, RoadState]:
    """按 curvature_profile 构建弯道 road，每段一个 geometry（line 或 arc）。
    profile 项: {"radius": R, "arc": L}  radius>0 右弯, <0 左弯, 0/缺省 直线。"""
    geoms: list[GeometrySeg] = []
    s_acc = 0.0
    cur = state
    total = 0.0
    for seg in profile:
        L = float(seg.get("arc", 0.0))
        R = float(seg.get("radius", 0.0))
        k = 0.0 if abs(R) < 1e-9 else 1.0 / R
        geoms.append(GeometrySeg(s_acc, cur.x, cur.y, cur.hdg, L, k))
        cur = cur.advance(L, k)
        s_acc += L
        total += L
    road = Road(rid, name, total, lanes, lane_width, speed, geoms)
    return road, cur


# ── nodes 折线 → 三次 Hermite 密采样折线 ─────────────────────

def _node_tangents(pts: list) -> list:
    """每个节点处的切线 = 相邻两段 chord 方向的归一化和（端点用单 chord）。

    为什么不用 Catmull-Rom：coarse 节点（~200m 间距）+ 直道↔弯道切向不连续时，
    CR 会在过渡处严重过冲（实测生成 R≈19m 的发卡弯，a_lat@20m/s≈21 m/s²，
    车根本拐不过来）。Hermite 的端点切线取 chord 方向平均，过冲小一个量级
    （S 弯实测 min R≈546m，a_lat≈0.7 m/s²），且 C¹ 连续无 kink。
    """
    n = len(pts)
    tang: list[tuple] = []
    for i in range(n):
        if i == 0:
            v = (pts[1][0] - pts[0][0], pts[1][1] - pts[0][1])
        elif i == n - 1:
            v = (pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1])
        else:
            a = (pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1])
            b = (pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
            v = (a[0] + b[0], a[1] + b[1])
        L = math.hypot(v[0], v[1])
        tang.append((v[0] / L, v[1] / L) if L > 1e-9 else (1.0, 0.0))
    return tang


def _hermite(p0: tuple, p1: tuple, m0: tuple, m1: tuple, t: float) -> tuple:
    """三次 Hermite 插值，t∈[0,1]，m0/m1 为端点切线向量（含弦长缩放）。"""
    t2 = t * t
    t3 = t2 * t
    h00 = 2.0 * t3 - 3.0 * t2 + 1.0
    h10 = t3 - 2.0 * t2 + t
    h01 = -2.0 * t3 + 3.0 * t2
    h11 = t3 - t2
    x = h00 * p0[0] + h10 * m0[0] + h01 * p1[0] + h11 * m1[0]
    y = h00 * p0[1] + h10 * m0[1] + h01 * p1[1] + h11 * m1[1]
    return x, y


def build_polyline_road(rid: int, name: str, nodes: list, lanes: int,
                        lane_width: float, speed: float,
                        state: RoadState) -> tuple[Road, RoadState]:
    """按 nodes 折线（[x, y, z] 三元组）构建弯道 road。

    nodes 是场景 JSON 里道路中心线的采样点 —— 前端 scene3d.js 与评估器
    demo_evaluator 消费的正是同一份，物理层（esmini）此前却把它们丢弃，
    只按 length_m 生成一条直线（curve_road 场景 S 弯变直道 → 车不跟弯）。

    实现：三次 Hermite 样条（端点切线 = 相邻 chord 方向平均）过全部 nodes，
    按 SPLINE_STEP 密采样后每段一条 <line> geometry。这样：
    - 精确穿过每个节点（前端/评估器同点），直道段保持直道。
    - C¹ 连续无 polyline kink；过冲小（S 弯 min R≈546m，20m/s 可安全通过）。
    - ref_path 的 kappa 是 sample_ahead 对采样 heading 做中心差分得到的
      （route.cpp），不读 planView 曲率属性 —— 折线密采样对 kappa 前馈
      完全够用，不需要真 <arc>。
    """
    pts = [(float(p[0]), float(p[1]), float(p[2]) if len(p) > 2 else 0.0)
           for p in nodes]
    n = len(pts)
    if n < 2:
        # 退化：退回直线
        return build_straight_road(rid, name, 1000.0, lanes, lane_width,
                                   speed, state)

    # 全共线（含 2 节点直道）：一条直线即可，不要密采样成 600 段。
    collinear = True
    for i in range(2, n):
        v1 = (pts[i - 1][0] - pts[0][0], pts[i - 1][1] - pts[0][1])
        v2 = (pts[i][0] - pts[0][0], pts[i][1] - pts[0][1])
        if abs(v1[0] * v2[1] - v1[1] * v2[0]) > 1e-6:
            collinear = False
            break
    if collinear:
        chord = math.hypot(pts[-1][0] - pts[0][0], pts[-1][1] - pts[0][1])
        hdg = math.atan2(pts[-1][1] - pts[0][1], pts[-1][0] - pts[0][0])
        road = Road(rid, name, chord, lanes, lane_width, speed, [
            GeometrySeg(0.0, pts[0][0], pts[0][1], hdg, chord, 0.0),
        ])
        end_state = RoadState(pts[-1][0], pts[-1][1], hdg)
        return road, end_state

    tang = _node_tangents(pts)
    samples: list[tuple] = []  # (x, y, z)
    for i in range(n - 1):
        p0 = pts[i]
        p1 = pts[i + 1]
        L = math.hypot(p1[0] - p0[0], p1[1] - p0[1])
        m0 = (tang[i][0] * L, tang[i][1] * L)
        m1 = (tang[i + 1][0] * L, tang[i + 1][1] * L)
        nstep = max(1, int(math.ceil(L / SPLINE_STEP)))
        for s in range(nstep):
            t = s / nstep
            x, y = _hermite((p0[0], p0[1]), (p1[0], p1[1]), m0, m1, t)
            z = p0[2] + (p1[2] - p0[2]) * t
            samples.append((x, y, z))
    samples.append((pts[n - 1][0], pts[n - 1][1], pts[n - 1][2]))

    # 去重（相邻采样点过近，避免零长段 / heading 差分噪声）
    dedup: list[tuple] = []
    for s in samples:
        if dedup and math.hypot(s[0] - dedup[-1][0], s[1] - dedup[-1][1]) < 0.1:
            continue
        dedup.append(s)
    samples = dedup

    geoms: list[GeometrySeg] = []
    s_acc = 0.0
    elev: list = [{"s": 0.0, "h": samples[0][2]}]
    for i in range(len(samples) - 1):
        x0, y0, _ = samples[i]
        x1, y1, z1 = samples[i + 1]
        seg_len = math.hypot(x1 - x0, y1 - y0)
        if seg_len < 1e-9:
            continue
        hdg = math.atan2(y1 - y0, x1 - x0)
        geoms.append(GeometrySeg(s_acc, x0, y0, hdg, seg_len, 0.0))
        s_acc += seg_len
        elev.append({"s": s_acc, "h": z1})

    total = s_acc
    road = Road(rid, name, total if total > 0 else 1.0, lanes, lane_width,
                speed, geoms)
    # z 全 0 时不生成 elevationProfile（与既有场景一致，向后兼容）
    if any(abs(p[2]) > 1e-6 for p in pts) and len(elev) > 1:
        road.elevation_profile = elev
    end_hdg = geoms[-1].hdg if geoms else state.hdg
    end_state = RoadState(samples[-1][0], samples[-1][1], end_hdg)
    return road, end_state


# ── 场景 JSON → Road 列表 ──────────────────────────────────

DEFAULT_LANE_WIDTH = 3.5
DEFAULT_SPEED = 13.89  # 50 km/h

# nodes 折线 Hermite 密采样步长（m）。5m 一段 → 每段 Δh≈κ·5≈0.03 rad
# （R≈200m），heading 连续无 kink，esmini 位置查询开销可忽略。
SPLINE_STEP = 5.0

def roads_from_legacy_road(road_cfg: dict) -> list[Road]:
    """旧格式 road{curve_start_x, curve_length_m, curve_offset_m} → 2 个 road。"""
    cstart = float(road_cfg.get("curve_start_x", 0.0))
    clen = float(road_cfg.get("curve_length_m", 0.0))
    coff = float(road_cfg.get("curve_offset_m", 0.0))
    lanes = 2
    lw = DEFAULT_LANE_WIDTH
    sp = DEFAULT_SPEED
    state = RoadState(0.0, 0.0, 0.0)
    roads: list[Road] = []
    if cstart > 0:
        r, state = build_straight_road(0, "urban", cstart, lanes, lw, sp, state)
        roads.append(r)
    if clen > 0:
        # 小角度近似: offset = L²·k/2  →  k = 2·offset/L²
        k = (2.0 * coff / (clen * clen)) if clen > 0 else 0.0
        end = state.advance(clen, k)
        roads.append(Road(
            id=len(roads), name="curve", length=clen, lane_count=lanes,
            lane_width=lw, speed_limit=sp,
            geoms=[GeometrySeg(0.0, state.x, state.y, state.hdg, clen, k)]
        ))
        state = end
    if not roads:
        # 纯直道
        r, _ = build_straight_road(0, "urban", 1000.0, lanes, lw, sp, state)
        roads.append(r)
    return roads


def _xodr_lane_id(id_num: int) -> int:
    """map.json lane id 后缀 N → OpenDRIVE lane id。
    约定（map_compiler/extract_city_map）：正向 lane id 1..100（xodr right 侧 =
    负数 id），对向 lane id 101..200（xodr left 侧 = 正数 id）。"""
    return -id_num if id_num <= 100 else (id_num - 100)


def _parse_lane_succ(succ: str) -> tuple[Optional[str], Optional[int]]:
    """lane successor 形如 "<road_str>.lane.<N>" → (road 字符串 id, lane 后缀 N)。"""
    if not isinstance(succ, str):
        return None, None
    road_str, sep, tail = succ.rpartition(".lane.")
    if not sep or not tail.isdigit():
        return None, None
    return road_str, int(tail)


def roads_from_road_network(rn_cfg: dict) -> tuple[list[Road], list[Junction]]:
    """新格式 road_network{edges:[...], junctions[...]} → (roads, junctions)。

    每个 edge 一个 road；edges 之间默认顺序拼接（端点连续）。
    junctions 数组描述道路分叉(fork)/汇入(merge)：

    ── laneLink 生成（2026-08 修复，esmini 路口 lane 级选路必需）──
    esmini 的 Junction::GetNumberOfRoadConnections(road_id, lane_id) 遍历
    connection 的 <laneLink> 条目（RoadManager.cpp:5772-5792），缺 laneLink
    时返回 0 → RM_PositionMoveForward 失败 → NPC 无法跨越路口。
    生成逻辑：对于每个 fork junction，从 incoming road 的 lane_meta 提取
    successors（形如 "<road>.lane.<N>"），解析出目标 road 数字 id + lane index，
    转换为 xodr lane id（forward 侧 = -index, oncoming 侧 = +(index-100)）
    生成 <laneLink from="incoming_lane_id" to="connecting_lane_id"/>。
    同时给 incoming road 设 successor_junction（<link> 指向 junction 而非 road），
    connecting road 设 predecessor_junction（<link> 指向 junction 而非 incoming）。
    """
    edges = rn_cfg.get("edges") or rn_cfg.get("segments") or []
    state = RoadState(0.0, 0.0, 0.0)
    roads: list[Road] = []
    end_states: dict[int, RoadState] = {}   # road id → 该 road 终点的全局状态
    start_states: dict[int, RoadState] = {}  # road id → 该 road 起点的全局状态
    # 字符串 road id → 数字索引（load_scenario 注入，lane successors 解析用）
    road_id_map: dict = rn_cfg.get("road_id_map") or {}

    # 第一遍：顺序构建所有 edge，记录每段终点状态供 junction 分支起算。
    # 同时为前后 edge 串接 successor/predecessor link —— 这是 OpenDRIVE 道路拓扑
    # 连贯性的核心：缺了这个 NPC 沿 road 0 走到 s=250m 就"撞墙"，不会自动进入
    # road 1（esmini 把它们当成 5 条独立断头路）。
    # 注：edges 列表里如果混了 fork connecting_road（id 出现在 junctions[*].connecting_roads
    # 里），其 predecessor 由 fork 配置接管（line 256-275），这里跳过主路 link。
    # 因此先建好 edges 拿到 id 集合，但串接 link 时需先知道哪些 id 是 fork connecting_road。
    junction_conn_ids: set[int] = set()
    junctions_cfg_pre = rn_cfg.get("junctions") or []
    for jcfg in junctions_cfg_pre:
        for c in (jcfg.get("connecting_roads") or []):
            cid = int(c.get("id", -1))
            if cid >= 0:
                junction_conn_ids.add(cid)

    main_edge_ids: list[int] = []
    for i, e in enumerate(edges):
        eid = int(e.get("id", i))
        # skip_main_link：fork connecting_road 已在 junctions 里接管 link 关系
        skip_main_link = eid in junction_conn_ids
        etype = str(e.get("type", "road"))
        length = float(e.get("length_m", 0.0))
        lane_v = e.get("lanes", e.get("lane_count", 2))
        lane_meta: list = []
        if isinstance(lane_v, list):
            # 新格式：lanes[] 含 id/index/direction/successors（laneLink 生成用）。
            # id 形如 "<road>.lane.<N>"，N 与 id 后缀一致（正向 1..100、对向 101..200）。
            lanes = int(e.get("lane_count", len(lane_v)))
            for _li, _l in enumerate(lane_v):
                if not isinstance(_l, dict):
                    continue
                _lid = str(_l.get("id", ""))
                _, _sep, _tail = _lid.rpartition(".lane.")
                _id_num = int(_tail) if (_sep and _tail.isdigit()) else int(_l.get("index", _li + 1))
                lane_meta.append({
                    "id_num": _id_num,
                    "index": int(_l.get("index", _li + 1)),
                    "direction": int(_l.get("direction", 1)),
                    "successors": list(_l.get("successors", []) or []),
                })
        else:
            lanes = int(lane_v)
        lw = float(e.get("lane_width", DEFAULT_LANE_WIDTH))
        sp = float(e.get("speed_limit", DEFAULT_SPEED))
        oneway = bool(e.get("oneway", False))
        profile = e.get("curvature_profile")
        nodes = e.get("nodes")
        if nodes and len(nodes) >= 2:
            # nodes 折线优先：前端/评估器用同一份 nodes 画路/判出路沿，
            # 物理层必须跟随 nodes 而不是只看 length_m（否则 S 弯变直道）。
            r, state = build_polyline_road(
                eid, etype, nodes, lanes, lw, sp, state)
        elif profile:
            r, state = build_curve_road_from_profile(
                eid, etype, profile, lanes, lw, sp, state)
        else:
            r, state = build_straight_road(eid, etype, length, lanes, lw, sp, state)
        r.oneway = oneway
        # 匝道(ramp)类道路默认单向
        if "ramp" in etype.lower():
            r.oneway = True
        # 高架 elevation_profile 透传（ [{"s":0,"h":0},{"s":250,"h":8}] ）
        r.elevation_profile = list(e.get("elevation_profile") or [])
        r.lane_meta = lane_meta
        roads.append(r)
        end_states[eid] = state
        start_states[eid] = RoadState(r.geoms[0].x, r.geoms[0].y, r.geoms[0].hdg) if r.geoms else state
        main_edge_ids.append((eid, skip_main_link))

    # 为顺序拼接的主路 edge 串接 successor/predecessor link（OpenDRIVE 拓扑连贯性）。
    # 这是仿真世界"车开不出收费广场"等问题的根因：road 0/1/2/3/4 之前完全断头。
    # 跳过 fork connecting_road（其 link 由 junctions 接管）。
    main_road_by_id = {r.id: r for r in roads if r.junction_id == -1}
    for i in range(1, len(main_edge_ids)):
        prev_id, prev_skip = main_edge_ids[i - 1]
        curr_id, curr_skip = main_edge_ids[i]
        if prev_skip or curr_skip:
            continue
        if prev_id not in main_road_by_id or curr_id not in main_road_by_id:
            continue
        prev_r = main_road_by_id[prev_id]
        curr_r = main_road_by_id[curr_id]
        if prev_r.junction_id == -1 and curr_r.junction_id == -1:
            if prev_r.successor < 0:
                prev_r.successor = curr_id
            if curr_r.predecessor < 0:
                curr_r.predecessor = prev_id

    junctions: list[Junction] = []
    junctions_cfg = rn_cfg.get("junctions") or []
    for jcfg in junctions_cfg:
        try:
            jid = int(jcfg.get("id", 100 + len(junctions)))
        except (TypeError, ValueError):
            # Reserved semantic junctions have no OpenDRIVE connecting roads yet.
            continue
        jtype = str(jcfg.get("type", "fork"))
        incoming = int(jcfg.get("incoming_road", -1))
        if incoming < 0:
            continue
        start_state = end_states.get(incoming, RoadState())

        if jtype == "fork":
            # 分叉：每条 connecting_road 从 incoming_road 终点起独立构建。
            # 并生成 laneLink + junction link（让 esmini 在路口按 lane 选路）。
            conns: list[tuple[int, Optional[int]]] = []
            conn_lane_links: dict[int, list[tuple[int, int]]] = {}  # cid -> [(from,to)]
            # 找 incoming road 对象（用 road id 匹配）
            inc_road = None
            for r in roads:
                if r.id == incoming:
                    inc_road = r
                    break
            # 判断 fork 在 incoming 的哪一端：connecting road 几何起点 vs
            # incoming 终点/起点（city_grid 拆段模型里一个 road 段两端各一交叉口，
            # 需区分末端 fork（successor）与起点 fork（predecessor））。
            inc_end = end_states.get(incoming)
            inc_start = start_states.get(incoming)
            fork_at_end: Optional[bool] = None
            for r2 in roads:
                if not r2.geoms:
                    continue
                if any(int(c.get("id", -1)) == r2.id for c in jcfg.get("connecting_roads", [])):
                    cs = (r2.geoms[0].x, r2.geoms[0].y)
                    if inc_end and abs(cs[0] - inc_end.x) + abs(cs[1] - inc_end.y) < 4.0:
                        fork_at_end = True
                    elif inc_start and abs(cs[0] - inc_start.x) + abs(cs[1] - inc_start.y) < 4.0:
                        fork_at_end = False
                    if fork_at_end is not None:
                        break
            if inc_road is not None:
                if fork_at_end is True:
                    inc_road.successor_junction = jid
                elif fork_at_end is False:
                    inc_road.predecessor_junction = jid

            for c in jcfg.get("connecting_roads", []):
                cid = int(c.get("id", -1))
                if cid < 0:
                    continue
                cname = str(c.get("name", f"conn_{cid}"))
                clen = float(c.get("length_m", 0.0))
                clanes = int(c.get("lanes", c.get("lane_count", 1)))
                clw = float(c.get("lane_width", DEFAULT_LANE_WIDTH))
                csp = float(c.get("speed_limit", DEFAULT_SPEED))
                cprofile = c.get("curvature_profile")
                # 分支各自从分叉点起算，互不影响：拷贝起点状态
                # 如果连接道路 id 已存在（已在 edges 中定义），则更新它的
                # junction_id/predecessor 而非重新构建。
                existing = None
                for r in roads:
                    if r.id == cid:
                        existing = r
                        break
                if existing:
                    cr = existing
                    cr.junction_id = jid
                    cr.predecessor_junction = jid  # 指向 junction 而非 incoming road
                    cend = end_states.get(cid, start_state)
                else:
                    c_nodes = c.get("nodes")
                    if c_nodes and len(c_nodes) >= 2:
                        cr, cend = build_polyline_road(
                            cid, cname, c_nodes, clanes, clw, csp, start_state)
                    elif cprofile:
                        cr, cend = build_curve_road_from_profile(
                            cid, cname, cprofile, clanes, clw, csp, start_state)
                    else:
                        cr, cend = build_straight_road(cid, cname, clen, clanes, clw, csp, start_state)
                    cr.junction_id = jid
                    cr.predecessor_junction = jid
                    cr.elevation_profile = list(c.get("elevation_profile") or [])
                    roads.append(cr)
                # 若配置了 target_road，分支末端接续到目标主路
                tgt = c.get("target_road")
                if tgt is not None:
                    cr.successor = int(tgt)
                    conns.append((cid, int(tgt)))
                else:
                    conns.append((cid, None))
                end_states[cid] = cend

                # ── laneLink 生成 ──
                # 从 incoming road 的 lane_meta 中找 successors 指向 cid 的 lane，
                # 每个这种 lane 生成 (from_xodr_lane, to_xodr_lane)。
                succ_lane_pairs: list[tuple[int, int]] = []
                if inc_road is not None:
                    for lm in inc_road.lane_meta:
                        for s in lm.get("successors", []):
                            src_road_str, src_lane_idx = _parse_lane_succ(s)
                            if src_road_str is None:
                                continue
                            src_road_num = road_id_map.get(src_road_str, -1)
                            if src_road_num == cid and src_lane_idx is not None:
                                from_lane = _xodr_lane_id(lm["id_num"])
                                to_lane = _xodr_lane_id(src_lane_idx)
                                succ_lane_pairs.append((from_lane, to_lane))
                if succ_lane_pairs:
                    conn_lane_links[cid] = succ_lane_pairs
            junc_obj = Junction(jid, f"fork_{jid}", incoming, conns)
            junc_obj.lane_links = conn_lane_links
            junctions.append(junc_obj)

        elif jtype == "merge":
            # 汇入：incoming_road（加速车道）→ 新建短 connecting road → target_road（主线）。
            # OpenDRIVE 要求 connectingRoad ≠ incomingRoad，所以必须生成独立的过渡段。
            target = int(jcfg.get("target_road", -1))
            inc_state = end_states.get(incoming, RoadState())

            # 生成唯一 connecting road id（基于 junction id，避免碰撞）
            conn_rid = jid * 100 + 1
            existing_ids = {r.id for r in roads}
            while conn_rid in existing_ids:
                conn_rid += 1

            # 沿用 incoming road 的车道/限速参数
            conn_sp = DEFAULT_SPEED
            conn_lw = DEFAULT_LANE_WIDTH
            conn_lanes = 1
            for rd in roads:
                if rd.id == incoming:
                    conn_sp = rd.speed_limit
                    conn_lw = rd.lane_width
                    conn_lanes = rd.lane_count
                    break

            # 从 incoming road 终点起构建短过渡段（几何接续，拓扑独立）
            conn_len = 5.0
            conn_road, conn_end = build_straight_road(
                conn_rid, f"merge_{jid}_conn", conn_len, conn_lanes, conn_lw, conn_sp, inc_state)
            conn_road.junction_id = jid
            conn_road.predecessor = incoming
            if target >= 0:
                conn_road.successor = target
            roads.append(conn_road)
            end_states[conn_rid] = conn_end

            conns2: list[tuple[int, Optional[int]]] = [(conn_rid, target if target >= 0 else None)]
            junctions.append(Junction(jid, f"merge_{jid}", incoming, conns2))

    if not roads:
        r, _ = build_straight_road(0, "urban", 1000.0, 2, DEFAULT_LANE_WIDTH,
                                   DEFAULT_SPEED, RoadState())
        roads.append(r)

    # ── cross_roads：横向交叉道路（十字路口）──────────────────
    # 每条 cross_road 在主路上某个 s 位置垂直分叉，长度对称向两侧延伸，
    # 朝向 = 主路 hdg ± π/2（默认 +π/2 = 朝左 / 北方向）。
    # 这不是 fork/merge junction（esmini junction 拓扑复杂），而是独立的"地面道路"
    # 直接画在主路垂直方向上——视觉上形成十字路口形状即可，ego 不走 cross_road。
    # junction_id 保持 -1（普通道路）以保证 OpenDRIVE 合法性：若用 -2 等自定义值
    # 会要求配对 <junction id="-2"> 元素，esmini 校验失败会拒绝整个 xodr。
    cross_cfg = rn_cfg.get("cross_roads") or []
    # 计算主路在 s 位置的全局 (x, y, hdg)。先找主路上 s 落在哪个 edge 内。
    # 主路 = edges 顺序拼接，每段 length 累加。
    main_cum_lengths: list[tuple[int, float, float, float, float]] = []  # (edge_id, s_start, s_end, x_start, y_start, hdg_start)
    cum_s = 0.0
    cx, cy, chdg = 0.0, 0.0, 0.0
    for rd in roads:
        # 只看 edge 主路（junction_id == -1），跳过 fork/merge connecting_road
        if rd.junction_id != -1:
            continue
        # 用第一段几何的起点状态
        if rd.geoms:
            g0 = rd.geoms[0]
            main_cum_lengths.append((rd.id, cum_s, cum_s + rd.length, g0.x, g0.y, g0.hdg))
        else:
            main_cum_lengths.append((rd.id, cum_s, cum_s + rd.length, cx, cy, chdg))
        cum_s += rd.length

    for ci, cc in enumerate(cross_cfg):
        at_s = float(cc.get("at_s", 0.0))
        clen = float(cc.get("length_m", 200.0))
        clanes = int(cc.get("lanes", 2))
        clw = float(cc.get("lane_width", DEFAULT_LANE_WIDTH))
        csp = float(cc.get("speed_limit", DEFAULT_SPEED))
        direction = int(cc.get("direction", 1))  # +1=左/北(+Y), -1=右/南
        # 找到 at_s 落在哪个主路段，插值出全局 (x, y, hdg)
        target_state = None
        for ent in main_cum_lengths:
            eid, s_start, s_end, ex, ey, ehdg = ent
            if s_start <= at_s <= s_end + 1e-6:
                # 沿 ehdg 方向前进 (at_s - s_start) 米
                offset = at_s - s_start
                target_state = RoadState(
                    ex + offset * math.cos(ehdg),
                    ey + offset * math.sin(ehdg),
                    ehdg)
                break
        if target_state is None:
            continue
        # cross_road 起点放在交叉位置，朝向 = 主路 hdg + π/2 * direction
        cross_hdg = target_state.hdg + (math.pi / 2) * direction
        # 道路从交叉点向 +hdg 方向延伸 clen/2，向 -hdg 方向延伸 clen/2
        # 为符合 OpenDRIVE 几何（从 s=0 沿 +s 方向），把起点放在 -clen/2 处
        start_x = target_state.x - (clen / 2) * math.cos(cross_hdg)
        start_y = target_state.y - (clen / 2) * math.sin(cross_hdg)
        cross_state = RoadState(start_x, start_y, cross_hdg)
        cid = 1000 + ci  # 避免与 edges/junction id 冲突
        cname = str(cc.get("name", f"cross_road_{ci}"))
        cr, _ = build_straight_road(cid, cname, clen, clanes, clw, csp, cross_state)
        # 保持 junction_id = -1（普通道路），避免要求配对 <junction> 元素
        roads.append(cr)

    return roads, junctions


# ── Road 列表 → xodr XML ───────────────────────────────────

def road_to_xml(road: Road) -> ET.Element:
    r = ET.Element("road", {
        "name": road.name,
        "length": f"{road.length:.6f}",
        "id": str(road.id),
        "junction": str(road.junction_id),
    })
    # NOA Phase 1: <link> 描述 connecting road 与 incoming/target road 的拓扑连接，
    # esmini RoadManager 据此构建分叉/汇入路网。
    # 2026-08：road 级 predecessor/successor 用 elementType="road"（直连），
    # predecessor_junction/successor_junction 用 elementType="junction"（过路口）。
    if (road.predecessor >= 0 or road.successor >= 0 or
            road.predecessor_junction >= 0 or road.successor_junction >= 0):
        link = ET.SubElement(r, "link")
        if road.predecessor >= 0:
            ET.SubElement(link, "predecessor", {
                "elementId": str(road.predecessor),
                "elementType": "road",
                "contactPoint": "end",
            })
        if road.successor >= 0:
            ET.SubElement(link, "successor", {
                "elementId": str(road.successor),
                "elementType": "road",
                "contactPoint": "start",
            })
        if road.predecessor_junction >= 0:
            ET.SubElement(link, "predecessor", {
                "elementId": str(road.predecessor_junction),
                "elementType": "junction",
            })
        if road.successor_junction >= 0:
            ET.SubElement(link, "successor", {
                "elementId": str(road.successor_junction),
                "elementType": "junction",
            })
    pv = ET.SubElement(r, "planView")
    for g in road.geoms:
        geom = ET.SubElement(pv, "geometry", {
            "s": f"{g.s:.6f}",
            "x": f"{g.x:.6f}",
            "y": f"{g.y:.6f}",
            "hdg": f"{g.hdg:.6f}",
            "length": f"{g.length:.6f}",
        })
        if abs(g.curvature) < 1e-9:
            ET.SubElement(geom, "line")
        else:
            ET.SubElement(geom, "arc", {"curvature": f"{g.curvature:.9f}"})

    # 高架 elevationProfile：按 elevation_profile 列表生成分段线性 3 阶多项式。
    # 相邻两点 (s_i, h_i) → (s_{i+1}, h_{i+1}) 之间：
    #   a = h_i, b = (h_{i+1} - h_i) / (s_{i+1} - s_i), c = d = 0
    # 末尾补一个终止点锁定最后一个 h 到 road.length。
    # 空列表跳过（esmini 默认 elevation=0，向后兼容）。
    if road.elevation_profile:
        ep = ET.SubElement(r, "elevationProfile")
        pts = sorted(road.elevation_profile, key=lambda p: float(p.get("s", 0)))
        # 起点缺失时补 (0, 0)
        if not pts or float(pts[0].get("s", 0)) > 1e-6:
            pts.insert(0, {"s": 0.0, "h": 0.0})
        for i, p in enumerate(pts):
            s_i = float(p.get("s", 0))
            h_i = float(p.get("h", 0))
            # 计算斜率 b：用下一个点，没有下一个点则 b=0
            if i + 1 < len(pts):
                s_next = float(pts[i + 1].get("s", s_i))
                h_next = float(pts[i + 1].get("h", h_i))
                ds = s_next - s_i
                b = (h_next - h_i) / ds if abs(ds) > 1e-9 else 0.0
            else:
                b = 0.0
            ET.SubElement(ep, "elevation", {
                "s": f"{s_i:.6f}",
                "a": f"{h_i:.6f}",
                "b": f"{b:.9f}",
                "c": "0.0",
                "d": "0.0",
            })
        # 终止点：确保最后一段之后 elevation 保持不变到 road.length
        last_s = float(pts[-1].get("s", 0))
        last_h = float(pts[-1].get("h", 0))
        if last_s < road.length - 1e-6:
            ET.SubElement(ep, "elevation", {
                "s": f"{road.length:.6f}",
                "a": f"{last_h:.6f}",
                "b": "0.0", "c": "0.0", "d": "0.0",
            })

    # 判断是否单向道路：匝道(ramp)、加速车道等名称包含 ramp/oneway 或显式 oneway=True
    is_oneway = ("ramp" in road.name.lower() or
                 road.lane_count == 1 or
                 getattr(road, "oneway", False))
    # 车道：center(参考线 width=0) + right N 条顺向 + left N 条对向(双向道路)
    # 场景 JSON 的 "lanes" 是**双向合计**车道数（如 4 = 2 顺行 + 2 对向）。
    # 单向道路（ramp/oneway）时 lanes 全部在 right 侧。
    # 旧实现误把 lanes 当"每侧车道数"，导致 4 车道场景生成 8 条 drivable lane，
    # control_node 的 lane_count=8 使 lane_center_y 映射全错、oncoming 检查误判。
    n_per_side = road.lane_count if is_oneway else max(1, road.lane_count // 2)
    lanes = ET.SubElement(r, "lanes")
    ls = ET.SubElement(lanes, "laneSection", {"s": "0.0"})
    center = ET.SubElement(ls, "center")
    cl = ET.SubElement(center, "lane", {"id": "0", "type": "none"})
    ET.SubElement(cl, "width", {"a": "0", "b": "0", "c": "0", "d": "0"})
    # right 侧：顺向车道（id=-1,-2,...,-n_per_side）
    right = ET.SubElement(ls, "right")
    for li in range(1, n_per_side + 1):
        ln = ET.SubElement(right, "lane", {"id": f"-{li}", "type": "driving"})
        ET.SubElement(ln, "width", {
            "s": "0.0",
            "a": f"{road.lane_width:.4f}", "b": "0", "c": "0", "d": "0"
        })
    # left 侧：对向车道（id=+1,+2,...,+n_per_side），双向道路才生成
    if not is_oneway:
        left = ET.SubElement(ls, "left")
        for li in range(1, n_per_side + 1):
            ln = ET.SubElement(left, "lane", {"id": f"{li}", "type": "driving"})
            ET.SubElement(ln, "width", {
                "s": "0.0",
                "a": f"{road.lane_width:.4f}", "b": "0", "c": "0", "d": "0"
            })
    # 限速 via type/speed (OpenDRIVE 用 road type 元素)
    t = ET.SubElement(r, "type", {"s": "0.0", "type": "town"})
    ET.SubElement(t, "speed", {"max": f"{road.speed_limit:.4f}", "unit": "m/s"})
    return r


def build_xodr(roads: list[Road], junctions: list[Junction] | None = None) -> ET.Element:
    root = ET.Element("OpenDRIVE")
    ET.SubElement(root, "header", {
        "revMajor": "1", "revMinor": "4", "name": "FlowEngine",
        "version": "1.4", "date": "",
        "north": "0", "south": "0", "east": "0", "west": "0",
    })
    for rd in roads:
        root.append(road_to_xml(rd))
    # NOA Phase 1: <junction> 元素声明分叉/汇入拓扑。esmini 用其解析 connecting
    # road 与 incoming road 的连接关系，是表达匝道分叉/加速车道汇入的关键。
    for j in (junctions or []):
        je = ET.SubElement(root, "junction", {
            "name": j.name,
            "id": str(j.id),
        })
        for ci, (conn_road, _target) in enumerate(j.connections):
            # target 仅用于 connecting road 的 <successor>，已在 road_to_xml 的 <link> 中表达
            conn_el = ET.SubElement(je, "connection", {
                "id": str(ci),
                "incomingRoad": str(j.incoming_road),
                "connectingRoad": str(conn_road),
                "contactPoint": "start",
            })
            # 2026-08: laneLink —— esmini 在 junction 处按 lane 选路必需
            # （Junction::GetNumberOfRoadConnections 遍历 <laneLink>，缺则返回 0）。
            for from_lane, to_lane in (j.lane_links.get(conn_road) or []):
                ET.SubElement(conn_el, "laneLink", {
                    "from": str(from_lane),
                    "to": str(to_lane),
                })
    return root


# ── 主入口 ─────────────────────────────────────────────────

def convert(scenario: dict) -> str:
    """场景 dict → xodr XML 字符串。"""
    junctions: list[Junction] = []
    if "road_network" in scenario:
        roads, junctions = roads_from_road_network(scenario["road_network"])
    elif "road" in scenario:
        roads = roads_from_legacy_road(scenario["road"])
    else:
        r, _ = build_straight_road(0, "urban", 1000.0, 2, DEFAULT_LANE_WIDTH,
                                   DEFAULT_SPEED, RoadState())
        roads = [r]
    root = build_xodr(roads, junctions)
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="unicode", xml_declaration=True)


def load_scenario(path: Path) -> dict:
    """Load a scenario and resolve an optional reusable map reference."""
    with path.open("r", encoding="utf-8") as f:
        scenario = json.load(f)
    if not isinstance(scenario, dict):
        raise ValueError(f"{path}: scenario root must be an object")
    map_ref = scenario.get("map_file")
    if map_ref is None and isinstance(scenario.get("map_id"), str):
        map_ref = f"maps/{scenario['map_id']}/map.json"
    if map_ref is None:
        return scenario
    map_path = Path(map_ref)
    if not map_path.is_absolute():
        candidates = (
            path.parent / map_path,
            path.parent.parent / map_path,
            Path.cwd() / map_path,
        )
        map_path = next((candidate for candidate in candidates if candidate.is_file()), candidates[0])
    with map_path.open("r", encoding="utf-8") as f:
        city_map = json.load(f)
    roads = city_map.get("roads", [])
    if not isinstance(roads, list) or not roads:
        raise ValueError(f"{map_path}: roads must be a non-empty array")
    route_file = scenario.get("route_file")
    route_id = scenario.get("route_id")
    if route_file and route_id:
        route_path = Path(route_file)
        if not route_path.is_absolute():
            route_path = next(
                (candidate for candidate in (
                    path.parent / route_path,
                    path.parent.parent / route_path,
                    Path.cwd() / route_path,
                ) if candidate.is_file()),
                route_path,
            )
        with route_path.open("r", encoding="utf-8") as f:
            route_data = json.load(f)
        selected = next(
            (route for route in route_data.get("routes", [])
             if route.get("id") == route_id),
            None,
        )
        if selected is None:
            raise ValueError(f"{route_path}: route not found: {route_id}")
        chain = selected.get("road_chain", [])
        by_id = {road.get("id"): road for road in roads}
        missing = [road_id for road_id in chain if road_id not in by_id]
        if missing:
            raise ValueError(f"{route_path}: route {route_id} references missing roads {missing}")
        roads = [by_id[road_id] for road_id in chain]
    # road 字符串 id → 数字索引映射（map.json 用字符串 road/lane id，
    # json_to_xodr 内部用数字 edge id，junctions 引用需换算到同一套索引）。
    name_to_idx: dict = {}
    edges = []
    for index, road in enumerate(roads):
        edge = dict(road)
        name_to_idx[str(edge.get("id"))] = index
        edge["id"] = edge.get("legacy_id", index)
        if "nodes" not in edge and "centerline" in edge:
            edge["nodes"] = edge["centerline"]
        lane_data = edge.get("lanes")
        if isinstance(lane_data, list):
            # 保留 lanes[]（含 id/index/direction/successors），供 junction laneLink
            # 生成（esmini 路口 lane 级转向必需）；另加 lane_count 数字字段给计数消费方。
            edge["lane_count"] = len(lane_data)
            if lane_data and isinstance(lane_data[0], dict):
                edge["lane_width"] = lane_data[0].get(
                    "width", edge.get("lane_width", DEFAULT_LANE_WIDTH)
                )
        elif isinstance(lane_data, int):
            edge["lane_count"] = lane_data
        edge.pop("legacy_id", None)
        edges.append(edge)

    # 把 map.json 的 junctions（字符串 road id）换算成数字索引；非字符串透传，
    # 向后兼容既有数字 id / reserved 占位 junction。
    def _map_id(v):
        if isinstance(v, str):
            return name_to_idx.get(v, -1)
        return v

    juncs = []
    for jcfg in city_map.get("junctions", []):
        jc = dict(jcfg)
        if "incoming_road" in jc:
            jc["incoming_road"] = _map_id(jc["incoming_road"])
        if "target_road" in jc:
            jc["target_road"] = _map_id(jc["target_road"])
        conns = []
        for c in jc.get("connecting_roads", []):
            cc = dict(c)
            if "id" in cc:
                cc["id"] = _map_id(cc["id"])
            if "target_road" in cc:
                cc["target_road"] = _map_id(cc["target_road"])
            conns.append(cc)
        jc["connecting_roads"] = conns
        juncs.append(jc)

    resolved = dict(scenario)
    resolved["road_network"] = {
        "edges": edges,
        "cross_roads": city_map.get("cross_roads", []),
        "junctions": juncs,
        # 字符串 road id → 数字索引（lane successors 形如 "<road>.lane.<N>" 需要
        # 解析回数字 road id 才能与 connection 的 connectingRoad 对上，生成 laneLink）
        "road_id_map": name_to_idx,
    }
    return resolved


def main() -> int:
    ap = argparse.ArgumentParser(description="FlowEngine 场景 JSON → OpenDRIVE xodr")
    ap.add_argument("scenario", help="场景 JSON 文件路径")
    ap.add_argument("-o", "--output", help="输出 xodr 路径 (默认 stdout)")
    args = ap.parse_args()

    scenario_path = Path(args.scenario).resolve()
    scenario = load_scenario(scenario_path)
    xml = convert(scenario)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(xml)
        print(f"✓ {args.scenario} → {args.output} ({len(xml)} bytes)", file=sys.stderr)
    else:
        print(xml)
    return 0


if __name__ == "__main__":
    sys.exit(main())
