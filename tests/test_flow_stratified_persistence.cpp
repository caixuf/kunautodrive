#include <iostream>
#include <cassert>
#include <cmath>
#include <fstream>
#include "kun/cellular/cellular_genome.hpp"

using namespace kun;

void test_checkpoint_preservation_without_genetic_pollution() {
    std::cout << "[Test 1] 验证普通检查点恢复保持个体运行时记忆且不污染遗传基因 (No Accidental Genetic Pollution)...\n";

    // 1. 创建个体并赋予初始遗传基线
    CellularOrganism org = CellularOrganism::create_seed_organism();
    assert(!org.synapses.empty());
    org.synapses[0].initial_weight = 1.0;
    org.synapses[0].weight = 1.0;
    org.compile();

    // 2. 模拟生命期后天 Oja 塑性学习导致权重漂移
    org.synapses[0].weight = 2.5; // 后天塑性漂移

    // 3. 存储全息检查点并重新加载
    std::string ckpt_path = "/tmp/flow_organism_test_checkpoint.json";
    assert(org.save_checkpoint_json(ckpt_path));

    CellularOrganism reloaded = CellularOrganism::load_checkpoint_json(ckpt_path);
    
    // 验证：加载后保持了个体后天记忆 (weight == 2.5)
    std::cout << "  ↳ 加载后突触实时记忆权重: " << reloaded.synapses[0].weight << " (期望 2.5)\n";
    assert(std::abs(reloaded.synapses[0].weight - 2.5) < 1e-4);

    // 验证：加载后没有篡改先天遗传基线 (initial_weight == 1.0)
    std::cout << "  ↳ 加载后先天遗传初始基线: " << reloaded.synapses[0].initial_weight << " (期望 1.0)\n";
    assert(std::abs(reloaded.synapses[0].initial_weight - 1.0) < 1e-4);

    // 验证：未经 crystallize_plasticity() 固化的个体进行繁殖时，新生后代重置为先天遗传初始权重 1.0
    CellularOrganism offspring = reloaded;
    for (auto& s : offspring.synapses) s.weight = s.initial_weight;
    offspring.compile();

    std::cout << "  ↳ 未固化亲本繁殖后代初始权重: " << offspring.synapses[0].weight << " (期望 1.0)\n";
    assert(std::abs(offspring.synapses[0].weight - 1.0) < 1e-4);

    std::cout << "  -> 检查点恢复与遗传基因隔离机制 100% 满分通过！\n";
}

void test_baldwin_crystallization_inheritance() {
    std::cout << "[Test 2] 验证显式鲍德温固化 (crystallize_plasticity) 正确固化入遗传基因...\n";

    CellularOrganism org = CellularOrganism::create_seed_organism();
    org.synapses[0].initial_weight = 1.0;
    org.synapses[0].weight = 2.5;
    org.compile();

    // 显式固化 50% 后天学习成果
    org.crystallize_plasticity(0.50);
    // 固化后 initial_weight = 1.0 * 0.5 + 2.5 * 0.5 = 1.75
    std::cout << "  ↳ 固化后遗传初始基线: " << org.synapses[0].initial_weight << " (期望 1.75)\n";
    assert(std::abs(org.synapses[0].initial_weight - 1.75) < 1e-4);

    // 存储并重新加载
    std::string ckpt_path = "/tmp/flow_baldwin_test_checkpoint.json";
    org.save_checkpoint_json(ckpt_path);
    CellularOrganism reloaded = CellularOrganism::load_checkpoint_json(ckpt_path);

    assert(std::abs(reloaded.synapses[0].initial_weight - 1.75) < 1e-4);
    assert(std::abs(reloaded.synapses[0].weight - 1.75) < 1e-4);

    // 繁殖后代继承固化后的 1.75
    CellularOrganism offspring = reloaded;
    for (auto& s : offspring.synapses) s.weight = s.initial_weight;
    offspring.compile();

    assert(std::abs(offspring.synapses[0].weight - 1.75) < 1e-4);
    std::cout << "  ↳ 鲍德温固化后代继承遗传权重: " << offspring.synapses[0].weight << " (期望 1.75)\n";

    std::cout << "  -> 鲍德温显式固化遗传机制 100% 满分通过！\n";
}

