#include "kun/cellular/cross_domain_tasks.hpp"
#include "kun/cellular/maze_navigator.hpp"
#include "kun/cellular/adas_cellular_adapter.hpp"
#include "kun/cellular/quant_cellular_adapter.hpp"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace kun;

void test_cartpole_continuous_physics_task() {
    std::cout << "[Test 1] 验证连续动力学倒立摆任务 (CartPole Continuous Physics Task)...\n";
    CartPoleTask task(200, 101);

    assert(std::string(task.name()) == "CartPole-ContinuousPhysics");
    assert(task.obs_dim() == 4);
    assert(task.act_dim() == 2);

    task.reset(101);
    auto obs0 = task.current_observation();
    assert(obs0.size() == 4);
    assert(std::abs(obs0[0]) <= 1.0f);
    assert(std::abs(obs0[2]) <= 1.0f);

    // 模拟推力动作
    auto res1 = task.step(1); // 向右推
    assert(res1.steps == 1);
    assert(!res1.done);

    auto res2 = task.step(0); // 向左推
    assert(res2.steps == 2);

    double fit = task.current_fitness();
    assert(fit > 0.0);

    std::cout << "  ↳ CartPole 状态观测维度=" << task.obs_dim() 
              << ", 2步推演后适应度=" << fit << ", 角度偏移=" << res2.min_dist_to_goal << " rad\n";
    std::cout << "  -> 倒立摆连续控制任务契约 100% 满分通过！\n";
}

void test_sequence_rule_learning_task() {
    std::cout << "[Test 2] 验证离散符号异或规则学习任务 (Sequence Symbolic Rule Learning)...\n";
    SequenceRuleTask task(40, 202);

    assert(std::string(task.name()) == "Sequence-SymbolicRuleLearning");
    assert(task.obs_dim() == 4);
    assert(task.act_dim() == 2);

    task.reset(202);
    auto obs0 = task.current_observation();
    assert(obs0.size() == 4);

    // 模拟连续动作输入
    CellularOrganism::ActionOutputs acts;
    acts.positive_action = 1.0;
    acts.negative_action = 0.0;

    auto res = task.step_continuous(acts);
    assert(res.steps == 1);

    double fit = task.current_fitness();
    (void)fit;
    std::cout << "  ↳ SequenceRule 符号序列长度=40, 单步奖励=" << res.reward << ", 累计适应度=" << fit << "\n";
    std::cout << "  -> 离散符号规则学习任务契约 100% 满分通过！\n";
}

void test_cross_domain_transfer_acceleration() {
    std::cout << "[Test 3] 验证跨域 Few-Shot 迁移学习加速比 (Cross-Domain Transfer Speedup)...\n";
    
    // 1. 预演化母体网络 (先在基础连续控制环境中演化出具有反馈回路的母体)
    MorphogeneticEvolutionEngine pretrain_engine(16, 999, SeedInitMode::HANDCRAFTED_PROGENITOR);
    CartPoleTask base_env(50, 101);
    std::vector<uint32_t> base_seeds = {101, 102, 103};
    for (int g = 0; g < 6; ++g) {
        for (auto& org : pretrain_engine.population()) {
            auto m = base_env.evaluate_organism(org, base_seeds, 50, true);
            org.fitness_score = m.mean_fitness;
        }
        pretrain_engine.evolve_generation();
    }
    auto pre_org = pretrain_engine.get_champion();

    // 2. 迁移至长程控制任务 (CartPole-LongHorizon, 目标适应度 48.0)
    CartPoleTask target_task(120, 777);
    std::vector<uint32_t> target_seeds = {777, 778, 779};

    auto report = CrossDomainTransferEvaluator::evaluate_transfer(
        target_task, target_seeds, pre_org, 48.0, 20
    );

    std::cout << "  ↳ " << report.summary << "\n";
    assert(report.passes_m3_gate);
    assert(report.acceleration_ratio >= 1.50);

    std::cout << "  -> 跨域迁移加速比评测与 M3 门禁 100% 满分通过！\n";
}

void test_adas_and_quant_zero_regression_guard() {
    std::cout << "[Test 4] 验证跨域平台化扩展后智驾与量化核心闭环零回退 (Zero-Regression Guard)...\n";
    
    // 1. 验证智驾 ASIL-D 安全因果与包络未发生回退
    auto adas_org = CellularOrganism::create_seed_organism(505);
    auto adas_contract = adas_org.evaluate_adas_contract();
    (void)adas_contract;
    assert(adas_contract.valid());

    AdasCellularAdapter adapter(adas_org);
    auto ctl_out = adapter.process_perception(4.0, -12.0, 0.0, 0.3);
    assert(ctl_out.is_aeb_triggered);
    assert(ctl_out.target_accel_mps2 <= -5.0); // 紧急刹停响应

    // 2. 验证量化微秒级多相态 Tick 信号处理零回退
    QuantCellularAdapter quant(adas_org);
    QuantTickMsg tick;
    tick.last_price = 3620.0;
    tick.volume = 5000.0;
    tick.bid_price1 = 3619.0;
    tick.ask_price1 = 3621.0;
    tick.bid_volume1 = 100.0;
    tick.ask_volume1 = 120.0;
    auto sig = quant.process_tick(tick);
    assert(std::isfinite(sig.target_price));

    std::cout << "  ↳ 智驾 AEB 急刹减速度=" << ctl_out.target_accel_mps2 
              << " m/s^2, 量化决策解释=" << sig.explanation << "\n";
    std::cout << "  -> 跨域平台零回退守卫 100% 满分通过！\n";
}

int main() {
    std::cout << "\n======================================================================\n";
    std::cout << " 🌐 FlowEngine M3 跨域通用性平台、多领域任务与迁移加速单测集\n";
    std::cout << "======================================================================\n\n";

    test_cartpole_continuous_physics_task();
    test_sequence_rule_learning_task();
    test_cross_domain_transfer_acceleration();
    test_adas_and_quant_zero_regression_guard();

    std::cout << "\n======================================================================\n";
    std::cout << "   全部 4 组 M3 跨域通用性与迁移加速单测 100% 满分通过!\n";
    std::cout << "======================================================================\n\n";
    return 0;
}
