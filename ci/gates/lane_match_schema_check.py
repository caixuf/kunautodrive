#!/usr/bin/env python3
"""Validate the cJSON payload of `localization/lane_match`.

Locks the topic to the schema declared in
docs/REQ_L3_DIR2_HDMAP.md §6.1 (v1.0). The check runs offline against
fixture files (no broker) so it is CI-portable and side-effect-free.

M2 schema (5 fields appended on top of any pre-existing payload):
  llt_id              int   (0 = unmatched)
  llt_s               float (meters along centerline)
  llt_offset          float (meters, left positive)
  llt_heading_err_rad float (radians)
  valid               int   (0 = matching degraded)

Rules:
  * All 5 keys must be present.
  * `valid=0` allows `llt_id == 0` (degraded; nothing else is enforced).
  * `valid=1` requires `llt_id > 0` (a real match).
  * Extra keys are accepted (M3 will add 5 more; forward-compat).
  * Missing key or wrong type -> FAIL with a message naming the offender.

Usage:
  python3 ci/gates/lane_match_schema_check.py <fixture.json> [<fixture2.json> ...]
  python3 ci/gates/lane_match_schema_check.py --self-test
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REQUIRED_FIELDS: tuple[tuple[str, type], ...] = (
    ("llt_id", int),
    ("llt_s", float),
    ("llt_offset", float),
    ("llt_heading_err_rad", float),
    ("valid", int),
)

# bool is a subclass of int in Python; reject True/False masquerading as int.
DISALLOWED_TYPES = (bool,)


def _check_payload(payload: object, source: str) -> list[str]:
    errors: list[str] = []
    if not isinstance(payload, dict):
        errors.append(f"{source}: payload is not a JSON object (got {type(payload).__name__})")
        return errors

    for key, expected in REQUIRED_FIELDS:
        if key not in payload:
            errors.append(f"{source}: missing required field {key!r}")
            continue
        value = payload[key]
        if isinstance(value, DISALLOWED_TYPES):
            errors.append(f"{source}: field {key!r} is bool (got {value!r}), expected {expected.__name__}")
            continue
        if expected is int and not isinstance(value, int):
            errors.append(f"{source}: field {key!r} is {type(value).__name__} ({value!r}), expected int")
        elif expected is float and not isinstance(value, (int, float)):
            errors.append(f"{source}: field {key!r} is {type(value).__name__} ({value!r}), expected number")

    if not errors:
        valid = payload.get("valid")
        llt_id = payload.get("llt_id")
        if valid == 1 and isinstance(llt_id, int) and llt_id <= 0:
            errors.append(f"{source}: valid=1 requires llt_id > 0 (got {llt_id})")
    return errors


def _run_self_test() -> int:
    cases: list[tuple[str, dict, bool]] = [
        ("valid_payload", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
        }, True),
        ("valid_payload_degraded", {
            "id_mismatch_frames": 5, "frames": 50,
            "llt_id": 0, "llt_s": 0.0, "llt_offset": 0.0,
            "llt_heading_err_rad": 0.0, "valid": 0,
        }, True),
        ("missing_field", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02,
        }, False),
        ("valid_one_with_zero_llt_id", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 0, "llt_s": 0.0, "llt_offset": 0.0,
            "llt_heading_err_rad": 0.0, "valid": 1,
        }, False),
        ("llt_offset_as_string", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": "bad",
            "llt_heading_err_rad": 0.02, "valid": 1,
        }, False),
        ("valid_as_bool_true", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": True,
        }, False),
        ("forward_compat_extra_fields", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
            "curvature": 0.001, "lane_width": 3.5,
            "left_lanelet_id": 41, "right_lanelet_id": 43, "flags": 0,
        }, True),
    ]
    failures = 0
    for name, payload, expect_pass in cases:
        errs = _check_payload(payload, name)
        passed = (len(errs) == 0) == expect_pass
        marker = "PASS" if passed else "FAIL"
        if not passed:
            failures += 1
        print(f"  [{marker}] {name} (expect_pass={expect_pass})")
        for e in errs:
            print(f"          {e}")
    return 0 if failures == 0 else 1


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("fixtures", nargs="*", type=Path, help="JSON fixtures to validate")
    p.add_argument("--self-test", action="store_true", help="run built-in smoke tests")
    args = p.parse_args(argv)

    if args.self_test:
        print("lane_match_schema_check self-test:")
        return _run_self_test()

    if not args.fixtures:
        p.error("at least one fixture path or --self-test required")

    total_errors = 0
    for f in args.fixtures:
        try:
            payload = json.loads(f.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            print(f"{f}: cannot read JSON: {e}", file=sys.stderr)
            total_errors += 1
            continue
        errs = _check_payload(payload, str(f))
        if errs:
            total_errors += 1
            for e in errs:
                print(e, file=sys.stderr)
        else:
            print(f"OK  {f}")

    if total_errors:
        print(f"\n{total_errors} fixture(s) failed", file=sys.stderr)
    else:
        print(f"\nall {len(args.fixtures)} fixture(s) passed")
    return 0 if total_errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
