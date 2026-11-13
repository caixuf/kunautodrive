#!/usr/bin/env python3
"""Unit tests for tools/flow_doctor.py."""

from __future__ import annotations

import json
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools import flow_doctor  # noqa: E402


def _make_fake_repo(tmp_path: Path) -> Path:
    (tmp_path / "build" / "bin").mkdir(parents=True)
    for b in ("flow_launcher", "flowmond", "flowctl"):
        (tmp_path / "build" / "bin" / b).write_text("bin", encoding="utf-8")
    (tmp_path / "build" / "lib").mkdir(parents=True)
    for i in range(10):
        (tmp_path / "build" / "lib" / f"libn{i}_node.so").write_text("so", encoding="utf-8")
    (tmp_path / "config").mkdir(parents=True)
    (tmp_path / "config" / "pipeline.json").write_text("{}", encoding="utf-8")
    (tmp_path / "models").mkdir(parents=True)
    (tmp_path / "models" / "m.txt").write_text("model", encoding="utf-8")
    (tmp_path / "models" / "registry.json").write_text(
        json.dumps({"current_id": "v1", "versions": [{"id": "v1", "path": "models/m.txt"}]}),
        encoding="utf-8",
    )
    return tmp_path


def test_environment_stale_pid_and_shm_fix(tmp_path: Path) -> None:
    repo = _make_fake_repo(tmp_path / "repo")
    pid_dir = tmp_path / "pids"
    shm_dir = tmp_path / "shm"
    pid_dir.mkdir()
    shm_dir.mkdir()
    (pid_dir / "flow_dead.pid").write_text("999999", encoding="utf-8")
    (shm_dir / "flow_stats_bridge").write_text("shm", encoding="utf-8")

    rep_warn = flow_doctor.DoctorReport("2026-11-13T00:00:00", str(repo))
    flow_doctor.check_environment(rep_warn, repo, fix=False, pid_glob_dir=pid_dir, shm_dir=shm_dir, port=59999)
    by_name = {i.name: i for i in rep_warn.items}
    assert by_name["stale_pids"].status == "WARN"
    assert by_name["shm_residue"].status == "WARN"

    rep_fix = flow_doctor.DoctorReport("2026-11-13T00:00:00", str(repo))
    flow_doctor.check_environment(rep_fix, repo, fix=True, pid_glob_dir=pid_dir, shm_dir=shm_dir, port=59999)
    by_name_fix = {i.name: i for i in rep_fix.items}
    assert by_name_fix["stale_pids"].status == "PASS"
    assert by_name_fix["shm_residue"].status == "PASS"
    assert not (pid_dir / "flow_dead.pid").exists()
    assert not (shm_dir / "flow_stats_bridge").exists()


def test_monitor_jitter_and_collision_detection(tmp_path: Path) -> None:
    logs_dir = tmp_path / "logs"
    logs_dir.mkdir()
    (logs_dir / "monitor.log").write_text(
        "[2026-11-13 11:00:00.000] [INFO ] bus pub=1 del=2 drop=0 tasks=20 cte=0.01\n"
        "[2026-11-13 11:00:00.020] [INFO ] bus pub=2 del=4 drop=0 tasks=20 cte=0.02\n"
        "[2026-11-13 11:00:00.520] [INFO ] bus pub=3 del=6 drop=0 tasks=20 cte=0.03\n",
        encoding="utf-8",
    )
    (logs_dir / "flowsim.log").write_text(
        "[2026-11-13 11:00:00.100] [ERROR] COLLISION ego vs npc_1\n",
        encoding="utf-8",
    )

    rep = flow_doctor.DoctorReport("2026-11-13T00:00:00", str(tmp_path))
    flow_doctor.check_stalls_and_logs(rep, logs_dir, stall_ms=200.0)
    by_name = {i.name: i for i in rep.items}
    assert by_name["collisions"].status == "FAIL"
    assert by_name["monitor_jitter"].status == "WARN"
    assert by_name["monitor_jitter"].meta["stall_count"] == 1
    assert by_name["monitor_jitter"].meta["max_gap_ms"] == 500.0


def test_e2e_health_and_bundle_export(tmp_path: Path) -> None:
    repo = _make_fake_repo(tmp_path / "repo")
    logs_dir = tmp_path / "logs"
    logs_dir.mkdir()
    (logs_dir / "inference.log").write_text(
        "[2026-11-13 11:00:00.000] [INFO ] #1 [sdsc-cortex] mode=shadow ego_v=5.0 → speed=5.2 d=0.00 (shadow Δ=-2.50 vs planning)\n",
        encoding="utf-8",
    )
    topo = tmp_path / "topology.json"
    topo.write_text("{}", encoding="utf-8")

    rep = flow_doctor.DoctorReport("2026-11-13T00:00:00", str(repo))
    flow_doctor.check_e2e_health(rep, repo, logs_dir)
    by_name = {i.name: i for i in rep.items}
    assert by_name["model_registry"].status == "PASS"
    assert by_name["inference_runtime"].status == "PASS"

    # Reject /tmp test pollution in registry
    (repo / "models" / "registry.json").write_text(
        json.dumps({"current_id": "bad", "models": [{"id": "bad", "path": "/tmp/xyz/model.txt"}]}),
        encoding="utf-8",
    )
    rep_bad = flow_doctor.DoctorReport("2026-11-13T00:00:00", str(repo))
    flow_doctor.check_e2e_health(rep_bad, repo, logs_dir)
    assert {i.name: i for i in rep_bad.items}["model_registry"].status == "FAIL"

    bundle_dir = tmp_path / "bundle"
    flow_doctor.export_bundle(bundle_dir, rep, topo, logs_dir, repo)
    assert (bundle_dir / "doctor_report.json").is_file()
    assert (bundle_dir / "topology.json").is_file()
    assert (bundle_dir / "pipeline.json").is_file()
    assert (bundle_dir / "logs" / "inference.log").is_file()
