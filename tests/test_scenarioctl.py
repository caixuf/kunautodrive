import json
import tempfile
import unittest
from pathlib import Path

from tools.scenarioctl import (
    generate_variants,
    validate_scenario,
    validate_suite,
)


class ScenarioCtlTest(unittest.TestCase):
    def test_repository_scenario_suite_is_valid(self):
        root = Path(__file__).resolve().parents[1]
        self.assertEqual(validate_suite(root / "scenarios" / "suite.json"), [])

    def test_repository_scenario_is_valid(self):
        root = Path(__file__).resolve().parents[1]
        data = json.loads((root / "scenarios" / "straight_road.json").read_text())
        self.assertEqual(validate_scenario(data, root / "scenarios" / "straight_road.json"), [])

    def test_missing_road_edge_field_is_reported(self):
        scenario = {
            "name": "broken",
            "description": "test",
            "random_seed": 1,
            "ego": {"x": 0, "y": 0, "heading": 0, "init_speed": 0},
            "road_network": {"edges": [{"id": 0, "lanes": 2, "nodes": [[0, 0], [1, 0]]}]},
            "actors": [],
            "pass_criteria": {},
        }
        errors = validate_scenario(scenario, Path("broken.json"))
        self.assertTrue(any("lane_width" in error for error in errors))

    def test_generation_is_seeded_and_preserves_scenario_contract(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            paths = generate_variants(
                root / "scenarios" / "straight_road.json",
                Path(tmp), count=2, seed=100, speed_scale=0.5,
                position_jitter_m=2.0,
            )
            first = json.loads(paths[0].read_text())
            second = json.loads(paths[1].read_text())
        self.assertEqual(first["random_seed"], 100)
        self.assertEqual(second["random_seed"], 101)
        self.assertEqual(first["generation"]["seed"], 100)
        self.assertAlmostEqual(
            first["ego"]["target_speed"],
            json.loads((root / "scenarios" / "straight_road.json").read_text())
            ["ego"]["target_speed"] * 0.5,
        )
        self.assertEqual(validate_scenario(first, paths[0]), [])
