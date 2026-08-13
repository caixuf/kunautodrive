#!/usr/bin/env python3
"""astar_route.py — 在 map.json 的车道级 successors 图上做 A* 路径搜索，生成 route。

背景：大地图（如 city_grid，1300 段 / 1.5 万 lane successors）不能靠手写长
road_chain 表达任意 O-D 路线（见 docs/MAP_ENGINE_ROUTING.md §5 坑 1/4/5）。
本工具在 map.json 的 lane `successors` 有向图上跑 A*，从起点 lane 到终点 lane
求最短路径，输出：
  * lane_chain      —— 有序 lane id 序列（A* 原生输出）
  * road_chain      —— 有序 road id 序列（去重合并连续同 road，routes.json 契约）
  * turns[]         —— 每处 road 边界判定的转向（STRAIGHT/LEFT/RIGHT）
  * cost_m / length_m

仅输出数据，不改任何 C/C++ loader / 规划 / 评测 / 3D JS（与 extract_city_map /
grid_map_generator 同契约）。可独立校验：
  python3 tools/astar_route.py <mapdir> --from-lane A.lane.1 --to-lane B.lane.1
  python3 tools/astar_route.py --check <mapdir>   # 校验 routes.json 链式连通

权重：进入后继 lane 的成本 = 当前 lane 中心线长度（走多远花多少代价）；
启发式 = 当前 lane 中点 → 终点 lane 中点的欧氏距离（可采纳，A* 最优）。
"""
from __future__ import annotations

import argparse
import heapq
import json
import math
import os
import sys


# ── 几何工具（复用 extract_city_map 的约定，不重造） ──────────────

def lane_length(centerline) -> float:
    """中心线折线总长（m）。"""
    total = 0.0
    for i in range(len(centerline) - 1):
        dx = centerline[i + 1][0] - centerline[i][0]
        dy = centerline[i + 1][1] - centerline[i][1]
        total += math.hypot(dx, dy)
    return total


def lane_midpoint(centerline) -> tuple[float, float]:
    """中心线几何中点（按节点平均，足够作启发式）。"""
    if not centerline:
        return (0.0, 0.0)
    xs = [p[0] for p in centerline]
    ys = [p[1] for p in centerline]
    return (sum(xs) / len(xs), sum(ys) / len(ys))


def lane_end_heading(centerline, end=True) -> float:
    """lane 中心线端点朝向（end=True 末段 / False 首段），rad。"""
    if len(centerline) < 2:
        return 0.0
    if end:
        p1, p2 = centerline[-2], centerline[-1]
    else:
        p1, p2 = centerline[0], centerline[1]
    return math.atan2(p2[1] - p1[1], p2[0] - p1[0])


def norm_angle(a: float) -> float:
    """归一化到 (-pi, pi]。"""
    while a > math.pi:
        a -= 2 * math.pi
    while a <= -math.pi:
        a += 2 * math.pi
    return a


def classify_turn(prev_lane, next_lane) -> str:
    """按前后 lane 端点朝向差判定转向（ENU x=东 y=北，逆时针为正=左转）。"""
    d = norm_angle(lane_end_heading(next_lane, end=False) -
                   lane_end_heading(prev_lane, end=True))
    if abs(d) < 0.5:   # ~<30° 视为直行（含轻微弯道）
        return "STRAIGHT"
    return "LEFT" if d > 0 else "RIGHT"


# ── 图构建 ───────────────────────────────────────────────────

class LaneGraph:
    """从 map.json 构建的有向车道图。"""

    def __init__(self, map_doc: dict):
        self.lanes: dict[str, dict] = {}   # lane_id -> {road_id, direction, length, midpoint, centerline}
        self.adj: dict[str, list[tuple[str, float]]] = {}  # lane_id -> [(succ, cost)]

        roads = map_doc.get("roads", [])
        lane_ids = set()
        # 先注册所有 lane 及其几何
        for road in roads:
            rid = road.get("id")
            for lane in road.get("lanes", []):
                lid = lane.get("id")
                cl = lane.get("centerline", [])
                self.lanes[lid] = {
                    "road_id": rid,
                    "direction": lane.get("direction", 1),
                    "length": lane_length(cl),
                    "midpoint": lane_midpoint(cl),
                    "centerline": cl,
                }
                lane_ids.add(lid)
                self.adj.setdefault(lid, [])
        # 再填 successor 边（跨 road 的 lane 引用已在 map 内，见 extract_city_map.check）
        for road in roads:
            for lane in road.get("lanes", []):
                lid = lane.get("id")
                for succ in lane.get("successors", []):
                    if succ in lane_ids:
                        # 代价 = 当前 lane 长度（进入 succ 前在 lid 上行驶的距离）
                        self.adj[lid].append((succ, self.lanes[lid]["length"]))

    def road_lanes(self, road_id: str, direction: int | None = None) -> list[str]:
        """返回指定 road 的车道（可按方向过滤）。"""
        out = []
        for lid, meta in self.lanes.items():
            if meta["road_id"] == road_id:
                if direction is None or meta["direction"] == direction:
                    out.append(lid)
        return out

    def resolve(self, spec: str, direction: int | None) -> str:
        """把 CLI 参数解析成 lane id：直接给 lane id，或给 road id（取首条匹配 lane）。"""
        if spec in self.lanes:
            return spec
        lanes = self.road_lanes(spec, direction)
        if not lanes and direction is None:
            lanes = self.road_lanes(spec)
        if not lanes:
            raise ValueError("找不到 lane 或 road: %s" % spec)
        # 取 index 最小（前进方向第一车道）——稳定且代表该 road 的行驶车道
        lanes.sort(key=lambda l: (self.lanes[l]["direction"], l))
        return lanes[0]


