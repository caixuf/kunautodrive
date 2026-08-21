#!/usr/bin/env python3
"""check_map_connectivity.py — 全地图 main 链车道连通回归检查（P2，2026-08）。

对 `maps/*/` 下每个同时含 map.json + routes.json 的地图跑
`astar_route.py --check`（每条 route：相邻 road 对有 lane successor 连通 +
首尾 road A* 端点可达）。地图数据（osm_to_map / grid / map_compiler）或
routes.json 改动后，main 链一旦断开（lane successors 缺失 / chain 引用错路 /
几何 gap），本检查立即 FAIL —— 与 scenario-file-gate 对称，防止"改了地图
demo 里 ego 开不动"类回归。

设计：
  * 只检查**被 scenes 引用**的地图（scenarios/*.json 的 map_file），纯仓库
    附带的示例/半成品图不阻塞合并。
  * **已知断链白名单**：既有数据质量问题（如 osm_test 陆家嘴环路 14.7m gap、
    city_center 无 lane successors）在 KNOWN_BROKEN 列出，命中时 WARN 不阻断；
    白名单外的任何 FAIL 都 ERROR 阻断——保证"新断链必被抓，历史债不挡路"。

用法：
  python3 tools/check_map_connectivity.py            # 全量检查（CI 用）
  python3 tools/check_map_connectivity.py --map maps/osm_lujiazui
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

# 本项目根目录（脚本位于 tools/ 下）
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from tools.astar_route import check as check_route  # noqa: E402

# ── 已知断链白名单（已全部修复，白名单已清空）
KNOWN_BROKEN: set[str] = set()


def referenced_maps() -> set[str]:
    """scenarios/*.json 引用的 map 目录 id（map_file 形如 ../maps/<id>/map.json）。"""
    refs: set[str] = set()
    for path in glob.glob(os.path.join(ROOT, "scenarios", "*.json")):
        try:
            with open(path, encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, ValueError):
            continue
        mf = doc.get("map_file")
        if isinstance(mf, str) and "/maps/" in mf:
            mid = mf.split("/maps/", 1)[1].split("/", 1)[0]
            refs.add(mid)
    return refs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--map", dest="only_map", help="只检查指定地图目录（如 maps/osm_lujiazui）")
    ap.add_argument("--all", action="store_true",
                    help="检查全部地图（含未被场景引用的示例图）")
    args = ap.parse_args()

    referenced = referenced_maps()
    maps_dir = os.path.join(ROOT, "maps")
    failures, warnings = [], []
    checked = 0

    for map_dir in sorted(glob.glob(os.path.join(maps_dir, "*"))):
        if not os.path.isdir(map_dir):
            continue
        mid = os.path.basename(map_dir)
        if not os.path.exists(os.path.join(map_dir, "map.json")) or \
           not os.path.exists(os.path.join(map_dir, "routes.json")):
            continue
        if not args.all and args.only_map is None and mid not in referenced:
            print("  skip %-14s （未被场景引用）" % mid)
            continue
        if args.only_map and mid not in args.only_map and map_dir not in args.only_map:
            continue

        checked += 1
        print("=== %s ===" % map_dir)
        code = check_route(map_dir)
        if code == 0:
            print("  PASS")
        elif mid in KNOWN_BROKEN:
            warnings.append(mid)
            print("  WARN 已知断链（白名单，不阻断）→ %s" % mid)
        else:
            failures.append(mid)
            print("  ERROR → %s" % mid)

    print("\n%d 张地图检查，%d FAIL，%d WARN" % (checked, len(failures), len(warnings)))
    if warnings:
        print("已知断链白名单（待修复）: %s" % ", ".join(warnings))
    if failures:
        print("FAIL: %s" % ", ".join(failures))
        return 1
    print("地图连通性检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
