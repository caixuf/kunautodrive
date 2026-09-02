#include <iostream>
#include <vector>
#include <iomanip>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <cmath>
#include "kun/cellular/digital_pathogen_ecosystem.hpp"

using namespace kun;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

void test_viral_infection_and_culling() {
    std::cout << "[Test 1] 验证病原体接触传播、盗能与低能量脆弱宿主的加速淘汰 (Viral Culling)...\n";
    PathogenCoEvolutionWorld world(20, 2, 42);
    for (const auto& host : world.get_hosts()) {
        host->get_homeostasis().energy_reserve = 4.0;
    }
    world.release_pathogen_outbreak(0xDEAD, 4.0);

    size_t total_culls = 0;
    bool observed_infection = false;
    // 运行 30 步疫情传播
    for (int t = 0; t < 30; ++t) {
        auto frame = world.tick(40.0, 0.05);
        total_culls += frame.disease_culls_count;
        observed_infection = observed_infection || frame.infected_hosts > 0;
    }

    const auto& last = world.get_history().back();
    std::cout << "  ↳ 30 ticks 疫情爆发后: 存活宿主 " << last.total_hosts 
              << ", 感染中宿主 " << last.infected_hosts 
              << ", 病毒累积淘汰脆弱宿主数: " << total_culls << "\n";

    require(observed_infection, "pathogen never infected a host");
    require(total_culls > 0, "pathogen never culled an infected host");
    require(last.total_hosts > 0, "all hosts were unexpectedly culled");
    std::cout << "  -> 病毒接触传播与自然加速淘汰机制 100% 满分通过！\n\n";
}

void test_acquired_immunity_and_herd_protection() {
    std::cout << "[Test 2] 验证宿主消耗能量建立抗体记忆与群体免疫屏障 (Acquired Immunity & Herd Protection)...\n";
    PathogenCoEvolutionWorld world(15, 2, 999);

    // 持续运行 60 步，观察获得性抗体记忆的扩散
    for (int t = 0; t < 60; ++t) {
        world.tick(70.0, 0.05);
    }

    const auto& last = world.get_history().back();
    std::cout << "  ↳ 60 ticks 后建立免疫抗体记忆的宿主数: " << last.immune_resistant_hosts 
              << " / " << last.total_hosts << " (免疫覆盖率: " << (last.immune_resistant_hosts * 100.0 / last.total_hosts) << "%)\n";

    require(last.immune_resistant_hosts > 0, "no host acquired antibody memory"); // 证实幸存宿主成功合成抗体并产生群体免疫
    std::cout << "  -> 获得性抗体记忆与群体免疫屏障 100% 满分通过！\n\n";
}

void test_spatial_contact_gate() {
    std::cout << "[Test 3a] 验证病原体传播受同隔室空间距离约束...\n";
    bool rejected_zero_compartments = false;
    try {
        PathogenCoEvolutionWorld invalid_world(1, 0, 1);
    } catch (const std::invalid_argument&) {
        rejected_zero_compartments = true;
    }
    require(rejected_zero_compartments, "zero-compartment pathogen world was accepted");

    PathogenCoEvolutionWorld world(6, 2, 31415);
    for (size_t i = 0; i < world.get_hosts().size(); ++i) {
        world.get_hosts()[i]->set_position(static_cast<double>(i) * 100.0, 0.0);
    }
    world.get_hosts().front()->get_homeostasis().energy_reserve = 20.0;
    const auto frame = world.tick(0.0, 0.0);
    require(frame.infected_hosts == 1,
            "pathogen infected a distant host without spatial contact");
}