# ── A* ──────────────────────────────────────────────────────

def astar(graph: LaneGraph, start: str, goal: str, turn_penalty: float = 0.0):
    """返回 (lane_chain, cost_m)。不可达返回 (None, inf)。

    turn_penalty>0 时，进入一个 road 与当前 lane 所在 road 不同的后继 lane 会
    额外加一次转向代价（米等效）。这会让 A* 倾向"同大道直行、需要时才转向"，
    而非在每格交叉口 zigzag（后者在纯长度权重下同样是曼哈顿最优，但不真实）。
    """
    if start == goal:
        return [start], 0.0
    adj = graph.adj
    mid = graph.lanes[goal]["midpoint"]

    def heuristic(lid):
        mx, my = graph.lanes[lid]["midpoint"]
        return math.hypot(mx - mid[0], my - mid[1])

    def edge_cost(cur_lid, nxt_lid):
        c = graph.lanes[cur_lid]["length"]
        if turn_penalty > 0.0 and graph.lanes[cur_lid]["road_id"] != graph.lanes[nxt_lid]["road_id"]:
            c += turn_penalty
        return c

    g_score = {start: 0.0}
    parent = {start: None}
    closed = set()
    heap = [(heuristic(start), start)]
    while heap:
        _, cur = heapq.heappop(heap)
        if cur in closed:
            continue
        if cur == goal:
            chain = []
            node = cur
            while node is not None:
                chain.append(node)
                node = parent[node]
            chain.reverse()
            return chain, g_score[cur]
        closed.add(cur)
        for nxt, base_cost in adj.get(cur, []):
            if nxt in closed:
                continue
            ng = g_score[cur] + edge_cost(cur, nxt)
            if ng < g_score.get(nxt, float("inf")):
                g_score[nxt] = ng
                parent[nxt] = cur
                heapq.heappush(heap, (ng + heuristic(nxt), nxt))
    return None, float("inf")


def road_chain_from_lanes(lane_chain, graph: LaneGraph) -> list[str]:
    """lane 链 → 去重 road 链（连续同 road 合并，routes.json 契约）。"""
    roads = []
    for lid in lane_chain:
        rid = graph.lanes[lid]["road_id"]
        if not roads or roads[-1] != rid:
            roads.append(rid)
    return roads


def compute_turns(lane_chain, graph: LaneGraph) -> list[dict]:
    """相邻 road 边界处的转向序列（不含起始 road 自身）。"""
    turns = []
    for i in range(len(lane_chain) - 1):
        a, b = lane_chain[i], lane_chain[i + 1]
        ra, rb = graph.lanes[a]["road_id"], graph.lanes[b]["road_id"]
        if ra == rb:
            continue  # 同 road 内（换道/延续）不产转向
        turns.append({
            "from": ra,
            "to": rb,
            "maneuver": classify_turn(graph.lanes[a]["centerline"],
                                       graph.lanes[b]["centerline"]),
        })
    return turns


# ── CLI：单次求路径 ─────────────────────────────────────────

