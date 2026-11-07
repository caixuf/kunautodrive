#!/usr/bin/env python3
"""影子评估 / 晋级门禁的共享契约（单一事实源）。

为什么单独成文件：同一组阈值此前散落在
  - `tools/evaluate_shadow_mode.py` 的 THRESHOLDS
  - `tools/modelctl.py` 的 PROMOTE_SHADOW_* 字面量
  - `modules/adas_nodes/safety_arbiter.h` 的 C 侧包络
以及 sidecar 字段清单在 `inference_node.cpp`（产出）、`learning_loop.py`
（透传）、`modelctl.py`（消费）三处手同步 —— 2026-11-07 实测就因漏同步
`shadow_steer_mae` 导致正规学习闭环产物全被 promote 拒绝。

本文件只放**纯常量与纯函数**，不 import 任何仓库内模块，避免循环依赖。
C 侧 `SAFETY_ARBITER_STEER_ENVELOPE_RAD` 仍需另用 gate 比对（见 TODO）。
"""

from __future__ import annotations

import math

SCHEMA = "flowengine.shadow_eval.v1"

# 门禁阈值基准（与 Apollo / KunAutoDrive L3 规范对齐）。
# 角度类以“度”保存（人工可读），rad 由调用方换算。
THRESHOLDS = {
    # 影子模式精度
    "speed_mae_warn": 2.0,      # m/s
    "speed_mae_fail": 5.0,      # m/s
    "steer_mae_warn_deg": 4.0,  # deg (~0.07 rad)
    "steer_mae_fail_deg": 6.9,  # deg (~0.12 rad，Simplex 安全包络上限)
    "ade_warn_m": 1.5,          # m (3s 时域内平均位移误差)
    "ade_fail_m": 3.0,          # m
    "fde_warn_m": 2.5,          # m (3s 终点位移误差)
    "fde_fail_m": 5.0,          # m
    "arbiter_intervention_max_pct": 10.0,  # 仲裁介入率上限 (%)

    # 闭环动力学与舒适度
    "jerk_max": 12.0,           # m/s^3
    "min_ttc_warn_s": 2.5,      # s
    "min_ttc_fail_s": 1.2,      # s
    "min_gap_fail_m": 0.5,      # m
}

# ── promote（晋级）门禁 ──
PROMOTE_SHADOW_MAE_MAX = THRESHOLDS["speed_mae_warn"]
PROMOTE_SHADOW_MIN_N = 50
PROMOTE_SHADOW_STEER_MAE_MAX_RAD = math.radians(THRESHOLDS["steer_mae_fail_deg"])
PROMOTE_SHADOW_ADE_MAX_M = THRESHOLDS["ade_fail_m"]
PROMOTE_SHADOW_FDE_MAX_M = THRESHOLDS["fde_fail_m"]

# sidecar → shadow_eval.json → promote_gate 的字段集合。
# 新增门禁判据时必须同时更新这里与 inference sidecar 的产出。
SHADOW_SIDECAR_KEYS = (
    "shadow_speed_mae",
    "shadow_speed_rmse",
    "shadow_speed_mae_settled",
    "shadow_speed_rmse_settled",
    "shadow_settled_n",
    "shadow_n",
    "shadow_steer_mae",
    "shadow_steer_rmse",
    "shadow_steer_n",
    "shadow_ade",
    "shadow_fde",
    "shadow_ade_mean",
    "shadow_fde_mean",
    "shadow_gate_supported",
    "model",
)

# 数值型门禁键 → (别名, 上限, 单位, 缺失是否拒绝)
PROMOTE_NUMERIC_CHECKS = (
    # 转向偏差：缺字段 = 无法判定 → 拒绝
    ("shadow_steer_mae", None, PROMOTE_SHADOW_STEER_MAE_MAX_RAD, "rad", True),
    # 轨迹误差：速度-only backend 可能没有 rollout → 有则判、无则跳过
    ("shadow_ade_mean", "shadow_ade", PROMOTE_SHADOW_ADE_MAX_M, "m", False),
    ("shadow_fde_mean", "shadow_fde", PROMOTE_SHADOW_FDE_MAX_M, "m", False),
)


def shadow_value(shadow: dict, primary: str, alias: str | None = None):
    """取指标值；主键为 None/null 时回退别名（dict.get 的默认值不会回退）。"""
    value = shadow.get(primary)
    if value is None and alias is not None:
        value = shadow.get(alias)
    return value