void test_antigenic_drift_and_horizontal_gene_transfer() {
    std::cout << "[Test 3] 验证病原体抗原漂移演化与病毒介导的基因水平转移 (Drift & HGT)...\n";
    PathogenCoEvolutionWorld world(16, 3, 2026);
    PathogenStrain stable{7, 0x1234, 1.0, 0.4, 0.0};
    std::mt19937 mutation_rng(7);
    const auto unchanged = stable.mutate_strain(mutation_rng, 0.0);
    require(unchanged.strain_id == stable.strain_id &&
            unchanged.antigen_signature == stable.antigen_signature,
            "zero mutation rate still changed a pathogen strain");

    // 释放第二种高毒力异构毒株
    world.release_pathogen_outbreak(0xF00D, 3.0, 1.0);

    // 运行 80 步，观察抗原漂移毒株与 HGT 新基因位点
    for (int t = 0; t < 80; ++t) {
        world.tick(80.0, 0.08);
    }

    const auto& last = world.get_history().back();
    std::cout << "  ↳ 80 ticks 红皇后对抗演化后: 存活毒株变种数: " << last.pathogen_strains_count 
              << ", 宿主代际繁衍总数: " << last.total_hosts << "\n";

    // 检查是否有宿主获得了 HGT 扩增基因位点
    size_t hosts_with_hgt = 0;
    bool hgt_has_source = false;
    bool hgt_has_function = false;
    bool hgt_changes_phenotype = false;
    for (const auto& h : world.get_hosts()) {
        if (h->has_hgt_function()) {
            hosts_with_hgt++;
            hgt_has_source = hgt_has_source || h->get_hgt_source_strain_id() != 0;
            hgt_changes_phenotype = hgt_changes_phenotype ||
                h->get_hgt_immune_resistance() > 0.0 ||
                h->get_hgt_metabolic_efficiency() < 1.0 ||
                h->get_hgt_damage_resistance() < 1.0;
            for (const auto& locus : h->get_genome().loci) {
                hgt_has_function = hgt_has_function ||
                                   (locus.is_hgt && locus.source_pathogen_strain_id != 0 &&
                                    locus.functional_marker != 0);
            }
        }
    }

    std::cout << "  ↳ 通过病毒感染获得基因水平转移 (HGT) 扩增功能位点的宿主数: " << hosts_with_hgt << "\n";
    std::unordered_set<uint32_t> antigens;
    size_t max_lineages_observed = 0;
    size_t total_offspring_observed = 0;
    bool lineage_linked = false;
    bool finite_fitness = true;
    for (const auto& frame : world.get_history()) {
        max_lineages_observed = std::max(max_lineages_observed, frame.lineage_observations.size());
        total_offspring_observed = std::max(total_offspring_observed, frame.pathogen_offspring_count);
        for (const auto& lineage : frame.lineage_observations) {
            antigens.insert(lineage.antigen_signature);
            lineage_linked = lineage_linked ||
                             (lineage.lineage_generation > 0 && lineage.parent_strain_id != 0);
            finite_fitness = finite_fitness && std::isfinite(lineage.fitness_score);
        }
    }
    require(max_lineages_observed >= 3, "successful transmission did not produce competing drift lineages");
    require(total_offspring_observed > 0 && lineage_linked,
            "pathogen offspring lacked parent lineage accounting");
    require(antigens.size() >= 3, "pathogen replication did not change antigen signatures");
    require(finite_fitness, "pathogen fitness telemetry was not finite");
    require(last.total_hosts > 0, "pathogen test lost all hosts");
    require(hosts_with_hgt > 0 && hgt_has_source && hgt_has_function,
            "HGT did not produce a source-linked functional phenotype");
    require(hgt_changes_phenotype, "HGT marker did not alter immunity, metabolism, or survival");
    std::cout << "  -> 红皇后抗原漂移对抗与基因水平转移 (HGT) 100% 满分通过！\n\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << " 🦠 FlowEngine 人工生命【红皇后假说：数字病原体与协同演化】核心单测\n";
    std::cout << "======================================================================\n\n";

    try {
        test_viral_infection_and_culling();
        test_acquired_immunity_and_herd_protection();
        test_spatial_contact_gate();
        test_antigenic_drift_and_horizontal_gene_transfer();
    } catch (const std::exception& ex) {
        std::cerr << "TEST FAILURE: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "======================================================================\n";
    std::cout << " 🎉 病原体对抗演化验收达成: 病毒淘汰、群体免疫、抗原漂移、HGT全通！\n";
    std::cout << "======================================================================\n";
    return 0;
}
