#!/usr/bin/env python3
"""monitor_server HTTP 响应完整性守卫（`monitor_http_integrity`）。

守的是「**响应体自洽**」这条不变量，而不是某个具体端点：

  1. 客户端实收字节数 == 头部 `Content-Length`；
  2. `/api/*` 的 body 必须是**合法 JSON**；
  3. **大 body + 慢读/停读**时也不许截断；
  4. keep-alive 同连接连发两次，两份 body 各自完整（第二份不能混进第一份的尾巴）；
  5. body 里不得出现**陈旧 IPC 共享内存段的毒负载**（见下）。

两条独立缺陷，各自对应上面一组断言：

**（甲）发送路径丢 `write()` 返回值** → 第 1/3/4 条守。
monitor_server 给每个客户端 socket 设了 `SO_SNDTIMEO=5s`，而带发送超时的阻塞
`write()` 在发送缓冲填满、等对端窗口超时后会**返回"已写字节数"**（partial）——
旧代码把它丢掉，剩余字节永不发出：头部仍写着完整 `Content-Length`，客户端于是把
**下一个响应**的字节读成这次 body 的尾巴，表现为「合法 JSON 前缀 + 垃圾尾巴 +
解析失败」。SSE 路径一直走 `safe_write`，HTTP 响应路径曾漏掉。
触发条件：body 必须大于内核发送缓冲（本机约 1.8MB，TCP 自动调优），且客户端在
服务端写 body 期间**不读** —— 仓库里最大的静态资源 three.module.js（1.27MB）
装在缓冲里绰绰有余，用它**测不出**（2026-10-24 实测：旧代码也完整送达），
故本用例起临时 doc root 放一个自造的 8MB 不可压文件。

**（乙）陈旧 IPC 共享内存段被当成 live dashboard JSON** → 第 2/5 条守。
`tests/test_bridges.c` 往 dashboard 通道发布的 10000 字节 `ABCDEFG…` 字母负载，
在测试进程退出后仍留在 `/dev/shm/flow_dashboard_shm`（它只 stop 订阅端、不 stop
发布端）；此后启动的 flowmond 连上这段陈旧段、把它当数据缓存 →
`/api/topology` 变成 `{"source":"live",…,"age_sec":2.0,BCDEFGHIJKLMNOP…`（10044
字节，解析失败，首字符 'A' 被缓存拼接逻辑当成 JSON 的 '{' 吃掉）。管线被 kill /
进程崩溃同样留段，所以这是消费侧必须自己守的边界。
本用例**自己跑 `test_bridges` 播下这个前提**（不依赖"上一个测试刚好跑过"的顺序运气）。

本测试**不需要跑管线**：flowmond 单独监听即可 —— 那恰好是原现象的复现条件
（管线已停、只剩缓存/兜底路径）。

运行：`ctest -R monitor_http_integrity`（或直接 `python3 tests/test_monitor_http_integrity.py`）。
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLOWMOND = ROOT / "build" / "bin" / "flowmond"
REAL_INDEX = ROOT / "tools" / "flowboard" / "index.html"
REAL_SMALL_ASSET = ROOT / "tools" / "flowboard" / "js" / "app.js"

BIG_BIN_BYTES = 8 * 1024 * 1024      # > 内核发送缓冲自动调优上限
STALL_SECONDS = 8.0                  # > 服务端 SO_SNDTIMEO 5s
SNDTIMEO_SECONDS = 5.0


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def _env_without_ld_library_path(topology_file: Path | None = None) -> dict:
    """IDE 终端注入的旧 libstdc++ 目录会让二进制启动即 `version not found`。

    topology_file 非空时改指隔离路径：本用例只验证 stale-SHM 防护，不该被
    上一次 demo/soak 留在默认 /tmp/flow_topology.json 的大快照污染
    （>128KB 时文件桥接会截断，产生与本用例无关的 JSON 坏体）。"""
    env = dict(os.environ)
    env.pop("LD_LIBRARY_PATH", None)
    if topology_file is not None:
        env["FLOWENGINE_STATE_FILE"] = str(topology_file)
        env["FLOW_TOPOLOGY_FILE"] = str(topology_file)
    return env


def _read_response(sock: socket.socket, timeout: float) -> tuple[int, bytes]:
    """读一个完整 HTTP 响应，返回 (Content-Length, body)。对端提前关闭即停。"""
    sock.settimeout(timeout)
    buf = b""
    while b"\r\n\r\n" not in buf:
        piece = sock.recv(4096)
        if not piece:
            break
        buf += piece
    header, _, body = buf.partition(b"\r\n\r\n")
    declared = -1
    for line in header.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            declared = int(line.split(b":", 1)[1])
    while declared < 0 or len(body) < declared:
        try:
            more = sock.recv(65536)
        except socket.timeout:
            break
        if not more:
            break
        body += more
    return declared, body


def _get(port: int, path: str, timeout: float = 15.0) -> tuple[int, bytes]:
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        declared = resp.headers.get("Content-Length")
        body = resp.read()
    return (int(declared) if declared is not None else -1), body


def _get_big_while_stalled(port: int, path: str) -> tuple[int, int, float]:
    """停读若干秒再收完整响应 —— 逼出服务端的 partial write。

    返回 (Content-Length, 实收字节, 耗时秒)。旧实现下实收 < 声明（剩余字节
    永不发出，随后服务端 keep-alive 超时关连接）。
    """
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    t0 = time.time()
    try:
        s.connect(("127.0.0.1", port))
        s.sendall(f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  f"Connection: close\r\n\r\n".encode())
        time.sleep(STALL_SECONDS)      # ← 关键：服务端 write 在此期间只能写进缓冲
        declared, body = _read_response(s, timeout=STALL_SECONDS + 25.0)
        return declared, len(body), time.time() - t0
    finally:
        s.close()


def _plant_stale_dashboard_shm() -> bool:
    """用项目自带的 publisher 播下「陈旧 IPC 共享内存段」这个复现前提。

    `tests/test_bridges.c` 会往 dashboard 通道发布 10000 字节 `ABCDEFG…` 字母负载
    （用来验证分块重组），而它**只 stop 订阅端、不 stop 发布端** → 段留在
    /dev/shm/flow_dashboard_shm。此后启动的 flowmond 会把这段陈旧 SHM 读成
    「一帧新 dashboard 快照」（2026-10-24 实测：/api/topology 变成
    `{"source":"live",…,"age_sec":2.0,BCDEFGHIJKLMNOP…`，10044 字节，解析失败）。

    本用例据此**自己制造复现前提**，不依赖"上一个测试刚好跑过"这种顺序运气。
    """
    bridges = ROOT / "build" / "bin" / "test_bridges"
    if not bridges.exists():
        return False
    try:
        subprocess.run([str(bridges)], cwd=str(ROOT), env=_env_without_ld_library_path(),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
        return True
    except Exception:  # noqa: BLE001
        return False


def _alphabet_run(body: bytes) -> int:
    """body 里最长的一段「连续 26 字母循环」长度（毒负载的特征指纹）。"""
    alphabet = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    best = run = 0
    for i in range(len(body)):
        if body[i:i + 1] == alphabet[run % 26:run % 26 + 1]:
            run += 1
            best = max(best, run)
        else:
            run = 0
    return best


def _check_api_body(path: str, declared: int, body: bytes, failures: list[str]) -> None:
    """长度自洽 + 合法 JSON + 没在替陈旧 SHM 的毒负载背书。

    信封字段（source/stale/age_sec）只在**走缓存路径**时要求 —— 兜底路径
    （本地总线视图，body 以 `{"self":"flowmond"` 开头）本来就不带这三个字段。
    """
    if len(body) != declared:
        failures.append(f"{path}: 实收 {len(body)} != Content-Length {declared}")
    run = _alphabet_run(body)
    if run >= 64:
        failures.append(
            f"{path}: body 里出现 {run} 字节连续 26 字母循环 —— 这是陈旧 IPC SHM 的"
            f"毒负载被当成 live dashboard JSON 下发了（头 80 字节: {body[:80]!r}）")
    try:
        payload = json.loads(body)
    except Exception as exc:  # noqa: BLE001
        failures.append(f"{path}: JSON 解析失败: {exc}（头 80 字节: {body[:80]!r}）")
        return
    if isinstance(payload, dict) and "source" in payload:
        for key in ("stale", "age_sec"):
            if key not in payload:
                failures.append(f"{path}: 走缓存路径但缺少信封字段 '{key}'")


def _wait_ready(port: int, timeout: float = 20.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            _get(port, "/api/topology", timeout=2.0)
            return True
        except Exception:
            time.sleep(0.25)
    return False


def _make_doc_root(tmp: Path) -> Path:
    """临时 doc root：<tmp>/tools/flowboard/index.html + js/app.js + 8MB .bin。

    `/tools/X` 会解析到 `dirname(html_path)` 去掉 `flowboard` 后的目录
    （monitor_server.c 的 /tools/ 路由），所以这样摆就够。
    """
    board = tmp / "tools" / "flowboard"
    (board / "js").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(REAL_INDEX, board / "index.html")
    if REAL_SMALL_ASSET.exists():
        shutil.copyfile(REAL_SMALL_ASSET, board / "js" / "app.js")
    # 不可压：os.urandom 保证 gzip 收不下去（何况 .bin 本来就不走 gzip）
    (tmp / "tools" / "big_incompressible.bin").write_bytes(os.urandom(BIG_BIN_BYTES))
    return board / "index.html"


def main() -> int:
    if not FLOWMOND.exists():
        print(f"SKIP: {FLOWMOND} 不存在（先构建）")
        return 0

    port = _free_port()
    failures: list[str] = []
    tmp = Path(tempfile.mkdtemp(prefix="monitor_http_integrity_"))
    log = tmp / "flowmond.log"
    try:
        # 0) 先播下复现前提：陈旧 IPC 共享内存段（内含 test_bridges 的字母负载）。
        #    flowmond 启动后会连上它 —— 这是"管线被 kill / 进程崩溃后残留"的忠实模型。
        planted = _plant_stale_dashboard_shm()
        print(f"  陈旧 SHM 前提：{'已播下（test_bridges 发布端不清理段）' if planted else '未播下（test_bridges 未构建，跳过毒负载覆盖）'}")

        html_path = _make_doc_root(tmp)
        proc = subprocess.Popen(
            [str(FLOWMOND), "--port", str(port), "--html-path", str(html_path)],
            cwd=str(ROOT),
            env=_env_without_ld_library_path(tmp / "isolated_topology.json"),
            stdout=log.open("w"), stderr=subprocess.STDOUT,
        )
        try:
            if not _wait_ready(port):
                print(f"FAIL: flowmond 起不来（{log}）")
                return 1

            # 1) 报告现象的那个端点 + 同类端点：长度自洽 + 合法 JSON + 不得替毒负载背书
            for path in ("/api/topology", "/api/topics"):
                declared, body = _get(port, path)
                _check_api_body(path, declared, body, failures)

            # 2) 普通静态资源要完整
            declared, body = _get(port, "/tools/flowboard/js/app.js")
            if len(body) != declared:
                failures.append(
                    f"/tools/flowboard/js/app.js: 实收 {len(body)} != Content-Length {declared}")

            # 3) 大 body + 停读：不许截断（本条是 partial write 的守卫）
            declared, received, elapsed = _get_big_while_stalled(
                port, "/tools/big_incompressible.bin")
            if declared < 0:
                failures.append("big_incompressible.bin: 响应没有 Content-Length")
            elif received != declared:
                failures.append(
                    f"big_incompressible.bin: 停读 {STALL_SECONDS:.0f}s (>SO_SNDTIMEO "
                    f"{SNDTIMEO_SECONDS:.0f}s) 下被截断 —— 实收 {received} != 声明 {declared}"
                    f"（丢 {declared - received} 字节；发送路径未处理 partial write）")
            else:
                print(f"  big body 停读用例：{received} 字节完整，耗时 {elapsed:.1f}s")

            # 4) keep-alive 同连接两次：两份 body 必须各自完整、都合法 JSON
            with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
                for i in (1, 2):
                    sock.sendall(b"GET /api/topology HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
                    declared, body = _read_response(sock, timeout=15.0)
                    if declared < 0 or len(body) != declared:
                        failures.append(
                            f"keep-alive 第 {i} 次: 实收 {len(body)} != Content-Length {declared}")
                        break
                    _check_api_body(f"keep-alive 第 {i} 次", declared, body, failures)
                    if any(f"第 {i} 次" in f for f in failures):
                        break
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print("monitor_http_integrity 失败：")
        for f in failures:
            print("  ✗", f)
        return 1
    print("monitor_http_integrity OK：长度自洽 + JSON 合法（不替陈旧 SHM 毒负载背书）"
          " + 大 body 停读不截断 + keep-alive 复用安全")
    return 0


if __name__ == "__main__":
    sys.exit(main())