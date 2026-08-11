import json
import tempfile
import unittest
from pathlib import Path

from tools.scenarioctl import validate_scenario, validate_suite


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
