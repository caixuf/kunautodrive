#!/usr/bin/env python3
"""synth_data.py — 合成训练样本生成器（IDM 规则，补真实采集的边界覆盖）

真实采集(planning 日常行为)天然缺少「高速逼近慢车/静止障碍」的急刹样本
（60s 采集 0 条近灯+强刹），模型学到的是「planning 日常」而非「安全底线」。
本生成器按 IDM 安全间距规则合成 (特征 → throttle/brake) 对，补进训练集。

关键约定（与 eval_closed_loop 执行端一致）：
  - throttle/brake 严格互斥（危险→刹车 0 油门；安全→油门 0 刹车）
  - front0_x 用世界坐标（与 data_recorder 一致：ego 在 x=0，障碍在 front_x）
  - 远处占位 = ego+500（无车时）

用法:
  python3 tools/train_e2e/synth_data.py --output /tmp/synth.jsonl
  # 网格密度: 速度×距离×前车速度
  python3 tools/train_e2e/synth_data.py --dense
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
from feature_schema import build_v3_features  # noqa: E402


def _oracle_ctrl(v: float, ego_y: float, heading: float, gap: float,
                 front_vx: float, empty_road: bool = False,
                 speed_limit: float = 30.0, lane_ref: float = -1.75) -> tuple[float, float, float]:
    target_v = min(20.0, speed_limit)
    steer = max(-0.35, min(0.35, -0.15 * (ego_y - lane_ref) - 0.60 * heading))
    if empty_road or gap > 120.0:
        thr = min(1.0, max(0.0, 0.25 + (target_v - v) * 0.04))
        return thr, 0.0, steer
    if front_vx < 0.5 and gap < 16.0:
        return 0.0, 0.65, steer
    dv = v - front_vx
    safe = 8.0 + v * 1.8
    ttc = gap / max(dv, 0.1) if dv > 0.05 else 999.0
    if gap < 8.0:
        return 0.0, min(1.0, 0.65 + (8.0 - gap) * 0.1), steer
    if gap < safe and dv > 0.2:
        brk = min(1.0, 0.45 + (safe - gap) * 0.08 + dv * 0.08)
        return 0.0, brk, steer
    if ttc < 3.5:
        brk = min(1.0, 0.40 + (3.5 - ttc) * 0.18)
        return 0.0, brk, steer
    if gap < safe * 1.6:
        follow_v = max(0.0, min(target_v, front_vx + (gap - safe) * 0.25))
        if v > follow_v + 0.8:
            return 0.0, min(1.0, 0.35 + (v - follow_v) * 0.15), steer
        thr = min(0.8, max(0.0, 0.08 + (follow_v - v) * 0.12))
        return thr, 0.0, steer
    thr = min(1.0, max(0.0, 0.25 + (target_v - v) * 0.04))
    return thr, 0.0, steer


def make_sample(v: float, front_x: float, front_vx: float,
                ego_y: float = -1.75, heading: float = 0.0,
                speed_limit: float = 30.0, tl_state: float = -1.0,
                ego_x: float = 0.0, empty_road: bool = False,
                rollout_id: str | None = None) -> dict:
    """构造一条 (特征, control) 样本: ego 在 ego_x, 障碍在 ego_x + front_x(世界坐标)。"""
    ego = {"x": ego_x, "y": ego_y, "v": v, "heading": heading, "yaw_rate": 0.0}
    if empty_road:
        obstacles = [
            {"x": ego_x + 500.0, "y": 0.0, "vx": 0.0, "vy": 0.0,
             "type": 1.0, "confidence": 1.0, "length": 4.5, "width": 1.9},
            {"x": ego_x + 500.0, "y": 0.0, "vx": 0.0, "vy": 0.0,
             "type": 1.0, "confidence": 1.0, "length": 4.5, "width": 1.9},
        ]
        gap = 500.0
    else:
        obstacles = [
            {"x": ego_x + front_x, "y": -1.75, "vx": front_vx, "vy": 0.0,
             "type": 1.0, "confidence": 1.0, "length": 4.5, "width": 1.9},
            {"x": ego_x + 500.0, "y": 0.0, "vx": 0.0, "vy": 0.0,
             "type": 1.0, "confidence": 1.0, "length": 4.5, "width": 1.9},
        ]
        gap = front_x - 2.25
    throttle, brake, steer = _oracle_ctrl(
        v, ego_y, heading, gap, front_vx, empty_road=empty_road,
        speed_limit=speed_limit)
    control = {"throttle": throttle, "brake": brake, "steering": steer,
               "steer": steer, "emergency_stop": False}
    scene = {"tl_state": tl_state, "tl_distance": -1.0, "curvature": 0.0,
             "speed_limit": speed_limit, "lane_count": 4, "lane_width": 3.5,
             "ego_lane_offset": ego_y}
    feat_ctrl = {"brake": 0.0, "emergency_stop": False}
    feats = build_v3_features(ego, obstacles, feat_ctrl, None, scene, [])
    sample = {"features_v3": feats, "control": control, "label": 0.0,
              "ego": {"x": ego_x, "y": ego_y, "v": v, "heading": heading,
                      "yaw_rate": 0.0},
              "synthetic": True, "synth_v": v, "synth_gap": gap}
    if rollout_id is not None:
        sample["rollout_id"] = rollout_id
    return sample


def gen_rollouts() -> list[dict]:
    """生成连续闭环运动轨迹，保证 5 帧时序滑窗具备真实 DT=0.05 速度/间距差分。"""
    import math as _m

    dt = 0.05
    wb = 2.7
    out: list[dict] = []
    configs = [
        # Cruise rollouts
        (0.0, None, 0.0, 0.0, -1.75, 0.0, 600),
        (0.0, None, 0.0, 5.0, -2.35, 0.04, 250),
        (0.0, None, 0.0, 5.0, -1.15, -0.04, 250),
        # Lead rollouts
        (0.0, 35.0, 6.0, 0.0, -1.75, 0.0, 600),
        (0.0, 40.0, 6.0, 0.0, -1.75, 0.0, 600),
        (0.0, 50.0, 6.0, 4.0, -2.05, 0.02, 500),
        (25.0, 40.0, 6.0, 2.0, -1.45, -0.02, 500),
        (0.0, 45.0, 10.0, 0.0, -1.75, 0.0, 500),
        # Emergency rollouts
        (0.0, 45.0, 0.0, 0.0, -1.75, 0.0, 450),
        (0.0, 60.0, 0.0, 0.0, -1.75, 0.0, 600),
        (0.0, 75.0, 0.0, 4.0, -1.75, 0.0, 500),
        (20.0, 55.0, 0.0, 0.0, -2.05, 0.02, 450),
    ]
    for idx, (ex0, ldist, lv, v0, y0, h0, steps) in enumerate(configs):
        rid = f"ro_{idx}"
        ex, ey, ev, eh = ex0, y0, v0, h0
        lx = (ex0 + ldist) if ldist is not None else None
        for _ in range(steps):
            if lx is not None:
                lx += lv * dt
                s = make_sample(ev, lx - ex, lv, ego_y=ey, heading=eh,
                                ego_x=ex, empty_road=False, rollout_id=rid)
            else:
                s = make_sample(ev, 500.0, 0.0, ego_y=ey, heading=eh,
                                ego_x=ex, empty_road=True, rollout_id=rid)
            out.append(s)
            thr = s["control"]["throttle"]
            brk = s["control"]["brake"]
            st = s["control"]["steering"]
            if brk > 0.2:
                ev = max(0.0, ev - brk * 8.0 * dt)
            elif thr > 0.0:
                ev = min(30.0, ev + thr * 3.33 * dt)
            yr = ev / wb * _m.tan(st)
            eh += yr * dt
            ex += ev * _m.cos(eh) * dt
            ey += ev * _m.sin(eh) * dt
    return out


def gen_grid() -> list[dict]:
    """平移不变网格 + 连续闭环轨迹 + 空路巡航/横向纠偏"""
    out = gen_rollouts()
    gid = 0
    for ego_x in [0.0, 40.0, 90.0, 180.0]:
        for v in [0, 4, 8, 12, 16]:
            for dist in [5, 9, 14, 20, 28, 40, 60, 120]:
                for fv in [0, 6, 12]:
                    rid = f"grid_{gid}"
                    gid += 1
                    s = make_sample(v, dist, fv, ego_x=ego_x, rollout_id=rid)
                    for _ in range(5):
                        out.append(s)
    for ego_x in [0.0, 150.0, 350.0]:
        for ego_y in [-2.75, -2.05, -1.75, -1.45, -0.75]:
            for heading in [-0.08, 0.0, 0.08]:
                for v in [0, 10, 20]:
                    rid = f"grid_{gid}"
                    gid += 1
                    s = make_sample(v, 500.0, 0.0, ego_y=ego_y, heading=heading,
                                    speed_limit=30.0, tl_state=-1.0, ego_x=ego_x,
                                    empty_road=True, rollout_id=rid)
                    for _ in range(5):
                        out.append(s)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="合成训练样本生成器")
    ap.add_argument("--output", default="/tmp/synth.jsonl")
    ap.add_argument("--dense", action="store_true",
                    help="更密网格: 速度(9)×距离(14)×前车速(7)=882 条")
    args = ap.parse_args()

    if args.dense:
        samples = []
        for v in [3, 5, 8, 10, 12, 16, 18, 20, 24]:
            for dist in [3, 5, 8, 10, 12, 15, 18, 22, 28, 35, 45, 60, 100, 300]:
                for fv in [-2, 0, 3, 6, 10, 14, 18]:
                    samples.append(make_sample(v, dist, fv))
    else:
        samples = gen_grid()

    with open(args.output, "w", encoding="utf-8") as f:
        for s in samples:
            f.write(json.dumps(s) + "\n")
    print(f"合成样本: {len(samples)} 条 → {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
