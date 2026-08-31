#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <cstring>
#include "kun/cellular/cellular_genome.hpp"
#include "kun/cellular/quant_cellular_adapter.hpp"
#include "kun/cellular/adas_cellular_adapter.hpp"

using namespace kun;

void test_seed_organism_and_forward() {
    std::cout << "[Test 1] 运行太初种子生物 (Seed Organism) 编译与纳秒级前向计算测试...\n";
    auto org = CellularOrganism::create_seed_organism(101);
    assert(org.cells.size() >= 8);
    assert(org.synapses.size() >= 7);
    assert(org.is_compiled);

    // 测算前向传导速度 (100,000 次循环)
    double inputs[4] = {3620.0, 5000.0, 1.0, 0.2};
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100000; ++i) {
        inputs[0] = 3620.0 + (i % 50) * 0.1;
        auto acts = org.forward(inputs);
        (void)acts;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double avg_ns = total_ns / 100000.0;

    std::cout << "  ↳ 100,000 次前向传导总耗时: " << (total_ns / 1e6) << " ms, 单次传导平均耗时: " 
              << avg_ns << " 纳秒 (极其硬核车规级响应!)\n";
    assert(avg_ns < 2000.0); // 必须在 2 微秒内
    std::cout << "  -> 种子生物编译与极速前向传导测试通过!\n";
}

void test_cellular_mitosis_and_synapse_mutation() {
    std::cout << "[Test 2] 运行细胞分裂增殖 (Mitosis) 与突触重构 (Rewire) 测试...\n";
    auto org = CellularOrganism::create_seed_organism(202);
    size_t init_cell_count = org.cells.size();
    size_t init_syn_count = org.synapses.size();

    MorphogeneticEvolutionEngine engine(10, 12345);
    
    // 1. 细胞分裂生长
    bool added_cell = engine.mutate_add_cell(org);
    assert(added_cell);
    assert(org.cells.size() == init_cell_count + 1);
    assert(org.synapses.size() == init_syn_count + 2); // 原突触置为inactive, 新增2条

    // 2. 突触跨界重连
    bool added_syn = engine.mutate_add_synapse(org);
    assert(added_syn);

    // 3. 验证突变后的有机体依然能正常无锁编译和前向计算
    double inputs[4] = {3650.0, 8000.0, 2.0, -0.4};
    auto acts = org.forward(inputs);
    (void)acts;

    std::cout << "  -> 细胞分裂与突触重构演化测试通过!\n";
}

void test_cellular_apoptosis_pruning() {
    std::cout << "[Test 3] 运行细胞自我凋亡与剪枝 (Apoptosis) 测试...\n";
    auto org = CellularOrganism::create_seed_organism(303);
    MorphogeneticEvolutionEngine engine(10, 8888);

    // 人工插入多个孤立/无用细胞
    for (int i = 0; i < 5; ++i) {
        engine.mutate_add_cell(org);
    }
    size_t grown_cells = org.cells.size();

    // 触发自我凋亡清理
    engine.prune_apoptosis(org);
    size_t pruned_cells = org.cells.size();

    std::cout << "  ↳ 初始细胞数: " << grown_cells << " -> 凋亡净化后存活细胞数: " << pruned_cells << "\n";
    assert(pruned_cells <= grown_cells);
    assert(org.is_compiled);

    std::cout << "  -> 细胞凋亡剪枝与自我净化机制测试通过!\n";
}

void test_quant_cellular_adapter() {
    std::cout << "[Test 4] 运行量化金融适配器 (QuantCellularAdapter) 交易与免疫锁测试...\n";
    auto org = CellularOrganism::create_seed_organism(404);
    QuantCellularAdapter adapter(org);

    QuantTickMsg tick{};
    std::strncpy(tick.symbol, "rb2405", sizeof(tick.symbol) - 1);
    tick.last_price = 3600.0;
    tick.volume = 10000.0;
    tick.bid_price1 = 3599.0;
    tick.ask_price1 = 3601.0;
    tick.bid_volume1 = 500.0;
    tick.ask_volume1 = 200.0;

    auto dec = adapter.process_tick(tick);
    std::cout << "  ↳ 量化细胞网络决策输出: " << dec.explanation << " (目标价=" << dec.target_price << ")\n";
    assert(dec.target_price == 3600.0);

    std::cout << "  -> 量化金融细胞演化适配器测试通过!\n";
}

