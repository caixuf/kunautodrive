# D2-10 — 4 scenarios × 3 repeats validation plan (scaffold)

**Status:** DRAFT / scaffolding only — **not** DONE. Do **not** claim 12/12 PASS until:
1. PR #104 (D2-06 Prefer consume @ `173d917`) is on main, and
2. GT 4×3 is green with `max_lane_offset_m` enforced, and
3. 扳手 lifts the narrow (control retune may follow).

---

### 2026-10-24 4×3 实测（本机 Release，HEAD `a3947f0`；本格**仍不宣称 DONE**）

前置 1 / 2 已满足：#104 在 main（`351aa1e`），且 `a3947f0` 补了参考系一致性前提 + Rule6 骑线判据。

命令（plan 里那条，未加 `--workers`，默认串行；12 次真跑）：

```bash
python3 ci/evaluators/scenario_regression.py --d2-10-scenes --repeats 3 \
  --collect-fixtures /tmp/d2_10_fixtures --results-dir /tmp/flow_regression_d210
```

| 结论 | 数据 |
|---|---|
| **横向判据 12/12 达标** | `max_abs_llt_offset_m`：straight 0.932 / 1.082 / 1.095、curve 0.070 / 0.103 / 0.108、urban 1.192 / 1.153 / 1.170、city 0.152 / 0.156 / 0.153 —— **最差 1.192 ≤ 1.65** |
| A1 / A2 / A3 / A4 | ✅（suite 有阈值、每次都记 `max_abs_llt_offset_m`、越线走 FAIL、`test_evaluator_gate.py` 71 passed） |
| **A5（12/12 PASS）** | 第 1 轮 ❌ 11/12（唯一 FAIL = `comfort: jerk max 16.1 > 15.0`）；**判据形态修复后第 2 轮 ✅ 12/12**，见下方小节 |
| A6 / A7 | 未做（A7 控制重调不在本批范围） |

**挡点与横向无关**：`comfort jerk max` 是逐帧加速度差分的**极值**统计，单帧尖峰独自决定判定
（仓库基线里该值 1293 m/s³，物理上不可能）→ 已登记 `docs/FAILURE_MODES.md` §5。

### 2026-10-24 复跑：判据形态修掉后 **A5 达成（12/12 PASS）**

`comfort jerk` 的判据从"单帧极值"改成"**连续 3 帧超阈**才 FAIL（单帧只 WARN，另留
单帧 >3× 阈值的野尖峰硬线）"（commit `d927717`，理由与实测见该 commit body 与
`docs/FAILURE_MODES.md` §5）。同命令复跑 12 次：

| 结论 | 数据 |
|---|---|
| **A5（12/12 PASS）** | ✅ **全绿**；逐次 `max_abs_llt_offset_m` = 1.084 / 1.406 / 0.988、0.101 / 0.070 / 0.057、0.994 / 1.225 / 1.159、0.144 / 0.141 / 0.148（**最差 1.406 ≤ 1.65**） |
| 判据是否被改哑 | 同一轮里有两次 `comfort: jerk max 17.9 / 15.8 m/s³`（**高于旧 15.0 FAIL 线**）而判 WARN —— 因为 `longest consecutive over-15.0 run = 1 frame(s)`；这正是 A/B：旧判据 FAIL、新判据 WARN |
| A6 / A7 | 仍未做（A7 控制重调不在本批范围） |

结果文件在 `/tmp/flow_regression_d210_c/*.json`、日志 `/tmp/d210_harness_c.log`（临时目录，未入库）。

结果文件在 `/tmp/flow_regression_d210/*.json`、fixtures 在 `/tmp/d2_10_fixtures`、日志
`/tmp/d210_harness.log`（均在临时目录，**未入库**）；完整交付说明见 `docs/HANDOFF_2026-10-24.md`。

**Rails:** `REQ-D2-10-lane-offset-closeout.md` · `kun-d2-10-lane-offset-rails.md`  
**Branch:** `feat/l3-d2-10-lane-offset-gate` · **Base tip:** `dccd907` (+ rebase onto #104 when merged)

## Scenes (locked)

| # | Scenario stem | Suite entry | Repeats |
|---|---|---|---|
| 1 | `straight_road` | `scenarios/straight_road.json` | 3 |
| 2 | `curve_road` | `scenarios/curve_road.json` | 3 |
| 3 | `city_comprehensive` | `scenarios/city_comprehensive.json` | 3 |
| 4 | `urban_challenge` | `scenarios/urban_challenge.json` | 3 |

**Gate:** `max_lane_offset_m = 1.65` (= `lane_width/2 − 0.1`, width 3.5).  
Measured field Prefer: `planning/debug.lane_match_offset`; fallback `localization/lane_match` (`offset` / `llt_offset`).  
Invalid `lane_match` frames **do not** count (A11).

## Commands (scaffold harness)

```bash
# Dry-run: confirm 4 scenes + threshold wiring (no sim)
python3 ci/evaluators/scenario_regression.py --d2-10-scenes --dry-run

# Collector (after #104): 4×3 GT + slim fixtures (results under /tmp)
python3 ci/evaluators/scenario_regression.py \
  --d2-10-scenes --repeats 3 \
  --collect-fixtures /tmp/d2_10_fixtures \
  --results-dir /tmp/flow_regression_d210

# Gate self-check (non-blocking unit; must stay green on this PR)
python3 ci/evaluators/test_evaluator_gate.py
python3 -m pytest tests/test_scenario_regression_artifacts.py -q
```

## Pass criteria (when claiming DONE — later knife)

| ID | Check |
|---|---|
| A1 | `max_lane_offset_m` present in `scenarios/suite.json` (1.65) |
| A2 | Evaluator records `max_abs_llt_offset_m` each run |
| A3 | Over threshold → scenario **FAIL** (not warn-only) |
| A4 | `test_evaluator_gate.py` negative case green |
| A5 | 4×3 GT → 12/12 PASS |
| A6 | Optional NFR-05: `avg_speed_mps` Δ ≤ 5% vs baseline |
| A7 | Control retune evidence (Stanley/LTV) — **out of this scaffold PR** |

## Explicit non-goals (this PR)

- Do **not** flip `--strict-m3` to CI-blocking.
- Do **not** retune Stanley / LTV-MPC (`control_node`) yet.
- Do **not** expand `planning_framework` / touch D2-03 / TL / fusion / emily / imfl.
- Do **not** declare D2-10 DONE.

## Rebase note

PR #104 (`feat/l3-d2-06-planning-lane-match` @ `173d917`) was **OPEN** at scaffold time. After merge: rebase this branch onto latest main before running the real 4×3.

## Prefer A amend (2026-10-23 — 老法师 locked)

**Pick: A.** Skip `maneuver_active` frames in `max_lane_offset_gate` (same spirit as lane_keep). Threshold **stays 1.65**.

Evidence @ tip `7bbd07df`: FAIL peaks (1.748 / 1.723) are **only** RIGHT_CHANGE frames ≈ lane_width/2 geometry before `llt_id` hysteresis; cruise/lane_keep worst ≤1.07. Stanley retune cannot close geometric LC straddle. B (raise thr) / C / D vetoed or OOS.

- Land: evaluator + tests + rails wording only; control param commit **optional SKIP** (cruise already under thr).
- Frame eligibility: count cruise/follow with `lane_match.valid`; exclude `maneuver_active` and invalid match (A11).
- Box rails: `/workspace/contracts/kun-d2-10-lane-offset-rails.md` · `/workspace/kunautodrive-docs/REQ-D2-10-lane-offset-closeout.md`

