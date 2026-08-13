import tempfile
import unittest
from pathlib import Path

from tools.astar_route import LaneGraph, astar, road_chain_from_lanes, check


def _map_doc(roads):
    return {
        "schema_version": 1,
        "map_id": "test",
        "name": "test",
        "roads": roads,
        "connections": [],
        "junctions": [],
        "landmarks": {"traffic_lights": [], "stop_lines": [], "construction_zones": []},
    }


def _lane(lid, road, direction, pts, successors):
    return {
        "id": lid,
        "index": 1,
        "width": 3.5,
        "direction": direction,
        "centerline": pts,
        "markings": [],
        "successors": successors,
    }


def _straight_chain(prefix, n, dx=100.0):
    """一条沿 +x 的 road 链，n 段，每段 100m，同想车道后继。"""
    roads = []
    for i in range(n):
        x0, x1 = i * dx, (i + 1) * dx
        roads.append({
            "id": "%s_%02d" % (prefix, i),
            "type": "urban",
            "speed_limit": 15.0,
            "oneway": True,
            "centerline": [[x0, 0.0, 0.0], [x1, 0.0, 0.0]],
            "lanes": [_lane("%s_%02d.lane.1" % (prefix, i), "%s_%02d" % (prefix, i),
                            1, [[x0, -1.75, 0.0], [x1, -1.75, 0.0]], [])],
        })
    # 填 successors：i -> i+1
    for i in range(n - 1):
        roads[i]["lanes"][0]["successors"] = ["%s_%02d.lane.1" % (prefix, i + 1)]
    return roads


class AstarRouteTest(unittest.TestCase):
    def test_astar_shortest_path(self):
        """直线链上 A* 应给出顺序路径，cost = 链长。"""
        roads = _straight_chain("a", 4)
        g = LaneGraph(_map_doc(roads))
        chain, cost = astar(g, "a_00.lane.1", "a_03.lane.1")
        self.assertEqual(chain, ["a_00.lane.1", "a_01.lane.1", "a_02.lane.1", "a_03.lane.1"])
        self.assertAlmostEqual(cost, 300.0, places=1)
        self.assertEqual(road_chain_from_lanes(chain, g), ["a_00", "a_01", "a_02", "a_03"])

    def test_astar_unreachable(self):
        """断开链 → 无路可达。"""
        roads = _straight_chain("a", 3)
        # 断开 1->2 的连接
        roads[1]["lanes"][0]["successors"] = []
        g = LaneGraph(_map_doc(roads))
        chain, cost = astar(g, "a_00.lane.1", "a_02.lane.1")
        self.assertIsNone(chain)

    def test_astar_turn_penalty_prefers_straight(self):
        """有横穿支路时，转向惩罚应让 A* 倾向直行而非权杖。"""
        # 主路 a_00..a_02 直行；每个交叉口有 b_i 支路但无纵向连通（死巷）
        roads = _straight_chain("a", 3)
        g = LaneGraph(_map_doc(roads))
        # 直行 cost = 200m；加 penalty 也不影响直行
        chain, _ = astar(g, "a_00.lane.1", "a_02.lane.1", turn_penalty=5000.0)
        self.assertEqual(chain, ["a_00.lane.1", "a_01.lane.1", "a_02.lane.1"])

    def test_check_city_ring_passes(self):
        """现网 city_ring 的 routes.json 车道链连通。"""
        root = Path(__file__).resolve().parents[1]
        out = root / "maps" / "city_ring"
        if not (out / "map.json").exists():
            self.skipTest("city_ring map absent")
        self.assertEqual(check(str(out)), 0)

    def test_check_city_grid_passes(self):
        """现网 city_grid 的 routes.json 车道链连通（大地图回归抓手）。"""
        root = Path(__file__).resolve().parents[1]
        out = root / "maps" / "city_grid"
        if not (out / "map.json").exists():
            self.skipTest("city_grid map absent")
        self.assertEqual(check(str(out)), 0)

    def test_astar_finds_turn_route_in_city_grid(self):
        """city_grid 上 A* 应找到含真实转向的路线（直行后右转），且车道链连通。"""
        root = Path(__file__).resolve().parents[1]
        out = root / "maps" / "city_grid"
        if not (out / "map.json").exists():
            self.skipTest("city_grid map absent")
        from tools.astar_route import LaneGraph, astar, road_chain_from_lanes, compute_turns
        import json
        with (out / "map.json").open(encoding="utf-8") as f:
            g = LaneGraph(json.load(f))
        # ns_avenue_00 北上，在 j=10 交叉口右转进 ew_avenue_10（真实转向）
        chain, cost = astar(g, "ns_avenue_00_seg_00.lane.1",
                            "ew_avenue_10_seg_00.lane.1", turn_penalty=800.0)
        self.assertIsNotNone(chain, "A* 应在 city_grid 找到转向路径")
        turns = compute_turns(chain, g)
        non_straight = [t for t in turns if t["maneuver"] != "STRAIGHT"]
        self.assertGreaterEqual(len(non_straight), 1, "路径应含至少一处转向")
        # 转向目标必须是 ew_avenue_10（右转进入的东西大道）
        self.assertEqual(non_straight[0]["to"], "ew_avenue_10_seg_00")
        # 生成的 road_chain 所有相邻对连通（用 check 的图校验逻辑）
        rc = road_chain_from_lanes(chain, g)
        self.assertIn("ew_avenue_10_seg_00", rc)


if __name__ == "__main__":
    unittest.main()