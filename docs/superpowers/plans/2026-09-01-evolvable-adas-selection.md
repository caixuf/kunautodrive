# Evolvable ADAS Selection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task with checkpoints.

**Goal:** Produce structurally changed ADAS cellular descendants that pass the complete six-scenario gate without adapter control fallbacks.

**Architecture:** Keep the cellular genome and mutation engine responsible for dependency-derived validity, while the test-side ADAS scenario suite owns deterministic behavior scoring. The evolution loop will evaluate every candidate on all six scenarios, apply measured forward cost, and retain per-scenario evidence for the descendant proof.

**Tech Stack:** C++20, existing `CellularOrganism`/`MorphogeneticEvolutionEngine`, CMake/CTest, `std::chrono`, deterministic `std::mt19937`.

---

**Implementation result:** Completed on 2026-09-01. The graph contract, guarded
layered mutation, shared six-scenario fitness suite, deterministic descendant
proof, negative controls, and held-out AEB perturbation are implemented and
covered by the targeted CTest regression.

## File map

- Modify `kun_quant/include/kun/cellular/cellular_genome.hpp`: add an ID-independent ADAS graph contract, configurable mutation layers, and atomic rollback for guarded mutation.
- Create `tests/adas_scenario_suite.hpp`: move the deterministic bicycle model and six scenario functions into a reusable test fixture; add per-scenario fitness and latency accounting.
- Modify `tests/test_flow_adas_real_control.cpp`: consume the shared scenario suite and keep the seed/no-bypass baseline test.
- Create `tests/test_flow_adas_evolution.cpp`: first hold contract/mutation tests, then run fixed-seed evolution and execute negative controls plus held-out perturbations.
- Modify `CMakeLists.txt`: register the new evolution test target and CTest entry.
- Create `docs/superpowers/specs/2026-09-01-evolvable-adas-selection-design.md`: approved design record (already committed).

## Task 1: Add an ID-independent graph contract

**Files:**
- Modify: `kun_quant/include/kun/cellular/cellular_genome.hpp` near `CellularOrganism::compute_mechanical_demixing_ratio`
- Test: `tests/test_flow_adas_evolution.cpp`

- [ ] **Step 1: Write the failing contract assertions**

Create `tests/test_flow_adas_evolution.cpp` with a test helper that removes the only active incoming edge to the immune action by cell type, then assert that the seed is valid before removal and invalid afterward:

```cpp
static void remove_only_immune_input(CellularOrganism& org) {
    uint32_t immune_id = 0;
    for (const auto& cell : org.cells) {
        if (cell.type == CellType::ACT_IMMUNE_BLOCK) {
            immune_id = cell.id;
            break;
        }
    }
    for (auto& syn : org.synapses) {
        if (syn.is_active && syn.to_cell_id == immune_id) {
            syn.is_active = false;
        }
    }
    org.compile();
}

void test_contract_derivation() {
    auto seed = CellularOrganism::create_adas_seed_organism(1);
    assert(seed.evaluate_adas_contract().valid());
    remove_only_immune_input(seed);
    assert(!seed.evaluate_adas_contract().valid());
}

int main() {
    test_contract_derivation();
    return 0;
}
```

- [ ] **Step 2: Register the focused target**

Add the new executable and CTest entry beside the existing ADAS target:

```cmake
add_executable(test_flow_adas_evolution tests/test_flow_adas_evolution.cpp)
if(WIN32)
    target_link_libraries(test_flow_adas_evolution kun_quant flowengine_core flowcoro_headers scheduler_cpp ws2_32 sqlite3)
else()
    target_link_libraries(test_flow_adas_evolution kun_quant flowengine_core flowcoro_headers flowcoro_net scheduler_cpp pthread sqlite3)
endif()
target_include_directories(test_flow_adas_evolution PRIVATE kun_quant/include include ${FLOWCORO_INCLUDE_DIR} tests)
target_compile_definitions(test_flow_adas_evolution PRIVATE FLOWCORO_INTEGRATION)
add_test(NAME test_flow_adas_evolution COMMAND test_flow_adas_evolution)
```

