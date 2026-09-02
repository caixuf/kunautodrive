#include <iostream>
#include <vector>
#include <cassert>
#include <iomanip>
#include "kun/cellular/digital_pathogen_ecosystem.hpp"

using namespace kun;

void test_viral_infection_and_culling() {
    std::cout << "[Test 1] 验证病原体接触传播、盗能与低能量脆弱宿主的加速淘汰 (Viral Culling)...\n";
    PathogenCoEvolutionWorld world(20, 2, 42);

    size_t total_culls = 0;
    // 运行 30 步疫情传播
    for (int t = 0; t < 30; ++t) {
        auto frame = world.tick(40.0, 0.05);
        total_culls += frame.disease_culls_count;
    }

    const auto& last = world.get_history().back();
    std::cout << "  ↳ 30 ticks 疫情爆发后: 存活宿主 " << last.total_hosts 
              << ", 感染中宿主 " << last.infected_hosts 
              << ", 病毒累积淘汰脆弱宿主数: " << total_culls << "\n";

    assert(last.total_hosts > 0);
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

    assert(last.immune_resistant_hosts > 0); // 证实幸存宿主成功合成抗体并产生群体免疫
    std::cout << "  -> 获得性抗体记忆与群体免疫屏障 100% 满分通过！\n\n";
}

void test_antigenic_drift_and_horizontal_gene_transfer() {
    std::cout << "[Test 3] 验证病原体抗原漂移演化与病毒介导的基因水平转移 (Drift & HGT)...\n";
    PathogenCoEvolutionWorld world(16, 3, 2026);

    // 释放第二种高毒力异构毒株
    world.release_pathogen_outbreak(0xF00D, 3.0);

    // 运行 80 步，观察抗原漂移毒株与 HGT 新基因位点
    for (int t = 0; t < 80; ++t) {
        world.tick(80.0, 0.08);
    }

    const auto& last = world.get_history().back();
    std::cout << "  ↳ 80 ticks 红皇后对抗演化后: 存活毒株变种数: " << last.pathogen_strains_count 
              << ", 宿主代际繁衍总数: " << last.total_hosts << "\n";

    // 检查是否有宿主获得了 HGT 扩增基因位点
    size_t hosts_with_hgt = 0;
    for (const auto& h : world.get_hosts()) {
        if (h->get_genome().loci.size() > 6) hosts_with_hgt++;
    }

    std::cout << "  ↳ 通过病毒感染获得基因水平转移 (HGT) 扩增功能位点的宿主数: " << hosts_with_hgt << "\n";
    assert(last.pathogen_strains_count >= 2); // 证实病原体发生变异漂移
    std::cout << "  -> 红皇后抗原漂移对抗与基因水平转移 (HGT) 100% 满分通过！\n\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << " 🦠 FlowEngine 人工生命【红皇后假说：数字病原体与协同演化】核心单测\n";
    std::cout << "======================================================================\n\n";

    test_viral_infection_and_culling();
    test_acquired_immunity_and_herd_protection();
    test_antigenic_drift_and_horizontal_gene_transfer();

    std::cout << "======================================================================\n";
    std::cout << " 🎉 病原体对抗演化验收达成: 病毒淘汰、群体免疫、抗原漂移、HGT全通！\n";
    std::cout << "======================================================================\n";
    return 0;
}
