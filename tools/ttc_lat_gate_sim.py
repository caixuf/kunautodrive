#!/usr/bin/env python3
"""TTC 横向门限参考系仿真（L3-P2 sensor 编排卡死修复，Python 先行）。

【实测证据】config/pipeline_sensor.json + scenarios/r157_cut_out.json：
  ego 变道途中永久刹停在 v=0。
  planning 日志: "TTC follow: gap=3.06 (src=hint) -> 0.0 m/s (was 20.0)"
  behavior 日志: best_gap=-1（无前车）→ 要 20 m/s
  两模块结论**相反**，planning 把 ego 按死。

【根因】探针实测同一帧：
    邻道静止障碍 真值 y=-1.75（车宽 2.0 → 占 y∈[-2.75,-0.75]）
    感知质心     y=-2.75  ← DBSCAN 只聚到**可见近面**，偏向 ego 侧 1.0m
    ego=(41.68, -4.58, h=-0.109)，正从源车道 -1.75 切向目标车道 -5.25
  两条链路的横向判据**参考系不同**：
    behavior: |obs_d − current_lane_d| ≤ lane_w*0.5 + 0.6   （**车道相对**）
    planning: |obs_lat|（**ego 瞬时位置**为零点）≤ lane_w*0.75
  ego 变道**途中** ego_y 悬在两车道之间，后者失去意义：
    真值 lat=2.80 > 2.625 → 拒绝 → GT 编排正常（实测 PASS）
    感知 lat=1.82 < 2.625 → 收下 → gap=3.06 → 刹到 0（实测卡死）
  ⇒ 门限**恰好**卡在真值与感知两种表示之间，结论翻转。

【判据候选】
  A. 现状：|obs_lat(ego 为零点)|        ≤ 0.75*lane_w
  B. 车道相对：|obs 到应跟随车道中心线|  ≤ 0.5*lane_w + 0.6
     （与 behavior_planner 的 lead_lat_tol 同式同值，同参考系）

本脚本断言：B 必须同时满足
  (1) 邻道车（真值 / 感知偏 1m 两种表示）都**拒绝**
  (2) 目标车道前车（真值 / 感知 ±1m 偏差 / 40m 远处）都**收下**
A 在 (1) 上失败 —— 即卡死根因。
"""
import math

LANE_W = 3.5
LANE_SOURCE = -1.75     # ego 正在离开的源车道中心
LANE_TARGET = -5.25     # ego 变道目标车道中心（= 应跟随车道）
LAT_MAX_A = LANE_W * 0.75
LAT_MAX_B = LANE_W * 0.5 + 0.6          # = 2.35，与 behavior lead_lat_tol 同值

# ── 实测帧（来自 planning 探针，非构造值）─────────────────────────────
EGO = (41.68, -4.58, -0.109)            # x, y, heading
ADJACENT_TRUTH = (45.00, -1.75)         # 邻道静止障碍 真值（源车道）
ADJACENT_PERCEPT = (44.98, -2.75)       # 邻道静止障碍 DBSCAN 质心（偏 1.0m）
TARGET_LEADS = [
    ("目标道前车 真值 y=-5.25",       44.60, -5.25),
    ("目标道前车 感知偏 +1m y=-4.25",  44.60, -4.25),
    ("目标道前车 感知偏 -1m y=-6.25",  44.60, -6.25),
    ("目标道前车 远处 40m 同车道",     81.00, -5.25),
]


def body_lat(ego, ox, oy):
    """obs 在 ego 车体系下的横向坐标（左为正）——候选 A 用的量。
    与 C 侧 planning_ttc_candidate.h 原式 `-rx*fwd_y + ry*fwd_x` 同式。"""
    ex, ey, eh = ego
    fx, fy = math.cos(eh), math.sin(eh)
    rx, ry = ox - ex, oy - ey
    return -rx * fy + ry * fx


