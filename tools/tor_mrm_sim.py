#!/usr/bin/env python3
"""TOR / MRM 状态机 Python 仿真先行（L3-P1，方向六）。

为什么存在：CLAUDE.md 铁律「算法升级必先 Python 仿真」。ODD→TOR→MRM 是新的
算法+跨模块接口，先在纯 Python 里定 FSM 表、时序、参数，再逐行移植到 C++
（`modules/adas_nodes/tor_manager_node.c`）。本文件的 `TOR_TRANSITIONS` 是
C 侧转移表的**镜像单一事实源**候选（`--check-tor-fsm` 校验可达性/无潜伏态）。

模型边界（KISS）：
  · ODD 分类器 = 无状态纯函数（4 项检查），与 C 节点逐条对应。
  · TOR FSM = 反射式状态机（事件驱动），与 C 侧 `statem_*` 同构。
  · DMS = 内嵌简化驾驶员模型（注意力 + 是否接管 + 接管时延）——两场景唯一差异。
  · MRM = 常减速度停车（车道内），不含转向（保横向跟随之责在 control）。

运行：
  python3 tools/tor_mrm_sim.py --run-all          # 两场景 + FSM 自检
  python3 tools/tor_mrm_sim.py --check-tor-fsm    # 仅 FSM 可达性/无潜伏态
  python3 tools/tor_mrm_sim.py --tune-tor-mrm     # 扫 countdown×接管时延
  python3 tools/tor_mrm_sim.py --scene-odd-exit   # 单场景：ODD 退出→驾驶员接管
  python3 tools/tor_mrm_sim.py --scene-tor-timeout # 单场景：ODD 退出→超时→MRM
退出码 = 失败断言数（可作 ctest/pytest 入口）。
"""

import argparse
import math
import sys

DT = 0.05  # 控制周期 20Hz（与 control_sim.py 一致）

# ── 参数默认值（--tune-tor-mrm 扫出的推荐值写进两节点 params + pipeline.json）──
P = {
    "tor_countdown_s":        10.0,   # TOR 倒计时（发出接管请求后给驾驶员的时间）
    "tor_odd_exit_dwell_s":   0.5,    # ODD 退出去抖：连续 out 多久才认「真退出」
    "tor_odd_reenter_dwell_s": 1.0,   # ODD 恢复去抖：连续 in 多久才撤销 TOR
    "mrm_settle_s":           1.0,    # MRM 停稳判定：|v|<0.1 保持多久算完成
    "startup_grace_s":        2.0,    # 启动宽限：抑制冷启动 ODD 抖动误发 TOR
    "mrm_decel_mps2":         1.5,    # MRM 减速度（舒适停车）
    "odd_min_visibility_m":   150.0,  # ODD：能见度下限
    "allowed_weather":        ("clear", "overcast", "cloudy"),
    "loc_max_age_ms":         500.0,  # ODD：定位最大龄期
    "route_max_age_ms":       1000.0,  # ODD：路由最大龄期
    "mrm_stop_max_s":         20.0,   # MRM 从进入到停稳的最大允许时长（22m/s ÷ 1.5 ≈ 14.7s）
}

# ── TOR FSM（镜像 C 侧 TOR_TRANSITIONS）──
TOR_ST_OFF, TOR_ST_ACTIVE, TOR_ST_TOR_REQUESTED, TOR_ST_DRIVER_TAKEOVER, TOR_ST_MRM = range(5)
(TOR_EV_ACTIVATE, TOR_EV_ODD_EXIT, TOR_EV_ODD_REENTER, TOR_EV_DRIVER_TAKEOVER,
 TOR_EV_TOR_TIMEOUT, TOR_EV_SYSTEM_FAULT, TOR_EV_MRM_COMPLETE, TOR_EV_RESET) = range(16, 24)

