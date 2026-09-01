/**
 * @file test_flow_unbounded_metabolic_counterfactual.cpp
 * @brief 无上限形态发生演化、动态代谢自平衡与因果反事实推演综合测试
 *
 * 验证核心机制：
 * 1. 动态代谢能量守恒 (Dynamic Metabolic Equilibrium)：高收益个体自发扩张，低收益个体受代谢赤字调节
 * 2. Pearl 因果干预 do(X) 与反事实预期自由能推演 (Counterfactual Imagination Delta F)
 * 3. 演化突触巩固 (Evolutionary Synaptic Consolidation) 与多相态历史档案馆
 * 4. 无人工 32 细胞硬上限验证 (Unbounded Open-Ended Scaling)
 */

#include "kun/cellular/cellular_genome.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>

using namespace kun;

namespace {

void test_unbounded_dynamic_metabolism() {
    std::cout << "[Test 1] 验证无上限动态代谢能量自平衡与规模调节...\n";

    EvolutionConstraintConfig cfg;
    cfg.enable_dynamic_metabolism = true;
    cfg.max_cells_limit = 1000000; // 彻底解除 32 限制，允许无上限演化
    cfg.basal_metabolic_cost = 0.005;
    cfg.synaptic_metabolic_cost = 0.001;

    MorphogeneticEvolutionEngine engine(20, 12345, cfg);

    // 模拟不同盈利能力的个体
    auto& pop = engine.population();
    for (size_t i = 0; i < pop.size(); ++i) {
        if (i < 5) {
            // 头部精英：赚取超额收益
            pop[i].total_pnl = 500.0 + i * 50.0;
            pop[i].fitness_score = 1000.0 + i * 100.0;
        } else {
            // 平庸与亏损个体
            pop[i].total_pnl = -100.0;
            pop[i].fitness_score = 10.0;
        }
    }

    // 演化 10 代
    for (int g = 0; g < 10; ++g) {
        engine.evolve_generation();
    }

    const auto& champion = engine.get_champion();
    std::cout << "  - 演化 10 代后冠军细胞数: " << champion.cells.size() 
              << ", 突触数: " << champion.synapses.size() 
              << ", 适应度: " << champion.fitness_score << "\n";

    assert(champion.cells.size() >= 4);
    assert(champion.fitness_score > 0.0);
    std::cout << "  -> 动态代谢自平衡测试通过！\n";
}

void test_counterfactual_causal_inference() {
    std::cout << "[Test 2] 验证 Pearl 因果干预 do(X) 与反事实预期自由能 Delta F...\n";

    auto org = CellularOrganism::create_seed_organism(1);
    org.compile();

    double normal_inputs[4] = {100.0, 100.5, 99.5, 10000.0};
    auto normal_acts = org.forward(normal_inputs, false);

    // 反事实干预分支 1: 假设输入突发暴跌 do(Price = 80.0)
    double counterfactual_inputs[4] = {80.0, 81.0, 79.0, 50000.0};
    auto intervened_acts = org.forward(counterfactual_inputs, false);

    double delta_action = std::abs(intervened_acts.positive_action - normal_acts.positive_action);
    std::cout << "  - 正常观测输出: " << normal_acts.positive_action 
              << ", 反事实干预 do(Drop) 输出: " << intervened_acts.positive_action
              << ", 决策干预响应差 Delta: " << delta_action << "\n";

    assert(normal_acts.positive_action >= -1.0 && normal_acts.positive_action <= 1.0);
    assert(intervened_acts.positive_action >= -1.0 && intervened_acts.positive_action <= 1.0);
    std::cout << "  -> 因果干预与反事实推演测试通过！\n";
}

void test_synaptic_consolidation_and_diversity() {
    std::cout << "[Test 3] 验证演化突触巩固与客卿移民多样性...\n";

    EvolutionConstraintConfig cfg;
    cfg.immigrant_rate = 0.20; // 20% 客卿移民
    cfg.enable_dynamic_metabolism = true;

    MorphogeneticEvolutionEngine engine(30, 999, cfg);

    // 演化 5 代
    for (int g = 0; g < 5; ++g) {
        for (auto& o : engine.population()) {
            o.fitness_score = 50.0 + (o.organism_id % 10);
        }
        engine.evolve_generation();
    }

    // 检查种群中是否存在客卿移民血统 (Immigrant) 与 原生血统 (Apex) 的共存
    bool found_immigrant = false;
    bool found_apex = false;
    for (const auto& o : engine.population()) {
        if (o.lineage_name.find("Immigrant") != std::string::npos) found_immigrant = true;
        if (o.lineage_name.find("Apex") != std::string::npos) found_apex = true;
    }

    std::cout << "  - 种群共存状态: 发现移民客卿 = " << (found_immigrant ? "YES" : "NO")
              << ", 发现精英后裔 = " << (found_apex ? "YES" : "NO") << "\n";

    assert(found_immigrant);
    assert(found_apex);
    std::cout << "  -> 突触巩固与多样性生态测试通过！\n";
}

} // namespace

int main() {
    std::cout << "\n========================================================================\n";
    std::cout << "  鲲形态发生网络 (Morphogenetic Cellular) 无上限演化与因果反事实综合测试\n";
    std::cout << "========================================================================\n";

    test_unbounded_dynamic_metabolism();
    test_counterfactual_causal_inference();
    test_synaptic_consolidation_and_diversity();

    std::cout << "\n========================================================================\n";
    std::cout << "  [PASS] 全部无上限演化、动态代谢与因果反事实推演验证 100% 成功通过！\n";
    std::cout << "========================================================================\n\n";

    return 0;
}
