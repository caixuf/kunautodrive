#!/usr/bin/env python3
"""长跑车道保持门禁 (L3 需求：正常车道保持下不压线, ~10min demo.sh soak)

动机：短跑（30-45s）评估只看得到起步 + 一次机动，抓不住"跑久了慢慢骑线/漂移"
这类低频退化——1Hz 极限环、横向积分漂移、曲率累积误差都要几分钟才显形。
本门禁起一次 ~10 分钟的 demo.sh，全程采样 /tmp/flow_topology.json，断言：

  Rule 1  权威车道级定位（localization/lane_match）覆盖率足够（否则判"看不见"）
  Rule 2  巡航帧（非机动、非近静止）车身不压线：越线帧占比 < 阈值
  Rule 3  巡航帧车心不越线（D2-06 原文判据）
  Rule 4  全程无碰撞、无 road departure、无 top-level FAIL 以外的严重项

判据与 demo_evaluator 的车道保持门禁同源（同阈值常量、同机动豁免口径），
复用它避免第二份实现。长跑的价值在"时间跨度"，不在"新判据"。

设计取舍：
  - 门禁自己生成一条**足够长**的直道场景（10min @ ~15m/s ≈ 9km，取 16km 留余量），
    不改仓库既有场景（既有场景 max_duration_s/路长都是为短跑调的）。
  - 默认 600s（10min），可用 --duration 覆盖（CI nightly 用 600，本地冒烟用 60）。
  - 采样 2Hz（0.5s），10min ≈ 1200 帧，内存无压力。

Usage:
  python3 ci/gates/long_run_lane_keep.py                 # 10min
  python3 ci/gates/long_run_lane_keep.py --duration 120  # 冒烟
  python3 ci/gates/long_run_lane_keep.py --workers 1 --json
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# 生成场景放**临时目录**，不放 scenarios/ —— 否则 scenario-file-gate
# (tools/scenarioctl.py validate) 会扫到未登记的生成文件，且容易被误提交。
GEN_SCENARIO = Path(os.environ.get("FLOW_LONG_RUN_SCENARIO",
                                   "/tmp/flow_long_run_lane_keep.json"))

# 与 ci/evaluators/demo_evaluator.py 同源阈值（改一处要同步，避免第二份真相）
LANE_KEEP_BODY_HALF_W_M = 0.9
LANE_KEEP_RATIO_FAIL = 0.30
LANE_KEEP_CONSEC_FAIL = 30
LANE_KEEP_MIN_COVERAGE = 0.50
LANE_MATCH_MIN_SAMPLES = 50


def _write_long_scenario() -> None:
    """生成一条 16km 单向 4 车道直道场景（长跑到不至于耗尽路）。"""
    length = 16000.0
    step = 100.0
    nodes = [[round(i * step, 3), 0.0, 0.0] for i in range(int(length / step) + 1)]
    scenario = {
        "name": "_long_run_lane_keep",
        "description": "长跑车道保持门禁自动生成：16km 单向 4 车道直道，无 NPC 干扰，"
                       "用于 ~10min soak 检测巡航期压线/漂移退化。",
        "random_seed": 137,
        "duration_s": 0,
        "lighting": "day", "weather": "clear", "visibility_m": 1000,
        "ego": {"x": 20.0, "y": -1.75, "heading": 0.0, "init_speed": 12.0,
                "target_speed": 15.0, "wheelbase": 2.7, "length": 4.6,
                "width": 2.0, "max_steer": 0.6},
        "road_network": {"edges": [{
            "id": 0, "type": "highway", "name": "long_straight",
            "length_m": length, "lanes": 4, "lane_width": 3.5,
            "speed_limit": 22.0, "oneway": True, "nodes": nodes,
        }]},
        "actors": [],
        "pass_criteria": {
            "no_collision": True,
            "max_duration_s": 1200.0,   # 容忍 20min，长跑门禁自己控时
            "min_avg_speed_mps": 5.0,
        },
    }
    GEN_SCENARIO.write_text(json.dumps(scenario, ensure_ascii=False, indent=2))


def _run_demo(duration: int, interval: float) -> list[dict]:
    """起 demo.sh 跑 duration 秒，按 interval 采样 topology，返回样本列表。

    注意：monitor 的 state_file 来自 pipeline.json 的 params（不是环境变量），
    故必须像 scenario_regression 一样 patch 一份 pipeline.json 指向本门禁的
    topology 路径，否则采到的是别人 / 默认路径的快照。
    """
    # 复用 scenario_regression 的 worker 工作区构造（单一真相，勿抄第二份）
    sys.path.insert(0, str(ROOT / "ci" / "evaluators"))
    import scenario_regression as sr  # noqa: E402

    workspace = ROOT / ".long_run_gate"
    _, worker_env = sr.prepare_worker_workspace(workspace, "lane_keep_soak")
    topo_path = Path(worker_env["FLOW_TOPOLOGY_FILE"])
    if topo_path.exists():
        topo_path.unlink()

    env = os.environ.copy()
    env.update(worker_env)
    env["FLOW_SKIP_SERVICES"] = "1"     # 长跑只评测，不要 dashboard/foxglove
    cmd = [str(ROOT / "scripts" / "demo.sh"), "--no-browser", "--skip-services",
           "--scenario", str(GEN_SCENARIO), str(duration)]
    proc = subprocess.Popen(cmd, cwd=ROOT, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1, start_new_session=True)

    def _kill(sig, _frm):
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError, OSError):
            pass
        signal.signal(sig, signal.SIG_DFL)
        os.kill(os.getpid(), sig)

    prev = (signal.signal(signal.SIGTERM, _kill), signal.signal(signal.SIGINT, _kill))
    samples: list[dict] = []
    started = time.monotonic()
    deadline = started + duration + 150.0   # 启动/收尾余量（长跑 wait budget 40s）
    first_seen = False
    t_start = time.monotonic()
    try:
        while proc.poll() is None and time.monotonic() < deadline:
            try:
                with topo_path.open(encoding="utf-8") as fh:
                    data = json.load(fh)
                if isinstance(data, dict):
                    # 采样时刻必须随样本记录：Rule5（横向目标跳变）要用它算
                    # 次/秒。漏记时 span=0 → rate 恒为 0 → 门禁永远绿
                    # （实测踩过一次，见 _lateral_chatter 的 span 断言）。
                    # 用 monitor 自带的 timestamp（仿真/墙钟均可）优先，
                    # 缺失才回退本进程单调钟相对时间。
                    ts = data.get("timestamp")
                    if not isinstance(ts, (int, float)):
                        ts = time.monotonic() - t_start
                    samples.append({"t_demo": float(ts), "metrics": data.get("metrics", {})})
                    first_seen = True
            except (FileNotFoundError, json.JSONDecodeError, OSError):
                pass
            time.sleep(interval)
    finally:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError, OSError):
            pass
        signal.signal(signal.SIGTERM, prev[0])
        signal.signal(signal.SIGINT, prev[1])
    if not first_seen:
        print("  ✗ 长跑未采到任何 topology 样本（monitor 未起或启动超时）", file=sys.stderr)
    return samples


def _lateral_chatter(samples: list[dict]) -> dict:
    """Rule5：横向目标跳变。判据本体复用 demo_evaluator.lateral_target_chatter。

    仓库铁律"同一判据不得有第二份实现"，所以这里只做 raw topology sample →
    evaluator series 的字段搬运，阈值/口径全在 evaluator 里。
    """
    sys.path.insert(0, str(ROOT / "ci" / "evaluators"))
    import demo_evaluator as de  # noqa: E402

    series: list[dict] = []
    for s in samples:
        m = s.get("metrics", {})
        ego = (m.get("scene", {}) or {}).get("ego", {})
        pd = m.get("planning_debug", {}) or {}
        beh = (m.get("behavior", {}).get("state") or "").upper()
        lane = m.get("scene", {}).get("lane", {}) or {}
        series.append({
            "t_demo": s.get("t_demo", s.get("t", 0.0)),
            "planning_target_offset": pd.get("target_lane_offset"),
            "speed": float(ego.get("speed", 0.0) or 0.0),
            "maneuver_active": any(t in beh for t in ("CHANGE", "OVERTAKE", "UTURN", "PARK")),
            "lane_width": float(lane.get("width", 3.5) or 3.5),
        })
    return de.lateral_target_chatter(series)


def _lane_keep_stats(samples: list[dict]) -> dict:
    """复用 demo_evaluator 的机动/近静止豁免口径，统计巡航帧压线。"""
    lm_valid = 0
    cruise = 0
    body_line = 0           # 车身压线帧
    center_cross = 0        # 车心越线帧
    max_consec = 0
    consec = 0
    worst_offset = 0.0
    collision = 0
    for s in samples:
        m = s.get("metrics", {})
        lm = m.get("lane_match", {})
        if lm.get("ok") and isinstance(lm.get("offset"), (int, float)):
            lm_valid += 1
        beh = (m.get("behavior", {}).get("state") or "")
        # 复用 evaluator 的机动判据：状态名含 CHANGE/OVERTAKE/UTURN/PARK
        maneuver = any(t in beh.upper() for t in ("CHANGE", "OVERTAKE", "UTURN", "PARK"))
        ego = (m.get("scene", {}) or {}).get("ego", {})
        speed = float(ego.get("speed", 0.0) or 0.0)
        if maneuver or speed <= 0.5:
            continue
        cruise += 1
        off = abs(float(lm.get("offset") or 0.0))
        worst_offset = max(worst_offset, off)
        lw = float(lm.get("lane_width") or 3.5)
        straddle = lw / 2.0 - LANE_KEEP_BODY_HALF_W_M
        if off > straddle:
            body_line += 1
            consec += 1
            max_consec = max(max_consec, consec)
        else:
            consec = 0
        if off > lw / 2.0 - 0.1:
            center_cross += 1
        if m.get("sim_collision", 0) or s.get("collisions"):
            collision += 1
    return {
        "samples": len(samples), "lane_match_valid": lm_valid, "cruise": cruise,
        "body_line": body_line, "center_cross": center_cross,
        "max_consec": max_consec, "worst_offset": worst_offset,
        "collision": collision,
    }


def run(duration: int, interval: float) -> tuple[list[str], dict]:
    _write_long_scenario()
    try:
        samples = _run_demo(duration, interval)
    finally:
        pass
    st = _lane_keep_stats(samples)
    failures: list[str] = []

    # Rule 5 判据复用 demo_evaluator 的纯函数（同口径，无第二份实现）。
    # 长跑是这类"目标持续阶跃"故障最该显形的场合。
    #
    # ⚠ 无法判定 ≠ 通过：insufficient_data（样本不足 / 缺时间戳 / 目标字段
    # 全程缺失）必须报出来，不能让它静默变成 PASS —— 那等于门禁永远绿。
    # 实测踩过：采样漏记时间戳时 span=0 → rate=0 → 对任何数据都 ok。
    chatter = _lateral_chatter(samples)
    st["target_chatter"] = chatter

    coverage = (st["lane_match_valid"] / st["samples"]) if st["samples"] else 0.0
    if st["samples"] < LANE_MATCH_MIN_SAMPLES:
        failures.append(f"Rule0 样本过少：{st['samples']} 帧（< {LANE_MATCH_MIN_SAMPLES}）——"
                        f"长跑没跑起来或 monitor 未写 topology")
        return failures, st
    if coverage < LANE_KEEP_MIN_COVERAGE:
        failures.append(f"Rule1 lane_match 覆盖率 {coverage:.0%} < {LANE_KEEP_MIN_COVERAGE:.0%}"
                        f"—— 权威车道定位不可用，长跑无意义")
        return failures, st

    if st["cruise"] == 0:
        failures.append("Rule2 无巡航帧可判（全程机动/静止？）")
        return failures, st
    body_ratio = st["body_line"] / st["cruise"]
    if st["max_consec"] >= LANE_KEEP_CONSEC_FAIL or body_ratio >= LANE_KEEP_RATIO_FAIL:
        failures.append(f"Rule2 长跑压线：车身越线 {st['body_line']} 帧 "
                        f"({body_ratio:.0%}, 连续 {st['max_consec']}, worst |off| "
                        f"{st['worst_offset']:.2f}m)")
    if st["center_cross"]:
        cross_ratio = st["center_cross"] / st["cruise"]
        if cross_ratio >= LANE_KEEP_RATIO_FAIL:
            failures.append(f"Rule3 长跑车心越线 {st['center_cross']} 帧 ({cross_ratio:.0%})")
    if st["collision"]:
        failures.append(f"Rule4 长跑发生碰撞 {st['collision']} 帧")
    if chatter["verdict"] == "fail":
        failures.append(
            f"Rule5 横向目标跳变：planning target_lane_offset 持续在车道中心之间"
            f"阶跃（{chatter['jumps']} 次 > 0.5×车道宽 / {chatter['span_s']:.0f}s "
            f"= {chatter['rate']:.2f}/s，幅度中位 {chatter['amp_median']:.2f}m）"
            f"—— 车会在两车道中心之间摇摆")
    elif chatter["verdict"] == "insufficient_data":
        failures.append(
            f"Rule5 无法判定（不等于通过）：横向目标跳变判据缺样本 —— "
            f"frames={chatter['frames']}, span_s={chatter['span_s']:.1f}, "
            f"jumps={chatter['jumps']}。目标字段缺失/采样无时间戳都会走到这里，"
            f"修好采样再看，别把这条当 PASS")
    return failures, st


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=int, default=600, help="soak 时长（秒），默认 600=10min")
    ap.add_argument("--interval", type=float, default=0.5, help="采样间隔（秒）")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    print(f"long_run_lane_keep: {args.duration}s soak (interval={args.interval}s)")
    started = time.time()
    failures, st = run(args.duration, args.interval)
    elapsed = time.time() - started
    if args.json:
        print(json.dumps({"failures": failures, "stats": st,
                          "elapsed_s": round(elapsed, 1)}, ensure_ascii=False, indent=2))
    else:
        print(f"  样本 {st['samples']} 帧 / lane_match 有效 {st['lane_match_valid']} / "
              f"巡航 {st['cruise']} / 压线 {st['body_line']} / 车心越线 {st['center_cross']} / "
              f"worst |off| {st['worst_offset']:.2f}m / 用时 {elapsed:.0f}s")
        if failures:
            print(f"\n✗ 长跑门禁 {len(failures)} 项违规：")
            for f in failures:
                print(f"  - {f}")
        else:
            print("✓ 长跑车道保持 OK：全程巡航不压线")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
