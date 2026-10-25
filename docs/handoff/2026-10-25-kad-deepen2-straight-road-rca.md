# Handoff: KAD Prefer deepen² — straight_road_fail_rca_v1 — 2026-10-25

> **Stream**: B — kunautodrive ADAS-cellular **transfer** (NOT cellular ADAS reopen).
> **Knife**: Prefer **KAD deepen²** — close named gap `straight_road_fail_rca_v1` (RCA + thin honesty **or** deliver-or-reason).
> **Base tip**: `6992ecf5` / `6992ecf51b13b433861c475f0dfc283f75ef4522` (main · `#112` deepen suite-pack)
> **Branch**: `feat/kad-adas-deepen2-straight-road-rca`
> **Owner**: 焊枪 (WSL caixuf) · Gate: 找茬喵 · Merge: 扳手
> **Outcome**: **B — deliver-or-reason** (FAIL kept honest; no thin honesty fix without inventing green)
> **Cellular ADAS status**: **PARK** — no kun-cellular edits · no P0/P1/champ · no DouZero/GPU/PSRO/4th ABI
> **Language lock**: ceiling/PARK · ready-for-later-compare · **never** promote-as-win / beat-Stanley / paste cellular W/L as product

---

## Dual-ceiling cite lock (honest labels — never "win")

| Ceiling | Cellular PR / tip | W/L | Label |
|---|---|---|---|
| Lock / prior W1 | `#3` @ `2922599a` | **9W/7L** | **ceiling evidence, NOT a win** |
| P0 reopen A6 | `#15` @ `50431c48` | **7W/9L** best | **A6 CEILING · PARK · STOP+ask**; champ md5 `7424dd1115b60c675aded7a59efae30a` held |

### Gate language (locked)

- Gate **≥12W/4L** never green on cellular. Soft **≥10/16 ≠ closeout**.
- Cellular cortex vs Stanley is **ceiling / PARK**, **not** a product win to import as green.
- Do **not** invent ≥12W from ceilings. Do **not** claim "cellular beat Stanley".
- Pack CTE digest is **ready for later compare** only — **not** a ship win / promote-as-win.
- This Prefer closes the **named gap** with an honest dossier; it does **not** claim FAIL→PASS.

---

## Named gap `straight_road_fail_rca_v1`

### Symptom (#112 archive @ tip_hash `9b62cd01…`)

| Field | Value |
|---|---|
| pack_scene_id | `straight_road` |
| cte_mean / cte_p95 / cte_max | ≈0.510 / ≈1.439 / ≈1.674 m |
| n_samples | 181 |
| evaluator_result | **FAIL** |
| Archived failure (exact) | `lateral target off-grid: planning target_lane_offset 取到非车道中心值 16/181 帧 (9% > 5%)，最大相位偏离 1.61 m（网格相位 1.75 m）—— 横向目标不是合法车道中心（根因层：参考系法向不一致）` |

Loose prior framing (`cte_max≈1.674` vs suite `max_lane_offset_m=1.65`) is **correlative symptom language only** — it is **not** the FAIL gate (see RCA).

### Named RCA (root cause)

| Dimension | Finding |
|---|---|
| **Class** | **Envelope / metric gate** (evaluator regression envelope) — **not** harness harvest bug, **not** pack-manifest drop, **not** plant cold-pack |
| **Gate that FAILs** | `demo_evaluator.lateral_target_offgrid` (`LATERAL_GRID_TOL_M=0.5`, `LATERAL_GRID_MAX_RATIO=0.05`) |
| **Mechanism** | Planning cruise writes `target = ego_d − llt_offset`, which holds only when road-ref normal is co-directed with lanelet normal. On bidirectional synthetic `straight_road` the intentional end U-turn / return half makes normals **anti-parallel**; target sweeps off the lane-center grid (phase 1.75 m) → off-grid frames. |
| **Scenario context** | `scenarios/straight_road.json` = 3000 m bidirectional 4-lane with physical U-turns at ends + mid S-curve; ego starts `heading=π`. U-turn is **intentional** scene design, not harness mis-wiring. |
| **Not the FAIL gate** | D2-10 `max_lane_offset_m=1.65` — **WARN-only** unless `enforce_max_lane_offset` / `--enforce-max-lane-offset`. Suite sets `max_lane_offset_m: 1.65` on `straight_road` but does **not** enforce. Pack peers with `cte_max > 1.65` still **PASS** (`lane_change_traffic` / `r157_cut_in` / `dense_npc`). |
| **Evidence** | `ci/evaluators/demo_evaluator.py` comments (2026-10-23 U-turn / return-half anti-parallel normals) + archived `evaluator_failures` string match the off-grid gate verbatim. |

