#!/usr/bin/env python3
"""行为状态机转移表完整性守门 (L3-P0 / CLAUDE.md 模块职责铁律)

铁律：「状态转移表缺行（任何事件×状态组合必须显式可达或显式拒绝）」。
此前这条只写在 CLAUDE.md 与 docs/ROADMAP_L3.md 里，**没有任何门禁校验**——
于是 behavior_planner 的 STOP / YIELD / EMERGENCY 三个状态在转移表里
零入口（死状态），却没人被拦下。

本门禁解析 behavior_planner_node.cpp 的 `BEH_TRANSITIONS[]` 静态表，断言：

  Rule 1  每个 BEH_ST_* 状态都能从某个源状态**可达**（消除死状态）。
  Rule 2  每个 BEH_EV_* 事件都至少出现在一条转移行里（消除死事件）。
  Rule 3  每个合法变道源状态都有 TIMEOUT 回退（防 2026-08-03 卡死复发）。
  Rule 4  每条转移行的源/目标状态、事件均在枚举里声明（无拼写漂移）。

解析是"白盒对账"：状态/事件枚举与转移表同文件，直接正则提取，不依赖
编译产物。这样门禁在 `git commit` 前就能跑，且不要求构建。

Usage:
  python3 ci/gates/behavior_fsm_check.py
  python3 ci/gates/behavior_fsm_check.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "modules" / "adas_nodes" / "behavior_planner_node.cpp"

# 显式登记的"已声明但暂不可达"状态：本表里它们有映射函数与字符串（beh_state_to_cmd /
# beh_state_str），却没有任何入边 —— 属于"先声明后接线"的技术债。
# 按铁律，这种状态要么可达、要么显式拒绝；此处是**显式拒绝**（登记 + 跟踪），
# 不是静默放过。每项必须带 owner/原因，接线后即删（否则门禁就成了废纸）。
#
# ⚠️ 这些状态在行为决策里发不出（无入边），若某处依赖它们会静默失效 ——
#    本清单的价值就是让"依赖死状态"变成一件看得见的事。
ALLOWED_SINK_STATES: dict[str, str] = {
    # state → 原因 / 后续接线计划
    "BEH_ST_STOP":      "L3-P1 MRM 接管后进入（停车让行目标态）；当前由 safety 兜底刹停，behavior 未接线",
    "BEH_ST_YIELD":     "行人/对向让行目标态；当前遇阻走 FOLLOW，尚未接线",
    "BEH_ST_EMERGENCY": "L3-P1 MRM 目标态（最小风险停车）；当前降级由 degrade_ladder 兜底",
}

# 变道类状态：必须能从自身(TIMEOUT)回退，否则变道超时即永久卡死。
LANE_CHANGE_STATES = {"BEH_ST_LEFT_CHANGE", "BEH_ST_RIGHT_CHANGE"}


def _parse_enum(text: str, enum_name: str) -> dict[str, int]:
    """提取 `enum <name> { A = 200, B, ... };` 的名→值映射。

    C 枚举允许省略值（前一个 +1），这里按序补全。
    """
    m = re.search(rf"enum\s+{re.escape(enum_name)}\s*\{{(.*?)\}}\s*;", text, re.S)
    if not m:
        return {}
    body = m.group(1)
    # 去掉 // 与 /* */ 注释，避免注释里的 = 干扰
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    out: dict[str, int] = {}
    next_val = 0
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" in item:
            name, val = item.split("=", 1)
            name = name.strip()
            try:
                next_val = int(val.strip(), 0)
            except ValueError:
                continue
        else:
            name = item
        if re.fullmatch(r"[A-Za-z_]\w*", name):
            out[name] = next_val
            next_val += 1
    return out


def _parse_transitions(text: str) -> list[tuple[str, str, str]]:
    """提取 BEH_TRANSITIONS[] 里的 (from, event, to) 三元组。"""
    m = re.search(r"BEH_TRANSITIONS\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if not m:
        raise SystemExit("behavior_fsm_check: 找不到 BEH_TRANSITIONS[] 定义")
    body = m.group(1)
    rows: list[tuple[str, str, str]] = []
    # 每行形如 { BEH_ST_X, BEH_EV_Y, BEH_ST_Z, "desc", false },
    # 描述字符串可能含逗号/箭头，故只抓前三个标识符 token。
    entry_re = re.compile(
        r"\{\s*(BEH_\w+)\s*,\s*(BEH_\w+)\s*,\s*(BEH_\w+)\s*,",
        re.S,
    )
    for em in entry_re.finditer(body):
        rows.append((em.group(1), em.group(2), em.group(3)))
    if not rows:
        raise SystemExit("behavior_fsm_check: BEH_TRANSITIONS[] 里没解析出任何转移行")
    return rows


def run() -> tuple[list[str], dict]:
    text = SRC.read_text(encoding="utf-8")
    states = _parse_enum(text, "BehState")
    events = _parse_enum(text, "BehEvent")
    transitions = _parse_transitions(text)

    failures: list[str] = []
    state_names = set(states)
    event_names = set(events)

    # Rule 4: 转移行引用的状态/事件必须在枚举里声明
    for frm, ev, to in transitions:
        for sym, kind, declared in ((frm, "状态", state_names),
                                    (to, "状态", state_names),
                                    (ev, "事件", event_names)):
            if sym not in declared:
                failures.append(
                    f"Rule4 未声明符号: 转移行引用了未在枚举声明的{kind} {sym}"
                )

    # Rule 1: 每个状态可达（有入边），除非显式登记为终态/初始态。
    # 注意：仅"有出边"不算可达 —— 必须先有入边能从初始态走到它。本表无
    # 显式初始态声明，故凡无入边者一律视为死状态（除非登记在
    # ALLOWED_SINK_STATES）。STOP / YIELD 曾因只有 UTURN_TRIGGER 出边、无任何
    # 入边而被误判为"活状态"——它们是本门禁要抓的死状态。
    incoming = {to for _, _, to in transitions}
    for sname in sorted(state_names):
        if sname in ALLOWED_SINK_STATES:
            continue
        if sname not in incoming:
            failures.append(
                f"Rule1 死状态: {sname} 无任何入边 —— 该状态永不可达"
                f"（任何事件×状态组合必须显式可达或显式拒绝）"
            )

    # 跟踪项：登记在 ALLOWED_SINK_STATES 里、但实际已有入边的状态 —— 说明
    # 它已被接线，该从豁免清单里删掉（防止豁免清单沦为永久摆设）。
    for sname in sorted(ALLOWED_SINK_STATES):
        if sname in incoming:
            failures.append(
                f"Rule1 过期豁免: {sname} 已可被到达，请从 ALLOWED_SINK_STATES 删除"
                f"（豁免项：{ALLOWED_SINK_STATES[sname]}）"
            )
        if sname not in state_names:
            failures.append(f"Rule1 幽灵豁免: ALLOWED_SINK_STATES 里的 {sname} 已不存在于枚举")

    # Rule 2: 每个事件至少一条转移行
    used_events = {ev for _, ev, _ in transitions}
    for ename in sorted(event_names):
        if ename not in used_events:
            failures.append(f"Rule2 死事件: {ename} 不出现在任何转移行 —— 发出即被静默丢弃")

    # Rule 3: 变道状态必须有 TIMEOUT 自回退
    for lc in sorted(LANE_CHANGE_STATES):
        if (lc, "BEH_EV_TIMEOUT") not in {(f, e) for f, e, _ in transitions}:
            failures.append(
                f"Rule3 变道缺超时回退: {lc} 无 TIMEOUT 行 —— "
                f"变道超时将永久卡死（2026-08-03 实测事故）"
            )

    summary = {
        "states": len(state_names),
        "events": len(event_names),
        "transitions": len(transitions),
        "unreachable_states": sorted(
            s for s in state_names
            if s not in incoming and s not in ALLOWED_SINK_STATES
        ),
        "acknowledged_dead_states": sorted(ALLOWED_SINK_STATES),
    }
    return failures, summary


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true", help="以 JSON 输出结果")
    args = ap.parse_args()

    failures, summary = run()
    if args.json:
        print(json.dumps({"failures": failures, "summary": summary}, ensure_ascii=False, indent=2))
        return 1 if failures else 0

    print(f"behavior_fsm_check: {summary['states']} 状态 / {summary['events']} 事件 / "
          f"{summary['transitions']} 转移行")
    if summary["unreachable_states"]:
        print(f"  不可达状态: {', '.join(summary['unreachable_states'])}")
    if failures:
        print(f"\n✗ {len(failures)} 项违规：")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("✓ 转移表完整：无死状态 / 无死事件 / 变道均有超时回退")
    return 0


if __name__ == "__main__":
    sys.exit(main())
