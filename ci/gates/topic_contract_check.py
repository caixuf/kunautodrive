#!/usr/bin/env python3
"""Lock pipeline subscribe/publish to NodePlugin s_inputs/s_outputs.

default profile  → set equality (JSON must declare every code topic)
experimental/hw → JSON ⊆ code only (no phantom topics)

Usage:
  python3 ci/gates/topic_contract_check.py
  python3 ci/gates/topic_contract_check.py --config config/pipeline.json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

NAME_TO_SRC = {
    "flowsim": "modules/adas_nodes/flowsim_node.cpp",
    "sensor_model": "modules/adas_nodes/sensor_model_node.c",
    "perception": "modules/adas_nodes/perception_node.cpp",
    "object_tracker": "modules/adas_nodes/object_tracker_node.c",
    "fusion": "modules/adas_nodes/fusion_node.cpp",
    "behavior_planner": "modules/adas_nodes/behavior_planner_node.cpp",
    "navigation": "modules/adas_nodes/navigation_node.c",
    "planning": "modules/adas_nodes/planning_node.cpp",
    "control": "modules/adas_nodes/control_node.cpp",
    "safety_control": "modules/adas_nodes/safety_control_node.cpp",
    "inference": "modules/adas_nodes/inference_node.cpp",
    "data_recorder": "modules/adas_nodes/data_recorder_node.c",
    "learner": "modules/adas_nodes/learner_node.c",
    "model_ota": "modules/adas_nodes/model_ota_node.c",
    "monitor": "modules/adas_nodes/monitor_node.c",
    "prediction": "modules/adas_nodes/prediction_node.c",
    "lane_detection": "modules/adas_nodes/lane_detection_node.c",
    "traffic_light_recognition": "modules/adas_nodes/traffic_light_recognition_node.c",
    "slam": "modules/adas_nodes/slam_node.cpp",
    "gps_driver": "modules/adas_nodes/gps_driver_node.c",
    "imu_driver": "modules/adas_nodes/imu_driver_node.c",
    "lidar_driver": "modules/adas_nodes/lidar_driver_node.c",
    "stereo_camera": "modules/adas_nodes/stereo_camera_node.c",
    "stereo_vision": "modules/adas_nodes/stereo_vision_node.c",
    "traversability": "modules/adas_nodes/traversability_node.c",
    "actuator": "modules/adas_nodes/actuator_node.c",
    "actuator_pwm": "modules/adas_nodes/actuator_pwm_node.c",
    "waypoint_follower": "modules/adas_nodes/waypoint_follower_node.c",
    "manual_drive": "modules/adas_nodes/manual_drive_node.c",
    "scene_assembler": "modules/adas_nodes/scene_assembler_node.c",
    "pem_collector": "modules/adas_nodes/pem_collector_node.cpp",
    "perception_fusion": "modules/adas_nodes/perception_fusion_node.cpp",
    "flowrec": "modules/adas_nodes/flowrec_node.c",
    "flowmond": "modules/adas_nodes/flowmond_node.cpp",
    "bev_detection": "modules/adas_nodes/bev_detection_node.cpp",
}

ARR_RE = re.compile(
    r"(?:static\s+)?(?:const\s+)?char\s*\*\s*s_(inputs|outputs)\[\]\s*=\s*\{([^;]*?)\};",
    re.S,
)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//.*?$", " ", text, flags=re.M)
    return text


def load_macros() -> dict[str, str]:
    macros: dict[str, str] = {}
    for hdr in (ROOT / "include").glob("*.h"):
        t = strip_comments(hdr.read_text(errors="ignore"))
        for m in re.finditer(r'#define\s+(TOPIC_\w+)\s+"([^"]+)"', t):
            macros[m.group(1)] = m.group(2)
        for m in re.finditer(r'#define\s+(\w+)\s+"([a-z0-9_][^"]*/[^"]+)"', t):
            macros.setdefault(m.group(1), m.group(2))
    return macros


def parse_code_topics(src: Path, macros: dict[str, str]) -> dict[str, list[str]]:
    raw = src.read_text(errors="ignore")
    loc = {
        m.group(1): m.group(2)
        for m in re.finditer(r'#define\s+(\w+)\s+"([^"]+)"', strip_comments(raw))
    }
    t = strip_comments(raw)
    out: dict[str, list[str]] = {"inputs": [], "outputs": []}
    for kind, body in ARR_RE.findall(t):
        items: list[str] = []
        for part in body.split(","):
            tok = part.strip()
            if not tok or tok in ("NULL", "nullptr"):
                continue
            if tok.startswith('"') and tok.endswith('"'):
                items.append(tok[1:-1])
            else:
                resolved = loc.get(tok) or macros.get(tok)
                if not resolved:
                    raise ValueError(f"{src}: unresolved token {tok} in s_{kind}")
                items.append(resolved)
        out[kind] = items
    return out


def json_topics(proc: dict) -> tuple[set[str], set[str]]:
    subs = set(proc.get("subscribe") or [])
    pubs: set[str] = set()
    for x in proc.get("publish") or []:
        pubs.add(x["topic"] if isinstance(x, dict) else x)
    return subs, pubs


def infer_profile(path: Path, cfg: dict) -> str:
    if cfg.get("profile"):
        return str(cfg["profile"])
    if path.name in ("pipeline.json", "pipeline_windows.json"):
        return "default"
    return "experimental"


def check_config(path: Path, macros: dict[str, str]) -> list[str]:
    cfg = json.loads(path.read_text())
    profile = infer_profile(path, cfg)
    errors: list[str] = []
    for proc in cfg.get("processes") or []:
        name = proc.get("name")
        if not name:
            continue
        rel = NAME_TO_SRC.get(name)
        if not rel:
            continue
        src = ROOT / rel
        if not src.exists():
            errors.append(f"{path.name}: node '{name}' source missing: {rel}")
            continue
        try:
            code = parse_code_topics(src, macros)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        code_in, code_out = set(code["inputs"]), set(code["outputs"])
        json_in, json_out = json_topics(proc)

        phantom_in = json_in - code_in
        phantom_out = json_out - code_out
        if phantom_in:
            errors.append(
                f"{path.name}:{name}: JSON subscribe not in s_inputs: {sorted(phantom_in)}"
            )
        if phantom_out:
            errors.append(
                f"{path.name}:{name}: JSON publish not in s_outputs: {sorted(phantom_out)}"
            )

        if profile == "default":
            missing_in = code_in - json_in
            missing_out = code_out - json_out
            if missing_in:
                errors.append(
                    f"{path.name}:{name}: JSON missing s_inputs: {sorted(missing_in)}"
                )
            if missing_out:
                errors.append(
                    f"{path.name}:{name}: JSON missing s_outputs: {sorted(missing_out)}"
                )
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", action="append", default=[])
    args = ap.parse_args()
    configs = (
        [Path(p) for p in args.config]
        if args.config
        else sorted((ROOT / "config").glob("pipeline*.json"))
    )
    macros = load_macros()
    errors: list[str] = []
    for path in configs:
        resolved = path if path.is_absolute() else ROOT / path
        if not resolved.is_file():
            errors.append(f"missing config: {path}")
            continue
        errors.extend(check_config(resolved, macros))

    if errors:
        for e in errors:
            print(f"::error::{e}")
        print(f"topic-contract-gate FAILED ({len(errors)} issue(s)).")
        return 1
    print(f"✓ topic contract OK ({len(configs)} config(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