- [ ] **Step 3: Run the focused target to verify the API is missing**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target test_flow_adas_evolution -j$(nproc)
```

Expected: compilation fails because `test_flow_adas_evolution.cpp` and
`evaluate_adas_contract()` do not exist yet.

- [ ] **Step 4: Implement the contract**

Add this public result type and method to `CellularOrganism`:

```cpp
struct AdasGraphContract {
    bool positive_action{false};
    bool negative_action{false};
    bool defensive_reset{false};
    bool immune_block{false};
    bool longitudinal_input{false};
    bool relative_velocity_input{false};
    bool lateral_input{false};
    bool ttc_input{false};
    size_t reachable_cells{0};

    bool valid() const {
        return positive_action && negative_action && defensive_reset &&
               immune_block && longitudinal_input && relative_velocity_input &&
               lateral_input && ttc_input;
    }
};

AdasGraphContract evaluate_adas_contract() const {
    AdasGraphContract result;
    std::unordered_map<uint32_t, size_t> id_to_index;
    std::vector<std::vector<uint32_t>> outgoing(cells.size());
    for (size_t i = 0; i < cells.size(); ++i) {
        id_to_index[cells[i].id] = i;
    }
    for (const auto& syn : synapses) {
        if (!syn.is_active || syn.is_recurrent) continue;
        auto from = id_to_index.find(syn.from_cell_id);
        auto to = id_to_index.find(syn.to_cell_id);
        if (from != id_to_index.end() && to != id_to_index.end()) {
            outgoing[from->second].push_back(to->second);
        }
    }

    std::vector<bool> reachable(cells.size(), false);
    std::vector<size_t> queue;
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto type = cells[i].type;
        if (type == CellType::SENSE_RAW_INPUT_0) {
            result.longitudinal_input = true;
            reachable[i] = true;
        } else if (type == CellType::SENSE_RAW_INPUT_1) {
            result.relative_velocity_input = true;
            reachable[i] = true;
        } else if (type == CellType::SENSE_RAW_INPUT_2) {
            result.lateral_input = true;
            reachable[i] = true;
        } else if (type == CellType::SENSE_RAW_INPUT_3) {
            result.ttc_input = true;
            reachable[i] = true;
        }
        if (reachable[i]) queue.push_back(i);
    }
    for (size_t head = 0; head < queue.size(); ++head) {
        for (uint32_t next_id : outgoing[queue[head]]) {
            const size_t next = id_to_index.at(next_id);
            if (!reachable[next]) {
                reachable[next] = true;
                queue.push_back(next);
            }
        }
    }
    for (size_t i = 0; i < cells.size(); ++i) {
        if (!reachable[i]) continue;
        ++result.reachable_cells;
        switch (cells[i].type) {
            case CellType::ACT_PRIMARY_POSITIVE: result.positive_action = true; break;
            case CellType::ACT_PRIMARY_NEGATIVE: result.negative_action = true; break;
            case CellType::ACT_DEFENSIVE_RESET: result.defensive_reset = true; break;
            case CellType::ACT_IMMUNE_BLOCK: result.immune_block = true; break;
            default: break;
        }
    }
    return result;
}
```

- [ ] **Step 5: Run the contract test**

Run:

```bash
cmake --build build --target test_flow_adas_evolution -j$(nproc) && ./build/bin/test_flow_adas_evolution
```

Expected: the contract assertions pass, including the removed-immune-edge
negative control.

- [ ] **Step 6: Commit**

```bash
git add kun_quant/include/kun/cellular/cellular_genome.hpp tests/test_flow_adas_evolution.cpp
git commit -m "feat(cellular): derive ADAS graph contract"
```

## Task 2: Add layered, dependency-guarded mutation

**Files:**
- Modify: `kun_quant/include/kun/cellular/cellular_genome.hpp` in `EvolutionConstraintConfig` and `MorphogeneticEvolutionEngine::mutate`
- Test: `tests/test_flow_adas_evolution.cpp`

- [ ] **Step 1: Write mutation rollback tests**

Use an ADAS-configured engine and assert that guarded mutation never leaves an
invalid contract and that at least one child has a different fingerprint:

```cpp
static size_t active_synapse_count(const CellularOrganism& org) {
    size_t count = 0;
    for (const auto& syn : org.synapses) if (syn.is_active) ++count;
    return count;
}

