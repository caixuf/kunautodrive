import json
import tempfile
import unittest
from pathlib import Path

from tools.ai_framework import (
    KinematicWorldModel,
    ReferenceVLA,
    VLAInput,
    WorldModelAction,
    WorldModelState,
    rollout_evaluation,
    rollout_reference,
)


class AIFrameworkTest(unittest.TestCase):
    def test_reference_vla_stops_for_red_light(self):
        output = ReferenceVLA().predict(
            VLAInput(0.0, "stop at the light", {"x": 0.0, "speed": 8.0},
                     scene_context={"speed_limit": 20.0,
                                    "traffic_light": {"state": "red"}})
        )
        self.assertEqual(output.maneuver, "stop")
        self.assertEqual(output.target_speed_mps, 0.0)

    def test_kinematic_world_model_advances_and_preserves_inputs(self):
        state = WorldModelState(0.0, {"x": 0.0, "y": 0.0, "speed": 2.0, "heading": 0.0})
        output = KinematicWorldModel().predict(
            state, WorldModelAction(throttle=1.0), 0.1
        )
        self.assertGreater(output.next_state.ego["x"], 0.0)
        self.assertEqual(state.ego["x"], 0.0)
        self.assertEqual(output.next_state.timestamp_s, 0.1)

    def test_rollout_artifact_is_machine_readable(self):
        scenario = {"ego": {"x": 0.0, "y": 0.0, "init_speed": 1.0},
                    "actors": [], "road_network": {"edges": [{"speed_limit": 10.0}]}}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "result.json"
            path.write_text(json.dumps(rollout_reference(scenario, 2, 0.1, "go")),
                            encoding="utf-8")
            result = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(result["schema_version"], "flowengine.ai_contracts.v1")
        self.assertEqual(len(result["frames"]), 2)

    def test_evaluator_artifact_can_feed_shadow_rollout(self):
        evaluation = {
            "schema_version": 1,
            "scenario": "straight_road",
            "result": "PASS",
            "run": {"run_id": "eval-001"},
            "samples": [
                {
                    "t_demo": 0.0,
                    "metrics": {
                        "scene": {
                            "ego": {"x": 0.0, "y": 0.0, "speed": 4.0},
                            "obstacles": [],
                            "lane": {"width": 3.5},
                        }
                    },
                },
                {
                    "t_demo": 0.1,
                    "metrics": {
                        "scene": {
                            "ego": {"x": 0.4, "y": 0.0, "speed": 4.0},
                            "obstacles": [],
                            "lane": {"width": 3.5},
                        }
                    },
                },
            ],
        }
        artifact = rollout_evaluation(evaluation, "follow route", max_frames=2)
        self.assertTrue(artifact["advisory_only"])
        self.assertEqual(artifact["source"]["run_id"], "eval-001")
        self.assertEqual(artifact["metrics"]["frame_count"], 2)
        self.assertEqual(len(artifact["frames"]), 2)
