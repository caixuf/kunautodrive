#!/usr/bin/env python3
"""Scenario Fuzzer: Lightweight, deterministic, and constrained scenario perturbation tool.

This tool performs parametric physical perturbations on existing FlowEngine scenario JSONs:
- Environmental visibility (visibility_m: [80, 150, 300, 800, 1200])
- Weather conditions (weather: ["clear", "rain", "fog"])
- Ego vehicle dynamics (init_speed, target_speed: +/- 15%)
- NPC actor velocities (vx, vy: +/- 15%) and cut-in/cut-out timing
- Dynamic traffic flow (Poisson rate: 0.12-0.35 veh/s, max vehicles: 12-28)
- Deterministic byte-level reproducibility via explicit seed (fuzz_seed)
- Automated verification via scenarioctl / scenario_loader constraints
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import random
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.scenarioctl import (  # noqa: E402
    SCENARIO_DIR,
    fingerprint,
    load_json,
    resolve_scenario,
    validate_scenario,
)

# Physical perturbation candidate sets and boundaries
ALLOWED_VISIBILITY_M: Tuple[int, ...] = (80, 150, 300, 800, 1200)
ALLOWED_WEATHER: Tuple[str, ...] = ("clear", "rain", "fog")
SPEED_PERTURBATION_RATIO = 0.15  # +/- 15%
SCENARIO_MAX_ACTORS = 64


@dataclass(frozen=True)
class FuzzOptions:
    """Configuration options for scenario fuzzing."""
    seed: int = 42
    speed_scale_range: Tuple[float, float] = (1.0 - SPEED_PERTURBATION_RATIO, 1.0 + SPEED_PERTURBATION_RATIO)
    visibility_options: Tuple[int, ...] = ALLOWED_VISIBILITY_M
    weather_options: Tuple[str, ...] = ALLOWED_WEATHER
    enable_poisson: Optional[bool] = None
    poisson_rate_range: Tuple[float, float] = (0.12, 0.35)
    poisson_vehicles_range: Tuple[int, int] = (12, 28)
    jitter_timing: bool = True


def get_enabled_suite_scenarios(suite_path: Path = SCENARIO_DIR / "suite.json") -> List[Path]:
    """Retrieve the list of enabled scenarios from the suite file."""
    data = load_json(suite_path)
    if not isinstance(data, dict):
        raise ValueError(f"{suite_path}: suite root must be an object")
    entries = data.get("scenarios", [])
    if not isinstance(entries, list):
        raise ValueError(f"{suite_path}: scenarios must be a list")
    enabled_paths: List[Path] = []
    for entry in entries:
        if isinstance(entry, dict) and entry.get("enabled", True):
            fpath = ROOT / entry["file"]
            if fpath.is_file():
                enabled_paths.append(fpath)
            else:
                raise FileNotFoundError(f"Scenario not found: {fpath}")
    return enabled_paths


def _clamp(val: float, min_val: float, max_val: float) -> float:
    return max(min_val, min(max_val, val))


def fuzz_scenario(
    source_data: Dict[str, Any],
    fuzz_seed: int,
    options: Optional[FuzzOptions] = None,
) -> Dict[str, Any]:
    """Apply constrained physical perturbations to a scenario dictionary deterministically."""
    if options is None:
        options = FuzzOptions(seed=fuzz_seed)

    # Initialize PRNG with the exact fuzz_seed to guarantee byte-level reproducibility
    rng = random.Random(fuzz_seed)

    scenario = copy.deepcopy(source_data)
    orig_name = str(scenario.get("name", "scenario"))
    scenario["name"] = f"{orig_name}_fuzz_{fuzz_seed:06d}"
    scenario["fuzz_seed"] = fuzz_seed
    base_seed = int(scenario.get("random_seed", 42))
    # Guarantee integer within positive 31-bit integer range
    scenario["random_seed"] = (abs(base_seed) + fuzz_seed * 1013) % 2147483647

    # 1. Environmental perturbation: visibility and weather
    scenario["visibility_m"] = rng.choice(options.visibility_options)
    scenario["weather"] = rng.choice(options.weather_options)
    if scenario["weather"] == "clear":
        scenario["lighting"] = rng.choice(["day", "dusk"])
    else:
        # Rain or fog typically reduces ambient daylight
        scenario["lighting"] = rng.choice(["day", "dusk", "night"])

    # 2. Ego vehicle initial and target speeds: +/- 15%
    ego = scenario.get("ego")
    if isinstance(ego, dict):
        if isinstance(ego.get("init_speed"), (int, float)):
            scale_init = rng.uniform(options.speed_scale_range[0], options.speed_scale_range[1])
            ego["init_speed"] = round(max(0.5, ego["init_speed"] * scale_init), 2)
        if isinstance(ego.get("target_speed"), (int, float)):
            scale_tgt = rng.uniform(options.speed_scale_range[0], options.speed_scale_range[1])
            new_target = round(max(1.0, ego["target_speed"] * scale_tgt), 2)
            # Ensure target_speed >= init_speed for forward cruise consistency
            if "init_speed" in ego and isinstance(ego["init_speed"], (int, float)):
                new_target = max(new_target, ego["init_speed"])
            ego["target_speed"] = new_target

    # 3. NPC velocities (vx, vy) and cut-in / timing perturbation
    actors = scenario.get("actors")
    if isinstance(actors, list):
        for actor in actors:
            if not isinstance(actor, dict):
                continue
            # Perturb longitudinal speed vx
            if isinstance(actor.get("vx"), (int, float)) and abs(actor["vx"]) > 0.05:
                scale_vx = rng.uniform(options.speed_scale_range[0], options.speed_scale_range[1])
                actor["vx"] = round(actor["vx"] * scale_vx, 2)
            # Perturb lateral speed vy (crucial for cut-in/cut-out scenarios)
            if isinstance(actor.get("vy"), (int, float)) and abs(actor["vy"]) > 0.05:
                scale_vy = rng.uniform(options.speed_scale_range[0], options.speed_scale_range[1])
                actor["vy"] = round(actor["vy"] * scale_vy, 2)
            # General speed field if present
            if isinstance(actor.get("speed"), (int, float)) and actor["speed"] > 0.05:
                scale_spd = rng.uniform(options.speed_scale_range[0], options.speed_scale_range[1])
                actor["speed"] = round(max(0.5, actor["speed"] * scale_spd), 2)

            # Spatial jitter on longitudinal position x/s (alters cut-in interaction timing)
            if options.jitter_timing:
                if isinstance(actor.get("x"), (int, float)):
                    # Perturb x by small jitter [-2.5m, +2.5m], keep safe from spawning on top of ego
                    x_shift = rng.uniform(-2.5, 2.5)
                    # Don't place actor directly on top of ego (assumed around x=5~20)
                    if ego and isinstance(ego.get("x"), (int, float)):
                        ego_x = ego["x"]
                        cand_x = actor["x"] + x_shift
                        if abs(cand_x - ego_x) < 8.0:
                            x_shift = 8.5 if cand_x >= ego_x else -8.5
                    actor["x"] = round(actor["x"] + x_shift, 2)
                elif isinstance(actor.get("s"), (int, float)):
                    s_shift = rng.uniform(-2.5, 2.5)
                    actor["s"] = round(max(0.0, actor["s"] + s_shift), 2)

    # 4. Choreography beats (timing & actuation)
    choreo = scenario.get("choreography")
    if isinstance(choreo, dict) and options.jitter_timing:
        beats = choreo.get("beats")
        if isinstance(beats, list):
            for beat in beats:
                if isinstance(beat, dict) and isinstance(beat.get("t"), (int, float)):
                    # Shift beat timing by +/- 1.0s, keep > 0.5s
                    t_jitter = rng.uniform(-1.0, 1.0)
                    beat["t"] = round(max(0.5, beat["t"] + t_jitter), 2)
                if isinstance(beat, dict) and isinstance(beat.get("vx"), (int, float)) and beat["vx"] > 0.05:
                    scale_bvx = rng.uniform(options.speed_scale_range[0], options.speed_scale_range[1])
                    beat["vx"] = round(max(0.0, beat["vx"] * scale_bvx), 2)

    # 5. ODD exit and driver takeover latency perturbations
    odd = scenario.get("odd")
    if isinstance(odd, dict) and options.jitter_timing:
        if isinstance(odd.get("visibility_drop_at_s"), (int, float)):
            odd["visibility_drop_at_s"] = round(max(1.0, odd["visibility_drop_at_s"] + rng.uniform(-0.8, 0.8)), 2)
        if isinstance(odd.get("degraded_visibility_m"), (int, float)):
            # Drop visibility must remain below typical odd threshold (150m)
            odd["degraded_visibility_m"] = round(_clamp(odd["degraded_visibility_m"] + rng.uniform(-15.0, 15.0), 40.0, 120.0), 1)

    driver = scenario.get("driver")
    if isinstance(driver, dict) and options.jitter_timing:
        if isinstance(driver.get("takeover_latency_s"), (int, float)):
            # Jitter latency slightly [-0.3s, +0.5s]
            driver["takeover_latency_s"] = round(max(0.4, driver["takeover_latency_s"] + rng.uniform(-0.3, 0.5)), 2)

    # 6. Fault injection timing perturbation (e.g. "sensor/lidar:8000")
    if isinstance(scenario.get("fault_injection"), str) and options.jitter_timing:
        parts = scenario["fault_injection"].split(":")
        if len(parts) >= 2 and parts[1].isdigit():
            base_ms = int(parts[1])
            perturbed_ms = max(1000, base_ms + rng.randint(-1200, 1200))
            parts[1] = str(perturbed_ms)
            scenario["fault_injection"] = ":".join(parts)

    # 7. Poisson dynamic traffic flow rate & max vehicles
    has_poisson = "enable_poisson_traffic" in scenario or options.enable_poisson is True
    if has_poisson:
        scenario["enable_poisson_traffic"] = True
        scenario["poisson_rate"] = round(rng.uniform(options.poisson_rate_range[0], options.poisson_rate_range[1]), 3)
        scenario["poisson_max_vehicles"] = rng.randint(options.poisson_vehicles_range[0], options.poisson_vehicles_range[1])

    # Record fuzzing provenance metadata
    scenario["fuzz_metadata"] = {
        "source_template": orig_name,
        "fuzz_seed": fuzz_seed,
        "visibility_m": scenario["visibility_m"],
        "weather": scenario["weather"],
        "has_poisson": bool(scenario.get("enable_poisson_traffic", False)),
    }

    return scenario


def serialize_scenario(data: Dict[str, Any]) -> str:
    """Format scenario JSON with deterministic indentation and trailing newline."""
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def generate_fuzzed_scenarios(
    template_path: Path,
    output_dir: Path,
    count: int = 1,
    base_seed: int = 42,
    options: Optional[FuzzOptions] = None,
    write_files: bool = True,
) -> List[Tuple[Path, Dict[str, Any], str]]:
    """Generate deterministic fuzzed variants of a scenario.

    Returns a list of tuples: (output_path, fuzzed_dict, sha256_hex)
    """
    raw_template = load_json(template_path)
    if not isinstance(raw_template, dict):
        raise ValueError(f"{template_path}: scenario root must be a JSON object")

    if count <= 0:
        raise ValueError("count must be a positive integer")

    if write_files:
        output_dir.mkdir(parents=True, exist_ok=True)

    results: List[Tuple[Path, Dict[str, Any], str]] = []
    stem = template_path.stem

    for i in range(count):
        variant_seed = base_seed + i * 37
        fuzzed_dict = fuzz_scenario(raw_template, fuzz_seed=variant_seed, options=options)
        serialized = serialize_scenario(fuzzed_dict)
        sha256_hex = hashlib.sha256(serialized.encode("utf-8")).hexdigest()
        out_filename = f"{stem}_fuzz_{variant_seed:06d}.json"
        out_path = output_dir / out_filename

        if write_files:
            out_path.write_text(serialized, encoding="utf-8")

        results.append((out_path, fuzzed_dict, sha256_hex))

    return results


def validate_scenario_strict(scenario: Dict[str, Any], path: Path) -> List[str]:
    """Perform strict structural and physical bounds validation on a scenario."""
    errors = validate_scenario(scenario, path)

    # Extra physical bounds checks
    vis = scenario.get("visibility_m")
    if vis is not None:
        if not isinstance(vis, (int, float)) or vis < 10.0 or vis > 5000.0:
            errors.append(f"{path}: visibility_m {vis} outside allowable bounds [10, 5000]")

    wtr = scenario.get("weather")
    if wtr is not None:
        if wtr not in ("clear", "rain", "fog", "snow", "overcast"):
            errors.append(f"{path}: weather '{wtr}' not in supported weather whitelist")

    actors = scenario.get("actors", [])
    if isinstance(actors, list) and len(actors) > SCENARIO_MAX_ACTORS:
        errors.append(f"{path}: actors count {len(actors)} exceeds SCENARIO_MAX_ACTORS ({SCENARIO_MAX_ACTORS})")

    ego = scenario.get("ego", {})
    if isinstance(ego, dict):
        init_spd = ego.get("init_speed")
        if isinstance(init_spd, (int, float)) and init_spd < 0.0:
            errors.append(f"{path}: ego.init_speed {init_spd} must be non-negative")

    return errors


def validate_suite_scenarios() -> int:
    """Validate all 16 enabled scenarios in suite.json."""
    enabled_paths = get_enabled_suite_scenarios()
    print(f"=== Validating {len(enabled_paths)} Enabled Scenarios in suite.json ===")
    all_ok = True
    for path in enabled_paths:
        data = load_json(path)
        sha = fingerprint(path)
        errors = validate_scenario_strict(data, path)
        if errors:
            all_ok = False
            print(f"FAIL {path.name} sha256={sha}")
            for err in errors:
                print(f"  - {err}")
        else:
            print(f"PASS {path.name:<32s} sha256={sha}")

    if all_ok:
        print(f"\nAll {len(enabled_paths)} enabled scenarios validated successfully.")
        return 0
    else:
        print(f"\nValidation failed for one or more scenarios.")
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", type=str, default=None,
                        help="Path or name of target scenario template")
    parser.add_argument("--all", action="store_true",
                        help="Fuzz all 16 enabled scenarios from suite.json")
    parser.add_argument("--validate", action="store_true",
                        help="Validate the 16 enabled scenarios and exit")
    parser.add_argument("--dry-run", action="store_true",
                        help="Generate and validate scenarios in-memory without writing to disk")
    parser.add_argument("--count", type=int, default=1,
                        help="Number of fuzzed variants per scenario (default: 1)")
    parser.add_argument("--seed", type=int, default=42,
                        help="Fuzz seed for deterministic reproducibility (default: 42)")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "output" / "fuzzed_scenarios",
                        help="Directory to save generated fuzzed scenarios")
    parser.add_argument("--enable-poisson", action="store_true",
                        help="Inject dynamic Poisson traffic flow into generated scenarios")

    args = parser.parse_args()

    if args.validate:
        return validate_suite_scenarios()

    options = FuzzOptions(
        seed=args.seed,
        enable_poisson=True if args.enable_poisson else None,
    )

    templates: List[Path] = []
    if args.all:
        templates = get_enabled_suite_scenarios()
    elif args.scenario:
        templates = [resolve_scenario(args.scenario)]
    else:
        # If neither --all nor --scenario is passed, default to all 16 enabled scenarios
        templates = get_enabled_suite_scenarios()

    output_dir = args.output_dir if args.output_dir.is_absolute() else ROOT / args.output_dir
    total_generated = 0
    has_errors = False

    print(f"Fuzzing {len(templates)} scenarios (count={args.count}, seed={args.seed}, dry_run={args.dry_run})...")

    for tmpl in templates:
        generated = generate_fuzzed_scenarios(
            template_path=tmpl,
            output_dir=output_dir,
            count=args.count,
            base_seed=args.seed,
            options=options,
            write_files=not args.dry_run,
        )
        for out_path, sc_dict, sha in generated:
            total_generated += 1
            errs = validate_scenario_strict(sc_dict, out_path)
            status = "PASS" if not errs else "FAIL"
            action_tag = "[dry-run]" if args.dry_run else "[written]"
            print(f"{status} {action_tag} {out_path.name} (template={tmpl.name}, sha256={sha[:12]}...)")
            if errs:
                has_errors = True
                for e in errs:
                    print(f"    - {e}")

    print(f"\nDone: {total_generated} scenario variants processed. Validation: {'PASSED' if not has_errors else 'FAILED'}")
    return 1 if has_errors else 0


if __name__ == "__main__":
    sys.exit(main())
