# Handoff: KAD Prefer deepen post-gate pack×6 remeasure — 2026-10-25

> **Stream**: B — kunautodrive ADAS-cellular **transfer** (NOT cellular ADAS reopen).
> **Knife**: Prefer **KAD deepen (post-gate)** — **LOCKED** =
> pack×6 regression remeasure under existing Stanley/baseline gate
> (`kad-stanley-baseline-gate` from `#116`) — **NO** invent-scenario.
> **Base tip**: `b0103bf5` / `b0103bf51b952cda9b69b36610f78e16b31e887f` (main · `#116` Stanley/baseline gate)
> **Branch**: `feat/kad-adas-deepen-postgate-remeasure`
> **Owner**: 手术刀 (WSL caixuf) · Gate: 找茬喵 · Merge: 扳手 · (+焊枪 only if gate gap)
> **Outcome**: **PASS** — pack×6 under controller=Stanley @ tip_hash `b0103bf5…`
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
- Cellular tip `2f3f221` cite-only if dual-ceiling / cross-stack context needed — **not** used as KAD product scoreboard paste here.

---

## Distinct from `#116` / keep `#115`

| Prior | This Prefer |
|---|---|
| `#116` Stanley/baseline gate | Wired fail-closed Stanley/baseline gate on pack×6 — **LIVE; this knife remeasures under it** |
| `#115` suite-expand | CI-locked product suite runner ×6 — **KEEP** |
| **This** | Fresh pack×6 **remeasure** under existing Stanley gate @ tip `b0103bf5…` → archive dossier |

Plant cold-pack (`tools/bench_kad_baseline_cte.py` · plant IDs straight/lane_change/…) stays **UNTOUCHED**.
Gate harness / CI check / evaluator / tol **not** rewired. No invent-scenario / no 7th scene.

---

## Remeasure (Stanley/baseline @ tip `b0103bf5…`) — PASS

| Field | Value |
|---|---|
| Tip (branch / measure) | `b0103bf51b952cda9b69b36610f78e16b31e887f` (`b0103bf5…`) |
| Artifact | `artifacts/kad_stanley_baseline_gate.json` |
| schema | `kad.stanley_baseline_gate.v1` |
| status | **ok** |
| controller | **Stanley** (default product lateral; `use_ltv_mpc=0`; no retune) |
| pack_id | `product_suite_pack_v1` |
| frozen_scene_count | 6 |
| numbers_claimed | true |
| ready_for_later_compare | true |
| Station | WSL caixuf `/home/caixuf/code/kunautodrive` |
| Command | `python3 tools/bench_kad_stanley_baseline_gate.py --out artifacts/kad_stanley_baseline_gate.json` |
| CI check | `python3 ci/gates/kad_stanley_baseline_gate_check.py` → **PASS** |
| CI self-test | `python3 ci/gates/kad_stanley_baseline_gate_check.py --self-test` → **SELFTEST PASS** |

### Per-scene (Stanley) — numbers + seeds

| pack_scene_id | evaluator_result | status | cte_mean | cte_p95 | cte_max | n | random_seed | wall_s |
|---|:---:|:---:|---:|---:|---:|---:|---:|---:|
| `straight_road` | **PASS** | ok | 0.501412 | 1.447059 | 1.686392 | 181 | 137 | 56.085 |
| `curve_road` | **PASS** | ok | 0.022943 | 0.044450 | 0.072194 | 241 | 42 | 70.620 |
| `lane_change_traffic` | **PASS** | ok | 0.323184 | 1.360224 | 1.701556 | 181 | 137 | 55.087 |
| `r157_cut_in` | **PASS** | ok | 0.261757 | 1.214671 | 1.718061 | 161 | 157 | 49.460 |
| `oncoming` | **PASS** | ok | 0.0 | 0.0 | 0.0 | 181 | 137 | 54.907 |
| `dense_npc` | **PASS** | ok | 0.442087 | 1.493666 | 1.720845 | 181 | 137 | 54.953 |

