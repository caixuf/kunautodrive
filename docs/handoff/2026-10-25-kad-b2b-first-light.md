# Handoff: KAD Prefer B2-b first-light measure — 2026-10-25

> **Stream**: B — kunautodrive ADAS-cellular **transfer** (NOT cellular ADAS reopen).
> **Knife**: Prefer **B2-b first-light** — one cold-pack measure of tip harness (Stanley × frozen 6).
> **Base tip**: `037592d8` / `037592d898ca34830945bda09bd5a3f4bbda9656` (main · `#110` B2-b harness)
> **Branch**: `feat/kad-adas-b2b-first-light`
> **Owner**: 手术刀 (WSL caixuf) · Gate: 找茬喵 · Merge: 扳手
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
- This measure is **ready for later compare** only — **not** a ship win / promote-as-win.

---

## Cmd + cold pack

```bash
# Host: WSL caixuf · repo /home/caixuf/code/kunautodrive · tip 037592d8
git fetch origin
git checkout -B feat/kad-adas-b2b-first-light 037592d8
python3 tools/bench_kad_baseline_cte.py
# → artifacts/kad_baseline_cte.json  (overwrite; tip_hash stamped from HEAD)
```

**Controller**: Stanley (default as wired in `control_sim`; **no retune**). Optional `--compare-mpc` not used this knife (KAD-internal named mode only if later needed).

**Cold pack** = B2-b frozen 6 (no silent drop / no param retune):

| scene_id | Source | Params (frozen) | Seed |
|---|---|---|---|
| `straight` | `control_sim.run_simulation` | `do_lane_change=false`, `target_speed=12`, `duration=20` | `null` |
| `lane_change` | `control_sim.run_simulation` | `do_lane_change=true`, `target_speed=12`, `duration=20` | `null` |
| `curve` | `control_sim.run_curve_scenario` | `kappa=0.005`, `target_speed=12`, `duration=15` | `null` |
| `obstacle` | `control_sim.run_obstacle_avoid_scenario` | `init_speed=12`, `obs_x=60`, `duration=10` | `null` |
| `merge` | `control_sim.run_merge_scenario` | `init_speed=8`, `target_speed=15`, `merge_x=50` | `null` |
| `cutin` | `control_sim.run_cutin_scenario` | `init_speed=12`, `cutin_speed=10`, `cutin_x=30` | `null` |

**Seeds**: all `null` — plant is deterministic (pure-Python `control_sim`); no silent seed churn. Recorded as null OK.

---

## Fresh JSON (tip_hash match)

| Field | Value |
|---|---|
| Path | `artifacts/kad_baseline_cte.json` |
| schema | `kad.baseline_cte.v1` |
| **tip_hash** | **`037592d898ca34830945bda09bd5a3f4bbda9656`** (matches tip; prior committed `ac14e9ba…` was **STALE**) |
| controller_label | Stanley (default product lateral in control_sim; no retune) |
| frozen_scene_count | 6 |
| ready_for_later_compare | true |

---

## Per-scene CTE numbers (meters)

| scene_id | cte_mean | cte_p95 | cte_max | pass |
|---|---:|---:|---:|:---:|
| `straight` | 0.0 | 0.0 | 0.0 | PASS |
| `lane_change` | 0.506634 | 2.751097 | 3.5 | PASS |
| `curve` | 0.006624 | 0.008571 | 0.008872 | PASS |
| `obstacle` | 1.483147 | 2.944477 | 3.001304 | PASS |
| `merge` | 3.01485 | 7.0 | 7.0 | PASS |
| `cutin` | 0.0 | 0.0 | 0.0 | PASS |

Metric: `control_sim` lat_error / lateral_offset ↔ `ControlRaw.cte` (B2-a §5). Numbers archived for **later compare only** — not a product W/L scoreboard, not a beat-Stanley claim, not ≥12W invent from cellular ceilings.

---

## Prior + next

| Item | Status |
|---|---|
| B2-b harness stub `#110` @ `037592d8` | LIVE — this knife measured it |
| Prior B2-b handoff | `docs/handoff/2026-10-25-kad-b2b-baseline-harness.md` |
| Cellular ADAS / champ | **PARK** · md5 `7424dd1115b60c675aded7a59efae30a` held |
| Invented search / openness deepen | **PARK** |
| Harness redesign / bus digest | Out of knife |

**Next**: 找茬喵 claim-language gate → 扳手 squash. Keep cellular ADAS PARK. Optional later bus digest of product `ControlRaw.cte` remains thin / non-win.

---

## Claim-language self-check (author · 手术刀)

- [x] No "cellular beat Stanley" / promote-as-win
- [x] No inventing ≥12W from ceilings
- [x] Dual-ceiling rows labeled ceiling / PARK / NOT a win (`#3` 9W/7L · `#15` 7W/9L)
- [x] Soft ≥10/16 ≠ closeout stated
- [x] No cellular W/L paste as product scoreboard
- [x] No kun-cellular ADAS / champ / P0 / P1 edits
- [x] No DouZero / GPU / PSRO / 4th ABI / Bot-box clone
- [x] Frozen 6 documented; no silent drop / no param retune
- [x] tip_hash matches `037592d8…`; stale `ac14e9ba…` overwritten by re-run
- [x] Ready-for-later-compare stated; measure ≠ ship win

## Acceptance B0–B7 (self)

| ID | Check | Result |
|---|---|---|
| B0 | Branch off `037592d8`; WSL caixuf only | PASS |
| B1 | Dual-ceiling honest; no beat-Stanley / promote-as-win | PASS |
| B2 | Frozen 6 + seeds/params recorded; no silent drop | PASS |
| B3 | JSON tip_hash = `037592d8…` | PASS |
| B4 | Numbers in handoff; ready-for-later-compare | PASS |
| B5 | Cellular ADAS PARK | PASS |
| B6 | No DouZero/GPU/PSRO / 4th ABI / invent search | PASS |
| B7 | Owner 手术刀; 找茬喵 claim gate | PASS (await gate) |
