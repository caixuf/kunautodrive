#!/usr/bin/env python3
"""tests/test_json_to_lanelet.py — map.json → lanelet.osm 转换器单元测试.

覆盖 M1_OSM_INTERFACE_CONTRACT.md §6 全部 7 个测试用例：
  1. 直道（手工造 1 road 2 lane，断言 relation/way 数量与中心线节点数）
  2. 弯道（含 curvature，断言坐标非线性）
  3. 单向 vs 双向（one_way tag yes/no）
  4. 含 traffic_light（断言 regulatory_element subtype）
  5. 字节稳定性（同 map.json 跑两次输出 bytes 相等）
  6. 缺字段（speed_limit 缺失 → 该 tag 不写）
  7. 大地图（osm_lujiazui_v2/map.json → 成功生成 .osm）
"""

from __future__ import annotations

import json
import math
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.json_to_lanelet import convert_file, convert_map_dict


class TestJsonToLanelet(unittest.TestCase):

    def setUp(self) -> None:
        self.tmp_dir = tempfile.mkdtemp()
        self.tmp_path = Path(self.tmp_dir)

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp_dir, ignore_errors=True)

    def test_01_straight_road_one_road_two_lanes(self) -> None:
        """用例 1: 直道（手工造 1 road 2 lane，断言 relation/way 数量与中心线节点数）"""
        map_data = {
            "schema_version": 1,
            "map_id": "straight_test",
            "name": "Straight Test",
            "roads": [
                {
                    "id": "road_1",
                    "type": "urban",
                    "speed_limit": 13.89,
                    "oneway": True,
                    "lanes": [
                        {
                            "id": "road_1.lane.1",
                            "index": 1,
                            "width": 3.5,
                            "direction": 1,
                            "centerline": [
                                [0.0, 0.0, 0.0],
                                [50.0, 0.0, 0.0],
                                [100.0, 0.0, 0.0],
                            ],
                        },
                        {
                            "id": "road_1.lane.2",
                            "index": 2,
                            "width": 3.5,
                            "direction": 1,
                            "centerline": [
                                [0.0, -3.5, 0.0],
                                [50.0, -3.5, 0.0],
                                [100.0, -3.5, 0.0],
                            ],
                        },
                    ],
                }
            ],
        }
        xml_str = convert_map_dict(map_data)
        root = ET.fromstring(xml_str)

        self.assertEqual(root.tag, "osm")
        self.assertEqual(root.get("version"), "0.6")
        self.assertEqual(root.get("generator"), "json_to_lanelet.py")

        # 2 条车道 → 2 个 lanelet relation
        relations = root.findall("relation")
        self.assertEqual(len(relations), 2)

        # 每条车道有 left/right 2 个 way → 共 4 个 way
        ways = root.findall("way")
        self.assertEqual(len(ways), 4)

        way_by_id = {w.get("id"): w for w in ways}

        # 验证每个 relation 的 left/right way 节点数等于 centerline 节点数 (3)
        for rel in relations:
            members = rel.findall("member")
            self.assertEqual(len(members), 2)
            left_ref = next(m.get("ref") for m in members if m.get("role") == "left")
            right_ref = next(m.get("ref") for m in members if m.get("role") == "right")

            left_nds = way_by_id[left_ref].findall("nd")
            right_nds = way_by_id[right_ref].findall("nd")
            self.assertEqual(len(left_nds), 3)
            self.assertEqual(len(right_nds), 3)

        # 验证节点总数: 2 lanes * 2 ways * 3 points = 12 nodes
        nodes = root.findall("node")
        self.assertEqual(len(nodes), 12)

    def test_02_curved_road_non_linear_coordinates(self) -> None:
        """用例 2: 弯道（含 curvature，断言坐标非线性）"""
        # 生成一段半径 R=100m 的圆弧，中心位于 (0, 100)
        pts = []
        for deg in [0.0, 15.0, 30.0, 45.0, 60.0]:
            rad = math.radians(deg)
            # x = 100 * sin(rad), y = 100 - 100 * cos(rad)
            pts.append([round(100.0 * math.sin(rad), 3), round(100.0 - 100.0 * math.cos(rad), 3), 0.0])

        map_data = {
            "schema_version": 1,
            "map_id": "curved_test",
            "name": "Curved Test",
            "roads": [
                {
                    "id": "road_curve",
                    "type": "urban",
                    "lanes": [
                        {
                            "id": "road_curve.lane.1",
                            "index": 1,
                            "width": 3.5,
                            "centerline": pts,
                        }
                    ],
                }
            ],
        }
        xml_str = convert_map_dict(map_data)
        root = ET.fromstring(xml_str)

        node_dict = {n.get("id"): (float(n.get("lon")), float(n.get("lat"))) for n in root.findall("node")}
        left_way = root.find("way")
        self.assertIsNotNone(left_way)

        left_coords = [node_dict[nd.get("ref")] for nd in left_way.findall("nd")]
        self.assertEqual(len(left_coords), 5)

        # 断言坐标非线性: 计算相邻段斜率，斜率随弧度变化
        slopes = []
        for i in range(len(left_coords) - 1):
            dx = left_coords[i + 1][0] - left_coords[i][0]
            dy = left_coords[i + 1][1] - left_coords[i][1]
            slopes.append(dy / dx if abs(dx) > 1e-6 else 999.0)

        # 随着转角增加，斜率 dy/dx 逐渐增大
        for i in range(len(slopes) - 1):
            self.assertGreater(slopes[i + 1], slopes[i])

    def test_03_oneway_vs_twoway(self) -> None:
        """用例 3: 单向 vs 双向（one_way tag 分别为 yes/no）"""
        map_data = {
            "schema_version": 1,
            "map_id": "oneway_test",
            "roads": [
                {
                    "id": "road_oneway",
                    "oneway": True,
                    "lanes": [
                        {
                            "id": "road_oneway.lane.1",
                            "centerline": [[0.0, 0.0], [10.0, 0.0]],
                        }
                    ],
                },
                {
                    "id": "road_twoway",
                    "oneway": False,
                    "lanes": [
                        {
                            "id": "road_twoway.lane.1",
                            "centerline": [[0.0, 10.0], [10.0, 10.0]],
                        }
                    ],
                },
            ],
        }
        xml_str = convert_map_dict(map_data)
        root = ET.fromstring(xml_str)

        rel_by_lane_id = {}
        for rel in root.findall("relation"):
            tags = {t.get("k"): t.get("v") for t in rel.findall("tag")}
            if tags.get("type") == "lanelet":
                rel_by_lane_id[tags.get("lanelet_id")] = tags

        self.assertEqual(rel_by_lane_id["road_oneway.lane.1"].get("one_way"), "yes")
        self.assertEqual(rel_by_lane_id["road_twoway.lane.1"].get("one_way"), "no")

    def test_04_traffic_light_regulatory_element(self) -> None:
        """用例 4: 含 traffic_light（断言 regulatory_element subtype）"""
        map_data = {
            "schema_version": 1,
            "map_id": "tl_test",
            "roads": [
                {
                    "id": "road_tl",
                    "lanes": [
                        {
                            "id": "road_tl.lane.1",
                            "centerline": [[0.0, 0.0], [50.0, 0.0]],
                        }
                    ],
                }
            ],
            "traffic_lights": [
                {
                    "id": 101,
                    "lane": "road_tl.lane.1",
                }
            ],
        }
        xml_str = convert_map_dict(map_data)
        root = ET.fromstring(xml_str)

        relations = root.findall("relation")
        # 1 个 lanelet relation + 1 个 regulatory_element relation
        self.assertEqual(len(relations), 2)

        lanelet_rel = relations[0]
        reg_rel = relations[1]

        # 断言 RT 出现在车道 relation 之后
        self.assertTrue(reg_rel.get("id").startswith("RT"))
        reg_tags = {t.get("k"): t.get("v") for t in reg_rel.findall("tag")}
        self.assertEqual(reg_tags.get("type"), "regulatory_element")
        self.assertEqual(reg_tags.get("subtype"), "traffic_light")
        self.assertEqual(reg_tags.get("lanelet_id"), "road_tl.lane.1")

        # 检查 member roles: stop_line 和 ref_line
        reg_members = {m.get("role"): m for m in reg_rel.findall("member")}
        self.assertIn("stop_line", reg_members)
        self.assertIn("ref_line", reg_members)

        # 停止线 way tag 必须为 stop_line
        stop_way_id = reg_members["stop_line"].get("ref")
        stop_way = next(w for w in root.findall("way") if w.get("id") == stop_way_id)
        stop_tags = {t.get("k"): t.get("v") for t in stop_way.findall("tag")}
        self.assertEqual(stop_tags.get("type"), "stop_line")

    def test_05_byte_level_determinism(self) -> None:
        """用例 5: 字节稳定性（同 map.json 跑两次输出 bytes 相等）"""
        test_map_path = ROOT / "maps" / "osm_test" / "map.json"
        self.assertTrue(test_map_path.exists(), f"Missing {test_map_path}")

        out1 = convert_file(test_map_path)
        out2 = convert_file(test_map_path)

        self.assertEqual(len(out1), len(out2))
        self.assertEqual(out1, out2)
        self.assertEqual(out1.encode("utf-8"), out2.encode("utf-8"))

    def test_06_missing_speed_limit_tag_omitted(self) -> None:
        """用例 6: 缺字段（speed_limit 缺失 → 该 tag 不写）"""
        map_data = {
            "schema_version": 1,
            "map_id": "no_speed_limit",
            "roads": [
                {
                    "id": "road_no_speed",
                    "type": "urban",
                    # 缺少 speed_limit 字段
                    "lanes": [
                        {
                            "id": "road_no_speed.lane.1",
                            "centerline": [[0.0, 0.0], [20.0, 0.0]],
                        }
                    ],
                }
            ],
        }
        xml_str = convert_map_dict(map_data)
        root = ET.fromstring(xml_str)

        rel = root.find("relation")
        self.assertIsNotNone(rel)

        tag_keys = [t.get("k") for t in rel.findall("tag")]
        self.assertNotIn("speed_limit", tag_keys)

        # 验证固定 tag 顺序（缺 speed_limit 时直接跳过）
        expected_keys = ["type", "subtype", "location", "one_way", "lanelet_id"]
        self.assertEqual(tag_keys, expected_keys)

    def test_07_large_map_osm_lujiazui_v2(self) -> None:
        """用例 7: 大地图（osm_lujiazui_v2/map.json → 成功生成 .osm）"""
        lujiazui_map = ROOT / "maps" / "osm_lujiazui_v2" / "map.json"
        self.assertTrue(lujiazui_map.exists(), f"Missing {lujiazui_map}")

        out_osm = self.tmp_path / "lujiazui.osm"
        xml_str = convert_file(lujiazui_map)
        out_osm.write_text(xml_str, encoding="utf-8")

        self.assertTrue(out_osm.exists())
        self.assertGreater(out_osm.stat().st_size, 1_000_000)

        # 验证 XML 解析与根属性
        root = ET.fromstring(xml_str)
        self.assertEqual(root.tag, "osm")
        self.assertEqual(root.get("version"), "0.6")
        self.assertEqual(root.get("generator"), "json_to_lanelet.py")

        # 统计原地图车道总数
        raw_data = json.loads(lujiazui_map.read_text(encoding="utf-8"))
        expected_lane_count = sum(len(r.get("lanes", [])) for r in raw_data.get("roads", []))
        self.assertEqual(expected_lane_count, 4620)

        lanelet_relations = [
            r for r in root.findall("relation")
            if any(t.get("k") == "type" and t.get("v") == "lanelet" for t in r.findall("tag"))
        ]
        self.assertEqual(len(lanelet_relations), expected_lane_count)

    def test_08_cli_execution_and_error_handling(self) -> None:
        """验证 CLI 接口调用与缺失 centerline 时的非零退出码"""
        cli_script = ROOT / "tools" / "json_to_lanelet.py"
        test_json = self.tmp_path / "test.json"
        out_osm = self.tmp_path / "out.osm"

        # 1. 成功 CLI 转换
        valid_map = {
            "roads": [
                {
                    "id": "r1",
                    "lanes": [{"id": "r1.lane.1", "centerline": [[0.0, 0.0], [10.0, 0.0]]}]
                }
            ]
        }
        test_json.write_text(json.dumps(valid_map), encoding="utf-8")
        cmd = [sys.executable, str(cli_script), str(test_json), "-o", str(out_osm)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0)
        self.assertTrue(out_osm.exists())
        self.assertIn("✓", res.stderr)

        # 2. 错误地图（缺少 centerline）导致非零退出码
        bad_json = self.tmp_path / "bad.json"
        bad_map = {"roads": [{"id": "bad", "lanes": [{"id": "bad.lane.1"}]}]}
        bad_json.write_text(json.dumps(bad_map), encoding="utf-8")
        bad_cmd = [sys.executable, str(cli_script), str(bad_json)]
        bad_res = subprocess.run(bad_cmd, capture_output=True, text=True)
        self.assertNotEqual(bad_res.returncode, 0)
        self.assertIn("Error", bad_res.stderr)


if __name__ == "__main__":
    unittest.main()
