#!/usr/bin/env python3
"""tests/test_lanelet_consistency_rule4_5.py — D2-08 step 2 新增 Rule 4/5 单测.

按 docs/_reports/D2_08_design.md §5.2 收尾：
- Rule 4: 每个 map.json 有 speed_limit 的 lane ↔ .osm 里 subtype=speed_limit regulatory 双向覆盖 + 数值一致
- Rule 5: 每个 map.json.landmarks.stop_lines[] item（按 lane 聚合）↔ .osm 里 subtype=stop_line regulatory 双向覆盖

本测试不依赖 maps/* 下真实数据（用 tempfile 自造小 fixture），
原因是真实 osm_lujiazui_v2 等大地图没有 stop_lines / lane-level speed_limit override，
Rule 4 Forward value-mismatch 与 Rule 5 Backward 都触发不到。
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ci" / "gates"))

from lanelet_consistency_check import (  # noqa: E402
    check_consistency,
    parse_regulatory_elements,
    _map_speed_limits,
    _map_stop_lines,
)


def _build_map(lanes_spec: list[dict], stop_lines: list[dict] | None = None) -> dict:
    """造一个最简 map.json.

    lanes_spec: [{id, road_speed, lane_speed, x0, y0}, ...]
    stop_lines: [{lane}, ...] 走 landmarks.stop_lines[]
    """
    # 按 road_speed 分桶
    roads: dict[float, list[dict]] = {}
    for spec in lanes_spec:
        rs = spec["road_speed"]
        roads.setdefault(rs, []).append(spec)
    roads_list = []
    for rs, specs in roads.items():
        lanes = [
            {
                "id": s["id"],
                "width": 3.5,
                "direction": 1,
                "centerline": [[s["x0"], s["y0"], 0], [s["x0"] + 10, s["y0"], 0]],
                **({"speed_limit": s["lane_speed"]} if s.get("lane_speed") is not None else {}),
            }
            for s in specs
        ]
        roads_list.append({"id": f"r_{rs}", "type": "urban", "speed_limit": rs, "lanes": lanes})
    return {
        "schema_version": 1,
        "map_id": "rule4_5_test",
        "name": "Rule 4/5 Test",
        "roads": roads_list,
        "landmarks": {"stop_lines": stop_lines or []},
    }


def _build_osm(lanelet_ids: list[str], speed_limits: dict[str, float] | None = None,
               stop_lines_per_lane: dict[str, int] | None = None,
               extra_regulatory: list[tuple[str, str, str]] | None = None) -> str:
    """造一个最简 .osm.

    - 每个 lane 1 个 lanelet relation（centerline 起点 = (x0+5, y0+5)，
      故意设成跟 map.json centerline 起点 [x0, y0] 偏差 5m → Rule 3 必 FAIL，
      本测试**只关注 Rule 4/5**，会从 errs 过滤 Rule 3 错误后断言）
    - speed_limits: {lane_id: mps} → 每个写 1 个 subtype=speed_limit regulatory
    - stop_lines_per_lane: {lane_id: count} → 每个 lane 写 count 个 subtype=stop_line regulatory
    - extra_regulatory: [(subtype, lane_id, value?)] → 额外 regulatory 用来测 Backward
    """
    speed_limits = speed_limits or {}
    stop_lines_per_lane = stop_lines_per_lane or {}
    extra_regulatory = extra_regulatory or []

    nodes: list[str] = []
    ways: list[str] = []
    rels: list[str] = []

    lane_to_ways: dict[str, tuple[int, int]] = {}
    node_id = 0
    way_id = 0
    rel_id = 0
    rt_id = 0

    for idx, lid in enumerate(lanelet_ids):
        # 每个 lane 中心点偏移 (10*idx, 0)，构造 left = (10*idx, 0), right = (10*idx+5, 0)
        # 这样 lanelet 几何中点 = (10*idx+2.5, 0)，map.json centerline 起点 = (10*idx, 0)
        # Rule 3 偏差 = 2.5m < COORD_TOLERANCE_M=1.0？实际 > 1.0 → Rule 3 必 FAIL，
        # 但**本测试只断言 Rule 4/5 错误**，通过 prefix 过滤
        x_base = 10 * idx
        l_first = node_id; node_id += 1
        l_last = node_id; node_id += 1
        r_first = node_id; node_id += 1
        r_last = node_id; node_id += 1
        # left: y=0 (lat=0), right: y=5 (lat=5)
        # 几何中点：(10*idx+2.5, 2.5)
        # map.json centerline 起点 = (10*idx, 0) → Rule 3 偏差 = hypot(2.5, 2.5) = 3.54m
        nodes.append(f'  <node id="N{l_first}" lat="0.0" lon="{x_base}.0" />')
        nodes.append(f'  <node id="N{l_last}" lat="0.0" lon="{x_base + 10}.0" />')
        nodes.append(f'  <node id="N{r_first}" lat="5.0" lon="{x_base}.0" />')
        nodes.append(f'  <node id="N{r_last}" lat="5.0" lon="{x_base + 10}.0" />')

        w_left = way_id; way_id += 1
        w_right = way_id; way_id += 1
        ways.append(
            f'  <way id="W{w_left}">\n'
            f'    <nd ref="N{l_first}" /><nd ref="N{l_last}" />\n'
            f'    <tag k="type" v="line_thin" />\n'
            f'  </way>'
        )
        ways.append(
            f'  <way id="W{w_right}">\n'
            f'    <nd ref="N{r_first}" /><nd ref="N{r_last}" />\n'
            f'    <tag k="type" v="line_thin" />\n'
            f'  </way>'
        )
        rels.append(
            f'  <relation id="R{rel_id}">\n'
            f'    <member type="way" ref="W{w_left}" role="left" />\n'
            f'    <member type="way" ref="W{w_right}" role="right" />\n'
            f'    <tag k="type" v="lanelet" />\n'
            f'    <tag k="subtype" v="road" />\n'
            f'    <tag k="lanelet_id" v="{lid}" />\n'
            f'  </relation>'
        )
        lane_to_ways[lid] = (w_left, w_right)
        rel_id += 1

    # speed_limit regulatory
    for lid, sl in speed_limits.items():
        if lid not in lane_to_ways:
            continue
        w_ref = lane_to_ways[lid][0]
        rels.append(
            f'  <relation id="RT{rt_id}">\n'
            f'    <member type="way" ref="W{w_ref}" role="ref_line" />\n'
            f'    <tag k="type" v="regulatory_element" />\n'
            f'    <tag k="subtype" v="speed_limit" />\n'
            f'    <tag k="speed_limit" v="{sl:.2f}" />\n'
            f'    <tag k="lanelet_id" v="{lid}" />\n'
            f'  </relation>'
        )
        rt_id += 1

    # stop_line regulatory
    for lid, count in stop_lines_per_lane.items():
        if lid not in lane_to_ways:
            continue
        for _ in range(count):
            w_ref = lane_to_ways[lid][0]
            rels.append(
                f'  <relation id="RT{rt_id}">\n'
                f'    <member type="way" ref="W{w_ref}" role="ref_line" />\n'
                f'    <tag k="type" v="regulatory_element" />\n'
                f'    <tag k="subtype" v="stop_line" />\n'
                f'    <tag k="lanelet_id" v="{lid}" />\n'
                f'  </relation>'
            )
            rt_id += 1

    # extra regulatory（用于测 Backward 漏报）
    for subtype, lid, val in extra_regulatory:
        # 需要找一个 ref_way；优先用现有 lane
        if lid in lane_to_ways:
            w_ref = lane_to_ways[lid][0]
        else:
            # 随便挂 W0（orphan）—— 只要 subtype+lane_id 一致，gate 就当 Backward 漏报
            w_ref = 0
        tag_extra = f'    <tag k="speed_limit" v="{val}" />\n' if subtype == "speed_limit" else ""
        rels.append(
            f'  <relation id="RT{rt_id}">\n'
            f'    <member type="way" ref="W{w_ref}" role="ref_line" />\n'
            f'    <tag k="type" v="regulatory_element" />\n'
            f'    <tag k="subtype" v="{subtype}" />\n'
            f'    {tag_extra}'
            f'    <tag k="lanelet_id" v="{lid}" />\n'
            f'  </relation>'
        )
        rt_id += 1

    body = "\n".join(nodes) + "\n" + "\n".join(ways) + "\n" + "\n".join(rels)
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<osm version="0.6" generator="rule4_5_test">\n'
        + body
        + "\n</osm>\n"
    )


def _filter_rule45(errs: list[str]) -> list[str]:
    """从 check_consistency 输出中筛 Rule 4/5 错误（去 Rule 1/2/3 噪声）."""
    out = []
    for e in errs:
        if e.startswith("坐标偏差") or "多 lane" in e or "多 lanelet_id" in e:
            continue
        out.append(e)
    return out


class TestRule4Forward(unittest.TestCase):
    """Rule 4 Forward: 每个 map.json 有 speed_limit 的 lane 在 .osm 里能找到对应 regulatory + 数值一致."""

    def test_all_match(self) -> None:
        """正例: map.json 2 lane 各有 road speed_limit, osm 写齐 → 0 错误."""
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            op = Path(td) / "lanelet.osm"
            mp.write_text(json.dumps(_build_map([
                {"id": "lane_a", "road_speed": 10, "lane_speed": None, "x0": 0, "y0": 0},
                {"id": "lane_b", "road_speed": 20, "lane_speed": None, "x0": 10, "y0": 0},
            ])))
            op.write_text(_build_osm(
                lanelet_ids=["lane_a", "lane_b"],
                speed_limits={"lane_a": 10.00, "lane_b": 20.00},
            ))
            errs = _filter_rule45(check_consistency(mp, op))
            self.assertEqual(errs, [], f"unexpected: {errs}")

    def test_lane_override(self) -> None:
        """lane.speed_limit 覆盖 road.speed_limit → osm 必须跟 lane override 值一致."""
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            op = Path(td) / "lanelet.osm"
            mp.write_text(json.dumps(_build_map([
                {"id": "lane_x", "road_speed": 10, "lane_speed": 30, "x0": 0, "y0": 0},
            ])))
            op.write_text(_build_osm(
                lanelet_ids=["lane_x"],
                speed_limits={"lane_x": 30.00},  # 必须跟 lane override 一致
            ))
            errs = _filter_rule45(check_consistency(mp, op))
            self.assertEqual(errs, [], f"unexpected: {errs}")

    def test_missing_regulatory(self) -> None:
        """map.json 声明 speed_limit 但 osm 漏 regulatory → FAIL."""
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            op = Path(td) / "lanelet.osm"
            mp.write_text(json.dumps(_build_map([
                {"id": "lane_a", "road_speed": 10, "lane_speed": None, "x0": 0, "y0": 0},
                {"id": "lane_b", "road_speed": 20, "lane_speed": None, "x0": 10, "y0": 0},
            ])))
            op.write_text(_build_osm(
                lanelet_ids=["lane_a", "lane_b"],
                speed_limits={"lane_a": 10.00},  # lane_b 漏
            ))
            errs = _filter_rule45(check_consistency(mp, op))
            self.assertEqual(len(errs), 1, f"want 1 err, got {errs}")
            self.assertIn("lane_b", errs[0])
            self.assertIn("speed_limit=20", errs[0])
            self.assertIn("找不到 subtype=speed_limit regulatory", errs[0])

    def test_value_mismatch(self) -> None:
        """map.json 声明 10 m/s, osm 写 12 m/s → FAIL（数值偏差）."""
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            op = Path(td) / "lanelet.osm"
            mp.write_text(json.dumps(_build_map([
                {"id": "lane_a", "road_speed": 10, "lane_speed": None, "x0": 0, "y0": 0},
            ])))
            op.write_text(_build_osm(
                lanelet_ids=["lane_a"],
                speed_limits={"lane_a": 12.00},
            ))
            errs = _filter_rule45(check_consistency(mp, op))
            self.assertEqual(len(errs), 1, f"want 1 err, got {errs}")
            self.assertIn("speed_limit 数值偏差", errs[0])
            self.assertIn("lane_a", errs[0])
            self.assertIn("10.0000", errs[0])
            self.assertIn("12.0000", errs[0])

    def test_value_within_tolerance(self) -> None:
        """数值偏差 ≤ 0.01 m/s → PASS（契约 §2.5.2 精度 2 位小数）."""
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            op = Path(td) / "lanelet.osm"
            mp.write_text(json.dumps(_build_map([
                {"id": "lane_a", "road_speed": 10.00, "lane_speed": None, "x0": 0, "y0": 0},
            ])))
            op.write_text(_build_osm(
                lanelet_ids=["lane_a"],
                speed_limits={"lane_a": 10.005},  # 偏差 0.005 < 0.01
            ))
            errs = _filter_rule45(check_consistency(mp, op))
            self.assertEqual(errs, [], f"unexpected: {errs}")


class TestRule4Backward(unittest.TestCase):
    """Rule 4 Backward: osm 写了 regulatory 但 map.json 没声明 → FAIL."""

    def test_extra_regulatory_no_map_decl(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            op = Path(td) / "lanelet.osm"
            mp.write_text(json.dumps(_build_map([
                {"id": "lane_a", "road_speed": 10, "lane_speed": None, "x0": 0, "y0": 0},
            ])))
            op.write_text(_build_osm(
                lanelet_ids=["lane_a"],
                speed_limits={"lane_a": 10.00},
                extra_regulatory=[("speed_limit", "phantom_lane", "50.00")],
            ))
            errs = _filter_rule45(check_consistency(mp, op))
            self.assertEqual(len(errs), 1, f"want 1 err, got {errs}")
            self.assertIn("phantom_lane", errs[0])
            self.assertIn("map.json 找不到对应 speed_limit 声明", errs[0])


class TestRule5Forward(unittest.TestCase):
    """Rule 5 Forward: landmarks.stop_lines[] count 与 .osm subtype=stop_line regulatory 一致."""

    def test_count_match(self) -> None:
        """lane_a 有 2 个 stop_line, osm 也写 2 个 → PASS."""
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            op = Path(td) / "lanelet.osm"
            mp.write_text(json.dumps(_build_map(
                [{"id": "lane_a", "road_speed": 10, "lane_speed": None, "x0": 0, "y0": 0}],
                stop_lines=[{"lane": "lane_a"}, {"lane": "lane_a"}],
            )))
            op.write_text(_build_osm(
                lanelet_ids=["lane_a"],
                speed_limits={"lane_a": 10.00},
                stop_lines_per_lane={"lane_a": 2},
            ))
            errs = _filter_rule45(check_consistency(mp, op))
            self.assertEqual(errs, [], f"unexpected: {errs}")

    def test_count_mismatch(self) -> None:
        """map.json 声明 1 个, osm 写 2 个 → FAIL."""
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            op = Path(td) / "lanelet.osm"
            mp.write_text(json.dumps(_build_map(
                [{"id": "lane_a", "road_speed": 10, "lane_speed": None, "x0": 0, "y0": 0}],
                stop_lines=[{"lane": "lane_a"}],
            )))
            op.write_text(_build_osm(
                lanelet_ids=["lane_a"],
                speed_limits={"lane_a": 10.00},
                stop_lines_per_lane={"lane_a": 2},
            ))
            errs = _filter_rule45(check_consistency(mp, op))
            self.assertEqual(len(errs), 1, f"want 1 err, got {errs}")
            self.assertIn("stop_line 数量偏差", errs[0])
            self.assertIn("lane_a", errs[0])
            self.assertIn("map.json=1", errs[0])
            self.assertIn(".osm=2", errs[0])


class TestRule5Backward(unittest.TestCase):
    """Rule 5 Backward: osm 写了 stop_line 但 map.json 没声明 → FAIL."""

    def test_extra_stop_line_no_decl(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            op = Path(td) / "lanelet.osm"
            mp.write_text(json.dumps(_build_map([
                {"id": "lane_a", "road_speed": 10, "lane_speed": None, "x0": 0, "y0": 0},
            ])))
            op.write_text(_build_osm(
                lanelet_ids=["lane_a"],
                speed_limits={"lane_a": 10.00},
                extra_regulatory=[("stop_line", "phantom_lane", "")],
            ))
            errs = _filter_rule45(check_consistency(mp, op))
            self.assertEqual(len(errs), 1, f"want 1 err, got {errs}")
            self.assertIn("phantom_lane", errs[0])
            self.assertIn("stop_line", errs[0])


class TestHelpers(unittest.TestCase):
    """单元测试新加的 helper 函数本身."""

    def test_map_speed_limits_lane_override(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            mp.write_text(json.dumps(_build_map([
                {"id": "a", "road_speed": 10, "lane_speed": None, "x0": 0, "y0": 0},
                {"id": "b", "road_speed": 10, "lane_speed": 30, "x0": 0, "y0": 0},
                {"id": "c", "road_speed": 10, "lane_speed": None, "x0": 0, "y0": 0},
            ])))
            sl = _map_speed_limits(mp)
            self.assertEqual(sl, {"a": 10.0, "b": 30.0, "c": 10.0})

    def test_map_stop_lines_aggregates_by_lane(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            mp = Path(td) / "map.json"
            mp.write_text(json.dumps(_build_map(
                [{"id": "a", "road_speed": 10, "lane_speed": None, "x0": 0, "y0": 0}],
                stop_lines=[{"lane": "a"}, {"lane": "a"}, {"lane": "b"}],
            )))
            sl = _map_stop_lines(mp)
            self.assertEqual(sl, {"a": 2, "b": 1})

    def test_parse_regulatory_skips_invalid_speed_limit(self) -> None:
        """speed_limit tag 写成非数字字符串 → parse_regulatory_elements 跳过（不抛异常）."""
        osm_data = """<?xml version="1.0" encoding="UTF-8"?>
<osm version="0.6" generator="test">
  <relation id="R0">
    <tag k="type" v="lanelet" />
    <tag k="subtype" v="road" />
    <tag k="lanelet_id" v="lane_a" />
  </relation>
  <relation id="RT0">
    <tag k="type" v="regulatory_element" />
    <tag k="subtype" v="speed_limit" />
    <tag k="speed_limit" v="13.89m/s" />
    <tag k="lanelet_id" v="lane_a" />
  </relation>
</osm>
"""
        with tempfile.TemporaryDirectory() as td:
            op = Path(td) / "lanelet.osm"
            op.write_text(osm_data)
            reg = parse_regulatory_elements(op)
            self.assertEqual(reg["speed_limit"], {})  # invalid speed_limit skipped
            self.assertEqual(reg["stop_line"], {})


if __name__ == "__main__":
    unittest.main(verbosity=2)
