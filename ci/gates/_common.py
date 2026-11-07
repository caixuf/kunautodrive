#!/usr/bin/env python3
"""ci/gates 共享小工具 —— 只放跨 gate 重复的纯函数，不放任何 gate 策略。

存在的理由：`processes[].params` 是"字符串型 JSON、需二次解析"的隐式契约，
此前 sensor_wiring / model_takeover 各写一份 `_params`；注释剥离也有多份实现
（替换字符还不一致）。这些是纯机械逻辑，收敛到一处，改一次即可。
"""

from __future__ import annotations

import json
import re


def params(proc: dict) -> dict:
    """`processes[].params` 可能是 JSON 字符串或 dict，统一成 dict。"""
    raw = proc.get("params")
    if isinstance(raw, str):
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {}
    return raw if isinstance(raw, dict) else {}


def node(cfg: dict, name: str) -> dict | None:
    """按 processes[].name 取节点（不要全局 grep mode：scheduler 也有 mode）。"""
    for proc in cfg.get("processes") or []:
        if isinstance(proc, dict) and proc.get("name") == name:
            return proc
    return None


def strip_comments(text: str) -> str:
    """剥离 C/C++ 块注释与行注释（用空格替换，保持行结构）。"""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//.*?$", " ", text, flags=re.M)
    return text


def strip_comments_and_dead_code(text: str) -> str:
    """再额外去掉 `#if 0 ... #endif` 死代码块。

    仅用于"子串存在性"类检查（如 safety_arbiter_apply 是否在控制链上）：
    注释掉的调用与 `#if 0` 块里的调用都不得让门禁假绿。
    """
    text = strip_comments(text)
    return re.sub(r"#\s*if\s+0\b.*?#\s*endif", "", text, flags=re.S)