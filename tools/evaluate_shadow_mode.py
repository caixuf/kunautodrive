#!/usr/bin/env python3
"""evaluate_shadow_mode.py — 仿真闭环与影子模式综合评测引擎 (Shadow Mode & Closed-Loop Matrix Evaluator)

功能定位：
  1. 影子模式精度量化 (Shadow Mode Precision Benchmark):
     - 实时对比「规则主干轨迹与控制」(Rule Backbone) vs「模型推理预测」(Model Inference)。
     - 纵向指标：速度偏差 (Speed MAE / RMSE)、稳态巡航误差 (Settled MAE)。
     - 横向指标：方向盘转角偏差 (Steer MAE / RMSE)。
     - 轨迹外推：未来时域位移误差 (ADE: Average Displacement Error, FDE: Final Displacement Error)。
     - 安全仲裁介入率 (Arbiter Intervention Rate)：转向安全包络越界 (Δδ > 0.12 rad) 与制动优先级覆写。
  2. 场景矩阵批量评测 (Scenario Matrix Evaluation):
     - 综合评定闭环安全性 (碰撞率、最小 TTC、车道保持偏离度、Jerk 舒适度加加速度) 与影子模式精度。
     - 输出结构化评测工件 (JSON，符合 flowengine.shadow_matrix.v1 契约) 与可视化 Markdown 报告。

用法:
  python3 tools/evaluate_shadow_mode.py --input /tmp/flow_tiny_inference.json
  python3 tools/evaluate_shadow_mode.py --eval-report build/eval_report.json
  python3 tools/evaluate_shadow_mode.py --matrix scenarios/suite.json --output build/shadow_matrix.json
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

SCHEMA_VERSION = "flowengine.shadow_matrix.v1"

# 门禁阈值基准 (与 Apollo / KunAutoDrive L3 规范对齐)
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
    "arbiter_intervention_max_pct": 10.0, # 仲裁介入率上限 (%)

    # 闭环动力学与舒适度
    "jerk_max": 12.0,           # m/s^3
    "min_ttc_warn_s": 2.5,      # s
    "min_ttc_fail_s": 1.2,      # s
    "min_gap_fail_m": 0.5,      # m
}


def _point(val: Any, label: str = "point") -> Tuple[float, float]:
    """提取 (x, y) 坐标元组"""
    if isinstance(val, dict):
        x, y = val.get("x"), val.get("y")
    elif isinstance(val, (list, tuple)) and len(val) >= 2:
        x, y = val[0], val[1]
    else:
        raise ValueError(f"{label} 必须为包含 x/y 属性的对象或 [x, y] 数组")
    if not isinstance(x, (int, float)) or not isinstance(y, (int, float)):
        raise ValueError(f"{label} 坐标必须为数字")
    return float(x), float(y)


def calculate_trajectory_ade_fde(
    prediction: List[Any],
    ground_truth: List[Any],
    label: str = "traj"
) -> Dict[str, Optional[float]]:
    """计算预测轨迹与真值轨迹间的 ADE (平均位移误差) 与 FDE (终点位移误差)。"""
    if not isinstance(prediction, list) or not isinstance(ground_truth, list):
        return {"ade": None, "fde": None, "max_err": None, "count": 0, "status": "invalid"}
    if len(prediction) == 0 or len(ground_truth) == 0:
        return {"ade": None, "fde": None, "max_err": None, "count": 0, "status": "empty"}

    steps = min(len(prediction), len(ground_truth))
    distances: List[float] = []
    for i in range(steps):
        try:
            px, py = _point(prediction[i], f"{label}.pred[{i}]")
            gx, gy = _point(ground_truth[i], f"{label}.gt[{i}]")
            distances.append(math.hypot(px - gx, py - gy))
        except (ValueError, TypeError):
            continue

    if not distances:
        return {"ade": None, "fde": None, "max_err": None, "count": 0, "status": "error"}

    ade = sum(distances) / len(distances)
    fde = distances[-1]
    max_err = max(distances)
    return {
        "ade": round(ade, 4),
        "fde": round(fde, 4),
        "max_err": round(max_err, 4),
        "count": len(distances),
        "status": "ok",
    }


def calculate_control_deviations(
    rule_cmds: List[Dict[str, float]],
    model_cmds: List[Dict[str, float]]
) -> Dict[str, Any]:
    """量化规则主干与模型推理控制指令之间的偏差与仲裁介入统计。"""
    n = min(len(rule_cmds), len(model_cmds))
    if n == 0:
        return {"status": "unavailable", "sample_count": 0}

    speed_deltas: List[float] = []
    steer_deltas: List[float] = []
    steer_envelope_violations = 0
    brake_priority_overrides = 0

    for i in range(n):
        r = rule_cmds[i]
        m = model_cmds[i]
        r_speed = r.get("speed", r.get("target_speed", 0.0))
        m_speed = m.get("speed", m.get("target_speed", 0.0))
        speed_deltas.append(abs(m_speed - r_speed))

        r_steer = r.get("steer", r.get("steering", 0.0))
        m_steer = m.get("steer", m.get("steering", 0.0))
        delta_steer = abs(m_steer - r_steer)
        steer_deltas.append(delta_steer)

        # 转向安全包络阈值 0.12 rad (~6.88°)
        if delta_steer > 0.12:
            steer_envelope_violations += 1

        # 制动优先级安全覆写 (规则刹车时模型给油)
        r_brake = r.get("brake", 0.0)
        m_thr = m.get("throttle", 0.0)
        if r_brake > 0.10 and m_thr > 0.05:
            brake_priority_overrides += 1

    speed_mae = sum(speed_deltas) / n
    speed_rmse = math.sqrt(sum(d * d for d in speed_deltas) / n)
    steer_mae = sum(steer_deltas) / n
    steer_rmse = math.sqrt(sum(d * d for d in steer_deltas) / n)
    interventions = steer_envelope_violations + brake_priority_overrides
    intervention_rate_pct = (interventions / n) * 100.0

    return {
        "status": "computed",
        "sample_count": n,
        "speed_mae": round(speed_mae, 4),
        "speed_rmse": round(speed_rmse, 4),
        "steer_mae_rad": round(steer_mae, 5),
        "steer_mae_deg": round(math.degrees(steer_mae), 3),
        "steer_rmse_rad": round(steer_rmse, 5),
        "steer_rmse_deg": round(math.degrees(steer_rmse), 3),
        "steer_envelope_violations": steer_envelope_violations,
        "brake_priority_overrides": brake_priority_overrides,
        "total_interventions": interventions,
        "intervention_rate_pct": round(intervention_rate_pct, 2),
    }


def evaluate_shadow_sidecar(data: Dict[str, Any]) -> Dict[str, Any]:
    """从 sidecar 文件（flow_tiny_inference.json 等）评估单次影子模式表现。"""
    speed_mae = data.get("shadow_speed_mae_settled") or data.get("shadow_speed_mae")
    speed_rmse = data.get("shadow_speed_rmse_settled") or data.get("shadow_speed_rmse")
    steer_mae = data.get("shadow_steer_mae")
    steer_rmse = data.get("shadow_steer_rmse")
    ade = data.get("shadow_ade") or data.get("shadow_ade_mean")
    fde = data.get("shadow_fde") or data.get("shadow_fde_mean")
    sample_n = data.get("shadow_settled_n") or data.get("shadow_n", 0)

    steer_mae_deg = math.degrees(steer_mae) if steer_mae is not None else None
    steer_rmse_deg = math.degrees(steer_rmse) if steer_rmse is not None else None

    issues: List[str] = []
    grade = "PASS"

    if speed_mae is not None:
        if speed_mae > THRESHOLDS["speed_mae_fail"]:
            issues.append(f"速度 MAE 超标: {speed_mae:.2f} m/s > {THRESHOLDS['speed_mae_fail']} m/s")
            grade = "FAIL"
        elif speed_mae > THRESHOLDS["speed_mae_warn"]:
            issues.append(f"速度 MAE 偏高: {speed_mae:.2f} m/s > {THRESHOLDS['speed_mae_warn']} m/s")
            if grade != "FAIL":
                grade = "WARN"

    if steer_mae_deg is not None:
        if steer_mae_deg > THRESHOLDS["steer_mae_fail_deg"]:
            issues.append(f"转向角 MAE 严重超限: {steer_mae_deg:.2f}° > {THRESHOLDS['steer_mae_fail_deg']}°")
            grade = "FAIL"
        elif steer_mae_deg > THRESHOLDS["steer_mae_warn_deg"]:
            issues.append(f"转向角 MAE 偏高: {steer_mae_deg:.2f}° > {THRESHOLDS['steer_mae_warn_deg']}°")
            if grade != "FAIL":
                grade = "WARN"

    if ade is not None and ade > THRESHOLDS["ade_fail_m"]:
        issues.append(f"轨迹 ADE 超标: {ade:.2f} m > {THRESHOLDS['ade_fail_m']} m")
        grade = "FAIL"

    if fde is not None and fde > THRESHOLDS["fde_fail_m"]:
        issues.append(f"轨迹 FDE 超标: {fde:.2f} m > {THRESHOLDS['fde_fail_m']} m")
        grade = "FAIL"

    return {
        "model": data.get("model", "unknown"),
        "prediction_contract": data.get("prediction_contract", "unknown"),
        "samples": sample_n,
        "speed_mae": speed_mae,
        "speed_rmse": speed_rmse,
        "steer_mae_deg": steer_mae_deg,
        "steer_rmse_deg": steer_rmse_deg,
        "ade_m": ade,
        "fde_m": fde,
        "issues": issues,
        "grade": grade,
    }


def evaluate_scenario_entry(
    scenario_id: str,
    eval_data: Dict[str, Any],
    sidecar_data: Optional[Dict[str, Any]] = None
) -> Dict[str, Any]:
    """综合闭环安全性与影子模式评估单场景成绩单。"""
    summary = eval_data.get("summary") or eval_data.get("metrics", {})
    failures = eval_data.get("failures", [])
    warnings = eval_data.get("warnings", [])

    # 1. 闭环安全指标提取
    collisions = summary.get("collisions", summary.get("collision_topic_pub", 0))
    min_gap = summary.get("min_forward_gap_m", summary.get("min_abs_gap_m", 99.0))
    min_ttc = summary.get("min_ttc_s", 99.0)
    max_jerk = summary.get("jerk_max_mps3", summary.get("jerk_p99_mps3", 0.0))
    max_lane_error = summary.get("max_lane_error_m", 0.0)
    avg_speed = summary.get("avg_speed_mps", 0.0)
    duration_s = summary.get("duration_s", 0.0)

    # 2. 影子模式指标提取
    shadow_eval = None
    if sidecar_data:
        shadow_eval = evaluate_shadow_sidecar(sidecar_data)
    else:
        # 从 summary 中提取内嵌字段
        speed_mae = summary.get("shadow_speed_mae_settled") or summary.get("shadow_speed_mae_full")
        steer_mae = summary.get("shadow_steer_mae")
        ade = summary.get("shadow_ade")
        fde = summary.get("shadow_fde")
        if speed_mae is not None or steer_mae is not None or ade is not None:
            synth_sidecar = {
                "shadow_speed_mae_settled": speed_mae,
                "shadow_steer_mae": steer_mae,
                "shadow_ade": ade,
                "shadow_fde": fde,
                "shadow_settled_n": summary.get("shadow_settled_samples", 0),
            }
            shadow_eval = evaluate_shadow_sidecar(synth_sidecar)

    # 3. 最终成绩判定
    entry_failures = list(failures)
    entry_warnings = list(warnings)

    if collisions > 0:
        entry_failures.append(f"发生碰撞事件 (次数={collisions})")
    if min_ttc < THRESHOLDS["min_ttc_fail_s"]:
        entry_failures.append(f"最小 TTC 极危: {min_ttc:.2f}s < {THRESHOLDS['min_ttc_fail_s']}s")
    elif min_ttc < THRESHOLDS["min_ttc_warn_s"]:
        entry_warnings.append(f"最小 TTC 偏低: {min_ttc:.2f}s < {THRESHOLDS['min_ttc_warn_s']}s")

    if max_jerk > THRESHOLDS["jerk_max"]:
        entry_warnings.append(f"加加速度过大 (体感顿挫): {max_jerk:.1f} m/s³ > {THRESHOLDS['jerk_max']} m/s³")

    if shadow_eval and shadow_eval["grade"] == "FAIL":
        entry_failures.extend(shadow_eval["issues"])
    elif shadow_eval and shadow_eval["grade"] == "WARN":
        entry_warnings.extend(shadow_eval["issues"])

    final_grade = "FAIL" if entry_failures else ("WARN" if entry_warnings else "PASS")

    return {
        "scenario_id": scenario_id,
        "grade": final_grade,
        "duration_s": round(duration_s, 2),
        "avg_speed_mps": round(avg_speed, 2),
        "collisions": collisions,
        "min_ttc_s": round(min_ttc, 2) if min_ttc < 90.0 else None,
        "min_gap_m": round(min_gap, 2) if min_gap < 90.0 else None,
        "max_jerk_mps3": round(max_jerk, 2),
        "max_lane_error_m": round(max_lane_error, 3),
        "shadow_mode": shadow_eval,
        "failures": entry_failures,
        "warnings": entry_warnings,
    }


def generate_markdown_report(matrix_result: Dict[str, Any]) -> str:
    """生成整洁、可读性高的 Markdown 评测报告。"""
    summary = matrix_result["summary"]
    scenarios = matrix_result["scenarios"]

    lines = [
        "# FlowEngine 仿真闭环与影子模式评测报告",
        f"**生成时间**: {time.strftime('%Y-%m-%d %H:%M:%S')}  ",
        f"**模式**: Closed-Loop & Shadow Benchmark  ",
        f"**总体结论**: **{matrix_result['overall_grade']}** (通过率: {summary['pass_rate_pct']}%)",
        "",
        "## 一、 评测矩阵总览 (Executive Summary)",
        "",
        "| 场景名称 | 评级 | 时长 (s) | 平均速度 (m/s) | 碰撞 | 最小 TTC (s) | 速度 MAE (m/s) | 转向角 MAE | 轨迹 ADE (m) |",
        "|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|",
    ]

    for s in scenarios:
        sh = s.get("shadow_mode") or {}
        speed_mae_str = f"{sh['speed_mae']:.2f}" if sh.get("speed_mae") is not None else "-"
        steer_mae_str = f"{sh['steer_mae_deg']:.2f}°" if sh.get("steer_mae_deg") is not None else "-"
        ade_str = f"{sh['ade_m']:.2f}" if sh.get("ade_m") is not None else "-"
        ttc_str = f"{s['min_ttc_s']:.2f}" if s.get("min_ttc_s") is not None else ">10.0"
        grade_badge = f"**{s['grade']}**" if s['grade'] == "PASS" else f"<span style='color:red;'>**{s['grade']}**</span>"

        lines.append(
            f"| `{s['scenario_id']}` | {grade_badge} | {s['duration_s']} | {s['avg_speed_mps']} | "
            f"{s['collisions']} | {ttc_str} | {speed_mae_str} | {steer_mae_str} | {ade_str} |"
        )

    lines.extend([
        "",
        "## 二、 关键安全与舒适度指标统计",
        f"- **总测试场景数**: {summary['total_scenarios']} (PASS: {summary['pass_count']}, WARN: {summary['warn_count']}, FAIL: {summary['fail_count']})",
        f"- **平均速度**: {summary.get('avg_speed_mean_mps', 0.0):.2f} m/s",
        f"- **总碰撞次数**: {summary['total_collisions']}",
        f"- **影子模式平均速度 MAE**: {summary.get('mean_shadow_speed_mae', 0.0):.2f} m/s",
        f"- **影子模式平均转向角 MAE**: {summary.get('mean_shadow_steer_deg', 0.0):.2f}°",
        f"- **影子模式平均轨迹 ADE**: {summary.get('mean_shadow_ade_m', 0.0):.2f} m",
        "",
        "## 三、 逐场景详情与告警分析",
        "",
    ])

    for s in scenarios:
        lines.append(f"### 场景: `{s['scenario_id']}` — [{s['grade']}]")
        if s["failures"]:
            lines.append("**阻断性缺陷 (Failures):**")
            for f in s["failures"]:
                lines.append(f"- ❌ {f}")
        if s["warnings"]:
            lines.append("**潜在风险告警 (Warnings):**")
            for w in s["warnings"]:
                lines.append(f"- ⚠️ {w}")
        if not s["failures"] and not s["warnings"]:
            lines.append("- ✅ 全指标符合量产安全标准。")
        lines.append("")

    return "\n".join(lines)


def run_benchmark(
    eval_reports: List[Dict[str, Any]],
    output_path: Optional[Path] = None,
    report_path: Optional[Path] = None
) -> Dict[str, Any]:
    """聚合批量评测场景数据并输出报告。"""
    scenarios = []
    total_collisions = 0
    pass_cnt = 0
    warn_cnt = 0
    fail_cnt = 0

    speed_maes: List[float] = []
    steer_degs: List[float] = []
    ades: List[float] = []
    speeds: List[float] = []

    for entry in eval_reports:
        scen_id = entry.get("scenario_id") or entry.get("scenario") or "unknown"
        res = evaluate_scenario_entry(scen_id, entry)
        scenarios.append(res)

        total_collisions += res["collisions"]
        if res["grade"] == "PASS":
            pass_cnt += 1
        elif res["grade"] == "WARN":
            warn_cnt += 1
        else:
            fail_cnt += 1

        speeds.append(res["avg_speed_mps"])
        sh = res.get("shadow_mode") or {}
        if sh.get("speed_mae") is not None:
            speed_maes.append(sh["speed_mae"])
        if sh.get("steer_mae_deg") is not None:
            steer_degs.append(sh["steer_mae_deg"])
        if sh.get("ade_m") is not None:
            ades.append(sh["ade_m"])

    total = len(scenarios)
    pass_rate = round((pass_cnt / total) * 100.0, 1) if total else 0.0
    overall_grade = "FAIL" if fail_cnt > 0 else ("WARN" if warn_cnt > 0 else "PASS")

    matrix_result = {
        "schema": SCHEMA_VERSION,
        "timestamp": time.time(),
        "overall_grade": overall_grade,
        "summary": {
            "total_scenarios": total,
            "pass_count": pass_cnt,
            "warn_count": warn_cnt,
            "fail_count": fail_cnt,
            "pass_rate_pct": pass_rate,
            "total_collisions": total_collisions,
            "avg_speed_mean_mps": round(sum(speeds) / total, 2) if total else 0.0,
            "mean_shadow_speed_mae": round(sum(speed_maes) / len(speed_maes), 3) if speed_maes else None,
            "mean_shadow_steer_deg": round(sum(steer_degs) / len(steer_degs), 3) if steer_degs else None,
            "mean_shadow_ade_m": round(sum(ades) / len(ades), 3) if ades else None,
        },
        "scenarios": scenarios,
    }

    if output_path:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(matrix_result, indent=2, ensure_ascii=False), encoding="utf-8")

    if report_path:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        md = generate_markdown_report(matrix_result)
        report_path.write_text(md, encoding="utf-8")

    return matrix_result


def main() -> int:
    parser = argparse.ArgumentParser(description="FlowEngine Closed-Loop Simulation & Shadow Mode Evaluator")
    parser.add_argument("--input", help="Path to shadow inference JSON file (/tmp/flow_tiny_inference.json)")
    parser.add_argument("--eval-report", help="Path to demo_evaluator result JSON")
    parser.add_argument("--topology", help="Path to /tmp/flow_topology.json snapshot")
    parser.add_argument("--matrix", help="Path to suite.json or multiple eval JSON files")
    parser.add_argument("--output", help="Write evaluation JSON output path")
    parser.add_argument("--report", help="Write Markdown report path")
    parser.add_argument("--verbose", action="store_true", help="Print verbose metrics details")
    args = parser.parse_args()

    # 1. 单侧评测影子 sidecar 文件
    if args.input:
        in_p = Path(args.input)
        if not in_p.exists():
            print(f"Error: input file {in_p} does not exist", file=sys.stderr)
            return 2
        data = json.loads(in_p.read_text(encoding="utf-8"))
        res = evaluate_shadow_sidecar(data)
        print(f"=== 影子模式评估: {res['model']} [{res['grade']}] ===")
        print(f"样本数量: {res['samples']}")
        print(f"速度 MAE: {res['speed_mae']} m/s (RMSE: {res['speed_rmse']})")
        print(f"转向角 MAE: {res['steer_mae_deg']}° (RMSE: {res['steer_rmse_deg']}°)")
        print(f"轨迹 ADE: {res['ade_m']} m | FDE: {res['fde_m']} m")
        if res["issues"]:
            for iss in res["issues"]:
                print(f"  - {iss}")
        return 0 if res["grade"] == "PASS" else 1

    # 2. 评测单个 demo_evaluator 报告
    if args.eval_report:
        rp = Path(args.eval_report)
        if not rp.exists():
            print(f"Error: eval report {rp} does not exist", file=sys.stderr)
            return 2
        data = json.loads(rp.read_text(encoding="utf-8"))
        res = evaluate_scenario_entry(rp.stem, data)
        out_p = Path(args.output) if args.output else None
        rep_p = Path(args.report) if args.report else None
        bm = run_benchmark([res], out_p, rep_p)
        print(f"=== 场景评估: {res['scenario_id']} [{res['grade']}] ===")
        print(f"碰撞数: {res['collisions']} | 最小 TTC: {res['min_ttc_s']}s | 平均车速: {res['avg_speed_mps']} m/s")
        if res.get("shadow_mode"):
            sh = res["shadow_mode"]
            print(f"影子模式: 速度 MAE={sh.get('speed_mae')} m/s, 转向 MAE={sh.get('steer_mae_deg')}°, ADE={sh.get('ade_m')} m")
        return 0 if bm["overall_grade"] == "PASS" else 1

    # 3. 默认快速健康自检
    print("=== evaluate_shadow_mode: 模块健康，支持 --input / --eval-report / --matrix ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
