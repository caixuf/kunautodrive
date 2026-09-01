#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>

#include "adas_scenario_suite.hpp"
#include "kun/cellular/cellular_genome.hpp"

using namespace kun;
using namespace kun::adas_test;

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

static bool test_contract_derivation() {
    auto seed = CellularOrganism::create_adas_seed_organism(1);
    if (!seed.evaluate_adas_contract().valid()) {
        std::cerr << "seed ADAS graph contract is invalid\n";
        return false;
    }
    remove_only_immune_input(seed);
    if (seed.evaluate_adas_contract().valid()) {
        std::cerr << "removing the immune path did not invalidate the contract\n";
        return false;
    }
    return true;
}

static bool test_contract_binds_ttc_to_immune_path() {
    auto candidate = CellularOrganism::create_adas_seed_organism(1);
    uint32_t ttc_id = 0;
    uint32_t positive_id = 0;
    for (const auto& cell : candidate.cells) {
        if (cell.type == CellType::SENSE_RAW_INPUT_3) ttc_id = cell.id;
        if (cell.type == CellType::ACT_PRIMARY_POSITIVE) positive_id = cell.id;
    }
    for (auto& syn : candidate.synapses) {
        if (syn.from_cell_id == ttc_id) syn.is_active = false;
    }
    candidate.synapses.push_back({ttc_id, positive_id, 0, 1.0, true, 60.0f, -1.0f});
    candidate.compile();
    if (candidate.evaluate_adas_contract().valid()) {
        std::cerr << "TTC rerouted to positive action without immune coverage\n";
        return false;
    }
    return true;
}

static size_t active_synapse_count(const CellularOrganism& org) {
    size_t count = 0;
    for (const auto& syn : org.synapses) {
        if (syn.is_active) ++count;
    }
    return count;
}

static bool test_guarded_mutation() {
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
        if (!candidate.evaluate_adas_contract().valid()) {
            std::cerr << "guarded mutation left an invalid contract\n";
            return false;
        }
        changed |= candidate.cells.size() != seed_cells ||
                   active_synapse_count(candidate) != seed_synapses;
    }
    if (!changed) {
        std::cerr << "guarded mutation did not produce a changed topology\n";
        return false;
    }
    return true;
}

static bool test_invalid_candidate_cannot_become_elite() {
    EvolutionConstraintConfig cfg;
    cfg.enable_dependency_guard = true;
    MorphogeneticEvolutionEngine engine(2, 88, cfg);
    engine.population()[0] = CellularOrganism::create_disconnected_embryo(1);
    engine.population()[1] = CellularOrganism::create_adas_seed_organism(2);
    engine.population()[0].fitness_score = 1e9;
    engine.population()[1].fitness_score = 1.0;
    engine.evolve_generation();
    if (!engine.get_champion().evaluate_adas_contract().valid()) {
        std::cerr << "invalid guarded candidate became elite\n";
        return false;
    }
    return true;
}

static bool test_invalid_candidate_skips_novelty_archive() {
    EvolutionConstraintConfig cfg;
    cfg.enable_dependency_guard = true;
    cfg.fitness_driver = FitnessDriverMode::NOVELTY_SEARCH;
    MorphogeneticEvolutionEngine engine(2, 89, cfg);
    engine.population()[0] = CellularOrganism::create_disconnected_embryo(1);
    engine.population()[1] = CellularOrganism::create_adas_seed_organism(2);
    engine.population()[0].fitness_score = 1.0;
    engine.population()[1].fitness_score = 1.0;
    engine.novelty_archive().archive.clear();
    engine.evolve_generation();
    if (engine.novelty_archive().archive.size() != 1) {
        std::cerr << "invalid candidate polluted novelty archive\n";
        return false;
    }
    return true;
}

struct EvolutionEvidence {
    CellularOrganism winner;
    AdasScenarioFitness fitness;
    size_t generation{0};
    bool found{false};
};