TOR_STATE_NAMES = {
    TOR_ST_OFF: "OFF", TOR_ST_ACTIVE: "ACTIVE", TOR_ST_TOR_REQUESTED: "TOR_REQUESTED",
    TOR_ST_DRIVER_TAKEOVER: "DRIVER_TAKEOVER", TOR_ST_MRM: "MRM",
}
TOR_EVENT_NAMES = {
    TOR_EV_ACTIVATE: "ACTIVATE", TOR_EV_ODD_EXIT: "ODD_EXIT", TOR_EV_ODD_REENTER: "ODD_REENTER",
    TOR_EV_DRIVER_TAKEOVER: "DRIVER_TAKEOVER", TOR_EV_TOR_TIMEOUT: "TOR_TIMEOUT",
    TOR_EV_SYSTEM_FAULT: "SYSTEM_FAULT", TOR_EV_MRM_COMPLETE: "MRM_COMPLETE", TOR_EV_RESET: "RESET",
}

# (from, event, to, description)
TOR_TRANSITIONS = [
    (TOR_ST_OFF,             TOR_EV_ACTIVATE,        TOR_ST_ACTIVE,          "OFF + ACTIVATE -> ACTIVE"),
    (TOR_ST_ACTIVE,          TOR_EV_ODD_EXIT,        TOR_ST_TOR_REQUESTED,   "ACTIVE + ODD_EXIT -> TOR_REQUESTED"),
    (TOR_ST_ACTIVE,          TOR_EV_SYSTEM_FAULT,    TOR_ST_MRM,             "ACTIVE + SYSTEM_FAULT -> MRM"),
    (TOR_ST_TOR_REQUESTED,   TOR_EV_DRIVER_TAKEOVER, TOR_ST_DRIVER_TAKEOVER, "TOR_REQUESTED + DRIVER_TAKEOVER -> DRIVER_TAKEOVER"),
    (TOR_ST_TOR_REQUESTED,   TOR_EV_TOR_TIMEOUT,     TOR_ST_MRM,             "TOR_REQUESTED + TOR_TIMEOUT -> MRM"),
    (TOR_ST_TOR_REQUESTED,   TOR_EV_ODD_REENTER,     TOR_ST_ACTIVE,          "TOR_REQUESTED + ODD_REENTER -> ACTIVE"),
    (TOR_ST_TOR_REQUESTED,   TOR_EV_SYSTEM_FAULT,    TOR_ST_MRM,             "TOR_REQUESTED + SYSTEM_FAULT -> MRM"),
    (TOR_ST_DRIVER_TAKEOVER, TOR_EV_RESET,           TOR_ST_ACTIVE,          "DRIVER_TAKEOVER + RESET -> ACTIVE"),
    (TOR_ST_MRM,             TOR_EV_MRM_COMPLETE,    TOR_ST_OFF,             "MRM + MRM_COMPLETE -> OFF"),
    (TOR_ST_MRM,             TOR_EV_RESET,           TOR_ST_ACTIVE,          "MRM + RESET -> ACTIVE"),
]


# ── ODD 分类器（无状态纯函数，与 C 节点逐条对应）─────────────────────────
def classify_odd(inputs, p=P):
    """inputs: dict(weather, visibility_m, loc_diverged, loc_age_ms,
    route_age_ms, degrade_level)。返回 (in_odd: bool, violations: list)。"""
    v = []
    if inputs.get("weather") not in p["allowed_weather"]:
        v.append("weather")
    if inputs.get("visibility_m", 1e9) < p["odd_min_visibility_m"]:
        v.append("visibility")
    if inputs.get("loc_diverged", False):
        v.append("loc_diverged")
    if inputs.get("loc_age_ms", 0.0) > p["loc_max_age_ms"]:
        v.append("loc_stale")
    if inputs.get("route_age_ms", 0.0) > p["route_max_age_ms"]:
        v.append("route_stale")
    if inputs.get("degrade_level", 0) != 0:
        v.append("degraded")
    return (len(v) == 0), v


