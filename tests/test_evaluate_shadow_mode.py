#!/usr/bin/env python3
"""test_evaluate_shadow_mode.py — 仿真闭环与影子模式评测引擎单元测试"""

import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.evaluate_shadow_mode import (
    calculate_trajectory_ade_fde,
    calculate_control_deviations,
    evaluate_shadow_sidecar,
    evaluate_scenario_entry,
    generate_markdown_report,
    run_benchmark,
    SCHEMA_VERSION,
)


class TestEvaluateShadowMode(unittest.TestCase):

    def test_trajectory_ade_fde_zero_on_identical(self):
        pred = [{"x": 0.0, "y": 0.0}, {"x": 2.0, "y": 0.0}, {"x": 4.0, "y": 0.0}]
        gt = [{"x": 0.0, "y": 0.0}, {"x": 2.0, "y": 0.0}, {"x": 4.0, "y": 0.0}]
        res = calculate_trajectory_ade_fde(pred, gt)
        self.assertEqual(res["status"], "ok")
        self.assertAlmostEqual(res["ade"], 0.0)
        self.assertAlmostEqual(res["fde"], 0.0)
        self.assertAlmostEqual(res["max_err"], 0.0)

    def test_trajectory_ade_fde_constant_offset(self):
        pred = [{"x": 0.0, "y": 1.0}, {"x": 2.0, "y": 1.0}, {"x": 4.0, "y": 1.0}]
        gt = [{"x": 0.0, "y": 0.0}, {"x": 2.0, "y": 0.0}, {"x": 4.0, "y": 0.0}]
        res = calculate_trajectory_ade_fde(pred, gt)
        self.assertEqual(res["status"], "ok")
        self.assertAlmostEqual(res["ade"], 1.0)
        self.assertAlmostEqual(res["fde"], 1.0)
        self.assertAlmostEqual(res["max_err"], 1.0)

    def test_trajectory_ade_fde_diverging(self):
        pred = [[0.0, 0.0], [2.0, 1.0], [4.0, 2.0]]
        gt = [[0.0, 0.0], [2.0, 0.0], [4.0, 0.0]]
        res = calculate_trajectory_ade_fde(pred, gt)
        self.assertEqual(res["status"], "ok")
        # distances: 0, 1, 2 -> mean = 1.0, fde = 2.0
        self.assertAlmostEqual(res["ade"], 1.0)
        self.assertAlmostEqual(res["fde"], 2.0)
        self.assertAlmostEqual(res["max_err"], 2.0)

    def test_trajectory_empty_or_invalid(self):
        res = calculate_trajectory_ade_fde([], [])
        self.assertEqual(res["status"], "empty")
        self.assertIsNone(res["ade"])

        res_inv = calculate_trajectory_ade_fde("not a list", [1, 2])
        self.assertEqual(res_inv["status"], "invalid")

    def test_control_deviations_metrics(self):
        rule = [
            {"speed": 10.0, "steer": 0.0, "brake": 0.0, "throttle": 0.3},
            {"speed": 10.0, "steer": 0.05, "brake": 0.20, "throttle": 0.0},
        ]
        model = [
            {"speed": 10.5, "steer": 0.02, "brake": 0.0, "throttle": 0.32},
            {"speed": 11.0, "steer": 0.20, "brake": 0.0, "throttle": 0.40},  # Steer envelope violation + brake priority override!
        ]
        res = calculate_control_deviations(rule, model)
        self.assertEqual(res["status"], "computed")
        self.assertEqual(res["sample_count"], 2)
        # speed deltas: 0.5, 1.0 -> MAE = 0.75
        self.assertAlmostEqual(res["speed_mae"], 0.75)
        # steer deltas: 0.02, 0.15 (>0.12 is violation) -> 1 envelope violation
        self.assertEqual(res["steer_envelope_violations"], 1)
        # 2nd sample: rule brake 0.20 and model throttle 0.40 -> 1 brake override
        self.assertEqual(res["brake_priority_overrides"], 1)
        self.assertEqual(res["total_interventions"], 2)
        self.assertEqual(res["intervention_rate_pct"], 100.0)

    def test_shadow_sidecar_evaluation_grades(self):
        # 1. 优秀通过
        pass_data = {
            "model": "onnx",
            "shadow_speed_mae_settled": 0.8,
            "shadow_steer_mae": 0.03,  # ~1.7 deg
            "shadow_ade": 0.4,
            "shadow_fde": 0.9,
            "shadow_settled_n": 50,
        }
        res_pass = evaluate_shadow_sidecar(pass_data)
        self.assertEqual(res_pass["grade"], "PASS")
        self.assertEqual(len(res_pass["issues"]), 0)

        # 2. 速度偏高告警
        warn_data = {
            "model": "tiny-mlp",
            "shadow_speed_mae_settled": 2.5,  # >2.0 WARN
            "shadow_settled_n": 40,
        }
        res_warn = evaluate_shadow_sidecar(warn_data)
        self.assertEqual(res_warn["grade"], "WARN")
        self.assertTrue(any("偏高" in iss for iss in res_warn["issues"]))

        # 3. 严重超限判定 FAIL
        fail_data = {
            "model": "tiny-mlp",
            "shadow_speed_mae_settled": 5.5,  # >5.0 FAIL
            "shadow_steer_mae": 0.15,         # ~8.6 deg > 6.9 deg FAIL
            "shadow_ade": 3.5,                # >3.0 FAIL
            "shadow_settled_n": 40,
        }
        res_fail = evaluate_shadow_sidecar(fail_data)
        self.assertEqual(res_fail["grade"], "FAIL")
        self.assertGreaterEqual(len(res_fail["issues"]), 3)

    def test_scenario_evaluation_and_matrix_report(self):
        scenario_a = {
            "scenario": "straight_road",
            "metrics": {
                "avg_speed_mps": 11.2,
                "duration_s": 20.0,
                "collision_topic_pub": 0,
                "min_ttc_s": 4.5,
                "shadow_speed_mae_settled": 0.9,
                "shadow_steer_mae": 0.02,
                "shadow_ade": 0.35,
            }
        }
        scenario_b = {
            "scenario": "dense_npc",
            "metrics": {
                "avg_speed_mps": 8.0,
                "duration_s": 25.0,
                "collision_topic_pub": 0,
                "min_ttc_s": 1.8,  # <2.5s WARN
                "shadow_speed_mae_settled": 1.4,
            }
        }

        with tempfile.TemporaryDirectory() as tmpdir:
            out_json = Path(tmpdir) / "matrix.json"
            out_md = Path(tmpdir) / "report.md"

            matrix = run_benchmark([scenario_a, scenario_b], out_json, out_md)

            self.assertEqual(matrix["schema"], SCHEMA_VERSION)
            self.assertEqual(matrix["summary"]["total_scenarios"], 2)
            self.assertEqual(matrix["summary"]["pass_count"], 1)
            self.assertEqual(matrix["summary"]["warn_count"], 1)
            self.assertEqual(matrix["summary"]["fail_count"], 0)
            self.assertEqual(matrix["overall_grade"], "WARN")
            self.assertTrue(out_json.exists())
            self.assertTrue(out_md.exists())

            report_content = out_md.read_text(encoding="utf-8")
            self.assertIn("FlowEngine 仿真闭环与影子模式评测报告", report_content)
            self.assertIn("straight_road", report_content)
            self.assertIn("dense_npc", report_content)


if __name__ == "__main__":
    unittest.main()
