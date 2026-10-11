#!/usr/bin/env python3
"""故障注入 → 降级路径 门禁（L3-P2，方向六）。

P2 的放行条件是「**每类注入故障都有可观测的降级路径**」。本门禁是它的
**有牙实现**：对 tools/fault_degrade_sim.py 里登记的每一类故障，真起一条管线、
真注入该故障、然后从 metrics.degrade / metrics.driver_mode / metrics.topics
里读回**实跑观测值**，与故障矩阵的期望逐条比对。

为什么不能只在 Python 里算：`tools/fault_degrade_sim.py` 证明的是"表的语义
自洽"，证明不了"注入真的生效、降级真的走到那条路"。两者缺一不可 ——
前者防止表写错，后者防止 C 侧接线断掉（L3-P1 就栽在这：判据齐全但
`tor_manager` 读错字段名，MRM 从未真停）。

判据（每类故障）：
  Rule 1  注入确实生效：实跑里出现了该 topic 的规则（FLOW_FAULT_INJECT 回读）
  Rule 2  降级路径可观测：观测到的 level >= 期望 level，且 reason 命中期望集合
  Rule 3  替代落点（level=L0 的类）：driver_mode / topic 频率出现期望证据
  Rule 4  ODD 联动（若声明）：odd.violations 含期望项

用法：
  python3 ci/gates/fault_inject_gate.py                  # 全部故障类
  python3 ci/gates/fault_inject_gate.py --only localization_loss
  python3 ci/gates/fault_inject_gate.py --duration 20 --list
退出码 0 = 全绿；非 0 = 失败类数。
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
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "ci" / "evaluators"))


def load_matrix():
    """复用 tools/fault_degrade_sim.py 的矩阵（单一事实源，勿抄第二份）。"""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "fault_degrade_sim", ROOT / "tools" / "fault_degrade_sim.py")
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


def run_with_injection(inject_spec: str, scenario: str, duration: int,
                       interval: float, tag: str,
                       attempts: int = 3) -> tuple[list[dict], str]:
    """起一次 demo.sh（带 FLOW_FAULT_INJECT），采样 topology，返回 (样本, 错误)。

    ``no topology samples collected`` 是**既有**的启动竞态（monitor 首帧 vs 采样
    开始，ROADMAP_L3 记录 ~1/6 概率，与本门禁要测的故障路径无关）。对这类
    **基础设施**失败做有界重试，避免用一次启动抖动诬告一条降级路径。
    重试只针对"一个样本都没采到"；一旦采到样本，判据结果就是确定的，不再重试
    （否则会把真实的降级路径缺陷重试掉）。
    """
    last_err = ""
    for attempt in range(1, attempts + 1):
        samples, err = _run_once(inject_spec, scenario, duration, interval, tag)
        if samples:
            if attempt > 1:
                print(f"  (retry {attempt}/{attempts} succeeded — previous attempt hit "
                      f"the known startup race)", flush=True)
            return samples, ""
        last_err = err
        if attempt < attempts:
            print(f"  (attempt {attempt}/{attempts} failed: {err}; retrying — "
                  f"known monitor startup race)", flush=True)
            time.sleep(2.0)
    return [], f"{last_err} (after {attempts} attempts)"


def _run_once(inject_spec: str, scenario: str, duration: int,
              interval: float, tag: str) -> tuple[list[dict], str]:
    """单次尝试：起 demo.sh 采样，返回 (样本, 错误)。"""
    import scenario_regression as sr

    workspace = ROOT / f".fault_gate_{tag}"
    _, worker_env = sr.prepare_worker_workspace(workspace, f"fault_{tag}")
    topo_path = Path(worker_env["FLOW_TOPOLOGY_FILE"])
    if topo_path.exists():
        topo_path.unlink()

    env = os.environ.copy()
    env.update(worker_env)
    env["FLOW_SKIP_SERVICES"] = "1"
    env["FLOW_FAULT_INJECT"] = inject_spec

    cmd = [str(ROOT / "scripts" / "demo.sh"), "--no-browser", "--skip-services",
           "--scenario", scenario, str(duration)]
    try:
        proc = subprocess.Popen(cmd, cwd=ROOT, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, bufsize=1, start_new_session=True)
    except OSError as exc:
        return [], f"cannot launch demo.sh: {exc}"

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
    deadline = started + duration + 150.0
    try:
        while proc.poll() is None and time.monotonic() < deadline:
            try:
                with topo_path.open(encoding="utf-8") as fh:
                    data = json.load(fh)
                if isinstance(data, dict):
                    samples.append(data)
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

    if not samples:
        return [], "no topology samples collected (monitor never started?)"
    return samples, ""


def observe(samples: list[dict]) -> dict:
    """从样本序列汇总本类故障的观测值（取全程极值/并集）。"""
    max_level = 0
    reasons: set[str] = set()
    odd_violations: set[str] = set()
    driver_modes: set[str] = set()
    control_modes: set[str] = set()
    loc_reasons: set[str] = set()
    loc_trustworthy_seen: set[bool] = set()
    topic_freq_min: dict[str, float] = {}
    inject_enabled = False
    inject_dropped = 0

    for s in samples:
        m = s.get("metrics", {}) if isinstance(s, dict) else {}
        dg = m.get("degrade")
        if isinstance(dg, dict):
            lv = dg.get("level")
            if isinstance(lv, (int, float)):
                max_level = max(max_level, int(lv))
            rn = dg.get("reason_name")
            if isinstance(rn, str) and rn and rn != "none":
                reasons.add(rn)
        odd = m.get("odd")
        if isinstance(odd, dict):
            for v in odd.get("violations") or []:
                odd_violations.add(str(v))
        dm = m.get("driver_mode")
        if isinstance(dm, str) and dm:
            driver_modes.add(dm)
        cd = m.get("control_debug")
        if isinstance(cd, dict):
            cm = cd.get("mode")
            if isinstance(cm, str) and cm:
                control_modes.add(cm)
        lz = m.get("localization")
        if isinstance(lz, dict):
            for r in lz.get("loc_reasons") or []:
                loc_reasons.add(str(r))
            lt = lz.get("loc_trustworthy")
            if isinstance(lt, bool):
                loc_trustworthy_seen.add(lt)
        fi = m.get("fault_inject")
        if isinstance(fi, dict):
            if fi.get("enabled"):
                inject_enabled = True
            d = fi.get("dropped")
            if isinstance(d, (int, float)):
                inject_dropped = max(inject_dropped, int(d))
        # topic 频率：取全程**最小值**（注入生效 → 该 topic 频率会掉到 0）
        for t in m.get("topics") or []:
            if not isinstance(t, dict):
                continue
            name = t.get("topic")
            freq = t.get("freq")
            if isinstance(name, str) and isinstance(freq, (int, float)):
                prev = topic_freq_min.get(name)
                topic_freq_min[name] = float(freq) if prev is None else min(prev, float(freq))

    return {
        "max_level": max_level,
        "reasons": reasons,
        "odd_violations": odd_violations,
        "driver_modes": driver_modes,
        "control_modes": control_modes,
        "loc_reasons": loc_reasons,
        "loc_trustworthy_seen": loc_trustworthy_seen,
        "topic_freq_min": topic_freq_min,
        "inject_enabled": inject_enabled,
        "inject_dropped": inject_dropped,
    }


def topic_of(inject_spec: str) -> str:
    return inject_spec.split(":", 1)[0]


def check_class(entry: dict, obs: dict, matrix_mod) -> list[str]:
    """把一类故障的观测值与期望比对，返回失败描述列表。

    判据分两类，**必须先确认注入生效**再看降级路径 —— 否则"注入没生效"
    会被误读成"降级路径缺失"，把测试基础设施问题记成 L3 缺陷。
    """
    fails = []
    topic = topic_of(entry["inject"])
    via = set(entry.get("observable_via") or ())

    # ── Rule 1：注入确已生效（实测丢弃计数，非仅看配置）──
    if "inject_ack" in via:
        if not obs["inject_enabled"]:
            fails.append("injection ack missing: metrics.fault_inject absent "
                         "(monitor not exporting it, or injection not enabled)")
        elif obs["inject_dropped"] <= 0:
            fails.append("injection ineffective: metrics.fault_inject.dropped == 0 "
                         f"— rule for '{topic}' matched no publish")
    freq = obs["topic_freq_min"].get(topic)
    if "topic_freq" in via:
        # ⚠️ 只对**限时**注入有意义。永久注入时该 topic 因 pub_count 恒 0 会
        # 整个从 stats 表消失（注入点在 bus 之前，消息从未进 bus），此时
        # "缺席"本身就是注入生效的强证据，而不是"无法确认"。
        # 2026-10-11 实测：perception_loss 永久注入 → 'perception/obstacles'
        # 从 41 个 topic 里消失（干净跑时在）。
        if freq is None:
            if obs["inject_dropped"] <= 0:
                fails.append(f"topic '{topic}' absent from metrics.topics AND no drops "
                             f"recorded — cannot confirm the fault took effect")
        elif obs["inject_dropped"] > 0 and freq > 1.0:
            # 注入了却仍在发 → 规则窗口/持续时间与运行时长不匹配
            fails.append(f"injection ineffective: '{topic}' min freq {freq:.2f} Hz "
                         f"despite {obs['inject_dropped']} drops")

    # ── Rule 2：降级等级与原因码 ──
    if obs["max_level"] < entry["level"]:
        fails.append(f"degrade level {obs['max_level']} < expected L{entry['level']} "
                     f"(reason(s) seen: {sorted(obs['reasons']) or ['none']})")
    expect_reasons = set(entry.get("reason_names") or ())
    if expect_reasons and not (obs["reasons"] & expect_reasons):
        fails.append(f"degrade reason {sorted(obs['reasons']) or ['none']} "
                     f"not in expected {sorted(expect_reasons)}")

    # ── Rule 3：control/debug 落点（如声明）──
    want_mode = entry.get("control_mode")
    if want_mode and want_mode not in obs["control_modes"]:
        fails.append(f"control_mode '{want_mode}' not observed "
                     f"(seen: {sorted(obs['control_modes']) or ['none']})")

    # ── Rule 3b：定位完整性落点（如声明）──
    # 语义要点：单源丢失时 loc_trustworthy 必须**保持 true**（冗余生效），
    # 全源丢失才 false —— 断言两侧都要，否则"永远 trustworthy=false"也能过。
    if "loc_integrity" in via and not obs["loc_trustworthy_seen"]:
        fails.append("loc_integrity evidence missing: metrics.localization absent "
                     "(monitor not caching fusion/localization?)")
    want_loc = entry.get("loc_reasons")
    if want_loc:
        missing = [r for r in want_loc if r not in obs["loc_reasons"]]
        if missing:
            fails.append(f"loc_reasons missing {missing} "
                         f"(seen: {sorted(obs['loc_reasons']) or ['none']})")
    want_trust = entry.get("loc_trustworthy")
    if want_trust is not None and obs["loc_trustworthy_seen"]:
        if want_trust not in obs["loc_trustworthy_seen"]:
            fails.append(f"loc_trustworthy={want_trust} never observed "
                         f"(seen: {sorted(obs['loc_trustworthy_seen'])})")

    # ── Rule 4：ODD 联动（如声明）──
    want_v = entry.get("odd_violation")
    if want_v and want_v not in obs["odd_violations"]:
        fails.append(f"ODD violation '{want_v}' not observed "
                     f"(seen: {sorted(obs['odd_violations']) or ['none']})")

    # ── Rule 5：声明的落点必须至少有一个真的给回了证据 ──
    if not fails and not obs["inject_enabled"] and "inject_ack" not in via:
        fails.append("no observation surface produced evidence "
                     f"(via={sorted(via)})")
    return fails


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--duration", type=int, default=25,
                    help="每类故障的仿真时长（秒），默认 25")
    ap.add_argument("--interval", type=float, default=0.5, help="采样间隔")
    ap.add_argument("--scenario", default="scenarios/straight_road.json",
                    help="注入所基于的场景")
    ap.add_argument("--only", default=None, help="只跑某一类（按 id）")
    ap.add_argument("--list", action="store_true", help="列出故障类后退出")
    ap.add_argument("--json", action="store_true", help="以 JSON 输出结果")
    args = ap.parse_args()

    matrix = load_matrix()
    classes = list(matrix.FAULT_MATRIX)
    if args.only:
        classes = [e for e in classes if e["id"] == args.only]
        if not classes:
            print(f"no fault class matches --only {args.only!r}", file=sys.stderr)
            return 2

    if args.list:
        for e in classes:
            print(f"{e['id']:<24} {e['inject']}")
        return 0

    # 表自洽性先过（表错 → 下面的比对没有意义）
    table_fails = matrix.check_matrix()
    if table_fails:
        for f in table_fails:
            print(f"  FAIL [matrix] {f}")
        print(f"\n故障矩阵自身不合法（{len(table_fails)} 项），先修 tools/fault_degrade_sim.py")
        return len(table_fails)

    results = []
    total_fails = 0
    for e in classes:
        print(f"\n─── fault class '{e['id']}'  inject={e['inject']} ───", flush=True)
        samples, err = run_with_injection(e["inject"], args.scenario,
                                          args.duration, args.interval, e["id"])
        if err:
            fails = [err]
        else:
            obs = observe(samples)
            fails = check_class(e, obs, matrix)
            print(f"  observed: level={obs['max_level']} "
                  f"reasons={sorted(obs['reasons']) or ['none']} "
                  f"control_modes={sorted(obs['control_modes']) or ['none']} "
                  f"loc_reasons={sorted(obs['loc_reasons']) or ['none']} "
                  f"loc_trustworthy={sorted(obs['loc_trustworthy_seen']) or ['none']} "
                  f"injected_drops={obs['inject_dropped']}")
        total_fails += len(fails)
        for f in fails:
            print(f"  ✗ {f}")
        if not fails:
            print("  ✓ observable degrade path confirmed")
        results.append({"id": e["id"], "inject": e["inject"],
                        "failures": fails, "passed": not fails})

    print("\n================ Fault Injection Matrix ================")
    for r in results:
        print(f"  {'PASS' if r['passed'] else 'FAIL'}  {r['id']}")
    print("=======================================================")
    if args.json:
        print(json.dumps({"results": results, "failures": total_fails},
                         ensure_ascii=False, indent=2))
    print(f"\n{len(results) - sum(1 for r in results if not r['passed'])}/{len(results)} "
          f"fault classes have an observable degrade path")
    if total_fails:
        print(f"FAIL: {total_fails} assertion(s) violated")
    return total_fails


if __name__ == "__main__":
    sys.exit(main())