def lane_lat(oy, lane_center_y):
    """obs 到**车道中心线**的横向距离——候选 B 用的量（Frenet 语义）。

    直道上车道中心线是 y=const 的直线，故横向距离 = |oy − lane_center_y|。
    注意：**不含 ego 位置**，这是与 A 的本质区别；也正是 C 侧改用
    `obs_d − follow_lane_center_d`（逐障碍 Frenet 投影）的等价直道形式。
    远处前车（40m）不因 heading≠0 产生 along·sin(h) 伪影。
    """
    return abs(oy - lane_center_y)


def crit_a(ego, ox, oy):
    return abs(body_lat(ego, ox, oy)) <= LAT_MAX_A


def crit_b(oy):
    return lane_lat(oy, LANE_TARGET) <= LAT_MAX_B


def main():
    ex, ey, eh = EGO
    print("=" * 78)
    print("实测帧：ego=(%.2f, %.2f, h=%.3f)  源车道 y=%.2f  目标车道 y=%.2f"
          % (ex, ey, eh, LANE_SOURCE, LANE_TARGET))
    print("=" * 78)
    print()
    print("A 门限 = 0.75*lane_w = %.3f（ego 为零点）" % LAT_MAX_A)
    print("B 门限 = 0.5*lane_w+0.6 = %.3f（车道中心线为参考，与 behavior 同值）" % LAT_MAX_B)
    print()

    def row(label, ox, oy, expect):
        la = body_lat(EGO, ox, oy)
        lb = lane_lat(oy, LANE_TARGET)
        a = crit_a(EGO, ox, oy)
        b = crit_b(oy)
        ok = (b == expect)
        print("| %-32s | %6.2f | %6.2f | %-4s | %-4s | %s |"
              % (label, la, lb, "收下" if a else "拒绝", "收下" if b else "拒绝",
                 "✅" if ok else "❌"))
        return a, b, ok

    print("### (1) 邻道车 —— 期望**拒绝**（它不在应跟随车道，不该刹停）")
    print()
    print("| 用例 | A: 车体系 lat | B: 车道线距 | A | B | B 正确? |")
    print("|---|---|---|---|---|---|")
    a_t, b_t, _ = row("邻道车 真值 y=-1.75", *ADJACENT_TRUTH, False)
    a_p, b_p, _ = row("邻道车 感知 y=-2.75(偏1m)", *ADJACENT_PERCEPT, False)
    print()
    print("### (2) 目标车道前车 —— 期望**收下**（漏掉真前车更危险）")
    print()
    print("| 用例 | A: 车体系 lat | B: 车道线距 | A | B | B 正确? |")
    print("|---|---|---|---|---|---|")
    oks = [row(lbl, ox, oy, True)[2] for lbl, ox, oy in TARGET_LEADS]
    print()

    print("=" * 78)
    # 注：A 的失败是**被诊断的 bug 本身**，不是本仿真的失败条件。
    # 通过条件 = B 同时满足 (1)(2)。
    fail = []
    if b_t or b_p:
        fail.append("B 收下了邻道车（真值或感知表示）")
    if not all(oks):
        fail.append("B 漏掉目标车道真前车")
    if fail:
        print("结论：B **不通过**")
        for f in fail:
            print("  ✗", f)
        return 1
    print("结论：**B（车道相对）通过** —— 邻道车（真值/感知偏 1m）均拒绝，")
    print("      目标道前车（真值/±1m 感知偏差/40m 远处）均收下。")
    print()
    print("对照 A（现状，ego 瞬时位置为零点）：")
    if a_p:
        print("  ✗ 把**感知**邻道车当跟车目标（车体系 lat=%.2f < %.2f）→ 实测"
              % (body_lat(EGO, *ADJACENT_PERCEPT), LAT_MAX_A))
        print("    gap=3.06 → command_speed 0 → 永久卡死（r157_cut_out）")
    if not a_t:
        print("  · 真值表示 lat=%.2f 恰好在门外 → 两种表示结论翻转 = 门限卡在中间"
              % body_lat(EGO, *ADJACENT_TRUTH))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
