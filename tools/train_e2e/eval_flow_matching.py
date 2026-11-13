#!/usr/bin/env python3
"""eval_flow_matching.py — ONNX Flow-Matching 轨迹模型开环 ADE/FDE 与延迟评估器。

用法:
    python3 tools/train_e2e/eval_flow_matching.py \
      --onnx models/flow_matching_v1/policy_flow_matching.onnx \
      --dataset /tmp/e2e_multi_samples.jsonl \
      --predictions-out /tmp/fm_predictions.json
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
import time
from typing import Any

import numpy as np

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.train_e2e.train_a100_single import load_real_trajectory_dataset  # noqa: E402


def _percentile(sorted_vals: list[float], pct: float) -> float:
    if not sorted_vals:
        return 0.0
    idx = min(len(sorted_vals) - 1, max(0, int(round((pct / 100.0) * (len(sorted_vals) - 1)))))
    return sorted_vals[idx]


def evaluate_onnx(
    onnx_path: Path,
    states: list[list[float]],
    trajectories: list[list[list[float]]],
    *,
    max_samples: int = 300,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    import onnxruntime as ort

    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    in_name = sess.get_inputs()[0].name
    out_name = sess.get_outputs()[0].name

    n = min(len(states), max_samples) if max_samples > 0 else len(states)
    ades: list[float] = []
    fdes: list[float] = []
    speed_errs: list[float] = []
    latencies_ms: list[float] = []
    predictions: list[dict[str, Any]] = []

    for i in range(n):
        x_in = np.asarray([states[i]], dtype=np.float32)
        t0 = time.perf_counter()
        pred = sess.run([out_name], {in_name: x_in})[0][0]  # [30, 4]
        latencies_ms.append((time.perf_counter() - t0) * 1000.0)

        gt = trajectories[i]
        step_dists = [
            math.hypot(float(pred[k][0]) - gt[k][0], float(pred[k][1]) - gt[k][1])
            for k in range(len(gt))
        ]
        ades.append(sum(step_dists) / len(step_dists))
        fdes.append(step_dists[-1])
        speed_errs.extend(abs(float(pred[k][2]) - gt[k][2]) for k in range(len(gt)))

        predictions.append({
            "prediction": [{"x": float(pred[k][0]), "y": float(pred[k][1])} for k in range(len(gt))],
            "ground_truth": [{"x": float(gt[k][0]), "y": float(gt[k][1])} for k in range(len(gt))],
        })

    lat_sorted = sorted(latencies_ms)
    summary = {
        "schema": "flowengine.flow_matching_eval.v1",
        "onnx_path": str(onnx_path),
        "samples": n,
        "ade_m": round(sum(ades) / max(1, len(ades)), 3),
        "fde_m": round(sum(fdes) / max(1, len(fdes)), 3),
        "speed_mae_mps": round(sum(speed_errs) / max(1, len(speed_errs)), 3),
        "latency_p50_ms": round(_percentile(lat_sorted, 50), 3),
        "latency_p95_ms": round(_percentile(lat_sorted, 95), 3),
        "latency_p99_ms": round(_percentile(lat_sorted, 99), 3),
    }
    return summary, predictions


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Evaluate Flow-Matching ONNX trajectory model.")
    ap.add_argument("--onnx", type=Path, required=True)
    ap.add_argument("--dataset", type=Path, required=True)
    ap.add_argument("--max-samples", type=int, default=300)
    ap.add_argument("--predictions-out", type=Path, default=None)
    args = ap.parse_args(argv)

    states, trajs = load_real_trajectory_dataset(args.dataset)
    summary, preds = evaluate_onnx(args.onnx, states, trajs, max_samples=args.max_samples)
    if args.predictions_out is not None:
        args.predictions_out.parent.mkdir(parents=True, exist_ok=True)
        args.predictions_out.write_text(
            json.dumps({"predictions": preds}, indent=2) + "\n",
            encoding="utf-8",
        )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