Seeds = scenario `random_seed` fields (frozen scene JSON; not invented).
All six scenes `evaluator_result=PASS` · `controller=Stanley` · tip_hash starts `b0103bf5…`.

### Remeasure notes (honesty)

- Full pack×6 headless run completed in one pass (no flake retries; no scene drop).
- tip_hash == measure HEAD `b0103bf5…` at capture time.
- Thin `KNOWN_LOCK_TIPS` allowlist updated to include measure tip
  `b0103bf51b952cda9b69b36610f78e16b31e887f` so post-commit CI tip honesty
  still holds without relaxing fail-closed rules (no tol raise / no invent PASS).
- `#115` product-suite lock still KEPT. Plant cold-pack UNTOUCHED.
- Dual-ceiling `#3` 9W/7L + `#15` 7W/9L cited again — **NOT win**.

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

## Plant cold-pack UNTOUCHED

| Item | Status |
|---|---|
| `tools/bench_kad_baseline_cte.py` | **UNTOUCHED** |
| `artifacts/kad_baseline_cte.json` | **UNTOUCHED** |
| Plant scenes | straight · lane_change · curve · obstacle · merge · cutin |
| Champ md5 `7424dd…` | **untouched** |
| `#115` product-suite lock | **KEPT** |
| `#116` Stanley gate harness / fail-closed rules | **reuse only — no rewire** |

---

## Touch list (this Prefer)

| Path | Role |
|---|---|
| `artifacts/kad_stanley_baseline_gate.json` | Fresh pack×6 numbers @ tip `b0103bf5…` |
| `docs/handoff/2026-10-25-kad-deepen-postgate-remeasure.md` | This archive |
| `ci/gates/kad_stanley_baseline_gate_check.py` | Thin `KNOWN_LOCK_TIPS` += measure tip only |

**Not touched:** invent 7th · plant rewrite · gate disable · tol raise · kun-cellular · champ · invent search · control retune · evaluator rewrite · TL/emily/lane_change P0.

---

## PARK / HOLD / bans held

**PARK**
- Invented search wins
- Cellular ADAS reopen / silent P1 / champ touch
- Invent-scenario / 7th pack scene
- Uncap / invent 328
- Further openness deepen

**HOLD**
- TL / emily
- lane_change P0

**Bans**
- No invent-scenario / invent search / uncap / invent 328
- No greenwash / control retune / ADAS reopen / silent P1 / strength claim
- No fake win / beat-Stanley / invent ≥12W from ceilings
- No DouZero / GPU / PSRO / 4th ABI
- No Bot-box private clone — WSL caixuf + ccp only
- No silent pack-scene drop / plant rewrite
- No pasting cellular W/L as KAD product scoreboard

---

## Acceptance map (R0–R9)

| ID | Check | Result |
|---|---|---|
| R0 | Branch off `b0103bf5`; WSL caixuf + ccp only | **PASS** |
| R1 | Dual-ceiling `#3` 9W/7L + `#15` 7W/9L cited; no fake win / beat-Stanley / strength | **PASS** (this handoff) |
| R2 | Pack×6 remeasure under existing Stanley/baseline gate | **PASS** (artifact) |
| R3 | Numbers + seeds archived; tip_hash honest `b0103bf5…`; PASS | **PASS** |
| R4 | Pack v1 freeze held (no 7th); plant cold-pack untouched; no greenwash / no control retune | **PASS** |
| R5 | Cellular ADAS PARK — no P0/P1/champ/cellular edits | **PASS** |
| R6 | PARK: invent search · invent-scenario · uncap/328 · further openness deepen · silent P1 | **PASS** |
| R7 | HOLD: TL / emily / lane_change P0 | **PASS** |
| R8 | No DouZero/GPU/PSRO / 4th ABI | **PASS** |
| R9 | Owner 手术刀 (+焊枪 gate gap); 找茬喵 gate | process — tip 找茬喵 + 扳手 (no DONE/squash) |

---

## Next

手术刀 measure dossier → **找茬喵** gate → **扳手** squash.
Do **not** declare DONE/squash from this seat. Design standby.
