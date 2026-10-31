#!/usr/bin/env python3
"""L3 EDR 门禁（L3-P2 方向六）：**真起管线、真写 PEM、读懂事件**。

P2 要求「EDR：在现有 PEM 流上增加 L3 法规事件：激活/退出、TOR、MRM、碰撞前后
窗口，复用 CRC / fsync / 轮转」。本门禁是它的有牙实现，断言：

  Rule 1  声明的事件分类可被 C 侧观测（tools/l3_edr_sim.py 的 EDR_TAXONOMY
          与 include/l3_edr.h 一致，先过分类自洽）
  Rule 2  PEM 里**真的**写出了 l3_* 事件（不是"代码里有 emit 调用"就算）
  Rule 3  L3 场景（ODD 退出→TOR 超时→MRM）必须集齐 activation / tor / mrm /
          odd_exit 四类 —— 这正是该场景的法规取证要求
  Rule 4  所有 l3_* 事件都是 critical（走 fsync，掉电不可丢）

为什么必须真跑：EDR 的实现横跨 monitor 的 embedded 模式分支与 PEM 写出路径，
"代码里有 emit" 与 "PEM 里有记录" 之间隔着好几层（本轮就实测到：EDR 源订阅
恰好被关在 `if (!g.embedded_mode)` 里，写 PEM 的模式收不到要记的事件）。

CI flake（2026-10-31 schedule run 41183249548）：pipeline exited 124、work 目录
无 PEM。门禁对 flow_launcher 的 Python timeout=duration+120（28+120≈148s）与
步骤墙钟吻合 —— 慢 runner / 启动挂起被 timeout 杀掉时 PEM 尚未 flush。
本轮 harden（仅门禁/CI 接线，不改产品刀）：
  - 拉大 timeout 预算（duration + PIPELINE_TIMEOUT_MARGIN_S）
  - 管线结束后确定性轮询 PEM 落盘（flush 竞态）
  - 对 timeout/空 PEM/缺 l3_* 做一次有界自动重试（对齐 fault_inject / #118）
  - 显式保证 FLOWENGINE_TEMP_DIR + monitor production PEM 模式

用法：
  python3 ci/gates/l3_edr_gate.py                    # 默认场景
  python3 ci/gates/l3_edr_gate.py --scenario scenarios/l3_odd_exit_timeout_mrm.json
  python3 ci/gates/l3_edr_gate.py --duration 30
  python3 ci/gates/l3_edr_gate.py --self-test        # 无管线自检
退出码 0 = 全绿；非 0 = 失败断言数。
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PEM_DUMP = ROOT / "tools" / "pem_dump.py"
LAUNCHER = ROOT / "build" / "bin" / "flow_launcher"
DEFAULT_SCENARIO = "scenarios/l3_odd_exit_timeout_mrm.json"

# L3 场景应集齐的事件（法规取证：激活/退出、TOR、MRM、ODD 退出）
REQUIRED_FOR_L3_SCENARIO = ("l3_activation", "l3_tor", "l3_mrm", "l3_odd_exit")

# 慢 runner / 关机 flush 预算。schedule flake 实测 duration+120 恰被打死 → 拉宽。
PIPELINE_TIMEOUT_MARGIN_S = 180
# 管线 rc=0 后等 PEM 落盘的确定性轮询窗口（秒）
PEM_APPEAR_WAIT_S = 8.0
PEM_APPEAR_POLL_S = 0.25
# timeout / 空 PEM / 缺 l3_* 时的自动重试次数（含首次）
DEFAULT_ATTEMPTS = 2


def load_taxonomy():
    """复用 tools/l3_edr_sim.py 的分类（单一事实源，勿抄第二份）。"""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "l3_edr_sim", ROOT / "tools" / "l3_edr_sim.py")
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


def build_production_pipeline(scenario: str, work: Path) -> Path:
    """基于 config/pipeline.json 造一份"带 L3 节点 + 写 PEM"的临时管线。

    为什么不能直接用某个现成配置：EDR 事件源自 odd_monitor / tor_manager（只在
    仿真编排里有），而 PEM 写出只在 `monitor.params.mode == "production"` 时开。
    两者此前从未同时成立，所以 l3_* 事件一条都记不下来。本门禁显式把这个组合
    造出来 —— 这也正是"EDR 要复用现有 PEM 流"的落地方式。

    production 模式在 monitor 侧等价 embedded_mode（见 monitor_node.c），并已
    保证 odd/state + tor/state 在两种模式下都订阅（EDR 源接线）。
    """
    src = json.loads((ROOT / "config" / "pipeline.json").read_text(encoding="utf-8"))
    monitor_patched = False
    for p in src.get("processes", []):
        params = p.get("params")
        if not isinstance(params, str):
            continue
        try:
            pr = json.loads(params)
        except json.JSONDecodeError:
            continue
        if "scenario_file" in pr:
            pr["scenario_file"] = scenario
        if p.get("name") == "monitor":
            pr["mode"] = "production"          # 打开 PEM 写出（= embedded_mode）
            pr.setdefault("rotate_sec", 300)
            pr.setdefault("rotate_mb", 100)
            pr.setdefault("retain_segments", 96)
            pr.setdefault("retain_mb", 1024)
            monitor_patched = True
        p["params"] = json.dumps(pr)
    if not monitor_patched:
        raise RuntimeError(
            "build_production_pipeline: no monitor process with JSON params "
            "— cannot force production PEM mode")
    out = work / "pipeline_edr.json"
    out.write_text(json.dumps(src, indent=2), encoding="utf-8")
    return out


def _pipeline_env(work: Path) -> dict:
    """FLOWENGINE_TEMP_DIR 决定 PEM 路径（不是 TMPDIR）—— 见 platform_paths.h。"""
    env = os.environ.copy()
    env["FLOWENGINE_TEMP_DIR"] = str(work)
    env["FLOW_LOG_DIR"] = str(work / "logs")
    env["FLOW_TOPOLOGY_FILE"] = str(work / "topology.json")
    return env


def _kill_process_group(proc: subprocess.Popen) -> None:
    """Timeout / 重试前清掉 launcher 子树，避免僵尸占盘/占端口。"""
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError, OSError):
        try:
            proc.terminate()
        except OSError:
            pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError, OSError):
            try:
                proc.kill()
            except OSError:
                pass
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pass


def run_pipeline(pipeline: Path, duration: int, work: Path,
                 timeout_margin: int = PIPELINE_TIMEOUT_MARGIN_S) -> tuple[int, Path]:
    """起一次管线；PEM 写到 work 目录（FLOWENGINE_TEMP_DIR=work）。

    用 start_new_session 方便 timeout 时 killpg；timeout=duration+margin，
    避免慢 schedule runner 在 PEM flush 前被 Python 打死（exit 124）。
    """
    env = _pipeline_env(work)
    budget = duration + max(30, int(timeout_margin))
    try:
        proc = subprocess.Popen(
            [str(LAUNCHER), str(pipeline), "--duration", str(duration)],
            cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
    except OSError as exc:
        print(f"  (launcher spawn failed: {exc})", flush=True)
        return 127, work
    try:
        try:
            proc.communicate(timeout=budget)
        except subprocess.TimeoutExpired:
            _kill_process_group(proc)
            return 124, work
        return (proc.returncode if proc.returncode is not None else 124), work
    finally:
        if proc.poll() is None:
            _kill_process_group(proc)


def wait_for_pem(work: Path, wait_s: float = PEM_APPEAR_WAIT_S,
                 poll_s: float = PEM_APPEAR_POLL_S) -> list[Path]:
    """管线结束后确定性等 PEM 落盘（fsync/rotate 竞态）。"""
    deadline = time.monotonic() + max(0.0, wait_s)
    while True:
        pems = sorted(work.glob("kunautodrive_pem_*.pem"))
        if pems:
            return pems
        if time.monotonic() >= deadline:
            return []
        time.sleep(poll_s)


def collect_events(work: Path, *, wait_s: float = PEM_APPEAR_WAIT_S) -> tuple[dict, list, str]:
    """解析 work 目录下所有 PEM 的 event 记录。返回 (计数, 原始记录, 错误)。"""
    pems = wait_for_pem(work, wait_s=wait_s)
    if not pems:
        return {}, [], (
            f"no PEM written under {work} "
            f"(PEM production mode off? FLOWENGINE_TEMP_DIR={work})")
    counts: dict[str, int] = {}
    raw: list = []
    err = ""
    for pem in pems:
        try:
            out = subprocess.run(
                [sys.executable, str(PEM_DUMP), "--jsonl", str(pem)],
                capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            err = f"pem_dump timed out on {pem.name}"
            continue
        if out.returncode != 0:
            err = f"pem_dump failed on {pem.name}: {out.stderr.strip()[:200]}"
            continue
        for line in out.stdout.splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("type") != "event":
                continue
            name = str(rec.get("name", ""))
            counts[name] = counts.get(name, 0) + 1
            raw.append(rec)
    return counts, raw, err


def _attempt_infra_failure(rc: int, counts: dict, err: str, scenario: str) -> str | None:
    """可重试的基础设施失败（timeout / 空 PEM / 缺 l3_*）；产品断言失败返回 None。"""
    if rc == 124:
        return f"pipeline exited 124 (timeout; expected 0)"
    if err and "no PEM written" in err:
        return err
    if not any(n.startswith("l3_") for n in counts):
        return (
            "no l3_* event in PEM — EDR not actually recorded "
            f"(PEM event names seen: {sorted(counts) or ['none']})")
    if "l3_odd_exit" in scenario or "l3_" in scenario:
        missing = [n for n in REQUIRED_FOR_L3_SCENARIO if counts.get(n, 0) == 0]
        if missing:
            return (
                f"L3 scenario must record {list(REQUIRED_FOR_L3_SCENARIO)} "
                f"but missing {missing} (got: {counts})")
    return None


def _fresh_work(keep_prefix: str | None = None) -> Path:
    return Path(tempfile.mkdtemp(prefix=keep_prefix or "l3_edr_gate_"))


def run_self_test() -> int:
    """无管线自检：production 接线、PEM 轮询、infra 失败分类、timeout 预算。"""
    failures: list[str] = []

    work = _fresh_work("l3_edr_selftest_")
    try:
        pipe = build_production_pipeline(DEFAULT_SCENARIO, work)
        cfg = json.loads(pipe.read_text(encoding="utf-8"))
        mon = next(p for p in cfg["processes"] if p.get("name") == "monitor")
        pr = json.loads(mon["params"])
        if pr.get("mode") != "production":
            failures.append(f"monitor.mode want production got {pr.get('mode')}")
        env = _pipeline_env(work)
        if env.get("FLOWENGINE_TEMP_DIR") != str(work):
            failures.append("FLOWENGINE_TEMP_DIR not wired to work dir")
        if PIPELINE_TIMEOUT_MARGIN_S < 150:
            failures.append(
                f"PIPELINE_TIMEOUT_MARGIN_S={PIPELINE_TIMEOUT_MARGIN_S} "
                f"too small for schedule flake (need >=150; was 120)")
        if wait_for_pem(work, wait_s=0.3, poll_s=0.05):
            failures.append("wait_for_pem should be empty initially")
        fake = work / "kunautodrive_pem_selftest_000.pem"
        fake.write_bytes(b"pem")
        found = wait_for_pem(work, wait_s=1.0, poll_s=0.05)
        if not found or found[0].name != fake.name:
            failures.append(f"wait_for_pem did not see fake PEM: {found}")
        tip = _attempt_infra_failure(124, {}, "", DEFAULT_SCENARIO)
        if tip is None or "124" not in tip:
            failures.append(f"rc=124 should be retryable infra, got {tip!r}")
        tip2 = _attempt_infra_failure(
            0, {}, f"no PEM written under {work}", DEFAULT_SCENARIO)
        if tip2 is None or "no PEM" not in tip2:
            failures.append(f"empty PEM should be retryable, got {tip2!r}")
        tip3 = _attempt_infra_failure(
            0, {"degrade_transition": 1}, "", DEFAULT_SCENARIO)
        if tip3 is None or "no l3_" not in tip3:
            failures.append(f"no l3_* should be retryable, got {tip3!r}")
        good = {
            "l3_activation": 1, "l3_tor": 1, "l3_mrm": 1, "l3_odd_exit": 1,
        }
        tip4 = _attempt_infra_failure(0, good, "", DEFAULT_SCENARIO)
        if tip4 is not None:
            failures.append(f"complete l3 set must not be infra-retry: {tip4!r}")
        tax = load_taxonomy()
        tax_fails = tax.check_taxonomy()
        if tax_fails:
            failures.extend(f"[taxonomy] {f}" for f in tax_fails)
    finally:
        subprocess.run(["rm", "-rf", str(work)], check=False)

    if failures:
        print("SELF-TEST FAIL:")
        for f in failures:
            print(f"  ✗ {f}")
        return len(failures)
    print(
        "✓ self-test: production PEM wiring + PEM wait + "
        "retry classifier + timeout margin")
    return 0


def _monitor_mode_from_pipeline(pipeline: Path) -> str:
    cfg = json.loads(pipeline.read_text(encoding="utf-8"))
    for p in cfg.get("processes", []):
        if p.get("name") != "monitor":
            continue
        params = p.get("params")
        if isinstance(params, str):
            return str(json.loads(params).get("mode", ""))
    return ""


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", default=DEFAULT_SCENARIO)
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--keep", action="store_true", help="保留临时工作目录")
    ap.add_argument(
        "--attempts", type=int, default=DEFAULT_ATTEMPTS,
        help="timeout/空PEM/缺l3_* 时的尝试次数（含首次，默认 2）")
    ap.add_argument(
        "--timeout-margin", type=int, default=PIPELINE_TIMEOUT_MARGIN_S,
        help="flow_launcher Python timeout = duration + margin")
    ap.add_argument(
        "--self-test", action="store_true",
        help="无管线自检（wiring / PEM wait / retry 分类）")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    failures: list[str] = []

    # ── Rule 1：分类自洽（Python 侧单一事实源）──
    tax = load_taxonomy()
    for f in tax.check_taxonomy():
        failures.append(f"[taxonomy] {f}")
    declared = {e["event"] for e in tax.EDR_TAXONOMY}
    l3_declared = {n for n in declared if n.startswith("l3_")}
    print(f"declared L3 EDR events: {sorted(l3_declared)}")

    if not LAUNCHER.is_file():
        print(f"FAIL: launcher not built at {LAUNCHER}", file=sys.stderr)
        return 1

    # L3_EDR_GATE_RETRY=0 可关自动重试（本地复现）
    attempts = args.attempts
    _retry_env = os.environ.get("L3_EDR_GATE_RETRY", "1").strip().lower()
    if _retry_env in ("0", "false", "no", "off"):
        attempts = 1
    attempts = max(1, int(attempts))

    work: Path | None = None
    counts: dict = {}
    raw: list = []
    last_infra: str | None = None
    rc = 0
    err = ""

    try:
        for attempt in range(1, attempts + 1):
            if work is not None and not args.keep:
                subprocess.run(["rm", "-rf", str(work)], check=False)
            work = _fresh_work()
            pipeline = build_production_pipeline(args.scenario, work)
            mon_mode = _monitor_mode_from_pipeline(pipeline)
            print(
                f"running pipeline ({args.duration}s) scenario={args.scenario} "
                f"attempt={attempt}/{attempts} "
                f"FLOWENGINE_TEMP_DIR={work} monitor.mode={mon_mode} "
                f"timeout_budget={args.duration + args.timeout_margin}s …",
                flush=True)
            rc, _ = run_pipeline(
                pipeline, args.duration, work,
                timeout_margin=args.timeout_margin)
            counts, raw, err = collect_events(work)
            print(f"PEM event counts: {counts or '(none)'}")

            infra = _attempt_infra_failure(rc, counts, err, args.scenario)
            if infra is None:
                last_infra = None
                if rc != 0:
                    failures.append(f"pipeline exited {rc} (expected 0)")
                elif err and "no PEM" not in err:
                    failures.append(err)
                break
            last_infra = infra
            if attempt < attempts:
                print(
                    f"  (attempt {attempt}/{attempts} infra: {infra}; "
                    f"retrying — known timeout/PEM flush race)",
                    flush=True)
                time.sleep(2.0)
                continue
            if rc != 0:
                failures.append(f"pipeline exited {rc} (expected 0)")
            if err:
                failures.append(err)
            elif "no l3_" in infra:
                failures.append(infra)
            elif "missing" in infra:
                failures.append(infra)
            break

        # ── Rule 2 / 3 / 4（最终 counts）──
        if work is not None and not any("no PEM" in f for f in failures):
            seen_l3 = sorted(n for n in counts if n.startswith("l3_"))
            if not seen_l3:
                msg = (
                    "no l3_* event in PEM — EDR not actually recorded "
                    f"(PEM event names seen: {sorted(counts) or ['none']})")
                if msg not in failures:
                    failures.append(msg)
            else:
                undeclared = [n for n in seen_l3 if n not in declared]
                if undeclared:
                    failures.append(
                        f"PEM contains undeclared L3 events {undeclared} "
                        f"(taxonomy: {sorted(l3_declared)})")

            if "l3_odd_exit" in args.scenario or "l3_" in args.scenario:
                missing = [
                    n for n in REQUIRED_FOR_L3_SCENARIO if counts.get(n, 0) == 0]
                if missing:
                    msg = (
                        f"L3 scenario must record "
                        f"{list(REQUIRED_FOR_L3_SCENARIO)} "
                        f"but missing {missing} (got: {counts})")
                    if msg not in failures:
                        failures.append(msg)

            non_critical = [
                r.get("name") for r in raw
                if str(r.get("name", "")).startswith("l3_")
                and r.get("critical") is not True]
            if non_critical:
                failures.append(
                    "these L3 events were not marked critical/fsync: "
                    f"{sorted(set(non_critical))}")

        if args.json:
            print(json.dumps({
                "counts": counts, "failures": failures,
                "last_infra": last_infra,
            }, ensure_ascii=False, indent=2))
    finally:
        if work is not None:
            if args.keep:
                print(f"work dir kept: {work}")
            else:
                subprocess.run(["rm", "-rf", str(work)], check=False)

    if failures:
        print("\nFAIL:")
        for f in failures:
            print(f"  ✗ {f}")
        print(f"\n{len(failures)} assertion(s) violated")
        return len(failures)

    print("\n✓ L3 EDR events recorded, all critical/fsync")
    return 0


if __name__ == "__main__":
    sys.exit(main())
