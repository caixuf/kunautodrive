# Handoff: KAD B2-b baseline harness stub — 2026-10-25

> **Stream**: B — kunautodrive ADAS-cellular **transfer** (NOT cellular ADAS reopen).
> **Knife**: Prefer **B2-b** — thin Stanley / product ADAS path × frozen scenes → JSON CTE.
> **Base**: `c557058c` / `c557058c8d38bc7f6f177135cd1e87192c6d3dc6` (main · `#109` B1+B2-a)
> **Branch**: `feat/kad-adas-b2b-baseline-harness`
> **Owner**: 焊枪 (WSL) · Gate: 找茬喵 · Merge: 扳手
> **Cellular ADAS status**: **PARK** — no kun-cellular edits · no P0/P1/champ · no DouZero/GPU/PSRO/4th ABI

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
- This harness is **ready for later compare** only — not a ship win.

---

## What shipped

| Artifact | Path | Role |
|---|---|---|
| Thin harness CLI | `tools/bench_kad_baseline_cte.py` | Wraps in-tree `tools/control_sim.py` (Stanley default; optional `--compare-mpc`) |
| JSON CTE output | `artifacts/kad_baseline_cte.json` (also under gitignored `runs/` if redirected) | Per-scene `cte_mean` / `cte_p95` / `cte_max` (meters) |
| This handoff | `docs/handoff/2026-10-25-kad-b2b-baseline-harness.md` | Scenes · CLI · dual-ceiling · tip |

**Controller label**: **Stanley** — default product lateral path in `control_sim` (aligned with product Stanley + kappa FF; **no retune**; `use_ltv_mpc` only via optional `--compare-mpc`, not default).

**Metric**: CTE meters ↔ `ControlRaw.cte` per B2-a contract §5. Harvested as `control_sim` `lat_error` / lateral_offset / lane-center proxy (documented per scene). Pure-Python plant — headless WSL runnable without inventing scenes or product retune.

**Not shipped**: full `demo_evaluator` / `scenario_regression` / flowsim bus subscribe for `scenarios/suite.json` product scenes (needs flow_launcher stack). That remains a later thin digest if owners want bus-native `ControlRaw.cte` on named product scenarios — still not a cellular W/L scoreboard.

---

## Frozen scene list (locked — ≥3 ≤16)

| scene_id | Source | Params (frozen) | Seed | CTE definition |
|---|---|---|---|---|
| `straight` | `control_sim.run_simulation` | `do_lane_change=false`, `target_speed=12`, `duration=20` | n/a (deterministic) | `lat_error` series (Stanley convention) |
| `lane_change` | `control_sim.run_simulation` | `do_lane_change=true`, `target_speed=12`, `duration=20` | n/a | `lat_error` vs target lane |
| `curve` | `control_sim.run_curve_scenario` | `kappa=0.005`, `target_speed=12`, `duration=15` | n/a | circle-distance lateral_offset |
| `obstacle` | `control_sim.run_obstacle_avoid_scenario` | `init_speed=12`, `obs_x=60`, `duration=10` | n/a | `lane_center_y(2) - y` (start-lane proxy) |
| `merge` | `control_sim.run_merge_scenario` | `init_speed=8`, `target_speed=15`, `merge_x=50` | n/a | `lane_center_y(1) - y` (merge target) |
| `cutin` | `control_sim.run_cutin_scenario` | `init_speed=12`, `cutin_speed=10`, `cutin_x=30` | n/a | `lane_center_y(2) - y` (ego lane) |

**Count = 6.** Do not silent-drop scenes to greenwash. IDs reuse in-tree `control_sim` demo scene names — **not** a parallel cellular 16-scene dialect as product scoreboard.

---

## CLI

```bash
# Default: Stanley × frozen 6 → artifacts/kad_baseline_cte.json
python3 tools/bench_kad_baseline_cte.py

# Optional KAD-internal Stanley vs LTV-MPC W/L (named mode only — NOT cellular)
python3 tools/bench_kad_baseline_cte.py --compare-mpc

# Custom out path (runs/ is gitignored)
python3 tools/bench_kad_baseline_cte.py --out runs/kad_baseline_cte.json
```

JSON schema key: `kad.baseline_cte.v1` — fields include `tip_hash`, per-scene `cte_mean`/`cte_p95`/`cte_max`, `controller`, optional `optional_wl_vs_named_kad_mode`.

---

## Inventory → thinness note

| In-tree hook | Verdict for this knife |
|---|---|
| `tools/control_sim.py` | **Used** — headless Stanley/MPC plant; CTE-capable |
| `ci/evaluators/demo_evaluator.py` / `scenario_regression.py` | Present; needs full demo/flow_launcher — **not** required for Prefer close |
| `tools/scenarioctl.py` | Scenario JSON validate/fingerprint — not CTE harvest |
| `scenarios/suite.json` + D2-10 IDs | Product scene names exist; bus-native CTE digest deferred (no invent / no retune) |

**deliver-or-reason**: Prefer closed with honest JSON CTE from `control_sim` wrap. Full product-scenario `ControlRaw.cte` bus digest left as next thin — not blocked by inventing win / silent P1.

---

## Claim-language self-check (author)

- [x] No "cellular beat Stanley"
- [x] No inventing ≥12W from ceilings
- [x] Dual-ceiling rows labeled ceiling / PARK / NOT a win (`#3` 9W/7L · `#15` 7W/9L)
- [x] Soft ≥10/16 ≠ closeout stated
- [x] Optional W/L only vs named KAD-internal mode (Stanley vs LTV-MPC)
- [x] No cellular W/L paste as product scoreboard
- [x] No kun-cellular ADAS / champ / P0 / P1 edits
- [x] No DouZero / GPU / PSRO / 4th ABI
- [x] Ready-for-later-compare stated; harness ≠ ship win
- [x] Frozen scenes documented; no silent drop

---

## Next thin step

1. Keep cellular ADAS **PARK**.
2. Optional later: read-only bus digest of `ControlRaw.cte` for named `scenarios/suite.json` product scenes (still not cellular W/L).
3. Any future cortex experiment on KAD must cite the dual-ceiling table and must not claim beat-Stanley / ≥12W from ceilings.
4. Claim-language gate (找茬喵): reject PRs that greenwash ceiling rows or ship cellular champ bins as product.
