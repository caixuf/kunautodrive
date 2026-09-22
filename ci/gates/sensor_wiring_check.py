#!/usr/bin/env python3
"""sensor_wiring_check.py — sensor 模式接线一致性 gate（纯静态，秒级）

背景（2026-09-22 定位）：真点云链路上有**三处**必须成对出现的配置，分散在两个
节点里，任何一处不一致都**静默**失效、且现象都表现为"改了没效果"：

1. `perception.mode == "sensor"` 时，`sensor_model.lidar_mode` 必须是 1。
   否则生产者根本不发 `sensor/lidar_points`，而消费者一直在等它（只在日志里
   刷 "no fresh sensor/lidar_points"），感知整条链路全盲。

2. `perception.lidar_max_range_m` 不得小于 `sensor_model.lidar_max_range_m`。
   两个节点**各有一份量程门**（`sensor_model_node.c` 生产者、
   `perception_node.cpp:495` 消费者默认 60m），消费者更小时会把生产者发来的
   远处点**静默全丢**（无任何日志）。实测把生产者 60m→120m 而消费者不动，
   锥内识别率从 86.9% 掉到 37.0%。

3. 两个量程键必须**显式声明**。依赖代码默认值（都是 60.0）会让"生产者改了、
   消费者没改"这种分歧无法在静态检查里被发现。

这与 CLAUDE.md 的铁律"同一功能出现第二份实现 = 违规"同源：量程目前就是
"生产者参数 + 消费者参数"两份实现，这条 gate 把它们钉在同一个值域里。

Usage:
  python3 ci/gates/sensor_wiring_check.py
  python3 ci/gates/sensor_wiring_check.py --config config/pipeline_sensor.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _params(proc: dict) -> dict:
    """processes[].params 是 JSON 编码的字符串，需二次 json.loads（兼容 dict）。"""
    raw = proc.get("params")
    if isinstance(raw, str):
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {}
    return raw if isinstance(raw, dict) else {}


def _node(cfg: dict, name: str) -> dict | None:
    for proc in cfg.get("processes") or []:
        if isinstance(proc, dict) and proc.get("name") == name:
            return proc
    return None


def check_config(path: Path) -> list[str]:
    """Return the list of consistency violations for one launcher config."""
    try:
        cfg = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"{path.name}: cannot read/parse config ({exc})"]

    if not isinstance(cfg, dict):
        return [f"{path.name}: config root is not an object"]

    # 必须定位到 processes[].name=="perception"，不能全局 grep "mode"：
    # 每个 config 的 scheduler 也有 "mode": "choreo"。
    perc_node = _node(cfg, "perception")
    if perc_node is None:
        return []  # 该 config 没有感知节点（如 pipeline_car.json）→ 不适用
    if _params(perc_node).get("mode") != "sensor":
        return []  # 非 sensor 模式（ground_truth 直通）→ 不适用

    errors: list[str] = []
    sens_node = _node(cfg, "sensor_model")
    if sens_node is None:
        return [f"{path.name}: perception.mode=sensor 但没有 sensor_model 节点 —— "
                f"没人发布 sensor/lidar_points，感知将永远等不到点云"]
    sens = _params(sens_node)

    # ── 不变式 1：生产者必须真的发点云 ──
    if sens.get("lidar_mode") != 1:
        errors.append(
            f"{path.name}: perception.mode=sensor 但 sensor_model.lidar_mode="
            f"{sens.get('lidar_mode')!r}（必须显式设为 1）—— 生产者不发 "
            f"sensor/lidar_points，感知链路全盲"
        )

    # ── 不变式 3：量程必须显式声明（否则依赖代码默认值，分歧无法被发现）──
    s_range = sens.get("lidar_max_range_m")
    p_range = _params(perc_node).get("lidar_max_range_m")
    if s_range is None or p_range is None:
        errors.append(
            f"{path.name}: sensor 模式必须显式声明两处 lidar_max_range_m "
            f"(sensor_model={s_range!r}, perception={p_range!r}) —— 依赖代码默认值会让"
            f"两处量程门的分歧静默通过"
        )
        return errors

    # ── 不变式 2：消费者的量程门不得比生产者更紧 ──
    try:
        s_val, p_val = float(s_range), float(p_range)
    except (TypeError, ValueError):
        return errors + [
            f"{path.name}: lidar_max_range_m 必须是数值 "
            f"(sensor_model={s_range!r}, perception={p_range!r})"
        ]
    if p_val < s_val:
        errors.append(
            f"{path.name}: perception.lidar_max_range_m={p_val:g} < "
            f"sensor_model.lidar_max_range_m={s_val:g} —— 消费者会把 "
            f">{p_val:g}m 的点静默丢弃（无日志），远处目标全部消失"
        )
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", action="append", default=[],
                    help="config to check (repeatable); default: config/pipeline*.json")
    args = ap.parse_args()

    configs = ([Path(p) for p in args.config] if args.config
               else sorted((ROOT / "config").glob("pipeline*.json")))

    errors: list[str] = []
    for path in configs:
        resolved = path if path.is_absolute() else ROOT / path
        if not resolved.is_file():
            errors.append(f"missing config: {path}")
            continue
        errors.extend(check_config(resolved))

    if errors:
        for e in errors:
            print(f"::error::{e}")
        print(f"sensor-wiring-gate FAILED ({len(errors)} issue(s)).")
        return 1
    print(f"✓ sensor wiring OK ({len(configs)} config(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
