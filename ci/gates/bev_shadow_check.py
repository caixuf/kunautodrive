#!/usr/bin/env python3
"""bev_shadow_check.py — 方向一：BEV 影子输出精度门禁（带 A/B 自检）。

背景：`bev/obstacles` 是 BEV 检测头的影子输出（不覆盖 perception/obstacles）。
此前它零消费者、零门禁 —— 节点可以"在发"但发的是错的，或干脆停摆，均无人发现。
demo_evaluator 已有频率活性判据；本 gate 进一步验证**内容**：

  A. 正常跑：`metrics.bev.count` 与 `metrics.scene.obstacles`（真值障碍数）
     差 ≤ max(2, 20%)，且 count > 0（dense_npc 场景有目标）。
  B. 注入 `FLOW_FAULT_INJECT="bev/obstacles:0:999999"`：BEV 帧被丢弃 →
     count 归零 → 必须判 FAIL。若注入后仍判 PASS，说明门禁抓不住已知故障
     （"绿得不可信"），gate 自身报错。

Usage:
  python3 ci/gates/bev_shadow_check.py [--duration 4]
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "build" / "bin" / "flow_launcher"
SCENARIO = ROOT / "scenarios" / "bev_shadow.json"
TOPOLOGY = Path("/tmp/flow_topology.json")


def _patch_pipeline(work: Path, name: str) -> Path:
    cfg = json.loads((ROOT / "config" / "pipeline.json").read_text(encoding="utf-8"))
    for proc in cfg.get("processes") or []:
        if proc.get("name") == "flowsim":
            params = json.loads(proc.get("params") or "{}")
            params["scenario_file"] = str(SCENARIO)
            proc["params"] = json.dumps(params)
    out = work / f"pipeline_{name}.json"
    out.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    return out


def _run(pipeline: Path, log_dir: Path, duration: int, fault: str | None) -> dict:
    TOPOLOGY.unlink(missing_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["FLOW_LOG_DIR"] = str(log_dir)
    if fault:
        env["FLOW_FAULT_INJECT"] = fault
    else:
        env.pop("FLOW_FAULT_INJECT", None)
    result = subprocess.run(
        [str(LAUNCHER), str(pipeline), "--duration", str(duration)],
        cwd=ROOT, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120,
    )
    if not TOPOLOGY.is_file():
        raise SystemExit(
            f"bev shadow gate: no topology snapshot (rc={result.returncode})\n"
            f"{result.stdout[-600:]}\n{result.stderr[-600:]}"
        )
    return json.loads(TOPOLOGY.read_text(encoding="utf-8"))


def _measure(doc: dict) -> tuple[int, int, float]:
    metrics = doc.get("metrics") or {}
    bev = metrics.get("bev") or {}
    scene = metrics.get("scene") or {}
    try:
        count = int(bev.get("count", 0) or 0)
    except (TypeError, ValueError):
        count = 0
    obs = scene.get("obstacles") or []
    truth = len(obs) if isinstance(obs, list) else 0
    tol = max(2, int(0.2 * truth))
    return count, truth, tol


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--duration", type=int, default=4)
    ap.add_argument("--self-test", action="store_true", default=False, help="Run self-test (A/B fault injection)")
    args = ap.parse_args()
    if not LAUNCHER.is_file():
        raise SystemExit(f"missing {LAUNCHER} — build first")

    errors: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        pipe_ok = _patch_pipeline(work, "ok")
        pipe_bad = _patch_pipeline(work, "injected")

        ok_doc = _run(pipe_ok, work / "logs_ok", args.duration, None)
        ok_count, ok_truth, ok_tol = _measure(ok_doc)
        if ok_count < 1:
            errors.append(f"normal run: BEV count={ok_count} (expected >0 on dense_npc)")
        if abs(ok_count - ok_truth) > ok_tol:
            errors.append(
                f"normal run: BEV count {ok_count} vs truth {ok_truth} "
                f"exceeds tolerance ±{ok_tol}"
            )

        # 恒久注入（不带 duration）：sim clock 模式下 after/duration 窗口会因
        # 时钟基不同而失效（elapsed 下溢成超大值），永久规则无窗口判断。
        bad_doc = _run(pipe_bad, work / "logs_bad", args.duration,
                       "bev/obstacles:0")
        bad_count, bad_truth, bad_tol = _measure(bad_doc)
        gate_would_fail = (bad_count < 1) or (abs(bad_count - bad_truth) > bad_tol)
        if not gate_would_fail:
            errors.append(
                f"self-test: injecting bev/obstacles did NOT trip the gate "
                f"(count={bad_count}, truth={bad_truth}, tol=±{bad_tol}) — "
                f"gate cannot catch a known-bad BEV output"
            )

    if errors:
        for e in errors:
            print(f"::error::{e}")
        print(f"bev-shadow-gate FAILED ({len(errors)} issue(s)).")
        return 1
    print(f"✓ BEV shadow OK (normal count={ok_count}/truth={ok_truth}; "
          f"injected count={bad_count} correctly rejected)")
    return 0


if __name__ == "__main__":
    sys.exit(main())