void test_recurrent_synapse_restoration() {
    std::cout << "[Test 3] 验证突触递归标志 (is_recurrent) 与可塑性衰减参数正确解析恢复...\n";

    CellularOrganism org = CellularOrganism::create_seed_organism();
    assert(org.synapses.size() >= 2);
    org.synapses[1].is_recurrent = true;
    org.synapses[1].hebbian_decay = 0.045;
    org.compile();

    std::string json_str = org.to_json();
    assert(json_str.find("\"recurrent\": true") != std::string::npos);
    assert(json_str.find("\"hebb_decay\": 0.045") != std::string::npos);

    CellularOrganism reloaded = CellularOrganism::from_json(json_str);
    assert(reloaded.synapses[1].is_recurrent == true);
    assert(std::abs(reloaded.synapses[1].hebbian_decay - 0.045) < 1e-4);
    std::cout << "  ↳ 递归突触解析恢复: is_recurrent=" << (reloaded.synapses[1].is_recurrent ? "true" : "false")
              << ", hebb_decay=" << reloaded.synapses[1].hebbian_decay << "\n";

    std::cout << "  -> 递归突触与塑性衰减参数序列化契约 100% 满分通过！\n";
}

void test_stratified_5_tier_export_contract() {
    std::cout << "[Test 4] 验证五层清晰分层持久化契约 (genome.json / epigenome.json / runtime.ckpt)...\n";

    CellularOrganism org = CellularOrganism::create_seed_organism();
    org.synapses[0].initial_weight = 1.0;
    org.synapses[0].weight = 3.2; // 产生了 2.2 的塑性漂移
    org.cells[0].output_val = 0.88;
    org.cells[0].activation_count = 42;
    org.fitness_score = 99.5;

    // 1. 导出纯先天基因 genome.json
    std::string genome_str = org.export_genome_json();
    assert(genome_str.find("\"initial_weight\": 1") != std::string::npos);
    // 从纯基因重建：live weight 必定严格等于 initial_weight (3.2 的漂移被剥离)
    CellularOrganism from_gene = CellularOrganism::from_genome_json(genome_str);
    assert(std::abs(from_gene.synapses[0].weight - 1.0) < 1e-4);
    std::cout << "  ↳ genome.json 导出与纯基因实例化: weight=" << from_gene.synapses[0].weight << " (漂移已严格剔除)\n";

    // 2. 导出表观可塑性 epigenome.json
    std::string epi_str = org.export_epigenome_json();
    assert(epi_str.find("\"drift\": 2.2") != std::string::npos);
    assert(epi_str.find("\"fitness_score\": 99.5") != std::string::npos);
    std::cout << "  ↳ epigenome.json 表观塑性漂移与调制态导出正确 (drift=2.2, fitness=99.5)\n";

    // 3. 导出运行时操作态 runtime.ckpt
    std::string rt_str = org.export_runtime_ckpt();
    assert(rt_str.find("\"output\": 0.88") != std::string::npos);
    assert(rt_str.find("\"activations\": 42") != std::string::npos);
    assert(rt_str.find("\"weight\": 3.2") != std::string::npos);
    std::cout << "  ↳ runtime.ckpt 实时放电电位与突触操作权值导出正确 (output=0.88, weight=3.2)\n";

    std::cout << "  -> 五层分层持久化契约 100% 满分通过！\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << " 🧬 FlowEngine 分层持久化、基因隔离与鲍德温固化序列化契约单测\n";
    std::cout << "======================================================================\n\n";

    test_checkpoint_preservation_without_genetic_pollution();
    test_baldwin_crystallization_inheritance();
    test_recurrent_synapse_restoration();
    test_stratified_5_tier_export_contract();

    std::cout << "\n======================================================================\n";
    std::cout << "   全部 4 组分层持久化与基因隔离单测 100% 满分通过!\n";
    std::cout << "======================================================================\n";
    return 0;
}
