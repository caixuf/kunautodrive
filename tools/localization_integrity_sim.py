#!/usr/bin/env python3
"""定位完整性交叉校验 Python 仿真先行（L3-P2，方向六）。

为什么存在：P2 要求「冗余（雷达模型、**定位交叉校验**、看门狗）」。当前
`fusion_node` 有 **GPS + LiDAR/SLAM 双源**，但两者之间**没有任何互校**：
  · 只有一个 `diverged` 单标志（χ² 连续失败 >10 或协方差 trace>1e9 才置位）；
  · `EkfFusion.gated_count` 被算出来却**从未发布**（源码注释自己写了
    "fusion_node 应把它发布出去"）；
  · `DEGRADE_REASON_LOCALIZATION` 在 `degrade_ladder.h` 里定义、在
    `degrade_reason_name()` 里有名字，但**全仓没有任何地方 set 它** —— 死原因码。
于是"定位还准不准"在遥测里完全不可见，门禁也无从断言。本文件按 CLAUDE.md
铁律先定判定规则（纯函数 + 自检），再逐条移植到 C++。

模型边界（KISS）：
  · 只建判定"定位是否可信"所需的量，不建 EKF 内部矩阵运算。
  · 交叉校验的两个独立源（默认编排里实际存在的）：
      1. LiDAR 位置 (x,y)   —— sensor_model 发的位置
      2. GPS 速度/航向      —— sensor_model 发的 speed_mps / heading_deg
    同一物理量的两个独立测量 → 不一致即完整性下降（经典双源互校）。
  · SLAM `sensor/pose` 在默认编排里**无发布者**（见 pipeline.json 的
    allow_hung_subs），故不作前置要求；有则加分，无则不判失败。

运行：
  python3 tools/localization_integrity_sim.py --run-all    # 场景 + 自检
  python3 tools/localization_integrity_sim.py --check-rules # 规则自洽性
退出码 = 失败断言数（可作 ctest 入口）。
"""

import argparse
import math
import sys

# ── 与 C 侧一致的阈值（写进 include/localization_integrity.h，勿在别处再写一份）──
P = {
    # 源陈旧阈值（ms）：超过即认为该源不可用
    "source_stale_ms":            500.0,
    # 位置-速度一致性：LiDAR 位置差分算出的速度 vs GPS 速度
    "speed_agree_tol_mps":        3.0,    # 绝对差 > 此值 → 两源不一致
    "speed_agree_rel":            0.35,   # 或相对差 > 35%
    # 位置-航向一致性：LiDAR 位移方向 vs GPS 航向
    "heading_agree_tol_rad":      math.radians(25.0),
    # 需要多快速度以上才做航向/速度比对（低速时方向噪声大，比了没意义）
    "min_speed_for_check_mps":     3.0,
    # EKF χ² 拒绝率上限（被拒观测占比）
    "gated_ratio_max":             0.50,
    # 协方差 trace 上限（滤波发散前兆）
    "cov_trace_max":               100.0,
    # 连续不一致多久才升级为降级（去抖，抗单帧噪声）
    "disagree_dwell_s":            1.0,
}

# ── 完整性原因码（与 C 侧 LOCALIZATION_* 宏逐条对应）──
REASON_OK              = "ok"
REASON_GPS_STALE       = "gps_stale"
REASON_LIDAR_STALE     = "lidar_stale"
REASON_ALL_SOURCES_LOST = "all_sources_lost"
REASON_SPEED_MISMATCH  = "speed_mismatch"
REASON_HEADING_MISMATCH = "heading_mismatch"
REASON_FILTER_GATED    = "filter_gated"
REASON_COVARIANCE_GROWN = "covariance_grown"

# 会导致"定位不可信"的原因（→ 应触发 DEGRADE_REASON_LOCALIZATION）
CRITICAL_REASONS = (
    REASON_ALL_SOURCES_LOST,
    REASON_SPEED_MISMATCH,
    REASON_HEADING_MISMATCH,
    REASON_FILTER_GATED,
    REASON_COVARIANCE_GROWN,
)


