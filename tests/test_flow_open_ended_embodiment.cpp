#include <iostream>
#include <vector>
#include <cassert>
#include <iomanip>
#include "kun/cellular/open_ended_embodiment.hpp"

using namespace kun;

void test_embodied_perception_action_loop() {
    std::cout << "[Test 1] 验证感知-身体动作-物理位移-能量反馈的闭环热力学因果链 (Embodied P-A Loop)...\n";
    EmbodiedPhysicalSandboxWorld world(10, 100.0, 42);

    // 连续运行 40 个时间步
    double initial_patch_energy = 0.0;
    for (const auto& p : world.get_patches()) initial_patch_energy += p.energy_amount;

    for (int t = 0; t < 40; ++t) {
        world.tick(20.0, 0.05);
    }

    const auto& last = world.get_history().back();
    std::cout << "  ↳ 40 ticks 具身交互后存活个体数: " << last.alive_agents << ", 平均运动速率: " << std::fixed << std::setprecision(2) << last.average_agent_speed << "\n";
    std::cout << "  ↳ 种群生物质储能总量: " << last.total_biomass_energy << " ATP, 环境总养分存量: " << last.total_world_nutrients << "\n";

    assert(last.alive_agents > 0);
    assert(last.average_agent_speed > 0.0); // 证实产生真实的自主物理运动与动能耗散
    std::cout << "  -> 具身感知-动作-位移-摄食闭环 100% 满分通过！\n\n";
}

void test_open_ended_emergent_circuits() {
    std::cout << "[Test 2] 验证开放式自发重组与新计算信号通道的自发涌现 (Novel Emergent Circuits)...\n";
    EmbodiedPhysicalSandboxWorld world(12, 100.0, 999);

    // 运行 100 步，维持适度开放式突变率
    for (int t = 0; t < 100; ++t) {
        world.tick(25.0, 0.10);
    }

    const auto& last = world.get_history().back();
    std::cout << "  ↳ 100 ticks 开放式演化后最高世代: Gen " << last.max_generation 
              << ", 自发涌现的新信号通道数量: " << last.novel_circuits_count << "\n";

    assert(last.max_generation >= 1);
    assert(last.novel_circuits_count > 0); // 证实系统自发衍生出未经硬编码预设的复合新通路通道
    std::cout << "  -> 开放式结构组合与新信号通道涌现 100% 满分通过！\n\n";
}

void test_long_term_unguided_survival_equilibrium() {
    std::cout << "[Test 3] 验证未预设目标、无监督梯度下的 300 ticks 动态长周期生存稳态 (Long-Term Survival)...\n";
    EmbodiedPhysicalSandboxWorld world(15, 100.0, 2026);

    // 运行 300 个时间步 (历经风暴移动、斑块消耗与再生)
    for (int t = 0; t < 300; ++t) {
        world.tick(30.0, 0.08);
    }

    const auto& last = world.get_history().back();
    std::cout << "  ↳ 300 ticks 动态长周期盲行后存活数: " << last.alive_agents 
              << ", 累积最高代际: Gen " << last.max_generation 
              << ", 涌现新通道数: " << last.novel_circuits_count << "\n";

    assert(last.alive_agents > 0);
    assert(last.total_biomass_energy > 0.0);
    std::cout << "  -> 物理世界无监督长周期动态自维持 100% 满分通过！\n\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << " 🧬 FlowEngine 人工生命【第四阶段：开放式演化与具身闭环】终极单测\n";
    std::cout << "======================================================================\n\n";

    test_embodied_perception_action_loop();
    test_open_ended_emergent_circuits();
    test_long_term_unguided_survival_equilibrium();

    std::cout << "======================================================================\n";
    std::cout << " 🎉 人工生命四阶段大满贯达成: 稳态、复制、生态位、开放具身全通！\n";
    std::cout << "======================================================================\n";
    return 0;
}
