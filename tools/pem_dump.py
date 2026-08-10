#!/usr/bin/env python3
"""Decode FlowEngine embedded monitor .pem binary logs."""

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

RECORD = struct.Struct("<IHHIIQQIHH64s8d")
MAGIC = 0x314D4550
KINDS = {1: "system", 2: "topic", 3: "health", 4: "event"}


def records(path: Path):
    with path.open("rb") as stream:
        offset = 0
        while raw := stream.read(RECORD.size):
            if len(raw) != RECORD.size:
                raise ValueError(f"truncated record at offset {offset}")
            (magic, version, kind, size, crc, monotonic_us, realtime_us,
             sequence, flags, _reserved, name, *values) = RECORD.unpack(raw)
            if magic != MAGIC or version != 1 or size != RECORD.size:
                raise ValueError(f"invalid header at offset {offset}")
            check = bytearray(raw)
            struct.pack_into("<I", check, 12, 0)
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
            else:
                fields = {"values": values}
                record_type = f"unknown:{kind}"
            yield {"monotonic_us": monotonic_us, "realtime_us": realtime_us,
                   "sequence": sequence, "critical": bool(flags & 1),
                   "type": record_type, "name": label, **fields}
            offset += RECORD.size


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
