# Handoff: KAD Prefer suite-expand (CI-lock ×6) — 2026-10-25

> **Stream**: B — kunautodrive ADAS-cellular **transfer** (NOT cellular ADAS reopen).
> **Knife**: Prefer **KAD suite-expand** — **LOCKED = B** · CI-lock current
> `product_suite_pack_v1` ×6 as gate (with tip_hash honesty).
> **A PARKED** — Prefer OR allowed A|B but no named hard scenario id; inventing a
> 7th scene now = invent-search adjacency while ×6 just CLEARED. Do **not** pivot to A.
> **Base tip**: `b468c9a7` / `b468c9a755ec5af6f8bb76b042cc271ce46e60fc` (main · `#114` offgrid CLEARED)
> **Branch**: `feat/kad-adas-suite-expand`
> **Owner**: 焊枪 (WSL caixuf) · Gate: 找茬喵 · Merge: 扳手
> **Outcome**: **CI-lock wired** — thin fail-closed gate on pack×6 + locked artifact;
> evidence cites `#114` pack×6 all PASS @ tip_hash `d12e0a64…`.
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
- **Do not claim pack CTE = cellular win.**

---

## Knife B — CI-lock surface

| Path | Role |
|---|---|
| `ci/gates/kad_product_suite_lock_check.py` | **CI-lock** fail-closed: pack freeze ×6 · artifact status=ok · all scenes `evaluator_result=PASS` · tip_hash honesty (HEAD or ancestor with no lock-surface drift) · harness `--dry-run` smoke |
| `.github/workflows/ci.yml` · job `kad-product-suite-lock-gate` | Thin PR/push gate invoking the check (no full demo rebuild) |
| `artifacts/kad_product_suite_cte.json` | Locked evidence from `#114` (unchanged this Prefer — tip_hash `d12e0a64…`) |
| `docs/handoff/2026-10-25-kad-suite-expand.md` | This archive |

### Fail-closed rules

Gate **exits non-zero** when any of:

1. Pack / harness / artifact missing  
2. Pack ID ≠ `product_suite_pack_v1` or frozen IDs ≠ exact six (order+set) — **no 7th / no silent drop**  
3. Artifact `status != ok` or any scene `evaluator_result != PASS`  
4. `tip_hash` missing / not ancestor of `HEAD` / lock surfaces drifted since measure tip  

### Invokes harness

```bash
python3 ci/gates/kad_product_suite_lock_check.py
# internally: python3 tools/bench_kad_product_suite_cte.py --dry-run
```

Full pack×6 headless remeasure is **not** re-run in this thin CI job (needs
`flow_launcher` + ~5 min demos). Evidence is the locked artifact from `#114`.
If lock surfaces change after measure tip → gate fails and demands remeasure
(no greenwash / no tol raise / no scene drop).

---

## Lock evidence

### Cite `#114` pack×6 all PASS

| Field | Value |
|---|---|
| PR | `#114` Prefer offgrid-fix CLEARED |
| Main tip (this Prefer base) | `b468c9a755ec5af6f8bb76b042cc271ce46e60fc` (`b468c9a7…`) |
| Measure tip_hash (artifact) | `d12e0a6413ff56e431326b6ddfbcb633b3a3ee75` (`d12e0a64…`) |
| Artifact | `artifacts/kad_product_suite_cte.json` |
| schema | `kad.product_suite_cte.v1` |
| status | `ok` |
| pack_id | `product_suite_pack_v1` |
| frozen_scene_count | 6 |
| numbers_claimed | true |
| ready_for_later_compare | true |

`d12e0a64…` is an ancestor of `b468c9a7…` (merge of `#114`). Gate pins this
measure tip in `KNOWN_LOCK_TIPS` (cite `#114` CLEARED evidence). The `#114`
squash vs measure tip is Prefer citation comment + unit + artifact only —
runtime weld already measured; **no invented green JSON**. Optional remeasure
to `b468c9a7…` not required for honesty on this knife.

### Per-scene (from `#114` / artifact @ `d12e0a64`)

| pack_scene_id | evaluator_result |
|---|:---:|
| `straight_road` | **PASS** |
| `curve_road` | **PASS** |
| `lane_change_traffic` | **PASS** |
| `r157_cut_in` | **PASS** |
| `oncoming` | **PASS** |
| `dense_npc` | **PASS** |

Prior handoff: `docs/handoff/2026-10-25-kad-offgrid-fix.md`.

---

## Pack freeze held (no 7th)

| pack_scene_id | suite file |
|---|---|
| `straight_road` | `scenarios/straight_road.json` |
| `curve_road` | `scenarios/curve_road.json` |
| `lane_change_traffic` | `scenarios/lane_change_traffic.json` |
| `r157_cut_in` | `scenarios/r157_cut_in.json` |
| `oncoming` | `scenarios/oncoming.json` |
| `dense_npc` | `scenarios/dense_npc.json` |

**A invent-scenario PARKED.** Exam / parking HOLD. No silent drop. No tol raise.

---

## Plant cold-pack ×6 UNTOUCHED

| Item | Status |
|---|---|
| `tools/bench_kad_baseline_cte.py` | **UNTOUCHED** |
| Plant IDs `straight · lane_change · curve · obstacle · merge · cutin` | **UNTOUCHED** |
| Control scoreboard / retune | **not touched** |
| Champ ADAS md5 `7424dd…` | **untouched** |
| kun-cellular | **no edits** |

---

## Honesty / non-claims

- No fake win / beat-Stanley / invent ≥12W from ceilings  
- No claim pack CTE = cellular win  
- No gate greenwash (raise tol / drop scene / disable evaluator)  
- No silent P1 / champ paste / plant rewrite  
- No invent search / further openness deepen / DouZero/GPU/PSRO/4th ABI  
- deliver-or-reason path exists if lock cannot land without inventing green — **not needed this Prefer** (wiring landed)

---

## Tip / branch

| | |
|---|---|
| Base | `b468c9a7` / `b468c9a755ec5af6f8bb76b042cc271ce46e60fc` |
| Branch | `feat/kad-adas-suite-expand` off that tip **only** |
| Evidence tip_hash | `d12e0a64…` (cite `#114`; ancestor of base; no lock-surface drift) |

---

## Acceptance map (rails S0–S7)

| ID | Check | This Prefer |
|---|---|---|
| S0 | Branch off `b468c9a7`; WSL caixuf + ccp | **PASS** |
| S1 | Dual-ceiling `#3` 9W/7L + `#15` 7W/9L cited; no fake win | **PASS** (this handoff) |
| S2 | Knife B: ×6 CI-lock wired **or** deliver-or-reason | **PASS** (gate + CI job) |
| S3 | Lock evidence: pack×6 PASS cited; tip_hash honest | **PASS** (cite `#114` / `d12e0a64…`) |
| S4 | Pack v1 freeze held; plant untouched; no greenwash | **PASS** |
| S5 | Cellular ADAS PARK | **PASS** |
| S6 | No invent search / no A invent-scenario / no 4th ABI | **PASS** |
| S7 | Owner 焊枪; 找茬喵 gate | process |

---

## Residual

Thin CI job does **not** rebuild + re-run full pack×6 demos on every PR (cost /
headless stack). Fail-closed artifact + tip_hash drift check is the lock.
Nightly / manual full remeasure remains available via:

```bash
FLOW_SKIP_BUILD=1 python3 tools/bench_kad_product_suite_cte.py \
    --out artifacts/kad_product_suite_cte.json
```

If that remeasure lands, tip_hash should start with the measure HEAD (e.g.
`b468c9a7…` when measured there).
