#!/usr/bin/env python3
"""mine_hard_cases.py — 端到端数据飞轮难例回捞脚本

扫描仿真/实车运行导出的 JSONL 切片，按以下触发条件筛选高价值长尾难例并按优先级排序：
  1. 急动/舒适性突变：|jerk| > jerk_thresh (默认 5.0 m/s³)
  2. 险情逼近：min_ttc_s < ttc_thresh (默认 1.5 s)
  3. 端到端低置信度：confidence < conf_thresh (默认 0.4)
  4. 安全仲裁接管/延迟超时：immune_lock / safety_override / latency_budget_exceeded

用法:
  python3 tools/train_e2e/mine_hard_cases.py --input runs/latest/rollout.jsonl --output runs/latest/hard_cases.jsonl
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any


def _estimate_v2_ttc(record: dict[str, Any]) -> float | None:
    """Estimate min TTC (s) from flowengine.e2e_sample.v2 ego + obstacles."""
    ego = record.get("ego")
    obs_list = record.get("obstacles")
    if not isinstance(ego, dict) or not isinstance(obs_list, list) or not obs_list:
        return None
    ex = float(ego.get("x", 0.0))
    ey = float(ego.get("y", 0.0))
    ev = float(ego.get("v", 0.0))
    hdg = float(ego.get("heading", 0.0))
    evx, evy = ev * math.cos(hdg), ev * math.sin(hdg)
    min_ttc: float | None = None
    for obs in obs_list:
        if not isinstance(obs, dict):
            continue
        dx = float(obs.get("x", 0.0)) - ex
        dy = float(obs.get("y", 0.0)) - ey
        dist = math.hypot(dx, dy)
        if dist < 1e-3 or dist > 80.0:
            continue
        rvx = evx - float(obs.get("vx", 0.0))
        rvy = evy - float(obs.get("vy", 0.0))
        closing = (dx * rvx + dy * rvy) / dist
        if closing > 0.5:
            ttc = dist / closing
            min_ttc = ttc if min_ttc is None else min(min_ttc, ttc)
    return min_ttc


def _attach_v2_kinematics(records: list[dict[str, Any]]) -> list[float | None]:
    """Compute finite-difference jerk (m/s^3) for consecutive e2e_sample.v2 records."""
    jerks: list[float | None] = [None] * len(records)
    prev_a: float | None = None
    for i in range(1, len(records)):
        r0, r1 = records[i - 1], records[i]
        e0, e1 = r0.get("ego"), r1.get("ego")
        t0, t1 = r0.get("t"), r1.get("t")
        if not (isinstance(e0, dict) and isinstance(e1, dict) and isinstance(t0, (int, float)) and isinstance(t1, (int, float))):
            prev_a = None
            continue
        dt = (float(t1) - float(t0)) / 1000.0
        if dt <= 1e-3 or dt > 1.0:
            prev_a = None
            continue
        a = (float(e1.get("v", 0.0)) - float(e0.get("v", 0.0))) / dt
        if prev_a is not None:
            jerks[i] = (a - prev_a) / dt
        prev_a = a
    return jerks


def score_hard_case(
    record: dict[str, Any],
    *,
    jerk_thresh: float = 5.0,
    ttc_thresh: float = 1.5,
    conf_thresh: float = 0.4,
    derived_jerk: float | None = None,
) -> tuple[float, list[str]]:
    """Return (priority_score, reasons). Score == 0.0 means not a hard case."""
    reasons: list[str] = []
    score = 0.0

    jerk = record.get("jerk")
    if not isinstance(jerk, (int, float)):
        jerk = derived_jerk
    if isinstance(jerk, (int, float)) and abs(float(jerk)) > jerk_thresh:
        reasons.append("high_jerk")
        score += 1.0 + min(3.0, (abs(float(jerk)) - jerk_thresh) / 5.0)

    ttc = record.get("min_ttc_s")
    if not isinstance(ttc, (int, float)):
        ttc = _estimate_v2_ttc(record)
    if isinstance(ttc, (int, float)) and 0.0 <= float(ttc) < ttc_thresh:
        reasons.append("low_ttc")
        score += 2.0 + (ttc_thresh - float(ttc))

    conf = record.get("confidence")
    if isinstance(conf, (int, float)) and float(conf) < conf_thresh:
        reasons.append("low_confidence")
        score += 1.0 + (conf_thresh - float(conf))

    ctrl = record.get("control") if isinstance(record.get("control"), dict) else {}
    if record.get("immune_lock") or record.get("safety_override") or ctrl.get("emergency_stop"):
        reasons.append("safety_takeover")
        score += 3.0
    elif isinstance(ctrl.get("brake"), (int, float)) and float(ctrl["brake"]) >= 0.5:
        reasons.append("hard_brake")
        score += 1.5 + float(ctrl["brake"])

    if record.get("latency_budget_exceeded"):
        reasons.append("latency_budget_exceeded")
        score += 1.0

    return round(score, 4), reasons


def mine_hard_cases(
    records: list[dict[str, Any]],
    *,
    jerk_thresh: float = 5.0,
    ttc_thresh: float = 1.5,
    conf_thresh: float = 0.4,
    limit: int | None = None,
) -> list[dict[str, Any]]:
    derived_jerks = _attach_v2_kinematics(records)
    mined: list[dict[str, Any]] = []
    for idx, rec in enumerate(records):
        score, reasons = score_hard_case(
            rec,
            jerk_thresh=jerk_thresh,
            ttc_thresh=ttc_thresh,
            conf_thresh=conf_thresh,
            derived_jerk=derived_jerks[idx],
        )
        if score > 0.0:
            item = dict(rec)
            item["_hard_case_index"] = idx
            item["_hard_case_score"] = score
            item["_hard_case_reasons"] = reasons
            mined.append(item)
    mined.sort(key=lambda r: r["_hard_case_score"], reverse=True)
    if limit is not None and limit > 0:
        mined = mined[:limit]
    return mined


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Mine hard cases from E2E rollout JSONL.")
    ap.add_argument("--input", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--jerk-thresh", type=float, default=5.0)
    ap.add_argument("--ttc-thresh", type=float, default=1.5)
    ap.add_argument("--conf-thresh", type=float, default=0.4)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args(argv)

    records: list[dict[str, Any]] = []
    with args.input.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                records.append(json.loads(line))

    mined = mine_hard_cases(
        records,
        jerk_thresh=args.jerk_thresh,
        ttc_thresh=args.ttc_thresh,
        conf_thresh=args.conf_thresh,
        limit=args.limit or None,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as fh:
        for item in mined:
            fh.write(json.dumps(item, ensure_ascii=False) + "\n")

    print(f"mined {len(mined)}/{len(records)} hard cases -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