void test_guarded_mutation() {
    EvolutionConstraintConfig cfg;
    cfg.seed_mode = SeedInitMode::HANDCRAFTED_PROGENITOR;
    cfg.enable_dependency_guard = true;
    cfg.fast_mutation_rate = 1.0;
    cfg.medium_mutation_rate = 1.0;
    cfg.slow_mutation_rate = 1.0;
    MorphogeneticEvolutionEngine engine(12, 77, cfg);
    const auto seed = CellularOrganism::create_adas_seed_organism(1);
    const size_t seed_cells = seed.cells.size();
    const size_t seed_synapses = active_synapse_count(seed);
    bool changed = false;
    for (auto& candidate : engine.population()) {
        candidate = seed;
        engine.mutate(candidate);
        assert(candidate.evaluate_adas_contract().valid());
        changed |= candidate.cells.size() != seed_cells ||
                   active_synapse_count(candidate) != seed_synapses;
    }
    assert(changed);
}
```

- [ ] **Step 2: Run the focused test to verify missing configuration/API**

Run:

```bash
cmake --build build --target test_flow_adas_evolution -j$(nproc)
```

Expected: compilation fails because the mutation-layer configuration,
`enable_dependency_guard`, and the test mutation entry point are absent.

- [ ] **Step 3: Add configuration and a test-only mutation entry point**

Extend `EvolutionConstraintConfig` without changing existing defaults:

```cpp
bool enable_dependency_guard{false};
double fast_mutation_rate{0.80};
double medium_mutation_rate{0.45};
double slow_mutation_rate{0.35};
```

The existing public `mutate(CellularOrganism&)` entry point is used directly
by the focused test, so the test does not require a production-only wrapper.

- [ ] **Step 4: Make mutation atomic when the guard is enabled**

At the start of `mutate`, snapshot only when the guard is active. Replace the
three hard-coded mutation probabilities with the three configuration values,
and roll back the complete candidate if its post-mutation contract is invalid:

```cpp
void mutate(CellularOrganism& org) {
    std::optional<CellularOrganism> before;
    if (constraint_cfg_.enable_dependency_guard) before.emplace(org);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    size_t active_syns = 0;
    for (const auto& syn : org.synapses) if (syn.is_active) ++active_syns;
    if (active_syns < 4) {
        for (int i = 0; i < 3; ++i) mutate_add_synapse(org);
    }
    if (dist(rng_) < std::min(0.95, constraint_cfg_.fast_mutation_rate * adaptive_mutation_boost_)) {
        mutate_parameters(org);
    }
    if (dist(rng_) < std::min(0.85, constraint_cfg_.medium_mutation_rate * adaptive_mutation_boost_)) {
        mutate_add_synapse(org);
    }
    if (constraint_cfg_.enable_mechanotransduction) {
        if (dist(rng_) < std::min(0.70, constraint_cfg_.slow_mutation_rate * adaptive_mutation_boost_)) {
            mutate_mechanosensitive_mitosis(org);
        }
    } else if (dist(rng_) < std::min(0.70, constraint_cfg_.slow_mutation_rate * adaptive_mutation_boost_)) {
        mutate_add_cell(org);
    }
    if (dist(rng_) < 0.05) prune_apoptosis(org);

    if (before && !org.evaluate_adas_contract().valid()) {
        org = std::move(*before);
    }
}
```

Include `<optional>` with the existing standard-library headers. This is a
dependency guard, not a cell-ID lock: a replacement topology is accepted if
the derived action/input paths remain valid.

At the beginning of `evolve_generation`, before metabolic scoring and sorting,
assign `-1e12` to any guarded candidate whose contract is invalid:

```cpp
if (constraint_cfg_.enable_dependency_guard) {
    for (auto& org : population_) {
        if (!org.evaluate_adas_contract().valid()) org.fitness_score = -1e12;
    }
}
```

This prevents an invalid immigrant or caller-supplied high score from
becoming an elite.

- [ ] **Step 5: Run mutation and regression tests**

Run:

```bash
cmake --build build --target test_flow_adas_evolution test_flow_compartment_demixing test_flow_adas_real_control -j$(nproc) &&
./build/bin/test_flow_adas_evolution &&
./build/bin/test_flow_compartment_demixing &&
./build/bin/test_flow_adas_real_control
```

Expected: guarded candidates remain valid, at least one topology changes, and
the existing two tests remain green.

- [ ] **Step 6: Commit**

```bash
git add kun_quant/include/kun/cellular/cellular_genome.hpp tests/test_flow_adas_evolution.cpp
git commit -m "feat(cellular): guard layered ADAS mutations"
```

## Task 3: Extract deterministic six-scenario fitness

**Files:**
- Create: `tests/adas_scenario_suite.hpp`
- Modify: `tests/test_flow_adas_real_control.cpp`
- Test: `tests/test_flow_adas_evolution.cpp`

- [ ] **Step 1: Move the existing scenario fixture without behavior changes**

Move `KinematicBicycleVehicle` and the six `run_scenario_*` functions from
`test_flow_adas_real_control.cpp` into `tests/adas_scenario_suite.hpp`, keeping
the exact constants and thresholds. The header must contain only inline
functions and types so both test executables can include it.

- [ ] **Step 2: Add a reusable result and evaluator**

Append this interface to the header:

```cpp
struct AdasScenarioFitness {
    std::array<bool, 6> passed{};
    std::array<double, 6> metric{};
    double latency_ns{0.0};
    double cost{0.0};
    double score{-1000.0};

