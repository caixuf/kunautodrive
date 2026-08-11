import json
import tempfile
import unittest
from pathlib import Path

from ci.evaluators.scenario_regression import (
    archive_failed_result,
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

    def test_sha256_is_stable(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "data"
            path.write_bytes(b"kunautodrive")
            self.assertEqual(
                sha256_file(path),
                "0e6ba1b786d8c0290d2e320355f0d0205297a8efd2393422d97cee7b8e7de280",
            )
