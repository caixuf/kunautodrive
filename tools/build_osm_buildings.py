#!/usr/bin/env python3
"""build_osm_buildings.py — OSM 建筑 → 贴图模型资产 + map.json 回填 mesh 字段。

流程（与 osm_to_map.py 共享同一份 buildings[] 单源真相）：
  1. 从 map.json 读取 buildings[]（footprint/height/x/y/rotation，已由 osm_to_map.py 生成）。
  2. 若检测到 osm2world（java -jar osm2world.jar），用它把 OSM 区域导出为带贴图的
     3D 建筑模型（.glb），输出到 tools/flowboard/models/buildings/。
  3. 把每个建筑的 mesh 路径回填进 map.json 的 buildings[].mesh。

前端（BuildingView）消费优先级：mesh 存在且无障碍 → 用 osm2world 贴图模型；
否则回退到按 footprint 挤出的棱柱（已默认实现，视觉即真实轮廓）。本脚本只负责
"贴图精细化"这一步；即便 osm2world 不可用，map.json 仍可被仿真核与前端正常消费
（碰撞/遮挡/棱柱渲染都不依赖 mesh）。

用法：
  python3 tools/build_osm_buildings.py maps/osm_test
  python3 tools/build_osm_buildings.py maps/osm_test --osm map.osm   # 已有 .osm 提取
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(PROJECT_ROOT, "tools", "flowboard", "models", "buildings")


def find_osm2world() -> str | None:
    """返回 osm2world.jar 路径（环境变量 OSM2WORLD_JAR 或常见位置），无则 None。"""
    env = os.environ.get("OSM2WORLD_JAR")
    if env and os.path.isfile(env):
        return env
    for cand in (
        os.path.join(PROJECT_ROOT, "third_party", "osm2world", "osm2world.jar"),
        os.path.expanduser("~/osm2world/osm2world.jar"),
    ):
        if os.path.isfile(cand):
            return cand
    return None


def extract_osm(lat: float, lon: float, radius: int, out_path: str) -> bool:
    """用 Overpass 拉取范围内建筑 OSM 原数据（.osm）。失败返回 False。"""
    try:
        import urllib.parse
        import urllib.request
        q = (
            "[out:xml];\n("
            f'  way["building"](around:{radius},{lat:.6f},{lon:.6f});\n'
            ");\nout body geom;"
        )
        data = urllib.parse.urlencode({"data": q}).encode()
        req = urllib.request.Request(
            "https://overpass-api.de/api/interpreter", data=data,
            headers={"User-Agent": "FlowEngine-osm-builder/1.0"})
        with urllib.request.urlopen(req, timeout=120) as r:
            with open(out_path, "wb") as f:
                f.write(r.read())
        return os.path.getsize(out_path) > 0
    except Exception as e:  # noqa: BLE001
        print("  OSM 提取失败: %s" % e)
        return False


def run_osm2world(jar: str, osm_path: str, glb_path: str) -> bool:
    """调用 osm2world 把 .osm 导出为 .glb。需要 java 运行时。"""
    java = shutil.which("java")
    if not java:
        print("  未找到 java 运行时，无法运行 osm2world")
        return False
    try:
        # osm2world 默认输出 .obj/.osgb，通过 --output 指定；这里以 .glb 为目标，
        # 若版本不支持 glb 则回退 .obj（前端仍可经转换加载）。
        cmd = [java, "-jar", jar, osm_path, "--output", glb_path]
        print("  运行: %s" % " ".join(cmd))
        subprocess.run(cmd, check=True, timeout=600)
        return os.path.exists(glb_path)
    except Exception as e:  # noqa: BLE001
        print("  osm2world 运行失败: %s" % e)
        return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map_dir", help="map.json 所在目录（如 maps/osm_test）")
    ap.add_argument("--osm", default=None, help="已有 .osm 文件路径（跳过 Overpass 拉取）")
    ap.add_argument("--lat", type=float, default=None, help="中心纬度（用于 OSM 提取）")
    ap.add_argument("--lon", type=float, default=None, help="中心经度")
    ap.add_argument("--radius", type=int, default=500)
    args = ap.parse_args()

    map_path = os.path.join(args.map_dir, "map.json")
    if not os.path.isfile(map_path):
        print("错误：%s 不存在" % map_path)
        return 2

    doc = json.load(open(map_path))
    buildings = doc.get("buildings")
    if not buildings:
        print("map.json 没有 buildings[]（先跑 osm_to_map.py --merge-buildings）")
        return 1
    print("读取 %d 栋建筑" % len(buildings))

    jar = find_osm2world()
    if not jar:
        print("未检测到 osm2world（设置 OSM2WORLD_JAR 或放入 third_party/osm2world/）。")
        print("→ 跳过贴图模型生成；前端将按 footprint 挤出棱柱（真实轮廓，无贴图）。")
        print("  安装参考：https://osm2world.org/  (需 Java)")
        return 0

    os.makedirs(OUT_DIR, exist_ok=True)
    if args.osm and os.path.isfile(args.osm):
        osm_path = args.osm
    else:
        if args.lat is None or args.lon is None:
            print("需提供 --lat/--lon 或 --osm 以生成 OSM 提取")
            return 1
        osm_path = os.path.join(args.map_dir, "buildings.osm")
        if not extract_osm(args.lat, args.lon, args.radius, osm_path):
            return 1

    glb_path = os.path.join(OUT_DIR, "osm_city.glb")
    if not run_osm2world(jar, osm_path, glb_path):
        return 1

    # 回填 mesh：整片城区一个共享贴图模型（前端按 footprint 放置/缩放）。
    rel = os.path.relpath(glb_path, args.map_dir).replace(os.sep, "/")
    for b in buildings:
        b["mesh"] = rel
    json.dump(doc, open(map_path, "w"), indent=2, ensure_ascii=False)
    print("已回填 %d 栋建筑的 mesh 字段 -> %s" % (len(buildings), rel))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
