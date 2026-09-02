#include <iostream>
#include <vector>
#include <cassert>
#include <iomanip>
#include "kun/cellular/digital_homeostasis.hpp"

using namespace kun;

void test_homeostasis_baseline_metabolism() {
    std::cout << "[Test 1] 验证正常营养环境下的细胞自主代谢与储能动态平衡 (Homeostatic Equilibrium)...\n";
    DigitalHomeostasisEngine engine(100, 4, 1000.0, 50.0);

    // 运行 50 个时间步，正常营养流入
    for (int t = 0; t < 50; ++t) {
        engine.tick(40.0, 0.0);
    }

    auto hist = engine.get_history();
    const auto& last = hist.back();
    std::cout << "  ↳ 50 ticks 后存活细胞数: " << last.alive_cells << " / 100 (活跃: " << last.active_cells << ", 休眠: " << last.dormant_cells << ")\n";
    std::cout << "  ↳ 细胞群体内部总储能: " << last.total_internal_energy << " ATP, 局部环境底质余量: " << last.global_nutrient_reserve << "\n";
    
    assert(last.alive_cells == 100);
    assert(last.active_cells >= 95);
    std::cout << "  -> 正常环境代谢自维持与稳态平衡 100% 满分通过！\n\n";
}

void test_famine_induced_dormancy_and_apoptosis() {
    std::cout << "[Test 2] 验证环境断供/饥荒压力下的自发休眠保护与阶梯式凋亡 (Famine & Apoptosis)...\n";
    // 初始化极低底质 (0 外部底质，初始内部仅 12 ATP)
    DigitalHomeostasisEngine engine(50, 2, 0.0, 12.0);

    size_t dormant_observed = 0;
    size_t apoptotic_observed = 0;

    for (int t = 0; t < 500; ++t) {
        auto frame = engine.tick(0.0, 0.0); // 绝对断供饥荒
        if (frame.dormant_cells > 0) dormant_observed++;
        if (frame.apoptotic_cells > 0) apoptotic_observed++;
    }

    auto hist = engine.get_history();
    const auto& last = hist.back();
    std::cout << "  ↳ 绝对饥荒 400 ticks 后: 存活 " << last.alive_cells << ", 凋亡淘汰 " << last.apoptotic_cells 
              << ", 触发自发休眠保护 step数: " << dormant_observed << "\n";

    assert(dormant_observed > 0);       // 证实能量降低至 10 时必定触发自发休眠降低功耗
    assert(apoptotic_observed > 0);     // 证实能量彻底耗尽后细胞自发解离凋亡
    assert(last.apoptotic_cells == 50); // 最终能量完全耗尽全部可解释性死亡
    std::cout << "  -> 饥荒自发休眠与有限生命期凋亡淘汰 100% 满分通过！\n\n";
}

void test_toxicity_shock_and_autonomous_repair() {
    std::cout << "[Test 3] 验证毒性冲击、细胞损伤累积与自主稳态修复回路 (Damage & Self-Repair)...\n";
    DigitalHomeostasisEngine engine(60, 3, 500.0, 60.0);

    // 1. 正常运行 10 步
    for (int t = 0; t < 10; ++t) engine.tick(30.0, 0.0);

    // 2. 注入外部剧烈毒性冲击 (Toxic Shock) 持续 5 步
    for (int t = 0; t < 5; ++t) engine.tick(30.0, 30.0);

    // 3. 撤销毒性，恢复营养，观察系统自主执行稳态修复 (Self-Repair)
    uint32_t total_repairs = 0;
    for (int t = 0; t < 40; ++t) {
        auto frame = engine.tick(40.0, 0.0);
        total_repairs += frame.total_repairs_executed;
    }

    std::cout << "  ↳ 毒性冲击后系统自主触发自修复总次数: " << total_repairs << " 次\n";
    assert(total_repairs > 0);
    std::cout << "  -> 损伤检测、能量消耗与自主修复闭环 100% 满分通过！\n\n";
}

void test_spatial_compartment_membrane_diffusion() {
    std::cout << "[Test 4] 验证局部空间隔室、膜通透性与浓度梯度扩散平衡 (Membrane Diffusion)...\n";
    DigitalHomeostasisEngine engine(40, 2, 0.0, 50.0);

    // 将隔室 0 注入 500 营养，隔室 1 为 0
    engine.get_compartments()[0].nutrient_concentration = 500.0;
    engine.get_compartments()[1].nutrient_concentration = 0.0;

    double initial_diff = std::abs(engine.get_compartments()[0].nutrient_concentration - engine.get_compartments()[1].nutrient_concentration);

    // 连续运行 40 步跨膜扩散
    for (int t = 0; t < 40; ++t) {
        engine.tick(0.0, 0.0);
    }

    double final_diff = std::abs(engine.get_compartments()[0].nutrient_concentration - engine.get_compartments()[1].nutrient_concentration);
    std::cout << "  ↳ 初始浓度阶梯差异: " << initial_diff << " -> 40 ticks 跨膜扩散后差异缩小至: " << final_diff << "\n";
    assert(final_diff < initial_diff);
    std::cout << "  -> 局部空间隔室与跨膜扩散平衡 100% 满分通过！\n\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << " 🧬 FlowEngine 人工生命【第一阶段：数字稳态引擎】核心单测\n";
    std::cout << "======================================================================\n\n";

    test_homeostasis_baseline_metabolism();
    test_famine_induced_dormancy_and_apoptosis();
    test_toxicity_shock_and_autonomous_repair();
    test_spatial_compartment_membrane_diffusion();

    std::cout << "======================================================================\n";
    std::cout << " 🎉 数字稳态第一阶段验收达成: 有限能量、自发休眠、损伤自修复全通！\n";
    std::cout << "======================================================================\n";
    return 0;
}
