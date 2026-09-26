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

M3 schema (5 additional fields appended on top of M2):
  curvature           double (1/m, 0 = straight, + = left curve)
  lane_width          double (m, 0 = unknown)
  left_lanelet_id     uint64 (0 = no left neighbor)
  right_lanelet_id    uint64 (0 = no right neighbor)
  flags               uint32 (ABI reserved, set 0 in current producers)

Field type model: each REQUIRED_FIELDS entry is `(name, kind)` where
`kind` is one of:
  "int"    - Python int only (rejects bool, float, str, etc.)
  "number" - Python int or float (rejects bool, str, etc.)
The reason for "int" being strict: cJSON distinguishes integer-valued
numbers from float-valued numbers, and `left_lanelet_id` etc. are
declared uint64 in spec §6.1 v1.0. Feeding a JSON `42.0` for a lanelet
id is a producer bug we want to surface, not silently coerce.

Rules:
  * In default mode the M2 5 keys must be present; M3 5 keys are
    optional. When present, M3 keys are still type-checked (so a
    malformed payload is caught even without --strict-m3).
  * --strict-m3 promotes M3 5 keys to required (full 10-field check).
    Default mode keeps M2-only payloads passing for backward compat
    with producers that haven't rolled out M3 yet.
  * valid=0 allows llt_id == 0 (degraded; nothing else is enforced).
  * valid=1 requires llt_id > 0 (a real match).
  * Extra keys (beyond the documented 10) are accepted — forward-compat
    for any future M4+ field without gate churn.
  * Missing key or wrong type -> FAIL with a message naming the offender.

Usage:
  python3 ci/gates/lane_match_schema_check.py [--strict-m3] <fixture.json> [<fixture2.json> ...]
  python3 ci/gates/lane_match_schema_check.py --self-test [--strict-m3]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# (key, kind) tuples in spec §6.1 v1.0 field order:
#   M2 (5) ... M3 (5) appended on top.
#   Field order MUST match spec; do not reorder.
REQUIRED_FIELDS: tuple[tuple[str, str], ...] = (
    ("llt_id",              "int"),       # M2
    ("llt_s",               "number"),
    ("llt_offset",          "number"),
    ("llt_heading_err_rad", "number"),
    ("valid",               "int"),
    ("curvature",           "number"),    # M3 — spec §6.2
    ("lane_width",          "number"),
    ("left_lanelet_id",     "int"),
    ("right_lanelet_id",    "int"),
    ("flags",               "int"),
)

# M3 5 keys used to discriminate "required in strict only" vs
# "required in both" within REQUIRED_FIELDS.
M3_FIELDS: frozenset[str] = frozenset({
    "curvature", "lane_width",
    "left_lanelet_id", "right_lanelet_id", "flags",
})

# bool is a subclass of int in Python; reject True/False masquerading as int.
DISALLOWED_TYPES = (bool,)


def _type_error(source: str, key: str, value: object, kind: str) -> str:
    """Format a single type-mismatch error message for a field."""
    if isinstance(value, DISALLOWED_TYPES):
        # bool is a Python int subclass; surface it explicitly so the
        # producer sees the difference between `valid: True` (bug) and
        # `valid: 1` (intended).
        return f"{source}: field {key!r} is bool (got {value!r}), expected {kind}"
    return (
        f"{source}: field {key!r} is {type(value).__name__} "
        f"({value!r}), expected {kind}"
    )


def _check_payload(
    payload: object,
    source: str,
    *,
    strict_m3: bool = False,
) -> list[str]:
    """Validate a parsed JSON payload against the lane_match schema.

    Returns an empty list on success, or a list of human-readable error
    messages (one per problem) on failure.

    `strict_m3=False` (default): M2 5 fields required, M3 5 fields
    optional but type-checked when present. This is the M2-compatible
    behavior — M3 producers get type validation for free while M2-only
    fixtures keep passing.

    `strict_m3=True`: all 10 fields required. Used by `--strict-m3`
    to enforce M3 producers ship all 5 new keys.
    """
    errors: list[str] = []
    if not isinstance(payload, dict):
        errors.append(
            f"{source}: payload is not a JSON object (got {type(payload).__name__})"
        )
        return errors

    for key, kind in REQUIRED_FIELDS:
        is_m3 = key in M3_FIELDS
        if key not in payload:
            # In default mode, missing M3 fields are allowed (backward
            # compat with M2-only producers). In strict mode they're
            # treated like any other required field.
            if is_m3 and not strict_m3:
                continue
            errors.append(f"{source}: missing required field {key!r}")
            continue
        value = payload[key]
        if isinstance(value, DISALLOWED_TYPES):
            errors.append(_type_error(source, key, value, kind))
            continue
        if kind == "int" and not isinstance(value, int):
            errors.append(_type_error(source, key, value, kind))
        elif kind == "number" and not isinstance(value, (int, float)):
            errors.append(_type_error(source, key, value, kind))

    if not errors:
        valid = payload.get("valid")
        llt_id = payload.get("llt_id")
        if valid == 1 and isinstance(llt_id, int) and llt_id <= 0:
            errors.append(
                f"{source}: valid=1 requires llt_id > 0 (got {llt_id})"
            )
    return errors


