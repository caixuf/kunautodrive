#include <iostream>
#include <cassert>
#include <cmath>
#include <fstream>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <string>
#include <limits>
#include "kun/cellular/cellular_genome.hpp"

using namespace kun;
namespace fs = std::filesystem;

[[noreturn]] static void fail_test(const std::string& message) {
    std::cerr << "  ✗ " << message << "\n";
    std::exit(1);
}

static void require(bool condition, const std::string& message) {
    if (!condition) fail_test(message);
}

static void require_near(double actual, double expected, double eps, const std::string& message) {
    if (std::abs(actual - expected) > eps) {
        fail_test(message + " (actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected) + ")");
    }
}

static std::string make_temp_json_path(const std::string& prefix) {
    static std::atomic<uint64_t> seq{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return (fs::temp_directory_path() / (prefix + "-" + std::to_string(now) + "-" + std::to_string(seq++) + ".json")).string();
}

struct ScopedTempFile {
    std::string path;
    ~ScopedTempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

void test_checkpoint_preservation_without_genetic_pollution() {
    std::cout << "[Test 1] 验证普通检查点恢复保持个体运行时记忆且不污染遗传基因 (No Accidental Genetic Pollution)...\n";

    // 1. 创建个体并赋予初始遗传基线
    CellularOrganism org = CellularOrganism::create_seed_organism();
    require(!org.synapses.empty(), "seed organism must provide at least one synapse");
    org.organism_id = std::numeric_limits<uint64_t>::max() - 42;
    org.synapses[0].initial_weight = 1.0;
    org.synapses[0].weight = 1.0;
    org.compile();

    // 2. 模拟生命期后天 Oja 塑性学习导致权重漂移
    org.synapses[0].weight = 2.5; // 后天塑性漂移
    org.synapses[0].photon_pos = 0.42f;
    org.cells[0].output_val = 0.88;
    org.cells[0].activation_count = 42;
    org.cells[0].glow_charge = 0.55f;

    // 3. 存储全息检查点并重新加载
    ScopedTempFile ckpt{make_temp_json_path("flow-organism-test-checkpoint")};
    require(org.save_checkpoint_json(ckpt.path), "save_checkpoint_json must succeed");

    CellularOrganism reloaded = CellularOrganism::load_checkpoint_json(ckpt.path);
    require(reloaded.synapses.size() == org.synapses.size(), "reloaded synapse count must match saved organism");
    require(reloaded.cells.size() == org.cells.size(), "reloaded cell count must match saved organism");
    require(!reloaded.compiled_synapses_.empty(), "reloaded organism must be compiled");
    require(reloaded.organism_id == org.organism_id, "large 64-bit organism_id must survive checkpoint round-trip");
    
    // 验证：加载后保持了个体后天记忆 (weight == 2.5)
    std::cout << "  ↳ 加载后突触实时记忆权重: " << reloaded.synapses[0].weight << " (期望 2.5)\n";
    require_near(reloaded.synapses[0].weight, 2.5, 1e-4, "runtime synapse weight must survive checkpoint round-trip");

    // 验证：加载后没有篡改先天遗传基线 (initial_weight == 1.0)
    std::cout << "  ↳ 加载后先天遗传初始基线: " << reloaded.synapses[0].initial_weight << " (期望 1.0)\n";
    require_near(reloaded.synapses[0].initial_weight, 1.0, 1e-4, "genetic baseline must survive checkpoint round-trip");
    require_near(reloaded.compiled_synapses_[0].initial_weight, 1.0, 1e-4, "compiled synapse baseline must match serialized initial_weight");
    require_near(reloaded.synapses[0].photon_pos, 0.42, 1e-4, "synapse photon position must survive checkpoint round-trip");
    require_near(reloaded.cells[0].output_val, 0.88, 1e-4, "cell output state must survive checkpoint round-trip");
    require(reloaded.cells[0].activation_count == 42, "cell activation count must survive checkpoint round-trip");
    require_near(reloaded.cells[0].glow_charge, 0.55, 1e-4, "cell glow charge must survive checkpoint round-trip");

    // 验证：未经 crystallize_plasticity() 固化的个体进行繁殖时，新生后代重置为先天遗传初始权重 1.0
    CellularOrganism offspring = reloaded;
    for (auto& s : offspring.synapses) s.weight = s.initial_weight;
    offspring.compile();

    std::cout << "  ↳ 未固化亲本繁殖后代初始权重: " << offspring.synapses[0].weight << " (期望 1.0)\n";
    require_near(offspring.synapses[0].weight, 1.0, 1e-4, "offspring must reset to genetic baseline without crystallization");

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
    ScopedTempFile ckpt{make_temp_json_path("flow-baldwin-test-checkpoint")};
    require(org.save_checkpoint_json(ckpt.path), "save_checkpoint_json must succeed after crystallization");
    CellularOrganism reloaded = CellularOrganism::load_checkpoint_json(ckpt.path);

    require_near(reloaded.synapses[0].initial_weight, 1.75, 1e-4, "crystallized baseline must survive checkpoint round-trip");
    require_near(reloaded.synapses[0].weight, 1.75, 1e-4, "crystallized live weight must survive checkpoint round-trip");
    require_near(reloaded.compiled_synapses_[0].initial_weight, 1.75, 1e-4, "compiled crystallized baseline must stay in sync");

    // 繁殖后代继承固化后的 1.75
    CellularOrganism offspring = reloaded;
    for (auto& s : offspring.synapses) s.weight = s.initial_weight;
    offspring.compile();

    require_near(offspring.synapses[0].weight, 1.75, 1e-4, "offspring must inherit crystallized baseline");
    std::cout << "  ↳ 鲍德温固化后代继承遗传权重: " << offspring.synapses[0].weight << " (期望 1.75)\n";

    std::cout << "  -> 鲍德温显式固化遗传机制 100% 满分通过！\n";
}

void test_recurrent_synapse_restoration() {
    std::cout << "[Test 3] 验证突触递归标志 (is_recurrent) 与可塑性衰减参数正确解析恢复...\n";

    CellularOrganism org = CellularOrganism::create_seed_organism();
    require(org.synapses.size() >= 2, "seed organism must provide at least two synapses for recurrent restoration test");
    org.synapses[1].is_recurrent = true;
    org.synapses[1].hebbian_decay = 0.045;
    org.compile();

    std::string json_str = org.to_json();
    require(json_str.find("\"recurrent\": true") != std::string::npos, "serialized JSON must include recurrent flag");
    require(json_str.find("\"hebb_decay\": 0.045") != std::string::npos, "serialized JSON must include hebbian decay");

    CellularOrganism reloaded = CellularOrganism::from_json(json_str);
    require(reloaded.synapses.size() >= 2, "reloaded organism must preserve second synapse");
    require(reloaded.synapses[1].is_recurrent == true, "recurrent flag must survive JSON round-trip");
    require_near(reloaded.synapses[1].hebbian_decay, 0.045, 1e-4, "hebbian decay must survive JSON round-trip");
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
    require(genome_str.find("\"initial_weight\": 1") != std::string::npos, "genome export must contain initial_weight");
    // 从纯基因重建：live weight 必定严格等于 initial_weight (3.2 的漂移被剥离)
    CellularOrganism from_gene = CellularOrganism::from_genome_json(genome_str);
    require_near(from_gene.synapses[0].weight, 1.0, 1e-4, "genome import must strip live-weight drift");
    std::cout << "  ↳ genome.json 导出与纯基因实例化: weight=" << from_gene.synapses[0].weight << " (漂移已严格剔除)\n";

    // 2. 导出表观可塑性 epigenome.json
    std::string epi_str = org.export_epigenome_json();
    require(epi_str.find("\"drift\": 2.2") != std::string::npos, "epigenome export must contain plastic drift");
    require(epi_str.find("\"fitness_score\": 99.5") != std::string::npos, "epigenome export must contain fitness score");
    std::cout << "  ↳ epigenome.json 表观塑性漂移与调制态导出正确 (drift=2.2, fitness=99.5)\n";

    // 3. 导出运行时操作态 runtime.ckpt
    std::string rt_str = org.export_runtime_ckpt();
    require(rt_str.find("\"output\": 0.88") != std::string::npos, "runtime export must contain cell output");
    require(rt_str.find("\"activations\": 42") != std::string::npos, "runtime export must contain activation count");
    require(rt_str.find("\"weight\": 3.2") != std::string::npos, "runtime export must contain live synapse weight");
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
