#!/usr/bin/env python3
"""
KunQuant Terminal Web Server
Lightweight HTTP/WebSocket server for the KunQuant Professional Trading Desk
"""

import http.server
import socketserver
import os
import sys
import webbrowser

PORT = 8900
WEB_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "kunboard")

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def log_message(self, format, *args):
        # 简化日志输出
        sys.stderr.write(f"[KunQuant UI Server] {args[0]} - {args[1]}\n")

def main():
    os.chdir(WEB_DIR)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", PORT), Handler) as httpd:
        print(f"\n=======================================================")
        print(f"  鲲量化 (KunQuant) 专业交易工作台已启动")
        print(f"  URL: http://localhost:{PORT}")
        print(f"=======================================================\n")
        
        if "--no-browser" not in sys.argv:
            try:
                webbrowser.open(f"http://localhost:{PORT}")
            except Exception:
                pass
        
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nServer stopped.")

if __name__ == "__main__":
    main()
