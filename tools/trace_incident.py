#!/usr/bin/env python3
"""trace_incident.py — 事故逐层追溯工具

读 /tmp/flow_topology.json，对指定时间点或最近一次事故，
dump 每一层在那一刻的输入/输出，快速定位哪个模块出了问题。

用法:
    python3 tools/trace_incident.py                    # 最近一次事故（road_departure / collision）
    python3 tools/trace_incident.py --at 17.6          # 指定时间戳
    python3 tools/trace_incident.py --json /tmp/snap.json  # 指定快照
    python3 tools/trace_incident.py --export-json      # 导出结构化 JSON 到 stdout
    python3 tools/trace_incident.py --json-output /tmp/out.json # 导出结构化 JSON 到文件
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_JSON = Path("/tmp/flow_topology.json")


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def lane_idx_from_y(y: float, lc: int, lw: float) -> int:
    off = -y / lw + (lc - 1) * 0.5
    return max(0, min(lc - 1, int(round(off))))


def lane_center_y(idx: int, lc: int, lw: float) -> float:
    return -(idx - (lc - 1) * 0.5) * lw


def find_incident(samples: list[dict], data: dict) -> dict | None:
    """从 samples 和 topology 中找最近一次事故。"""
    # Check collision in topic stats
    for t in data.get("metrics", {}).get("topics", []):
        if t["topic"] == "sim/collision" and t["pub"] > 0:
            return {"type": "collision", "topic_pub": t["pub"], "at_s": "unknown (log only)"}

    # Check road departure from samples
    scene = data.get("metrics", {}).get("scene", {})
    lane = scene.get("lane", {})
    lc = int(lane.get("count", 4))
    lw = float(lane.get("width", 3.5))
    half_road = lc * lw * 0.5

    if len(samples) >= 5:
        ys = [s.get("y", 0) for s in samples]
        ts = [s.get("t", 0) for s in samples]

        # Find when ego left the road
        for i in range(len(ys)):
            if abs(ys[i]) > half_road:
                return {
                    "type": "road_departure",
                    "at_s": ts[i] - ts[0] if ts else 0,
                    "y": ys[i],
                    "speed": samples[i].get("speed", 0),
                    "index": i,
                }
    return None


def print_layer(label: str, items: list[tuple[str, str]], indent: int = 2) -> None:
    pad = " " * indent
    print(f"\n{pad}── {label} ──")
    for key, val in items:
        print(f"{pad}  {key}: {val}")


def analyze_incident(data: dict, at_s: float | None = None, verbose: bool = False) -> dict:
    metrics = data.get("metrics", {})
    scene = metrics.get("scene", {})
    beh = metrics.get("behavior", {})
    vehicle = metrics.get("vehicle", {})
    pd = metrics.get("planning_debug", {})
    cd = metrics.get("control_debug", {})
    tor = metrics.get("tor", {})
    samples = data.get("samples", [])

    # Find incident
    if at_s is None:
        incident = find_incident(samples, data)
    else:
        best = None
        best_dt = float("inf")
        t0_sel = float(samples[0].get("t", 0)) if samples else 0.0
        for s in samples:
            rel = s.get("t_demo")
            if not isinstance(rel, (int, float)):
                rel = float(s.get("t", 0)) - t0_sel
            dt = abs(float(rel) - at_s)
            if dt < best_dt:
                best_dt = dt
                best = s
        if best:
            incident = {"type": "manual", "at_s": at_s, "sample": best}
        else:
            incident = None

    target_t = incident.get("at_s") if incident else (at_s if at_s is not None else None)

    # 样本时间：t_demo 已是场景内秒；否则用相对首样本的 t。
    # 不用墙钟 timestamp 去减，否则 live 轮询永远对不上事故点，层数据落到末样本。
    def _sample_rel_s(s: dict, t0: float) -> float:
        rel = s.get("t_demo")
        if isinstance(rel, (int, float)):
            return float(rel)
        return float(s.get("t", 0)) - t0

    t0 = float(samples[0].get("t", 0)) if samples else 0.0
    incident_sample = None
    if incident and isinstance(incident.get("sample"), dict):
        incident_sample = incident["sample"]
    elif isinstance(target_t, (int, float)) and samples:
        best_dt = float("inf")
        for s in samples:
            dt = abs(_sample_rel_s(s, t0) - float(target_t))
            if dt < best_dt:
                best_dt = dt
                incident_sample = s
    if incident_sample is None and samples:
        incident_sample = samples[-1]

    # Layer 1: Localization
    ego = scene.get("ego", {})
    lane = scene.get("lane", {})
    lc = int(lane.get("count", 4))
    lw = float(lane.get("width", 3.5))
    half_road = lc * lw * 0.5
    if incident_sample:
        sx = incident_sample.get("x", 0)
        sy = incident_sample.get("y", 0)
        sh = incident_sample.get("heading", 0)
        ss = incident_sample.get("speed", 0)
    else:
        sx, sy, sh, ss = ego.get("x", 0), ego.get("y", 0), ego.get("heading", 0), ego.get("speed", 0)

    li = lane_idx_from_y(sy, lc, lw)
    lcy = lane_center_y(li, lc, lw)
    off_road = abs(sy) > half_road

    # Layer 2: Perception
    obstacles = scene.get("obstacles", [])
    beh_obs_count = beh.get("obs_count", 0)
    same_lane_ahead = []
    for o in obstacles:
        wy = sy + o.get("y", 0)
        if o.get("x", 0) > 0 and abs(wy - lcy) < lw * 0.5 + 0.6:
            o_copy = dict(o)
            o_copy["_wy"] = wy
            same_lane_ahead.append(o_copy)

    # Layer 4: Planning
    traj = scene.get("trajectory_path", [])

    # Lateral consistency checks
    lateral_verdicts = []
    if pd and cd:
        pd_d = pd.get("target_lane_offset", 0)
        cd_lane_d = cd.get("lane_d", 0)
        if abs(pd_d - cd_lane_d) > 0.1:
            lateral_verdicts.append(f"❌ planning.target_lane_offset={pd_d:.3f} ≠ control.lane_d={cd_lane_d:.3f} → 轨迹横向偏移没传到control")
        else:
            lateral_verdicts.append(f"✅ planning→control 横向偏移一致: d={pd_d:.3f}m")

        expected_target = cd.get("road_center_y", 0) + cd_lane_d
        actual_target = cd.get("target_y", 0)
        if abs(expected_target - actual_target) > 0.05:
            lateral_verdicts.append(f"❌ target_y计算错误: expected rc_y+lane_d={expected_target:.3f}, got {actual_target:.3f}")

        beh_target = pd.get("beh_target_lane", -1)
        if beh_target >= 0 and cd.get("lat_error", 0) != 0:
            lat_err = cd.get("lat_error", 0)
            steer = cd.get("steer", 0)
            if abs(steer) < 0.001 and abs(lat_err) > 0.3:
                lateral_verdicts.append(f"❌ lat_error={lat_err:.3f}m 但 steer≈0 → Stanley没有输出修正方向")

    # Collision timeline & details
    launcher_log = Path("/tmp/flow_launcher_stderr.txt")
    col_data = {}
    if launcher_log.exists():
        txt = launcher_log.read_text(encoding="utf-8", errors="ignore")
        col_match = re.search(r"COLLISION\s+ego.*?\{.*?overlap", txt, re.DOTALL)
        if col_match:
            for key in ("obs_x", "obs_y", "obs_vx", "obs_vy", "obs_heading", "obs_speed", "obs_id"):
                vm = re.search(rf'"{key}"\s*:\s*([-\d.e+]+)', txt)
                if vm:
                    col_data[key] = float(vm.group(1))

    collision_timeline = []
    if (incident and incident["type"] == "collision") or col_data:
        if len(samples) >= 3:
            col_t = max(s.get("t_demo", s.get("t", 0)) for s in samples)
            timeline_start = col_t - 5.0
            col_id = int(col_data.get("obs_id", -1)) if col_data else -1
            for s in samples:
                ts = s.get("t_demo", s.get("t", 0))
                if ts < timeline_start or ts > col_t:
                    continue
                sx_s = s.get("x", 0)
                sy_s = s.get("y", 0)
                ss_s = s.get("speed", 0)
                gap = float("inf")
                for ent in s.get("metrics", {}).get("scene", {}).get("entities", []):
                    if int(ent.get("id", -1)) == col_id:
                        gap = float(ent.get("x", 0)) - sx_s
                        break
                bm = s.get("metrics", {}).get("behavior", {})
                state = bm.get("state", "?")
                lead = bm.get("lead_speed", 0)
                collision_timeline.append({
                    "t": round(ts, 2), "ego_x": round(sx_s, 2), "ego_y": round(sy_s, 2),
                    "speed": round(ss_s, 2), "gap": round(gap, 2) if math.isfinite(gap) else None,
                    "lead": round(lead, 2), "state": state
                })

    # 法规事件只收非名义态。ACTIVE/OFF 是巡航常态，写进 events 会让仪表盘每 3s 误报接管。
    if not isinstance(tor, dict):
        tor = {}
    _TOR_EVENT_STATES = {"TOR_REQUESTED", "DRIVER_TAKEOVER"}
    events = []
    tor_state = tor.get("state") if isinstance(tor, dict) else None
    if tor_state in _TOR_EVENT_STATES:
        events.append({
            "type": "l3_tor",
            "state": tor_state,
            "countdown_s": tor.get("countdown_s", 0),
            "reason": tor.get("reason", ""),
            "t_s": tor.get("t_s", target_t if isinstance(target_t, (int, float)) else 0),
            "dms": tor.get("dms")
        })
    mrm_obj = tor.get("mrm") if isinstance(tor.get("mrm"), dict) else {}
    ctl_mode = cd.get("mode", "") if isinstance(cd.get("mode"), str) else ""
    if mrm_obj.get("active") or tor_state == "MRM" or ctl_mode == "MRM" or ctl_mode.startswith("MRM+"):
        events.append({
            "type": "l3_mrm",
            "active": True,
            "target_speed": mrm_obj.get("target_speed", 0),
            "speed": tor.get("speed", ss),
            "reason": tor.get("reason", ctl_mode),
            "t_s": tor.get("t_s", target_t if isinstance(target_t, (int, float)) else 0)
        })
    if (incident and incident["type"] == "collision") or col_data:
        col_t_val = col_data.get("t_s", target_t if target_t is not None else 0)
        events.append({
            "type": "l3_collision",
            "t_s": col_t_val,
            "details": col_data
        })
    # 与 find_incident 同一判据（样本数门槛在那边）。不要再扫一遍，
    # 否则不足 5 个样本时事件和时间轴会对同一段轨迹给出两个答案。
    if incident and incident.get("type") == "road_departure":
        events.append({
            "type": "road_departure",
            "t_s": incident.get("at_s", 0),
            "y": incident.get("y", 0),
            "speed": incident.get("speed", 0),
            "index": incident.get("index", -1),
        })

    # 「场景明明有前车」只在碰撞/出路沿/指定时刻成立。
    # 单有 TOR 或空直道巡航时不能据此判盲开，否则 live 轮询会把正常工况打红。
    investigate = incident is not None or at_s is not None
    verdicts = []
    if investigate:
        if not same_lane_ahead and not off_road:
            verdicts.append("❌ 感知: 同车道无前车（但场景明明有）→ 盲开")
        elif same_lane_ahead and beh.get("state") in ("CRUISE",):
            closest = min(same_lane_ahead, key=lambda o: o["x"])
            if closest["x"] < 100:
                verdicts.append(f"❌ 行为: 前车 dx={closest['x']:.0f}m 但 state=CRUISE → 没进 FOLLOW")
        if beh.get("best_gap", -1) > 0 and beh.get("state") in ("FOLLOW", "LEFT_CHANGE", "RIGHT_CHANGE"):
            if traj and len(traj[-1]) > 2 and traj[-1][2] > beh.get("target_speed", 0) + 2:
                verdicts.append(f"❌ 规划: target_speed={beh.get('target_speed',0):.1f} 但轨迹末点={traj[-1][2]:.1f} → 不减速")
        if off_road:
            if vehicle.get("speed", 0) > 5:
                verdicts.append(f"🚨 出路沿时速度 {vehicle.get('speed',0):.1f} m/s → 恢复了但太慢")
            else:
                verdicts.append("⚠️ 出路沿但速度低 → 恢复中")

        verdicts.extend(lateral_verdicts)
        if beh.get("state") in ("LEFT_CHANGE", "RIGHT_CHANGE"):
            timer = beh.get("state_timer", 0)
            dist = beh.get("dist_to_target_lane", -1)
            verdicts.append(f"ℹ️ 变道中: state_timer={timer:.1f}s dist_to_target={dist:.2f}m "
                            f"committed→target: {int(beh.get('committed_lane',-1))}→{int(beh.get('target_lane',-1))}")

        if not verdicts:
            verdicts.append("✅ 所有模块行为一致，事故因外部因素（场景/初始化）")
    elif events:
        verdicts.append("ℹ️ 存在 L3 法规事件，见 events")
    else:
        verdicts.append("✅ 巡航中，未发现碰撞、出路沿或 L3 接管事件")

    # Timing samples
    timing_samples = []
    t0 = samples[0].get("t", 0) if samples else 0
    for s in samples:
        timing_samples.append({
            "t": round(s.get("t_demo", s.get("t", 0) - t0), 2),
            "x": round(s.get("x", 0), 2),
            "y": round(s.get("y", 0), 2),
            "speed": round(s.get("speed", 0), 2),
            "heading": round(s.get("heading", 0), 3),
            "steer": round(s.get("steer", 0), 3) if "steer" in s else 0.0,
        })

    return {
        "ok": True,
        "incident": incident,
        "verdicts": verdicts,
        "events": events,
        "timing_samples": timing_samples,
        "collision_timeline": collision_timeline,
        "col_data": col_data,
        "summary": {
            "has_incident": bool(incident),
            "incident_type": incident.get("type") if incident else "none",
            "status": "fail" if any("❌" in v or "🚨" in v for v in verdicts) else "pass",
            "event_count": len(events),
            "verdicts_count": len(verdicts)
        },
        "layers": {
            "localization": {
                "x": sx, "y": sy, "heading_deg": math.degrees(sh), "speed": ss,
                "lane_idx": li, "center_y": lcy, "cte": sy - lcy, "off_road": off_road
            },
            "perception": {
                "truth_count": len(obstacles),
                "same_lane_ahead_count": len(same_lane_ahead),
                "perceived_count": beh_obs_count,
            },
            "behavior": {
                "state": beh.get("state", "?"),
                "obs_count": beh.get("obs_count", 0),
                "best_gap": beh.get("best_gap", -1),
                "lead_speed": beh.get("lead_speed", 0),
                "target_speed": beh.get("target_speed", 0),
                "follow_speed": beh.get("follow_speed", 0),
                "desired_gap": beh.get("desired_gap", 0),
                "committed_lane": beh.get("committed_lane", -1),
                "target_lane": beh.get("target_lane", -1),
            },
            "planning": {
                "traj_points": len(traj),
                "ego_d": pd.get("ego_d", 0),
                "target_lane_offset": pd.get("target_lane_offset", 0),
                "traj_valid": bool(pd.get("traj_valid", 0) > 0.5),
            },
            "control": {
                "controller": "MPC" if cd.get("mpc_used", 0) > 0.5 else "Stanley",
                "ego_y": cd.get("ego_y", 0),
                "lane_d": cd.get("lane_d", 0),
                "target_y": cd.get("target_y", 0),
                "lat_error": cd.get("lat_error", 0),
                "steer_deg": math.degrees(cd.get("steer", 0)),
                "mode": cd.get("mode", "?"),
                "hazard": bool(cd.get("hazard", 0) > 0.5),
            }
        }
    }


def trace(data: dict, at_s: float | None = None, verbose: bool = False) -> int:
    res = analyze_incident(data, at_s=at_s, verbose=verbose)
    incident = res.get("incident")

    if not incident:
        print("未发现事故")
        print("提示: 用 --at <时间> 指定追溯点，或跑完 demo 后有碰撞/出路沿数据再来")
        return 0

    at_val = incident.get("at_s", "?")
    if isinstance(at_val, (int, float)):
        at_str = f"~{at_val:.1f}s"
    else:
        at_str = str(at_val)
    print(f"\n{'='*60}")
    print(f"  事故类型: {incident['type']}")
    print(f"  发生时间: {at_str}")
    print(f"{'='*60}")

    loc = res["layers"]["localization"]
    print_layer("1. 定位 (Localization)", [
        (f"x={loc['x']:.1f}  y={loc['y']:.2f}  heading={loc['heading_deg']:.0f}°", ""),
        (f"speed", f"{loc['speed']:.1f} m/s ({loc['speed']*3.6:.0f} km/h)"),
        (f"lane", f"idx={loc['lane_idx']}  center_y={loc['center_y']:.2f}  cte={loc['cte']:.2f}m"),
        (f"off_road?", "🚨 YES" if loc['off_road'] else "OK"),
    ])

    perc = res["layers"]["perception"]
    print_layer("2. 感知 (真值 vs 感知)", [
        (f"truth obstacles", f"{perc['truth_count']} (same-lane ahead: {perc['same_lane_ahead_count']})"),
        (f"perceived (behavior.obs_count)", f"{perc['perceived_count']}"),
        (f"⚠️ 不匹配!" if perc['perceived_count'] < perc['truth_count'] * 0.5 else "✅ 基本一致", ""),
    ])

    beh = res["layers"]["behavior"]
    print_layer("3. 行为规划 (Behavior)", [
        (f"state", f"{beh['state']}"),
        (f"obs_count", f"{beh['obs_count']}"),
        (f"best_gap", f"{beh['best_gap']:.1f} m"),
        (f"committed_lane", f"{beh['committed_lane']}"),
        (f"target_lane", f"{beh['target_lane']}"),
        (f"lead_speed", f"{beh['lead_speed']:.1f} m/s"),
        (f"target_speed", f"{beh['target_speed']:.1f} m/s"),
        (f"follow_speed", f"{beh['follow_speed']:.1f} m/s"),
        (f"desired_gap", f"{beh['desired_gap']:.1f} m"),
    ])

    plan = res["layers"]["planning"]
    print_layer("4. 规划轨迹 (Planning)", [
        (f"points", f"{plan['traj_points']}"),
        (f"traj_valid", "✅" if plan['traj_valid'] else "❌ INVALID"),
        (f"target_lane_offset", f"{plan['target_lane_offset']:.3f} m"),
    ])

    ctl = res["layers"]["control"]
    print_layer("5. 控制输出 (Control)", [
        (f"controller", f"{ctl['controller']}"),
        (f"mode", f"{ctl['mode']}"),
        (f"steer", f"{ctl['steer_deg']:.1f}°"),
        (f"lat_error", f"{ctl['lat_error']:.3f} m"),
        (f"hazard", "🚨 HAZARD" if ctl['hazard'] else "ok"),
    ])

    col_tl = res.get("collision_timeline", [])
    if col_tl:
        print_layer("F. 碰撞复盘 (Collision Timeline)", [])
        col_data = res.get("col_data", {})
        if col_data:
            print(f"    碰撞对象: id={int(col_data.get('obs_id', -1))}")
            print(f"    NPC 位置:  x={col_data.get('obs_x',0):.1f}  y={col_data.get('obs_y',0):.2f}")
            print(f"    NPC 速度:  {col_data.get('obs_speed',0):.1f} m/s")
        print(f"    ┌─ 碰撞前 5s 时间线 ──")
        print(f"    │  {'t(s)':>6}  {'ego_x':>7}  {'ego_y':>7}  {'speed':>6}  {'gap':>6}  {'lead':>6}  {'state':>10}")
        for pt in col_tl:
            gap_str = f"{pt['gap']:.1f}" if pt['gap'] is not None else "∞"
            print(f"    │  {pt['t']:>6.1f}  {pt['ego_x']:>7.1f}  {pt['ego_y']:>7.2f}  {pt['speed']:>6.1f}  {gap_str:>6}  {pt['lead']:>6.1f}  {pt['state']:>10}")
        print(f"    └{'─'*65}")

    print("\n  >>> 结论 <<<")
    for v in res["verdicts"]:
        print(f"    {v}")
    print()

    return 1 if any("❌" in v for v in res["verdicts"]) else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="FlowEngine 事故逐层追溯")
    ap.add_argument("--at", type=float, default=None, help="指定事故时间点 (s)")
    ap.add_argument("--json", type=Path, default=DEFAULT_JSON, help="拓扑JSON路径")
    ap.add_argument("--verbose", "-v", action="store_true", help="显示详细感知数据")
    ap.add_argument("--json-output", type=Path, default=None, help="导出结构化JSON到指定文件")
    ap.add_argument("--export-json", action="store_true", help="以JSON格式输出到stdout")
    args = ap.parse_args()

    data = load(args.json)
    if args.json_output or args.export_json:
        res = analyze_incident(data, at_s=args.at, verbose=args.verbose)
        res_json = json.dumps(res, indent=2, ensure_ascii=False)
        if args.json_output:
            args.json_output.write_text(res_json, encoding="utf-8")
        if args.export_json:
            print(res_json)
        return 0

    return trace(data, at_s=args.at, verbose=args.verbose)


if __name__ == "__main__":
    sys.exit(main())
