# Handoff: ADAS ← cellular transfer (Stream B) — 2026-10-25

> **Stream**: B — kunautodrive ADAS-cellular **transfer** (NOT cellular ADAS reopen).
> **Base**: `a3947f0c` / `a3947f0cf3945880f5c238182f6eda2db6e395fa`
> **Branch**: `docs/kad-adas-cellular-transfer-2026-10-25`
> **B2 choice**: **B2-a Interface contract** (default) — see `docs/contracts/ADAS_CELLULAR_TRANSFER_INTERFACE_CONTRACT.md`
> **Cellular ADAS status**: **PARK** — no kun-cellular ADAS / champ bins / P0 retest / silent P1 in this stream.

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

---

## What transfers to kunautodrive

Vocabulary, eval surface, and process lessons only — **no binary import**, no welding cellular `include/` into the KAD product binary as a shipped champ.

### Vocabulary (bench → KAD nodes)

- Cellular 6-D ADAS inputs ↔ KAD control / localization surface:
  - `cte` (cross-track error, m) ↔ `ControlRaw.cte` / Stanley `lat_err` in `control_node.cpp`
  - `dpsi` / heading error ↔ Stanley heading term / LTV-MPC `e_psi`
  - `kappa` (path curvature) ↔ Stanley kappa feedforward / MPC path
  - `v` / `verr` (speed / speed error) ↔ `ControlRaw.speed` / `error` / PID longitudinal
  - `danger` ↔ safety / TTC / degrade ladder (`DEGRADE_REASON_LARGE_CTE`)
- SDSC receptor encoding map already mirrored in-tree at `include/sdsc_adas_adapter.h` (`sdsc_adas_encode_receptors`) — **documentation / research adapter**, not a claim that cellular champ is product-ready.

### Eval surface

- Prefer **KAD-native** metrics already on the bus:
  - Primary: `ControlRaw.cte` (m), signed (正=偏左)
  - Secondary: heading / yaw terms already logged in control debug
  - Scenario regression / demo_evaluator PASS-FAIL for product scenes (Stanley / LTV-MPC path)
- Cellular W/L (16-scene style) is **bench dialect** for comparing cortex checkpoints against a Stanley baseline **inside the cellular harness**. Mapping into KAD means: translate scene names → KAD scenarios / frozen digests, and report CTE / PASS — **not** paste cellular W/L as a KAD product scoreboard.

### Process lessons

- Dual-ceiling discipline: cite lock tip + W/L with **ceiling / PARK** labels; never rebrand a ceiling as a ship win.
- Thin docs-first transfer before any bin / include welding.
- STOP+ask when A6-class reopen is proposed; Stream B does not reopen cellular P0.

---

## What does NOT transfer

- Cellular **champ bin** as a kunautodrive product artifact (md5 `7424dd…` held on cellular side; not copied here).
- Claiming **cellular beat Stanley** (forbidden claim language).
- Reopening cellular P0 / silent P1 / kun-cellular ADAS edits in this stream (**B3 PARK held**).
- Inventing **≥12W** (or any soft ≥10/16 closeout) from ceiling rows.
- DouZero / GPU / PSRO narratives.
- Welding cellular `include/` trees into the KAD runtime binary as the default controller path.

---

## Explicit: cellular cortex vs Stanley

Cellular cortex vs Stanley remains **ceiling evidence / PARK**, **not** a product win to import as green into kunautodrive. The dual-ceiling table above is the cite lock: `#3` 9W/7L = ceiling evidence (NOT a win); `#15` 7W/9L best = **A6 CEILING · PARK · STOP+ask**.

---

## B2 choice (this PR)

**B2-a — Interface contract** (preferred default).

Rationale: KAD already has Stanley/LTV-MPC product control, `ControlRaw.cte`, `sdsc_adas_adapter.h`, and contract-doc style (`M1_OSM_INTERFACE_CONTRACT.md`, `FLOWBOARD_CONTRACT.md`). A thin mapping contract under `docs/contracts/` is the cheapest honest artifact: topics / msg fields / CTE (KAD-native) vocabulary bridge — **no bin copy**, no cellular include welding.

B2-b (frozen-scene Stanley/product baseline JSON W/L or CTE stub) deferred: existing hooks exist (`demo_evaluator`, scenario regression) but wiring a cellular-dialect W/L JSON would invite fake-transfer-win language; out of scope for this thin docs PR.

B2-c N/A — B2-a ships.

Artifact: [`docs/contracts/ADAS_CELLULAR_TRANSFER_INTERFACE_CONTRACT.md`](../contracts/ADAS_CELLULAR_TRANSFER_INTERFACE_CONTRACT.md)

---

## Next thin step

1. Keep cellular ADAS **PARK** (no reopen without explicit STOP+ask owner decision).
2. Optionally add a **read-only** CTE digest hook in KAD demos that logs `ControlRaw.cte` mean/p95 for named scenes — still **not** a cellular W/L scoreboard.
3. Any future cortex experiment on KAD must cite the dual-ceiling table and must not claim beat-Stanley / ≥12W from ceilings.
4. Claim-language gate (找茬喵): reject PRs that greenwash ceiling rows or ship cellular champ bins as product.

---

## Claim-language self-check (author)

- [x] No "cellular beat Stanley"
- [x] No inventing ≥12W from ceilings
- [x] Dual-ceiling rows labeled ceiling / PARK / NOT a win
- [x] Soft ≥10/16 ≠ closeout stated
- [x] No cellular champ bin copy
- [x] No kun-cellular ADAS edits in this stream (B3 PARK)
- [x] Transfer framed as vocabulary / eval / process only