def _angle_diff(a, b):
    """两角之差，归一化到 (-pi, pi]。"""
    d = (a - b) % (2.0 * math.pi)
    if d > math.pi:
        d -= 2.0 * math.pi
    return d


def check_speed_agreement(lidar_x, lidar_y, prev_x, prev_y, dt,
                          gps_speed_mps, p=P):
    """交叉校验 ①：LiDAR 位置差分速度 vs GPS 速度。

    两个独立源测同一物理量。任一源缺失（prev 为 None 或 gps 为 None）→ 返回
    None（无法判定，**不等于通过** —— 与仓库 require 语义一致）。

    低速时位置差分被噪声主导（|Δpos| 的量化误差 /dt 会放大），故低于
    min_speed_for_check_mps 时不判。
    """
    if prev_x is None or prev_y is None or gps_speed_mps is None or dt <= 0.0:
        return None
    lidar_speed = math.hypot(lidar_x - prev_x, lidar_y - prev_y) / dt
    if max(lidar_speed, abs(gps_speed_mps)) < p["min_speed_for_check_mps"]:
        return None
    diff = abs(lidar_speed - abs(gps_speed_mps))
    rel = diff / max(lidar_speed, abs(gps_speed_mps), 1e-6)
    ok = (diff <= p["speed_agree_tol_mps"]) or (rel <= p["speed_agree_rel"])
    return {"ok": ok, "lidar_speed": lidar_speed, "gps_speed": abs(gps_speed_mps),
            "diff": diff, "rel": rel}


def check_heading_agreement(lidar_x, lidar_y, prev_x, prev_y,
                            gps_heading_rad, p=P):
    """交叉校验 ②：LiDAR 位移方向 vs GPS 航向。

    同样需要足够速度（低速时位移方向不可靠），且位移要够大才免于量化噪声。
    """
    if prev_x is None or prev_y is None or gps_heading_rad is None:
        return None
    dx, dy = lidar_x - prev_x, lidar_y - prev_y
    if math.hypot(dx, dy) < 0.2:   # 位移过小，方向不可信
        return None
    lidar_heading = math.atan2(dy, dx)
    diff = abs(_angle_diff(lidar_heading, gps_heading_rad))
    return {"ok": diff <= p["heading_agree_tol_rad"],
            "lidar_heading": lidar_heading, "gps_heading": gps_heading_rad,
            "diff": diff}


def assess_integrity(gps_age_ms, lidar_age_ms, speed_check, heading_check,
                     gated_ratio, cov_trace, p=P):
    """综合判定定位完整性。返回 (trustworthy: bool, reasons: list[str])。

    ⚠️ 返回值语义（关键，勿误读）：``trustworthy`` = **定位结果仍可用于驾驶**，
    而不是"零告警"。单源陈旧（gps_stale / lidar_stale）时冗余正在生效、定位仍
    可信 → trustworthy 为 True，但 reasons 里仍列出该源已失效（供遥测/dashboard
    显示"当前在单源模式"）。

    判定顺序即优先级：源全丢 > 源不一致 > 滤波器不健康。
    ``reasons`` 是**全部**命中的原因（便于诊断），不只是第一个。
    """
    reasons = []

    gps_ok = gps_age_ms is not None and gps_age_ms <= p["source_stale_ms"]
    lidar_ok = lidar_age_ms is not None and lidar_age_ms <= p["source_stale_ms"]

    if not gps_ok and not lidar_ok:
        # 两个独立的位姿/速度源都没了 → 定位真丢（不是"某一路降级"）
        reasons.append(REASON_ALL_SOURCES_LOST)
    else:
        if not gps_ok:
            reasons.append(REASON_GPS_STALE)
        if not lidar_ok:
            reasons.append(REASON_LIDAR_STALE)

    # 交叉校验只在两源都可用时才有意义（单源无从互校）
    if gps_ok and lidar_ok:
        if speed_check is not None and not speed_check["ok"]:
            reasons.append(REASON_SPEED_MISMATCH)
        if heading_check is not None and not heading_check["ok"]:
            reasons.append(REASON_HEADING_MISMATCH)

    if gated_ratio is not None and gated_ratio > p["gated_ratio_max"]:
        reasons.append(REASON_FILTER_GATED)
    if cov_trace is not None and cov_trace > p["cov_trace_max"]:
        reasons.append(REASON_COVARIANCE_GROWN)

    critical = [r for r in reasons if r in CRITICAL_REASONS]
    return (len(critical) == 0), reasons


