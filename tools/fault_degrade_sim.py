#!/usr/bin/env python3
"""故障注入 → 降级路径 矩阵 Python 仿真先行（L3-P2，方向六）。

为什么存在：P2 的放行条件是「**每类注入故障都有可观测的降级路径**」。要断言
"每类"，先得把"类"与"路径"定义成一个可判定的表。CLAUDE.md 铁律要求算法/接口
升级先 Python 仿真：本文件是该表的**单一事实源**，C 侧 supervisor 的转移语义
（`src/core/degrade_ladder.c::degrade_supervisor_tick` + `safety_control_node.cpp`
的 raw_cmd 超时分支）在此逐条镜像，`tools/fault_matrix_check.py` 与 CI 门禁按
它去比对**实跑观测到的** metrics.degrade。

模型边界（KISS，只建判定所需的量）：
  · supervisor 心跳超时递进 = 纯函数（多节点/单节点 × >500ms/>2000ms）。
  · safety_control 的 raw_cmd 超时 = 「车在动 + raw_cmd 陈旧 >2s」才升 L3
    （车已停稳时停发 raw_cmd 属正常，不得误判 —— 这个守卫本身也要被守住）。
  · 不建物理/规划，故本文件只判定"降级等级与原因码"，不判定车速轨迹。

运行：
  python3 tools/fault_degrade_sim.py --run-all        # 全部故障类 + 自检
  python3 tools/fault_degrade_sim.py --check-matrix   # 表自洽性（无死类/无空路径）
  python3 tools/fault_degrade_sim.py --list           # 打印故障类清单
退出码 = 失败断言数（可作 ctest 入口）。
"""

import argparse
import sys

# ── 与 C 侧一致的常量（勿在别处再写一份）─────────────────────────────
DEGRADE_L0, DEGRADE_L1, DEGRADE_L2, DEGRADE_L3 = 0, 1, 2, 3

REASON_NONE = "none"
REASON_PLANNING_TO = "planning_timeout"
REASON_CONTROL_TO = "control_timeout"
REASON_FUSION_TO = "fusion_timeout"
REASON_SENSOR_TO = "sensor_timeout"
REASON_LARGE_CTE = "large_cte"
REASON_COLLISION = "collision_risk"
REASON_LOCALIZATION = "localization_lost"
REASON_MANUAL = "manual"
REASON_HEARTBEAT = "heartbeat_lost"

# supervisor 阈值（镜像 degrade_ladder.c）
HB_TIMEOUT_MS = 500      # 心跳超时窗口
HB_CONFIRM_MS = 150      # 额外持续确认（去抖）
HB_LOST_MS = 2000        # 失联阈值
HB_RECOVER_MS = 3000     # 全健康持续 3s 才自动恢复
RAW_CMD_TIMEOUT_US = 2_000_000   # safety_control 的 raw_cmd 超时


