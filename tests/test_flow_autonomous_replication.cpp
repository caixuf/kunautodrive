#include <iostream>
#include <vector>
#include <cassert>
#include <iomanip>
#include "kun/cellular/autonomous_replicator.hpp"

using namespace kun;

void test_energy_driven_autonomous_reproduction() {
    std::cout << "[Test 1] 验证内部储能盈余自发驱动的去中心化子代繁衍 (Energy-Driven Reproduction)...\n";
    DecentralizedEcologicalWorld world(10, 2, 42);

    // 运行 40 个时间步，外部提供充足营养输入
    for (int t = 0; t < 40; ++t) {
        world.tick(80.0, 0.0, 0.05);
    }

    auto hist = world.get_history();
    const auto& last = hist.back();
    std::cout << "  ↳ 40 ticks 繁衍后种群规模: " << last.total_population << " (初始 10)\n";
    std::cout << "  ↳ 累积繁衍最高代际: Gen " << last.max_generation << ", 独立遗传谱系分支数: " << last.unique_lineages_count << "\n";

    assert(last.total_population > 10); // 证实种群在能量盈余下自发展开繁衍
    assert(last.max_generation >= 1);   // 证实子代繁衍并递增世代
    std::cout << "  -> 能量驱动去中心化繁衍与代际递增 100% 满分通过！\n\n";
}

void test_carrying_capacity_and_famine_suppression() {
    std::cout << "[Test 2] 验证隔室空间容量硬上限与环境断供下的繁殖自律抑制 (Carrying Capacity & Famine)...\n";
    DecentralizedEcologicalWorld world(20, 2, 999);

    // 1. 设置极小隔室承载力 (单隔室最多 15 个个体)
    world.get_compartments()[0].carrying_capacity = 15;
    world.get_compartments()[1].carrying_capacity = 15;

    // 运行 60 步高营养输入
    for (int t = 0; t < 60; ++t) {
        world.tick(100.0, 0.0, 0.05);
    }

    size_t pop_before_famine = world.get_history().back().total_population;
    std::cout << "  ↳ 达到环境承载力上限时种群总数: " << pop_before_famine << " (理论两隔室上限 30)\n";
    assert(pop_before_famine <= 30); // 证实严格受控于空间承载力，不发生无序膨胀

    // 2. 切断营养进入饥荒 80 步
    for (int t = 0; t < 80; ++t) {
        world.tick(0.0, 0.0, 0.05); // 零输入
    }

    auto hist_famine = world.get_history().back();
    std::cout << "  ↳ 饥荒 80 ticks 后种群总数自发回落至: " << hist_famine.total_population << ", 出生数降至: " << hist_famine.total_births << "\n";
    assert(hist_famine.total_births == 0); // 证实饥荒期个体储能不足，繁殖自发完全停滞
    std::cout << "  -> 空间承载力硬阻滞与饥荒自律停育 100% 满分通过！\n\n";
}

void test_intergenerational_mutation_and_phylogeny_tree() {
    std::cout << "[Test 3] 验证多代遗传中的自发变异与多样性谱系树扩张 (Phylogeny & Mutation)...\n";
    DecentralizedEcologicalWorld world(8, 4, 1234);

    // 运行 80 步，维持适度变异率 0.08
    for (int t = 0; t < 80; ++t) {
        world.tick(70.0, 0.0, 0.08);
    }

    const auto& last = world.get_history().back();
    std::cout << "  ↳ 80 ticks 演化后最高世代: Gen " << last.max_generation 
              << ", 异构谱系哈希数: " << last.unique_lineages_count 
              << ", 存活总个体数: " << last.total_population << "\n";

    assert(last.max_generation >= 2);
    assert(last.unique_lineages_count >= 2); // 证实内生变异成功演化出多个异构谱系
    std::cout << "  -> 多代变异累积与谱系树分支扩张 100% 满分通过！\n\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << " 🧬 FlowEngine 人工生命【第二阶段：自主复制与代际遗传】核心单测\n";
    std::cout << "======================================================================\n\n";

    test_energy_driven_autonomous_reproduction();
    test_carrying_capacity_and_famine_suppression();
    test_intergenerational_mutation_and_phylogeny_tree();

    std::cout << "======================================================================\n";
    std::cout << " 🎉 自主复制第二阶段验收达成: 能量驱动繁殖、空间容量阻滞、谱系树全通！\n";
    std::cout << "======================================================================\n";
    return 0;
}
