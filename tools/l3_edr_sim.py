#!/usr/bin/env python3
"""L3 法规事件数据记录（EDR）分类 Python 仿真先行（L3-P2 方向六）。

为什么存在：P2 要求「EDR：在现有 PEM 流上增加 L3 法规事件：激活/退出、TOR、
MRM、碰撞前后窗口，复用 CRC / fsync / 轮转」。

开工盘点发现：PEM 里有 **degrade_transition** 一类事件（monitor_node.c 的
`pem/degrade_event`），但 L3 特有的四类**一个都没有**：
  · 系统激活/退出（ACTIVE ↔ OFF）—— L3 法规要求记录"系统何时接管驾驶任务"
  · TOR 请求/接管/超时（生命周期与倒计时剩余）
  · MRM 进入/完成（含触发原因与停稳位置）
  · 碰撞（含**碰撞前后窗口**：法规要求碰撞时刻前后各 N 秒的状态快照可回溯）

本文件是该事件分类的**单一事实源**（与 C 侧 `include/l3_edr.h` 逐条对应）：
先定"记哪些事件、每个事件记哪几个字段、何时必须 fsync"，再移植成 C。
`--check-taxonomy` 强制每类事件都声明了触发条件 + 字段 + 是否关键，
防止"加了事件类型但没人知道该记什么"。

运行：
  python3 tools/l3_edr_sim.py --run-all      # 分类自洽 + 从状态序列推导事件
  python3 tools/l3_edr_sim.py --check-taxonomy
  python3 tools/l3_edr_sim.py --list
退出码 = 失败断言数（可作 ctest 入口）。
"""

import argparse
import sys

# 与 C 侧 PEM_RECORD_EVENT 的 name 字段对齐（易读、可 grep）
EV_ACTIVATION = "l3_activation"        # 系统激活/退出
EV_TOR        = "l3_tor"               # TOR 请求 / 驾驶员接管 / 超时
EV_MRM        = "l3_mrm"               # MRM 进入 / 完成
EV_COLLISION  = "l3_collision"         # 碰撞（含前后窗口标记）
EV_ODD_EXIT   = "l3_odd_exit"          # ODD 退出/重入（域外是 TOR 的前提）

# ── 事件分类（单一事实源）───────────────────────────────────────────────
# 字段约定：PEM 每条记录固定 8 个 double（PEM_RECORD_VALUE_COUNT）。
#   fields = 该事件实际使用的字段名（按顺序对应 values[0..7]），未用到的必须
#   显式留空并写 0 —— 避免"字段含义靠猜"。
#   critical=True 的事件走 fsync（关键事件不可因掉电丢失）。
EDR_TAXONOMY = [
    {
        "event": EV_ACTIVATION,
        "source_topic": "tor/state",
        "trigger": "tor.state 在 {OFF,ACTIVE} 之间跃迁",
        "fields": ["active", "prev_state_code", "state_code", "t_s",
                   "countdown_s", "reserved", "reserved", "reserved"],
        "critical": True,
        "note": "L3 法规：必须记录系统何时开始/停止对驾驶任务负责",
    },
    {
        "event": EV_TOR,
        "source_topic": "tor/state",
        "trigger": "tor.state 进入 TOR_REQUESTED / DRIVER_TAKEOVER，或由前者离开",
        "fields": ["state_code", "countdown_s", "odd_in", "dms_attention",
                   "dms_will_takeover", "dms_latency_s", "t_s", "reserved"],
        "critical": True,
        "note": "L3 法规：接管请求的时刻、剩余倒计时、驾驶员状态必须可回溯",
    },
    {
        "event": EV_MRM,
        "source_topic": "tor/state",
        "trigger": "tor.state 进入 MRM（mrm.active 置位）或由 MRM 离开",
        "fields": ["active", "target_speed", "speed", "reason_code",
                   "t_s", "reserved", "reserved", "reserved"],
        "critical": True,
        "note": "L3 法规：最小风险策略的进入时刻、原因、停车过程必须可回溯",
    },
    {
        "event": EV_ODD_EXIT,
        "source_topic": "odd/state",
        "trigger": "odd.in_odd 在 {true,false} 之间跃迁",
        "fields": ["in_odd", "violation_count", "visibility_m", "loc_age_ms",
                   "route_age_ms", "degrade_level", "t_s", "reserved"],
        "critical": True,
        "note": "ODD 退出是 TOR 的**前提**，必须与 TOR 事件成对可读",
    },
    {
        "event": EV_COLLISION,
        "source_topic": "sim/collision",
        "trigger": "收到碰撞事件（一次碰撞记一条，带前后窗口标记）",
        "fields": ["pre_window", "post_window", "ego_speed", "other_id",
                   "rel_speed", "severity", "t_s", "reserved"],
        "critical": True,
        "note": "L3 法规：碰撞前后**各 N 秒**的状态必须可回溯（本项只记碰撞本身，"
                "前后窗口由 PEM 已有的周期性 topic 记录提供 —— 故 PEM 必须持续"
                "记录 vehicle/state 等，不能只在事件时刻记）",
    },
]

