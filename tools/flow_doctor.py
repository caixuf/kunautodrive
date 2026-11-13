#!/usr/bin/env python3
"""flow_doctor.py — KunAutoDrive 统一自诊断工具。

聚合环境/构建产物检查、僵尸资源清理、pipeline_check 链路体检、
日志错误与进程卡顿尖峰扫描、E2E 模型/影子健康度检查，以及故障现场一键打包。

用法:
    python3 tools/flow_doctor.py
    python3 tools/flow_doctor.py --json
    python3 tools/flow_doctor.py --fix
    python3 tools/flow_doctor.py --bundle /tmp/flow_bundle_01
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass, field
from datetime import datetime
import json
import os
from pathlib import Path
import re
import shutil
import socket
import sys
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools import pipeline_check  # noqa: E402

_TS_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]\s+\[(ERROR|WARN |INFO |DEBUG)\]\s+(.*)$")
_DROP_RE = re.compile(r"\bdrop=(\d+)\b")
_CTE_RE = re.compile(r"\bcte=([-+]?\d+(?:\.\d+)?)\b")
_SHADOW_DELTA_RE = re.compile(r"shadow Δ=([-+]?\d+(?:\.\d+)?)\s+vs\s+planning")


@dataclass
class CheckItem:
    section: str
    name: str
    status: str  # PASS | WARN | FAIL
    detail: str
    meta: dict[str, Any] = field(default_factory=dict)


@dataclass
class DoctorReport:
    generated_at: str
    repo_root: str
    items: list[CheckItem] = field(default_factory=list)

    def add(self, section: str, name: str, status: str, detail: str, **meta: Any) -> None:
        self.items.append(CheckItem(section=section, name=name, status=status, detail=detail, meta=meta))

    def counts(self) -> dict[str, int]:
        return {
            "pass": sum(1 for i in self.items if i.status == "PASS"),
            "warn": sum(1 for i in self.items if i.status == "WARN"),
            "fail": sum(1 for i in self.items if i.status == "FAIL"),
        }

    def exit_code(self) -> int:
        c = self.counts()
        if c["fail"] > 0:
            return 2
        if c["warn"] > 0:
            return 1
        return 0

    def to_dict(self) -> dict[str, Any]:
        c = self.counts()
        overall = "FAIL" if c["fail"] else ("WARN" if c["warn"] else "PASS")
        return {
            "schema": "flow_doctor.v1",
            "generated_at": self.generated_at,
            "repo_root": self.repo_root,
            "overall": overall,
            "counts": c,
            "items": [asdict(i) for i in self.items],
        }


def _pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _port_open(port: int, host: str = "127.0.0.1") -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.2)
        return sock.connect_ex((host, port)) == 0


def check_environment(
    report: DoctorReport,
    repo_root: Path = REPO_ROOT,
    *,
    fix: bool = False,
    pid_glob_dir: Path = Path("/tmp"),
    shm_dir: Path = Path("/dev/shm"),
    port: int = 8800,
) -> None:
    """检查构建产物、僵尸 PID/SHM 残留与端口占用。"""
    bins = ["flow_launcher", "flowmond", "flowctl"]
    missing_bins = [b for b in bins if not (repo_root / "build" / "bin" / b).is_file()]
    if missing_bins:
        report.add("environment", "binaries", "FAIL", f"缺少构建产物: {', '.join(missing_bins)}")
    else:
        report.add("environment", "binaries", "PASS", f"核心可执行文件齐备 ({len(bins)}/3)")

    so_files = list((repo_root / "build" / "lib").glob("lib*_node.so")) if (repo_root / "build" / "lib").is_dir() else []
    if len(so_files) < 10:
        report.add("environment", "node_plugins", "WARN", f"仅发现 {len(so_files)} 个节点插件 .so")
    else:
        report.add("environment", "node_plugins", "PASS", f"已发现 {len(so_files)} 个节点插件 .so")

    stale_pids: list[Path] = []
    live_pids: list[str] = []
    for pf in sorted(pid_glob_dir.glob("flow_*.pid")):
        try:
            pid = int(pf.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            stale_pids.append(pf)
            continue
        if _pid_alive(pid):
            live_pids.append(f"{pf.name}({pid})")
        else:
            stale_pids.append(pf)

    if stale_pids:
        if fix:
            for pf in stale_pids:
                pf.unlink(missing_ok=True)
            report.add("environment", "stale_pids", "PASS", f"已清理 {len(stale_pids)} 个僵尸 PID 文件")
        else:
            report.add(
                "environment",
                "stale_pids",
                "WARN",
                f"发现 {len(stale_pids)} 个失效 PID 文件（可用 --fix 清理）",
                files=[str(p) for p in stale_pids],
            )
    else:
        report.add("environment", "stale_pids", "PASS", f"无失效 PID 残留（运行中: {len(live_pids)}）")

    shm_entries = sorted(shm_dir.glob("flow_*")) if shm_dir.is_dir() else []
    if shm_entries and not live_pids:
        if fix:
            for s in shm_entries:
                s.unlink(missing_ok=True)
            report.add("environment", "shm_residue", "PASS", f"已清理 {len(shm_entries)} 个 SHM 残留")
        else:
            report.add(
                "environment",
                "shm_residue",
                "WARN",
                f"无存活进程但存在 {len(shm_entries)} 个 /dev/shm/flow_* 残留（可用 --fix 清理）",
                files=[s.name for s in shm_entries],
            )
    else:
        report.add("environment", "shm_residue", "PASS", f"SHM 状态正常 (entries={len(shm_entries)}, live={len(live_pids)})")

    port_busy = _port_open(port)
    if port_busy and not live_pids:
        report.add("environment", "port_8800", "WARN", f"端口 {port} 被占用但未检测到 flow_*.pid")
    else:
        status_txt = "监听中" if port_busy else "空闲"
        report.add("environment", "port_8800", "PASS", f"端口 {port} {status_txt}")


def check_pipeline_snapshot(report: DoctorReport, topology_path: Path) -> None:
    """复用 pipeline_check.py 检查拓扑、Topic 频率与感知/行为完整性。"""
    if not topology_path.is_file():
        report.add("pipeline", "topology_snapshot", "WARN", f"未找到 {topology_path}（请先运行仿真）")
        return

    try:
        data = json.loads(topology_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        report.add("pipeline", "topology_snapshot", "FAIL", f"解析 {topology_path} 失败: {exc}")
        return

    for sec_key, (desc, func) in pipeline_check.SECTIONS.items():
        sec = pipeline_check._Section(sec_key, desc)
        func(data, sec)
        sec.summarize()
        if sec.fail_count > 0:
            st = "FAIL"
        elif sec.warn_count > 0:
            st = "WARN"
        else:
            st = "PASS"
        detail = f"{desc}: {sec.pass_count} pass / {sec.warn_count} warn / {sec.fail_count} fail"
        bad_msgs = [
            r.msg
            for r in sec.results
            if not r.ok
        ]
        report.add("pipeline", sec_key, st, detail, issues=bad_msgs[:10])


def _percentile(sorted_vals: list[float], pct: float) -> float:
    if not sorted_vals:
        return 0.0
    idx = min(len(sorted_vals) - 1, max(0, int(round((pct / 100.0) * (len(sorted_vals) - 1)))))
    return sorted_vals[idx]


def analyze_monitor_log(monitor_log: Path, stall_ms: float = 200.0) -> dict[str, Any]:
    """解析 monitor.log 时间戳间隔、bus drop 与 CTE 峰值。"""
    timestamps: list[datetime] = []
    stalls: list[dict[str, Any]] = []
    max_drop = 0
    max_abs_cte = 0.0

    for line in monitor_log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = _TS_RE.match(line)
        if not m:
            continue
        ts = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
        msg = m.group(3)
        if "bus pub=" in msg:
            if timestamps:
                gap_ms = (ts - timestamps[-1]).total_seconds() * 1000.0
                if gap_ms > stall_ms:
                    stalls.append({"at": m.group(1), "gap_ms": round(gap_ms, 1)})
            timestamps.append(ts)
        dm = _DROP_RE.search(msg)
        if dm:
            max_drop = max(max_drop, int(dm.group(1)))
        cm = _CTE_RE.search(msg)
        if cm:
            max_abs_cte = max(max_abs_cte, abs(float(cm.group(1))))

    gaps = sorted(
        (timestamps[i] - timestamps[i - 1]).total_seconds() * 1000.0
        for i in range(1, len(timestamps))
    )
    return {
        "samples": len(timestamps),
        "p50_gap_ms": round(_percentile(gaps, 50), 2),
        "p95_gap_ms": round(_percentile(gaps, 95), 2),
        "p99_gap_ms": round(_percentile(gaps, 99), 2),
        "max_gap_ms": round(gaps[-1], 1) if gaps else 0.0,
        "stall_count": len(stalls),
        "stalls": stalls[:10],
        "max_drop": max_drop,
        "max_abs_cte": round(max_abs_cte, 3),
    }


def check_stalls_and_logs(report: DoctorReport, logs_dir: Path, stall_ms: float = 200.0) -> None:
    """扫描 /tmp/flow_logs/*.log 中的错误、碰撞、掉包与 monitor 停顿尖峰。"""
    if not logs_dir.is_dir():
        report.add("stalls_and_logs", "logs_dir", "WARN", f"日志目录不存在: {logs_dir}")
        return

    errors: list[str] = []
    warnings: list[str] = []
    collisions: list[str] = []
    for lf in sorted(logs_dir.glob("*.log")):
        for line in lf.read_text(encoding="utf-8", errors="replace").splitlines():
            if "COLLISION" in line:
                collisions.append(f"{lf.name}: {line}")
            elif "[ERROR]" in line:
                errors.append(f"{lf.name}: {line}")
            elif "[WARN ]" in line:
                warnings.append(f"{lf.name}: {line}")

    if collisions:
        report.add("stalls_and_logs", "collisions", "FAIL", f"检测到 {len(collisions)} 条 COLLISION 日志", samples=collisions[:5])
    else:
        report.add("stalls_and_logs", "collisions", "PASS", "所有节点日志 0 COLLISION")

    if errors:
        report.add("stalls_and_logs", "node_errors", "FAIL", f"检测到 {len(errors)} 条 ERROR 日志", samples=errors[:5])
    elif warnings:
        report.add("stalls_and_logs", "node_errors", "WARN", f"0 ERROR，但有 {len(warnings)} 条 WARN 日志", samples=warnings[:5])
    else:
        report.add("stalls_and_logs", "node_errors", "PASS", "所有节点日志 0 ERROR / 0 WARN")

    monitor_log = logs_dir / "monitor.log"
    if not monitor_log.is_file():
        report.add("stalls_and_logs", "monitor_jitter", "WARN", f"未找到 {monitor_log}")
        return

    stats = analyze_monitor_log(monitor_log, stall_ms=stall_ms)
    if stats["max_drop"] > 0:
        report.add("stalls_and_logs", "bus_drop", "FAIL", f"消息总线存在丢包 max_drop={stats['max_drop']}", **stats)
    else:
        report.add("stalls_and_logs", "bus_drop", "PASS", "消息总线 drop=0")

    if stats["stall_count"] > 0:
        report.add(
            "stalls_and_logs",
            "monitor_jitter",
            "WARN",
            (
                f"检测到 {stats['stall_count']} 次后端停顿 (>{stall_ms:.0f}ms), "
                f"max={stats['max_gap_ms']}ms, p99={stats['p99_gap_ms']}ms"
            ),
            **stats,
        )
    else:
        report.add(
            "stalls_and_logs",
            "monitor_jitter",
            "PASS",
            f"后端采样平稳 (samples={stats['samples']}, p99={stats['p99_gap_ms']}ms, max={stats['max_gap_ms']}ms)",
            **stats,
        )


def check_e2e_health(
    report: DoctorReport,
    repo_root: Path = REPO_ROOT,
    logs_dir: Path = Path("/tmp/flow_logs"),
) -> None:
    """检查模型注册表合法性与 inference 节点影子/延迟状态。"""
    reg_path = repo_root / "models" / "registry.json"
    if not reg_path.is_file():
        report.add("e2e_health", "model_registry", "WARN", f"未找到 {reg_path}")
    else:
        try:
            reg = json.loads(reg_path.read_text(encoding="utf-8"))
            cur_id = reg.get("current_id", "")
            models = {m.get("id"): m for m in reg.get("models", []) if isinstance(m, dict)}
            cur = models.get(cur_id, {})
            mpath = str(cur.get("path", ""))
            resolved = Path(mpath) if Path(mpath).is_absolute() else (repo_root / mpath)
            if mpath.startswith("/tmp/"):
                report.add("e2e_health", "model_registry", "FAIL", f"current_id={cur_id} 指向临时目录: {mpath}")
            elif not mpath or not resolved.exists():
                report.add("e2e_health", "model_registry", "WARN", f"current_id={cur_id} 模型文件不存在: {mpath}")
            else:
                report.add("e2e_health", "model_registry", "PASS", f"current_id={cur_id} -> {mpath}")
        except (OSError, json.JSONDecodeError) as exc:
            report.add("e2e_health", "model_registry", "FAIL", f"解析 {reg_path} 失败: {exc}")

    inf_log = logs_dir / "inference.log"
    if not inf_log.is_file():
        report.add("e2e_health", "inference_runtime", "WARN", f"未找到 {inf_log}")
        return

    text = inf_log.read_text(encoding="utf-8", errors="replace")
    deltas = [abs(float(m.group(1))) for m in _SHADOW_DELTA_RE.finditer(text)]
    heuristic = "heuristic fallback" in text
    budget_exceeded = "latency budget" in text.lower() or "budget_exceeded" in text.lower()
    if budget_exceeded:
        report.add("e2e_health", "inference_runtime", "WARN", "inference.log 出现推理延迟预算超限告警")
    elif deltas:
        mean_d = round(sum(deltas) / len(deltas), 2)
        max_d = round(max(deltas), 2)
        mode_note = "heuristic fallback" if heuristic else "model active"
        report.add(
            "e2e_health",
            "inference_runtime",
            "PASS",
            f"影子推理运行中 ({mode_note}, samples={len(deltas)}, mean|Δ|={mean_d}, max|Δ|={max_d})",
            heuristic_fallback=heuristic,
            mean_abs_delta=mean_d,
            max_abs_delta=max_d,
        )
    else:
        report.add("e2e_health", "inference_runtime", "WARN", "inference.log 未发现影子推理采样输出")


def export_bundle(
    bundle_dir: Path,
    report: DoctorReport,
    topology_path: Path,
    logs_dir: Path,
    repo_root: Path = REPO_ROOT,
) -> None:
    """将自诊断报告、配置、拓扑快照与节点日志打包至 bundle_dir。"""
    bundle_dir.mkdir(parents=True, exist_ok=True)
    (bundle_dir / "doctor_report.json").write_text(
        json.dumps(report.to_dict(), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    if topology_path.is_file():
        shutil.copy2(topology_path, bundle_dir / "topology.json")
    cfg = repo_root / "config" / "pipeline.json"
    if cfg.is_file():
        shutil.copy2(cfg, bundle_dir / "pipeline.json")
    if logs_dir.is_dir():
        dst_logs = bundle_dir / "logs"
        dst_logs.mkdir(exist_ok=True)
        for lf in logs_dir.glob("*.log"):
            shutil.copy2(lf, dst_logs / lf.name)


def print_human_report(report: DoctorReport) -> None:
    cur_sec = None
    for item in report.items:
        if item.section != cur_sec:
            cur_sec = item.section
            print(f"\n── {cur_sec} " + "─" * max(4, 54 - len(cur_sec)))
        tag = {"PASS": "✓ PASS", "WARN": "! WARN", "FAIL": "✗ FAIL"}.get(item.status, item.status)
        print(f"  [{tag}] {item.name}: {item.detail}")
    c = report.counts()
    print("\n" + "=" * 60)
    print(f"flow_doctor 总结: {report.to_dict()['overall']} ({c['pass']} pass / {c['warn']} warn / {c['fail']} fail)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="KunAutoDrive 统一自诊断工具")
    parser.add_argument("--topology", type=Path, default=Path("/tmp/flow_topology.json"), help="拓扑快照路径")
    parser.add_argument("--logs-dir", type=Path, default=Path("/tmp/flow_logs"), help="节点日志目录")
    parser.add_argument("--stall-ms", type=float, default=200.0, help="后端停顿告警阈值 (ms)")
    parser.add_argument("--fix", action="store_true", help="自动清理僵尸 PID 与 SHM 残留")
    parser.add_argument("--json", action="store_true", help="输出 JSON 格式诊断报告")
    parser.add_argument("--bundle", type=Path, default=None, help="打包诊断现场到指定目录")
    args = parser.parse_args(argv)

    report = DoctorReport(
        generated_at=datetime.now().isoformat(timespec="seconds"),
        repo_root=str(REPO_ROOT),
    )
    check_environment(report, REPO_ROOT, fix=args.fix)
    check_pipeline_snapshot(report, args.topology)
    check_stalls_and_logs(report, args.logs_dir, stall_ms=args.stall_ms)
    check_e2e_health(report, REPO_ROOT, args.logs_dir)

    if args.bundle is not None:
        export_bundle(args.bundle, report, args.topology, args.logs_dir, REPO_ROOT)
        report.add("bundle", "export", "PASS", f"已打包故障现场至 {args.bundle}")

    if args.json:
        print(json.dumps(report.to_dict(), ensure_ascii=False, indent=2))
    else:
        print_human_report(report)
    return report.exit_code()


if __name__ == "__main__":
    raise SystemExit(main())