    bool all_passed() const {
        return std::all_of(passed.begin(), passed.end(), [](bool value) { return value; });
    }
};

inline AdasScenarioFitness evaluate_adas_fitness(CellularOrganism& organism) {
    AdasScenarioFitness result;
    if (!organism.evaluate_adas_contract().valid()) return result;
    AdasCellularAdapter adapter(organism);

    adapter.get_organism().reset_state(true);
    double max_err = 0.0, mean_err = 0.0, latency = 0.0;
    result.passed[0] = run_scenario_curve_tracking(adapter, max_err, mean_err, latency);
    result.metric[0] = max_err;
    result.latency_ns += latency * 1000.0;

    adapter.get_organism().reset_state(true);
    double safety_dist = 0.0; bool aeb = false;
    result.passed[1] = run_scenario_emergency_cutin_aeb(adapter, safety_dist, aeb);
    result.metric[1] = safety_dist;

    adapter.get_organism().reset_state(true);
    double settle = 0.0, overshoot = 0.0;
    result.passed[2] = run_scenario_lane_change(adapter, settle, overshoot);
    result.metric[2] = overshoot;

    adapter.get_organism().reset_state(true);
    double gap_error = 0.0;
    result.passed[3] = run_scenario_stop_and_go(adapter, gap_error);
    result.metric[3] = gap_error;

    adapter.get_organism().reset_state(true);
    double merge_speed = 0.0;
    result.passed[4] = run_scenario_ramp_merge(adapter, merge_speed);
    result.metric[4] = merge_speed;

    adapter.get_organism().reset_state(true);
    double clearance = 0.0;
    result.passed[5] = run_scenario_obstacle_swerve(adapter, clearance);
    result.metric[5] = clearance;

    result.cost = result.latency_ns * 0.0001 +
                  static_cast<double>(organism.cells.size()) * 0.01 +
                  static_cast<double>(organism.synapses.size()) * 0.002;
    result.score = result.all_passed() ? 600.0 - result.cost : -600.0 - result.cost;
    return result;
}
```

Use `<array>` and keep the evaluator's hard failure behavior explicit: an
invalid contract returns the failure score rather than a success-shaped
default.

- [ ] **Step 3: Update the baseline test to include the shared header**

Delete the moved definitions from `test_flow_adas_real_control.cpp`, include
`adas_scenario_suite.hpp`, and keep its existing seed baseline assertions.

- [ ] **Step 4: Run both scenario tests**

Run:

```bash
cmake --build build --target test_flow_adas_real_control test_flow_adas_evolution -j$(nproc) &&
./build/bin/test_flow_adas_real_control
```

Expected: all six baseline scenario lines still pass with the same thresholds,
and the new evaluator compiles.

- [ ] **Step 4: Commit**

```bash
git add tests/adas_scenario_suite.hpp tests/test_flow_adas_real_control.cpp tests/test_flow_adas_evolution.cpp
git commit -m "test(adas): add reusable six-scenario fitness"
```

## Task 4: Prove evolved descendants

**Files:**
- Create: `tests/test_flow_adas_evolution.cpp`

- [ ] **Step 1: Implement fixed-seed descendant evaluation**

Run the progenitor through a small population and deterministic generations:

```cpp
int main() {
    test_contract_derivation();
    test_guarded_mutation();

    EvolutionConstraintConfig cfg;
    cfg.seed_mode = SeedInitMode::HANDCRAFTED_PROGENITOR;
    cfg.enable_dependency_guard = true;
    cfg.fast_mutation_rate = 0.35;
    cfg.medium_mutation_rate = 0.20;
    cfg.slow_mutation_rate = 0.10;

    MorphogeneticEvolutionEngine engine(24, 20260901, cfg);
    auto seed = CellularOrganism::create_adas_seed_organism(1);
    const auto seed_fitness = evaluate_adas_fitness(seed);
    assert(seed_fitness.all_passed());

    bool changed_passing_descendant = false;
    CellularOrganism winner;
    const size_t seed_cells = seed.cells.size();
    const size_t seed_synapses = active_synapse_count(seed);
    for (size_t generation = 0; generation < 20; ++generation) {
        for (auto& candidate : engine.population()) {
            candidate = seed;
            candidate.organism_id = generation + 1;
            const auto fitness = evaluate_adas_fitness(candidate);
            candidate.fitness_score = fitness.score;
        }
        engine.evolve_generation();
        for (const auto& candidate : engine.population()) {
            if (candidate.evaluate_adas_contract().valid() &&
                (candidate.cells.size() != seed_cells ||
                 active_synapse_count(candidate) != seed_synapses)) {
                auto check = candidate;
                if (evaluate_adas_fitness(check).all_passed()) {
                    changed_passing_descendant = true;
                    winner = candidate;
                }
            }
        }
    }
    assert(changed_passing_descendant);
}
```

The test evaluates a mutable `CellularOrganism seed` variable; no cast is
needed because the evaluator resets runtime state while scoring.

- [ ] **Step 2: Add negative controls and held-out perturbation checks**

Require a disconnected embryo to fail the AEB path and run the seed-derived
winner with small perturbations to the scenario inputs. The perturbation
checks must call the same adapter graph, not a second controller:

```cpp
auto blank = CellularOrganism::create_disconnected_embryo(9);
AdasCellularAdapter blank_adapter(blank);
auto blank_ctl = blank_adapter.process_perception(8.0, -4.0, 0.0, 0.8);
assert(!blank_ctl.is_aeb_triggered);