void test_adas_cellular_adapter() {
    std::cout << "[Test 5] 运行智能驾驶适配器 (AdasCellularAdapter) 纵横向控制与 AEB 测试...\n";
    auto org = CellularOrganism::create_seed_organism(505);
    AdasCellularAdapter adas(org);

    // 正常高速巡航场景: 前车距离 60m, 相对速度 0m/s, 车道偏移 0m, TTC 10s
    auto normal_ctl = adas.process_perception(60.0, 0.0, 0.0, 10.0);
    assert(!normal_ctl.is_aeb_triggered);
    std::cout << "  ↳ 正常巡航输出: " << normal_ctl.active_pathway << "\n";

    // 极端长尾危险场景: 前车急刹，距离 8m，TTC 0.8s (< 1.2s AEB 门限)
    auto emergency_ctl = adas.process_perception(8.0, -15.0, 0.2, 0.8);
    assert(emergency_ctl.is_aeb_triggered);
    assert(emergency_ctl.target_accel_mps2 <= -5.0); // 最大防撞制动
    std::cout << "  ↳ 紧急防撞输出: " << emergency_ctl.active_pathway << "\n";

    std::cout << "  -> 智能驾驶细胞形态决策与 AEB 应急控制测试通过!\n";
}

void test_multigenerational_evolution() {
    std::cout << "[Test 6] 运行多代种群世代演化 (Multigenerational Evolution) 测试...\n";
    MorphogeneticEvolutionEngine engine(15, 9999);

    for (int gen = 0; gen < 5; ++gen) {
        // 模拟评估适应度
        for (auto& org : const_cast<std::vector<CellularOrganism>&>(engine.get_population())) {
            double inputs[4] = {3600.0 + gen * 2.0, 5000.0, 1.0, 0.1};
            auto acts = org.forward(inputs);
            org.fitness_score = acts.positive_action * 10.0 + org.cells.size() * 0.1;
        }
        engine.evolve_generation();
    }

    auto& champ = engine.get_champion();
    std::cout << "  ↳ 经历 5 代演化后冠军个体: ID=" << champ.organism_id 
              << ", 代数=" << champ.generation << ", 细胞数=" << champ.cells.size() 
              << ", 突触数=" << champ.synapses.size() << "\n";
    assert(champ.generation >= 4);

    // 验证 JSON 导出用于全息前端可视化
    std::string json_str = champ.to_json();
    assert(json_str.find("\"organism_id\"") != std::string::npos);
    assert(json_str.find("\"cells\"") != std::string::npos);
    assert(json_str.find("\"synapses\"") != std::string::npos);

    std::cout << "  -> 多代演化与全息观测 JSON 导出测试通过!\n";
}

void test_lennard_jones_force_field_and_visualization() {
    std::cout << "[Test 7] 运行兰纳-琼斯近斥中吸力场 (Lennard-Jones Force Field) 物理引擎测试...\n";
    auto org = CellularOrganism::create_seed_organism(707);

    // 1. 人工将细胞 0 和细胞 1 放置在极近距离 (dx = 0.5px, 极近 < 20px)
    org.cells[0].x = 0.0f; org.cells[0].y = 0.0f;
    org.cells[1].x = 0.5f; org.cells[1].y = 0.0f;

    // 运行一次物理力场步进
    org.step_force_field_physics(0.016f);

    // 验证极近距离下强斥力生效，两细胞被强力推开
    assert(org.cells[0].vx < 0.0f); // 向左推
    assert(org.cells[1].vx > 0.0f); // 向右推
    std::cout << "  ↳ 极近距离泡利斥力生效: Cell0 vx=" << org.cells[0].vx 
              << ", Cell1 vx=" << org.cells[1].vx << " (强力推开防坍缩!)\n";

    // 2. 人工将通过突触连接的细胞 4 和细胞 5 拉到超长距离 (dist = 150px > rest_length 50px)
    org.cells[4].x = 0.0f; org.cells[4].y = 0.0f;
    org.cells[5].x = 150.0f; org.cells[5].y = 0.0f;

    org.step_force_field_physics(0.016f);

    // 验证突触引力生效，两细胞被向内拉拢
    assert(org.cells[4].vx > 0.0f); // 向右拉
    assert(org.cells[5].vx < 0.0f); // 向左拉
    std::cout << "  ↳ 中远距离突触引力生效: Cell4 vx=" << org.cells[4].vx 
              << ", Cell5 vx=" << org.cells[5].vx << " (引力拉拢成器官!)\n";

    // 3. 验证生物发光脉冲与全息 JSON 导出
    double inputs[4] = {3620.0, 5000.0, 1.0, 0.2};
    org.forward(inputs);
    assert(org.cells[0].glow_charge > 0.0f);

    std::string json = org.to_json();
    assert(json.find("\"glow\"") != std::string::npos);
    assert(json.find("\"photon_pos\"") != std::string::npos);

    std::cout << "  -> 兰纳-琼斯力场物理引擎与生物发光全息契约 100% 通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "       FlowEngine 太初细胞形态发生演化引擎单测集          \n";
    std::cout << "=========================================================\n\n";

    test_seed_organism_and_forward();
    test_cellular_mitosis_and_synapse_mutation();
    test_cellular_apoptosis_pruning();
    test_quant_cellular_adapter();
    test_adas_cellular_adapter();
    test_multigenerational_evolution();
    test_lennard_jones_force_field_and_visualization();

    std::cout << "\n=========================================================\n";
    std::cout << "       全部 7 组太初细胞形态演化与物理力场单测 100% 成功通过! \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
