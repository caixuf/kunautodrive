#!/usr/bin/env python3
"""osm_bbox_clip.py — 纯标准库 OSM XML 边界裁剪（osmosis --bounding-box 的轻量替代）。

裁剪规则：
  - 节点：bbox 外的丢弃（带标签的 POI 节点同规则）
  - way：按"节点在 bbox 内的最长连续段"切分；段长 ≥2 保留，继承原 tags
    （跨边界长 way 被截断为 bbox 内部分，避免 OSM2World/netconvert 构建全域几何）
  - relation：丢弃（驾驶/渲染链路均未消费 relation）

用法：
  python3 tools/osm_bbox_clip.py IN.osm OUT.osm LEFT BOTTOM RIGHT TOP
  （经纬度度数，left<right, bottom<top）
"""
from __future__ import annotations

import math
import sys
import xml.etree.ElementTree as ET


def main() -> int:
    if len(sys.argv) != 7:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    left, bottom, right, top = (float(v) for v in sys.argv[3:7])

    tree = ET.parse(src)
    root = tree.getroot()

    def in_bbox(lat: float, lon: float) -> bool:
        return bottom <= lat <= top and left <= lon <= right

    # ── 节点筛选 ─────────────────────────────────────────────
    kept_nodes: dict[str, ET.Element] = {}
    for nd in root.findall("node"):
        if in_bbox(float(nd.get("lat")), float(nd.get("lon"))):
            kept_nodes[nd.get("id")] = nd

    # ── way 按连续内段切分 ───────────────────────────────────
    out_ways: list[ET.Element] = []
    next_id = [1_000_000_000]  # 切分生成的新 way id（避让原 id 空间）

    def flush_run(run: list[str], tags: list[ET.Element]) -> None:
        if len(run) < 2:
            return
        # 丢弃过短碎片：边界切分会产生亚米级 2 点 way，下游（OSM2World
        # 路口连接器等）在退化几何上会出现 Infinity/内存爆炸。
        coords = [(float(kept_nodes[r].get("lat")), float(kept_nodes[r].get("lon")))
                  for r in run]
        total = 0.0
        for (la0, lo0), (la1, lo1) in zip(coords, coords[1:]):
            total += math.hypot((la1 - la0) * 111320.0,
                                (lo1 - lo0) * 111320.0)
        if total < 2.0:
            return
        w = ET.Element("way", {"id": str(next_id[0])})
        next_id[0] += 1
        for ref in run:
            ET.SubElement(w, "nd", {"ref": ref})
        for t in tags:
            w.append(t)
        out_ways.append(w)

    for way in root.findall("way"):
        refs = [nd.get("ref") for nd in way.findall("nd")]
        tags = list(way.findall("tag"))
        run: list[str] = []
        for ref in refs:
            if ref in kept_nodes:
                run.append(ref)
            else:
                flush_run(run, tags)
                run = []
        flush_run(run, tags)

    # 只保留被 way 引用或自身带标签的节点
    used = {ref for w in out_ways for ref in (nd.get("ref") for nd in w.findall("nd"))}
    final_nodes = [nd for nid, nd in kept_nodes.items()
                   if nid in used or nd.findall("tag")]

    # ── 输出 ─────────────────────────────────────────────────
    out = ET.Element("osm", {"version": "0.6", "generator": "osm_bbox_clip"})
    ET.SubElement(out, "bounds", {"minlat": str(bottom), "minlon": str(left),
                                  "maxlat": str(top), "maxlon": str(right)})
    for nd in final_nodes:
        out.append(nd)
    for w in out_ways:
        out.append(w)
    ET.ElementTree(out).write(dst, encoding="UTF-8", xml_declaration=True)
    print("nodes: %d → %d, ways → %d" %
          (len(root.findall("node")), len(final_nodes), len(out_ways)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
