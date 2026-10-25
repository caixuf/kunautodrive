# ADAS ← cellular transfer — Interface contract (v0.1)

> **Status**: thin docs contract (Stream B / B2-a). **Not** a product win. **Not** a bin import.
> **Companion handoff**: [`docs/handoff/2026-10-25-adas-cellular-transfer.md`](../handoff/2026-10-25-adas-cellular-transfer.md)
> **Style refs**: `docs/M1_OSM_INTERFACE_CONTRACT.md`, `docs/FLOWBOARD_CONTRACT.md`
> **Hard non-goals**: copy cellular champ bin; weld cellular `include/` into KAD binary; reopen cellular P0; claim beat-Stanley / ≥12W.

---

## 1. Purpose

Map **cellular bench vocabulary** → **kunautodrive ADAS nodes / msgs / metrics** so later thin experiments share one dictionary. This contract does **not** authorize shipping a cellular checkpoint as the default KAD controller.

---

## 2. Dual-ceiling cite lock (must travel with any transfer claim)

| Ceiling | Cellular PR / tip | W/L | Label |
|---|---|---|---|
| Lock / prior W1 | `#3` @ `2922599a` | **9W/7L** | **ceiling evidence, NOT a win** |
| P0 reopen A6 | `#15` @ `50431c48` | **7W/9L** best | **A6 CEILING · PARK · STOP+ask**; champ md5 `7424dd1115b60c675aded7a59efae30a` held |

Gate ≥12W/4L never green on cellular. Soft ≥10/16 ≠ closeout. Cellular cortex vs Stanley = **ceiling/PARK**, not a green product import.

---

## 3. Topic / node mapping (KAD side — authoritative bus)

From `docs/PIPELINE_ARCHITECTURE.md` and `modules/adas_nodes`:

| Role | KAD topic | Node | Notes |
|---|---|---|---|
| Ego / plant state | `vehicle/state` | `flowsim` (sim) / drivers (hw) | EgoState-shaped |
| Fused pose | `fusion/localization` | `fusion_node` / localization | Localization msg |
| Plan | `planning/trajectory` | `planning_node` | Trajectory |
| Raw control + CTE | `control/raw_cmd` | `control_node` | **ControlRaw** includes `cte` |
| Safe command | `control/cmd` | `safety_control_node` | ControlCmd (no cte field) |

Default lateral product path: **Stanley + kappa FF** (optional LTV-MPC via `use_ltv_mpc`). See `docs/ALGORITHM_STACK.md`, `docs/GLOSSARY.md` (Stanley).

---

## 4. Message field mapping (cellular 6-D → KAD)

Cellular ADAS bench packs 6 floats into `sdsc_adas_encode_receptors` (also present in-tree as research adapter `include/sdsc_adas_adapter.h`):

| Cellular input idx | Cellular name | KAD source (preferred) | Notes |
|---|---|---|---|
| 0 | `cte_n` (normalized CTE) | `ControlRaw.cte` on `control/raw_cmd` | Physical CTE in **meters** on bus; normalization is bench-local — do not write normalized CTE into `ControlRaw.cte` |
| 1 | `dpsi_n` | heading error vs trajectory / ref (control internal; Localization.heading + Trajectory) | Not a dedicated IDL field today |
| 2 | `kappa_n` | path curvature from planning / road geometry | Stanley FF / MPC path |
| 3 | `v_n` | `ControlRaw.speed` / `EgoState.speed_mps` / `Localization.v` | m/s on bus |
| 4 | `verr_n` | `ControlRaw.error` (speed error) | m/s |
| 5 | `danger_n` | safety / TTC / degrade (`DEGRADE_REASON_LARGE_CTE`) | No single float topic; map via safety_control / odd_monitor signals |

### Receptor channel names (documentation only)

From `include/sdsc_cortex.h` comments: `REC_CTE_L/R`, `REC_CTE_COARSE_L/R`, `REC_PSI`, `REC_PSI_STRONG`, `REC_KAPPA`, `REC_CENTRIPETAL`, `REC_SPEED`, `REC_VERR`, `REC_VERR_NEG`, `REC_DANGER`. These label the **research cortex layout**, not a requirement that KAD product nodes publish receptor tensors.

---

## 5. Eval metric mapping

| Cellular bench dialect | KAD-native mapping | Closeout rule |
|---|---|---|
| Scene W/L vs Stanley baseline | Scenario / demo PASS-FAIL + CTE stats from `ControlRaw.cte` | Do **not** paste cellular W/L as KAD product score |
| Soft ≥10/16 | Not a KAD closeout | Explicitly **≠ closeout** |
| Gate ≥12W/4L | Never treat as green from ceiling cites | Locked never-green on cellular ceilings |

**Primary KAD metric for transfer experiments**: CTE in meters (`ControlRaw.cte`), optionally mean / p95 / max over a named scene. Secondary: existing scenario_regression / demo_evaluator PASS.

---

## 6. In-tree adapter boundary (read carefully)

| Artifact | Role | Allowed in Stream B? |
|---|---|---|
| `include/sdsc_adas_adapter.h` | Encode 6-D → 12 receptors; thin C inline | Document / optional research hook — **not** product default controller |
| `include/sdsc_cortex.h` | Cortex forward kernel present in tree | Same — research / optional; not a ship claim |
| Cellular champ `.bin` | Trained weights on cellular side | **NO copy** into this PR / product path |
| Welding cellular `include/` over KAD | Binary product change | **NO** |

---

## 7. Explicit non-claims

- This contract does **not** assert cellular cortex beats Stanley.
- Dual-ceiling rows remain **ceiling / PARK**, never wins.
- No ≥12W invented from ceilings; soft ≥10/16 ≠ closeout.
- Cellular ADAS remains **PARK** (Stream B does not edit kun-cellular ADAS).

---

## 8. Next thin implementation (out of this PR)

If a follow-up is needed: add a **read-only** logger / digest that records `ControlRaw.cte` for named KAD scenes. Still no champ bin, no beat-Stanley claim language.
