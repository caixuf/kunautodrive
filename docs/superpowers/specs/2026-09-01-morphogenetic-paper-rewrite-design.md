# Morphogenetic Cellular Graph Paper Rewrite Design

**Date:** 2026-09-01
**Target:** `docs/morphogenetic_cellular_evolution_paper.md` and
`docs/morphogenetic_cellular_evolution_paper.zh.md`
**Positioning:** Reproducible research paper

## 1. Objective

Rewrite the English and Chinese manuscripts as one evidence-aligned research
paper. The paper will present the implemented cellular graph architecture,
deterministic execution, guarded structural evolution, and simulation evidence
without treating hypotheses, synthetic experiments, or roadmap items as
real-world validation.

The rewrite must improve both scientific credibility and visual clarity.

## 2. Central Research Question

Can a small typed computational graph undergo structural mutation while
preserving task-critical input-to-action dependencies, and can the resulting
organisms execute deterministically with measurable behavior in reproducible
simulation tasks?

The paper will not claim to demonstrate biological life, general intelligence,
production trading profitability, vehicle certification, or open-ended
evolution.

## 3. Evidence Hierarchy

Every substantive claim belongs to exactly one tier:

| Tier | Label | Required support | Permitted wording |
|---|---|---|---|
| E1 | Empirically demonstrated | Reproducible repository test or benchmark | “measured,” “observed,” “passed” |
| E2 | Implemented | Source implementation exists but evidence is incomplete | “the implementation provides” |
| E3 | Hypothesis | Proposed mechanism or scaling interpretation | “we hypothesize,” “may,” “future work” |

ASIL-D certification, production trading, real-road validation, autonomous
organ formation, cognition, ecology, and trillion-cell scaling are E3 unless
new independent evidence is added.

## 4. Manuscript Architecture

1. **Title and structured abstract**
   - Problem, method, evaluated evidence, principal result, limitations.
2. **Introduction**
   - Narrow motivation and explicit research questions.
3. **Related work**
   - Neuroevolution, genetic programming, neural cellular automata,
     force-directed morphology, and deterministic graph execution.
4. **System model**
   - Cell primitives, synapses, state, compilation, execution semantics.
5. **Structural evolution**
   - Mutation operators, dependency-derived graph contracts, rollback,
     selection, and the distinction between protection and fixed IDs.
6. **Mechanical embedding**
   - Lennard-Jones and spring dynamics as implemented spatial organization;
     demixing and spectral separation remain hypotheses where not measured.
7. **Experimental methodology**
   - Hardware/software environment, seeds, repetitions, metrics, baselines,
     failure criteria, and reproducibility commands.
8. **Results**
   - Microbenchmarks, ADAS simulation, synthetic market experiment, ablations.
9. **Threats to validity and limitations**
   - Synthetic data, deterministic simulators, small graph descendants,
     benchmark scope, absent independent safety certification.
10. **Discussion and testable hypotheses**
    - Mechanical compartments, sparse scaling, ecology, and macro-operators.
11. **Conclusion**
    - Restricted to evidence established in the paper.
12. **Appendices**
    - Reproduction matrix, claim-to-test ledger, complete parameters.

## 5. Visual and Typographic System

The Markdown source will use a restrained publication style that exports cleanly
to PDF and HTML:

- one title, one subtitle, and a compact author/date block;
- numbered sections with no decorative terminal banners;
- a structured abstract with **Background**, **Method**, **Results**, and
  **Limitations**;
- a compact “Contributions” panel containing no more than five items;
- consistent `Figure N` and `Table N` captions;
- tables limited to one purpose and one unit convention each;
- equations followed by symbol definitions at first use;
- diagrams redrawn as Mermaid only when they encode architecture or procedure;
- no emoji, court language, marketing superlatives, or absolute claims;
- callouts used only for `Evidence`, `Limitation`, and `Hypothesis`;
- English and Chinese manuscripts share section numbers, figure numbers, table
  numbers, metrics, and evidence labels;
- repository-relative links replace machine-local `file:///` links.

The visual hierarchy will emphasize evidence:

```text
Research question
    -> Method
        -> Reproducible experiment
            -> Result
                -> Limitation
                    -> Testable next hypothesis
```

## 6. Claim Corrections

The rewrite will make the following mandatory corrections:

- replace “proof of convergence” with bounded empirical convergence evidence;
- replace “ASIL-D certified/proven” with auditable graph constraints or
  generated verification artifacts, as applicable;
- replace “live trading” with synthetic or simulated market evaluation unless
  an external dataset and execution record are documented;
- replace “production chassis/real road” with deterministic vehicle simulation;
- distinguish the Transformer parameter count from computational-cell count;
- distinguish GPU tensor experiments from the C++ `CellularOrganism` runtime;
- report measured latency as a distribution with hardware and graph size, not
  as a universal constant;
- move cognition, ecosystems, radiation metaphors, organ emergence, and
  trillion-scale projections into explicitly labeled hypotheses;
- reconcile primitive taxonomy, parameter dimensionality, force-field timings,
  scenario counts, seeds, and all numerical tables between languages.

## 7. Quantitative-Trading Scope

The quantitative-finance section will state that:

- four raw market fields currently enter `QuantCellularAdapter`;
- the current adapter emits hold, buy, sell, close, and risk-lock decisions;
- existing “combat” tests use generated price paths rather than documented
  exchange replay data;
- current cellular fitness is not yet a full walk-forward objective containing
  realized PnL, slippage, fees, drawdown, and fill feedback;
- production-readiness and profitability are therefore not conclusions of this
  paper.

## 8. ADAS Scope

The ADAS section will report only the six deterministic scenarios implemented
in the shared scenario suite. It will distinguish graph-derived actions from
adapter logic and document the negative controls, dependency-contract failures,
fixed seeds, structural descendant changes, and held-out TTC perturbation.

The paper will explicitly state that these experiments are not real-vehicle,
hardware-in-the-loop, ISO 26262 qualification, or safety certification.

## 9. Rewrite Acceptance Criteria

The rewrite is complete when:

1. every headline number maps to a test, benchmark, or an E2/E3 label;
2. English and Chinese tables contain identical facts;
3. unsupported absolutes and certification language are absent;
4. implemented mechanisms are separated from hypotheses;
5. the quantitative-trading and ADAS evaluation boundaries are explicit;
6. all local file links are removed;
7. section, figure, table, equation, and terminology conventions are consistent;
8. the manuscripts remain readable as papers rather than repository changelogs;
9. unrelated model and source files are not modified.

## 10. Out of Scope

- new algorithms, tests, datasets, or benchmark runs;
- implementation changes to the cellular engine;
- journal-specific LaTeX templates;
- claims requiring external certification or independent replication;
- fabrication of missing statistical results.
