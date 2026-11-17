#!/usr/bin/env python3
"""train_parking_policy.py — 20,000+ 次闭环泊车快速训练与策略参数优化器。

使用多进程 + Cross-Entropy Method (CEM) 在三类随机化泊车工况下快速训练：
  1. 垂直倒车入库 (Perpendicular Reverse)
  2. 侧方位停车 (Parallel Parking)
  3. 歪斜车位二次揉库纠偏 (Crooked-Slot Recovery ±30°)

训练结束后在 5,000 次独立测试集上对比 Baseline vs Trained，并导出
config/parking_policy_tuned.json。
"""

from __future__ import annotations

import argparse
import json
import math
import multiprocessing as mp
import random
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
THIS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(THIS_DIR))

from maneuver_tracker import ManeuverTracker, TrajectoryPoint, norm_angle  # noqa: E402
from open_space_parking_sim import (  # noqa: E402
    WHEELBASE,
    generate_parallel_parking_trajectory,
    generate_perpendicular_parking_trajectory,
)

MAX_STEER = 0.60
DEFAULT_OUT = ROOT / "config" / "parking_policy_tuned.json"

# [la_rev, la_fwd, k_heading, k_cte, k_rec_steer, v_rev_taper_m]
BASELINE_PARAMS = np.array([0.50, 1.20, 1.00, 0.40, 2.50, 0.80], dtype=np.float64)
PARAM_BOUNDS = np.array([
    [0.25, 1.00],  # la_rev
    [0.60, 2.00],  # la_fwd
    [0.40, 2.50],  # k_heading
    [0.10, 1.50],  # k_cte
    [1.20, 4.00],  # k_rec_steer
    [0.30, 1.50],  # v_rev_taper_m
], dtype=np.float64)


class TunableParkingTracker(ManeuverTracker):
    def __init__(self, traj: list[TrajectoryPoint], params: np.ndarray):
        super().__init__(traj)
        self.la_rev = float(params[0])
        self.la_fwd = float(params[1])
        self.k_heading = float(params[2])
        self.k_cte = float(params[3])

    def _lookahead(self, base_i: int) -> TrajectoryPoint:
        rev = self.gear == -1
        target = self.traj[base_i]
        j = base_i
        arc = 0.0
        la = self.la_rev if rev else self.la_fwd
        while j + 1 < self.n and arc < la:
            next_v = self.traj[j + 1].v
            if abs(next_v) > 0.1 and (next_v < -0.1) != rev:
                break
            arc += math.hypot(self.traj[j + 1].x - self.traj[j].x,
                              self.traj[j + 1].y - self.traj[j].y)
            j += 1
            target = self.traj[j]
        return target

    def _update_gear(self, base_i: int, current_speed: float) -> bool:
        base_v = self.traj[base_i].v
        if abs(base_v) > 0.1:
            want = 1 if base_v > 0 else -1
        else:
            want = self.gear
            for i in range(base_i, self.n):
                if abs(self.traj[i].v) > 0.1:
                    want = 1 if self.traj[i].v > 0 else -1
                    break
        if want != self.gear:
            if abs(current_speed) > 0.15:
                return True
            self.gear = want
        return False

    def compute_steer(self, ex: float, ey: float, eh: float, target: TrajectoryPoint) -> float:
        dx = target.x - ex
        dy = target.y - ey
        # Signed lateral error in vehicle frame
        ey_lat = -math.sin(eh) * dx + math.cos(eh) * dy
        dh = norm_angle(target.heading - eh)
        if self.gear == -1:
            # Reverse: steer sign flips for lateral error, plus heading correction
            raw = self.k_heading * (-dh) + self.k_cte * (-ey_lat)
        else:
            raw = self.k_heading * dh + self.k_cte * ey_lat
        return max(-MAX_STEER, min(MAX_STEER, raw))


