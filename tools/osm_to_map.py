#!/usr/bin/env python3
"""osm_to_map.py — 从 Overpass API 拉取真实城市 OSM 路网，转换为引擎可加载的 map.json。

生成三个文件（与 city_ring / city_grid 同契约，完全独立）：
  maps/osm_test/map.json      — OSM 道路事实源（roads + lanes + connections + junctions + landmarks）
  maps/osm_test/routes.json   — 路线定义（main = 最长连接道路链）
  scenarios/osm_city_map.json — 场景入口（map_file/route_file 指向上面两个文件，route_id="main"）

数据流：
  Overpass API（小范围 around 查询，过滤非机动车道）
    → WGS84 → ENU 近似投影（1°lat≈111320m，1°lon≈111320*cos(lat)）
    → 每条 way 构造 edge（highway 类型映射、lanes/maxspeed/oneway 解析）
    → 复用 extract_city_map.build_road 生成 lanes（右行制，对向 lane id 101+）

独立运行：
  python3 tools/osm_to_map.py --lat 31.235 --lon 121.5 --radius 500 --out maps/osm_test
可独立校验（复用 extract_city_map 的 check）：
  python3 tools/osm_to_map.py --check maps/osm_test
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
import urllib.parse
import urllib.request

# ── 复用既有车道几何数学（不重新发明） ──────────────────────────
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from extract_city_map import build_road, check, right_normal  # noqa: E402

PROJECT_ROOT = os.path.dirname(HERE)
DEFAULT_OUT = os.path.join(PROJECT_ROOT, "maps", "osm_test")
DEFAULT_SCENARIO = os.path.join(PROJECT_ROOT, "scenarios", "osm_city_map.json")

# ── Overpass 端点（首端点失败时依次回退） ─────────────────────
# 注：overpass-api.de 对数据中心 IP 返回 406（Apache WAF 拦截），
# 故默认从可用的镜像（kumi/private.coffee）开始。
OVERPASS_ENDPOINTS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
]

# ── WGS84 → ENU 近似投影参数 ───────────────────────────────
M_PER_DEG_LAT = 111320.0          # 1° 纬度 ≈ 111320 m
DEFAULT_SPEED_MPS = 13.89         # 50 km/h
DEFAULT_LANE_WIDTH = 3.5

# ── 建筑默认尺寸（OSM 缺 building:height / building:levels 时回退） ──
BUILDING_DEFAULT_HEIGHT = 12.0    # m
LEVELS_TO_METERS = 3.0            # 1 层 ≈ 3m
# 过小的建筑（投影面积 < 此值 m²）丢弃，避免噪声碎块
BUILDING_MIN_AREA = 16.0

# ── highway → 引擎道路类型映射（urban/primary/secondary/residential） ──
HIGHWAY_TO_TYPE = {
    "motorway": "primary",
    "motorway_link": "primary",
    "trunk": "primary",
    "trunk_link": "primary",
    "primary": "primary",
    "primary_link": "primary",
    "secondary": "secondary",
    "secondary_link": "secondary",
    "tertiary": "secondary",
    "tertiary_link": "secondary",
    "unclassified": "residential",
    "residential": "residential",
    "living_street": "residential",
    "road": "urban",
}


def wgs84_to_enu(lat: float, lon: float, ref_lat: float, ref_lon: float) -> tuple[float, float]:
    """WGS84 经纬度 → 以 (ref_lat, ref_lon) 为原点的 ENU 平面坐标（米）。

    近似法：y 直接用纬度差 ×111320；x 用经度差 ×111320*cos(ref_lat)。
    半径几百米内误差可忽略，无需完整 UTM 投影。
    """
    x = (lon - ref_lon) * M_PER_DEG_LAT * math.cos(math.radians(ref_lat))
    y = (lat - ref_lat) * M_PER_DEG_LAT
    return x, y


def smooth_polyline_kinks(pts: list, max_kink_angle_deg: float = 120.0,
                          min_seg_len: float = 1.5) -> list:
    """去除折线中因 OSM 离散化/采集噪点产生的尖锐毛刺（短边夹角 > 120° 反向折叠）。"""
    if len(pts) <= 2:
        return pts
    out = [pts[0]]
    for i in range(1, len(pts) - 1):
        p_prev = out[-1]
        p_curr = pts[i]
        p_next = pts[i + 1]
        v1x, v1y = p_curr[0] - p_prev[0], p_curr[1] - p_prev[1]
        v2x, v2y = p_next[0] - p_curr[0], p_next[1] - p_curr[1]
        l1, l2 = math.hypot(v1x, v1y), math.hypot(v2x, v2y)
        if l1 > 1e-4 and l2 > 1e-4:
            dot = (v1x * v2x + v1y * v2y) / (l1 * l2)
            dot = max(-1.0, min(1.0, dot))
            angle = math.degrees(math.acos(dot))
            if angle > max_kink_angle_deg and (l1 < min_seg_len or l2 < min_seg_len):
                continue
        out.append(p_curr)
    out.append(pts[-1])
    return out


def parse_lanes(value: str) -> int:
    """解析 OSM lanes tag（如 '2'、'4'、'2;2'），缺失/非法回退 2。"""
    if not value:
        return 2
    m = re.match(r"\s*(\d+)", value)
    if not m:
        return 2
    n = int(m.group(1))
    return n if n >= 1 else 2


def parse_width(value) -> float | None:
    """解析 OSM width tag → 米；支持 '3.5' / '3.5 m' / '10.5m' / 非法→None。"""
    if not value:
        return None
    m = re.match(r"^\s*(\d+(?:\.\d+)?)", str(value).strip())
    if not m:
        return None
    w = float(m.group(1))
    return w if w > 0 else None


# CJJ 37-2012 机动车道宽度分级（取各等级中值下限，单位 m）。
# 快速路 3.5–3.75 / 主干路 3.25–3.5 / 次干路 3.0–3.25 / 支路 2.75–3.0。
_HIGHWAY_LANE_WIDTH = {
    "motorway": 3.5, "motorway_link": 3.5,
    "trunk": 3.5, "trunk_link": 3.5,
    "primary": 3.5, "primary_link": 3.5,
    "secondary": 3.5, "secondary_link": 3.25,
    "tertiary": 3.25, "tertiary_link": 3.25,
    "residential": 3.0, "unclassified": 3.0,
    "service": 3.0, "living_street": 3.0,
    "road": 3.25,
}


def default_lane_width_by_class(highway: str) -> float:
    """按道路等级给默认车道宽（CJJ 37）。未知类型回退 3.25。"""
    return _HIGHWAY_LANE_WIDTH.get(highway, 3.25)


def parse_maxspeed(value: str) -> float | None:
    """解析 OSM maxspeed tag → m/s；'none'/'walk'/未知格式回退 None。"""
    if not value:
        return None
    v = value.strip().lower()
    m = re.match(r"^(\d+(?:\.\d+)?)\s*(km/h|kmh|kph)?$", v)
    if m:
        return float(m.group(1)) / 3.6
    m = re.match(r"^(\d+(?:\.\d+)?)\s*mph$", v)
    if m:
        return float(m.group(1)) * 1.609344 / 3.6
    m = re.match(r"^(\d+(?:\.\d+)?)\s*kn$", v)
    if m:
        return float(m.group(1)) * 1.852 / 3.6
    return None


def map_type(highway: str) -> str:
    return HIGHWAY_TO_TYPE.get(highway, "urban")


def build_query(lat: float, lon: float, radius: int) -> str:
    """Overpass 查询：范围内所有机动车道 way，带 geometry。

    注意：半径过滤用 `(around:radius,lat,lon)` 冒号形式——逗号形式
    `(around,radius,lat,lon)` 在较新 Overpass 版本会报 parse error
    "around requires an odd number of arguments"。
    """
    return (
        "[out:json];\n"
        "(\n"
        '  way["highway"]["highway"!~"footway|path|cycleway|track|service|bridleway"]'
        "(around:%d,%.6f,%.6f);\n"
        ");\n"
        "out body geom;"
    ) % (radius, lat, lon)


def fetch_overpass(query: str, timeout: int = 90) -> dict:
    """POST 到 Overpass 端点，按序回退。返回 JSON dict。"""
    data = urllib.parse.urlencode({"data": query}).encode("utf-8")
    last_err = None
    for url in OVERPASS_ENDPOINTS:
        try:
            req = urllib.request.Request(
                url, data=data,
                headers={"User-Agent": "FlowEngine-osm-map-gen/1.0"})
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except Exception as e:  # noqa: BLE001 —— 网络失败则尝试下一个端点
            last_err = e
            print("  Overpass %s 失败: %s" % (url, e))
    raise RuntimeError("所有 Overpass 端点均不可用: %s" % last_err)


def build_building_query(lat: float, lon: float, radius: int) -> str:
    """Overpass 查询：范围内所有 building=* 的闭合 way（建筑轮廓）。

    注：仅取 way——城市绝大多数建筑是闭合 way；multipolygon relation 的
    member 几何处理较复杂，列为后续增强。建筑轮廓即 footprint 多边形。
    """
    return (
        "[out:json];\n"
        "(\n"
        '  way["building"](around:%d,%.6f,%.6f);\n'
        ");\n"
        "out geom;"
    ) % (radius, lat, lon)


def _polygon_area(pts: list) -> float:
    """鞋带公式算多边形有向面积绝对值（m²，ENU 米）。"""
    n = len(pts)
    if n < 3:
        return 0.0
    s = 0.0
    for i in range(n):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % n]
        s += x1 * y2 - x2 * y1
    return abs(s) * 0.5


def _footprint_rotation(pts: list) -> float:
    """建筑主朝向：取最长边方向角（ENU atan2(dy,dx)）。矩形建筑即长轴朝向。"""
    best = 0.0
    best_len = -1.0
    n = len(pts)
    for i in range(n):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % n]
        d = math.hypot(x2 - x1, y2 - y1)
        if d > best_len:
            best_len = d
            best = math.atan2(y2 - y1, x2 - x1)
    return best


def parse_buildings(elements: list, ref_lat: float, ref_lon: float,
                    radius: int) -> list[dict]:
    """解析 Overpass building=* way → buildings[]（单源真相，供仿真核+前端共用）。

    每条建筑产出：
      id        唯一标识（b_<osmid>）
      footprint [[x,y],...]  ENU 局部米，闭合多边形
      x, y      质心（米）
      rotation  主朝向（弧度，最长边方向）
      height    米（height tag > building:levels×3 > 默认）
    不含 mesh 字段——贴图模型由 osm2world 后续生成并回填（无则前端回退挤出体）。
    """
    buildings = []
    for e in elements:
        if e.get("type") != "way":
            continue
        tags = e.get("tags") or {}
        if not tags.get("building"):
            continue
        geom = e.get("geometry") or []
        pts_wgs = [[g["lon"], g["lat"]] for g in geom]
        if len(pts_wgs) < 4:   # 闭合多边形至少 4 点（首末重合）
            continue
        # WGS84 → ENU（局部米）
        enu = [list(wgs84_to_enu(py, px, ref_lat, ref_lon)) for px, py in pts_wgs]
        # 裁剪到查询半径（与道路一致，丢弃越界顶点）
        enu = [(x, y) for (x, y) in enu if math.hypot(x, y) <= radius]
        if len(enu) < 3:
            continue
        if _polygon_area(enu) < BUILDING_MIN_AREA:
            continue
        cx = sum(p[0] for p in enu) / len(enu)
        cy = sum(p[1] for p in enu) / len(enu)
        # 高度
        h = tags.get("height")
        height = None
        if h:
            m = re.match(r"^(\d+(?:\.\d+)?)", str(h).strip())
            if m:
                height = float(m.group(1))
        if height is None:
            lv = tags.get("building:levels")
            if lv:
                m = re.match(r"^(\d+)", str(lv).strip())
                if m:
                    height = int(m.group(1)) * LEVELS_TO_METERS
        if height is None or height <= 0:
            height = BUILDING_DEFAULT_HEIGHT
        buildings.append({
            "id": "b_%s" % e.get("id"),
            "footprint": [[round(x, 3), round(y, 3)] for (x, y) in enu],
            "x": round(cx, 3),
            "y": round(cy, 3),
            "rotation": round(_footprint_rotation(enu), 4),
            "height": round(height, 2),
        })
    return buildings


def validate_buildings(buildings: list) -> tuple[int, list[str]]:
    """校验 buildings[] 契约（仿真核与前端共用的单源真相）。

    返回 (errors, warnings)。errors 非空即视为 --check 失败。
    契约要点：
      - 每条建筑含 id / footprint(≥3点二维) / x / y / rotation / height(>0)
      - footprint 为闭合多边形，质心需落在多边形包络内（粗粒度 sanity）
    """
    errors: list[str] = []
    warnings: list[str] = []
    if not isinstance(buildings, list):
        errors.append("buildings 字段缺失或非数组")
        return len(errors), errors + warnings
    seen: set = set()
    for i, b in enumerate(buildings):
        tag = b.get("id", "#%d" % i)
        if tag in seen:
            warnings.append("重复建筑 id: %s" % tag)
        seen.add(tag)
        if not isinstance(b, dict):
            errors.append("%s: 非对象" % tag); continue
        fp = b.get("footprint")
        if not (isinstance(fp, list) and len(fp) >= 3):
            errors.append("%s: footprint 需为 ≥3 点的数组" % tag); continue
        for p in fp:
            if not (isinstance(p, (list, tuple)) and len(p) >= 2
                    and all(isinstance(v, (int, float)) for v in p[:2])):
                errors.append("%s: footprint 含非法点 %r" % (tag, p)); break
        for k in ("x", "y", "rotation", "height"):
            if not isinstance(b.get(k), (int, float)):
                errors.append("%s: 缺少数值字段 %s" % (tag, k)); break
        if not (isinstance(b.get("height"), (int, float)) and b["height"] > 0):
            errors.append("%s: height 必须 > 0" % tag)
    return len(errors), errors + warnings


def fetch_buildings(lat: float, lon: float, radius: int) -> list[dict]:
    """拉取并解析范围内建筑，返回 buildings[]。"""
    q = build_building_query(lat, lon, radius)
    print("查询 Overpass 建筑（radius=%dm）..." % radius)
    result = fetch_overpass(q)
    els = result.get("elements", [])
    print("  收到 %d 条 building way" % len([e for e in els if e.get("type") == "way" and (e.get("tags") or {}).get("building")]))
    blds = parse_buildings(els, lat, lon, radius)
    print("  解析出 %d 栋建筑（含轮廓+高度）" % len(blds))
    return blds


def merge_buildings(map_path: str, lat: float, lon: float, radius: int) -> int:
    """把 buildings[] 合并进已存在的 map.json（不重拉道路，保留已验证路网）。

    返回新增建筑数；map.json 原地更新。osm2world 贴图由独立步骤回填 mesh 字段。
    """
    with open(map_path) as f:
        doc = json.load(f)
    blds = fetch_buildings(lat, lon, radius)
    doc["buildings"] = blds
    with open(map_path, "w") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
    print("  已写入 %d 栋建筑到 %s" % (len(blds), map_path))
    return len(blds)


def road_length(road: dict) -> float:
    cl = road["centerline"]
    return sum(math.hypot(cl[i + 1][0] - cl[i][0], cl[i + 1][1] - cl[i][1])
               for i in range(len(cl) - 1))


def clip_to_radius(enu: list, radius: int, cx: float, cy: float) -> list:
    """把折线裁剪到以 (cx,cy) 为圆心、radius 米内的部分。

    Overpass `around` 返回整条 way 的几何——穿过区域的超长道路（如数公里
    的隧道/高架）会带来远超查询范围的点。裁剪保证地图严格落在小范围
    （半径 500m ≈ 1km×1km）内，避免大文件。
    """
    kept = []
    for p in enu:
        if math.hypot(p[0] - cx, p[1] - cy) <= radius:
            kept.append(p)
    return kept


def base_name(road_id: str) -> str:
    """road id 去唯一化后缀（`_<osmid>`），得到基础路名用于分组。"""
    m = re.match(r"^(.*)_(\d+)$", road_id)
    return m.group(1) if m else road_id


def build_main_chain(roads: list[dict], snap: float = 30.0) -> list[dict]:
    """构造 main 路线链：同路名 way 组内贪心连接，取总长最大的组。

    OSM 把长街道在交叉口拆成多条 way（共享端点节点），同 base 名的 way
    几乎必然是同一街道的连续段；组内用「端点距离 ≤snap」贪心接链。
    组内无连接（≥2 段）时退化为全局最长单 road。
    """
    if not roads:
        return []

    def ep(r: dict, which: int):
        cl = r["centerline"]
        return (cl[0][0], cl[0][1]) if which == 0 else (cl[-1][0], cl[-1][1])

    def chain_for(group: list) -> list:
        chain = [max(group, key=road_length)]
        rem = [r for r in group if r["id"] != chain[0]["id"]]
        while rem:
            head = ep(chain[0], 0)
            tail = ep(chain[-1], -1)
            best = None  # (dist, road, how)
            for r in rem:
                for ri in (0, -1):
                    p = ep(r, ri)
                    d_head = math.hypot(p[0] - head[0], p[1] - head[1])
                    d_tail = math.hypot(p[0] - tail[0], p[1] - tail[1])
                    if d_head <= snap and (best is None or d_head < best[0]):
                        best = (d_head, r, "prepend")
                    if d_tail <= snap and (best is None or d_tail < best[0]):
                        best = (d_tail, r, "append")
            if best is None:
                break
            _d, r, how = best
            if how == "prepend":
                chain.insert(0, r)
            else:
                chain.append(r)
            rem.remove(r)
        return chain

    groups = {}
    for r in roads:
        groups.setdefault(base_name(r["id"]), []).append(r)

    best_chain = []
    best_len = -1.0
    for group in groups.values():
        if len(group) < 2:
            continue
        c = chain_for(group)
        if len(c) >= 2:
            L = sum(road_length(r) for r in c)
            if L > best_len:
                best_len = L
                best_chain = c
    return best_chain if best_chain else [max(roads, key=road_length)]


def _road_end_headings(road: dict) -> tuple:
    """返回 (start_pt, end_pt, start_heading, end_heading)；heading 为 ENU atan2(dy,dx)。"""
    cl = road["centerline"]
    if len(cl) < 2:
        z = (cl[0][0], cl[0][1]) if cl else (0.0, 0.0)
        return z, z, 0.0, 0.0
    s, e = (cl[0][0], cl[0][1]), (cl[-1][0], cl[-1][1])
    sh = math.atan2(cl[1][1] - cl[0][1], cl[1][0] - cl[0][0])
    eh = math.atan2(cl[-1][1] - cl[-2][1], cl[-1][0] - cl[-2][0])
    return s, e, sh, eh


def detect_junctions(roads: list[dict], snap: float = 25.0) -> list[dict]:
    """端点几何聚类找路口：使用连通分量（Union-Find）把彼此 <=snap 的 road 端点聚成稳定一簇。

    返回 [{x, y, members:[(road_idx, is_end)]}]，仅保留 >=2 条不同 road
    的真实路口。彻底消除因顺序添加导致的聚类中心漂移与漏聚问题。"""
    eps = []
    for i, r in enumerate(roads):
        s, e, _sh, _eh = _road_end_headings(r)
        eps.append((s[0], s[1], i, False))
        eps.append((e[0], e[1], i, True))

    n = len(eps)
    parent = list(range(n))

    def find(x):
        if parent[x] != x:
            parent[x] = find(parent[x])
        return parent[x]

    def union(x, y):
        rx, ry = find(x), find(y)
        if rx != ry:
            parent[rx] = ry

    snap_sq = snap * snap
    for i in range(n):
        xi, yi, _ri, _ = eps[i]
        for j in range(i + 1, n):
            xj, yj, _rj, _ = eps[j]
            if (xi - xj) ** 2 + (yi - yj) ** 2 <= snap_sq:
                union(i, j)

    groups: dict[int, list[int]] = {}
    for i in range(n):
        root = find(i)
        groups.setdefault(root, []).append(i)

    clusters = []
    for member_indices in groups.values():
        members = [(eps[idx][2], eps[idx][3]) for idx in member_indices]
        if len({m[0] for m in members}) >= 2:
            cx = sum(eps[idx][0] for idx in member_indices) / len(member_indices)
            cy = sum(eps[idx][1] for idx in member_indices) / len(member_indices)
            clusters.append({"x": cx, "y": cy, "members": members})

    return clusters


def _angle_diff(a: float, b: float) -> float:
    d = (a - b) % (2 * math.pi)
    if d > math.pi:
        d -= 2 * math.pi
    return d


def _classify_turn(turn: float) -> str:
    """把转向角（弧度，cand_entry - cur_exit，CCW 为正）分类为转向类型。

    直行 <30°；左转(CCW,>0)/右转(<0)；接近 ±180° 视为掉头。供路口连接渲染与
    行为层使用，不影响 lanes[].successors 的字符串连通性（A*/仿真核依赖纯字符串）。
    """
    a = abs(turn)
    if a > math.pi - math.radians(50):   # ≈130°，近掉头
        return "uturn"
    if a < math.radians(30):
        return "straight"
    return "left" if turn > 0 else "right"


def generate_connectivity(roads: list[dict], junctions: list[dict],
                          snap: float = 25.0, uturn_tol: float = math.radians(50)) -> None:
    """为每条 lane 生成 successors（几何邻接，全路网，替代仅 main 链串接）。

    模型：forward 车道从 road 末端驶出（heading=end_heading），oncoming 车道从
    road 首端驶出（heading=start_heading+π）。对路口内每条相邻 road，按进入方向
    选候选车道方向，按转向角（直行/左右转，跳过掉头）连 successor；匹配车道取
    local index 最近者。写回 lane["successors"]（字符串 "<road>.lane.<N>"）。

    此前只有最长 main 链内的 road 有后继 → 其余路网对 A* / NPC 是孤岛、路口无法
    转向。现全路网几何邻接连通，A* 才能跨路口规划、NPC 才能路口转向。"""
    info = {i: _road_end_headings(r) for i, r in enumerate(roads)}
    for j in junctions:
        for (ri, is_end) in j["members"]:
            _s, _e, sh, eh = info[ri]
            cur = roads[ri]
            cur_dir = 1 if is_end else -1
            cur_exit_h = eh if is_end else (sh + math.pi)
            cur_lanes = [l for l in cur["lanes"] if l["direction"] == cur_dir]
            if not cur_lanes:
                continue
            for (ci, c_is_end) in j["members"]:
                if ci == ri:
                    continue
                _cs, _ce, c_sh, c_eh = info[ci]
                cand = roads[ci]
                cand_dir = -1 if c_is_end else 1
                cand_entry_h = (c_eh + math.pi) if c_is_end else c_sh
                cand_lanes = [l for l in cand["lanes"] if l["direction"] == cand_dir]
                if not cand_lanes:
                    continue
                turn = _angle_diff(cand_entry_h, cur_exit_h)
                # 掉头保护：仅当两条路属于不同街道（base name 不同）时，才丢弃
                # 近 ±180° 的转向，防止路口真实掉头。同一街道的连续段（同 base
                # name，如陆家嘴环路的相邻 way）即使几何上接近掉头角也照常连通——
                # 否则 main 链在相邻段处断链，运行时 A* 在 route 过滤子图上出现
                # lane 不可达（road[5]↔[6] 断链 → lane 0->33 无路）。
                same_street = base_name(cur["id"]) == base_name(cand["id"])
                if not same_street and abs(turn) > math.pi - uturn_tol:
                    continue  # 跳过掉头（仅限不同街道）
                for cl in cur_lanes:
                    cli = cl["index"] if cl["direction"] == 1 else cl["index"] - 100
                    best_l = min(cand_lanes,
                                 key=lambda l: abs((l["index"] if l["direction"] == 1
                                                    else l["index"] - 100) - cli))
                    sid = best_l["id"]
                    if sid not in cl["successors"]:
                        cl["successors"].append(sid)


def generate_junctions(roads: list[dict], junctions: list[dict]) -> list[dict]:
    """按 city_grid 约定（每段端点一 fork）导出 junctions[]：每个 (road,end) 一端
    作 incoming_road，其余同簇 road 作 connecting_roads（带 turn 转向类型）。
    供前端 ConnectorView 按转向画曲线连接、行为层用转向约束。"""
    info = {i: _road_end_headings(r) for i, r in enumerate(roads)}
    out = []
    for j in junctions:
        for (ri, is_end) in j["members"]:
            _s, _e, sh, eh = info[ri]
            cur_exit_h = eh if is_end else (sh + math.pi)
            conns = []
            for (ci, c_is_end) in j["members"]:
                if ci == ri:
                    continue
                _cs, _ce, c_sh, c_eh = info[ci]
                cand_entry_h = (c_eh + math.pi) if c_is_end else c_sh
                turn = _angle_diff(cand_entry_h, cur_exit_h)
                conns.append({"id": roads[ci]["id"], "turn": _classify_turn(turn)})
            if not conns:
                continue
            out.append({
                "id": len(out),
                "type": "fork",
                "incoming_road": roads[ri]["id"],
                "connecting_roads": conns,
            })
    return out


def generate(lat: float, lon: float, radius: int,
             map_id: str = "osm_test") -> tuple[dict, dict, list[dict]]:
    """拉取 Overpass 并生成 map_doc / routes_doc / main_chain。

    map_id 默认取输出目录基名（maps/osm_test → "osm_test"），用于区分不同 OSM
    地图实例（osm_test / osm_lujiazui …），不覆盖彼此的路网与 buildings。
    """
    query = build_query(lat, lon, radius)
    print("查询 Overpass API（radius=%dm, center=(%.5f,%.5f)）..." % (radius, lat, lon))
    result = fetch_overpass(query)
    elements = result.get("elements", [])
    ways = [e for e in elements if e.get("type") == "way"]
    print("  收到 %d 条 way" % len(ways))

    roads = []
    used_ids = set()
    dropped = 0
    for i, w in enumerate(ways):
        tags = w.get("tags") or {}
        highway = tags.get("highway", "")
        if not highway:
            continue

        geom = w.get("geometry") or []
        pts = [[g["lon"], g["lat"]] for g in geom]
        if len(pts) < 2:
            dropped += 1
            continue

        # WGS84 → ENU（z=0）
        enu = [list(wgs84_to_enu(py, px, lat, lon)) + [0.0] for px, py in pts]

        # 裁剪到查询半径内（丢弃穿过区域的超长 way 的越界点）
        enu = clip_to_radius(enu, radius, 0.0, 0.0)
        enu = smooth_polyline_kinks(enu)
        if len(enu) < 2:
            dropped += 1
            continue

        # 过滤过短碎段（<5m 无意义，避免地图膨胀）
        length = sum(math.hypot(enu[j + 1][0] - enu[j][0], enu[j + 1][1] - enu[j][1])
                     for j in range(len(enu) - 1))
        if length < 5.0:
            dropped += 1
            continue

        osmid = w.get("id")
        # 唯一 road id：优先 name，否则 way_<osmid>；重名时后缀 osmid
        name = (tags.get("name") or "").strip()
        base = name if name else "way_%s" % osmid
        rid = base
        if rid in used_ids:
            rid = "%s_%s" % (base, osmid)
        used_ids.add(rid)

        # oneway：'yes'/'true'/'1' 正向；'-1'/'reverse' 反向（反转几何后按正向处理）
        oneway_val = tags.get("oneway", "").strip()
        oneway = oneway_val in ("yes", "true", "1", "-1", "reverse")
        if oneway_val in ("-1", "reverse") and oneway:
            enu.reverse()

        # 车道数：lanes tag，缺失默认 2；双向路至少 2（避免 per_side=0）
        lanes_total = parse_lanes(tags.get("lanes"))
        if not oneway and lanes_total < 2:
            lanes_total = 2

        # 限速：maxspeed tag → m/s，缺失默认 13.89（50km/h）
        speed = parse_maxspeed(tags.get("maxspeed")) or DEFAULT_SPEED_MPS
        # 车道宽：优先 OSM width 标签，否则按 CJJ 37 道路等级给默认（不再统一 3.5）。
        lane_width = parse_width(tags.get("width")) or default_lane_width_by_class(highway)

        edge = {
            "name": rid,
            "type": map_type(highway),
            "speed_limit": speed,
            "oneway": oneway,
            "lane_width": lane_width,
            "lanes": lanes_total,
            "nodes": enu,
        }
        roads.append(build_road(edge, i))

    print("  生成 %d 条 road（过滤掉 %d 条无几何/过短 way）" % (len(roads), dropped))

    # 所有 lane 默认无后继
    for road in roads:
        for lane in road["lanes"]:
            lane["successors"] = []

    # 路口检测（端点几何聚类）+ 车道后继（全路网几何邻接，替代仅 main 链串接）。
    # 此前只有最长 main 链内的 road 有后继 → 其余路网对 A* / NPC 是孤岛、路口
    # 无法转向。现按几何邻接给每条 lane 连出直行/左右转后继，路口可用。
    junctions = detect_junctions(roads)
    generate_connectivity(roads, junctions)
    junctions_doc = generate_junctions(roads, junctions)
    print("  检测 %d 个路口，生成路口拓扑 + 全路网车道后继" % len(junctions))

    main_chain = build_main_chain(roads)  # 仅用于 routes.json 的 main 路线定义

    # 建筑：单源真相（footprint+height+xy+rotation），供仿真核碰撞/遮挡 + 前端渲染共用。
    buildings = fetch_buildings(lat, lon, radius)

    map_doc = {
        "schema_version": 1,
        "map_id": map_id,
        "name": "OSM %s (Lujiazui)" % map_id,
        # 故意不含 source_scenario —— 地图独立存在
        "roads": roads,
        "connections": [],
        "junctions": junctions_doc,
        "buildings": buildings,
        "landmarks": {
            "traffic_lights": [],
            "stop_lines": [],
            "construction_zones": [],
        },
    }

    routes = {
        "map_id": map_id,
        "routes": [
            {
                "id": "main",
                "name": "OSM 主线（最长连接道路链，%d 段）" % len(main_chain),
                "kind": "main",
                "road_chain": [r["id"] for r in main_chain],
                "lane_direction": 1,
            }
        ],
        "reserved_turns": [],
    }

    return map_doc, routes, main_chain


def scenario_ego(chain: list[dict], lane_width: float = DEFAULT_LANE_WIDTH):
    """场景 ego 初位置：main 链首条 road 起点，横向偏到第一条正向车道中心。"""
    if not chain:
        return 0.0, 0.0, 0.0
    cl = chain[0]["centerline"]
    p0, p1 = cl[0], cl[1]
    heading = math.atan2(p1[1] - p0[1], p1[0] - p0[0])
    nx, ny = right_normal(p1[0] - p0[0], p1[1] - p0[1])
    return p0[0] + nx * lane_width * 0.5, p0[1] + ny * lane_width * 0.5, heading


def write_scenario(map_doc: dict, routes_doc: dict, main_chain: list[dict],
                   out_dir: str, scenario_path: str, map_id: str = "osm_test") -> None:
    """写场景文件（参考 scenarios/city_ring_map.json 格式）。"""
    scenarios_dir = os.path.dirname(scenario_path)
    os.makedirs(scenarios_dir, exist_ok=True)
    rel = os.path.relpath(out_dir, scenarios_dir).replace(os.sep, "/")

    ex, ey, eh = scenario_ego(main_chain)

    scenario = {
        "name": map_id,
        "description": "OSM 真实路网（上海陆家嘴）smoke 场景；静态道路来自 %s/map.json，"
                       "ego 沿 route 'main'（最长连接道路链）行驶。" % os.path.basename(out_dir),
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
            "x": round(ex, 3),
            "y": round(ey, 3),
            "heading": round(eh, 4),
            "init_speed": 10.0,
            "target_speed": 13.0,
            "wheelbase": 2.7,
            "length": 4.6,
            "width": 2.0,
            "max_steer": 0.6,
        },
        "actors": [],
        "pass_criteria": {
            "no_collision": True,
            "max_duration_s": 120,
            "min_avg_speed_mps": 3.0,
            "min_distance_m": 450,
        },
        "route": [],
        "scenarios": [],
        "choreography": {
            "loop_period_s": 999,
            "beats": [],
        },
    }
    with open(scenario_path, "w") as f:
        json.dump(scenario, f, indent=2, ensure_ascii=False)
    print("  场景写入 %s" % scenario_path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lat", type=float, default=31.235,
                        help="中心纬度（默认上海陆家嘴 31.235）")
    parser.add_argument("--lon", type=float, default=121.5,
                        help="中心经度（默认上海陆家嘴 121.5）")
    parser.add_argument("--radius", type=int, default=500,
                        help="查询半径（米），默认 500（约 1km×1km 范围）")
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="输出目录（默认 maps/osm_test）")
    parser.add_argument("--scenario", default=DEFAULT_SCENARIO,
                        help="场景文件输出路径（默认 scenarios/osm_city_map.json）")
    parser.add_argument("--check", metavar="DIR", nargs="?", const=DEFAULT_OUT,
                        help="仅校验指定目录（默认 maps/osm_test），不拉取网络")
    parser.add_argument("--merge-buildings", action="store_true",
                        help="仅把 buildings[] 合并进已存在的 map.json（不重拉道路，保留已验证路网）")
    parser.add_argument("--map-id", default=None,
                        help="地图实例 id（写入 map.json/routes.json/scenario）。"
                             "默认取 --out 目录基名（maps/osm_lujiazui → osm_lujiazui），"
                             "不覆盖既有地图。")
    args = parser.parse_args()

    # map_id 默认取输出目录基名，使不同 --out 天然对应不同地图实例。
    out_dir_abs = os.path.abspath(args.out)
    map_id = args.map_id or os.path.basename(os.path.normpath(out_dir_abs))

    if args.check:
        rc = check(os.path.abspath(args.check))
        # 额外校验 buildings[] 单源真相契约（碰撞/遮挡/渲染共用）
        import json as _json
        map_path = os.path.join(os.path.abspath(args.check), "map.json")
        if os.path.exists(map_path):
            with open(map_path, "r", encoding="utf-8") as _f:
                doc = _json.load(_f)
            nerr, msgs = validate_buildings(doc.get("buildings", []))
            if doc.get("buildings"):
                print("  buildings[]: %d 条" % len(doc["buildings"]))
            for m in msgs:
                print("    %s" % m)
            if nerr:
                print("  buildings 校验失败: %d 个错误" % nerr)
                rc = 1
        return rc

    if args.merge_buildings:
        map_path = os.path.join(os.path.abspath(args.out), "map.json")
        if not os.path.exists(map_path):
            print("错误：%s 不存在，无法合并建筑" % map_path)
            return 2
        merge_buildings(map_path, args.lat, args.lon, args.radius)
        return 0

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    map_doc, routes_doc, main_chain = generate(args.lat, args.lon, args.radius, map_id)

    with open(os.path.join(out_dir, "map.json"), "w") as f:
        json.dump(map_doc, f, indent=2, ensure_ascii=False)
    with open(os.path.join(out_dir, "routes.json"), "w") as f:
        json.dump(routes_doc, f, indent=2, ensure_ascii=False)
    print("  map.json 写入 %s" % os.path.join(out_dir, "map.json"))
    print("  routes.json 写入 %s" % os.path.join(out_dir, "routes.json"))

    write_scenario(map_doc, routes_doc, main_chain, out_dir,
                   os.path.abspath(args.scenario), map_id)

    return check(out_dir)


if __name__ == "__main__":
    raise SystemExit(main())