def should_escalate(reasons_history, dt_s, p=P):
    """去抖：连续不一致累计超过 disagree_dwell_s 才升级为降级。

    单帧不一致可能是量化噪声 / 消息对齐抖动；直接用会把正常场景打成降级。
    与 degrade_ladder 的心跳去抖（timeout_since + 150ms 确认）同一手法。
    """
    if not reasons_history:
        return False
    return len(reasons_history) * dt_s >= p["disagree_dwell_s"]


# ── 场景 ──────────────────────────────────────────────────────────────
def scenario_healthy():
    """两源一致、滤波健康 → 必须判 ok。"""
    return assess_integrity(
        gps_age_ms=20.0, lidar_age_ms=20.0,
        speed_check={"ok": True}, heading_check={"ok": True},
        gated_ratio=0.0, cov_trace=1.0)


def scenario_gps_lost():
    """GPS 丢、LiDAR 在 → 单源降级（gps_stale），但**不是** all_sources_lost，
    且不应做交叉校验（单源无从互校）→ 不得报 mismatch。"""
    return assess_integrity(
        gps_age_ms=5000.0, lidar_age_ms=20.0,
        speed_check=None, heading_check=None,
        gated_ratio=0.0, cov_trace=1.0)


def scenario_all_sources_lost():
    """两源全丢 → all_sources_lost（critical）。"""
    return assess_integrity(
        gps_age_ms=5000.0, lidar_age_ms=5000.0,
        speed_check=None, heading_check=None,
        gated_ratio=None, cov_trace=None)


def scenario_speed_mismatch():
    """两源都在但速度差得离谱（LiDAR 说 15、GPS 说 3）→ mismatch。"""
    sc = check_speed_agreement(15.0, 0.0, 0.0, 0.0, 1.0, 3.0)
    return assess_integrity(
        gps_age_ms=20.0, lidar_age_ms=20.0,
        speed_check=sc, heading_check=None,
        gated_ratio=0.0, cov_trace=1.0)


def scenario_filter_gated():
    """观测被 χ² 大量拒绝 → 滤波器没在跟踪（_gated_count 那个 TODO）。"""
    return assess_integrity(
        gps_age_ms=20.0, lidar_age_ms=20.0,
        speed_check={"ok": True}, heading_check={"ok": True},
        gated_ratio=0.9, cov_trace=1.0)


# ── 自检 ──────────────────────────────────────────────────────────────
def check_rules():
    """规则自洽性：原因码集合与 critical 集合一致、阈值合法。"""
    fails = []
    for key, val in P.items():
        if not isinstance(val, (int, float)) or val <= 0:
            fails.append(f"threshold '{key}' must be positive, got {val}")
    if not set(CRITICAL_REASONS).issubset({
            REASON_ALL_SOURCES_LOST, REASON_SPEED_MISMATCH, REASON_HEADING_MISMATCH,
            REASON_FILTER_GATED, REASON_COVARIANCE_GROWN}):
        fails.append("CRITICAL_REASONS contains an unknown reason code")
    # 单源陈旧不该是 critical：那正是"冗余生效"而不是"定位丢失"
    if REASON_GPS_STALE in CRITICAL_REASONS or REASON_LIDAR_STALE in CRITICAL_REASONS:
        fails.append("single-source staleness must NOT be critical "
                     "(redundancy working is not a localization failure)")
    return fails


