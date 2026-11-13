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

from tools.train_e2e.train_a100_single import load_real_trajectory_dataset  # noqa: E402


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