# ── 故障类定义：每类 = 一个注入规格 + 期望的降级路径 ──────────────────
# 注入规格用 FLOW_FAULT_INJECT 的语法（见 include/fault_inject.h）：
#   <topic>:<after_ms>[:<duration_ms>]
#
# 期望字段：
#   level         期望达到的**最低**降级等级（>= 判定，允许更高）
#   reason_names  期望的原因码（出现其一即可）；空元组 = 不检查原因码
#   odd_violation 期望 ODD 分类器给出的 violation（可选，None=不检查）
#   control_mode  期望 control/debug 的 mode（可选，None=不检查）
#   observable_via 该类故障"可观测路径"的落点。取值：
#                   "degrade"      — 靠 metrics.degrade.level/reason_name 观测
#                   "control_mode" — 靠 metrics.control_debug.mode（如 DATA_TIMEOUT fallback）
#                   "topic_freq"   — 靠 topic 频率门禁（topic 归零即证据）
#                   "inject_ack"   — 靠 metrics.fault_inject.dropped（注入确已生效）
#                   P2 放行条件要求每类都至少有一个落点；空元组 = 违规（会被
#                   --check-matrix 判 FAIL）。用**显式枚举**而不是在 note 里
#                   grep 关键词——后者会因措辞变化而静默失效。
#   note          人读说明
#
# ⚠️ level/reason/control_mode 全部来自**实测**（2026-10-11 逐类真跑探针），
#    不是推断。改本表前请重跑 ci/gates/fault_inject_gate.py 核对。
FAULT_MATRIX = [
    {
        "id": "localization_loss",
        "inject": "fusion/localization:4000",
        "level": DEGRADE_L1,
        "reason_names": (REASON_FUSION_TO, REASON_LOCALIZATION),
        "odd_violation": "loc_stale",
        "control_mode": None,
        "observable_via": ("degrade", "inject_ack"),
        "note": "定位链路丢失（实测丢弃 603 条）→ safety_control 判 fusion 丢失 → L1/fusion_timeout；"
                "ODD 同步判 loc_stale（定位龄期超限）。这是当前唯一走到**降级等级**的注入类",
    },
    {
        "id": "perception_loss",
        "inject": "perception/obstacles:4000",
        "level": DEGRADE_L0,
        "reason_names": (),
        "odd_violation": None,
        "control_mode": None,
        "observable_via": ("inject_ack", "topic_freq"),
        "note": "感知障碍丢失（实测丢弃 402 条）：安全层按'无目标'巡航，**不升降级**（正确的降级语义："
                "不该为了没看见障碍就降级），control 进 ROAD_GUARD 保持车道。可观测证据 = 注入确认 + "
                "perception/obstacles 频率归零，不是降级等级",
    },
    {
        "id": "planning_loss",
        "inject": "planning/trajectory:4000",
        "level": DEGRADE_L0,
        "reason_names": (),
        "odd_violation": None,
        "control_mode": "DATA_TIMEOUT",
        "observable_via": ("control_mode", "inject_ack", "topic_freq"),
        "note": "规划轨迹丢失（实测丢弃 299 条）→ control 进 DATA_TIMEOUT 车道保持 fallback。"
                "supervisor 侧因 planning 心跳仍在 → 不降级，故证据是 control_mode 而非 degrade。"
                "⚠️ 本类曾**完全不可观测**：fallback 分支在发布 control/debug 之前 continue，"
                "本轮补了最小 debug 块才可见（由本门禁发现）",
    },
    {
        "id": "raw_cmd_timeout",
        "inject": "control/raw_cmd:4000",
        "level": DEGRADE_L0,
        "reason_names": (),
        "odd_violation": None,
        "control_mode": None,
        "observable_via": ("inject_ack", "topic_freq"),
        "note": "raw_cmd 丢失（实测丢弃 403 条）：车随即停稳（speed<=0.5）→ safety_control 的 "
                "moving 守卫**抑制** L3。这是刻意的：无条件升 L3 会造成「停→L3→起步→L3」"
                "无限拉锯（见 safety_control_node.cpp 注释）。守卫生效本身就是被验证的行为；"
                "行驶中失联的 L3 路径由 safety_fault_evidence 单测覆盖",
    },
    {
        "id": "gps_loss_redundancy",
        "inject": "sensor/gps:4000",
        "level": DEGRADE_L0,
        "reason_names": (),
        "odd_violation": None,
        "control_mode": None,
        "loc_reasons": ("gps_stale",),
        "loc_trustworthy": True,
        "observable_via": ("loc_integrity", "inject_ack", "topic_freq"),
        "note": "GPS 单源丢失（实测丢弃 ~400 条）：**冗余生效** —— LiDAR 仍在，"
                "fusion 的交叉校验如实上报 gps_stale，但 loc_trustworthy 保持 true、"
                "**不得降级**。这正是 P2「冗余」要断言的行为：单路失效不等于定位丢失",
    },
    {
        "id": "all_sources_lost",
        "inject": "sensor/gps:4000;sensor/lidar:4000",
        "level": DEGRADE_L2,
        "reason_names": (REASON_LOCALIZATION,),
        "odd_violation": "loc_untrustworthy",
        "control_mode": None,
        "loc_reasons": ("all_sources_lost",),
        "loc_trustworthy": False,
        "observable_via": ("degrade", "loc_integrity", "inject_ack"),
        "note": "两个独立源全丢 → fusion 判 all_sources_lost（critical）→ 去抖 1s 后"
                "置 L2/localization_lost。**这条把 DEGRADE_REASON_LOCALIZATION 从死码"
                "变成活码**：此前它定义在 degrade_ladder.h、在 degrade_reason_name() 里"
                "有名字，但全仓没有任何地方 set 它",
    },
]

