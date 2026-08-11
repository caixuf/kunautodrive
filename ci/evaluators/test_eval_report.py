import unittest

from tools.eval_report import (
    build_report,
    compute_open_loop_metrics,
    required_metric_failures,
    trajectory_error,
)


class EvaluationReportTest(unittest.TestCase):
    def test_standard_ade_and_fde(self):
        ade, fde = trajectory_error(
            [{"x": 0, "y": 0}, {"x": 2, "y": 0}],
            [{"x": 0, "y": 1}, {"x": 1, "y": 1}],
        )
        self.assertAlmostEqual(ade, 1.20710678, places=6)
        self.assertAlmostEqual(fde, 1.41421356, places=6)

    def test_missing_ground_truth_is_unavailable_not_zero(self):
        result = compute_open_loop_metrics([{"prediction": [[0, 0], [1, 0]]}])
        self.assertEqual(result["status"], "invalid")
        self.assertIsNone(result["ade_m"])

    def test_report_aggregates_pass_rate_and_producer_defined_mpi(self):
        report = build_report(
            [
                {"scenario": "a", "result": "PASS",
                 "metrics": {"mpi": 0.8, "mpi_definition": "intervention_rate"}},
                {"scenario": "b", "result": "FAIL", "metrics": {}},
            ],
            {
                "predictions": [{
                    "scenario_id": "a",
                    "prediction": [[0, 0], [2, 0]],
                    "ground_truth": [[0, 1], [1, 1]],
                }],
            },
        )
        self.assertEqual(report["scenario_pass_rate"], 0.5)
        self.assertEqual(report["open_loop"]["status"], "computed")
        self.assertAlmostEqual(report["open_loop"]["fde_m"], 2 ** 0.5)
        self.assertEqual(report["mpi"]["status"], "computed")
        self.assertEqual(report["mpi"]["definition"], "intervention_rate")

    def test_report_does_not_fabricate_unavailable_metrics(self):
        report = build_report([{"scenario": "a", "result": "PASS", "metrics": {}}])
        self.assertEqual(report["open_loop"]["status"], "unavailable")
        self.assertEqual(report["mpi"]["status"], "unavailable")
        self.assertIsNone(report["open_loop"]["ade_m"])

    def test_required_metric_gate_rejects_unavailable_metrics(self):
        report = build_report([{"scenario": "a", "result": "PASS", "metrics": {}}])
        failures = required_metric_failures(
            report, require_open_loop=True, require_mpi=True
        )
        self.assertEqual(len(failures), 2)

    def test_required_metric_gate_accepts_computed_metrics(self):
        report = build_report(
            [{
                "scenario": "a",
                "result": "PASS",
                "metrics": {"mpi": 0.2, "mpi_definition": "intervention_rate"},
            }],
            {
                "predictions": [{
                    "prediction": [[0, 0], [1, 0]],
                    "ground_truth": [[0, 0], [1, 0]],
                }],
            },
        )
        self.assertEqual(required_metric_failures(
            report, require_open_loop=True, require_mpi=True
        ), [])
