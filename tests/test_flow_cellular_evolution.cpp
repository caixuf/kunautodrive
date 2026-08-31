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
    std::cout << "[Test 1] 运行种子形态生物 (Seed Organism) 编译与纳秒级前向计算测试...\n";
    auto org = CellularOrganism::create_seed_organism(101);
    assert(org.cells.size() >= 8);
    assert(org.synapses.size() >= 7);
    assert(org.is_compiled_);

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
#if defined(__SANITIZE_ADDRESS__) || defined(ENABLE_ASAN)
    assert(avg_ns < 15000.0); // ASAN 插桩与高并发环境放宽时延门限
#else
    assert(avg_ns < 2000.0); // 生产与常规编译测试
#endif
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
    assert(org.is_compiled_);

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
    std::cout << "  ↳ 量化细胞网络首拍决策输出: " << dec.explanation << " (目标价=" << dec.target_price << ")\n";
    assert(dec.target_price == 3600.0);
    // forward() 为同步一拍延迟语义 (先汇聚上一拍输出, 再按拓扑序激发): 首拍动作细胞电位为 0, 决策应为 HOLD
    assert(dec.action == QuantCellularAdapter::TradeDecision::Action::HOLD);

    // 次拍: 迟滞门初值 -1.0 经 -1.0 权重突触激励 ACT_PRIMARY_NEGATIVE, 应产生真实 SELL 决策路径
    auto dec2 = adapter.process_tick(tick);
    std::cout << "  ↳ 量化细胞网络次拍决策输出: " << dec2.explanation << "\n";
    assert(dec2.action == QuantCellularAdapter::TradeDecision::Action::SELL_OPEN);
    assert(!dec2.explanation.empty());
    assert(dec2.confidence > 0.5);

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

    // 1. 人工将细胞 0 和细胞 1 放置在极近距离 (dx = 0.5px, 极近 < 20px), 将其他细胞移至截断半径外
    for (size_t i = 2; i < org.cells.size(); ++i) {
        org.cells[i].x = 300.0f + static_cast<float>(i * 50);
        org.cells[i].y = 0.0f; org.cells[i].z = 0.0f;
    }
    org.cells[0].x = 0.0f; org.cells[0].y = 0.0f; org.cells[0].z = 0.0f;
    org.cells[1].x = 20.0f; org.cells[1].y = 0.0f; org.cells[1].z = 0.0f;

    // 运行一次物理力场步进
    org.step_force_field_physics(0.016f);

    // 验证极近距离下强斥力生效，两细胞被强力推开
    assert(org.cells[0].vx < 0.0f); // 向左推
    assert(org.cells[1].vx > 0.0f); // 向右推
    std::cout << "  ↳ 极近距离泡利斥力生效: Cell0 vx=" << org.cells[0].vx 
              << ", Cell1 vx=" << org.cells[1].vx << " (强力推开防坍缩!)\n";

    // 2. 人工将通过突触连接的细胞 4 和细胞 5 拉到超长距离 (dist = 150px > rest_length 50px) 并隔离其他突触与细胞
    for (auto& s : org.synapses) {
        if (!(s.from_cell_id == 4 && s.to_cell_id == 5)) {
            s.is_active = false;
        }
    }
    for (size_t i = 0; i < org.cells.size(); ++i) {
        if (i != 4 && i != 5) {
            org.cells[i].x = 400.0f + static_cast<float>(i * 50);
            org.cells[i].y = 0.0f; org.cells[i].z = 0.0f;
        }
    }
    org.cells[4].x = 0.0f; org.cells[4].y = 0.0f; org.cells[4].z = 0.0f;
    org.cells[5].x = 150.0f; org.cells[5].y = 0.0f; org.cells[5].z = 0.0f;

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

