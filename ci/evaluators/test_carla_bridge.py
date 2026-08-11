import io
import json
import unittest
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest.mock import patch

from tools import carla_bridge


class _FakeCarla:
    class VehicleControl:
        def __init__(self, **kwargs):
            self.values = kwargs


class _FakeSensor:
    def __init__(self):
        self.callback = None

    def listen(self, callback):
        self.callback = callback


class CarlaBridgeContractTest(unittest.TestCase):
    def test_capability_manifest_is_truthful_and_versioned(self):
        manifest = carla_bridge.capability_manifest()
        self.assertEqual(manifest["schema"], "flowengine.simulator_capabilities.v1")
        backends = {entry["backend"]: entry for entry in manifest["backends"]}
        self.assertIn("flowsim", backends)
        self.assertIn("carla", backends)
        self.assertIn("available", backends["carla"])
        self.assertIn("sensors", backends["carla"])

    def test_control_mapping_clamps_normalized_ranges(self):
        command = carla_bridge.ControlCommand(
            throttle=2.0, brake=-1.0, steer=-2.0, gear=-1,
        )
        mapped = carla_bridge.to_carla_control(command, _FakeCarla)
        self.assertEqual(mapped.values["throttle"], 1.0)
        self.assertEqual(mapped.values["brake"], 0.0)
        self.assertEqual(mapped.values["steer"], -1.0)
        self.assertTrue(mapped.values["reverse"])
        self.assertEqual(mapped.values["gear"], -1)

    def test_sensor_batch_keeps_clock_and_latest_packet(self):
        bridge = carla_bridge.CarlaBridge(queue_depth=2)
        sensor = _FakeSensor()
        bridge.register_sensor(
            "lidar", sensor,
            carla_bridge.SensorSpec("lidar", "ray_cast", "lidar", required=True),
        )
        sensor.callback(SimpleNamespace(frame=7, timestamp=1.25))
        clock = carla_bridge.SimulationClock(frame=7, sim_time_s=1.25, delta_s=0.05)
        batch = bridge.read_sensor_batch(clock)
        self.assertEqual(batch.clock, clock)
        self.assertEqual(batch.packets["lidar"].frame, 7)

    def test_missing_carla_fails_explicitly(self):
        bridge = carla_bridge.CarlaBridge()
        with patch.object(carla_bridge.importlib, "import_module",
                          side_effect=ImportError("missing carla")):
            with self.assertRaises(carla_bridge.CarlaUnavailableError):
                bridge.connect()
        self.assertFalse(bridge.connected)

    def test_check_command_returns_machine_readable_capability(self):
        output = io.StringIO()
        with redirect_stdout(output):
            result = carla_bridge.main(["check", "--json"])
        payload = json.loads(output.getvalue())
        self.assertEqual(payload["schema"], "flowengine.simulator_capabilities.v1")
        self.assertIn(result, (0, 2))

