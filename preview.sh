#!/usr/bin/env bash
# demo.sh — 一键起「独立网页」完整预览地图（无需 C++ monitor_server）。
# 默认预览新陆家嘴 osm_lujiazui_v2；可传地图名/端口，例如：
#   ./demo.sh                       # 预览 osm_lujiazui_v2，自动开浏览器
#   ./demo.sh city_ring             # 预览其他地图
#   ./demo.sh osm_lujiazui_v2 --port 9000 --no-open
set -euo pipefail
cd "$(dirname "$0")"
exec python3 tools/preview_map.py "$@"