# 合法的可观测落点（新增落点必须同时补上对应的门禁实现）
OBSERVABLE_VIA = ("degrade", "control_mode", "topic_freq", "inject_ack", "loc_integrity")


def supervisor_progression(node_ages_ms, now_ms, current_level=DEGRADE_L0):
    """镜像 degrade_supervisor_tick 的递进策略（不含自动恢复，单次判定）。

    node_ages_ms: 各受监控节点的心跳龄期（ms）
    返回 (level, reason_name)
    """
    timeout_count = sum(1 for a in node_ages_ms if a > HB_TIMEOUT_MS)
    timeout_1s_count = sum(1 for a in node_ages_ms if a > HB_LOST_MS)

    if timeout_1s_count >= 2:
        return DEGRADE_L3, REASON_PLANNING_TO
    if timeout_1s_count >= 1:
        return DEGRADE_L2, REASON_CONTROL_TO
    if timeout_count >= 2:
        return DEGRADE_L2, REASON_PLANNING_TO
    if timeout_count >= 1:
        return DEGRADE_L1, REASON_CONTROL_TO
    return DEGRADE_L0, REASON_NONE


def raw_cmd_timeout_escalates(age_us, speed_mps, has_state=True):
    """镜像 safety_control 的 raw_cmd 超时分支：仅在**车在动**时升 L3。

    这条守卫是真实存在且必须守住的：车已停稳时 raw_cmd 停发属正常行为，
    若无条件升 L3，会出现「车停→L3清除→起步→raw_cmd陈旧→L3→刹车→停稳→
    恢复→起步」的无限拉锯（见 safety_control_node.cpp 的注释）。
    """
    if not has_state:
        return False
    moving = speed_mps > 0.5
    return moving and age_us > RAW_CMD_TIMEOUT_US


def fault_class_observable(entry):
    """该类故障是否有可观测的降级路径。

    判定口径（与 P2 放行条件对齐）：显式声明了至少一个可观测落点。
    """
    via = entry.get("observable_via") or ()
    return len(via) > 0


