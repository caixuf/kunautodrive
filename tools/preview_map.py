#!/usr/bin/env python3
"""preview_map.py — 一键起「独立网页」完整预览地图（无需 C++ monitor_server）。

flowboard 的 map_preview.html 依赖 POST /api/map/preview 取车道级地图数据，
线上由 monitor_server 提供。本脚本在本地起一个静态服务器：
  * 以仓库根为根，提供 /tools/flowboard/* 与 /maps/*（页面与 vendored three 均本地，离线可用）；
  * 拦截 /api/map/preview，直接从磁盘 maps/<id>/map.json + routes.json 返回，
    契约与 monitor_server 一致：{ ok:true, map:<map.json>, routes:<routes.json> }。

three.js 走 map_preview.html 内 import-map 指向的本地 vendor，无外网依赖。

用法：
  python3 tools/preview_map.py                      # 默认预览 osm_lujiazui_v2，自动开浏览器
  python3 tools/preview_map.py --map city_ring      # 预览其他地图
  python3 tools/preview_map.py --port 9000 --no-open
  python3 tools/preview_map.py --list               # 列出可用地图
"""
from __future__ import annotations

import argparse
import functools
import json
import os
import sys
import threading
import webbrowser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS_DIR = os.path.join(REPO, "maps")
DEFAULT_MAP = "osm_lujiazui_v2"


def list_maps() -> list[str]:
    if not os.path.isdir(MAPS_DIR):
        return []
    out = []
    for name in sorted(os.listdir(MAPS_DIR)):
        mp = os.path.join(MAPS_DIR, name, "map.json")
        if os.path.isfile(mp):
            out.append(name)
    return out


def sanitize_map_id(map_id: str) -> str | None:
    if not map_id or not all(c.isalnum() or c in "_-" for c in map_id):
        return None
    if ".." in map_id:
        return None
    return map_id


def make_handler():
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=REPO, **kwargs)

        def _send_json(self, payload: dict, status: int = 200):
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            parsed = urlparse(self.path)
            if parsed.path.rstrip("/") != "/api/map/preview":
                self.send_error(404, "Not Found")
                return
            try:
                length = int(self.headers.get("Content-Length", "0") or "0")
                raw = self.rfile.read(length) if length else b"{}"
                req = json.loads(raw.decode("utf-8") or "{}")
            except Exception as e:  # noqa: BLE001
                self._send_json({"ok": False, "error": f"bad request: {e}"}, 400)
                return

            map_id = sanitize_map_id(str(req.get("map") or ""))
            if not map_id:
                self._send_json({"ok": False, "error": "invalid map id"}, 400)
                return

            map_path = os.path.join(MAPS_DIR, map_id, "map.json")
            map_path = os.path.abspath(map_path)
            if not map_path.startswith(os.path.abspath(MAPS_DIR)) or not os.path.isfile(map_path):
                self._send_json({"ok": False, "error": f"map not found: {map_id}"}, 404)
                return

            try:
                with open(map_path, encoding="utf-8") as f:
                    map_doc = json.load(f)
            except Exception as e:  # noqa: BLE001
                self._send_json({"ok": False, "error": f"map parse error: {e}"}, 500)
                return

            routes_doc: dict = {"routes": []}
            routes_path = os.path.join(MAPS_DIR, map_id, "routes.json")
            if os.path.isfile(routes_path):
                try:
                    with open(routes_path, encoding="utf-8") as f:
                        routes_doc = json.load(f)
                except Exception:  # noqa: BLE001
                    routes_doc = {"routes": []}

            self._send_json({"ok": True, "map": map_doc, "routes": routes_doc})

        def log_message(self, fmt, *args):  # 静默常规请求日志，仅错误可见
            if fmt.startswith("%s %s 200") or "200" in str(args):
                return
            sys.stderr.write("%s - - %s\n" % (self.address_string(), fmt % args))

    return Handler


def find_free_port(host: str, preferred: int) -> int:
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind((host, preferred))
        port = s.getsockname()[1]
        return port
    except OSError:
        s.close()
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.bind((host, 0))
        port = s.getsockname()[1]
        return port
    finally:
        try:
            s.close()
        except OSError:
            pass


def main() -> int:
    ap = argparse.ArgumentParser(description="一键起独立网页完整预览地图（无需 monitor_server）")
    ap.add_argument("--map", default=DEFAULT_MAP, help=f"地图目录名（默认 {DEFAULT_MAP}）")
    ap.add_argument("--host", default="127.0.0.1", help="绑定地址（默认 127.0.0.1）")
    ap.add_argument("--port", type=int, default=8011, help="首选端口（默认 8011，占用则自动顺延）")
    ap.add_argument("--no-open", action="store_true", help="不自动打开浏览器")
    ap.add_argument("--list", action="store_true", help="列出可用地图后退出")
    args = ap.parse_args()

    if args.list:
        print("可用地图：")
        for m in list_maps():
            print("  -", m)
        return 0

    if args.map not in list_maps():
        print(f"[preview] 未找到地图 '{args.map}'。可用：{list_maps()}", file=sys.stderr)
        return 2

    port = find_free_port(args.host, args.port)
    handler = make_handler()
    httpd = ThreadingHTTPServer((args.host, port), handler)

    url = f"http://{args.host}:{port}/tools/flowboard/map_preview.html?map={args.map}"
    print(f"[preview] 服务已起：{args.host}:{port}（仓库根为静态根）", flush=True)
    print(f"[preview] 打开：{url}", flush=True)
    print("[preview] 按 Ctrl+C 停止。", flush=True)

    if not args.no_open:
        # 放到后台线程：即便环境无浏览器（headless），也不阻塞/拖垮主服务。
        try:
            t = threading.Thread(target=lambda: webbrowser.open(url), daemon=True)
            t.start()
        except Exception:  # noqa: BLE001
            pass

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[preview] 已停止。")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