### Thin-fix audit (allowed = harness / scenario-config / envelope **honesty** only)

| Candidate | Verdict | Why |
|---|---|---|
| Disable gate via `pass_criteria.lateral_grid_tolerance_m: null` | **FORBIDDEN** | Scene uses synthetic `road_network` (grid-aligned). Null-disable is for true `map_file` lanelet intentional off-grid. Would be **threshold greenwash**. |
| Raise `LATERAL_GRID_MAX_RATIO` / tol | **FORBIDDEN** | Silent raise-threshold greenwash. |
| Retune planning cruise / control for anti-parallel normals | **FORBIDDEN** | Control / product retune · plant-adjacent. Out of knife. |
| Drop / rewrite U-turn out of `straight_road` | **FORBIDDEN** | Silent scene drop / scenario rewrite. Pack freeze held. |
| Claim FAIL cleared because `cte_max` WARN-only | **FORBIDDEN** | Fake win; misattributes gate. |
| Harness harvest / state_file isolation bug | **N/A** | CTE series harvested (n=181); FAIL is evaluator envelope on real samples — not a 0-sample fake-block. |

**Conclusion**: no thin honesty fix available without inventing green → **Outcome B**.

---

## Remeasure @ tip `6992ecf5` (full pack×6)

### Cmd

```bash
# Host: WSL caixuf · repo /home/caixuf/code/kunautodrive · tip 6992ecf5
git fetch origin
git checkout -B feat/kad-adas-deepen2-straight-road-rca 6992ecf51b13b433861c475f0dfc283f75ef4522
FLOW_SKIP_BUILD=1 python3 tools/bench_kad_product_suite_cte.py --out artifacts/kad_product_suite_cte.json
```

### Fresh JSON

| Field | Value |
|---|---|
| Path | `artifacts/kad_product_suite_cte.json` |
| schema | `kad.product_suite_cte.v1` |
| status | `ok` |
| **tip_hash** | **`6992ecf51b13b433861c475f0dfc283f75ef4522`** (starts `6992ecf5…`; prior was `9b62cd01…`) |
| pack_id | `product_suite_pack_v1` |
| frozen_scene_count | 6 |
| numbers_claimed | true |
| ready_for_later_compare | true |
| wall (host) | ≈343 s |

### Per-scene CTE (meters) — tip `6992ecf5`

| pack_scene_id | cte_mean | cte_p95 | cte_max | n_samples | evaluator_result |
|---|---:|---:|---:|---:|:---:|
| `straight_road` | 0.507647 | 1.484676 | 1.74566 | 181 | **FAIL** |
| `curve_road` | 0.023884 | 0.051378 | 0.074169 | 240 | PASS |
| `lane_change_traffic` | 0.3241 | 1.390631 | 1.731957 | 180 | PASS |
| `r157_cut_in` | 0.268258 | 1.237494 | 1.702309 | 161 | PASS |
| `oncoming` | 0.0 | 0.0 | 0.0 | 181 | PASS |
| `dense_npc` | 0.447387 | 1.473373 | 1.721292 | 181 | PASS |

### `straight_road` evaluator_failures (fresh)

