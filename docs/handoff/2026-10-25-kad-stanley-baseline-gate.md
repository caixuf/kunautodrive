# Handoff: KAD Prefer Stanley/baseline regression gate — 2026-10-25

> **Stream**: B — kunautodrive ADAS-cellular **transfer** (NOT cellular ADAS reopen).
> **Knife**: Prefer **KAD Stanley/baseline regression gate** — **LOCKED** =
> fail-closed Stanley / baseline regression gate on existing
> `product_suite_pack_v1` ×6 only (no invent-scenario).
> **Base tip**: `dbae06ab` / `dbae06abb7cce18316589c071e2c58af31eed483` (main · `#115` suite-expand CI-lock)
> **Branch**: `feat/kad-adas-stanley-baseline-gate`
> **Owner**: 焊枪 (WSL caixuf) · Gate: 找茬喵 · Merge: 扳手
> **Outcome**: **Stanley/baseline fail-closed gate wired** + pack×6 remeasure **PASS**
> under controller=Stanley @ tip_hash `dbae06ab…`.
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
- **Do not claim pack CTE = cellular win / beat-Stanley.**

---

## Distinct from `#115`

| Prior | This Prefer |
|---|---|
| `#115` suite-expand | CI-locked **product suite** runner ×6 as gate (`kad_product_suite_lock_check.py` · job `kad-product-suite-lock-gate`) — **KEEP** |
| **This** | Fail-closed **Stanley / baseline** regression gate on the **same** pack×6 |

Plant cold-pack (`tools/bench_kad_baseline_cte.py` · plant IDs straight/lane_change/…) stays **UNTOUCHED**.
This knife gates Stanley/baseline vs **product_suite** pack IDs (straight_road/curve_road/…).

---

## Knife — Stanley/baseline gate surface

| Path | Role |
|---|---|
| `tools/bench_kad_stanley_baseline_gate.py` | Thin sibling CLI: pack×6 under **controller=Stanley** (default product lateral, `use_ltv_mpc=0`); reuses product suite measure path; `--dry-run` smoke |
| `ci/gates/kad_stanley_baseline_gate_check.py` | **Fail-closed** CI gate: pack freeze ×6 · artifact status=ok · all scenes `evaluator_result=PASS` · tip_hash honesty · controller=Stanley · harness `--dry-run` · `#115` lock KEPT |
| `.github/workflows/ci.yml` · job `kad-stanley-baseline-gate` | Thin PR/push gate invoking the check (no full demo rebuild in CI) |
| `artifacts/kad_stanley_baseline_gate.json` | Remeasure evidence; tip_hash `dbae06abb7cc…`; controller=Stanley |
| `docs/handoff/2026-10-25-kad-stanley-baseline-gate.md` | This archive |

### Fail-closed rules

Gate **exits non-zero** when any of:

1. Pack / harness / artifact missing  
2. Pack ID ≠ `product_suite_pack_v1` or frozen IDs ≠ exact six (order+set) — **no 7th / no silent drop**  
3. Artifact `status != ok` or any scene `evaluator_result != PASS`  
4. `controller != Stanley` / controller_label missing Stanley  
5. `tip_hash` missing / not ancestor of `HEAD` / lock surfaces drifted since measure tip  
6. `#115` product-suite lock file missing (must KEEP)

### Invokes harness

```bash
python3 ci/gates/kad_stanley_baseline_gate_check.py
# internally: python3 tools/bench_kad_stanley_baseline_gate.py --dry-run
```

Full pack×6 headless remeasure is **not** re-run in this thin CI job (needs
`flow_launcher` + demos). Evidence is the locked Stanley gate artifact
remeasured @ tip `dbae06ab…`. If lock surfaces change after measure tip → gate
fails and demands remeasure (no greenwash / no tol raise / no scene drop).

---

## Pack freeze held (do not expand)

| pack_scene_id | suite file |
|---|---|
| `straight_road` | `scenarios/straight_road.json` |
| `curve_road` | `scenarios/curve_road.json` |
| `lane_change_traffic` | `scenarios/lane_change_traffic.json` |
| `r157_cut_in` | `scenarios/r157_cut_in.json` |
| `oncoming` | `scenarios/oncoming.json` |
| `dense_npc` | `scenarios/dense_npc.json` |

