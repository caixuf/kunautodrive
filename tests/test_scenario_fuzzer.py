#!/usr/bin/env python3
"""Unit tests for Scenario Fuzzer (tools/scenario_fuzzer.py).

Covers:
1. Deterministic generation & byte-level reproducibility (identical seed -> identical output)
2. Physical bounds and constraints (visibility, weather, ego speeds, NPC velocities, Poisson)
3. Schema validity and scenarioctl / scenario_loader compatibility
4. Extreme boundary constraints & error detection
5. Full 16-scenario suite validation
"""

from __future__ import annotations

import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools.scenarioctl import validate_scenario
from tools.scenario_fuzzer import (
    ALLOWED_VISIBILITY_M,
    ALLOWED_WEATHER,
    FuzzOptions,
    fuzz_scenario,
    generate_fuzzed_scenarios,
    get_enabled_suite_scenarios,
    serialize_scenario,
    validate_scenario_strict,
    validate_suite_scenarios,
    ROOT,
    SCENARIO_DIR,
)


class TestScenarioFuzzer(unittest.TestCase):
    def setUp(self):
        self.straight_path = SCENARIO_DIR / "straight_road.json"
        self.r157_cut_in_path = SCENARIO_DIR / "r157_cut_in.json"
        self.l3_odd_path = SCENARIO_DIR / "l3_odd_exit_takeover.json"
        self.straight_data = json.loads(self.straight_path.read_text(encoding="utf-8"))
        self.r157_cut_in_data = json.loads(self.r157_cut_in_path.read_text(encoding="utf-8"))
        self.l3_odd_data = json.loads(self.l3_odd_path.read_text(encoding="utf-8"))

    def test_deterministic_generation_same_seed(self):
        """Identical seed must yield byte-for-byte identical output."""
        fuzzed_a = fuzz_scenario(self.straight_data, fuzz_seed=12345)
        fuzzed_b = fuzz_scenario(self.straight_data, fuzz_seed=12345)

        bytes_a = serialize_scenario(fuzzed_a).encode("utf-8")
        bytes_b = serialize_scenario(fuzzed_b).encode("utf-8")

        self.assertEqual(bytes_a, bytes_b)
        self.assertEqual(hashlib.sha256(bytes_a).hexdigest(), hashlib.sha256(bytes_b).hexdigest())

    def test_different_seeds_produce_different_perturbations(self):
        """Different seeds must produce different perturbations."""
        fuzzed_1 = fuzz_scenario(self.straight_data, fuzz_seed=101)
        fuzzed_2 = fuzz_scenario(self.straight_data, fuzz_seed=202)

        self.assertNotEqual(fuzzed_1["random_seed"], fuzzed_2["random_seed"])
        self.assertNotEqual(fuzzed_1["fuzz_seed"], fuzzed_2["fuzz_seed"])

    def test_physical_perturbation_bounds(self):
        """Ensure perturbed parameters strictly adhere to physical constraints."""
        seeds = [1, 42, 99, 137, 256, 1024, 9999]
        for seed in seeds:
            fuzzed = fuzz_scenario(self.straight_data, fuzz_seed=seed)

            # Visibility constraint
            self.assertIn(fuzzed["visibility_m"], ALLOWED_VISIBILITY_M)

            # Weather constraint
            self.assertIn(fuzzed["weather"], ALLOWED_WEATHER)

            # Ego speed constraints (+/- 15%)
            orig_init = self.straight_data["ego"]["init_speed"]
            orig_tgt = self.straight_data["ego"]["target_speed"]
            self.assertGreaterEqual(fuzzed["ego"]["init_speed"], orig_init * 0.84)
            self.assertLessEqual(fuzzed["ego"]["init_speed"], orig_init * 1.16)
            self.assertGreaterEqual(fuzzed["ego"]["target_speed"], orig_tgt * 0.84)
            self.assertLessEqual(fuzzed["ego"]["target_speed"], orig_tgt * 1.16)
            self.assertGreaterEqual(fuzzed["ego"]["target_speed"], fuzzed["ego"]["init_speed"])

            # Pass scenarioctl validation
            errs = validate_scenario_strict(fuzzed, self.straight_path)
            self.assertEqual(errs, [])

    def test_npc_velocity_and_cutin_perturbation(self):
        """Verify NPC velocities and cut-in parameters are perturbed properly."""
        orig_actor0_vx = self.r157_cut_in_data["actors"][0]["vx"]
        orig_actor1_vy = self.r157_cut_in_data["actors"][1]["vy"]  # cut-in lateral velocity

        fuzzed = fuzz_scenario(self.r157_cut_in_data, fuzz_seed=777)
        new_actor0_vx = fuzzed["actors"][0]["vx"]
        new_actor1_vy = fuzzed["actors"][1]["vy"]

        # Bounds on perturbed velocities (+/- 15%)
        self.assertGreaterEqual(new_actor0_vx, orig_actor0_vx * 0.84)
        self.assertLessEqual(new_actor0_vx, orig_actor0_vx * 1.16)

        # vy must preserve direction (negative for leftward cut-in) and be scaled within +/- 15%
        self.assertLess(new_actor1_vy, 0.0)
        self.assertGreaterEqual(abs(new_actor1_vy), abs(orig_actor1_vy) * 0.84)
        self.assertLessEqual(abs(new_actor1_vy), abs(orig_actor1_vy) * 1.16)

        errs = validate_scenario_strict(fuzzed, self.r157_cut_in_path)
        self.assertEqual(errs, [])

    def test_odd_and_driver_takeover_perturbation(self):
        """Verify ODD drop and driver takeover latency perturbations in L3 scenarios."""
        fuzzed = fuzz_scenario(self.l3_odd_data, fuzz_seed=888)

        # ODD drop timing and degraded visibility
        self.assertIn("odd", fuzzed)
        self.assertGreater(fuzzed["odd"]["visibility_drop_at_s"], 0.0)
        self.assertLessEqual(fuzzed["odd"]["degraded_visibility_m"], 135.0)

        # Driver takeover latency
        self.assertIn("driver", fuzzed)
        self.assertGreaterEqual(fuzzed["driver"]["takeover_latency_s"], 0.4)

        errs = validate_scenario_strict(fuzzed, self.l3_odd_path)
        self.assertEqual(errs, [])

    def test_poisson_traffic_injection_and_ranges(self):
        """Verify Poisson traffic injection respects bounded rates and quotas."""
        opts = FuzzOptions(enable_poisson=True)
        fuzzed = fuzz_scenario(self.straight_data, fuzz_seed=555, options=opts)

        self.assertTrue(fuzzed.get("enable_poisson_traffic"))
        self.assertGreaterEqual(fuzzed["poisson_rate"], 0.12)
        self.assertLessEqual(fuzzed["poisson_rate"], 0.35)
        self.assertGreaterEqual(fuzzed["poisson_max_vehicles"], 12)
        self.assertLessEqual(fuzzed["poisson_max_vehicles"], 28)

        errs = validate_scenario_strict(fuzzed, self.straight_path)
        self.assertEqual(errs, [])

    def test_strict_validator_catches_invalid_bounds(self):
        """Ensure validate_scenario_strict detects out-of-boundary values."""
        bad_scenario = copy.deepcopy(self.straight_data)

        # 1. Invalid visibility
        bad_scenario["visibility_m"] = 8000.0
        errs = validate_scenario_strict(bad_scenario, Path("bad.json"))
        self.assertTrue(any("visibility_m" in e for e in errs))

        # 2. Unsupported weather
        bad_scenario["visibility_m"] = 800
        bad_scenario["weather"] = "tornado"
        errs = validate_scenario_strict(bad_scenario, Path("bad.json"))
        self.assertTrue(any("weather" in e for e in errs))

        # 3. Negative init_speed
        bad_scenario["weather"] = "rain"
        bad_scenario["ego"]["init_speed"] = -5.0
        errs = validate_scenario_strict(bad_scenario, Path("bad.json"))
        self.assertTrue(any("ego.init_speed" in e for e in errs))

    def test_suite_enabled_scenarios_validation(self):
        """The 16 enabled scenarios in suite.json must all pass strict validation."""
        enabled = get_enabled_suite_scenarios()
        self.assertEqual(len(enabled), 16)
        rc = validate_suite_scenarios()
        self.assertEqual(rc, 0)

    def test_generate_to_disk_and_verify(self):
        """Verify generate_fuzzed_scenarios writes valid files with repeatable hashes."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            results1 = generate_fuzzed_scenarios(
                template_path=self.r157_cut_in_path,
                output_dir=tmp_path,
                count=3,
                base_seed=1000,
                write_files=True,
            )
            self.assertEqual(len(results1), 3)

            # Check that files exist and parse as valid JSON
            for out_file, sc_dict, sha in results1:
                self.assertTrue(out_file.is_file())
                content = out_file.read_text(encoding="utf-8")
                loaded = json.loads(content)
                self.assertEqual(validate_scenario_strict(loaded, out_file), [])

            # Generate again with identical seed into another dir
            with tempfile.TemporaryDirectory() as tmp_dir2:
                results2 = generate_fuzzed_scenarios(
                    template_path=self.r157_cut_in_path,
                    output_dir=Path(tmp_dir2),
                    count=3,
                    base_seed=1000,
                    write_files=True,
                )
                for (p1, d1, sha1), (p2, d2, sha2) in zip(results1, results2):
                    self.assertEqual(sha1, sha2)
                    self.assertEqual(p1.name, p2.name)


if __name__ == "__main__":
    unittest.main()
