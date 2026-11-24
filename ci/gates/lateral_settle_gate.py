#!/usr/bin/env python3
"""lateral_settle_gate — 出弯横摆门禁（40Hz 全速率离线闭环）。

背景：demo_evaluator 的晃动判据读 /tmp/flow_topology.json 只有 5Hz（Nyquist
2.5Hz），1~2Hz 横向极限环 / 出弯过冲被混叠，抓不到。本门禁用 tools/control_sim.py
的 40Hz 确定性闭环（与 control_node.cpp 横向律逐项对齐）测「转弯后横向摆动」：
直道→R=1/κ 弯→直道，断言出弯后横向误差峰值、巡航纹波、转向翻号率。

判据（tools/control_sim.py 单一事实源常量）：
  CURVE_EXIT_OVERSHOOT_FAIL / CURVE_EXIT_YPP_FAIL / CURVE_EXIT_FLIPS_FAIL

历史：横向摇晃被"修"6 次不收敛，根因之一是门禁没牙 + 欠采样（见 memory
lateral-oscillation-gate-blind）。本脚本必须在现有代码上能跑红，才有资格当门禁。

用法：
  python3 ci/gates/lateral_settle_gate.py            # 退出码 0=PASS 1=FAIL
  python3 ci/gates/lateral_settle_gate.py -v         # 打印每工况详细
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
sys.path.insert(0, os.path.join(_ROOT, "tools"))

import control_sim as cs  # noqa: E402

# 覆盖 demo 实际工况：城市弯 R≈48@12、匝道 R50@14（a_lat≈3.9 逼近 4.0 权限）。
# 再补一个高速宽弯 R75@15，覆盖"高速出弯"。
CASES = [
    dict(kappa=1.0 / 48.0, target_speed=12.0, label="city R48@12"),
    dict(kappa=1.0 / 50.0, target_speed=14.0, label="ramp R50@14"),
    dict(kappa=1.0 / 75.0, target_speed=15.0, label="hiway R75@15"),
]


def main(argv):
    verbose = "-v" in argv or "--verbose" in argv
    failed = []
    print("lateral_settle_gate — 出弯横摆（40Hz 离线闭环）")
    print(f"  阈值: overshoot≤{cs.CURVE_EXIT_OVERSHOOT_FAIL}m, "
          f"y_pp≤{cs.CURVE_EXIT_YPP_FAIL}m, flips≤{cs.CURVE_EXIT_FLIPS_FAIL}/s")
    for c in CASES:
        kw = dict(c)
        label = kw.pop("label")
        ok, m, fails = cs.evaluate_curve_exit(**kw)
        status = "PASS" if ok else "FAIL"
        print(f"  [{status}] {label}: overshoot={m['overshoot_m']:.3f}m "
              f"y_pp={m['y_pp_m']:.3f}m flips={m['steer_flips_per_s']:.2f}/s "
              f"off={m['off_road']}")
        if verbose and fails:
            for f in fails:
                print(f"         - {f}")
        if not ok:
            failed.append((label, fails))

    print("")
    if failed:
        print(f"  => FAIL ({len(failed)}/{len(CASES)} 工况超标)")
        for label, fails in failed:
            print(f"     • {label}: " + "; ".join(fails))
        return 1
    print(f"  => PASS (全部 {len(CASES)} 工况)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
