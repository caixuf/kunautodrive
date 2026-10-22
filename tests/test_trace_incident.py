#!/usr/bin/env python3
"""事故追溯 JSON 契约。

live 仪表盘每 3s 调 analyze_incident。名义 TOR（ACTIVE/OFF）和空直道
不能被记成接管或「盲开」；碰撞、出路沿、TOR_REQUESTED、MRM 必须出事件。
"""

from __future__ import annotations

import unittest

from tools.trace_incident import analyze_incident


def _topo(tor_state="ACTIVE", samples=None, topics=None, mode="ACCEL"):
    if samples is None:
        samples = [
            {"t": 1000.0, "x": 10.0, "y": 0.0, "speed": 12.0, "heading": 0.0},
            {"t": 1001.0, "x": 22.0, "y": 0.1, "speed": 12.0, "heading": 0.0},
            {"t": 1002.0, "x": 34.0, "y": 0.0, "speed": 12.0, "heading": 0.0},
        ]
    return {
        "timestamp": 1_700_000_000_000_000,
        "samples": samples,
        "metrics": {
            "topics": topics or [],
            "scene": {"ego": {"x": 34, "y": 0, "heading": 0, "speed": 12},
                      "lane": {"count": 4, "width": 3.5},
                      "obstacles": []},
            "behavior": {"state": "CRUISE", "obs_count": 0, "best_gap": -1},
            "vehicle": {"speed": 12},
            "planning_debug": {},
            "control_debug": {"mode": mode, "mpc_used": 0, "steer": 0, "lat_error": 0},
            "tor": {
                "state": tor_state,
                "countdown_s": 0,
                "reason": "",
                "mrm": {"active": tor_state == "MRM", "target_speed": 0},
            },
        },
    }


class TestTraceIncident(unittest.TestCase):
    def test_cruise_active_tor_is_not_an_event(self):
        res = analyze_incident(_topo("ACTIVE"))
        self.assertTrue(res["ok"])
        self.assertIsNone(res["incident"])
        self.assertEqual(res["events"], [])
        self.assertEqual(res["summary"]["status"], "pass")
        self.assertEqual(res["summary"]["has_incident"], False)
        self.assertFalse(any("盲开" in v for v in res["verdicts"]))

    def test_off_state_is_not_an_event(self):
        res = analyze_incident(_topo("OFF"))
        self.assertEqual(res["events"], [])
        self.assertEqual(res["summary"]["status"], "pass")

    def test_tor_requested_emits_event_without_false_blind_drive(self):
        res = analyze_incident(_topo("TOR_REQUESTED"))
        types = [e["type"] for e in res["events"]]
        self.assertEqual(types, ["l3_tor"])
        self.assertEqual(res["events"][0]["state"], "TOR_REQUESTED")
        self.assertIsNone(res["incident"])
        self.assertFalse(any("盲开" in v for v in res["verdicts"]))
        self.assertEqual(res["summary"]["status"], "pass")

    def test_mrm_from_tor_and_from_control_mode(self):
        by_state = analyze_incident(_topo("MRM"))
        self.assertEqual([e["type"] for e in by_state["events"]], ["l3_mrm"])

        by_mode = analyze_incident(_topo("ACTIVE", mode="MRM+MANEUVER"))
        self.assertEqual([e["type"] for e in by_mode["events"]], ["l3_mrm"])

    def test_road_departure_picks_the_off_road_sample(self):
        samples = [
            {"t": 50.0, "x": 1.0, "y": 0.0, "speed": 8.0, "heading": 0.0},
            {"t": 51.0, "x": 5.0, "y": 0.1, "speed": 8.0, "heading": 0.0},
            {"t": 52.0, "x": 9.0, "y": 0.2, "speed": 8.0, "heading": 0.0},
            {"t": 53.0, "x": 13.0, "y": 0.2, "speed": 8.0, "heading": 0.0},
            {"t": 54.0, "x": 17.0, "y": 9.5, "speed": 7.0, "heading": 0.2},
        ]
        res = analyze_incident(_topo(samples=samples))
        self.assertEqual(res["incident"]["type"], "road_departure")
        self.assertAlmostEqual(res["incident"]["at_s"], 4.0)
        self.assertTrue(any(e["type"] == "road_departure" for e in res["events"]))
        self.assertAlmostEqual(res["layers"]["localization"]["x"], 17.0)
        self.assertTrue(res["layers"]["localization"]["off_road"])
        self.assertEqual(res["summary"]["status"], "fail")

    def test_at_selects_relative_sample_not_wall_clock(self):
        samples = [
            {"t": 5000.0, "x": 1.0, "y": 0.0, "speed": 5.0, "heading": 0.0},
            {"t": 5001.0, "x": 40.0, "y": 0.0, "speed": 5.0, "heading": 0.0},
            {"t": 5002.0, "x": 80.0, "y": 0.0, "speed": 5.0, "heading": 0.0},
        ]
        res = analyze_incident(_topo(samples=samples), at_s=1.0)
        self.assertEqual(res["incident"]["type"], "manual")
        self.assertAlmostEqual(res["layers"]["localization"]["x"], 40.0)

    def test_collision_topic_emits_event(self):
        res = analyze_incident(_topo(topics=[{"topic": "sim/collision", "pub": 3}]))
        self.assertEqual(res["incident"]["type"], "collision")
        self.assertTrue(any(e["type"] == "l3_collision" for e in res["events"]))


if __name__ == "__main__":
    unittest.main()
