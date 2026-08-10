#!/usr/bin/env python3

import json
import os
import tempfile
import unittest
from pathlib import Path

import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import vehicle_config  # noqa: E402
import product_config  # noqa: E402


class VehicleConfigTest(unittest.TestCase):
    def setUp(self):
        self.base = vehicle_config.load_json(ROOT / "config/pipeline_car.json")
        self.product = product_config.load(ROOT / "config/product.json")
        os.environ["PRODUCT_HOME"] = "/opt/product"

    def test_profiles_generate_valid_pipelines(self):
        for path in sorted((ROOT / "config/vehicles").glob("*.json")):
            profile = vehicle_config.load_json(path)
            if profile.get("status") != "ready":
                continue
            base = vehicle_config.load_json(
                ROOT / "config" / profile["base_pipeline"])
            pipeline = vehicle_config.build_pipeline(
                base, profile, self.product["plugin_dir"])
            self.assertEqual([], vehicle_config.validate_pipeline(pipeline), path.name)
            self.assertEqual(profile["id"], pipeline["vehicle"]["id"])
            for node in pipeline["processes"]:
                self.assertNotIn("build/lib/", node["library_path"])

    def test_waypoint_path_expands_to_deployment_root(self):
        profile = vehicle_config.load_json(
            ROOT / "config/vehicles/rc_car.json")
        pipeline = vehicle_config.build_pipeline(
            self.base, profile, self.product["plugin_dir"])
        node = next(n for n in pipeline["processes"]
                    if n["name"] == "waypoint_follower")
        params = json.loads(node["params"])
        self.assertEqual("/opt/product/etc/waypoints.json",
                         params["waypoints_file"])

    def test_unknown_node_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unknown node"):
            vehicle_config.build_pipeline(
                self.base, {"nodes": {"missing": {}}},
                self.product["plugin_dir"])

    def test_plugin_directory_is_checked(self):
        pipeline = vehicle_config.build_pipeline(
            self.base, vehicle_config.load_json(
                ROOT / "config/vehicles/rc_car.json"),
            self.product["plugin_dir"])
        with tempfile.TemporaryDirectory() as directory:
            errors = vehicle_config.validate_pipeline(pipeline, Path(directory))
        self.assertTrue(any("plugin not found" in error for error in errors))

    def test_product_metadata_is_single_source(self):
        product = product_config.load(ROOT / "config/product.json")
        pipeline = vehicle_config.build_pipeline(
            self.base,
            vehicle_config.load_json(ROOT / "config/vehicles/rc_car.json"),
            product["plugin_dir"])
        self.assertTrue(all(
            node["library_path"].startswith(product["plugin_dir"] + "/")
            for node in pipeline["processes"]))

    def test_placeholder_profile_is_rejected(self):
        profile = vehicle_config.load_json(
            ROOT / "config/vehicles/passenger_vehicle.placeholder.json")
        with self.assertRaisesRegex(ValueError, "not deployable"):
            vehicle_config.build_pipeline(
                self.base, profile, self.product["plugin_dir"])


if __name__ == "__main__":
    unittest.main()
