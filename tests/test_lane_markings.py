"""test_lane_markings.py — 标线生产规则零回归 + 匝道减速类型。"""
import unittest

from tools.lane_markings import markings_for_lane, is_ramp_type


def types_of(mk):
    return [(m["type"], m["side"]) for m in mk]


class LaneMarkingsTest(unittest.TestCase):
    def test_oneway_single_lane(self):
        mk = markings_for_lane(1, 1, oneway=True)
        self.assertEqual(types_of(mk), [
            ("solid_white", "left"),
            ("solid_white", "right"),
        ])

    def test_oneway_two_lanes(self):
        inner = markings_for_lane(1, 2, oneway=True)
        outer = markings_for_lane(2, 2, oneway=True)
        self.assertEqual(types_of(inner), [
            ("solid_white", "left"),
            ("dashed_white", "right"),
        ])
        self.assertEqual(types_of(outer), [
            ("solid_white", "right"),
        ])

    def test_bidirectional_forward_and_opp(self):
        fwd = markings_for_lane(1, 2, oneway=False, is_opp=False)
        opp = markings_for_lane(1, 2, oneway=False, is_opp=True)
        self.assertIn(("double_yellow", "left"), types_of(fwd))
        self.assertIn(("double_yellow", "right"), types_of(opp))
        self.assertIn(("dashed_white", "right"), types_of(fwd))
        self.assertIn(("dashed_white", "left"), types_of(opp))

    def test_urban_no_deceleration(self):
        mk = markings_for_lane(1, 1, oneway=True, road_type="urban")
        self.assertFalse(any(t == "deceleration" for t, _ in types_of(mk)))

    def test_ramp_adds_deceleration_on_outer(self):
        self.assertTrue(is_ramp_type("ramp_curve"))
        self.assertTrue(is_ramp_type("motorway_link"))
        self.assertFalse(is_ramp_type("urban"))
        outer = markings_for_lane(2, 2, oneway=True, road_type="ramp_curve")
        inner = markings_for_lane(1, 2, oneway=True, road_type="ramp_curve")
        self.assertIn(("deceleration", "right"), types_of(outer))
        self.assertNotIn("deceleration", [t for t, _ in types_of(inner)])

    def test_sumo_has_opposing_like_osm2kmap(self):
        mk = markings_for_lane(1, 1, oneway=True, has_opposing=True)
        self.assertEqual(types_of(mk), [
            ("double_yellow", "left"),
            ("solid_white", "right"),
        ])


if __name__ == "__main__":
    unittest.main()