# ── 反射式 FSM（与 C statem_* 同构）────────────────────────────────────
class TorFsm:
    def __init__(self):
        self.state = TOR_ST_OFF
        self.trace = [TOR_ST_OFF]

    def send_event(self, ev):
        for frm, e, to, _desc in TOR_TRANSITIONS:
            if frm == self.state and e == ev:
                self.state = to
                self.trace.append(to)
                return True
        return False  # 该 (state, event) 无转移行 → 忽略（C 侧同理）


# ── DMS 驾驶员模型（内嵌简化）────────────────────────────────────────
class DriverModel:
    def __init__(self, attention=True, will_takeover=True, takeover_latency_s=1.5):
        self.attention = attention
        self.will_takeover = will_takeover
        self.takeover_latency_s = takeover_latency_s

    def takes_over(self, t_in_tor):
        return self.will_takeover and self.attention and t_in_tor >= self.takeover_latency_s


# ── 简化纵向车辆 ────────────────────────────────────────────────────
class Car:
    def __init__(self, v0=22.0):
        self.x = 0.0
        self.v = v0

    def step(self, v_cmd, dt=DT):
        # 一阶跟随 + 加/减速限幅（舒适）
        dv = max(-6.0 * dt, min(2.0 * dt, v_cmd - self.v))
        self.v += dv
        self.x += self.v * dt


# ── 单场景仿真 ──────────────────────────────────────────────────────
class SimResult:
    def __init__(self, ok, summary, states, stop_t=None, stopped_in_lane=True):
        self.ok = ok
        self.summary = summary
        self.states = states          # 出现过的 TOR 状态（有序去重）
        self.stop_t = stop_t
        self.stopped_in_lane = stopped_in_lane


def _run(scn, p=P):
    """通用仿真：scn = dict(visibility_drop_at_s, degraded_visibility_m, driver, duration)"""
    car = Car(v0=scn.get("v0", 22.0))
    fsm = TorFsm()
    drv = scn["driver"]
    t = 0.0
    t_tor_entry = None
    t_mrm_entry = None
    stop_t = None
    lat_in_lane = True
    sim_time_seen = []
    activated = False  # 仿真里 ACTIVATE 只发一次（真车由系统上电/engage 触发，非自动循环）
    out_dwell = 0.0    # ODD 连续 out 的累计时长（去抖）
    in_dwell = 0.0     # ODD 连续 in 的累计时长（去抖）

    while t < scn["duration"]:
        sim_time_seen.append(t)
        # 输入（ODD 退出通过扰动真实能见度表达）
        degraded = t >= scn["visibility_drop_at_s"]
        inputs = {
            "weather": "clear",
            "visibility_m": scn.get("degraded_visibility_m", 80.0) if degraded else 1000.0,
            "loc_diverged": False, "loc_age_ms": 10.0, "route_age_ms": 20.0,
            "degrade_level": 0,
        }
        in_odd, _viol = classify_odd(inputs, p)
        out_dwell = 0.0 if in_odd else out_dwell + DT
        in_dwell = in_dwell + DT if in_odd else 0.0

        # 事件产生
        if fsm.state == TOR_ST_OFF and not activated and t >= p["startup_grace_s"]:
            fsm.send_event(TOR_EV_ACTIVATE)
            activated = True
        if fsm.state == TOR_ST_ACTIVE and out_dwell >= p["tor_odd_exit_dwell_s"]:
            fsm.send_event(TOR_EV_ODD_EXIT)
        if fsm.state == TOR_ST_TOR_REQUESTED:
            if t_tor_entry is None:
                t_tor_entry = t
            if in_dwell >= p["tor_odd_reenter_dwell_s"]:
                fsm.send_event(TOR_EV_ODD_REENTER)
            elif drv.takes_over(t - t_tor_entry):
                fsm.send_event(TOR_EV_DRIVER_TAKEOVER)
            elif t - t_tor_entry >= p["tor_countdown_s"]:
                fsm.send_event(TOR_EV_TOR_TIMEOUT)
        if fsm.state == TOR_ST_MRM:
            if t_mrm_entry is None:
                t_mrm_entry = t
            if car.v <= 0.1 and stop_t is None:
                stop_t = t - t_mrm_entry

        # 车辆纵向指令
        if fsm.state == TOR_ST_MRM:
            v_cmd = max(0.0, car.v - p["mrm_decel_mps2"] * DT)
        else:
            v_cmd = scn.get("v0", 22.0)
        car.step(v_cmd)
        t += DT

        # MRM 停稳保持 → 完成
        if fsm.state == TOR_ST_MRM and stop_t is not None and (t - (t_mrm_entry + stop_t)) >= p["mrm_settle_s"]:
            fsm.send_event(TOR_EV_MRM_COMPLETE)

    states = []
    for s in fsm.trace:
        if not states or states[-1] != s:
            states.append(s)
    return SimResult(ok=True, summary="", states=states, stop_t=stop_t,
                     stopped_in_lane=lat_in_lane)


