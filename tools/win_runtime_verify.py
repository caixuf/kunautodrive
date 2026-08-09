#!/usr/bin/env python3
"""Verify the native Windows/MinGW runtime without PowerShell process handling.

Starts flow_launcher and flowmond, verifies the TCP parameter bridge and dashboard
API, then terminates both children. Run from the repository root:
    python tools/win_runtime_verify.py
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-win"
BIN = BUILD / "bin"
LIB = BUILD / "lib"
PIPELINE_SOURCE = ROOT / "config" / "pipeline_windows.json"


def require(path: Path, description: str) -> Path:
    if not path.exists():
        raise RuntimeError(f"{description} not found: {path}")
    return path


def mingw_bin() -> Path:
    packages = Path(os.environ["LOCALAPPDATA"]) / "Microsoft/WinGet/Packages"
    candidates = sorted(packages.glob("BrechtSanders.WinLibs.POSIX.UCRT*/mingw64/bin"))
    if not candidates:
        raise RuntimeError("WinLibs POSIX/UCRT GCC was not found under WinGet packages")
    return require(candidates[0], "WinLibs bin directory")


def deploy_runtime_dll(source: Path, destination: Path) -> None:
    """Copy a runtime DLL only when the deployed file differs.

    A prior short demo can still be unwinding while this verifier starts, in
    which case Windows holds its loaded DLL read-only.  Re-copying identical
    compiler runtime files is unnecessary and would make the check flaky.
    """
    if destination.exists() and (
        destination.stat().st_size == source.stat().st_size
        and destination.read_bytes() == source.read_bytes()
    ):
        return
    shutil.copy2(source, destination)


def stop_old_processes() -> None:
    subprocess.run(
        ["taskkill", "/F", "/IM", "flow_launcher.exe"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
    )
    subprocess.run(
        ["taskkill", "/F", "/IM", "flowmond.exe"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
    )


def run_flowctl(env: dict[str, str], *args: str) -> str:
    result = subprocess.run(
        [str(BIN / "flowctl.exe"), "param", *args],
        cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=10, check=False,
    )
    output = result.stdout.decode("utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError(f"flowctl {' '.join(args)} failed ({result.returncode}):\n{output}")
    return output


def fetch_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=5) as response:
        return json.load(response)


def make_runtime_pipeline(runtime_temp: Path) -> Path:
    pipeline = json.loads(require(PIPELINE_SOURCE, "Windows pipeline").read_text(encoding="utf-8"))
    for process in pipeline.get("processes", []):
        library_path = process.get("library_path")
        if not library_path:
            continue
        name = Path(library_path.replace("\\", "/")).name
        if name.startswith("lib"):
            name = name[3:]
        if name.endswith(".so"):
            name = name[:-3] + ".dll"
        candidates = (LIB / name, LIB / f"lib{name}")
        process["library_path"] = str(next((path for path in candidates if path.exists()), candidates[0]))
    output = runtime_temp / "pipeline_windows_runtime.json"
    output.write_text(json.dumps(pipeline, indent=2), encoding="utf-8")
    return output


def wait_parameter_bridge(port: int, timeout_s: float = 25.0) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.5)
    raise RuntimeError(f"parameter bridge did not listen on 127.0.0.1:{port}")


def main() -> int:
    require(BIN / "flow_launcher.exe", "flow_launcher")
    require(BIN / "flowmond.exe", "flowmond")
    require(LIB, "plugin directory")

    mingw = mingw_bin()
    for dll in ("libatomic-1.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"):
        deploy_runtime_dll(require(mingw / dll, "MinGW runtime DLL"), BIN / dll)

    runtime_temp = Path(os.environ["LOCALAPPDATA"]) / "FlowEngine/tmp"
    log_dir = runtime_temp / "flow_logs"
    runtime_temp.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    runtime_pipeline = make_runtime_pipeline(runtime_temp)

    env = os.environ.copy()
    env.update({
        "FLOWENGINE_TEMP_DIR": str(runtime_temp),
        "FLOWENGINE_STATE_FILE": str(runtime_temp / "flow_topology.json"),
        "FLOW_LOG_DIR": str(log_dir),
        "FLOWENGINE_PLUGIN_DIR": str(LIB),
        "FLOW_PARAM_PORT": "18777",
        "PATH": os.pathsep.join((str(BIN), str(LIB), str(mingw), env["PATH"])),
    })

    stop_old_processes()
    launcher = flowmond = None
    launcher_log = (log_dir / "win_runtime_launcher.log").open("w", encoding="utf-8")
    flowmond_log = (log_dir / "win_runtime_flowmond.log").open("w", encoding="utf-8")
    try:
        flowmond = subprocess.Popen(
            [str(BIN / "flowmond.exe"), "--html-path", str(ROOT / "tools/flowboard/index.html")],
            cwd=ROOT, env=env, stdout=flowmond_log, stderr=subprocess.STDOUT,
        )
        time.sleep(1)
        if flowmond.poll() is not None:
            raise RuntimeError(f"flowmond exited early with {flowmond.returncode}")

        launcher = subprocess.Popen(
            [str(BIN / "flow_launcher.exe"), str(runtime_pipeline), "--duration", "30"],
            cwd=ROOT, env=env, stdout=launcher_log, stderr=subprocess.STDOUT,
        )
        time.sleep(8)
        if launcher.poll() is not None:
            raise RuntimeError(f"flow_launcher exited early with {launcher.returncode}")

        wait_parameter_bridge(int(env["FLOW_PARAM_PORT"]))
        listing = run_flowctl(env, "list")
        rows = [line for line in listing.splitlines() if line.startswith("  ")]
        if len(rows) < 10:
            raise RuntimeError(f"parameter bridge returned too few parameters:\n{listing}")

        original = run_flowctl(env, "get", "behavior.cruise_speed").strip()
        run_flowctl(env, "set", "behavior.cruise_speed", "19")
        updated = run_flowctl(env, "get", "behavior.cruise_speed").strip()
        if "19" not in updated:
            raise RuntimeError(f"hot reload was not observable: before={original}, after={updated}")

        topology = {}
        for _ in range(20):
            try:
                topology = fetch_json("http://127.0.0.1:8800/api/topology")
                scene = topology.get("metrics", {}).get("scene", topology.get("scene", {}))
                edges = scene.get("road_network", {}).get("edges", [])
                if topology.get("nodes") and edges:
                    break
            except OSError:
                pass
            time.sleep(0.5)
        else:
            raise RuntimeError("flowmond did not receive a complete topology from launcher")

        scene = topology.get("metrics", {}).get("scene", topology.get("scene", {}))
        edges = scene.get("road_network", {}).get("edges", [])
        print(f"PASS: params={len(rows)}, hot_reload={updated}, nodes={len(topology['nodes'])}, road_edges={len(edges)}")
        return 0
    finally:
        for proc in (flowmond, launcher):
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
        launcher_log.close()
        flowmond_log.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
