#!/usr/bin/env python3
"""backfill_osm_elevation.py — 给已编译 map.json 补 OSM 桥隧/层高高程。

背景：osm_lujiazui_v2 等地图在 elevation 功能落地前生成，map.json 的
bridge/tunnel 标记缺失、centerline z 全 0 → 前端/仿真看不到立体交通。
本工具在**已编译的 map.json** 上做增量回填（不重跑 netconvert），
按 road.id 里内嵌的 OSM way id（road_<wayid>sN / road_<wayid>）回查
Overpass bridge/tunnel/layer 元数据，写 centerline z + bridge/tunnel 标记。

桥 = 6m×max(1,layer)；隧道固定 -4m。与 osm2kmap.apply_osm_elevation 同语义。

用法：
  python3 tools/backfill_osm_elevation.py maps/osm_lujiazui_v2/map.json \
      --ref-lat 31.235 --ref-lon 121.5 --radius 6000

幂等：已有 z 的 centerline 会被覆盖为与 OSM 一致的值；原文件备份到
<path>.bak（若不存在）。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

# 多端点 + 短退避：Overpass 间歇 504/429 属常态，重试 2 轮；避开
# private.coffee（实测会长时间挂起拖垮整次回填）。
OVERPASS_ENDPOINTS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
]


def fetch_osm_bridge_tunnel(ref_lat: float, ref_lon: float, radius_m: int) -> dict:
    """回查区域内带 bridge/tunnel/layer 的 way 元数据（way_id → meta）。"""
    query = f"""
    [out:json][timeout:25];
    (
      way(around:{radius_m},{ref_lat},{ref_lon})[highway][bridge];
      way(around:{radius_m},{ref_lat},{ref_lon})[highway][tunnel];
      way(around:{radius_m},{ref_lat},{ref_lon})[highway][layer];
    );
    out tags center 10000;
    """
    data = urllib.parse.urlencode({"data": query}).encode()
    last_ex = None
    for attempt in range(3):
        for ep in OVERPASS_ENDPOINTS:
            try:
                req = urllib.request.Request(
                    ep, data=data,
                    headers={"User-Agent": "flowengine-elevation-backfill/1.0"})
                with urllib.request.urlopen(req, timeout=45) as r:
                    resp = json.load(r)
                result = {}
                for elem in resp.get("elements", []):
                    if elem.get("type") != "way":
                        continue
                    tags = elem.get("tags", {})
                    layer = None
                    if "layer" in tags:
                        try:
                            layer = int(tags["layer"])
                        except (ValueError, TypeError):
                            pass
                    result[elem["id"]] = {
                        "layer": layer,
                        "bridge": "bridge" in tags,
                        "tunnel": "tunnel" in tags,
                        "name": tags.get("name"),
                    }
                return result
            except Exception as ex:  # noqa: BLE001 —— 端点级重试
                last_ex = ex
        if attempt < 2:
            time.sleep(3 * (attempt + 1))
    raise last_ex if last_ex else RuntimeError("Overpass 全部端点失败")


def extract_way_id(road_id: str) -> int | None:
    """road_1290439822s3 / road_724847592 → OSM way id；connector/无号 → None。"""
    m = re.search(r"road_(\d+)", str(road_id))
    return int(m.group(1)) if m else None


def elev_of(info: dict) -> float | None:
    if info["tunnel"]:
        return -4.0
    if info["bridge"]:
        return 6.0 * max(1, info["layer"] or 1)
    if info["layer"] is not None:
        return 6.0 * max(1, info["layer"]) if info["layer"] > 0 else -4.0
    return None


def apply_z(points: list, z: float) -> int:
    n = 0
    for p in points:
        if len(p) < 3:
            p.append(z)
        else:
            p[2] = z
        n += 1
    return n


def backfill(map_path: Path, ref_lat: float, ref_lon: float, radius: int) -> int:
    doc = json.loads(map_path.read_text(encoding="utf-8"))
    roads = doc.get("roads", [])
    way_ids = {extract_way_id(r.get("id", "")) for r in roads}
    way_ids.discard(None)
    if not way_ids:
        print("map.json 无 road_<wayid> 形式 id，跳过")
        return 0
    meta = fetch_osm_bridge_tunnel(ref_lat, ref_lon, radius)
    if not meta:
        print("Overpass 未返回桥隧元数据，跳过")
        return 0

    n_roads = 0
    n_piers = 0
    n_tun = 0
    for r in roads:
        if "road_j" in str(r.get("id", "")):
            continue  # 内部 connector 不参与高程
        wid = extract_way_id(r.get("id", ""))
        info = meta.get(wid)
        if info is None:
            continue
        z = elev_of(info)
        if z is None and not info["tunnel"] and not info["bridge"]:
            continue
        changed = False
        if z is not None:
            n_piers += apply_z(r.get("centerline", []), z)
            for lane in r.get("lanes", []):
                n_piers += apply_z(lane.get("centerline", []), z)
            changed = True
        if info["tunnel"] and not r.get("tunnel"):
            r["tunnel"] = True
            n_tun += 1
            changed = True
        if info["bridge"] and not r.get("bridge"):
            r["bridge"] = True
            changed = True
        if changed:
            n_roads += 1

    if n_roads:
        backup = map_path.with_suffix(map_path.suffix + ".bak")
        if not backup.exists():
            backup.write_text(map_path.read_text(encoding="utf-8"), encoding="utf-8")
        map_path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                            encoding="utf-8")
    print("回填 %d 条 road 高程（%d 个 centerline/lane 点），标记 tunnel %d 条"
          % (n_roads, n_piers, n_tun))
    return n_roads


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("map_path", type=Path)
    ap.add_argument("--ref-lat", type=float, required=True)
    ap.add_argument("--ref-lon", type=float, required=True)
    ap.add_argument("--radius", type=int, default=6000)
    args = ap.parse_args()
    n = backfill(args.map_path, args.ref_lat, args.ref_lon, args.radius)
    return 0 if n >= 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