def run_all():
    failures = list(check_rules())

    ok, reasons = scenario_healthy()
    if not ok or reasons:
        failures.append(f"healthy case must be ok with no reasons, got ok={ok} {reasons}")

    ok, reasons = scenario_gps_lost()
    # 单源陈旧 = 冗余生效：定位仍可信（trustworthy），但必须如实报出该源已失效
    if not ok:
        failures.append("GPS loss with LiDAR alive must stay trustworthy "
                        "(redundancy working)")
    if REASON_ALL_SOURCES_LOST in reasons:
        failures.append("GPS loss alone must NOT be all_sources_lost (LiDAR still available)")
    if REASON_GPS_STALE not in reasons:
        failures.append(f"GPS loss must report gps_stale, got {reasons}")
    if REASON_SPEED_MISMATCH in reasons or REASON_HEADING_MISMATCH in reasons:
        failures.append("single source must not be cross-checked (no second source to compare)")

    ok, reasons = scenario_all_sources_lost()
    if ok or REASON_ALL_SOURCES_LOST not in reasons:
        failures.append(f"both sources lost must be critical, got ok={ok} {reasons}")

    ok, reasons = scenario_speed_mismatch()
    if ok or REASON_SPEED_MISMATCH not in reasons:
        failures.append(f"speed mismatch must be critical, got ok={ok} {reasons}")

    ok, reasons = scenario_filter_gated()
    if ok or REASON_FILTER_GATED not in reasons:
        failures.append(f"filter gating must be critical, got ok={ok} {reasons}")

    # 低速时不做速度/航向比对（位置差分被噪声主导）
    if check_speed_agreement(1.0, 0.0, 0.9, 0.0, 1.0, 0.5) is not None:
        failures.append("speed check must abstain below min_speed_for_check_mps")
    # 缺一源 → 无法判定（None），不是"通过"
    if check_speed_agreement(10.0, 0.0, None, None, 1.0, 10.0) is not None:
        failures.append("speed check must abstain when a source is missing")

    # 去抖：单帧不一致不该立刻升级
    if should_escalate([REASON_SPEED_MISMATCH], dt_s=0.05):
        failures.append("a single disagreeing frame must not escalate (debounce)")
    if not should_escalate([REASON_SPEED_MISMATCH] * 25, dt_s=0.05):
        failures.append("sustained disagreement (>1s) must escalate")

    # 角度归一化正确性
    if abs(_angle_diff(0.0, 2.0 * math.pi - 0.1) - 0.1) > 1e-9:
        failures.append("_angle_diff must wrap across the ±pi boundary")
    return failures


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run-all", action="store_true", help="场景 + 自检")
    ap.add_argument("--check-rules", action="store_true", help="仅规则自洽性")
    args = ap.parse_args()

    if args.check_rules:
        fails = check_rules()
        for f in fails:
            print(f"  FAIL {f}")
        print(f"localization rules: {'FAIL' if fails else 'OK'}")
        return len(fails)

    print("── 定位完整性交叉校验（trustworthy = 定位仍可用于驾驶）──")
    for name, fn in (("healthy              ", scenario_healthy),
                     ("gps_lost             ", scenario_gps_lost),
                     ("all_sources_lost     ", scenario_all_sources_lost),
                     ("speed_mismatch       ", scenario_speed_mismatch),
                     ("filter_gated         ", scenario_filter_gated)):
        ok, reasons = fn()
        print(f"  {'ok  ' if ok else 'DEGRADE'} {name} reasons={reasons or ['none']}")

    failures = run_all()
    for f in failures:
        print(f"  FAIL {f}")
    print(f"\n结果: {'PASS' if not failures else 'FAIL'} ({len(failures)} 项失败)")
    return len(failures)


if __name__ == "__main__":
    sys.exit(main())