def check_matrix():
    """表自洽性：无重复 id、无非法等级、落点合法、每类都有可观测路径。"""
    failures = []
    seen = set()
    for e in FAULT_MATRIX:
        if e["id"] in seen:
            failures.append(f"duplicate fault class id: {e['id']}")
        seen.add(e["id"])
        if e["level"] not in (DEGRADE_L0, DEGRADE_L1, DEGRADE_L2, DEGRADE_L3):
            failures.append(f"{e['id']}: invalid level {e['level']}")
        if not e["inject"]:
            failures.append(f"{e['id']}: empty injection spec")
        for via in e.get("observable_via") or ():
            if via not in OBSERVABLE_VIA:
                failures.append(f"{e['id']}: unknown observable_via '{via}' "
                                f"(valid: {', '.join(OBSERVABLE_VIA)})")
        if not fault_class_observable(e):
            failures.append(
                f"{e['id']}: no observable degrade path (observable_via empty) — "
                f"P2 requires every injected fault to have one")
        # level>=L1 的类必须以 degrade 落点为证，否则等级断言无处可查
        if e["level"] > DEGRADE_L0 and "degrade" not in (e.get("observable_via") or ()):
            failures.append(f"{e['id']}: expects L{e['level']} but does not observe 'degrade'")
        # 声明了 control_mode 期望的类必须以 control_mode 为落点（否则断言无依据）
        if e.get("control_mode") and "control_mode" not in (e.get("observable_via") or ()):
            failures.append(f"{e['id']}: expects control_mode '{e['control_mode']}' "
                            f"but does not declare 'control_mode' in observable_via")
        # 声明了 odd_violation 的类必须以 degrade 为落点（ODD 与降级同源于 odd/state）
        if e.get("odd_violation") and "degrade" not in (e.get("observable_via") or ()):
            failures.append(f"{e['id']}: expects ODD violation '{e['odd_violation']}' "
                            f"but does not declare 'degrade' in observable_via")
        # 声明了 loc_reasons 的类必须以 loc_integrity 为落点（否则断言无依据）
        if e.get("loc_reasons") and "loc_integrity" not in (e.get("observable_via") or ()):
            failures.append(f"{e['id']}: expects loc_reasons {e['loc_reasons']} "
                            f"but does not declare 'loc_integrity' in observable_via")
        # 声明 loc_trustworthy 的类同样需要 loc_integrity 落点
        if e.get("loc_trustworthy") is not None and "loc_integrity" not in (e.get("observable_via") or ()):
            failures.append(f"{e['id']}: declares loc_trustworthy but no 'loc_integrity' "
                            f"in observable_via")
        if (e.get("loc_reasons") or e.get("loc_trustworthy") is not None) and \
                "inject_ack" not in (e.get("observable_via") or ()):
            failures.append(f"{e['id']}: localization assertions require 'inject_ack' "
                            f"to prove the fault actually took effect")
    return failures


def scenario_localization_loss():
    """定位丢失：supervisor 侧 fusion 心跳超时 → L1/fusion_timeout。

    模型：只有 fusion 一个节点超时（planning/control 心跳正常）→ 单节点超时 → L1。
    """
    ages = [HB_TIMEOUT_MS + HB_CONFIRM_MS + 1, 30, 30]  # fusion 超时，其余健康
    return supervisor_progression(ages, now_ms=10_000)


def scenario_all_nodes_lost():
    """全链路失联：两个以上节点 >2000ms → L3。"""
    ages = [HB_LOST_MS + 100, HB_LOST_MS + 100, 30]
    return supervisor_progression(ages, now_ms=10_000)


def scenario_raw_cmd_car_stopped():
    """raw_cmd 陈旧但车已停稳 → **不得**升 L3（守卫生效）。"""
    return raw_cmd_timeout_escalates(RAW_CMD_TIMEOUT_US + 500_000, speed_mps=0.0)


def scenario_raw_cmd_car_moving():
    """raw_cmd 陈旧且车在动 → 必须升 L3。"""
    return raw_cmd_timeout_escalates(RAW_CMD_TIMEOUT_US + 500_000, speed_mps=15.0)


