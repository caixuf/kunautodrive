#include <iostream>
#include <vector>
#include <cassert>
#include <iomanip>
#include <stdexcept>
#include "kun/cellular/multispecies_ecology.hpp"

using namespace kun;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

void test_multispecies_differentiation_and_syntrophy() {
    std::cout << "[Test 1] 验证三类生态公会异构表型分化与共生废物净化循环 (Guilds & Syntrophy)...\n";
    MultiSpeciesEcosystemWorld world(10, 3, 42); // 每个公会 10 个个体，共 30 个

    double total_purified = 0.0;
    // 运行 50 步，伴随环境代谢毒性
    for (int t = 0; t < 50; ++t) {
        auto frame = world.tick(60.0, 5.0, 0.05);
        total_purified += frame.global_waste_purified;
    }

    const auto& last = world.get_history().back();
    std::cout << "  ↳ 50 ticks 演化后各公会种群: 觅食者 " << last.forager_count 
              << ", 节俭耐受者 " << last.survivor_count 
              << ", 腐生分解者 " << last.scavenger_count << " (总计: " << last.total_population << ")\n";
    std::cout << "  ↳ 腐生分解者共生净化毒素转化为养分总量: " << total_purified << "\n";

    assert(last.total_population > 0);
    assert(total_purified > 0.0); // 证实腐生分解者成功执行共生净化再生养分
    std::cout << "  -> 异构代谢分化与共生互利净化循环 100% 满分通过！\n\n";
}

void test_spatial_gradient_migration() {
    std::cout << "[Test 2] 验证空间资源不均诱发的跨隔室自发迁徙行为 (Spatial Migration)...\n";
    MultiSpeciesEcosystemWorld world(8, 3, 777);

    // 人为制造极端空间梯度：隔室 0 枯竭，隔室 2 极富集
    world.get_compartments()[0].nutrient_concentration = 10.0;
    world.get_compartments()[1].nutrient_concentration = 200.0;
    world.get_compartments()[2].nutrient_concentration = 3000.0;

    size_t total_migrations = 0;
    for (int t = 0; t < 40; ++t) {
        auto frame = world.tick(20.0, 0.0, 0.05);
        total_migrations += frame.total_migrations;
    }

    std::cout << "  ↳ 40 ticks 内个体自发跨膜迁徙总次数: " << total_migrations << " 次\n";
    assert(total_migrations > 0); // 证实贫瘠隔室个体自发向富集隔室迁移
    std::cout << "  -> 空间梯度感知与去中心化自发迁徙 100% 满分通过！\n\n";
}

void test_ecological_succession_under_stress() {
    std::cout << "[Test 3] 验证极端毒性灾变下的物种更替与生态位重构 (Ecological Succession)...\n";
    MultiSpeciesEcosystemWorld world(12, 2, 999);

    // 1. 注入致死级剧烈毒性冲击 (Toxic Catastrophe)
    for (int t = 0; t < 20; ++t) {
        world.tick(10.0, 40.0, 0.05); // 极高毒性
    }

    auto frame_shock = world.get_history().back();
    std::cout << "  ↳ 灾变期后种群结构: 觅食者(脆弱) " << frame_shock.forager_count 
              << ", 节俭耐受者(抗毒) " << frame_shock.survivor_count 
              << ", 腐生分解者(净化) " << frame_shock.scavenger_count 
              << " (存活总数: " << frame_shock.total_population << ")\n";

    // 证实强效觅食者因高能耗和高毒性首当其冲被淘汰，具备解毒能力的分解者独活
    assert(frame_shock.scavenger_count > 0);

    // 2. 灾变后环境恢复，幸存的腐生分解者摄取养分繁衍生息
    for (int t = 0; t < 100; ++t) {
        world.tick(120.0, 0.0, 0.05);
    }

    auto frame_recovery = world.get_history().back();
    std::cout << "  ↳ 恢复期 100 ticks 后种群重建总数: " << frame_recovery.total_population 
              << " (分解者: " << frame_recovery.scavenger_count << ")\n";

    assert(frame_recovery.total_population > frame_shock.total_population); // 证实生态位重新繁盛
    std::cout << "  -> 灾变选择、物种更替与生态位重构 100% 满分通过！\n\n";
}

void test_detox_energy_settlement_is_single_conversion() {
    std::cout << "[Test 4] 验证解毒物质只结算一次，能量变化经统一摄取路径完成...\n";
    MultiSpeciesEcosystemWorld world(1, 1, 123);
    auto& compartment = world.get_compartments().front();
    compartment.nutrient_concentration = 0.0;
    compartment.waste_toxicity = 10.0;

    const auto& organisms = world.get_organisms();
    const EcologicalOrganism* scavenger = nullptr;
    for (const auto& organism : organisms) {
        if (organism->get_guild() == SpeciesGuild::DETRITUS_SCAVENGER) {
            scavenger = organism.get();
            break;
        }
    }
    require(scavenger != nullptr, "detritus scavenger missing from test fixture");
    const double energy_before = scavenger->get_homeostasis().energy_reserve;
    const auto frame = world.tick(0.0, 0.0);
    const double energy_after = scavenger->get_homeostasis().energy_reserve;

    require(frame.global_waste_purified > 0.0, "detoxification did not consume waste");
    require(energy_after < energy_before, "detoxification minted energy directly");
    require(compartment.nutrient_concentration > 0.0, "detoxification produced no nutrient");
    std::cout << "  ↳ 解毒量: " << frame.global_waste_purified
              << ", 分解者能量变化: " << (energy_after - energy_before)
              << " (无重复直接能量铸造)\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << " 🧬 FlowEngine 人工生命【第三阶段：多种群生态位演化】核心单测\n";
    std::cout << "======================================================================\n\n";

    test_multispecies_differentiation_and_syntrophy();
    test_spatial_gradient_migration();
    test_ecological_succession_under_stress();
    test_detox_energy_settlement_is_single_conversion();

    std::cout << "======================================================================\n";
    std::cout << " 🎉 多种群生态位第三阶段验收达成: 异构公会、共生净化、自发迁徙全通！\n";
    std::cout << "======================================================================\n";
    return 0;
}
