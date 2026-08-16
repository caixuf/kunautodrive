import json
import tempfile
import unittest
from pathlib import Path

from tools.map_compiler import compile_map
from tools.extract_city_map import check as extract_check
from tools.osm2kmap import serialize_kmap


def _write(tmp: Path, text: str) -> Path:
    p = tmp / "map.kmap"
    p.write_text(text, encoding="utf-8")
    return p


class MapCompilerTest(unittest.TestCase):
    def _compile(self, text):
        with tempfile.TemporaryDirectory() as td:
            return compile_map(_write(Path(td), text))

    def test_minimal_map_auto_derives_lanes(self):
        text = """Map {
    id: m1
    name: "M1"
    Road a {
        type: urban
        lanes: 4
        laneWidth: 3.5
        speedLimit: 15.0
        oneWay: false
        Point { x: 0; y: 0; z: 0 }
        Point { x: 100; y: 0; z: 0 }
    }
}"""
        m = self._compile(text)
        self.assertEqual(m["map_id"], "m1")
        self.assertEqual(len(m["roads"]), 1)
        road = m["roads"][0]
        # 双向 4 lane → 2 正向 + 2 对向
        self.assertEqual(len(road["lanes"]), 4)
        dirs = [l["direction"] for l in road["lanes"]]
        self.assertEqual(sorted(dirs), [-1, -1, 1, 1])
        for lane in road["lanes"]:
            for key in ("id", "index", "width", "direction", "centerline", "markings", "successors"):
                self.assertIn(key, lane)
            self.assertGreaterEqual(len(lane["centerline"]), 2)

    def test_elevation_profile(self):
        text = """Map {
    id: m2
    Road a {
        type: urban
        lanes: 2
        laneWidth: 3.5
        speedLimit: 10.0
        oneWay: false
        Point { x: 0; y: 0; z: 0 }
        Point { x: 100; y: 0; z: 0 }
        Elevation { s: 0; z: 0 }
        Elevation { s: 100; z: 5 }
    }
}"""
        m = self._compile(text)
        self.assertEqual(m["roads"][0]["elevation_profile"],
                         [{"s": 0, "z": 0}, {"s": 100, "z": 5}])

    def test_explicit_lane_overrides_markings_and_successors(self):
        text = """Map {
    id: m3
    Road a {
        type: urban
        lanes: 2
        laneWidth: 3.5
        speedLimit: 10.0
        oneWay: false
        Point { x: 0; y: 0; z: 0 }
        Point { x: 100; y: 0; z: 0 }
        Lane {
            index: 1
            direction: 1
            successors: ["b.lane.1"]
            Marking { type: solid_yellow; side: left }
        }
    }
    Road b {
        type: urban
        lanes: 2
        laneWidth: 3.5
        speedLimit: 10.0
        oneWay: false
        Point { x: 100; y: 0; z: 0 }
        Point { x: 200; y: 0; z: 0 }
    }
}"""
        m = self._compile(text)
        lane1 = [l for l in m["roads"][0]["lanes"] if l["direction"] == 1 and l["index"] == 1][0]
        self.assertEqual(lane1["successors"], ["b.lane.1"])
        self.assertEqual(lane1["markings"], [{"type": "solid_yellow", "side": "left"}])
        # 未覆盖的对向 lane 仍自动派生默认 marking
        lane_opp = [l for l in m["roads"][0]["lanes"] if l["direction"] == -1][0]
        self.assertTrue(any(mk["type"] == "double_yellow" for mk in lane_opp["markings"]))

    def test_multiline_connection(self):
        text = """Map {
    id: m4
    Road a {
        type: urban
        lanes: 2
        laneWidth: 3.5
        speedLimit: 10.0
        oneWay: false
        Point { x: 0; y: 0; z: 0 }
        Point { x: 100; y: 0; z: 0 }
    }
    Connection {
        from: a
        to: a
        type: continue
    }
}"""
        m = self._compile(text)
        self.assertEqual(m["connections"], [{"from_road": "a", "to_road": "a", "type": "continue"}])

    def test_full_dsl_matches_extracted_map(self):
        """编译 maps/examples/city_ring_full.kmap 应与 maps/city_ring/map.json 几何/拓扑一致。"""
        root = Path(__file__).resolve().parents[1]
        kmap = root / "maps" / "examples" / "city_ring_full.kmap"
        map_json = root / "maps" / "city_ring" / "map.json"
        if not kmap.exists() or not map_json.exists():
            self.skipTest("kmap/map.json not present")
        compiled = compile_map(kmap)
        extracted = json.loads(map_json.read_text(encoding="utf-8"))
        self.assertEqual(len(compiled["roads"]), len(extracted["roads"]))
        for ca, cb in zip(compiled["roads"], extracted["roads"]):
            self.assertEqual(ca["id"], cb["id"])
            self.assertEqual(ca["type"], cb["type"])
            self.assertEqual(len(ca["lanes"]), len(cb["lanes"]))
            for la, lb in zip(ca["lanes"], cb["lanes"]):
                self.assertEqual(la["id"], lb["id"])
                self.assertEqual(la["direction"], lb["direction"])
                self.assertEqual(la["successors"], lb["successors"])

    def test_extract_check_passes(self):
        """extract_city_map 的独立校验对当前 map.json 通过。"""
        root = Path(__file__).resolve().parents[1]
        out = root / "maps" / "city_ring"
        if not (out / "map.json").exists():
            self.skipTest("map.json not present")
        self.assertEqual(extract_check(str(out)), 0)

    def test_osm2kmap_serialize_roundtrip_stable(self):
        """osm2kmap 产出的 .kmap 经 map_compiler 编译后，再 serialize→compile 必须不变。

        这是 §7「adapter 产 DSL → compiler 编译」的门禁核心：证明 DSL 枢纽能把
        net2map 式 doc（每车道显式几何 + junction + traffic light + building）
        落成稳定契约，且不残留内部注解（nodes/sumo_id/target_road）。
        """
        aid = "road_a"
        doc = {
            "map_id": "t1", "name": "SUMO t1", "schema_version": 1,
            "roads": [
                {"id": aid, "name": "", "type": "urban", "speed_limit": 13.89,
                 "oneway": True, "lane_width": 3.2, "sumo_id": "123",
                 "centerline": [[0.0, 0.0, 0.0], [100.0, 5.0, 0.0]],
                 "lanes": [
                     {"id": aid + ".lane.1", "index": 1, "width": 3.2, "direction": 1,
                      "centerline": [[0.0, 0.0, 0.0], [100.0, 1.5, 0.0]],
                      "markings": [{"type": "solid_white", "side": "left"}],
                      "successors": [aid + ".lane.2"]},
                     {"id": aid + ".lane.2", "index": 2, "width": 3.2, "direction": 1,
                      "centerline": [[0.0, 3.2, 0.0], [100.0, 4.7, 0.0]],
                      "markings": [{"type": "dashed_white", "side": "right"}], "successors": []},
                 ]},
                {"id": "road_jc_0", "name": "", "type": "residential", "speed_limit": 8.0,
                 "oneway": True, "lane_width": 2.75, "sumo_id": ":J_0_0", "target_road": aid,
                 "centerline": [[100.0, 5.0, 0.0], [120.0, 3.0, 0.0]],
                 "lanes": [
                     {"id": "road_jc_0.lane.1", "index": 1, "width": 2.75, "direction": 1,
                      "centerline": [[100.0, 5.0, 0.0], [120.0, 3.0, 0.0]],
                      "markings": [{"type": "solid_white", "side": "left"}], "successors": []},
                 ]},
            ],
            "connections": [],
            "junctions": [
                {"id": 100, "type": "fork", "incoming_road": aid,
                 "connecting_roads": [{"id": "road_jc_0", "turn": "left"},
                                      {"id": aid, "turn": "straight"}],
                 "shape": [[100.0, 5.0], [102.0, 6.0]]},
            ],
            "landmarks": {"traffic_lights": [
                {"id": 0, "x": 5.413, "y_lane": -1.605, "heading": -2.6358,
                 "red_s": 45.0, "yellow_s": 6.0, "green_s": 39.0, "phase_offset_s": 0.0}],
                "stop_lines": [], "construction_zones": []},
            "buildings": [
                {"id": "b_x", "x": 690.1, "y": -902.029, "rotation": 2.0074, "height": 12.0,
                 "footprint": [[647.765, -904.865], [670.542, -893.755]]},
            ],
        }
        with tempfile.TemporaryDirectory() as td:
            p1 = Path(td) / "map.kmap"
            p1.write_text(serialize_kmap(doc, doc["map_id"], doc["name"]), encoding="utf-8")
            compiled = compile_map(p1)
            # 契约断言：内部注解不残留，关键结构齐备
            for r in compiled["roads"]:
                self.assertNotIn("nodes", r)
                self.assertIn("name", r)
            self.assertEqual(len(compiled["junctions"]), 1)
            self.assertEqual(len(compiled["landmarks"]["traffic_lights"]), 1)
            self.assertEqual(len(compiled["buildings"]), 1)
            self.assertEqual(compiled["buildings"][0]["footprint"],
                             doc["buildings"][0]["footprint"])
            # 不变量：再 serialize→compile 逐字节等价
            p2 = Path(td) / "map2.kmap"
            p2.write_text(serialize_kmap(compiled, compiled["map_id"], compiled["name"]),
                          encoding="utf-8")
            self.assertEqual(compile_map(p2), compiled)


if __name__ == "__main__":
    unittest.main()