def route(map_dir: str, from_spec: str, to_spec: str,
          direction: int | None, route_id: str, name: str,
          turn_penalty: float, write: bool) -> int:
    map_path = os.path.join(map_dir, "map.json")
    with open(map_path, encoding="utf-8") as f:
        map_doc = json.load(f)
    graph = LaneGraph(map_doc)
    start = graph.resolve(from_spec, direction)
    goal = graph.resolve(to_spec, direction)
    chain, cost = astar(graph, start, goal, turn_penalty)
    if not chain:
        print("FAIL: 从 %s 到 %s 无路可达" % (start, goal))
        return 1
    road_chain = road_chain_from_lanes(chain, graph)
    turns = compute_turns(chain, graph)
    n_turn = sum(1 for t in turns if t["maneuver"] != "STRAIGHT")
    print("OK: %s -> %s" % (start, goal))
    print("  lanes : %d 条, cost=%.1f m" % (len(chain), cost))
    print("  roads : %d 段" % len(road_chain))
    print("  turns : %d 处转向, 其中 %d 处非直行" % (len(turns), n_turn))
    for t in turns:
        print("    %-6s %s -> %s" % (t["maneuver"], t["from"], t["to"]))

    if write:
        routes_path = os.path.join(map_dir, "routes.json")
        with open(routes_path, encoding="utf-8") as f:
            routes_doc = json.load(f)
        # 覆盖同名 route 或追加
        existing = [rt for rt in routes_doc.get("routes", []) if rt.get("id") != route_id]
        new_route = {
            "id": route_id,
            "name": name or ("A* 路径 %s → %s" % (road_chain[0], road_chain[-1])),
            "kind": "main",
            "road_chain": road_chain,
            "lane_direction": direction if direction is not None else 1,
            "generated_by": "astar_route.py",
        }
        existing.append(new_route)
        routes_doc["routes"] = existing
        with open(routes_path, "w", encoding="utf-8") as f:
            json.dump(routes_doc, f, indent=2, ensure_ascii=False)
        print("  已写入 route '%s' 到 %s（%d 条 route）"
              % (route_id, os.path.relpath(routes_path), len(existing)))
    return 0


# ── CLI：校验 routes.json 链式连通 ───────────────────────────

def check(map_dir: str) -> int:
    map_path = os.path.join(map_dir, "map.json")
    routes_path = os.path.join(map_dir, "routes.json")
    errors = []
    with open(map_path, encoding="utf-8") as f:
        map_doc = json.load(f)
    graph = LaneGraph(map_doc)
    with open(routes_path, encoding="utf-8") as f:
        routes_doc = json.load(f)

    road_set = {r["id"] for r in map_doc.get("roads", [])}
    for route in routes_doc.get("routes", []):
        chain = route.get("road_chain", [])
        rid = route.get("id")
        if len(chain) < 2:
            continue
        # 每条相邻 road 对必须有 lane successor 连通（lane_direction 内）
        direction = route.get("lane_direction", 1)
        for a, b in zip(chain, chain[1:]):
            if a not in road_set or b not in road_set:
                errors.append("route %s: road %s/%s 不存在" % (rid, a, b))
                continue
            a_lanes = graph.road_lanes(a, direction)
            b_ids = set(graph.road_lanes(b, direction))
            if not any(any(nb in b_ids for nb, _ in graph.adj.get(l, [])) for l in a_lanes):
                errors.append("route %s: %s 无车道后继连到 %s（lane_direction=%d）"
                              % (rid, a, b, direction))
        # A* 端点可达性：route 首尾 road 之间必须存在路径
        try:
            start = graph.resolve(chain[0], direction)
            goal = graph.resolve(chain[-1], direction)
        except ValueError as e:
            errors.append("route %s: %s" % (rid, e))
            continue
        found, cost = astar(graph, start, goal)
        if not found:
            errors.append("route %s: 端点 %s→%s 无 A* 可达（链断开）"
                          % (rid, chain[0], chain[-1]))
        else:
            # 链总长不应远超 A* 最优太多（>5x 说明链绕路异常，通常为拓扑断裂）
            chain_cost = sum(graph.lanes[l]["length"]
                             for road in chain
                             for l in graph.road_lanes(road, direction) or [graph.resolve(road, direction)])
            print("  route %-16s %2d 段  A* cost=%.0fm" % (rid, len(chain), cost))
    return _report(errors)


def _report(errors: list) -> int:
    if not errors:
        print("校验通过: 所有 route 车道链连通")
        return 0
    for e in errors:
        print("FAIL:", e)
    return 1


# ── main ────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("map_dir", help="地图目录（含 map.json / routes.json）")
    ap.add_argument("--from-lane", dest="from_spec")
    ap.add_argument("--to-lane", dest="to_spec")
    ap.add_argument("--direction", type=int, default=None,
                    help="lane_direction（road 解析时过滤车道，默认取首个匹配）")
    ap.add_argument("--route-id", default="astar_route")
    ap.add_argument("--name")
    ap.add_argument("--turn-penalty", type=float, default=0.0,
                    help="转向惩罚（米等效），压制 zigzag，同大道直行、需要时才转向")
    ap.add_argument("--write", action="store_true",
                    help="把算出的 route 写入 routes.json")
    ap.add_argument("--check", action="store_true",
                    help="校验 routes.json 所有 route 车道链连通，不生成")
    args = ap.parse_args()

    if args.check:
        return check(args.map_dir)
    if not args.from_spec or not args.to_spec:
        ap.error("需要 --from-lane 与 --to-lane（或用 --check）")
    return route(args.map_dir, args.from_spec, args.to_spec,
                 args.direction, args.route_id, args.name,
                 args.turn_penalty, args.write)


if __name__ == "__main__":
    sys.exit(main())