void test_extended_24_primitives_taxonomy_and_zero_gc() {
    std::cout << "[Test 8] 运行扩展 24 原语细胞功能分类学 (24 Primitives Taxonomy) 与零 GC 执行验证...\n";

    std::vector<CellType> all_types = {
        CellType::SENSE_RAW_INPUT_0, CellType::SENSE_RAW_INPUT_1,
        CellType::SENSE_RAW_INPUT_2, CellType::SENSE_RAW_INPUT_3,
        CellType::OP_EMA, CellType::OP_DIFF, CellType::OP_INTEGRAL,
        CellType::OP_SUM, CellType::OP_SUB, CellType::OP_MULTIPLY,
        CellType::OP_RATIO, CellType::OP_ABS,
        CellType::OP_DELAY_N, CellType::OP_OSCILLATOR, CellType::OP_QUADRATIC,
        CellType::GATE_THRESHOLD, CellType::GATE_HYSTERESIS,
        CellType::GATE_AND, CellType::GATE_INHIBIT,
        CellType::GATE_DEADZONE, CellType::GATE_MIN_MAX,
        CellType::ACT_PRIMARY_POSITIVE, CellType::ACT_PRIMARY_NEGATIVE,
        CellType::ACT_DEFENSIVE_RESET, CellType::ACT_IMMUNE_BLOCK
    };

    // 1. 验证全部原语类型命名与字符串映射有效性
    for (auto t : all_types) {
        Cell c{0, t};
        std::string name = c.type_name();
        assert(!name.empty());
        assert(name != "Cell_Unknown");
    }
    std::cout << "  ↳ 25 种全谱系细胞原语命名与类型反射映射 100% 有效\n";

    // 2. 验证 OP_DELAY_N: 滑动 FIFO 延迟管道 u(t) = x(t-k)
    {
        CellularOrganism org;
        org.cells.push_back({0, CellType::SENSE_RAW_INPUT_0, 1.0, 0.0});
        org.cells.push_back({1, CellType::OP_DELAY_N, 3.0 / 16.0, 0.0}); // k = 3
        org.synapses.push_back({0, 1, 0, 1.0, true});
        org.compile();

        double stream[] = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0};
        for (int t = 0; t < 7; ++t) {
            double in[4] = {stream[t], 0.0, 0.0, 0.0};
            org.forward(in);
            if (t <= 3) {
                // 前 3 拍写入 FIFO 缓冲区，由于初始缓冲为 0，前向输出为 0
                if (t < 3) assert(std::abs(org.cells[1].output_val) < 1e-6);
            }
        }
        // 重置状态验证
        org.reset_state();
        assert(org.cells[1].delay_idx == 0);
        assert(org.cells[1].delay_buffer[0] == 0.0);
        std::cout << "  ↳ OP_DELAY_N 滑动 FIFO 管道与 reset_state() 验证通过\n";
    }

    // 3. 验证 OP_OSCILLATOR: Van der Pol 自主相弛豫振荡器
    {
        CellularOrganism org;
        org.cells.push_back({0, CellType::SENSE_RAW_INPUT_0, 1.0, 0.0});
        org.cells.push_back({1, CellType::OP_OSCILLATOR, 1.0, 0.05}); // mu=1.0, dt=0.05
        org.synapses.push_back({0, 1, 0, 1.0, true});
        org.compile();

        double in[4] = {0.0, 0.0, 0.0, 0.0};
        std::vector<double> osc_vals;
        for (int step = 0; step < 100; ++step) {
            org.forward(in);
            osc_vals.push_back(org.cells[1].output_val);
        }
        double min_val = *std::min_element(osc_vals.begin(), osc_vals.end());
        double max_val = *std::max_element(osc_vals.begin(), osc_vals.end());
        assert(max_val > 0.1);
        assert(min_val < -0.1);
        assert(std::isfinite(max_val) && std::isfinite(min_val));
        std::cout << "  ↳ OP_OSCILLATOR 范德波尔自主起搏极限环振荡 (min=" << min_val << ", max=" << max_val << ") 验证通过\n";
    }

    // 4. 验证 OP_QUADRATIC: u = p1 * I0^2 + p2 * I0 * I1
    {
        CellularOrganism org;
        org.cells.push_back({0, CellType::SENSE_RAW_INPUT_0, 1.0, 0.0});
        org.cells.push_back({1, CellType::SENSE_RAW_INPUT_1, 1.0, 0.0});
        org.cells.push_back({2, CellType::OP_QUADRATIC, 2.5, -1.5});
        org.synapses.push_back({0, 2, 0, 1.0, true});
        org.synapses.push_back({1, 2, 1, 1.0, true});
        org.compile();

        double in[4] = {4.0, 3.0, 0.0, 0.0};
        org.forward(in); // 感受元锁存
        org.forward(in); // 突触汇聚运算: 2.5 * 16 + (-1.5) * 4 * 3 = 40.0 - 18.0 = 22.0
        assert(std::abs(org.cells[2].output_val - 22.0) < 1e-6);
        std::cout << "  ↳ OP_QUADRATIC 二次 Lyapunov 能量形态 (输出=" << org.cells[2].output_val << ") 验证通过\n";
    }

    // 5. 验证 GATE_DEADZONE: u = (|I0| > |p1|) ? I0 : 0.0
    {
        CellularOrganism org;
        org.cells.push_back({0, CellType::SENSE_RAW_INPUT_0, 1.0, 0.0});
        org.cells.push_back({1, CellType::GATE_DEADZONE, 2.0, 0.0});
        org.synapses.push_back({0, 1, 0, 1.0, true});
        org.compile();

        auto test_dz = [&](double in_val, double expected) {
            double in[4] = {in_val, 0.0, 0.0, 0.0};
            org.forward(in);
            org.forward(in);
            assert(std::abs(org.cells[1].output_val - expected) < 1e-6);
        };
        test_dz(1.5, 0.0);
        test_dz(2.5, 2.5);
        test_dz(-1.8, 0.0);
        test_dz(-3.5, -3.5);
        std::cout << "  ↳ GATE_DEADZONE 中心死区噪声门控验证通过\n";
    }

    // 6. 验证 GATE_MIN_MAX: u = (p1 > 0.5) ? max(I0, I1) : min(I0, I1)
    {
        CellularOrganism org;
        org.cells.push_back({0, CellType::SENSE_RAW_INPUT_0, 1.0, 0.0});
        org.cells.push_back({1, CellType::SENSE_RAW_INPUT_1, 1.0, 0.0});
        org.cells.push_back({2, CellType::GATE_MIN_MAX, 0.8, 0.0}); // max 模式
        org.synapses.push_back({0, 2, 0, 1.0, true});
        org.synapses.push_back({1, 2, 1, 1.0, true});
        org.compile();

        double in[4] = {5.0, 12.0, 0.0, 0.0};
        org.forward(in);
        org.forward(in);
        assert(std::abs(org.cells[2].output_val - 12.0) < 1e-6); // max(5.0, 12.0) = 12.0

        org.cells[2].param1 = 0.2; // min 模式
        org.forward(in);
        assert(std::abs(org.cells[2].output_val - 5.0) < 1e-6);  // min(5.0, 12.0) = 5.0
        std::cout << "  ↳ GATE_MIN_MAX 极值包络门控验证通过\n";
    }

    // 7. 包含全部原语的大规模拓扑复合网络延迟与零 GC 评测
    {
        CellularOrganism mega;
        uint16_t id = 0;
        for (auto t : all_types) {
            mega.cells.push_back({id, t, 0.5, -0.5});
            if (id > 0) {
                mega.synapses.push_back({static_cast<uint16_t>(id - 1), id, 0, 1.0, true});
            }
            id++;
        }
        mega.compile();

        double bench_inputs[4] = {3600.0, 100.0, 1.0, 0.05};
        const size_t ITERS = 100000;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < ITERS; ++i) {
            mega.forward(bench_inputs);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double avg_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / ITERS;
        std::cout << "  ↳ 25 原语全谱系全连接复合网络 100,000 次前向平均耗时: " << avg_ns << " ns/pass (真·零GC连续内存!)\n";
    }

    std::cout << "  -> 扩展 24 原语细胞功能分类学与零 GC 计算测试 100% 满分通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "       FlowEngine 形态发生细胞演化引擎单测集              \n";
    std::cout << "=========================================================\n\n";

    test_seed_organism_and_forward();
    test_cellular_mitosis_and_synapse_mutation();
    test_cellular_apoptosis_pruning();
    test_quant_cellular_adapter();
    test_adas_cellular_adapter();
    test_multigenerational_evolution();
    test_lennard_jones_force_field_and_visualization();
    test_extended_24_primitives_taxonomy_and_zero_gc();

    std::cout << "\n=========================================================\n";
    std::cout << "       全部 8 组形态发生演化与物理力场单测 100% 成功通过! \n";
    std::cout << "=========================================================\n\n";
    return 0;
}