def _make_crooked_traj(
    start_pose: tuple[float, float, float],
    target_pose: tuple[float, float, float],
    k_rec_steer: float,
) -> tuple[list[TrajectoryPoint], tuple[float, float, float]]:
    dt = 0.05
    pts: list[TrajectoryPoint] = []
    sx, sy, sh = start_pose
    goal_x, goal_y, target_heading = target_pose

    def step_steer(steer: float, v: float, n: int) -> None:
        nonlocal sx, sy, sh
        k = math.tan(steer) / WHEELBASE
        for _ in range(n):
            yaw = v / WHEELBASE * math.tan(steer)
            sh = norm_angle(sh + yaw * dt)
            sx += (v * math.cos(sh) - WHEELBASE * 0.5 * math.sin(sh) * yaw) * dt
            sy += (v * math.sin(sh) + WHEELBASE * 0.5 * math.cos(sh) * yaw) * dt
            pts.append(TrajectoryPoint(sx, sy, sh, kappa=k, v=v))

    for _ in range(45):
        dh = norm_angle(target_heading - sh)
        steer = 0.0 if abs(dh) < 0.01 else max(-0.48, min(0.48, dh * k_rec_steer))
        step_steer(steer, 1.0, 1)
    step_steer(0.0, 0.5, 10)
    # Finish heading alignment (the 45-step loop froze steer below 0.01 rad
    # and often left >3° residual, failing the slot heading gate).
    for _ in range(60):
        dh = norm_angle(target_heading - sh)
        if abs(dh) < 0.002:
            break
        step_steer(max(-0.50, min(0.50, dh * k_rec_steer)), 0.35, 1)
    step_steer(0.0, 0.2, 5)
    # Drive the reverse leg by the true slot distance instead of a fixed
    # 35+12 steps (which stopped ~0.87 m short and made the trainer grade
    # against its own endpoint).
    dx = goal_x - sx
    dy = goal_y - sy
    dist_back = -(dx * math.cos(sh) + dy * math.sin(sh))
    if dist_back > 0.02:
        n = max(1, int(round(dist_back / (0.30 * dt))))
        step_steer(0.0, -0.30, n)
    elif dist_back < -0.02:
        n = max(1, int(round(-dist_back / (0.30 * dt))))
        step_steer(0.0, 0.30, n)
    pts.append(TrajectoryPoint(goal_x, goal_y, target_heading, kappa=0.0, v=0.0))
    return pts, (sx, sy, sh)


def run_episode(mode: int, seed: int, params: np.ndarray) -> tuple[float, float, bool]:
    rng = random.Random(seed)
    if mode == 0:
        start = (rng.uniform(-0.8, 0.8), 2.0 + rng.uniform(-0.4, 0.4), rng.uniform(-0.12, 0.12))
        traj, goal = generate_perpendicular_parking_trajectory(start)
    elif mode == 1:
        start = (rng.uniform(-0.6, 0.6), 2.0 + rng.uniform(-0.35, 0.35), rng.uniform(-0.10, 0.10))
        traj, goal = generate_parallel_parking_trajectory(start)
    else:
        deg = rng.choice([-1.0, 1.0]) * rng.uniform(12.0, 30.0)
        start = (10.0 + rng.uniform(-0.2, 0.2), -5.0 + rng.uniform(-0.2, 0.2),
                 norm_angle(-math.pi * 0.5 + math.radians(deg)))
        goal = (10.0, -5.0, -math.pi * 0.5)  # true slot, not the generator endpoint
        traj, _ = _make_crooked_traj(start, goal, float(params[4]))

    tracker = TunableParkingTracker(traj, params)
    cx, cy, ch = start
    cv = 0.0
    dt = 0.05

    for _ in range(450):
        st_cmd, thr, brk, gear = tracker.tick(cx, cy, ch, cv, dt)
        _exec_pt, base_i = tracker._exec()
        target = tracker._lookahead(base_i)
        steer_fb = tracker.compute_steer(cx, cy, ch, target)
        # Blend geometric tracker steer with tunable residual feedback
        blend = min(0.35, max(0.0, float(params[5]) * 0.25))
        steer = max(-MAX_STEER, min(MAX_STEER, (1.0 - blend) * st_cmd + blend * steer_fb))

        if thr < 0:
            cv = max(-2.5, cv + thr * 3.33 * dt)
        elif thr > 0:
            cv = min(15.0, cv + thr * 3.33 * dt)
        elif brk > 0:
            if cv > 0:
                cv = max(0.0, cv - brk * 8.0 * dt)
            elif cv < 0:
                cv = min(0.0, cv + brk * 8.0 * dt)

        yaw = cv / WHEELBASE * math.tan(steer)
        ch = norm_angle(ch + yaw * dt)
        cx += (cv * math.cos(ch) - WHEELBASE * 0.5 * math.sin(ch) * yaw) * dt
        cy += (cv * math.sin(ch) + WHEELBASE * 0.5 * math.cos(ch) * yaw) * dt

        if tracker.s >= tracker.cum_s[-1] and abs(cv) < 0.05:
            break

    pos_err = math.hypot(cx - goal[0], cy - goal[1])
    h_err_deg = abs(math.degrees(norm_angle(ch - goal[2])))
    passed = (pos_err < 0.25) and (h_err_deg < 3.0)
    return pos_err, h_err_deg, passed


def _eval_batch_worker(args: tuple[np.ndarray, int, int]) -> tuple[float, int]:
    params, start_seed, count = args
    loss_sum = 0.0
    passes = 0
    for i in range(count):
        s = start_seed + i
        pos_err, h_deg, ok = run_episode(i % 3, s, params)
        loss_sum += pos_err + 0.08 * h_deg + (0.0 if ok else 1.5)
        if ok:
            passes += 1
    return loss_sum / count, passes


