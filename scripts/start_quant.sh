#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PORT="${1:-8900}"
AI_PORT="${2:-8901}"
BIN="$ROOT/build/bin/kun_quant_server"
STATIC_DIR="$ROOT/tools/kunboard"

if [ ! -f "$BIN" ]; then
    echo "[!] 未找到 C++ 服务端二进制: $BIN"
    echo "    正在执行快速编译..."
    cmake --build "$ROOT/build" --target kun_quant_server -j"$(nproc)"
fi

echo "======================================================="
echo "   鲲量化 (KunQuant) 生产级全栈服务启动脚本"
echo "   前端/REST 端口: http://localhost:$PORT"
echo "   AI 投研微服务:  http://localhost:$AI_PORT"
echo "   静态前端目录:   $STATIC_DIR"
echo "======================================================="

AI_PID=""
SERVER_PID=""

cleanup() {
    echo ""
    echo "[!] 正在停止 KunQuant 全栈服务..."
    if [ -n "$SERVER_PID" ]; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
    fi
    if [ -n "$AI_PID" ]; then
        kill -TERM "$AI_PID" 2>/dev/null || true
    fi
    sleep 0.5
    if [ -n "$SERVER_PID" ]; then
        kill -9 "$SERVER_PID" 2>/dev/null || true
    fi
    if [ -n "$AI_PID" ]; then
        kill -9 "$AI_PID" 2>/dev/null || true
    fi
    echo "[✓] 服务已全部安全停止。"
}
trap cleanup EXIT INT TERM

export PYTHONPATH="$ROOT:${PYTHONPATH:-}"

# 1. 启动 Python AI 投研与自主诊断守护服务 (8901)
echo "[1/2] 正在启动 Python AI 投研后台微服务 (端口: $AI_PORT)..."
python3 -m ai_service.server "$AI_PORT" &
AI_PID=$!
sleep 0.5

# 2. 启动 C++ 原生量化引擎与 Web 服务 (8900)
echo "[2/2] 正在启动 C++ 原生量化引擎 (端口: $PORT)..."
"$BIN" --port "$PORT" --ai-port "$AI_PORT" --static-dir "$STATIC_DIR" &
SERVER_PID=$!

echo ""
echo "======================================================="
echo "  ✓ KunQuant 模拟实盘系统已成功启动！"
echo "  ✓ 浏览器访问: http://localhost:$PORT"
echo "  ✓ 按 Ctrl+C 即可安全退出"
echo "======================================================="

wait "$SERVER_PID"
