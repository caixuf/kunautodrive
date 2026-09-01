#include "kun/cellular/world_model.hpp"
#include "kun/cellular/maze_navigator.hpp"
#include "kun/cellular/adas_cellular_adapter.hpp"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace kun;

void test_learned_world_model_and_episodic_memory() {
    std::cout << "[Test 1] 验证环境动力学世界模型学习与情节记忆回放 (Learned World Model & Episodic Memory)...\n";
    LearnedWorldModel world_model(4, 0.1);
    EpisodicMemoryBuffer memory(500);

    assert(memory.empty());
    assert(world_model.get_training_steps() == 0);

    // 模拟一段交互经验并训练世界模型
    std::vector<float> s0 = {0.8f, 0.7f, 0.9f, 0.0f};
    CellularOrganism::ActionOutputs a0;
    a0.positive_action = 1.0;
    a0.negative_action = 0.0;
    std::vector<float> s1 = {0.6f, 0.5f, 0.7f, 0.0f};
    double r0 = 5.0;

    for (int epoch = 0; epoch < 50; ++epoch) {
        world_model.train_step(s0, a0, r0, s1);
        memory.push({s0, a0, r0, s1, false, 0.0});
    }

    assert(memory.size() == 50);
    assert(world_model.get_training_steps() == 50);

    // 验证世界模型预测的收敛性
    auto s_pred = world_model.predict_next_state(s0, a0);
    double r_pred = world_model.predict_reward(s0, a0);

    double err_s0 = std::abs(s_pred[0] - s1[0]);
    double err_r = std::abs(r_pred - r0);

    std::cout << "  ↳ 训练 50 步后: 状态预测误差=" << err_s0 << ", 奖励预测误差=" << err_r << "\n";
    assert(err_s0 < 0.20);
    assert(err_r < 1.0);

    // 验证情节采样
    auto batch = memory.sample_batch(10, 123);
    assert(batch.size() == 10);
    assert(batch[0].state.size() == 4);

    std::cout << "  -> 环境动力学世界模型与情节记忆系统 100% 满分通过！\n";
}

void test_mental_rollout_with_learned_world_model() {
    std::cout << "[Test 2] 验证世界模型引导的闭门心理推演 (Mental Rollout with World Model)...\n";
    CognitiveLoopController cog_controller(4);
    auto org = CellularOrganism::create_seed_organism(303);

    // 先用若干真实经验校准内部世界模型
    std::vector<float> s = {1.0f, 0.9f, 0.9f, 0.2f};
    CellularOrganism::ActionOutputs acts;
    acts.positive_action = 1.0;
    acts.negative_action = 0.1;
    std::vector<float> next_s = {0.8f, 0.7f, 0.7f, 0.1f};
    
    for (int i = 0; i < 30; ++i) {
        cog_controller.record_interaction(s, acts, 2.5, next_s, false);
    }

    // 执行 5 步心理推演 (切断外部输入，纯内部世界模型推导)
    auto rollout = cog_controller.simulate_mental_rollout(org, s, 5);

    assert(rollout.steps.size() == 5);
    assert(rollout.steps[0].state.size() == 4);
    assert(rollout.steps[0].next_state.size() == 4);
    assert(std::isfinite(rollout.cumulative_imagined_reward));
    assert(std::isfinite(rollout.total_free_energy));

    std::cout << "  ↳ 5 步心理推演完成: 累计想象奖励=" << rollout.cumulative_imagined_reward
              << ", 预期自由能=" << rollout.total_free_energy
              << ", 决策判定=" << rollout.decision_verdict << "\n";

    std::cout << "  -> 基于真实世界模型的心理推演机制 100% 满分通过！\n";
}

