#!/usr/bin/env python3
"""Unit tests for real trajectory loading in train_a100_single.py."""

from __future__ import annotations

import json
import math
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.train_e2e.train_a100_single import build_state_vector_59, load_real_trajectory_dataset  # noqa: E402


def test_build_state_vector_59_encodes_ego_frame_obstacles() -> None:
    rec = {
        "features_v3": [12.0, 211.0, math.pi / 2.0] + [0.0] * 19 + [0.25],
        "ego": {"x": 100.0, "y": 200.0, "v": 12.0, "heading": math.pi / 2.0, "yaw_rate": 0.02},
        "control": {"throttle": 0.4, "brake": 0.0, "steering": 0.01},
        "planning": {"target_speed": 15.0},
        "scene_context": {"speed_limit": 15.0, "curvature": 0.01, "lane_count": 4, "lane_width": 3.5, "ego_lane_offset": 0.25},
        "obstacles": [
            {"x": 100.0, "y": 220.0, "vx": 0.0, "vy": 8.0, "type": 1, "confidence": 0.95},
        ],
    }
    st = build_state_vector_59(rec)
    assert len(st) == 59
    # st[1] normalized to ego_lane_offset instead of global y=211.0
    assert abs(st[1] - 0.25) < 1e-6
    # Nearest obstacle 20m ahead along +y (ego heading pi/2) -> ego-frame lx ~= +20.0, ly ~= 0.0, lvx ~= 8.0
    assert abs(st[23] - 20.0) < 1e-4
    assert abs(st[24]) < 1e-4
    assert abs(st[25] - 8.0) < 1e-4
    assert abs(st[58] - 1.0) < 1e-6  # obstacle_count


def test_load_real_trajectory_dataset_ego_frame_and_segment_split(tmp_path: Path) -> None:
    jsonl = tmp_path / "samples.jsonl"
    rows = []
    # Segment 1: moving North (+y) at 10 m/s for 4.0s (41 frames at 10Hz)
    for k in range(41):
        rows.append(
            {
                "t": 1000 + k * 100,
                "features_v3": [10.0, 0.0, math.pi / 2.0] + [0.0] * 20,
                "ego": {"x": 100.0, "y": 200.0 + k * 1.0, "v": 10.0, "heading": math.pi / 2.0},
            }
        )
    # Segment 2 (unrelated episode after spatial/time jump): only 1.0s -> too short for 3.0s horizon
    for k in range(10):
        rows.append(
            {
                "t": 90000 + k * 100,
                "features_v3": [5.0, 0.0, 0.0] + [0.0] * 20,
                "ego": {"x": 0.0 + k * 0.5, "y": 0.0, "v": 5.0, "heading": 0.0},
            }
        )

    jsonl.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")
    states, trajs = load_real_trajectory_dataset(tmp_path, state_dim=59, horizon=30, dt=0.1)

    # 41 frames at 10Hz = 4.0s -> frames 0..10 have >= 3.0s lookahead (11 samples), segment 2 skipped
    assert len(states) == 11
    assert len(states[0]) == 59
    assert len(trajs[0]) == 30
    # In ego body frame moving North (+y), step 1 (0.1s ahead) is lx ~= +1.0m, ly ~= 0.0m, heading_err ~= 0
    assert abs(trajs[0][0][0] - 1.0) < 1e-4
    assert abs(trajs[0][0][1]) < 1e-4
    assert abs(trajs[0][29][0] - 30.0) < 1e-3