def run_all():
    """全部故障类 + supervisor 语义自检。返回失败断言数。"""
    failures = list(check_matrix())

    # ── 故障类逐条检查（表本身自洽）──
    for e in FAULT_MATRIX:
        if not fault_class_observable(e):
            failures.append(f"{e['id']}: not observable")

    # ── supervisor 递进语义（镜像 C，四档必须都能到达）──
    l1 = supervisor_progression([600, 30, 30], 10_000)
    if l1 != (DEGRADE_L1, REASON_CONTROL_TO):
        failures.append(f"single >500ms node must map to L1/control_timeout, got {l1}")

    l2 = supervisor_progression([2100, 30, 30], 10_000)
    if l2 != (DEGRADE_L2, REASON_CONTROL_TO):
        failures.append(f"single >2000ms node must map to L2/control_timeout, got {l2}")

    l2b = supervisor_progression([600, 600, 30], 10_000)
    if l2b[0] != DEGRADE_L2:
        failures.append(f"two >500ms nodes must map to L2, got {l2b}")

    l3 = scenario_all_nodes_lost()
    if l3[0] != DEGRADE_L3:
        failures.append(f"two >2000ms nodes must map to L3, got {l3}")

    l0 = supervisor_progression([30, 30, 30], 10_000)
    if l0 != (DEGRADE_L0, REASON_NONE):
        failures.append(f"all healthy must stay L0/none, got {l0}")

    # ── raw_cmd 守卫：停稳不误报 / 行驶必须报 ──
    if scenario_raw_cmd_car_stopped():
        failures.append("raw_cmd stale while STOPPED must NOT escalate to L3 "
                        "(guards the stop→L3→crawl→L3 oscillation)")
    if not scenario_raw_cmd_car_moving():
        failures.append("raw_cmd stale while MOVING must escalate to L3")

    # ── 场景一致性：定位丢失走 L1，不是 L3（分级要有区分度）──
    loc = scenario_localization_loss()
    if loc[0] != DEGRADE_L1:
        failures.append(f"localization loss must be L1 (single node), got {loc}")

    return failures


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run-all", action="store_true", help="全部故障类 + 自检")
    ap.add_argument("--check-matrix", action="store_true", help="仅表自洽性")
    ap.add_argument("--list", action="store_true", help="打印故障类清单")
    args = ap.parse_args()

    if args.list:
        for e in FAULT_MATRIX:
            extra = ""
            if e.get("control_mode"):
                extra += f" mode={e['control_mode']}"
            if e.get("odd_violation"):
                extra += f" odd={e['odd_violation']}"
            print(f"{e['id']:<20} inject={e['inject']:<30} "
                  f"level>=L{e['level']} reason={','.join(e['reason_names']) or '-'}"
                  f"{extra} via={'+'.join(e.get('observable_via') or ())}")
        return 0

    if args.check_matrix:
        failures = check_matrix()
        for f in failures:
            print(f"  FAIL {f}")
        print(f"fault matrix: {'FAIL' if failures else 'OK'} "
              f"({len(FAULT_MATRIX)} classes)")
        return len(failures)

    # 默认 = --run-all
    print("── 故障注入 → 降级路径 矩阵自检 ──")
    for e in FAULT_MATRIX:
        mark = "ok  " if fault_class_observable(e) else "FAIL"
        print(f"  {mark} {e['id']:<24} level>=L{e['level']} "
              f"reason={','.join(e['reason_names'])}")

    print("── supervisor 递进语义 ──")
    for name, ages, expect in (
        ("all healthy      ", [30, 30, 30], (DEGRADE_L0, REASON_NONE)),
        ("1 node >500ms    ", [600, 30, 30], (DEGRADE_L1, REASON_CONTROL_TO)),
        ("2 nodes >500ms   ", [600, 600, 30], (DEGRADE_L2, REASON_PLANNING_TO)),
        ("1 node >2000ms   ", [2100, 30, 30], (DEGRADE_L2, REASON_CONTROL_TO)),
        ("2 nodes >2000ms  ", [2100, 2100, 30], (DEGRADE_L3, REASON_PLANNING_TO)),
    ):
        got = supervisor_progression(ages, 10_000)
        print(f"  {'ok  ' if got == expect else 'FAIL'} {name} -> L{got[0]}/{got[1]}")

    print("── raw_cmd 超时守卫 ──")
    stopped = scenario_raw_cmd_car_stopped()
    moving = scenario_raw_cmd_car_moving()
    print(f"  {'ok  ' if not stopped else 'FAIL'} car stopped  -> no L3 (guard holds)")
    print(f"  {'ok  ' if moving else 'FAIL'} car moving   -> L3")

    failures = run_all()
    for f in failures:
        print(f"  FAIL {f}")
    print(f"\n结果: {'PASS' if not failures else 'FAIL'} ({len(failures)} 项失败)")
    return len(failures)


if __name__ == "__main__":
    sys.exit(main())
