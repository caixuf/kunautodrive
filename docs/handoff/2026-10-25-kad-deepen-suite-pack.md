# Handoff: KAD Prefer deepen — product_suite_pack_v1 — 2026-10-25

> **Stream**: B — kunautodrive ADAS-cellular **transfer** (NOT cellular ADAS reopen).
> **Knife**: Prefer **KAD deepen** — one fixed product scenario pack `product_suite_pack_v1` → CTE digest.
> **Base tip**: `9b62cd01` / `9b62cd01877178e962a21959e841fff345a42b2b` (main · `#111` B2-b first-light)
> **Branch**: `feat/kad-adas-deepen-suite-pack`
> **Owner**: 焊枪 (WSL caixuf) · Gate: 找茬喵 · Merge: 扳手
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
- This pack digest is **ready for later compare** only — **not** a ship win / promote-as-win.

---

## What shipped

| Artifact | Path | Role |
|---|---|---|
| Frozen pack manifest | `scenarios/packs/product_suite_pack_v1.json` | Exactly 6 enabled suite product scenes |
| Thin pack harness | `tools/bench_kad_product_suite_cte.py` | Pack → demo_evaluator headless → JSON CTE |
| Fresh CTE JSON | `artifacts/kad_product_suite_cte.json` | Per-scene `cte_mean` / `cte_p95` / `cte_max` + tip_hash |
| This handoff | `docs/handoff/2026-10-25-kad-deepen-suite-pack.md` | Pack · cmd · numbers · dual-ceiling · tip |

**Not touched**: plant cold-pack frozen 6 · `tools/bench_kad_baseline_cte.py` · kun-cellular · champ · control retune · openness · invent search.

---

## Frozen pack `product_suite_pack_v1` (exact — no silent drop)

| pack_scene_id | file | duration_s (suite) |
|---|---|---|
| `straight_road` | `scenarios/straight_road.json` | 45 |
| `curve_road` | `scenarios/curve_road.json` | 60 |
| `lane_change_traffic` | `scenarios/lane_change_traffic.json` | 45 |
| `r157_cut_in` | `scenarios/r157_cut_in.json` | 40 |
| `oncoming` | `scenarios/oncoming.json` | 45 |
| `dense_npc` | `scenarios/dense_npc.json` | 45 |

**Count = 6.** Exam/disabled suite rows out. Other enabled suite rows (e.g. urban_challenge) intentionally out of this ≤6 lock — **not** a silent drop of locked IDs.

**Plant cold-pack** (straight / lane_change / curve / obstacle / merge / cutin) remains the B2-b harness dialect — **UNTOUCHED**; do not redo plant measure.

---

## Cmd

```bash
# Host: WSL caixuf · repo /home/caixuf/code/kunautodrive · tip 9b62cd01
git fetch origin
git checkout -B feat/kad-adas-deepen-suite-pack 9b62cd01877178e962a21959e841fff345a42b2b
FLOW_SKIP_BUILD=1 python3 tools/bench_kad_product_suite_cte.py
# → artifacts/kad_product_suite_cte.json  (tip_hash stamped from HEAD)

# Optional
python3 tools/bench_kad_product_suite_cte.py --dry-run
FLOW_SKIP_BUILD=1 python3 tools/bench_kad_product_suite_cte.py --only straight_road --duration-override 10  # probe only; no number claim
```

**Metric**: `demo_evaluator.sample_metrics.lane_error` meters (scene `road_network` / nearest-lane geometric proxy) ↔ `ControlRaw.cte` (B2-a §5). Harvested headlessly via `scripts/demo.sh` + `flow_launcher` with per-scene pipeline `monitor.state_file` isolation (same pattern as `scenario_regression.prepare_worker_workspace`).

**Controller / stack**: product pipeline as wired — **no retune**. evaluator PASS/FAIL is regression-envelope evidence, **not** a product W/L scoreboard and **not** a beat-Stanley claim.

---

## Fresh JSON (tip_hash match)

| Field | Value |
|---|---|
| Path | `artifacts/kad_product_suite_cte.json` |
| schema | `kad.product_suite_cte.v1` |
| status | `ok` |
| **tip_hash** | **`9b62cd01877178e962a21959e841fff345a42b2b`** |
| pack_id | `product_suite_pack_v1` |
| frozen_scene_count | 6 |
| numbers_claimed | true |
| ready_for_later_compare | true |

---

## Per-scene CTE numbers (meters)

| pack_scene_id | cte_mean | cte_p95 | cte_max | n_samples | evaluator_result |
|---|---:|---:|---:|---:|:---:|
| `straight_road` | 0.510207 | 1.438966 | 1.673724 | 181 | FAIL |
| `curve_road` | 0.023081 | 0.04511 | 0.071008 | 241 | PASS |
| `lane_change_traffic` | 0.322665 | 1.371022 | 1.742387 | 181 | PASS |
| `r157_cut_in` | 0.26417 | 1.300715 | 1.6938 | 161 | PASS |
| `oncoming` | 0.0 | 0.0 | 0.0 | 181 | PASS |
| `dense_npc` | 0.448397 | 1.49845 | 1.740932 | 181 | PASS |

`straight_road` evaluator FAIL is regression-envelope (not greenwashed away). CTE series still harvested and archived for **later compare only**.

---

## deliver-or-reason note (first attempt)

Initial harness set `FLOW_TOPOLOGY_FILE` alone → monitor still wrote default state_file → 0 samples / fake-blocked. Fixed by patching per-scene `pipeline.json` monitor `state_file` (mirrors `scenario_regression.prepare_worker_workspace`). Full suite-duration run then produced honest numbers. **No fake green JSON** was committed from the blocked attempt.

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
- [x] Frozen 6 documented; no silent drop
- [x] Plant cold-pack UNTOUCHED; tip_hash = measure tip `9b62cd01…`
- [x] evaluator FAIL on `straight_road` not hidden / not retuned to green

---

## Acceptance map (D0–D7)

| ID | Check | Result |
|---|---|---|
| D0 | Branch off `9b62cd01`; WSL caixuf + ccp | PASS |
| D1 | Dual-ceiling `#3` 9W/7L + `#15` 7W/9L cited; no fake win | PASS |
| D2 | Pack `product_suite_pack_v1` frozen 6; no silent drop | PASS |
| D3 | Artifact + numbers on WSL headless | PASS (`artifacts/kad_product_suite_cte.json`) |
| D4 | tip_hash matches measure tip; plant frozen 6 untouched | PASS |
| D5 | Cellular ADAS PARK | PASS |
| D6 | No invent / openness deepen / DouZero/GPU/PSRO / 4th ABI | PASS |
| D7 | Owner 焊枪; gate 找茬喵 | READY |

---

## Next

1. Keep cellular ADAS **PARK**.
2. Gate (找茬喵): claim-language + pack freeze + tip_hash + plant untouched.
3. Do **not** treat pack CTE as cellular scoreboard or ≥12W invent.
4. Optional later: bus-native `ControlRaw.cte` subscribe if owners want — still not a win claim.
