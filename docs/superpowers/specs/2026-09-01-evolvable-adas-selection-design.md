# Evolvable ADAS Selection Design

## Status

Approved for implementation on 2026-09-01.

## Problem

The ADAS cellular adapter now executes the control graph without its former
PD/TTC safety bypasses. The 15-cell ADAS progenitor passes the six existing
kinematic scenarios, but the previous population loop did not produce a valid
evolved descendant: unconstrained mutations could alter the immune path,
disconnect an effector, or remove useful longitudinal behavior. A high score
from the old loop therefore could not be called an evolved organism.

The first evolutionary milestone is narrower than open-ended intelligence:
descendants of one ADAS progenitor must be allowed to change structure while
retaining the functional contract through selection, without adapter fallbacks.

## Goals

1. Permit structural and parametric variation inside the cellular organism.
2. Make mutations preserve a function by protecting roles and dependencies,
   not by permanently protecting cell IDs.
3. Score every candidate on all six scenarios plus a measured execution-cost
   penalty.
4. Record enough evidence to distinguish a true descendant from an unchanged
   clone, an adapter bypass, or a scenario-specific overfit.
5. Keep existing generic evolution modes and the mechanical-demixing test
   behavior-compatible unless the new policy is explicitly enabled.

## Non-goals

- No infection, digital pathogen, or cross-organism code transfer.
- No ecological population-balancing policy in this milestone.
- No thousand-billion-cell runtime representation or macro-operator compiler.
- No claim that passing a kinematic test is production vehicle validation.
- No new hand-written controller in the adapter or in the evolution engine.

## Design

### 1. Functional roles are derived from the graph

The engine will derive a mutation protection view from the current organism:

- receptor coverage: which raw inputs can reach a live action;
- action coverage: which positive, negative, reset, and immune actions have a
  live upstream path;
- bridge cells and synapses whose removal would disconnect a required action;
- compartment/boundary metadata when available.

The view is recomputed after every accepted structural mutation. It is
dependency-aware rather than ID-aware: a replacement cell may take over a role
only after it has a valid path, while an isolated duplicate remains removable.

The default ADAS evolutionary policy requires at least one path for each
required action and at least one path from the longitudinal, lateral, and TTC
input groups. A candidate that violates this contract remains representable
for diagnostics but receives a hard invalidity penalty and cannot become an
elite.

### 2. Layered mutation policy

Mutation operates at three rates:

- fast: parameter and local synapse-weight drift;
- medium: local rewiring or insertion inside a dependency-safe region;
- slow: cell birth, apoptosis, or cross-boundary rewiring.

For a protected bridge, the engine will not delete or split the only live path
in one operation. A structural mutation may proceed only if the post-mutation
dependency view still satisfies the required action contract. If the mutation
would break it, the operation is rejected and the organism is left unchanged.
This is a validity gate, not a fixed cell quota. The existing skeleton-lock
option remains an ablation mode; the new policy protects derived functions
even when the skeleton is unlocked.

ADAS reflex synapses retain zero Hebbian rate by default. Evolutionary
parameter mutation changes genomic weights and parameters between evaluations;
online plasticity must not silently rewrite the genome during a scenario.

### 3. Reproducible six-scenario fitness

The ADAS test harness will expose a reusable evaluator that:

- resets organism state and plasticity before each scenario;
- runs curve tracking, emergency cut-in AEB, lane change, stop-and-go, ramp
  merge, and obstacle swerve;
- accumulates normalized behavior scores and explicit failure penalties;
- rejects non-finite outputs and invalid graph contracts;
- measures forward latency and active graph size for a cost term.

Fitness is the sum of scenario scores minus measured cost and invalidity
penalties. The evaluator will retain per-scenario results, not only one scalar,
so a candidate that wins by sacrificing one safety scenario is visible and
cannot be promoted as a valid descendant.

### 4. Selection evidence and anti-overfitting checks

Each generation will report:

- organism fingerprint (cell types, active edges, and genomic parameters);
- graph contract status and counts of changed cells/edges;
- six scenario outcomes and cost components;
- whether the candidate is a clone of the progenitor.

The validation test will require at least one structurally different
descendant to pass the complete six-scenario gate over multiple deterministic
seeds. It will also run these negative controls:

- blank embryo: no AEB without a graph path;
- adapter bypass probe: extreme TTC cannot trigger a disconnected graph;
- ablation: removing the only immune path fails the contract;
- held-out perturbation: small changes in initial speed, lead distance, and
  lane offset must not turn a passing descendant into a single-trace special
  case.

The milestone is successful only when behavior and structural evidence are
reported together. A hand-authored progenitor passing alone is a baseline,
not an evolutionary result.

## Data flow

```text
progenitor
    -> copy + layered mutation
    -> compile
    -> dependency contract
    -> six-scenario evaluator
    -> measured cost + scenario fitness
    -> deterministic selection
    -> descendant evidence
```

The adapter remains a unit conversion and graph execution boundary. It does
not inspect TTC, relative speed, or lane error to synthesize a control action.

## Error handling

- Failed compilation or a broken dependency contract produces an explicit
  invalid candidate result; it is never converted to a success-shaped score.
- Non-finite action values or scenario state terminate that candidate's
  evaluation with a failure penalty.
- Rejected mutations do not partially modify the organism.
- The evaluator reports the first failed scenario and the contract reason,
  while still preserving the candidate fingerprint for debugging.

## Testing strategy

1. Unit tests for dependency-derived role coverage and atomic mutation
   rejection.
2. Regression tests for the existing compartment-demixing and adapter
   no-bypass behavior.
3. Deterministic evolution test using fixed seeds, requiring changed topology
   and a complete six-scenario pass.
4. Negative-control and held-out perturbation tests described above.
5. Targeted CMake test execution before broader project validation.

## Acceptance criteria

The milestone is complete when all of the following hold:

- no ADAS control action is synthesized outside the cellular graph;
- a structurally changed descendant, not only the seed, passes all six gates;
- the result is reproducible for fixed seeds and includes per-scenario evidence;
- mutation protection is dependency-based and does not rely on permanent cell
  IDs;
- invalid candidates and adapter-bypass probes fail explicitly;
- existing cellular and demixing tests remain green.
