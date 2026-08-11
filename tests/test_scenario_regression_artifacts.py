import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ci.evaluators import scenario_regression
from ci.evaluators.scenario_regression import (
    archive_failed_result,
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
