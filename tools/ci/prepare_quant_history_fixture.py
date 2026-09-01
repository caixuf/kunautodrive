#!/usr/bin/env python3
"""
Deterministic CI fixture for tests/test_quant_historical_gate.cpp.

Generates a pinned, synthetic-but-stable commodity history dataset entirely from
stdlib math so GitHub Actions does not depend on external market-data services
or Git LFS state.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import math
import sys
from pathlib import Path

SYMBOLS = [
    "cu", "ru", "rb", "ta", "m", "a", "cf", "i", "j",
    "au", "ag", "zn", "al", "hc", "bu", "MA", "pp", "p",
]


def business_days(start: dt.date, end: dt.date) -> list[dt.date]:
    days: list[dt.date] = []
    cur = start
    while cur <= end:
        if cur.weekday() < 5:
            days.append(cur)
        cur += dt.timedelta(days=1)
    return days


def generate_symbol_rows(symbol: str, symbol_index: int, dates: list[dt.date]) -> list[list[str]]:
    phase = symbol_index * 0.37
    base = 80.0 + symbol_index * 9.0
    prev_close = base
    rows: list[list[str]] = []
    for t, cur_date in enumerate(dates):
        trend = 0.012 * t + 6.0 * math.sin(t / 230.0 + phase)
        cyc = (
            12.0 * math.sin(t / 18.0 + phase)
            + 7.0 * math.sin(t / 47.0 + phase * 1.9)
            + 3.0 * math.sin(t / 7.0 + phase * 0.7)
        )
        pulse = 4.0 * math.sin(t / 3.5 + phase * 1.3)
        close = max(20.0, base + trend + cyc + pulse)
        open_px = max(20.0, prev_close * (1.0 + 0.0035 * math.sin(t / 11.0 + phase)))
        wiggle = 0.008 + 0.006 * abs(math.sin(t / 5.0 + phase))
        high = max(open_px, close) * (1.0 + wiggle)
        low = min(open_px, close) * (1.0 - wiggle)
        volume = 100000 + (symbol_index + 1) * 700 + int(15000 * (1.0 + math.sin(t / 13.0 + phase))) + (t % 17) * 123
        rows.append(
            [
                symbol,
                cur_date.isoformat(),
                f"{open_px:.2f}",
                f"{high:.2f}",
                f"{low:.2f}",
                f"{close:.2f}",
                str(volume),
            ]
        )
        prev_close = close
    return rows


def write_fixture(out_dir: Path, start: dt.date, end: dt.date) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    dates = business_days(start, end)
    for idx, symbol in enumerate(SYMBOLS):
        out_file = out_dir / f"{symbol}.csv"
        with out_file.open("w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(["symbol", "date", "open", "high", "low", "close", "volume"])
            writer.writerows(generate_symbol_rows(symbol, idx, dates))


def inspect_fixture(out_dir: Path) -> dict[str, tuple[int, str | None, str | None]]:
    summary: dict[str, tuple[int, str | None, str | None]] = {}
    for symbol in SYMBOLS:
        csv_path = out_dir / f"{symbol}.csv"
        if not csv_path.exists():
            continue
        with csv_path.open("r", encoding="utf-8", newline="") as fh:
            reader = csv.reader(fh)
            header = next(reader, None)
            if header is None:
                continue
            dates = [row[1] for row in reader if len(row) >= 7]
        summary[symbol] = (len(dates), dates[0] if dates else None, dates[-1] if dates else None)
    return summary


def verify_fixture(out_dir: Path, min_symbols: int, min_bars: int) -> int:
    summary = inspect_fixture(out_dir)
    valid = {sym: meta for sym, meta in summary.items() if meta[0] >= min_bars}
    if len(valid) < min_symbols:
        print(
            f"ERROR: quant history fixture incomplete under {out_dir} "
            f"(found {len(valid)} symbol files with >= {min_bars} bars, need {min_symbols})",
            file=sys.stderr,
        )
        missing = [sym for sym in SYMBOLS if sym not in valid]
        if missing:
            print(f"Missing/short symbols: {', '.join(missing)}", file=sys.stderr)
        return 1

    oldest = min(meta[1] for meta in valid.values() if meta[1] is not None)
    newest = max(meta[2] for meta in valid.values() if meta[2] is not None)
    print(
        f"✓ quant history fixture ready: {len(valid)} symbols, "
        f"date range {oldest}..{newest}, root={out_dir}"
    )
    return 0


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Prepare deterministic quant history fixture for CI")
    ap.add_argument("--out", default="data/history", help="output directory")
    ap.add_argument("--check-only", action="store_true", help="only verify existing fixture")
    ap.add_argument("--min-symbols", type=int, default=10, help="minimum valid symbol CSVs required")
    ap.add_argument("--min-bars", type=int, default=200, help="minimum bars per symbol for preflight")
    ap.add_argument("--start", default="2004-01-02", help="fixture start date (YYYY-MM-DD)")
    ap.add_argument("--end", default="2024-12-31", help="fixture end date (YYYY-MM-DD)")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out)
    if not args.check_only:
        write_fixture(out_dir, dt.date.fromisoformat(args.start), dt.date.fromisoformat(args.end))
    return verify_fixture(out_dir, args.min_symbols, args.min_bars)


if __name__ == "__main__":
    sys.exit(main())