auto held_out = winner;
AdasCellularAdapter perturbed(held_out);
perturbed.get_organism().reset_state(true);
auto ctl = perturbed.process_perception(26.0, -5.0, 0.12, 0.9);
assert(ctl.is_aeb_triggered);
assert(ctl.target_accel_mps2 <= -5.5);
```

Also print the winner's changed cell/synapse counts, contract flags, six
metrics, latency, and cost so a passing scalar cannot hide a failed scenario.

- [ ] **Step 3: Run the complete targeted validation**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release &&
cmake --build build --target test_flow_adas_evolution test_flow_adas_real_control test_flow_compartment_demixing -j$(nproc) &&
ctest --test-dir build --output-on-failure -R 'test_flow_adas_(evolution|real_control)|test_flow_compartment_demixing'
```

Expected: all three tests pass, including a structurally changed descendant
with all six scenario gates and the negative controls.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt tests/test_flow_adas_evolution.cpp
git commit -m "test(adas): prove evolved descendant retention"
```

## Final self-review

- [ ] The contract is derived from cell types and active graph edges, never fixed cell IDs.
- [ ] Guarded mutations roll back atomically and unguarded generic modes retain their current behavior.
- [ ] Every candidate receives all six scenario results and a measured cost component.
- [ ] The adapter remains graph-only; the tests do not add a second controller.
- [ ] A descendant must differ structurally, pass all six scenarios, and pass the negative/held-out checks.
- [ ] Existing ADAS and mechanical-demixing regressions remain green.