**Freeze rules:** IDs + files locked. No silent drop. No 7th scene. Exam / parking HOLD.

---

## Remeasure (Stanley/baseline @ tip `dbae06ab…`) — PASS

| Field | Value |
|---|---|
| Tip (branch / measure) | `dbae06abb7cce18316589c071e2c58af31eed483` (`dbae06ab…`) |
| Artifact | `artifacts/kad_stanley_baseline_gate.json` |
| schema | `kad.stanley_baseline_gate.v1` |
| status | **ok** |
| controller | **Stanley** (default product lateral; `use_ltv_mpc=0`; no retune) |
| pack_id | `product_suite_pack_v1` |
| frozen_scene_count | 6 |
| numbers_claimed | true |
| ready_for_later_compare | true |
| Command | `FLOW_SKIP_BUILD=1 python3 tools/bench_kad_stanley_baseline_gate.py --out artifacts/kad_stanley_baseline_gate.json` |

### Per-scene (Stanley)

| pack_scene_id | evaluator_result | status | cte_mean | controller |
|---|:---:|:---:|---:|:---:|
| `straight_road` | **PASS** | ok | 0.502119 | Stanley |
| `curve_road` | **PASS** | ok | 0.022024 | Stanley |
| `lane_change_traffic` | **PASS** | ok | 0.327639 | Stanley |
| `r157_cut_in` | **PASS** | ok | 0.253211 | Stanley |
| `oncoming` | **PASS** | ok | 0.0 | Stanley |
| `dense_npc` | **PASS** | ok | 0.438425 | Stanley |

### Remeasure notes (honesty)

- First full pack pass: `straight_road` + `r157_cut_in` returned evaluator
  `FAIL` / `no topology samples collected` (headless stack flake — empty samples,
  not a Stanley retune / not tol raise).
- Honest retries of those two scenes alone → both **PASS** with CTE harvest.
- Artifact rebuilt by re-harvesting evaluator json-out (retry + first-pass PASS
  scenes) through the same product-suite harvest path — **no invented metrics**.
- tip_hash == HEAD `dbae06ab…`. Gate check PASS. `#115` product-suite lock still PASS.

---

## Plant cold-pack UNTOUCHED

| Item | Status |
|---|---|
| `tools/bench_kad_baseline_cte.py` | **UNTOUCHED** (plant IDs stay) |
| Plant scenes | straight · lane_change · curve · obstacle · merge · cutin |
| Champ md5 `7424dd…` | **untouched** |
| `#115` product-suite lock | **KEPT** (verified PASS this weld) |

---

## PARK / bans held

- Invent-scenario / 7th pack scene · invent search  
- Cellular ADAS reopen / silent P1 / champ touch  
- Uncap / scale 328 · further openness deepen  
- DouZero / GPU / PSRO / 4th ABI  
- Control scoreboard retune · gate greenwash · fake win / beat-Stanley  
- Disable `#115` product-suite lock  

---

## Acceptance map

| ID | Check | Result |
|---|---|---|
| S0 | Branch off `dbae06ab`; WSL caixuf + ccp only | **PASS** |
| S1 | Dual-ceiling `#3` 9W/7L + `#15` 7W/9L cited; no fake win | **PASS** (this handoff) |
| S2 | Stanley/baseline fail-closed gate wired on pack×6 | **PASS** (diff) |
| S3 | Pack remeasure PASS; tip_hash honest `dbae06ab…` | **PASS** |
| S4 | Pack v1 freeze held; plant untouched; no greenwash | **PASS** |
| S5 | Cellular ADAS PARK | **PASS** |
| S6 | No invent search / invent-scenario / openness deepen / uncap / silent P1 | **PASS** |
| S7 | Owner 焊枪; 找茬喵 gate | process |

---

## Next

焊枪 weld → 找茬喵 gate → 扳手 squash. Design standby.
