#!/usr/bin/env python3
"""Render a reusable map JSON as a standalone SVG preview."""

from __future__ import annotations

import argparse
import html
import json
from pathlib import Path


def render(path: Path) -> str:
    data = json.loads(path.read_text(encoding="utf-8"))
    roads = data.get("roads", [])
    points = [
        point
        for road in roads
        for point in road.get("centerline", road.get("nodes", []))
        if len(point) >= 2
    ]
    if not points:
        raise ValueError(f"{path}: no road centerlines")
    min_x = min(point[0] for point in points)
    max_x = max(point[0] for point in points)
    min_y = min(point[1] for point in points)
    max_y = max(point[1] for point in points)
    margin = 80.0
    width, height = 1200.0, 800.0
    scale = min((width - 2 * margin) / max(max_x - min_x, 1.0),
                (height - 2 * margin) / max(max_y - min_y, 1.0))

    def xy(point):
        return (margin + (point[0] - min_x) * scale,
                height - margin - (point[1] - min_y) * scale)

    lines = []
    labels = []
    for road in roads:
        centerline = road.get("centerline", road.get("nodes", []))
        if len(centerline) < 2:
            continue
        d = " ".join(
            ("M" if index == 0 else "L") + f" {xy(point)[0]:.1f},{xy(point)[1]:.1f}"
            for index, point in enumerate(centerline)
        )
        road_id = html.escape(str(road.get("id", "road")))
        lines.append(f'<path class="road" d="{d}" data-road="{road_id}"/>')
        lx, ly = xy(centerline[len(centerline) // 2])
        labels.append(f'<text x="{lx:.1f}" y="{ly - 8:.1f}">{road_id}</text>')

    connections = []
    for connection in data.get("connections", []):
        connections.append(
            f'<li>{html.escape(str(connection.get("from_road", connection.get("from", ""))))}'
            f' → {html.escape(str(connection.get("to_road", connection.get("to", ""))))}'
            f' ({html.escape(str(connection.get("type", "connection")))})</li>'
        )
    legend = "".join(connections) or "<li>no explicit connections</li>"
    title = html.escape(str(data.get("name", data.get("map_id", path.stem))))
    return f"""<!doctype html>
<meta charset="utf-8">
<title>{title} map preview</title>
<style>
body {{ margin: 0; background: #111827; color: #e5e7eb; font: 14px sans-serif; }}
main {{ display: flex; gap: 20px; padding: 20px; }}
svg {{ background: #263238; border: 1px solid #4b5563; }}
.road {{ fill: none; stroke: #d1d5db; stroke-width: 8; stroke-linecap: round; stroke-linejoin: round; }}
text {{ fill: #fbbf24; font-size: 13px; }}
aside {{ min-width: 260px; }} li {{ margin: 8px 0; }}
</style>
<main><svg viewBox="0 0 {width:.0f} {height:.0f}" width="{width:.0f}" height="{height:.0f}">
{"".join(lines)}{"".join(labels)}
</svg><aside><h2>{title}</h2><ul>{legend}</ul></aside></main>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(render(args.map), encoding="utf-8")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
