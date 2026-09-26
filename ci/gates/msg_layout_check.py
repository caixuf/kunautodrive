#!/usr/bin/env python3
"""Guard against the "wire size vs C struct size" trap in msg_codegen output.

Background
----------
`tools/msg_codegen.py` computes a struct's *wire* size as the plain sum of its
field sizes (`_struct_size`), and emits serialize/deserialize functions that
write fields back-to-back at exactly those offsets.  The generated C struct,
however, has **no packing directive**, so the C compiler inserts natural
alignment padding — and then `sizeof(T) != wire_size(T)`.

Which one is authoritative depends on how a message is sent:

  * `T_serialize()` / `T_deserialize()`  → wire layout  (packed, declared size)
  * `msg_cast(T)` / `msg_init_typed(&v, sizeof(v))` → memory layout (padded)

`_msg_cast_impl()` uses the legacy branch (`data_size == expected_size`) with
`expected_size = sizeof(T)`, so a producer/consumer pair must agree on which
convention they use.  Mixing them fails *silently*: fields land on the wrong
offsets, no error is logged.  (2026-09-21: `sensor_model` published
LidarPointCloud as raw struct memory while `perception_node`/`slam_node`
deserialized it as wire layout → `count` was read from the upper half of
`timestamp_us` → the point cloud was silently empty.)

This gate does not fix that.  It pins the current state so a *new* mismatch
(or a changed one) is caught immediately, and so the known list stays honest:
a type that stops mismatching must be removed from KNOWN_MISMATCHES.

Layout simulation follows the C rules the compiler applies for these simple
structs: every member is aligned to its own alignment, the struct's alignment
is the maximum member alignment, and the total is rounded up to it.

Usage:
  python3 ci/gates/msg_layout_check.py
  python3 ci/gates/msg_layout_check.py --list     # print the full table
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from msg_codegen import (  # noqa: E402  (needs sys.path tweak above)
    CodeGenerator,
    IDLParser,
    PRIMITIVE_SIZES,
    StructDef,
)

IDL_FILE = ROOT / "msg" / "adas_msgs.msg"

# Types whose C struct size differs from their declared wire size.  This is the
# *known* state as of 2026-09-21 — 12 of 20 messages.  Keep it accurate: the
# gate fails if a listed entry stops mismatching (then delete it) or if the
# numbers move.
KNOWN_MISMATCHES: dict[str, tuple[int, int]] = {
    # name: (wire_size, struct_size)
    "Behavior":             (22, 32),
    "ControlRaw":           (59, 60),
    "GpsData":              (36, 40),
    "ImuData":              (36, 40),
    "LidarPointCloud":      (40980, 40984),
    "Localization":         (57, 60),
    "Obstacle":             (35, 40),  # D2-07 phase 1: +bool obs_lane_match_hint (wire 34→35, struct 36→40; +4B trailing pad)
    "ObstacleList":         (4496, 5144),
    "Pose2D":               (29, 32),
    "PredictionHypothesis": (1017, 1020),
    "PredictionSet":        (3068, 3088),
    "Trajectory":           (2581, 2592),
}

# C alignment of each primitive (== its size on this project's targets).
_ALIGN = dict(PRIMITIVE_SIZES)


def _align_up(offset: int, align: int) -> int:
    return (offset + align - 1) // align * align


def struct_layout(name: str, structs: dict[str, StructDef],
                  seen: tuple[str, ...] = ()) -> tuple[int, int]:
    """Return (sizeof, alignof) of the generated C struct for ``name``."""
    if name in seen:  # defensive: the IDL has no recursive structs
        raise ValueError(f"recursive struct: {' -> '.join(seen + (name,))}")
    s = structs[name]
    offset = 0
    max_align = 1
    for f in s.fields:
        if f.idl_type in _ALIGN:
            elem_size = elem_align = _ALIGN[f.idl_type]
        elif f.is_enum:
            elem_size = elem_align = 1          # generated as int8_t
        elif f.is_nested:
            elem_size, elem_align = struct_layout(f.nested_typename, structs,
                                                  seen + (name,))
        elif f.idl_type == "char":
            elem_size = elem_align = 1
        else:
            raise ValueError(f"{name}.{f.name}: unknown type '{f.idl_type}'")
        count = f.array_size if f.array_size > 0 else 1
        offset = _align_up(offset, elem_align)
        offset += elem_size * count
        max_align = max(max_align, elem_align)
    return _align_up(offset, max_align), max_align


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true",
                    help="print the full size table (informational)")
    args = ap.parse_args()

    parser = IDLParser(IDL_FILE.read_text())
    parser.parse()
    # Reuse the generator's own wire-size computation so this gate can never
    # drift from what the generated serializer actually emits.
    gen = CodeGenerator(parser)
    structs: dict[str, StructDef] = parser.structs

    mismatches: dict[str, tuple[int, int]] = {}
    rows: list[tuple[str, int, int]] = []
    for name, s in structs.items():
        wire = gen._struct_size(s)
        struct_size, _ = struct_layout(name, structs)
        rows.append((name, wire, struct_size))
        if wire != struct_size:
            mismatches[name] = (wire, struct_size)

    if args.list:
        for name, wire, struct_size in sorted(rows):
            flag = "MISMATCH" if wire != struct_size else "OK"
            print(f"  {name:<24} wire={wire:<8} struct={struct_size:<8} {flag}")
        return 0

    errors: list[str] = []
    for name, (wire, struct_size) in sorted(mismatches.items()):
        known = KNOWN_MISMATCHES.get(name)
        if known is None:
            errors.append(
                f"NEW size mismatch: {name} wire={wire} struct={struct_size} — "
                f"either pack it or use *_serialize()/*_deserialize() for this type, "
                f"then add it to KNOWN_MISMATCHES with a reason"
            )
        elif known != (wire, struct_size):
            errors.append(
                f"size mismatch changed: {name} known={known} now=({wire}, {struct_size})"
            )

    for name, known in sorted(KNOWN_MISMATCHES.items()):
        if name not in mismatches:
            actual = next((r for r in rows if r[0] == name), None)
            detail = f"(now wire={actual[1]} struct={actual[2]})" if actual else "(type gone?)"
            errors.append(
                f"stale KNOWN_MISMATCHES entry: {name} {detail} — "
                f"remove it so the list keeps documenting reality"
            )

    if errors:
        for e in errors:
            print(f"::error::{e}")
        print(f"msg-layout-gate FAILED ({len(errors)} issue(s))."
              f" {len(mismatches)}/{len(rows)} types have wire != struct size;"
              f" see ci/gates/msg_layout_check.py header.")
        return 1

    print(f"✓ msg layout gate OK ({len(rows)} types, "
          f"{len(mismatches)} known wire/struct mismatches documented)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