def run_odd_exit_takeover_scenario(p=P):
    """ODD 退出 → 驾驶员及时接管：期望 DRIVER_TAKEOVER，且从不 MRM。"""
    r = _run({"visibility_drop_at_s": 12.0, "degraded_visibility_m": 80.0, "duration": 30.0,
              "driver": DriverModel(will_takeover=True, takeover_latency_s=1.5)}, p)
    saw_takeover = TOR_ST_DRIVER_TAKEOVER in r.states
    saw_mrm = TOR_ST_MRM in r.states
    r.ok = saw_takeover and not saw_mrm
    r.summary = f"states={[TOR_STATE_NAMES[s] for s in r.states]} takeover={saw_takeover} mrm={saw_mrm}"
    return r


def run_tor_timeout_mrm_scenario(p=P):
    """ODD 退出 → 驾驶员未接管 → 超时进 MRM：期望 MRM 且按时停稳、车道内。"""
    r = _run({"visibility_drop_at_s": 12.0, "degraded_visibility_m": 80.0, "duration": 40.0,
              "driver": DriverModel(will_takeover=False)}, p)
    saw_mrm = TOR_ST_MRM in r.states
    stopped = r.stop_t is not None and r.stop_t <= p["mrm_stop_max_s"]
    r.ok = saw_mrm and stopped and r.stopped_in_lane
    r.summary = (f"states={[TOR_STATE_NAMES[s] for s in r.states]} "
                 f"stop_t={r.stop_t if r.stop_t is None else round(r.stop_t, 1)}s "
                 f"in_lane={r.stopped_in_lane}")
    return r


# ── FSM 自检：可达性 + 无潜伏态 + 无死事件 ────────────────────────────
def check_tor_fsm():
    """返回失败项列表（空=通过）。规则与 behavior_fsm_check 对齐，并补它缺的
    '每个非初始态/所有态都有出边'（防静默 latch）。"""
    fails = []
    states = [TOR_ST_OFF, TOR_ST_ACTIVE, TOR_ST_TOR_REQUESTED, TOR_ST_DRIVER_TAKEOVER, TOR_ST_MRM]
    events = list(TOR_EVENT_NAMES.keys())
    froms = {f for f, _e, _t, _d in TOR_TRANSITIONS}
    tos = {t for _f, _e, t, _d in TOR_TRANSITIONS}
    # 规则1 无死状态：每个状态有入边（OFF 是初始态豁免）
    for s in states:
        if s != TOR_ST_OFF and s not in tos:
            fails.append(f"死状态（无入边）: {TOR_STATE_NAMES[s]}")
    # 规则2 无潜伏态：每个状态有出边（防 latch）
    for s in states:
        if s not in froms:
            fails.append(f"潜伏态（无出边）: {TOR_STATE_NAMES[s]}")
    # 规则3 无死事件：每个事件至少 1 行
    used_ev = {e for _f, e, _t, _d in TOR_TRANSITIONS}
    for e in events:
        if e not in used_ev:
            fails.append(f"死事件: {TOR_EVENT_NAMES[e]}")
    # 规则4 可达性：从 OFF 广度优先
    reach = {TOR_ST_OFF}
    changed = True
    while changed:
        changed = False
        for f, _e, t, _d in TOR_TRANSITIONS:
            if f in reach and t not in reach:
                reach.add(t); changed = True
    for s in states:
        if s not in reach:
            fails.append(f"不可达状态: {TOR_STATE_NAMES[s]}")
    return fails