def evaluate_suite(pool: mp.pool.Pool, params: np.ndarray, total_episodes: int, seed_base: int) -> dict[str, Any]:
    n_workers = pool._processes
    chunk = total_episodes // n_workers
    tasks = [
        (params, seed_base + w * chunk, chunk if w < n_workers - 1 else total_episodes - w * chunk)
        for w in range(n_workers)
    ]

    def _run_chunk(t: tuple[np.ndarray, int, int]) -> list[tuple[float, float, bool]]:
        p, sb, cnt = t
        return [run_episode(i % 3, sb + i, p) for i in range(cnt)]

    all_res: list[tuple[float, float, bool]] = []
    # Run sequentially per-worker via _eval_detailed
    for r in pool.map(_eval_detailed_worker, tasks):
        all_res.extend(r)

    pos_errs = np.array([x[0] for x in all_res])
    h_errs = np.array([x[1] for x in all_res])
    passes = sum(1 for x in all_res if x[2])
    return {
        "episodes": len(all_res),
        "pass_rate": round(passes / len(all_res), 4),
        "mean_pos_err_m": round(float(np.mean(pos_errs)), 4),
        "p99_pos_err_m": round(float(np.percentile(pos_errs, 99)), 4),
        "mean_heading_err_deg": round(float(np.mean(h_errs)), 3),
        "p99_heading_err_deg": round(float(np.percentile(h_errs, 99)), 3),
    }


def _eval_detailed_worker(args: tuple[np.ndarray, int, int]) -> list[tuple[float, float, bool]]:
    p, sb, cnt = args
    return [run_episode(i % 3, sb + i, p) for i in range(cnt)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--generations", type=int, default=25)
    ap.add_argument("--pop-size", type=int, default=32)
    ap.add_argument("--episodes-per-cand", type=int, default=27)
    ap.add_argument("--eval-episodes", type=int, default=5000)
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    args = ap.parse_args()

    t0 = time.perf_counter()
    n_workers = min(16, mp.cpu_count() or 4)
    rng = np.random.default_rng(2026)

    mean = BASELINE_PARAMS.copy()
    std = np.array([0.15, 0.25, 0.35, 0.25, 0.50, 0.25], dtype=np.float64)
    best_params = BASELINE_PARAMS.copy()
    best_loss = 1e9
    train_episodes = args.generations * args.pop_size * args.episodes_per_cand

    with mp.Pool(processes=n_workers) as pool:
        baseline_eval = evaluate_suite(pool, BASELINE_PARAMS, args.eval_episodes, seed_base=900000)

        for gen in range(args.generations):
            cands = rng.normal(mean, std, size=(args.pop_size, len(mean)))
            cands = np.clip(cands, PARAM_BOUNDS[:, 0], PARAM_BOUNDS[:, 1])
            tasks = [
                (cands[k], 100000 + gen * 5000 + k * args.episodes_per_cand, args.episodes_per_cand)
                for k in range(args.pop_size)
            ]
            results = pool.map(_eval_batch_worker, tasks)
            losses = np.array([r[0] for r in results])
            elite_idx = np.argsort(losses)[: max(4, args.pop_size // 4)]
            elites = cands[elite_idx]
            mean = 0.7 * np.mean(elites, axis=0) + 0.3 * mean
            std = np.maximum(0.02, 0.7 * np.std(elites, axis=0) + 0.3 * std)
            if losses[elite_idx[0]] < best_loss:
                best_loss = float(losses[elite_idx[0]])
                best_params = cands[elite_idx[0]].copy()

        trained_eval = evaluate_suite(pool, best_params, args.eval_episodes, seed_base=900000)

    elapsed = time.perf_counter() - t0
    names = ["la_rev", "la_fwd", "k_heading", "k_cte", "k_rec_steer", "v_rev_taper_m"]
    artifact = {
        "schema": "flowengine.parking_policy_tuned.v1",
        "train_episodes": train_episodes,
        "eval_episodes": args.eval_episodes,
        "elapsed_s": round(elapsed, 2),
        "tuned_params": {k: round(float(v), 4) for k, v in zip(names, best_params)},
        "baseline_eval": baseline_eval,
        "trained_eval": trained_eval,
    }
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(artifact, indent=2) + "\n", encoding="utf-8")

    print(
        f"✓ Trained {train_episodes} episodes + Evaluated {args.eval_episodes} held-out episodes "
        f"in {elapsed:.2f}s ({n_workers} workers)"
    )
    print(f"  Baseline: pass={baseline_eval['pass_rate']*100:.2f}% "
          f"mean_pos={baseline_eval['mean_pos_err_m']:.3f}m "
          f"mean_hdg={baseline_eval['mean_heading_err_deg']:.2f}°")
    print(f"  Trained : pass={trained_eval['pass_rate']*100:.2f}% "
          f"mean_pos={trained_eval['mean_pos_err_m']:.3f}m "
          f"mean_hdg={trained_eval['mean_heading_err_deg']:.2f}°")
    print(f"  Exported: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