# 关键事件必须走 fsync（掉电不可丢）——法规取证要求
CRITICAL_REQUIRE_FSYNC = True

# 碰撞前后窗口长度（秒），法规取证最小要求
COLLISION_PRE_WINDOW_S = 5.0
COLLISION_POST_WINDOW_S = 5.0


def event_by_name(name):
    for e in EDR_TAXONOMY:
        if e["event"] == name:
            return e
    return None


def check_taxonomy():
    """分类自洽性：每类都有触发条件/字段/关键标记，字段不超 8 个且无空名。"""
    fails = []
    seen = set()
    for e in EDR_TAXONOMY:
        name = e.get("event", "")
        if not name:
            fails.append("an event entry has no 'event' name")
            continue
        if name in seen:
            fails.append(f"duplicate event name: {name}")
        seen.add(name)
        if not e.get("source_topic"):
            fails.append(f"{name}: missing source_topic (where is it observed?)")
        if not e.get("trigger"):
            fails.append(f"{name}: missing trigger condition")
        fields = e.get("fields") or []
        if len(fields) != 8:
            fails.append(f"{name}: must declare exactly 8 fields (PEM fixed width), "
                         f"got {len(fields)}")
        for f in fields:
            if not isinstance(f, str) or not f.strip():
                fails.append(f"{name}: empty field name in fields[]")
        if "critical" not in e:
            fails.append(f"{name}: must declare 'critical' (fsync policy)")
        if not e.get("note"):
            fails.append(f"{name}: missing note (why is this recorded?)")
    return fails


def derive_events(state_seq):
    """从 tor/odd 状态序列推导应记录的事件（镜像 C 侧的边沿检测）。

    state_seq: [{"tor_state":..., "odd_in":..., "mrm_active":...,
                 "speed":..., "t_s":...}, ...]
    返回事件名列表（按发生顺序）。这是 C 侧"只在**跃迁**时记一条"的语义：
    稳态不刷记录，否则 PEM 会被 20Hz 的重复事件淹掉。
    """
    events = []
    prev = None
    for s in state_seq:
        if prev is not None:
            pt, ct = prev.get("tor_state"), s.get("tor_state")
            if pt != ct:
                # OFF ↔ ACTIVE = 激活/退出；进入 TOR/MRM 各有自己的事件
                if ct in ("OFF", "ACTIVE") or pt in ("OFF", "ACTIVE"):
                    events.append(EV_ACTIVATION)
                if ct in ("TOR_REQUESTED", "DRIVER_TAKEOVER") or \
                        pt in ("TOR_REQUESTED", "DRIVER_TAKEOVER"):
                    events.append(EV_TOR)
                if ct == "MRM" or pt == "MRM":
                    events.append(EV_MRM)
            if prev.get("odd_in") != s.get("odd_in"):
                events.append(EV_ODD_EXIT)
            if bool(prev.get("mrm_active")) != bool(s.get("mrm_active")):
                if s.get("mrm_active"):
                    events.append(EV_MRM)
        prev = s
    return events


