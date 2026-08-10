#!/usr/bin/env python3
"""Decode KunAutoDrive embedded monitor .pem binary logs."""

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

KINDS = {1: "system", 2: "topic", 3: "health", 4: "event", 5: "business"}


def load_layout() -> tuple[dict[str, int], dict[str, int], int]:
    """Load the same append-only PEM v1 layout consumed by the C writer."""
    layout = None
    for parent in (Path(__file__).resolve().parent, *Path(__file__).resolve().parents):
        for candidate in (
            parent / "include" / "pem_log_layout.def",
            parent / "include" / "flowengine" / "pem_log_layout.def",
        ):
            if candidate.is_file():
                layout = candidate
                break
        if layout is not None:
            break
    if layout is None:
        raise RuntimeError("cannot find include/pem_log_layout.def")

    sizes: dict[str, int] = {}
    for line in layout.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line.startswith("PEM_FIELD(") or not line.endswith(")"):
            continue
        name, size = line[len("PEM_FIELD("):-1].split(",", 1)
        sizes[name.strip()] = int(size.strip())
    required = ("magic", "version", "type", "size", "crc32", "monotonic_us",
                "realtime_us", "sequence", "flags", "reserved", "name", "values")
    if tuple(sizes) != required:
        raise RuntimeError(f"unsupported PEM layout in {layout}")
    offsets: dict[str, int] = {}
    offset = 0
    for name, size in sizes.items():
        offsets[name] = offset
        offset += size
    return sizes, offsets, offset


SIZES, OFFSETS, RECORD_SIZE = load_layout()
MAGIC = 0x314D4550


def u16(raw: bytes, field: str) -> int:
    return int.from_bytes(raw[OFFSETS[field]:OFFSETS[field] + SIZES[field]], "little")


def u32(raw: bytes, field: str) -> int:
    return int.from_bytes(raw[OFFSETS[field]:OFFSETS[field] + SIZES[field]], "little")


def u64(raw: bytes, field: str) -> int:
    return int.from_bytes(raw[OFFSETS[field]:OFFSETS[field] + SIZES[field]], "little")


def records(path: Path):
    with path.open("rb") as stream:
        offset = 0
        while raw := stream.read(RECORD_SIZE):
            if len(raw) != RECORD_SIZE:
                raise ValueError(f"truncated record at offset {offset}")
            magic = u32(raw, "magic")
            version = u16(raw, "version")
            kind = u16(raw, "type")
            size = u32(raw, "size")
            crc = u32(raw, "crc32")
            monotonic_us = u64(raw, "monotonic_us")
            realtime_us = u64(raw, "realtime_us")
            sequence = u32(raw, "sequence")
            flags = u16(raw, "flags")
            name = raw[OFFSETS["name"]:OFFSETS["name"] + SIZES["name"]]
            values = struct.unpack_from(
                f"<{SIZES['values'] // 8}d", raw, OFFSETS["values"])
            if magic != MAGIC or version != 1 or size != RECORD_SIZE:
                raise ValueError(f"invalid header at offset {offset}")
            check = bytearray(raw)
            check[OFFSETS["crc32"]:OFFSETS["crc32"] + SIZES["crc32"]] = b"\0" * SIZES["crc32"]
            if zlib.crc32(check) & 0xFFFFFFFF != crc:
                raise ValueError(f"CRC mismatch at offset {offset}")
            label = name.split(b"\0", 1)[0].decode("utf-8", errors="replace")
            if kind == 1:
                fields = dict(zip(("cpu_pct", "mem_pct", "rss_kb", "load1", "threads"), values))
                record_type = "system"
            elif kind == 2:
                fields = dict(zip(("frequency_hz", "avg_latency_us", "p99_latency_us",
                                   "dropped", "subscribers", "published", "delivered",
                                   "deadline_violations"), values))
                record_type = "topic"
            elif kind == 3:
                fields = dict(zip(("status", "capabilities", "errors", "avg_latency_us",
                                   "p99_latency_us", "stalls", "cpu_pct",
                                   "last_heartbeat_us"), values))
                record_type = "health"
            elif kind == 4:
                fields = dict(zip(("level", "reason", "transition_ms",
                                   "disable_lane_change", "speed_limit",
                                   "safety_margin"), values))
                record_type = "event"
            elif kind == 5:
                fields = dict(zip(("distance_m", "driving_time_s", "speed_mps",
                                   "latitude", "longitude", "accuracy_m",
                                   "source", "fix_age_s"), values))
                record_type = "business"
            else:
                fields = {"values": values}
                record_type = f"unknown:{kind}"
            yield {"monotonic_us": monotonic_us, "realtime_us": realtime_us,
                   "sequence": sequence, "critical": bool(flags & 1),
                   "type": record_type, "name": label, **fields}
            offset += RECORD_SIZE


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--jsonl", action="store_true")
    parser.add_argument("--type", choices=sorted(KINDS.values()))
    parser.add_argument("--name")
    parser.add_argument("--since-us", type=int, help="minimum realtime timestamp in us")
    parser.add_argument("--until-us", type=int, help="maximum realtime timestamp in us")
    args = parser.parse_args()
    try:
        for path in args.files:
            for record in records(path):
                if args.type and record["type"] != args.type:
                    continue
                if args.name and args.name not in record["name"]:
                    continue
                if args.since_us and record["realtime_us"] < args.since_us:
                    continue
                if args.until_us and record["realtime_us"] > args.until_us:
                    continue
                if args.jsonl:
                    print(json.dumps(record, ensure_ascii=False))
                elif record["type"] == "system":
                    print(f'{record["realtime_us"]} system cpu={record["cpu_pct"]:.1f}% '
                          f'mem={record["mem_pct"]:.1f}% rss={record["rss_kb"]:.0f}KB')
                else:
                    print(f'{record["realtime_us"]} {record["type"]:<6} '
                          f'{record["name"]:<31} critical={int(record["critical"])} '
                          f'{record.get("frequency_hz", record.get("status", record.get("level", 0))):.1f}')
    except (OSError, ValueError) as error:
        print(f"pem_dump: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