1. **Primary (same class as #112)**: `lateral target off-grid: planning target_lane_offset 取到非车道中心值 16/181 帧 (9% > 5%)，最大相位偏离 1.66 m（网格相位 1.75 m）—— 横向目标不是合法车道中心（根因层：参考系法向不一致）`
2. **Secondary (this remeasure)**: `comfort: jerk max 15.9 m/s³ exceeds 15.0 (ISO 2631 comfort band ~10; accelerometer-grade ride)` — additional envelope hit; **not** used as a greenwash pretext and **not** threshold-raised.

`straight_road` evaluator FAIL is **kept honest** (not retuned / not dropped / not threshold-raised). CTE series still harvested for **later compare only**.

---

## deliver-or-reason dossier (Outcome B)

| Field | Content |
|---|---|
| **cause** | Envelope gate `lateral_target_offgrid` on intentional U-turn / return-half of bidirectional synthetic `straight_road`: planning cruise target formula assumes co-directed normals; anti-parallel normals put `target_lane_offset` off lane-center grid (16/181 ≈ 9% > 5%). |
| **blocked** | Thin honesty path exhausted: disabling/raising the grid gate = greenwash; planning/control fix = retune (banned); scene U-turn removal = drop/rewrite (banned). `max_lane_offset_m=1.65` is WARN-only here and is **not** the FAIL. |
| **needed** | Deferred planning-side honesty for anti-parallel ref normals on bidirectional synthetic roads (or a later Prefer scoped by 北极星). **STOP** widen this knife. Ask 扳手 → 北极星 before any retune / gate change / extra-scenario Prefer. |
| **FAIL kept** | Yes — Prefer still closes via honest dossier. |

---

## Untouched locks

| Item | Status |
|---|---|
| Pack `product_suite_pack_v1` ×6 IDs | **freeze held** (no silent drop) |
| Plant cold-pack ×6 + `bench_kad_baseline_cte.py` | **UNTOUCHED** |
| Champ ADAS md5 `7424dd…` | **untouched** |
| kun-cellular / P0 / P1 / invent search / further openness deepen | **PARK** |
| Control / product gain retune | **not done** |
| Extra fixed scenario Prefer / bus-native `ControlRaw.cte` Prefer | **deferred** (not this knife) |

---

## Claim-language self-check (author)

- [x] No "cellular beat Stanley"
- [x] No inventing ≥12W from ceilings
- [x] Dual-ceiling rows labeled ceiling / PARK / NOT a win (`#3` 9W/7L · `#15` 7W/9L)
- [x] Soft ≥10/16 ≠ closeout stated
- [x] No cellular W/L paste as product scoreboard
- [x] No kun-cellular ADAS / champ / P0 / P1 edits
- [x] No DouZero / GPU / PSRO / 4th ABI
- [x] Ready-for-later-compare stated; pack digest ≠ ship win
- [x] Named RCA = off-grid envelope (not max_lane_offset greenwash story)
- [x] No silent raise-threshold / drop scene / control retune
- [x] tip_hash = measure tip `6992ecf5…`; plant freeze held
- [x] Outcome B deliver-or-reason; FAIL kept honest

---

## Acceptance map (D20–D27)

| ID | Check | Result |
|---|---|---|
| D20 | Branch off `6992ecf5`; WSL caixuf + ccp | PASS |
| D21 | Dual-ceiling `#3` 9W/7L + `#15` 7W/9L cited; no fake win | PASS |
| D22 | Named gap closed **or** deliver-or-reason with named cause | PASS (Outcome B) |
| D23 | Artifact + numbers on WSL; tip_hash = `6992ecf5…` | PASS |
| D24 | Pack v1 freeze held; plant cold-pack untouched; no silent drop | PASS |
| D25 | Cellular ADAS PARK | PASS |
| D26 | No invent / further openness deepen / DouZero/GPU/PSRO / 4th ABI | PASS |
| D27 | Owner 焊枪; gate 找茬喵 | READY |

---

## Next

1. Keep cellular ADAS **PARK**.
2. Gate (找茬喵): RCA honesty + deliver-or-reason + tip_hash + pack/plant untouched + no fake win.
3. Do **not** treat pack CTE as cellular scoreboard or ≥12W invent.
4. Any planning U-turn / anti-parallel-normal fix or gate Prefer → 扳手 → 北极星 only (out of this knife).
