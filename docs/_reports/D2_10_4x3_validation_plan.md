# D2-10 — 4 scenarios × 3 repeats validation plan (scaffold)

**Status:** DRAFT / scaffolding only — **not** DONE. Do **not** claim 12/12 PASS until:
1. PR #104 (D2-06 Prefer consume @ `173d917`) is on main, and
2. GT 4×3 is green with `max_lane_offset_m` enforced, and
3. 扳手 lifts the narrow (control retune may follow).

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

