# Handoff: KAD Prefer offgrid-fix — 2026-10-25

> **Stream**: B — kunautodrive ADAS-cellular **transfer** (NOT cellular ADAS reopen).
> **Knife**: Prefer **KAD offgrid-fix** — thin planning weld on named RCA `lateral_target_offgrid`
> (bidirectional U-turn / return-half · road-ref vs lanelet anti-parallel).
> **Base tip**: `d12e0a64` / `d12e0a6413ff56e431326b6ddfbcb633b3a3ee75` (main · `#113` deepen² Outcome B)
> **Branch**: `feat/kad-adas-offgrid-fix`
> **Owner**: 焊枪 (WSL caixuf) · Gate: 找茬喵 · Merge: 扳手
> **Outcome**: **offgrid gate CLEARED** on `straight_road` (pack×6 all PASS) — fresh rebuild of
> existing `a3947f0c` weld + Prefer citation + unit covering anti-parallel / A11 grid honesty.
> **Cellular ADAS status**: **PARK** — no kun-cellular edits · no P0/P1/champ · no DouZero/GPU/PSRO/4th ABI
> **Language lock**: ceiling/PARK · ready-for-later-compare · **never** promote-as-win / beat-Stanley

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

---

## Weld (thin)

### Prior code (already on tip `d12e0a64` via `a3947f0c`)

| Surface | Content |
|---|---|
| `planning_coordinates.h::lane_ref_aligned` | Geometric heading co-direct check (default 45°) |
| `planning_node.cpp` cruise branch `ref_frame_ok` | On anti-parallel / no ref → A11 lane-center grid fallback (no third path) |
| Gate | `demo_evaluator.lateral_target_offgrid` (Rule6) — **untouched** (no raise / null disable) |

### This Prefer delta

| Path | Role |
|---|---|
| `planning_node.cpp` cruise comment | Cite Prefer offgrid-fix / deepen² `#113` Outcome B deferred weld |
| `test_planning_coordinates.cpp` | Unit: anti-parallel reject + A11 grid stays on-phase (U-turn / return-half offgrid evidence) |
| `artifacts/kad_product_suite_cte.json` | Fresh pack×6 @ tip `d12e0a64…` after **rebuild** of `libplanning_node.so` |
| `docs/handoff/2026-10-25-kad-offgrid-fix.md` | This archive |

### Why rebuild mattered (honesty)

Deepen² remeasure @ `6992ecf5` used harness default `FLOW_SKIP_BUILD=1` against
`build/lib/libplanning_node.so` mtime **2026-10-23 19:22** — **older than** weld commit
`a3947f0c` (**2026-10-23 23:26**). Stale `.so` reproduced the pre-weld 16/181 offgrid FAIL.
This Prefer rebuilt `planning_node` (+ unit) then remeasured; offgrid **clears**.

No gate disable · no `LATERAL_GRID_*` raise · no U-turn drop · no plant rewrite · no control retune.

---

## Remeasure @ tip `d12e0a64` (full pack×6)

### Cmd

```bash
# Host: WSL caixuf · repo /home/caixuf/code/kunautodrive · tip d12e0a64
# Rebuild planning_node (nested Makefile) then:
FLOW_SKIP_BUILD=1 python3 tools/bench_kad_product_suite_cte.py \
    --out artifacts/kad_product_suite_cte.json
```

### Fresh JSON

| Field | Value |
|---|---|
| Path | `artifacts/kad_product_suite_cte.json` |
| schema | `kad.product_suite_cte.v1` |
| status | `ok` |
| **tip_hash** | **`d12e0a6413ff56e431326b6ddfbcb633b3a3ee75`** (starts `d12e0a64…`) |
| pack_id | `product_suite_pack_v1` |
| frozen_scene_count | 6 |
| numbers_claimed | true |
| ready_for_later_compare | true |

### Per-scene CTE (meters) — tip `d12e0a64`

| pack_scene_id | cte_mean | cte_p95 | cte_max | n_samples | evaluator_result |
|---|---:|---:|---:|---:|:---:|
| `straight_road` | 0.500505 | 1.49922 | 1.739365 | 181 | **PASS** |
| `curve_road` | 0.024186 | 0.048062 | 0.078816 | 241 | PASS |
| `lane_change_traffic` | 0.318007 | 1.34031 | 1.736431 | 181 | PASS |
| `r157_cut_in` | 0.277277 | 1.308134 | 1.745286 | 161 | PASS |
| `oncoming` | 0.0 | 0.0 | 0.0 | 181 | PASS |
| `dense_npc` | 0.447043 | 1.48977 | 1.740753 | 181 | PASS |

### `straight_road` offgrid movement

| | deepen² @ stale `.so` (`6992ecf5`) | this Prefer @ rebuilt `.so` (`d12e0a64`) |
|---|---|---|
| evaluator_result | **FAIL** | **PASS** |
| Primary FAIL | `lateral_target_offgrid` 16/181 ≈ 9% > 5%; phase 1.75 m | **cleared** (gate moves → ok) |
| Secondary | comfort jerk max 15.9 FAIL | comfort jerk max ~13.2 **WARN only** (not raised) |

Residual: none on offgrid. Comfort secondary no longer FAIL; left as WARN envelope (no threshold raise).

---

## Untouched locks

| Item | Status |
|---|---|
| Pack `product_suite_pack_v1` ×6 IDs | **freeze held** |
| Plant cold-pack ×6 + `bench_kad_baseline_cte.py` | **UNTOUCHED** |
| Champ ADAS md5 `7424dd…` | **untouched** |
| kun-cellular / P0 / P1 / invent search / further openness deepen | **PARK** |
| Evaluator `lateral_target_offgrid` tol / ratio | **untouched** (no greenwash) |
| Control / product gain retune | **not done** |

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
- [x] Named RCA = anti-parallel normals; weld = ref_frame_ok + A11 (no gate greenwash)
- [x] tip_hash = measure tip `d12e0a64…`; plant freeze held
- [x] Offgrid gate **moved/cleared** honestly (rebuild + unit); no invent green

---

## Acceptance map (F0–F7)

| ID | Check | Result |
|---|---|---|
| F0 | Branch off `d12e0a64`; WSL caixuf + ccp | PASS |
| F1 | Dual-ceiling `#3` 9W/7L + `#15` 7W/9L cited; no fake win | PASS |
| F2 | RCA cause addressed (anti-parallel / offgrid weld) | PASS (`a3947f0c` + Prefer cite + unit) |
| F3 | Remeasure: `straight_road` offgrid **clears**; tip_hash `d12e0a64…` | PASS |
| F4 | Pack v1 freeze held; plant cold-pack untouched; no silent drop / gate greenwash | PASS |
| F5 | Cellular ADAS PARK | PASS |
| F6 | No invent / further openness deepen / DouZero/GPU/PSRO / 4th ABI | PASS |
| F7 | Owner 焊枪; gate 找茬喵 | READY |

---

## Next

1. Keep cellular ADAS **PARK**.
2. Gate (找茬喵): weld thinness + tip_hash + pack/plant untouched + dual-ceiling honesty + offgrid cleared.
3. Do **not** treat pack CTE as cellular scoreboard or ≥12W invent.
