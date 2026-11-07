#!/usr/bin/env python3
"""model_takeover_boundary_check.py — 方向四「模型 + 规则混合」接管边界 gate。

规则层是**最低安全护栏**，模型层只有在影子评估/闭环门禁通过后、并且仲裁器
（`safety_arbiter`）仍在执行路径上时，才允许更接近控制出口。本 gate 用静态
检查钉住三条边界：

1. 除 `pipeline_cortex.json`（profile=experimental，启动即告警）外，
   所有 pipeline 的 `inference.control_mode` 必须是 `shadow`（缺省也算 shadow）。
   防止模型被悄悄接成 `direct_ctrl` 直接驱动默认 demo/CI 路径。
2. `pipeline_cortex.json` 必须是 `profile: experimental` —— 允许它做实验，
   但必须能被一眼认出不是主路径。
3. `safety_control_node.cpp` 必须调用 `safety_arbiter_apply(...)`。
   仲裁器是"模型不达标即回退规则"的唯一执行点；把它从控制链里拿掉，
   模型层就失去护栏（本 gate 只查调用存在，不查参数取舍）。

Usage:
  python3 ci/gates/model_takeover_boundary_check.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORTEX_CONFIG = "pipeline_cortex.json"
ARBITER_CALL = "safety_arbiter_apply("
ARBITER_SRC = ROOT / "modules" / "adas_nodes" / "safety_control_node.cpp"


def _params(proc: dict) -> dict:
    raw = proc.get("params")
    if isinstance(raw, str):
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {}
    return raw if isinstance(raw, dict) else {}


def check_config(path: Path) -> list[str]:
    try:
        cfg = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"{path.name}: cannot read/parse config ({exc})"]

    inference = next(
        (p for p in cfg.get("processes") or []
         if isinstance(p, dict) and p.get("name") == "inference"),
        None,
    )
    if inference is None:
        return []  # 该 config 没有模型节点 → 不适用

    errors: list[str] = []
    mode = _params(inference).get("control_mode", "shadow")
    is_cortex = path.name == CORTEX_CONFIG

    if is_cortex:
        profile = cfg.get("profile", "")
        if profile != "experimental":
            errors.append(
                f"{path.name}: direct_ctrl 实验配置必须声明 profile=experimental，"
                f"当前为 {profile!r} —— 否则它会被当成主路径"
            )
        return errors

    if mode != "shadow":
        errors.append(
            f"{path.name}: inference.control_mode={mode!r}（要求 shadow）—— "
            f"模型直接驱动非实验管线绕过影子评估；只有 {CORTEX_CONFIG} 允许 direct_ctrl"
        )
    return errors


def check_arbiter_in_path() -> list[str]:
    if not ARBITER_SRC.is_file():
        return [f"missing {ARBITER_SRC.relative_to(ROOT)} — 无法确认安全仲裁仍在控制链"]
    src = ARBITER_SRC.read_text(encoding="utf-8")
    if ARBITER_CALL not in src:
        return [
            "safety_control_node.cpp 未调用 safety_arbiter_apply(...) —— "
            "规则主干失去最低安全护栏（模型不达标时不再回退规则）"
        ]
    return []


def main() -> int:
    configs = sorted((ROOT / "config").glob("pipeline*.json"))
    errors: list[str] = []
    for path in configs:
        errors.extend(check_config(path))
    errors.extend(check_arbiter_in_path())

    if errors:
        for e in errors:
            print(f"::error::{e}")
        print(f"model-takeover-boundary-gate FAILED ({len(errors)} issue(s)).")
        return 1
    print(f"✓ model takeover boundary OK ({len(configs)} config(s), arbiter in path)")
    return 0


if __name__ == "__main__":
    sys.exit(main())