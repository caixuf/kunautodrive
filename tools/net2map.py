#!/usr/bin/env python3
"""net2map.py — SUMO net.xml → FlowEngine map.json（车道级几何直出）。

背景：osm_to_map.py 手搓车道几何（中心线偏移合成 + 路口聚类），陆家嘴实测
"很多路看着很奇怪"。SUMO netconvert 吃同一份 OSM 直接产出车道级精确形状
（含路口内部连接车道 shape），本脚本把它翻译成 map.json 契约，
flowsim(json_to_xodr) / flowboard(/api/map/preview) / A*(astar_route) /
评估器(road_network_from_map_file) 全部零改动消费。

坐标链：net.xml shape（UTM − netOffset）→ 加回 |netOffset| 得 UTM → pyproj
逆投影到 WGS84 → 项目统一 ENU 近似（与 osm_to_map.wgs84_to_enu 同公式，
需 --ref-lat/--ref-lon 与当初建图一致），保证新旧地图同坐标系。

契约要点（与 json_to_xodr.load_scenario / scene_pub / astar_route 对齐）：
  * 每个 SUMO edge（单方向）→ 一条 road，oneway=true；
    centerline = 车行道**行进方向左边界**（最左车道中心线左移半宽），
    json_to_xodr 把 oneway 车道全部放参考线右侧，几何自洽。
  * lane 序号：lane.1 = 最左车道（贴道路中线）= SUMO 最大 index；
    lane.N = 最右（贴路肩）= SUMO index 0。人行道/自行车道被过滤。
  * 路口内部 edge（function="internal"）→ connector road（type 继承 to-edge），
    junctions[] 按 (junction, from-edge) 分组为 fork 契约，lane successors
    串起 from-lane → 内部 lane → to-lane，json_to_xodr 据此生成 laneLink。
  * 红绿灯暂不映射：landmarks.traffic_lights 消费契约是直路 x/y_lane 模型，
    SUMO tlLogic 塞进去是死数据，留待 TL 契约升级。

用法：
  python3 tools/net2map.py /tmp/lujiazui.net.xml --out maps/osm_lujiazui_v2 \
      --ref-lat 31.235 --ref-lon 121.5 [--buildings-from maps/osm_lujiazui/map.json] \
      [--scenario scenarios/osm_lujiazui_v2.json]

产出（与 osm_to_map 同契约，下游零改动消费）：
  <out>/map.json      — 车道级路网（roads + junctions + landmarks + buildings）
  <out>/routes.json   — main 路线 = 最长几何连接真实道路链（自动生成）
  --scenario 可选写出 scenarios/<map_id>.json 场景入口（默认不写，沿用既有）

依赖：pyproj（pip install pyproj）。
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

# ── 与 osm_to_map.py 相同的 ENU 近似参数 ─────────────────────
M_PER_DEG_LAT = 111320.0

# highway 类型 → 引擎道路类型（与 osm_to_map 一致，输入为 net.xml 的
# "highway.secondary" 形式，先剥前缀再查表）
HIGHWAY_TO_TYPE = {
    "motorway": "primary", "motorway_link": "primary",
    "trunk": "primary", "trunk_link": "primary",
    "primary": "primary", "primary_link": "primary",
    "secondary": "secondary", "secondary_link": "secondary",
    "tertiary": "secondary", "tertiary_link": "secondary",
    "unclassified": "residential", "residential": "residential",
    "living_street": "residential", "service": "residential",
    "road": "urban",
}

# 机动车类别（SUMO lane allow/disallow 判定）
VEHICLE_CLASSES = {
    "passenger", "private", "emergency", "bus", "coach", "truck", "trailer",
    "delivery", "taxi", "motorcycle", "moped",
}

# 整条 edge 强制跳过的类型（无论 lane allow 如何）
SKIP_EDGE_TYPES = {
    "highway.footway", "highway.pedestrian", "highway.path",
    "highway.cycleway", "highway.steps", "highway.construction",
    "railway.light_rail", "railway.rail", "railway.tram",
}

# SUMO connection dir → map.json junction turn
DIR_TO_TURN = {"s": "straight", "l": "left", "r": "right", "t": "uturn",
               "L": "left", "R": "right", "partially": "straight"}


def wgs84_to_enu(lat: float, lon: float, ref_lat: float, ref_lon: float) -> tuple[float, float]:
    """与 osm_to_map.wgs84_to_enu 同公式。"""
    x = (lon - ref_lon) * M_PER_DEG_LAT * math.cos(math.radians(ref_lat))
    y = (lat - ref_lat) * M_PER_DEG_LAT
    return x, y


def sanitize_id(raw: str) -> str:
    """SUMO id → map.json 安全 id：'-100#0'→'r100s0'，':123_0'→'j123_0'。"""
    s = raw
    if s.startswith(":"):
        s = "j" + s[1:]
    s = s.replace("-", "r").replace("#", "s")
    return s


def parse_shape(shape: str) -> list[tuple[float, float]]:
    pts = []
    for tok in (shape or "").split():
        x, y = tok.split(",")[:2]
        pts.append((float(x), float(y)))
    return pts


def offset_left(pts: list[tuple[float, float]], d: float) -> list[tuple[float, float]]:
    """折线沿行进方向左法线偏移 d 米（端点用相邻段法线，内部点用平均法线）。

    SUMO shape 是密采样折线（典型间距 <5m），不做 miter 截断也足够准。
    """
    if len(pts) < 2 or d == 0.0:
        return list(pts)
    out = []
    n = len(pts)
    for i, (px, py) in enumerate(pts):
        txs = tys = 0.0
        cnt = 0
        if i > 0:
            dx, dy = px - pts[i - 1][0], py - pts[i - 1][1]
            ln = math.hypot(dx, dy)
            if ln > 1e-9:
                txs += dx / ln; tys += dy / ln; cnt += 1
        if i < n - 1:
            dx, dy = pts[i + 1][0] - px, pts[i + 1][1] - py
            ln = math.hypot(dx, dy)
            if ln > 1e-9:
                txs += dx / ln; tys += dy / ln; cnt += 1
        if cnt == 0:
            out.append((px, py))
            continue
        tl = math.hypot(txs, tys)
        if tl < 1e-9:
            txs, tys = 1.0, 0.0
        else:
            txs, tys = txs / tl, tys / tl
        out.append((px - tys * d, py + txs * d))  # 左法线 = (-ty, tx)
    return out


def lane_drivable(lane: ET.Element) -> bool:
    """SUMO lane 是否机动车道（过滤人行道/自行车道/纯公交道）。"""
    allow = lane.get("allow")
    if allow:
        classes = set(allow.split())
        return bool(classes & VEHICLE_CLASSES)
    disallow = lane.get("disallow")
    if disallow:
        classes = set(disallow.split())
        if "all" in classes:
            return False
        return bool(VEHICLE_CLASSES - classes)
    return True  # 无限制 = 全车辆可行


# ── routes.json / scenario 生成（与 osm_to_map 对齐，复用同一契约） ──
def road_length(road: dict) -> float:
    """road 几何长度（中心折线累计，ENU 米）。"""
    cl = road.get("centerline") or []
    return sum(math.hypot(cl[i + 1][0] - cl[i][0], cl[i + 1][1] - cl[i][1])
               for i in range(len(cl) - 1))


def _road_end_headings(road: dict) -> tuple:
    """(start_pt, end_pt, start_heading, end_heading)；heading 为 ENU atan2(dy,dx)。"""
    cl = road.get("centerline") or [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0]]
    if len(cl) < 2:
        z = (cl[0][0], cl[0][1])
        return z, z, 0.0, 0.0
    s, e = (cl[0][0], cl[0][1]), (cl[-1][0], cl[-1][1])
    sh = math.atan2(cl[1][1] - cl[0][1], cl[1][0] - cl[0][0])
    eh = math.atan2(cl[-1][1] - cl[-2][1], cl[-1][0] - cl[-2][0])
    return s, e, sh, eh


def _angle_diff(a: float, b: float) -> float:
    d = (a - b) % (2 * math.pi)
    if d > math.pi:
        d -= 2 * math.pi
    return d


def _road_name(r: dict) -> str:
    return (r.get("name") or "").strip()


def detect_junctions(roads: list[dict], snap: float = 35.0) -> list[dict]:
    """端点几何聚类找路口：把彼此 ≤snap 的真实 road 端点聚成一簇。

    只聚真实 road（排除路口内部 connector），供 generate_connectivity 用。
    返回 [{x, y, members:[(real_idx, is_end)]}]，仅保留 ≥2 条不同 road 的真实路口。
    snap 略大于 build_main_chain 的 30m，保证几何邻接的链路对也落进同一簇。
    """
    real = [r for r in roads if not str(r.get("sumo_id", "")).startswith(":")]
    eps = []
    for i, r in enumerate(real):
        s, e, _sh, _eh = _road_end_headings(r)
        eps.append((s[0], s[1], i, False))
        eps.append((e[0], e[1], i, True))
    clusters = []
    for (x, y, i, is_end) in eps:
        best, bd = -1, snap * snap
        for ci, c in enumerate(clusters):
            d = (x - c["x"]) ** 2 + (y - c["y"]) ** 2
            if d < bd:
                bd = d
                best = ci
        if best >= 0:
            c = clusters[best]
            n = len(c["members"])
            c["x"] = (c["x"] * n + x) / (n + 1)
            c["y"] = (c["y"] * n + y) / (n + 1)
            c["members"].append((i, is_end))
        else:
            clusters.append({"x": x, "y": y, "members": [(i, is_end)]})
    return [c for c in clusters
            if len({real[m[0]]["id"] for m in c["members"]}) >= 2]


def generate_connectivity(roads: list[dict], junctions: list[dict],
                          snap: float = 35.0,
                          uturn_tol: float = math.radians(50)) -> None:
    """为每条 lane 生成 successors 几何邻接兜底（全路网，替代仅 main 链串接）。

    SUMO `<connection>` 只连「真实 road → 内部 connector → 真实 road」，但贪心接出的
    main 链按几何直接拼真实 road（跳过 connector），导致相邻真实 road 之间缺 lane
    后继 → A* / ego 在路口断链。此处按端点几何聚类（detect_junctions）给每对路口内
    相邻真实 road 的车道补 successors（直行按 index 对齐、转向按角度，跳过不同街道
    的掉头），使全路网可驾驶、main 链连通。已是纯追加，不覆盖 SUMO 精确后继。
    """
    real = [r for r in roads if not str(r.get("sumo_id", "")).startswith(":")]
    info = {r["id"]: _road_end_headings(r) for r in real}
    for j in junctions:
        for (ri, is_end) in j["members"]:
            cur = real[ri]
            _cs, _ce, csh, ceh = info[cur["id"]]
            cur_exit_h = ceh if is_end else (csh + math.pi)
            cur_lanes = cur["lanes"]
            if not cur_lanes:
                continue
            for (ci, c_is_end) in j["members"]:
                if ci == ri:
                    continue
                cand = real[ci]
                _ps, _pe, psh, peh = info[cand["id"]]
                cand_entry_h = (peh + math.pi) if c_is_end else psh
                turn = _angle_diff(cand_entry_h, cur_exit_h)
                same_street = _road_name(cur) == _road_name(cand) and _road_name(cur) != ""
                if not same_street and abs(turn) > math.pi - uturn_tol:
                    continue  # 跳过不同街道的掉头
                for cl in cur_lanes:
                    best_l = min(cand["lanes"], key=lambda l: abs(l["index"] - cl["index"]))
                    sid = best_l["id"]
                    if sid not in cl["successors"]:
                        cl["successors"].append(sid)


def _build_successor_graph(roads: list[dict]):
    """从 lane successors 推出「真实 road → 真实 road」有向邻接（跳过路口内部
    connector）。net2map 的 road 已是 SUMO 单方向 edge，lane successors 经内部
    connector 精确连到下游 road —— 这是路网真实可驾驶拓扑，比几何邻接更可靠。
    返回 (by_id, nxt, prev)：nxt/prev 为 road_id → [road_id]。
    """
    real = [r for r in roads if not str(r.get("sumo_id", "")).startswith(":")]
    by_id = {r["id"]: r for r in real}
    lane_road: dict[str, str] = {}
    for r in real:
        for l in r["lanes"]:
            lane_road[l["id"]] = r["id"]
    internal = {r["id"] for r in real if r["sumo_id"].startswith(":")}

    def resolve_real(lane_id: str, depth: int = 0):
        """lane 经内部 connector 最终落入的真实 road（最多跳 2 层）。"""
        rid = lane_road.get(lane_id)
        if rid is None or rid not in by_id:
            return None
        if rid not in internal:
            return rid
        if depth >= 2:
            return None
        for il in by_id[rid]["lanes"]:
            if il["id"] == lane_id:
                for s in il["successors"]:
                    t = resolve_real(s, depth + 1)
                    if t:
                        return t
        return None

    nxt: dict[str, list[str]] = {r["id"]: [] for r in real}
    for r in real:
        out: list[str] = []
        for l in r["lanes"]:
            for s in l["successors"]:
                t = resolve_real(s)
                if t and t != r["id"]:
                    out.append(t)
        # 去重保序
        seen = set()
        nxt[r["id"]] = [t for t in out if not (t in seen or seen.add(t))]
    prev: dict[str, list[str]] = {r["id"]: [] for r in real}
    for a, nbrs in nxt.items():
        for b in nbrs:
            prev[b].append(a)
    return by_id, nxt, prev


def build_main_chain(roads: list[dict], snap: float = 30.0,
                     max_len: int = 120) -> list[str]:
    """构造 main 路线链：沿**真实可驾驶拓扑（lane successors）**做最长简单路径，
    而非几何邻接。这样保证相邻段必有 lane 后继、ego 能一路开到底，且不会把道路
    接到自家逆行副本或死胡同支路上。

    只走**正向 road**（`sumo_id` 不以 `-` 开头，排除 SUMO 逆向 edge），避免 main 路线
    在自家逆行副本上绕 u-turn 圈。种子取「下游可达长度最大」的正向 road（主干动脉），
    向前/向后贪心扩展，每步选转向最直、平局取下游最长的未访问邻居。
    """
    real = [r for r in roads
            if not str(r.get("sumo_id", "")).startswith(":")
            and not str(r.get("sumo_id", "")).startswith("-")]
    if not real:
        return []
    by_id, nxt, prev = _build_successor_graph(real)
    hdg = {r["id"]: _road_end_headings(r) for r in real}

    # 下游可达长度：记忆化 DP（迭代后序遍历，显式栈避免深递归；环以 instack 守卫
    # 断开，仅为种子/平局估计，非精确最长路径）。memo 跨调用持久化 → 整体 O(N)。
    memo: dict[str, float] = {}

    def downstream(r: str) -> float:
        if r in memo:
            return memo[r]
        result: dict[str, float] = {}
        instack: set[str] = set()
        stack: list = [(r, False)]
        while stack:
            node, processed = stack.pop()
            if node in result:
                continue
            if processed:
                best = 0.0
                for nb in nxt.get(node, []):
                    best = max(best, road_length(by_id[nb]) + memo.get(nb, 0.0))
                result[node] = best
                instack.discard(node)
                continue
            if node in instack:   # 环守卫：回边跳过，断开环
                continue
            instack.add(node)
            stack.append((node, True))
            for nb in nxt.get(node, []):
                if nb not in result and nb not in instack:
                    stack.append((nb, False))
        for k, v in result.items():
            memo[k] = v
        return result.get(r, 0.0)

    def turn(a: str, b: str) -> float:
        sa, ea, sha, eha = hdg[a]
        sb, eb, shb, ehb = hdg[b]
        return abs(_angle_diff(shb, eha))  # a 末端 → b 首端

    # 种子：下游可达长度最大（主干动脉）
    seed = max(real, key=lambda r: road_length(r) + downstream(r["id"]))["id"]

    chain = [seed]
    visited = {seed}
    # 向前扩展
    while len(chain) < max_len:
        cur = chain[-1]
        cands = [(nb, turn(cur, nb)) for nb in nxt.get(cur, [])
                 if nb not in visited]
        if not cands:
            break
        cands.sort(key=lambda x: (x[1], -downstream(x[0])))  # 直行优先，平局取下游长
        nxt_id = cands[0][0]
        chain.append(nxt_id)
        visited.add(nxt_id)
    # 向后扩展（用 prev 邻接）
    while len(chain) < max_len:
        cur = chain[0]
        cands = [(p, turn(p, cur)) for p in prev.get(cur, [])
                 if p not in visited]
        if not cands:
            break
        cands.sort(key=lambda x: (x[1], -downstream(x[0])))
        p = cands[0][0]
        chain.insert(0, p)
        visited.add(p)
    return chain



def scenario_ego(chain_roads: list[dict], lane_width: float = 3.0):
    """场景 ego 初位置：main 链首条 road 第一条正向车道中心线起点 + 航向。"""
    if not chain_roads:
        return 0.0, 0.0, 0.0
    r0 = chain_roads[0]
    lanes = r0.get("lanes") or []
    lane = min(lanes, key=lambda l: l.get("index", 1)) if lanes else None
    cl = (lane or r0).get("centerline") or [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0]]
    p0 = cl[0]
    p1 = cl[1] if len(cl) > 1 else cl[0]
    heading = math.atan2(p1[1] - p0[1], p1[0] - p0[0])
    return float(p0[0]), float(p0[1]), heading


def write_scenario(out_dir: str, scenario_path: str, map_id: str,
                  chain_roads: list[dict]) -> None:
    """写场景文件（与 osm_to_map.write_scenario 同契约）。"""
    os.makedirs(os.path.dirname(os.path.abspath(scenario_path)), exist_ok=True)
    rel = os.path.relpath(out_dir, os.path.dirname(os.path.abspath(scenario_path)))
    rel = rel.replace(os.sep, "/")
    ex, ey, eh = scenario_ego(chain_roads)
    scenario = {
        "name": map_id,
        "description": "OSM 路网（SUMO netconvert，net2map.py 生成）：ego 沿 route 'main' 行驶。",
        "map_file": os.path.join(rel, "map.json"),
        "route_file": os.path.join(rel, "routes.json"),
        "route_id": "main",
        "random_seed": 202608,
        "duration_s": 120,
        "lighting": "day",
        "weather": "clear",
        "visibility_m": 1200,
        "npc_lane_change": True,
        "ego": {
            "x": round(ex, 3), "y": round(ey, 3), "heading": round(eh, 4),
            "init_speed": 10.0, "target_speed": 13.0,
            "wheelbase": 2.7, "length": 4.6, "width": 2.0, "max_steer": 0.6,
        },
        "actors": [],
        "pass_criteria": {
            "no_collision": True, "max_duration_s": 120,
            "min_avg_speed_mps": 3.0, "min_distance_m": 450,
        },
        "route": [], "scenarios": [],
        "choreography": {"loop_period_s": 999, "beats": []},
    }
    with open(scenario_path, "w", encoding="utf-8") as f:
        json.dump(scenario, f, indent=2, ensure_ascii=False)


class NetConverter:
    def __init__(self, net_path: str, ref_lat: float, ref_lon: float):
        tree = ET.parse(net_path)
        self.root = tree.getroot()
        self.ref_lat, self.ref_lon = ref_lat, ref_lon

        loc = self.root.find("location")
        if loc is None:
            raise ValueError("net.xml 缺 <location>（投影信息）")
        self.net_off = tuple(float(v) for v in loc.get("netOffset").split(","))
        proj = loc.get("projParameter", "")
        m = re.search(r"\+zone=(\d+)", proj)
        if not m:
            raise ValueError(f"无法从 projParameter 解析 UTM zone: {proj}")
        from pyproj import Transformer  # 延迟导入，--help 不需要
        self.to_wgs84 = Transformer.from_crs(
            f"EPSG:326{int(m.group(1)):02d}", "EPSG:4326", always_xy=True)

        # 索引
        self.edges: dict[str, ET.Element] = {}
        self.internal_edges: dict[str, ET.Element] = {}
        for e in self.root.findall("edge"):
            if e.get("function") == "internal":
                self.internal_edges[e.get("id")] = e
            else:
                self.edges[e.get("id")] = e
        self.junctions = {j.get("id"): j for j in self.root.findall("junction")}
        self.connections = self.root.findall("connection")

        # 每个 edge 的可驾驶 lane 映射：sumo_lane_idx → 我们的 index（1=最左）
        self.lane_index_map: dict[str, dict[int, int]] = {}
        self.drivable_lanes: dict[str, list[ET.Element]] = {}
        for eid, e in {**self.edges, **self.internal_edges}.items():
            lanes = [l for l in e.findall("lane") if lane_drivable(l)]
            lanes.sort(key=lambda l: int(l.get("index", "0")))
            self.drivable_lanes[eid] = lanes
            n = len(lanes)
            self.lane_index_map[eid] = {
                int(l.get("index", "0")): n - pos for pos, l in enumerate(lanes)
            }

        # road id：有街名用街名（可读），重名/无名靠 edge id 兜底唯一
        self.road_id: dict[str, str] = {}
        used: set[str] = set()
        for eid in list(self.edges) + list(self.internal_edges):
            rid = self._make_road_id(eid, used)
            self.road_id[eid] = rid

    def _make_road_id(self, eid: str, used: set) -> str:
        e = self.edges.get(eid)
        if e is None:
            e = self.internal_edges.get(eid)
        name = e.get("name") if e is not None else None
        base = (name or "road").strip().replace(" ", "_")
        rid = f"{base}_{sanitize_id(eid)}"
        while rid in used:
            rid += "_"
        used.add(rid)
        return rid

    # ── 坐标变换 ─────────────────────────────────────────────
    def to_enu(self, x: float, y: float) -> tuple[float, float]:
        ux, uy = x - self.net_off[0], y - self.net_off[1]  # netOffset 为负 → 加回
        lon, lat = self.to_wgs84.transform(ux, uy)
        return wgs84_to_enu(lat, lon, self.ref_lat, self.ref_lon)

    def shape_enu(self, shape: str) -> list[list[float]]:
        return [[*self.to_enu(x, y), 0.0] for x, y in parse_shape(shape)]

    # ── 主转换 ───────────────────────────────────────────────
    def convert(self) -> dict:
        roads: list[dict] = []
        # lane successors：lane_id → [lane_id]
        succ: dict[str, list[str]] = {}

        def lane_lid(eid: str, sumo_idx: int) -> str | None:
            our = self.lane_index_map.get(eid, {}).get(sumo_idx)
            if our is None:
                return None
            return f"{self.road_id[eid]}.lane.{our}"

        # ── 第一步：connection → successors + junction 分组 ──
        # junctions_out: (junc_sumo_id, from_eid) → {turn 信息, shape}
        junc_groups: dict[tuple[str, str], list[dict]] = {}
        junc_of_internal: dict[str, str] = {}   # internal edge id → junction sumo id
        target_of_internal: dict[str, str] = {}  # internal edge id → to edge id
        for c in self.connections:
            fe, te = c.get("from"), c.get("to")
            if fe not in self.edges or te not in self.edges:
                continue
            fl, tl = int(c.get("fromLane", "0")), int(c.get("toLane", "0"))
            src = lane_lid(fe, fl)
            if src is None:
                continue
            via = c.get("via")
            if via:
                ie, il = via.rsplit("_", 1)
                mid = lane_lid(ie, int(il))
                if mid is None:
                    continue
                succ.setdefault(src, []).append(mid)
                dst = lane_lid(te, tl)
                if dst is not None:
                    succ.setdefault(mid, []).append(dst)
                target_of_internal[ie] = te
                # 归属 junction：internal edge id ":J_0" 的前缀 ":J"
                jid_sumo = ie.rsplit("_", 1)[0]
                junc_of_internal[ie] = jid_sumo
                turn = DIR_TO_TURN.get(c.get("dir", "s"), "straight")
                grp = junc_groups.setdefault((jid_sumo, fe), [])
                if not any(g["id"] == ie for g in grp):
                    grp.append({"id": ie, "turn": turn})
            else:
                dst = lane_lid(te, tl)
                if dst is not None:
                    succ.setdefault(src, []).append(dst)

        # ── 第二步：roads（普通 + connector） ────────────────
        def emit_road(eid: str, is_internal: bool) -> dict | None:
            lanes = self.drivable_lanes.get(eid, [])
            if not lanes:
                return None
            e = (self.edges if not is_internal else self.internal_edges)[eid]
            leftmost = lanes[-1]  # SUMO index 最大 = 最左车道
            lw = float(leftmost.get("width") or 3.2)
            ref_pts = offset_left(parse_shape(leftmost.get("shape")), lw / 2.0)
            centerline = [[*self.to_enu(x, y), 0.0] for x, y in ref_pts]
            n = len(lanes)
            lane_entries = []
            has_reverse = f"-{eid}" in self.edges or (
                eid.startswith("-") and eid[1:] in self.edges)
            for pos, l in enumerate(lanes):
                our_idx = n - pos
                marks = []
                if our_idx == 1:
                    marks.append({"type": "double_yellow" if has_reverse
                                  else "solid_white", "side": "left"})
                marks.append({"type": "solid_white" if our_idx == n
                              else "dashed_white", "side": "right"})
                lid = f"{self.road_id[eid]}.lane.{our_idx}"
                lane_entries.append({
                    "id": lid,
                    "index": our_idx,
                    "width": float(l.get("width") or 3.2),
                    "direction": 1,
                    "centerline": self.shape_enu(l.get("shape")),
                    "markings": marks,
                    "successors": succ.get(lid, []),
                })
            raw_type = e.get("type", "")
            hwy = raw_type.split(".", 1)[1] if "." in raw_type else raw_type
            rtype = HIGHWAY_TO_TYPE.get(hwy, "urban")
            if is_internal:
                tgt = target_of_internal.get(eid)
                if tgt is not None:
                    te = self.edges.get(tgt)
                    traw = te.get("type", "") if te is not None else ""
                    thwy = traw.split(".", 1)[1] if "." in traw else traw
                    rtype = HIGHWAY_TO_TYPE.get(thwy, rtype)
            speeds = [float(l.get("speed", "13.89")) for l in lanes]
            road = {
                "id": self.road_id[eid],
                "name": e.get("name") or "",
                "type": rtype,
                "speed_limit": max(speeds),
                "oneway": True,
                "lane_width": lw,
                "centerline": centerline,
                "lanes": lane_entries,
                "sumo_id": eid,
            }
            if is_internal:
                tgt = target_of_internal.get(eid)
                if tgt is not None and tgt in self.road_id:
                    road["target_road"] = self.road_id[tgt]
            return road

        skipped = 0
        for eid, e in self.edges.items():
            if e.get("type", "") in SKIP_EDGE_TYPES:
                skipped += 1
                continue
            r = emit_road(eid, is_internal=False)
            if r is not None:
                roads.append(r)
        for eid in self.internal_edges:
            r = emit_road(eid, is_internal=True)
            if r is not None:
                roads.append(r)

        # ── 第三步：junctions（fork 契约） ───────────────────
        junctions_out: list[dict] = []
        jid_seq = 100
        for (jid_sumo, fe), conns in sorted(junc_groups.items()):
            j = self.junctions.get(jid_sumo)
            shape = []
            if j is not None and j.get("shape"):
                shape = [list(self.to_enu(x, y)) for x, y in parse_shape(j.get("shape"))]
            entry = {
                "id": jid_seq,
                "type": "fork",
                "incoming_road": self.road_id[fe],
                "connecting_roads": [
                    {"id": self.road_id[g["id"]], "turn": g["turn"]}
                    for g in conns if g["id"] in self.road_id
                ],
            }
            if shape:
                entry["shape"] = shape
            if entry["connecting_roads"]:
                junctions_out.append(entry)
                jid_seq += 1

        return {
            "schema_version": 1,
            "map_id": "",  # 由调用方填
            "name": "",
            "generator": "net2map.py (SUMO netconvert)",
            "roads": roads,
            "connections": [],
            "junctions": junctions_out,
            "landmarks": {"traffic_lights": [], "stop_lines": [],
                          "construction_zones": []},
            "buildings": [],
            "_stats": {"skipped_non_vehicle_edges": skipped},
        }


def main() -> int:
    ap = argparse.ArgumentParser(description="SUMO net.xml → FlowEngine map.json")
    ap.add_argument("net", help="netconvert 输出的 .net.xml")
    ap.add_argument("--out", required=True, help="输出地图目录（写 map.json）")
    ap.add_argument("--ref-lat", type=float, required=True, help="ENU 原点纬度")
    ap.add_argument("--ref-lon", type=float, required=True, help="ENU 原点经度")
    ap.add_argument("--buildings-from", default=None,
                    help="可选：从既有 map.json 拷贝 buildings/landmarks")
    ap.add_argument("--scenario", default=None,
                    help="可选：写出场景入口 scenarios/<map_id>.json（默认不写）")
    ap.add_argument("--map-id", default=None, help="默认取 --out 目录基名")
    args = ap.parse_args()

    conv = NetConverter(args.net, args.ref_lat, args.ref_lon)
    doc = conv.convert()

    map_id = args.map_id or os.path.basename(os.path.abspath(args.out))
    doc["map_id"] = map_id
    doc["name"] = f"SUMO {map_id}"

    # buildings/landmarks 继承：显式 --buildings-from 优先，否则保留 out 目录
    # 既有 map.json 的（覆盖生成时建筑不丢）。
    inherit = args.buildings_from
    existing = os.path.join(args.out, "map.json")
    if inherit is None and os.path.isfile(existing):
        inherit = existing
    if inherit and os.path.isfile(inherit):
        with open(inherit, encoding="utf-8") as f:
            old = json.load(f)
        doc["buildings"] = old.get("buildings", [])
        old_lm = old.get("landmarks")
        if isinstance(old_lm, dict):
            doc["landmarks"] = old_lm

    # 全路网几何邻接兜底：SUMO connection 只连「road→内部connector→road」，
    # 贪心 main 链直接拼真实 road，相邻真实 road 间缺 lane 后继 → A*/ego 路口断链。
    # 此处按端点几何聚类补 successors（纯追加，不覆盖 SUMO 精确后继）。
    gen_junc = detect_junctions(doc["roads"])
    generate_connectivity(doc["roads"], gen_junc)

    stats = doc.pop("_stats")
    os.makedirs(args.out, exist_ok=True)
    out_path = os.path.join(args.out, "map.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False)

    n_lanes = sum(len(r["lanes"]) for r in doc["roads"])
    n_conn = sum(1 for r in doc["roads"] if r["sumo_id"].startswith(":"))
    n_succ = sum(len(l["successors"]) for r in doc["roads"] for l in r["lanes"])
    print("wrote %s" % out_path)
    print("  roads: %d（其中路口 connector %d）" % (len(doc["roads"]), n_conn))
    print("  lanes: %d（lane successors %d 条）" % (n_lanes, n_succ))
    print("  junctions: %d, buildings: %d" %
          (len(doc["junctions"]), len(doc["buildings"])))
    print("  skipped non-vehicle edges: %d" % stats["skipped_non_vehicle_edges"])

    # ── routes.json（main 路线 = 最长几何连接真实道路链） ──
    by_id = {r["id"]: r for r in doc["roads"]}
    chain_ids = build_main_chain(doc["roads"])
    chain_roads = [by_id[i] for i in chain_ids if i in by_id]
    routes_doc = {
        "map_id": map_id,
        "routes": [
            {
                "id": "main",
                "name": "SUMO 主线（最长几何连接真实道路链，%d 段）" % len(chain_roads),
                "kind": "main",
                "road_chain": [r["id"] for r in chain_roads],
                "lane_direction": 1,
            }
        ],
        "reserved_turns": [],
    }
    routes_path = os.path.join(args.out, "routes.json")
    with open(routes_path, "w", encoding="utf-8") as f:
        json.dump(routes_doc, f, ensure_ascii=False)
    print("  routes.json 写入 %s（main 链 %d 段）" % (routes_path, len(chain_roads)))

    # ── 可选场景入口 ──
    if args.scenario:
        write_scenario(args.out, os.path.abspath(args.scenario), map_id, chain_roads)

    print("下一步：")
    print("  python3 tools/astar_route.py %s --from-lane <A> --to-lane <B> --write"
          % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
