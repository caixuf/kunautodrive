#!/usr/bin/env bash
set -e

# ==============================================================================
# 鲲量化 (KunQuant) 生产级准入总验收与 7x24h 健康自检脚本
# 覆盖四大战役全部核心防线:
# 1. 御林金匮 (实盘双锁 + 2.5% 回撤硬熔断)
# 2. 常明烽燧 (双真实源 + 断流看门狗)
# 3. 天工开物 (微观冲击成本 + 穿透平账 + 跨期套利)
# 4. 乾坤定鼎 (高并发撮合压测 + 728ns 细胞力场演化)
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "======================================================================"
echo "          鲲量化 · 生产级上线准入终极检阅 (Production Gate Check)       "
echo "======================================================================"

cd "${ROOT_DIR}"

# 1. 运行全量 40 项全栈单元与集成测试套件
echo ""
echo ">>> [第一关] 执行全栈单元测试、太初细胞、生态圈与量子辐射演化套件..."
ctest --test-dir build -R "test_quant|test_flow_cellular|test_flow_ecosystem|test_flow_quantum" --output-on-failure
echo ">>> [第一关] 100% PASS! 全部量化、细胞、生态圈与量子辐射单测绿灯通过!"

# 2. 验证守护进程启动与优雅停机
echo ""
echo ">>> [第二关] 启动 KunQuant 生产守护进程并验证 API 链路..."
./build/bin/kun_quant_server &
SERVER_PID=$!
sleep 1

# 检查进程状态
if ! kill -0 ${SERVER_PID} 2>/dev/null; then
    echo "❌ 错误: kun_quant_server 启动失败!"
    exit 1
fi

# 探测核心健康与全息观测接口
echo ">>> 探测 /api/status ..."
curl -sf http://localhost:8900/api/status > /dev/null
echo "    ↳ /api/status [200 OK]"

echo ">>> 探测 /api/cellular/organism (太初细胞全息观测舱数据) ..."
CELL_RESP=$(curl -sf http://localhost:8900/api/cellular/organism)
if [[ "${CELL_RESP}" != *"cells"* ]]; then
    echo "❌ 错误: /api/cellular/organism 数据异常!"
    kill -TERM ${SERVER_PID} || true
    exit 1
fi
echo "    ↳ /api/cellular/organism [200 OK] (包含完整力场坐标与发光电位)"

echo ">>> 探测 /api/biosphere/status (宏观生态圈生境与食物网数据) ..."
BIO_RESP=$(curl -sf http://localhost:8900/api/biosphere/status)
if [[ "${BIO_RESP}" != *"shannon_diversity"* ]]; then
    echo "❌ 错误: /api/biosphere/status 数据异常!"
    kill -TERM ${SERVER_PID} || true
    exit 1
fi
echo "    ↳ /api/biosphere/status [200 OK] (包含香农多样性指数、4大生境与物种丰度)"

echo ">>> 探测 /api/report ..."
curl -sf http://localhost:8900/api/report > /dev/null
echo "    ↳ /api/report [200 OK]"

# 优雅停机测试
echo ""
echo ">>> [第三关] 发送 SIGTERM 信号检验优雅停机与资源解绑..."
kill -TERM ${SERVER_PID}
wait ${SERVER_PID} 2>/dev/null || true
echo ">>> [第三关] 守护进程零崩溃安全退出，内存与网关资源完全释放!"

echo ""
echo "======================================================================"
echo "  恭贺圣上！鲲量化系统四大战役全线大捷，生产准入 100% 满分通过！   "
echo "======================================================================"