def run_all():
    failures = list(check_taxonomy())

    # 1. 每类关键事件都必须声明 fsync 策略（法规取证：掉电不可丢）
    if CRITICAL_REQUIRE_FSYNC:
        for e in EDR_TAXONOMY:
            if e.get("critical") is not True:
                failures.append(f"{e['event']}: L3 regulatory events must be critical "
                                f"(fsync) — got critical={e.get('critical')}")

    # 2. 碰撞窗口必须为正（法规要求可回溯的前后状态）
    if COLLISION_PRE_WINDOW_S <= 0 or COLLISION_POST_WINDOW_S <= 0:
        failures.append("collision pre/post windows must be positive")

    # 3. 边沿检测语义：稳态不重复记录，跃迁各记一次
    steady = [{"tor_state": "ACTIVE", "odd_in": True, "mrm_active": False,
               "speed": 15.0, "t_s": i * 0.05} for i in range(20)]
    if derive_events(steady) != []:
        failures.append(f"steady state must emit no events, got {derive_events(steady)}")

    # 4. ODD 退出 → TOR 请求 → 接管 的完整链路
    seq = [
        {"tor_state": "ACTIVE", "odd_in": True, "mrm_active": False, "speed": 15.0, "t_s": 0.0},
        {"tor_state": "ACTIVE", "odd_in": False, "mrm_active": False, "speed": 15.0, "t_s": 1.0},
        {"tor_state": "TOR_REQUESTED", "odd_in": False, "mrm_active": False, "speed": 15.0, "t_s": 1.5},
        {"tor_state": "DRIVER_TAKEOVER", "odd_in": False, "mrm_active": False, "speed": 14.0, "t_s": 2.0},
    ]
    ev = derive_events(seq)
    for want in (EV_ODD_EXIT, EV_TOR):
        if want not in ev:
            failures.append(f"ODD-exit→TOR chain must emit {want}, got {ev}")

    # 5. 超时 → MRM 链路必须记 MRM
    seq_mrm = [
        {"tor_state": "TOR_REQUESTED", "odd_in": False, "mrm_active": False, "speed": 15.0, "t_s": 0.0},
        {"tor_state": "MRM", "odd_in": False, "mrm_active": True, "speed": 14.0, "t_s": 1.0},
        {"tor_state": "OFF", "odd_in": True, "mrm_active": False, "speed": 0.0, "t_s": 16.0},
    ]
    ev = derive_events(seq_mrm)
    for want in (EV_MRM, EV_ACTIVATION, EV_ODD_EXIT):
        if want not in ev:
            failures.append(f"TOR-timeout→MRM chain must emit {want}, got {ev}")

    # 6. 事件名必须唯一且可 grep（前缀统一 l3_）
    for e in EDR_TAXONOMY:
        if not e["event"].startswith("l3_"):
            failures.append(f"{e['event']}: L3 EDR event names must start with 'l3_' "
                            f"(keeps them greppable in PEM dumps)")

    # 7. 每类事件必须能从某个真实 topic 观察到（不虚构来源）
    known_topics = {"tor/state", "odd/state", "sim/collision"}
    for e in EDR_TAXONOMY:
        if e["source_topic"] not in known_topics:
            failures.append(f"{e['event']}: source_topic '{e['source_topic']}' is not "
                            f"a real topic ({sorted(known_topics)})")
    return failures


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run-all", action="store_true", help="分类自洽 + 事件推导自检")
    ap.add_argument("--check-taxonomy", action="store_true", help="仅分类自洽性")
    ap.add_argument("--list", action="store_true", help="列出事件分类")
    args = ap.parse_args()

    if args.list:
        for e in EDR_TAXONOMY:
            print(f"{e['event']:<16} <- {e['source_topic']:<16} "
                  f"critical={e['critical']} fields={len(e['fields'])}")
        return 0

    if args.check_taxonomy:
        fails = check_taxonomy()
        for f in fails:
            print(f"  FAIL {f}")
        print(f"l3 edr taxonomy: {'FAIL' if fails else 'OK'} "
              f"({len(EDR_TAXONOMY)} event classes)")
        return len(fails)

    print("── L3 EDR 事件分类 ──")
    for e in EDR_TAXONOMY:
        print(f"  ok   {e['event']:<16} <- {e['source_topic']:<16} "
              f"critical={e['critical']}")

    failures = run_all()
    for f in failures:
        print(f"  FAIL {f}")
    print(f"\n结果: {'PASS' if not failures else 'FAIL'} ({len(failures)} 项失败)")
    return len(failures)


if __name__ == "__main__":
    sys.exit(main())
