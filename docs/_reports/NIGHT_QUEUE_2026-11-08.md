# 夜间队列 2026-11-08

**分支：** `night/2026-11-08`（基于 main `80b8ef1`）
**规则：** 每项必须 (a) 验证命令绿，(b) 独立 commit，(c) 状态与证据写回本文件。
禁止放宽门禁阈值 / 改断言骗绿 / 只写文档不写代码 / push 远端。
遇 blocker：记录现象 + 已排除假设 + 复现命令，跳到下一项。

## 队列

- [ ] **T1 Flake 猎杀** — 场景矩阵连跑 8 次，定位并修 top flake
  - 验证：8 次 `scenario_regression.py --workers 4` 全绿
  - 预期：8/8 全绿；每次失败场景/根因记入下方证据
  - 状态：TODO

- [x] **T2 BEV 影子精度门禁** — monitor 订阅 `bev/obstacles`，评估器比对真值目标数
  - 验证：新 gate `ci/gates/bev_shadow_check.py`；A/B（故意丢帧必须红）
  - 预期：passthrough 模式下 BEV 目标数与真值差 ≤2；默认 18 场景不变
  - 状态：**DONE**（gate 正常 3/3、注入 0 被拒；`straight_road` 复跑 PASS，
    `bev_shadow_count=2/truth=2`）

- [ ] **T6 长稳** — `long_run_lane_keep --duration 600` ×3 + 10min soak
  - 验证：3/3 PASS，0 压线 / 0 车心越线 / 0 invariant
  - 预期：三次全绿
  - 状态：TODO

## 证据

### T2
- `python3 ci/gates/bev_shadow_check.py` → `✓ BEV shadow OK (normal count=3/truth=3; injected count=0 correctly rejected)`
- `scenario_regression --only straight_road` → PASS；summary `bev_obstacles_hz=9.977,
  bev_shadow_count=2, bev_truth_count=2`
- 覆盖范围按 BEV 栅格（前/后 60m、横向 ±20m）过滤真值；全场景目标数不再误判

## Morning handoff

- 队列状态：T1 TODO / T2 TODO / T6 TODO
- 需要在环决策的事项：无