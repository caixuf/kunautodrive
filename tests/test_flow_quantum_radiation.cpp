#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include "kun/cellular/quantum_radiation_field.hpp"

using namespace kun;

void test_wavefield_interference() {
    std::cout << "[Test 1] 运行空间量子波函数与干涉条纹强度计算测试...\n";
    QuantumRadiationField qfield(1234);

    float i_peak = qfield.evaluate_intensity(0.0f, 0.0f, 0.0f, 0.0f);
    float i_valley = qfield.evaluate_intensity(20.0f, 30.0f, 10.0f, 1.5f);

    std::cout << "  ↳ 原点处干涉强度: " << i_peak << "\n";
    std::cout << "  ↳ 空间干涉相消谷值: " << i_valley << "\n";

    assert(i_peak >= 0.0f);
    assert(i_valley >= 0.0f);
    assert(i_peak != i_valley); // 空间干涉分布非均匀

    std::cout << "  -> 空间量子波函数干涉计算测试 100% 通过!\n";
}

void test_cosmic_ray_propagation_and_strikes() {
    std::cout << "[Test 2] 运行离散宇宙射线粒子束生成与飞行推进测试...\n";
    QuantumRadiationField qfield(5678);

    // 运行 30 步以产生粒子束
    for (int i = 0; i < 30; ++i) {
        qfield.step(0.05f);
    }

    const auto& rays = qfield.cosmic_rays();
    std::cout << "  ↳ 空间活跃宇宙射线粒子束数: " << rays.size() << "\n";
    assert(!rays.empty());
    assert(rays[0].energy >= 30.0f);
    assert(rays[0].current_dist > 0.0f);

    std::cout << "  -> 宇宙射线生成与空间推进测试 100% 通过!\n";
}

void test_radiation_induced_mutagenesis() {
    std::cout << "[Test 3] 运行高能辐射软电离微突变与硬电离原语突变测试...\n";
    QuantumRadiationField qfield(9999);
    auto org = CellularOrganism::create_seed_organism(777);

    double orig_w0 = org.synapses[0].weight;

    // 暴露于辐射场 20 次
    for (int i = 0; i < 20; ++i) {
        qfield.step(0.1f);
        qfield.irradiate_organism(org, 0.0f, 0.0f, 0.0f, 10);
    }

    // 验证经过辐射照射后突触可塑性与结构保持有效编译
    double inputs[4] = {3600.0, 100.0, 1.0, 0.05};
    auto acts = org.forward(inputs);

    std::cout << "  ↳ 初始突触权重: " << orig_w0 << " -> 辐射诱变后权重: " << org.synapses[0].weight << "\n";
    std::cout << "  ↳ 辐射后前向激活动作: 买=" << acts.positive_action << ", 卖=" << acts.negative_action << "\n";

    assert(org.is_compiled());
    assert(std::isfinite(acts.positive_action));
    assert(std::isfinite(acts.negative_action));

    std::cout << "  -> 辐射诱变与前向可塑性计算测试 100% 通过!\n";
}

void test_quantum_tunneling_plateau_breakthrough() {
    std::cout << "[Test 4] 运行量子隧穿机制 (Quantum Tunneling) 跳出停滞势垒测试...\n";
    QuantumRadiationField qfield(8888);
    auto org = CellularOrganism::create_seed_organism(999);
    size_t orig_cell_count = org.cells.size();

    // 模拟适应度长时间停滞 (stagnation_ticks = 100) 并处于高干涉强辐射区
    bool tunnel_triggered = false;
    for (int i = 0; i < 50; ++i) {
        qfield.step(0.1f);
        qfield.irradiate_organism(org, 0.0f, 0.0f, 0.0f, 100);
        for (const auto& ev : qfield.recent_events()) {
            if (ev.mutation_type == "QUANTUM_TUNNELING") {
                tunnel_triggered = true;
                break;
            }
        }
        if (tunnel_triggered) break;
    }

    std::cout << "  ↳ 量子隧穿跃迁状态: " << (tunnel_triggered ? "成功穿透势垒" : "未触发") << "\n";
    assert(tunnel_triggered);
    assert(org.cells.size() > orig_cell_count); // 隧穿分裂新细胞

    double inputs[4] = {3600.0, 100.0, 1.0, 0.05};
    auto acts = org.forward(inputs);
    assert(std::isfinite(acts.positive_action));

    std::cout << "  -> 量子隧穿跳出停滞势垒测试 100% 通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "   量子波粒辐射与诱变引擎 (QuantumRadiation) 单元测试集   \n";
    std::cout << "=========================================================\n\n";

    test_wavefield_interference();
    test_cosmic_ray_propagation_and_strikes();
    test_radiation_induced_mutagenesis();
    test_quantum_tunneling_plateau_breakthrough();

    std::cout << "\n=========================================================\n";
    std::cout << "   量子辐射与波粒诱变全部 4 组单元测试 100% 满分通过!    \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
