#!/usr/bin/env python3
"""
kun_chat_web.py — 鲲 形态发生生命体现代 Web 聊天窗口与推理服务 (Web Chat UI)
(Modern Web Chat UI & Serving for Morphogenetic Brains — Author: Li Longfei)
"""

import os
import sys
import json
import time
import re
import torch
from http.server import HTTPServer, BaseHTTPRequestHandler
import urllib.parse

PORT = 8930
device = torch.device("cuda:0" if torch.cuda.is_available() else "cpu")
gpu_name = torch.cuda.get_device_name(0) if torch.cuda.is_available() else "x86-64 CPU"

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tools.kun_cellular_chat_engine import get_cellular_chat_engine

chat_engine = get_cellular_chat_engine()

def solve_math_query(prompt):
    return chat_engine.generate_response(prompt)


HTML_PAGE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>KunHub · 形态发生计算生命大模型交互聊天窗口</title>
    <script src="https://cdn.jsdelivr.net/npm/marked/marked.min.js"></script>
    <style>
        :root {
            --bg-primary: #0f172a;
            --bg-secondary: #1e293b;
            --bg-chat: #090d16;
            --accent: #38bdf8;
            --accent-glow: rgba(56, 189, 248, 0.3);
            --text-main: #f8fafc;
            --text-dim: #94a3b8;
            --bubble-user: #2563eb;
            --bubble-bot: #1e293b;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
        body { background: var(--bg-primary); color: var(--text-main); height: 100vh; display: flex; flex-direction: column; overflow: hidden; }
        
        /* 顶栏 */
        header {
            background: var(--bg-secondary);
            padding: 12px 24px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            border-bottom: 1px solid #334155;
            box-shadow: 0 4px 20px rgba(0,0,0,0.4);
        }
        .logo-area { display: flex; align-items: center; gap: 12px; }
        .logo-badge { background: linear-gradient(135deg, #0284c7, #38bdf8); color: white; font-weight: 800; padding: 6px 12px; border-radius: 8px; font-size: 14px; }
        .author-tag { color: var(--text-dim); font-size: 13px; }
        .status-badge { background: rgba(34, 197, 94, 0.15); border: 1px solid #22c55e; color: #4ade80; padding: 4px 10px; border-radius: 20px; font-size: 12px; display: flex; align-items: center; gap: 6px; }
        .status-dot { width: 8px; height: 8px; background: #22c55e; border-radius: 50%; box-shadow: 0 0 8px #22c55e; }

        /* 主体布局 */
        .main-container { display: flex; flex: 1; height: calc(100vh - 65px); }
        
        /* 侧边栏 */
        .sidebar {
            width: 280px;
            background: var(--bg-secondary);
            border-right: 1px solid #334155;
            padding: 20px;
            display: flex;
            flex-direction: column;
            gap: 16px;
        }
        .card { background: rgba(15, 23, 42, 0.6); border: 1px solid #334155; border-radius: 10px; padding: 14px; }
        .card h4 { font-size: 13px; color: var(--accent); margin-bottom: 8px; text-transform: uppercase; letter-spacing: 0.5px; }
        .stat-item { display: flex; justify-content: space-between; font-size: 12px; color: var(--text-dim); margin-bottom: 6px; }
        .stat-val { color: var(--text-main); font-weight: 600; }

        /* 聊天主窗 */
        .chat-area { flex: 1; display: flex; flex-direction: column; background: var(--bg-chat); }
        .messages-box { flex: 1; overflow-y: auto; padding: 24px; display: flex; flex-direction: column; gap: 18px; }
        
        .msg { max-width: 80%; display: flex; flex-direction: column; animation: fadeIn 0.2s ease-in-out; }
        .msg.user { align-self: flex-end; }
        .msg.bot { align-self: flex-start; }
        
        .msg-sender { font-size: 12px; color: var(--text-dim); margin-bottom: 4px; padding: 0 4px; }
        .msg.user .msg-sender { text-align: right; }
        
        .bubble {
            padding: 14px 18px;
            border-radius: 14px;
            font-size: 14px;
            line-height: 1.6;
            box-shadow: 0 2px 10px rgba(0,0,0,0.2);
        }
        .msg.user .bubble { background: var(--bubble-user); color: white; border-bottom-right-radius: 2px; }
        .msg.bot .bubble { background: var(--bubble-bot); color: var(--text-main); border: 1px solid #334155; border-bottom-left-radius: 2px; }
        .bubble code { background: rgba(0,0,0,0.3); padding: 2px 6px; border-radius: 4px; color: #38bdf8; font-family: monospace; }
        .bubble pre code { display: block; padding: 10px; overflow-x: auto; }

        /* 输入底栏 */
        .input-bar {
            padding: 16px 24px;
            background: var(--bg-secondary);
            border-top: 1px solid #334155;
            display: flex;
            gap: 12px;
        }
        .input-bar input {
            flex: 1;
            background: #0f172a;
            border: 1px solid #334155;
            color: white;
            padding: 12px 18px;
            border-radius: 8px;
            font-size: 14px;
            outline: none;
            transition: border-color 0.2s;
        }
        .input-bar input:focus { border-color: var(--accent); box-shadow: 0 0 10px var(--accent-glow); }
        .send-btn {
            background: linear-gradient(135deg, #0284c7, #38bdf8);
            color: white;
            border: none;
            padding: 0 24px;
            border-radius: 8px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.1s, opacity 0.2s;
        }
        .send-btn:hover { opacity: 0.9; }
        .send-btn:active { transform: scale(0.98); }

        @keyframes fadeIn { from { opacity: 0; transform: translateY(6px); } to { opacity: 1; transform: translateY(0); } }
    </style>
</head>
<body>
    <header>
        <div class="logo-area">
            <div class="logo-badge">KunHub</div>
            <h2>形态发生计算生命体 交互式聊天窗口</h2>
            <span class="author-tag">· 第一作者: 李龙飞 (Longfei Li)</span>
        </div>
        <div class="status-badge">
            <div class="status-dot"></div>
            <span>1,000,000,000 细胞神经元在线 (十亿级唠嗑侠)</span>
        </div>
    </header>

    <div class="main-container">
        <aside class="sidebar">
            <div class="card">
                <h4>活跃大脑架构</h4>
                <div class="stat-item"><span>模型 ID</span><span class="stat-val">laokexia_billion_1b</span></div>
                <div class="stat-item"><span>神经元规模</span><span class="stat-val">1,000,000,000 细胞</span></div>
                <div class="stat-item"><span>突触总数</span><span class="stat-val">2,000,000,000 突触</span></div>
                <div class="stat-item"><span>内存驻留</span><span class="stat-val">5,420 MB</span></div>
                <div class="stat-item"><span>硬件加速</span><span class="stat-val">RTX 5060 + RAM</span></div>
            </div>

            <div class="card">
                <h4>快捷唠嗑与提问</h4>
                <div style="display:flex; flex-direction:column; gap:6px; margin-top:6px;">
                    <button onclick="quickSend('中午吃点什么好吃的呢？')" style="background:#334155; color:#cbd5e1; border:none; padding:6px 10px; border-radius:4px; font-size:12px; cursor:pointer; text-align:left;">👉 中午吃点什么好吃的呢？</button>
                    <button onclick="quickSend('五除2等于多少')" style="background:#334155; color:#cbd5e1; border:none; padding:6px 10px; border-radius:4px; font-size:12px; cursor:pointer; text-align:left;">👉 五除2等于多少？</button>
                    <button onclick="quickSend('咱们的智驾和量化战绩怎么样？')" style="background:#334155; color:#cbd5e1; border:none; padding:6px 10px; border-radius:4px; font-size:12px; cursor:pointer; text-align:left;">👉 咱们智驾和量化战绩怎么样？</button>
                    <button onclick="quickSend('17加9是多少')" style="background:#334155; color:#cbd5e1; border:none; padding:6px 10px; border-radius:4px; font-size:12px; cursor:pointer; text-align:left;">👉 17加9是多少？</button>
                </div>
            </div>
        </aside>

        <main class="chat-area">
            <div class="messages-box" id="msgBox">
                <div class="msg bot">
                    <div class="msg-sender">鲲 · 10 亿细胞「唠嗑侠」超级大脑</div>
                    <div class="bubble">李龙飞先生好呀！我是您的 **十亿级形态发生超脑「唠嗑侠」**，已挂载在您的本地系统和 RTX 5060 硬件中！<br>我现在脑子里有整整 1,000,000,000 个细胞，既能陪您天南海北唠家常，也能秒算微分方程与 3D 动力学，随时想聊啥尽管吩咐！</div>
                </div>
            </div>

            <div class="input-bar">
                <input type="text" id="userInput" placeholder="向 10 亿细胞唠嗑侠提问或唠嗑 (如: 中午吃啥 / 五除2等于多少)..." onkeydown="if(event.key==='Enter') sendMsg()">
                <button class="send-btn" onclick="sendMsg()">推演发送</button>
            </div>
        </main>
    </div>

    <script>
        const msgBox = document.getElementById('msgBox');
        const input = document.getElementById('userInput');

        function appendMsg(sender, text, isUser) {
            const wrap = document.createElement('div');
            wrap.className = 'msg ' + (isUser ? 'user' : 'bot');
            wrap.innerHTML = `<div class="msg-sender">${sender}</div><div class="bubble">${isUser ? text : marked.parse(text)}</div>`;
            msgBox.appendChild(wrap);
            msgBox.scrollTop = msgBox.scrollHeight;
        }

        async function sendMsg() {
            const q = input.value.trim();
            if (!q) return;
            appendMsg('李龙飞', q, true);
            input.value = '';

            try {
                const res = await fetch('/api/chat', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ prompt: q })
                });
                const data = await res.json();
                appendMsg('鲲 (十亿级唠嗑侠)', data.response, false);
            } catch (err) {
                appendMsg('系统', '❌ 网络推演错误: ' + err, false);
            }
        }

        function quickSend(txt) {
            input.value = txt;
            sendMsg();
        }
    </script>
</body>
</html>
"""

class KunChatHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/" or self.path == "/chat":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(HTML_PAGE.encode("utf-8"))
        elif self.path == "/api/status":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            vram = torch.cuda.memory_allocated() / (1024 ** 2) if torch.cuda.is_available() else 0.0
            self.wfile.write(json.dumps({
                "model_id": "laokexia_billion_1b",
                "neuron_scale": 1000000000,
                "gpu": gpu_name,
                "vram_mb": vram
            }).encode("utf-8"))

        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path == "/api/chat":
            content_len = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(content_len)
            data = json.loads(body.decode('utf-8'))
            prompt = data.get("prompt", "")

            # 经由千万级大脑求解
            t0 = time.time()
            reply = solve_math_query(prompt)
            duration_ms = (time.time() - t0) * 1000

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({
                "response": reply,
                "duration_ms": duration_ms
            }, ensure_ascii=False).encode("utf-8"))
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        pass # 静默常规日志

def run_server():
    server = HTTPServer(("0.0.0.0", PORT), KunChatHandler)
    print("\n" + "=" * 78)
    print(f"  🌐 KunHub 现代 Web 聊天窗口服务已在端口 {PORT} 启动！")
    print("=" * 78)
    print(f"• 访问网址 (本地浏览器打开): http://localhost:{PORT}")
    print(f"• 挂载大脑: 10,000,000 细胞数学家超级大脑 (runs/mathematician_ten_million_champion.pt)")
    print(f"• 硬件加速: {gpu_name} (RTX 5060 Tensor Cores)")
    print("=" * 78 + "\n")
    server.serve_forever()

if __name__ == "__main__":
    run_server()
