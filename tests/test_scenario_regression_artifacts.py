import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ci.evaluators import scenario_regression
from ci.evaluators.scenario_regression import (
    archive_failed_result,
    merge_repeats,
    prepare_worker_workspace,
    sha256_file,
    write_run_manifest,
)


class ScenarioRegressionArtifactsTest(unittest.TestCase):
    def test_failed_result_is_archived_and_manifest_indexes_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results = root / "results"
            result_path = results / "curve_road.json"
            result_path.parent.mkdir()
            result_path.write_text(json.dumps({
                "scenario": "curve_road", "result": "FAIL",
                "failures": ["road departure"], "warnings": [], "summary": {},
            }), encoding="utf-8")

            archived = archive_failed_result(
                json.loads(result_path.read_text(encoding="utf-8")),
                result_path, root / "bad-cases", "run-001",
            )
            self.assertIsNotNone(archived)
            self.assertTrue(archived.is_file())

            suite_path = root / "suite.json"
            suite_path.write_text('{"name":"test","scenarios":[]}', encoding="utf-8")
            manifest = write_run_manifest(
                results, suite_path, {"name": "test"},
                [{"scenario": "curve_road", "result": "FAIL",
                  "failures": ["road departure"], "warnings": [],
                  "regressions": [], "result_path": str(result_path)}],
                "run-001",
            )
            data = json.loads(manifest.read_text(encoding="utf-8"))

        self.assertEqual(data["schema"], "flowengine.evaluation_run.v1")
        self.assertEqual(data["fail_count"], 1)
        self.assertEqual(data["results"][0]["scenario"], "curve_road")
        self.assertEqual(data["worker_count"], 1)

    def test_worker_workspace_isolates_monitor_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            workspace, env = prepare_worker_workspace(Path(tmp), "curve_road")
            pipeline = json.loads((workspace / "pipeline.json").read_text(encoding="utf-8"))
            monitor = next(node for node in pipeline["processes"] if node["name"] == "monitor")
            params = json.loads(monitor["params"])

            self.assertEqual(params["state_file"], env["FLOW_TOPOLOGY_FILE"])
            self.assertEqual(env["FLOW_SKIP_SERVICES"], "1")
            self.assertEqual(env["FLOW_SKIP_GLOBAL_CLEANUP"], "1")
            self.assertNotEqual(env["FLOW_TOPOLOGY_FILE"], "/tmp/flow_topology.json")

    def test_matrix_dispatches_isolated_workers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            suite_path = root / "suite.json"
            suite_path.write_text(json.dumps({
                "name": "parallel-test",
                "default_duration_s": 1,
                "scenarios": [
                    {"file": "scenarios/straight_road.json", "enabled": True},
                    {"file": "scenarios/curve_road.json", "enabled": True},
                ],
            }), encoding="utf-8")

            def fake_run(entry, _duration, _interval, _results_dir):
                key = Path(entry["file"]).stem
                return {
                    "scenario": key, "result": "PASS",
                    "failures": [], "warnings": [], "summary": {},
                }

            argv = [
                "scenario_regression.py", "--suite", str(suite_path),
                "--results-dir", str(root / "results"), "--workers", "2",
                "--no-archive",
            ]
            with patch.object(scenario_regression, "run_scenario", side_effect=fake_run), \
                    patch.object(sys, "argv", argv):
                self.assertEqual(scenario_regression.main(), 0)

            manifest = json.loads(
                (root / "results" / "run_manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["worker_count"], 2)
            self.assertEqual(manifest["pass_count"], 2)

    def test_sha256_is_stable(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "data"
            path.write_bytes(b"kunautodrive")
            self.assertEqual(
                sha256_file(path),
                "0e6ba1b786d8c0290d2e320355f0d0205297a8efd2393422d97cee7b8e7de280",
            )

    # ── --repeats：基线统计化（数值取中位 + 记录极差） ──────────────────
    # 动机：单样本基线在抖动大的场景上不成立 —— lane_change_traffic 实测
    # avg_speed_mps 三趟 9.506 / 13.353 / 13.409（极差 ≈29%），拿某一次当基线
    # 会让门槛随运气漂。见 HANDOFF_2026-09-23.md §16.2。

    def test_merge_repeats_takes_median_and_records_spread(self):
        def _p(speed, x_delta, state, flips=0.02):
            return {"scenario": "lane_change_traffic", "result": "PASS",
                    "failures": [], "warnings": [],
                    "summary": {"avg_speed_mps": speed, "x_delta_m": x_delta,
                                "behavior_state": state, "lane_change_count": 4,
                                "steer_flip_rate_hz": flips}}

        merged = merge_repeats([
            _p(9.506, 447.6, "FOLLOW"),
            _p(13.353, 565.2, "LEFT_CHANGE", flips=0.022),
            _p(13.409, 575.9, "FOLLOW"),
        ], 3)

        self.assertEqual(merged["result"], "PASS")
        # 数值取中位（不是首次、也不是均值）
        self.assertEqual(merged["summary"]["avg_speed_mps"], 13.353)
        self.assertEqual(merged["summary"]["x_delta_m"], 565.2)
        # 非数值取首次出现值
        self.assertEqual(merged["summary"]["behavior_state"], "FOLLOW")
        # 极差与样本数落盘（下划线前缀 → compare_summary 跳过，不参与门禁）
        self.assertEqual(merged["summary"]["_repeats"], 3)
        self.assertEqual(merged["summary"]["_spread"]["avg_speed_mps"], [9.506, 13.409])
        self.assertEqual(merged["summary"]["_spread"]["steer_flip_rate_hz"], [0.02, 0.022])
        # 三次同值的字段不记进 _spread（避免噪音）
        self.assertNotIn("lane_change_count", merged["summary"]["_spread"])

    def test_merge_repeats_does_not_mask_a_failed_repeat(self):
        merged = merge_repeats([
            {"scenario": "s", "result": "PASS", "failures": [], "warnings": [],
             "summary": {"avg_speed_mps": 12.0}},
            {"scenario": "s", "result": "FAIL", "failures": ["npc teleport"], "warnings": ["w"],
             "summary": {"avg_speed_mps": 0.0}},
        ], 2)
        self.assertEqual(merged["result"], "FAIL")      # 中位不得抹平偶发 FAIL
        self.assertIn("npc teleport", merged["failures"])
        self.assertIn("w", merged["warnings"])

    def test_merge_repeats_single_repeat_is_passthrough(self):
        payload = {"scenario": "s", "result": "PASS", "failures": [],
                   "warnings": [], "summary": {"avg_speed_mps": 1.0}}
        self.assertIs(merge_repeats([payload], 1), payload)

    def test_repeats_flag_records_median_baseline(self):
        """--repeats 3 --update-baseline：每场景跑 3 次，基线写中位。"""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            suite_path = root / "suite.json"
            suite_path.write_text(json.dumps({
                "name": "repeats-test", "default_duration_s": 1,
                "scenarios": [{"file": "scenarios/straight_road.json", "enabled": True}],
            }), encoding="utf-8")

            calls = {"n": 0}

            def fake_run(entry, _duration, _interval, _results_dir):
                calls["n"] += 1
                return {"scenario": "straight_road", "result": "PASS",
                        "failures": [], "warnings": [],
                        "summary": {"avg_speed_mps": 10.0 + calls["n"], "behavior_state": "CRUISE"}}

            argv = [
                "scenario_regression.py", "--suite", str(suite_path),
                "--results-dir", str(root / "results"),
                "--baseline-dir", str(root / "baseline"),
                "--repeats", "3", "--update-baseline", "--no-archive",
            ]
            with patch.object(scenario_regression, "run_scenario", side_effect=fake_run), \
                    patch.object(sys, "argv", argv):
                self.assertEqual(scenario_regression.main(), 0)

            self.assertEqual(calls["n"], 3)
            baseline = json.loads(
                (root / "baseline" / "straight_road.json").read_text(encoding="utf-8")
            )
            self.assertEqual(baseline["result"], "PASS")
            self.assertEqual(baseline["summary"]["avg_speed_mps"], 12.0)   # median(11,12,13)
            self.assertEqual(baseline["summary"]["_repeats"], 3)
            self.assertEqual(baseline["summary"]["_spread"]["avg_speed_mps"], [11.0, 13.0])

