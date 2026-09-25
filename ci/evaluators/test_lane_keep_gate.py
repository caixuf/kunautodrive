#!/usr/bin/env python3
"""车道保持门禁自测（D2-06 判据落地）。

门禁盯的是**权威**车道级定位 `localization/lane_match` 的 `offset`
（esmini world_to_frenet 解算），而不是评测器按 scene 几何现推的 lane_error。

覆盖：
  [1] 居中巡航 → 不判（零误报）
  [2] 实测故障复现：|offset|=1.55 m（掉头后骑线）→ 判"车身压线"
      并同时证明 D2-06 原文判据（lane_width/2 − 0.1 = 1.65 m）会漏掉它
  [3] 车心越线（|offset|=1.7 m）→ 更严重级 FAIL
  [4] 机动帧（变道/掉头）越线 → 不判（合法越线）
  [5] lane_match 缺失/无效 → 只 WARN 无法判定，不 FAIL
  [6] lane_match 覆盖率不足 → 只 WARN
  [7] summary 上报 lane_keep_* 指标

跑法：python3 ci/evaluators/test_lane_keep_gate.py
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

_PASSED = 0
_FAILED = 0


def _load_evaluator():
    spec = importlib.util.spec_from_file_location(
        "demo_evaluator", ROOT / "ci" / "evaluators" / "demo_evaluator.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["demo_evaluator"] = module
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def check(name: str, condition: bool, detail: str = "") -> None:
    global _PASSED, _FAILED
    if condition:
        _PASSED += 1
        print(f"  ok   {name}")
    else:
        _FAILED += 1
        print(f"  FAIL {name}{('  — ' + detail) if detail else ''}")


_TOPICS = [
    {"node": "monitor", "topic": "vehicle/state", "role": "sub"},
]


def _mk(x, speed=12.0, ts=0.0, lane_offset=None, lane_width=3.5,
        behavior_state="CRUISE", lane_match_ok=None):
    """一帧样本。lane_offset=None → metrics 里不带 lane_match（旧配置/无定位）。"""
    metrics = {
        "topics": _TOPICS,
        "vehicle": {"speed": speed, "x": x},
        "scene": {
            "ego": {"x": x, "y": 0.0, "speed": speed, "steer": 0.0, "heading": 0.0},
            "lane": {"width": lane_width, "count": 4},
            "obstacles": [],
            "entities": [],
        },
        "behavior": {"state": behavior_state},
        "driver_mode": "NOA:" + behavior_state,
    }
    if lane_offset is not None or lane_match_ok is not None:
        lm = {"ok": True if lane_match_ok is None else bool(lane_match_ok),
              "x": x, "y": 0.0}
        if lane_offset is not None:
            lm.update({"road_id": 1, "lane_id": 2, "s": x, "offset": lane_offset})
        metrics["lane_match"] = lm
    return {"timestamp": ts, "metrics": metrics, "nodes": []}


def _run(samples, label="lane-keep"):
    """score() 的返回顺序是 (failures, warnings, summary)——按名字解包，别按位置猜。"""
    fails, warns, summary = de.score(
        samples, ROOT / "does-not-exist.log",
        criteria={"min_distance_m": 0.0, "min_avg_speed_mps": 0.0},
        scenario_name=label, expected_edges=[], road=None)
    return fails, warns, summary


def _cruise(offset, n=60, dt=0.25, speed=12.0, behavior="CRUISE"):
    """一段匀速巡航：x 每帧前进 2 m（足够触发 10 m progress 判据）。"""
    return [_mk(10.0 + i * 2.0, speed=speed, ts=i * dt,
                lane_offset=offset, behavior_state=behavior) for i in range(n)]


def run_all_checks() -> int:
    print(f"\n[1] 居中巡航（|offset| = 0.2 m）—— 零误报")
    _f, _w, _s = _run(_cruise(0.2))
    check("no lane-keeping verdict when centered",
          not any("lane keeping" in x for x in _w + _f), f"w={_w} f={_f}")
    check("summary reports worst offset",
          abs(float(_s.get("lane_keep_worst_offset_m", -1)) - 0.2) < 1e-6,
          str(_s.get("lane_keep_worst_offset_m")))
    check("summary reports full lane_match coverage",
          abs(float(_s.get("lane_match_coverage", 0)) - 1.0) < 1e-6,
          str(_s.get("lane_match_coverage")))

    print(f"\n[2] 实测故障：掉头后骑线 |offset| = 1.55 m（lane_width 3.5 m）")
    # 这是真实日志里抓到的状态：规划目标 -1.75，自车停在 -3.3 → |offset|≈1.55。
    # D2-06 原文判据 1.75-0.1 = 1.65 > 1.55，因此必须靠"车身压线"判据兜住。
    _f, _w, _s = _run(_cruise(-1.55))
    check("body-straddle FAIL is raised for the real-world case",
          any("rides the lane line" in x for x in _f), f"f={_f}")
    check("literal D2-06 threshold (1.65 m) would NOT have caught it",
          not any("CENTER crossed" in x for x in _w + _f), f"w={_w} f={_f}")
    check("summary attributes it to straddle frames, not center crossing",
          int(_s.get("lane_keep_straddle_frames", 0)) == 60
          and int(_s.get("lane_keep_center_cross_frames", 0)) == 0,
          f"straddle={_s.get('lane_keep_straddle_frames')} "
          f"center={_s.get('lane_keep_center_cross_frames')}")

    print(f"\n[3] 车心越线 |offset| = 1.7 m —— 更严重级")
    _f, _w, _s = _run(_cruise(1.7))
    check("center-crossing FAIL is raised",
          any("CENTER crossed the lane line" in x for x in _f), f"f={_f}")

    print(f"\n[4] 机动帧（变道 / 掉头）越线 —— 合法，不判")
    for _state in ("LEFT_CHANGE", "U_TURN"):
        _f, _w, _s = _run(_cruise(1.6, behavior=_state))
        check(f"maneuver frames excluded ({_state})",
              not any("lane keeping" in x for x in _w + _f), f"w={_w} f={_f}")

    print(f"\n[5] lane_match 缺失 / ok=false —— 无法判定，只 WARN")
    _nomatch = [_mk(10.0 + i * 2.0, ts=i * 0.25) for i in range(60)]
    _f, _w, _s = _run(_nomatch)
    check("no FAIL when authoritative lane offset is absent",
          not any("lane keeping" in x for x in _f), f"f={_f}")
    check("explicit 'cannot tell whether ego rides the line' warning",
          any("cannot tell whether ego rides the line" in x for x in _w), f"w={_w}")
    _invalid = [_mk(10.0 + i * 2.0, ts=i * 0.25, lane_offset=-1.55, lane_match_ok=False)
                for i in range(60)]
    _f, _w, _s = _run(_invalid)
    check("ok=false samples are treated as unavailable, not as 'no violation'",
          not any("rides the lane line" in x for x in _f) and bool(_w), f"w={_w} f={_f}")

    print(f"\n[6] lane_match 覆盖率不足 —— 只 WARN，不 FAIL")
    _mixed = _cruise(-1.55, n=20) + [_mk(60.0 + i * 2.0, ts=6.0 + i * 0.25)
                                     for i in range(60)]
    _f, _w, _s = _run(_mixed)
    check("low-coverage run is skipped, not failed",
          not any("lane keeping" in x for x in _f)
          and any("coverage" in x for x in _w), f"w={_w} f={_f}")

    print(f"\n[7] summary 指标齐全")
    _keys = ("lane_match_coverage", "lane_keep_worst_offset_m",
             "lane_keep_straddle_frames", "lane_keep_straddle_ratio",
             "lane_keep_straddle_max_consecutive", "lane_keep_center_cross_frames",
             "lane_keep_center_cross_ratio", "lane_keep_max_excess_m")
    _f, _w, _s = _run(_cruise(-1.55))
    _missing = [k for k in _keys if k not in _s]
    check("all lane-keeping summary fields present", not _missing, str(_missing))

    print("\n" + "=" * 52)
    print(f"lane-keep gate self-test: {_PASSED} passed, {_FAILED} failed")
    print("=" * 52)
    return 1 if _FAILED else 0


de = _load_evaluator()


def test_lane_keep_gate() -> None:
    """pytest 入口。"""
    assert run_all_checks() == 0


if __name__ == "__main__":
    sys.exit(run_all_checks())