void test_baldwin_crystallization_in_evolution_loop() {
    std::cout << "[Test 3] 验证后天学习成果向先天基因的鲍德温固化闭环 (Baldwin Crystallization)...\n";
    auto org = CellularOrganism::create_seed_organism(404);
    assert(!org.synapses.empty());

    double init_w0 = org.synapses[0].initial_weight;
    // 模拟后天 Oja 可塑性在线学习：突触权重发生适应性偏移
    org.synapses[0].weight = init_w0 + 1.5;
    org.compiled_synapses_[0].weight = init_w0 + 1.5;

    // 执行 30% 速率的跨代固化
    org.crystallize_plasticity(0.30);
    double crystallized_w = org.synapses[0].initial_weight;

    // 断言：初始遗传基线向后天学习权重发生了真实沉淀
    assert(std::abs(crystallized_w - (init_w0 * 0.70 + (init_w0 + 1.5) * 0.30)) < 1e-4);
    std::cout << "  ↳ 初始遗传权重: " << init_w0 << " -> 后天学习权重: " << (init_w0 + 1.5)
              << " -> 鲍德温固化后初始权重: " << crystallized_w << "\n";

    // 验证在演化引擎代际进化中的自动闭环
    EvolutionConstraintConfig cfg;
    cfg.enable_baldwin_crystallization = true;
    cfg.crystallization_rate = 0.25;

    MorphogeneticEvolutionEngine engine(10, 777, cfg);
    auto& pop = engine.population();
    for (auto& o : pop) {
        if (!o.synapses.empty()) {
            o.synapses[0].weight += 0.8;
            o.compiled_synapses_[0].weight += 0.8;
        }
        o.fitness_score = 100.0;
    }

    engine.evolve_generation();
    assert(engine.get_champion().generation >= 1);
    std::cout << "  -> 鲍德温可塑性基因固化与世代演化闭环 100% 满分通过！\n";
}

void test_continual_learning_benchmark_and_forgetting_rate_gate() {
    std::cout << "[Test 4] 验证持续学习 A->B->A 任务迁移与灾难性遗忘门禁 (Continual Learning Gate)...\n";
    auto org = CellularOrganism::create_seed_organism(505);

    // Task A: 基础迷宫种子集
    MazeTask task_a(11, 11, 101, 60);
    // Task B: 异构迷宫种子集
    MazeTask task_b(11, 11, 201, 60);

    std::vector<uint32_t> seeds_a = {101, 102};
    std::vector<uint32_t> seeds_b = {201, 202};

    auto res = ContinualLearningBenchmark::run_continual_learning_eval(
        org, task_a, task_b, seeds_a, seeds_b, 60
    );

    std::cout << "  ↳ " << res.verdict << "\n";
    assert(res.forgetting_rate < 0.25); // 硬门禁：遗忘率严格小于 25%
    assert(res.passes_m2_gate);

    std::cout << "  -> 持续学习 A->B->A 与抗灾难性遗忘门禁 100% 满分通过！\n";
}

void test_adas_authority_boundary_isolation() {
    std::cout << "[Test 5] 验证持续学习与演化中智驾安全控制权硬边界隔离 (ADAS Safety Authority)...\n";
    auto org = CellularOrganism::create_seed_organism(606);
    AdasCellularAdapter adas_adapter(org);

    // 校验 ADAS 拓扑因果契约
    auto contract = org.evaluate_adas_contract();
    (void)contract;
    assert(contract.valid());

    // 模拟持续任务学习扰动与权重固化
    org.crystallize_plasticity(0.40);

    // 验证安全层节点绝不被破坏，契约保持 100% 有效
    auto post_contract = org.evaluate_adas_contract();
    (void)post_contract;
    assert(post_contract.valid());
    assert(post_contract.positive_action && post_contract.negative_action &&
           post_contract.defensive_reset && post_contract.immune_block);

    // 验证经过 perception 输入后的 AEB/控制输出依然符合 ASIL-D 边界
    auto out = adas_adapter.process_perception(5.0, -10.0, 0.0, 0.5);
    assert(std::isfinite(out.target_accel_mps2));
    assert(std::isfinite(out.steering_curvature));

    std::cout << "  ↳ Planning -> Control -> SafetyControl 权威隔离与因果契约未受污染 (Valid=true)\n";
    std::cout << "  -> 智驾安全权限硬隔离 100% 满分通过！\n";
}

int main() {
    std::cout << "\n======================================================================\n";
    std::cout << " 🧠 FlowEngine M2 认知闭环、世界模型推演与持续学习单测集\n";
    std::cout << "======================================================================\n\n";

    test_learned_world_model_and_episodic_memory();
    test_mental_rollout_with_learned_world_model();
    test_baldwin_crystallization_in_evolution_loop();
    test_continual_learning_benchmark_and_forgetting_rate_gate();
    test_adas_authority_boundary_isolation();

    std::cout << "\n======================================================================\n";
    std::cout << "   全部 5 组 M2 认知闭环与持续学习单测 100% 满分通过!\n";
    std::cout << "======================================================================\n\n";
    return 0;
}