# Self-test cases: (name, payload, expected_pass_in_default, expected_pass_in_strict).
# `expected_pass_in_default` = what the gate should return when called
# with strict_m3=False. `expected_pass_in_strict` = same but with
# strict_m3=True. Each case has its own expected outcome per mode;
# the self-test loop verifies both with a single run by selecting the
# right expectation for the active mode.
def _self_test_cases() -> list[tuple[str, dict, bool, bool]]:
    return [
        # --- M2 coverage (original 7 cases) ---
        ("valid_payload", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
        }, True, False),  # default: PASS (M2 ok) ; strict: FAIL (M3 missing)
        ("valid_payload_degraded", {
            "id_mismatch_frames": 5, "frames": 50,
            "llt_id": 0, "llt_s": 0.0, "llt_offset": 0.0,
            "llt_heading_err_rad": 0.0, "valid": 0,
        }, True, False),  # same reasoning as valid_payload
        ("missing_field", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02,  # 'valid' missing
        }, False, False),  # missing valid → errors in BOTH modes
        ("valid_one_with_zero_llt_id", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 0, "llt_s": 0.0, "llt_offset": 0.0,
            "llt_heading_err_rad": 0.0, "valid": 1,
        }, False, False),  # valid=1 + llt_id=0 → semantic contradiction
        ("llt_offset_as_string", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": "bad",
            "llt_heading_err_rad": 0.02, "valid": 1,
        }, False, False),
        ("valid_as_bool_true", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": True,
        }, False, False),
        # This case has M2 + all M3 fields — was the forward-compat
        # canary for M3. Still works in M3 mode (default + strict).
        ("forward_compat_extra_fields", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
            "curvature": 0.001, "lane_width": 3.5,
            "left_lanelet_id": 41, "right_lanelet_id": 43, "flags": 0,
        }, True, True),

        # --- M3 expansion (new cases) ---
        ("m3_valid_payload_10_fields", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
            "curvature": 0.0025, "lane_width": 3.5,
            "left_lanelet_id": 41, "right_lanelet_id": 43, "flags": 0,
        }, True, True),
        ("m3_curvature_as_string", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
            "curvature": "0.001",  # WRONG: str, not number
            "lane_width": 3.5,
            "left_lanelet_id": 41, "right_lanelet_id": 43, "flags": 0,
        }, False, False),
        ("m3_curvature_as_bool", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
            "curvature": False,  # WRONG: bool, not number
            "lane_width": 3.5,
            "left_lanelet_id": 41, "right_lanelet_id": 43, "flags": 0,
        }, False, False),
        ("m3_left_lanelet_id_as_bool", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
            "curvature": 0.001, "lane_width": 3.5,
            "left_lanelet_id": True,  # WRONG: bool, not int
            "right_lanelet_id": 43, "flags": 0,
        }, False, False),
        ("m3_right_lanelet_id_as_float", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
            "curvature": 0.001, "lane_width": 3.5,
            "left_lanelet_id": 41,
            "right_lanelet_id": 43.0,  # WRONG: float, not strict int
            "flags": 0,
        }, False, False),
        ("m3_flags_as_float", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
            "curvature": 0.001, "lane_width": 3.5,
            "left_lanelet_id": 41, "right_lanelet_id": 43,
            "flags": 1.0,  # WRONG: float, not strict int
        }, False, False),
        # Only fails under --strict-m3 (M3 missing required field).
        # In default mode M3 fields are optional → gate PASSes.
        ("m3_missing_left_lanelet_id_strict", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
            "curvature": 0.001, "lane_width": 3.5,
            # left_lanelet_id intentionally missing
            "right_lanelet_id": 43, "flags": 0,
        }, True, False),
        # Truly unknown fields beyond the documented 10 — forward-compat
        # for hypothetical M4+. Should be ignored in default mode. In
        # strict mode it FAILS because M3 fields are missing too (the
        # payload only has M2).
        ("m4_forward_compat_unknown_field", {
            "id_mismatch_frames": 0, "frames": 100,
            "llt_id": 42, "llt_s": 12.5, "llt_offset": -0.1,
            "llt_heading_err_rad": 0.02, "valid": 1,
            "future_m4_field": {"nested": [1, 2, 3]},
            "another_future": "anything",
        }, True, False),
    ]


def _run_self_test(strict_m3: bool = False) -> int:
    cases = _self_test_cases()
    failures = 0
    for name, payload, expect_default, expect_strict in cases:
        expect_pass = expect_strict if strict_m3 else expect_default
        errs = _check_payload(payload, name, strict_m3=strict_m3)
        # A test "PASSes" iff the gate produced exactly what we expected:
        #   no errors when expect_pass=True, some errors when expect_pass=False.
        passed = (len(errs) == 0) == expect_pass
        marker = "PASS" if passed else "FAIL"
        if not passed:
            failures += 1
        print(
            f"  [{marker}] {name} "
            f"(expect={'PASS' if expect_pass else 'FAIL'}, "
            f"strict_m3={strict_m3})"
        )
        for e in errs:
            print(f"          {e}")
    return 0 if failures == 0 else 1


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "fixtures", nargs="*", type=Path,
        help="JSON fixtures to validate",
    )
    p.add_argument(
        "--self-test", action="store_true",
        help="run built-in smoke tests",
    )
    p.add_argument(
        "--strict-m3", action="store_true",
        help=(
            "require M3 5 fields (curvature / lane_width / "
            "left_lanelet_id / right_lanelet_id / flags). "
            "Default lets them be optional for backward compat with "
            "M2-only producers; --strict-m3 enforces the full 10-field "
            "M3 schema."
        ),
    )
    args = p.parse_args(argv)

    if args.self_test:
        print("lane_match_schema_check self-test:")
        return _run_self_test(strict_m3=args.strict_m3)

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
        errs = _check_payload(payload, str(f), strict_m3=args.strict_m3)
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
