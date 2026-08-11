import json
import tempfile
import unittest
from pathlib import Path

from tools.map_compiler import compile_map
from tools.extract_city_map import check as extract_check


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


if __name__ == "__main__":
    unittest.main()
