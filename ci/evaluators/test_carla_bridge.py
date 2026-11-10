import io
import json
import math
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


class _FakeLocation:
    def __init__(self, x: float, y: float, z: float):
        self.x = x
        self.y = y
        self.z = z


class _FakeRotation:
    def __init__(self, pitch: float, yaw: float, roll: float):
        self.pitch = pitch
        self.yaw = yaw
        self.roll = roll


class _FakeVector:
    def __init__(self, x: float, y: float, z: float):
        self.x = x
        self.y = y
        self.z = z


class _FakeTransform:
    def __init__(self, location: _FakeLocation, rotation: _FakeRotation):
        self.location = location
        self.rotation = rotation


class _FakeActor:
    def __init__(self, x=10.0, y=20.0, z=5.0, pitch=3.0, yaw=90.0, roll=1.5,
                 vx=15.0, vy=5.0, vz=0.0, ax=1.0, ay=-2.0, az=0.0):
        self._transform = _FakeTransform(
            _FakeLocation(x, y, z),
            _FakeRotation(pitch, yaw, roll),
        )
        self._vel = _FakeVector(vx, vy, vz)
        self._acc = _FakeVector(ax, ay, az)

    def get_transform(self):
        return self._transform

    def get_velocity(self):
        return self._vel

    def get_acceleration(self):
        return self._acc

    def apply_control(self, ctrl):
        pass


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

    def test_capabilities_6dof_included_in_manifest(self):
        manifest = carla_bridge.capability_manifest()
        for backend in manifest["backends"]:
            caps = backend.get("capabilities_6dof")
            self.assertIsNotNone(caps, f"{backend['backend']} missing capabilities_6dof")
            self.assertTrue(caps.get("sprung_suspension_pitch_roll"))
            self.assertTrue(caps.get("friction_zones"))
            self.assertTrue(caps.get("open_space_reverse_gear"))
            self.assertTrue(caps.get("lidar_3d_obb"))

    def test_extract_ego_pose_6dof_coordinate_transform_and_friction_zones(self):
        actor = _FakeActor(
            x=10.0, y=20.0, z=5.0,
            pitch=3.0, yaw=90.0, roll=1.5,
            vx=15.0, vy=5.0, vz=0.0,
            ax=1.0, ay=-2.0, az=0.0,
        )

        # 1. 基础左手系 → 右手 ENU 系几何转换验证
        pose = carla_bridge.extract_ego_pose_6dof(actor)
        self.assertAlmostEqual(pose.x, 10.0)
        self.assertAlmostEqual(pose.y, -20.0)  # y 取反
        self.assertAlmostEqual(pose.z, 5.0)
        self.assertAlmostEqual(pose.heading, math.radians(-90.0))  # yaw 取反转弧度
        self.assertAlmostEqual(pose.pitch, math.radians(-3.0))     # pitch 取反转弧度
        self.assertAlmostEqual(pose.roll, math.radians(1.5))       # roll 转弧度
        self.assertAlmostEqual(pose.vx, 15.0)
        self.assertAlmostEqual(pose.vy, -5.0)  # vy 取反
        self.assertAlmostEqual(pose.vz, 0.0)
        self.assertAlmostEqual(pose.ax, 1.0)
        self.assertAlmostEqual(pose.ay, 2.0)   # ay 取反
        self.assertAlmostEqual(pose.az, 0.0)
        self.assertAlmostEqual(pose.mu_scale, 1.0)
        self.assertAlmostEqual(pose.road_grade, math.tan(pose.pitch))

        # 2. 摩擦区命中与降级测试
        friction_zones = [
            {
                "x_min": 0.0,
                "x_max": 50.0,
                "y_min": -30.0,
                "y_max": 0.0,
                "mu_scale": 0.35,
                "surface": "wet",
            }
        ]
        # (10.0, -20.0) 落在摩擦区内
        pose_wet = carla_bridge.extract_ego_pose_6dof(actor, friction_zones)
        self.assertAlmostEqual(pose_wet.mu_scale, 0.35)

        # 未命中摩擦区
        friction_zones_miss = [
            {
                "x_min": 50.0,
                "x_max": 100.0,
                "y_min": -10.0,
                "y_max": 10.0,
                "mu_scale": 0.2,
            }
        ]
        pose_dry = carla_bridge.extract_ego_pose_6dof(actor, friction_zones_miss)
        self.assertAlmostEqual(pose_dry.mu_scale, 1.0)

        # 多区域重叠时选取最小 mu_scale
        multi_friction_zones = [
            {
                "x_min": 0.0,
                "x_max": 50.0,
                "y_min": -30.0,
                "y_max": 0.0,
                "mu_scale": 0.35,
            },
            {
                "x_min": 5.0,
                "x_max": 25.0,
                "y_min": -25.0,
                "y_max": -15.0,
                "mu_scale": 0.20,
            },
        ]
        pose_multi = carla_bridge.extract_ego_pose_6dof(actor, multi_friction_zones)
        self.assertAlmostEqual(pose_multi.mu_scale, 0.20)

        # 3. to_flowsim_ego_dict 字典契约验证
        ego_dict = carla_bridge.to_flowsim_ego_dict(pose_wet)
        required_keys = {
            "x", "y", "z", "heading", "pitch", "roll",
            "vx", "vy", "vz", "ax", "ay", "az",
            "speed", "mu_scale", "road_grade"
        }
        self.assertTrue(required_keys.issubset(ego_dict.keys()))
        self.assertAlmostEqual(ego_dict["speed"], math.hypot(15.0, -5.0))
        self.assertAlmostEqual(ego_dict["mu_scale"], 0.35)
        self.assertAlmostEqual(ego_dict["road_grade"], math.tan(pose_wet.pitch))

    def test_carla_bridge_read_ego_pose_6dof(self):
        bridge = carla_bridge.CarlaBridge()
        # 未绑定 ego 时抛出 RuntimeError
        with self.assertRaises(RuntimeError):
            bridge.read_ego_pose_6dof()

        # 绑定 ego 后正常读取
        actor = _FakeActor(x=5.0, y=10.0)
        bridge._ego = actor
        pose = bridge.read_ego_pose_6dof()
        self.assertAlmostEqual(pose.x, 5.0)
        self.assertAlmostEqual(pose.y, -10.0)

        # 传递 friction_zones 读取与最小 mu_scale 匹配
        fz = [{"x_min": 0.0, "x_max": 10.0, "y_min": -15.0, "y_max": 0.0, "mu_scale": 0.25}]
        pose_fz = bridge.read_ego_pose_6dof(fz)
        self.assertAlmostEqual(pose_fz.mu_scale, 0.25)

