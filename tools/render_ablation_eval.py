#!/usr/bin/env python3
"""render_ablation_eval.py — 3D 渲染退化链消融与下游保真度验证原型。

对应 3D 渲染策略：
  1. 保真度判据不看 FID/LPIPS，而看下游任务一致性（检测 mAP 差、位姿误差、ID switch、轨迹偏差）。
  2. 渲染参数消融实验（L0 干净光栅化 -> +噪声 -> +Rolling Shutter -> +ISP -> +压缩块效应）。
  3. 显式校验 ISO 8855 (X前 Y左 Z上) <-> OpenGL/Three.js (X右 Y上 Z后) 坐标转换正交性。
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from typing import Any

# ISO 8855 (X-forward, Y-left, Z-up) -> OpenGL/Three.js (X-right, Y-up, Z-backward)
# p_gl = R_ISO_TO_GL @ p_iso
R_ISO_TO_GL = (
    (0.0, -1.0, 0.0),
    (0.0, 0.0, 1.0),
    (-1.0, 0.0, 0.0),
)

DEGRADATION_STAGES = [
    {"id": "L0_clean", "label": "Clean Rasterization", "noise_std": 0.0, "rolling_shutter_ms": 0.0, "isp": False, "codec_q": 100},
    {"id": "L1_noise", "label": "+Sensor Noise", "noise_std": 0.025, "rolling_shutter_ms": 0.0, "isp": False, "codec_q": 100},
    {"id": "L2_rolling", "label": "+Rolling Shutter", "noise_std": 0.025, "rolling_shutter_ms": 12.0, "isp": False, "codec_q": 100},
    {"id": "L3_isp", "label": "+ISP (WB/Gamma/Vignette)", "noise_std": 0.025, "rolling_shutter_ms": 12.0, "isp": True, "codec_q": 100},
    {"id": "L4_codec", "label": "+Codec Compression", "noise_std": 0.025, "rolling_shutter_ms": 12.0, "isp": True, "codec_q": 75},
]


def iso_to_gl(x: float, y: float, z: float) -> tuple[float, float, float]:
    return (
        R_ISO_TO_GL[0][0] * x + R_ISO_TO_GL[0][1] * y + R_ISO_TO_GL[0][2] * z,
        R_ISO_TO_GL[1][0] * x + R_ISO_TO_GL[1][1] * y + R_ISO_TO_GL[1][2] * z,
        R_ISO_TO_GL[2][0] * x + R_ISO_TO_GL[2][1] * y + R_ISO_TO_GL[2][2] * z,
    )


def gl_to_iso(x: float, y: float, z: float) -> tuple[float, float, float]:
    # R_ISO_TO_GL is orthonormal, so inverse == transpose
    return (
        R_ISO_TO_GL[0][0] * x + R_ISO_TO_GL[1][0] * y + R_ISO_TO_GL[2][0] * z,
        R_ISO_TO_GL[0][1] * x + R_ISO_TO_GL[1][1] * y + R_ISO_TO_GL[2][1] * z,
        R_ISO_TO_GL[0][2] * x + R_ISO_TO_GL[1][2] * y + R_ISO_TO_GL[2][2] * z,
    )


def verify_coordinate_transform() -> None:
    # Determinant of R_ISO_TO_GL must be +1 (proper rotation, no mirror flip)
    a, b, c = R_ISO_TO_GL
    det = (
        a[0] * (b[1] * c[2] - b[2] * c[1])
        - a[1] * (b[0] * c[2] - b[2] * c[0])
        + a[2] * (b[0] * c[1] - b[1] * c[0])
    )
    assert abs(det - 1.0) < 1e-9, f"Expected det=+1, got {det}"

    # Check forward (1, 0, 0)_iso -> (0, 0, -1)_gl; left (0, 1, 0)_iso -> (-1, 0, 0)_gl; up (0, 0, 1)_iso -> (0, 1, 0)_gl
    assert iso_to_gl(1.0, 0.0, 0.0) == (0.0, 0.0, -1.0)
    assert iso_to_gl(0.0, 1.0, 0.0) == (-1.0, 0.0, 0.0)
    assert iso_to_gl(0.0, 0.0, 1.0) == (0.0, 1.0, 0.0)

    # Round-trip check
    p = (12.34, -2.5, 1.65)
    p_rt = gl_to_iso(*iso_to_gl(*p))
    for u, v in zip(p, p_rt):
        assert math.isclose(u, v, abs_tol=1e-9)


def run_dry_ablation() -> dict[str, Any]:
    """Generate a baseline ablation table structure for downstream task consistency."""
    rows = []
    # Illustrative dry-run schema showing the exact metrics tracked per stage
    for idx, st in enumerate(DEGRADATION_STAGES):
        rows.append({
            "stage": st["id"],
            "label": st["label"],
            "config": {
                "noise_std": st["noise_std"],
                "rolling_shutter_ms": st["rolling_shutter_ms"],
                "isp": st["isp"],
                "codec_q": st["codec_q"],
            },
            "downstream_metrics": {
                "sim2real_map_gap": round(0.14 - idx * 0.025, 4),
                "pose_err_m": round(0.32 - idx * 0.04, 4),
                "id_switch_rate": round(0.08 - idx * 0.012, 4),
                "traj_ade_m": round(0.45 - idx * 0.06, 4),
            },
        })
    return {
        "schema": "flowengine.render_ablation.v1",
        "mode": "dry-run",
        "stages": rows,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        verify_coordinate_transform()
        res = run_dry_ablation()
        assert len(res["stages"]) == 5
        print("✓ render_ablation_eval --self-test PASSED (ISO8855<->GL det=+1 & 5 ablation stages)")
        return 0
    if args.dry_run:
        print(json.dumps(run_dry_ablation(), indent=2))
        return 0
    ap.error("--self-test or --dry-run is required")
    return 2


if __name__ == "__main__":
    sys.exit(main())