static EvolutionEvidence evolve_one_seed(uint32_t seed_value) {
    EvolutionConstraintConfig cfg;
    cfg.seed_mode = SeedInitMode::HANDCRAFTED_PROGENITOR;
    cfg.enable_dependency_guard = true;
    cfg.fast_mutation_rate = 0.0;
    cfg.medium_mutation_rate = 0.0;
    cfg.slow_mutation_rate = 1.0;

    MorphogeneticEvolutionEngine engine(24, seed_value, cfg);
    auto seed = CellularOrganism::create_adas_seed_organism(1);
    auto baseline = evaluate_adas_fitness(seed);
    if (!baseline.all_passed()) {
        std::cerr << "ADAS progenitor baseline failed for seed " << seed_value << "\n";
        return {};
    }
    const size_t seed_cells = seed.cells.size();
    const size_t seed_synapses = active_synapse_count(seed);

    EvolutionEvidence evidence;
    for (size_t generation = 0; generation < 20; ++generation) {
        for (auto& candidate : engine.population()) {
            candidate = seed;
            candidate.organism_id = generation + 1;
            auto fitness = evaluate_adas_fitness(candidate);
            candidate.fitness_score = fitness.score;
            candidate.total_pnl = fitness.score;
        }
        engine.evolve_generation();

        for (const auto& candidate : engine.population()) {
            if (!candidate.evaluate_adas_contract().valid() ||
                (candidate.cells.size() == seed_cells &&
                 active_synapse_count(candidate) == seed_synapses)) {
                continue;
            }
            auto checked = candidate;
            auto fitness = evaluate_adas_fitness(checked);
            if (fitness.all_passed()) {
                evidence.winner = candidate;
                evidence.fitness = fitness;
                evidence.generation = generation + 1;
                evidence.found = true;
                return evidence;
            }
        }
    }
    return evidence;
}

static void print_evidence(uint32_t seed_value, const EvolutionEvidence& evidence) {
    std::cout << "seed=" << seed_value
              << ", generation=" << evidence.generation
              << ", cells=" << evidence.winner.cells.size()
              << ", active_synapses=" << active_synapse_count(evidence.winner)
              << ", latency=" << std::fixed << std::setprecision(2)
              << evidence.fitness.latency_ns << " ns"
              << ", cost=" << evidence.fitness.cost << "\n";
    std::cout << "  metrics: curve_max=" << evidence.fitness.metric[0]
              << ", AEB_gap=" << evidence.fitness.metric[1]
              << ", lane_overshoot=" << evidence.fitness.metric[2]
              << ", follow_gap=" << evidence.fitness.metric[3]
              << ", merge_speed=" << evidence.fitness.metric[4]
              << ", swerve_clearance=" << evidence.fitness.metric[5] << "\n";
}

static bool test_negative_controls_and_held_out(const CellularOrganism& winner) {
    auto blank = CellularOrganism::create_disconnected_embryo(9);
    AdasCellularAdapter blank_adapter(blank);
    auto blank_ctl = blank_adapter.process_perception(8.0, -4.0, 0.0, 0.8);
    if (blank_ctl.is_aeb_triggered) {
        std::cerr << "blank embryo triggered the adapter AEB bypass\n";
        return false;
    }

    auto held_out = winner;
    AdasCellularAdapter perturbed(held_out);
    perturbed.get_organism().reset_state(true);
    auto ctl = perturbed.process_perception(26.0, -5.0, 0.12, 0.9);
    if (!ctl.is_aeb_triggered || ctl.target_accel_mps2 > -5.5) {
        std::cerr << "held-out AEB perturbation failed\n";
        return false;
    }
    return true;
}

int main() {
    if (!test_contract_derivation() ||
        !test_contract_binds_ttc_to_immune_path() ||
        !test_guarded_mutation() ||
        !test_invalid_candidate_cannot_become_elite() ||
        !test_invalid_candidate_skips_novelty_archive()) {
        return 1;
    }

    EvolutionEvidence first;
    for (uint32_t seed_value : {20260901u, 20260917u}) {
        auto evidence = evolve_one_seed(seed_value);
        if (!evidence.found ||
            !evidence.winner.evaluate_adas_contract().valid() ||
            !evidence.fitness.all_passed()) {
            std::cerr << "no valid changed descendant found for seed "
                      << seed_value << "\n";
            return 1;
        }
        print_evidence(seed_value, evidence);
        if (first.generation == 0) first = evidence;
    }
    if (!test_negative_controls_and_held_out(first.winner)) return 1;
    std::cout << "ADAS evolution: structurally changed descendants passed all gates\n";
    return 0;
}