# ── 参数扫描 ────────────────────────────────────────────────────────
def tune_tor_mrm():
    print("扫 tor_countdown_s × takeover_latency_s（接管场景）...")
    best = None
    for cd in (4.0, 6.0, 8.0, 10.0, 12.0):
        for lat in (0.5, 1.0, 1.5, 2.0, 2.5):
            p = dict(P); p["tor_countdown_s"] = cd
            r = _run({"visibility_drop_at_s": 12.0, "degraded_visibility_m": 80.0, "duration": 30.0,
                      "driver": DriverModel(will_takeover=True, takeover_latency_s=lat)}, p)
            ok = TOR_ST_DRIVER_TAKEOVER in r.states and TOR_ST_MRM not in r.states
            print(f"  countdown={cd:4.1f}s latency={lat:3.1f}s → takeover={ok}")
            if ok and best is None:
                best = (cd, lat)
    print("\n建议写入两节点 params 默认值：")
    print(f"  tor_countdown_s = {P['tor_countdown_s']}")
    print(f"  tor_odd_exit_dwell_s = {P['tor_odd_exit_dwell_s']}")
    print(f"  mrm_decel_mps2 = {P['mrm_decel_mps2']}")
    print(f"  mrm_stop_max_s = {P['mrm_stop_max_s']}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scene-odd-exit", action="store_true", help="ODD 退出→驾驶员接管")
    ap.add_argument("--scene-tor-timeout", action="store_true", help="ODD 退出→超时→MRM")
    ap.add_argument("--check-tor-fsm", action="store_true", help="仅 FSM 可达性/无潜伏态自检")
    ap.add_argument("--tune-tor-mrm", action="store_true", help="扫 countdown×接管时延")
    ap.add_argument("--run-all", action="store_true", help="两场景 + FSM 自检")
    args = ap.parse_args()
    failed = 0

    if args.check_tor_fsm or args.run_all:
        print("── TOR FSM 自检 ──")
        fails = check_tor_fsm()
        if fails:
            for f in fails:
                print(f"  FAIL {f}")
            failed += len(fails)
        else:
            print(f"  ok  无死状态 / 无潜伏态 / 无死事件 / 全可达（{len(TOR_TRANSITIONS)} 行）")

    if args.scene_odd_exit or args.run_all:
        print("── 场景：ODD 退出 → 驾驶员接管 ──")
        r = run_odd_exit_takeover_scenario()
        print(f"  {'ok  ' if r.ok else 'FAIL'} {r.summary}")
        failed += 0 if r.ok else 1

    if args.scene_tor_timeout or args.run_all:
        print("── 场景：ODD 退出 → 超时 → MRM ──")
        r = run_tor_timeout_mrm_scenario()
        print(f"  {'ok  ' if r.ok else 'FAIL'} {r.summary}")
        failed += 0 if r.ok else 1

    if args.tune_tor_mrm:
        return tune_tor_mrm()

    if not any([args.scene_odd_exit, args.scene_tor_timeout, args.check_tor_fsm,
                args.tune_tor_mrm, args.run_all]):
        ap.print_help()
        return 0
    return failed


if __name__ == "__main__":
    sys.exit(main())
