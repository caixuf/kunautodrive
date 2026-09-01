#pragma once

#include "kun/cellular/evolvable_task.hpp"
#include "kun/cellular/cellular_genome.hpp"
#include <vector>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>
#include <iostream>

namespace kun {

/**
 * @brief 经典连续倒立摆控制任务 (CartPoleTask) — 遵循 EvolvableTask 接口
 * 物理动力学状态: [x (小车位置), x_dot (小车速度), theta (摆杆夹角 rad), theta_dot (角速度)]
 */
class CartPoleTask : public EvolvableTask {
public:
    explicit CartPoleTask(int max_steps = 200, uint32_t seed = 42)
        : max_steps_(max_steps), rng_(seed) {
        reset(seed);
    }

    const char* name() const override { return "CartPole-ContinuousPhysics"; }
    size_t obs_dim() const override { return 4; }
    size_t act_dim() const override { return 2; }

    void reset(uint32_t episode_seed) override {
        rng_.seed(episode_seed);
        std::uniform_real_distribution<float> dist(-0.05f, 0.05f);
        x_ = dist(rng_);
        x_dot_ = dist(rng_);
        theta_ = dist(rng_);
        theta_dot_ = dist(rng_);
        step_count_ = 0;
    }

    std::vector<float> current_observation() const override {
        // 归一化到 [-1, 1] 区间便于神经细胞激活
        return {
            std::clamp(x_ / 2.4f, -1.0f, 1.0f),
            std::clamp(x_dot_ / 3.0f, -1.0f, 1.0f),
            std::clamp(theta_ / 0.2094f, -1.0f, 1.0f), // 12度
            std::clamp(theta_dot_ / 3.0f, -1.0f, 1.0f)
        };
    }

    StepResult step(int action) override {
        step_count_++;

        // 物理动力学常数
        const float gravity = 9.8f;
        const float masscart = 1.0f;
        const float masspole = 0.1f;
        const float total_mass = masscart + masspole;
        const float length = 0.5f; // 半杆长
        const float polemass_length = masspole * length;
        const float force = (action == 1) ? 10.0f : -10.0f;
        const float tau = 0.02f; // 20ms 时间步长

        float costheta = std::cos(theta_);
        float sintheta = std::sin(theta_);

        float temp = (force + polemass_length * theta_dot_ * theta_dot_ * sintheta) / total_mass;
        float thetaacc = (gravity * sintheta - costheta * temp) /
                         (length * (4.0f / 3.0f - masspole * costheta * costheta / total_mass));
        float xacc = temp - polemass_length * thetaacc * costheta / total_mass;

        // 半隐式欧拉积分
        x_ += tau * x_dot_;
        x_dot_ += tau * xacc;
        theta_ += tau * theta_dot_;
        theta_dot_ += tau * thetaacc;

        // 终止条件: 摆角超 12度 (0.2094 rad) 或 小车出界 (> 2.4m)
        bool failed = (std::abs(x_) > 2.4f || std::abs(theta_) > 0.2094f);
        bool timeout = (step_count_ >= max_steps_);

        StepResult res;
        res.obs = current_observation();
        res.done = (failed || timeout);
        res.success = (!failed && timeout);
        res.steps = step_count_;
        res.reward = failed ? 0.0 : (1.0 + (0.2094f - std::abs(theta_)) * 2.0); // 坚挺奖励 + 竖直姿态奖励
        res.min_dist_to_goal = std::abs(theta_);

        return res;
    }

    StepResult step_continuous(const CellularOrganism::ActionOutputs& acts) override {
        int act = (acts.positive_action >= acts.negative_action) ? 1 : 0;
        return step(act);
    }

    double current_fitness() const override {
        return static_cast<double>(step_count_) + (0.2094 - std::min(0.2094, (double)std::abs(theta_))) * 50.0;
    }

private:
    int max_steps_{200};
    int step_count_{0};
    float x_{0.0f};
    float x_dot_{0.0f};
    float theta_{0.0f};
    float theta_dot_{0.0f};
    std::mt19937 rng_;
};

/**
 * @brief 离散符号与序列规则学习任务 (SequenceRuleTask) — 遵循 EvolvableTask 接口
 * 规则目标: 识别时间序列异或奇偶校验 (XOR Parity Rule) 与动态周期转移
 */
class SequenceRuleTask : public EvolvableTask {
public:
    explicit SequenceRuleTask(int seq_length = 50, uint32_t seed = 42)
        : seq_length_(seq_length), rng_(seed) {
        reset(seed);
    }

    const char* name() const override { return "Sequence-SymbolicRuleLearning"; }
    size_t obs_dim() const override { return 4; }
    size_t act_dim() const override { return 2; }

    void reset(uint32_t episode_seed) override {
        rng_.seed(episode_seed);
        step_idx_ = 0;
        correct_count_ = 0;

        // 生成确定性二值符号序列
        seq_.resize(seq_length_ + 4);
        std::bernoulli_distribution dist(0.5);
        for (size_t i = 0; i < seq_.size(); ++i) {
            seq_[i] = dist(rng_) ? 1.0f : 0.0f;
        }
    }

    std::vector<float> current_observation() const override {
        size_t idx = static_cast<size_t>(step_idx_);
        float b0 = (idx < seq_.size()) ? seq_[idx] : 0.0f;
        float b1 = (idx + 1 < seq_.size()) ? seq_[idx + 1] : 0.0f;
        float b2 = (idx + 2 < seq_.size()) ? seq_[idx + 2] : 0.0f;
        float progress = static_cast<float>(step_idx_) / static_cast<float>(seq_length_);
        return {b0, b1, b2, progress};
    }

    StepResult step(int action) override {
        // 目标预测规则: Rule(b0, b1, b2) = (b0 ^ b1) == b2 ? 1 : 0 (异或奇偶性)
        float b0 = seq_[step_idx_];
        float b1 = seq_[step_idx_ + 1];
        float b2 = seq_[step_idx_ + 2];
        int target = ((static_cast<int>(b0) ^ static_cast<int>(b1)) == static_cast<int>(b2)) ? 1 : 0;

        bool is_correct = (action == target);
        if (is_correct) correct_count_++;
        step_idx_++;

        bool done = (step_idx_ >= seq_length_);
        StepResult res;
        res.obs = current_observation();
        res.reward = is_correct ? 2.0 : -1.0;
        res.done = done;
        res.success = (correct_count_ >= static_cast<int>(seq_length_ * 0.85));
        res.steps = step_idx_;
        res.min_dist_to_goal = static_cast<double>(seq_length_ - correct_count_);

        return res;
    }

    StepResult step_continuous(const CellularOrganism::ActionOutputs& acts) override {
        int act = (acts.positive_action >= acts.negative_action) ? 1 : 0;
        return step(act);
    }

    double current_fitness() const override {
        double accuracy = static_cast<double>(correct_count_) / std::max(1.0, static_cast<double>(seq_length_));
        return accuracy * 100.0;
    }

private:
    int seq_length_{50};
    int step_idx_{0};
    int correct_count_{0};
    std::vector<float> seq_;
    std::mt19937 rng_;
};

/**
 * @brief 跨域 Few-Shot 迁移学习加速比评测器 (CrossDomainTransferEvaluator)
 * 验证: 预演化母体网络迁移到新任务的收敛代数 vs 从零随机初始化的收敛代数
 * 门禁: 迁移加速比 (Transfer Speedup Ratio) > 1.5x
 */
class CrossDomainTransferEvaluator {
public:
    struct TransferReport {
        std::string source_domain;
        std::string target_domain;
        int scratch_convergence_generations{0};
        int transfer_convergence_generations{0};
        double acceleration_ratio{0.0};
        bool passes_m3_gate{false};
        std::string summary;
    };

    static TransferReport evaluate_transfer(
        EvolvableTask& target_task,
        const std::vector<uint32_t>& task_seeds,
        CellularOrganism pre_adapted_org,
        double target_fitness_threshold = 40.0,
        int max_generations = 25
    ) {
        TransferReport report;
        report.source_domain = "Maze-SpatialNavigation";
        report.target_domain = target_task.name();

        // 1. 从零随机初始化种群 (Scratch Baseline - 极小随机未分化胚胎母体)
        MorphogeneticEvolutionEngine scratch_engine(16, 42, SeedInitMode::MINIMAL_RANDOM_GRAPH);
        int scratch_gen = max_generations;

        for (int g = 1; g <= max_generations; ++g) {
            auto& pop = scratch_engine.population();
            double best_fit = -1e9;
            for (auto& org : pop) {
                auto m = target_task.evaluate_organism(org, task_seeds, 80, true);
                org.fitness_score = m.mean_fitness;
                if (m.mean_fitness > best_fit) best_fit = m.mean_fitness;
            }
            if (best_fit >= target_fitness_threshold) {
                scratch_gen = g;
                break;
            }
            scratch_engine.evolve_generation();
        }

        // 2. 跨域迁移种群 (Transfer via Pre-adapted Organism Seed)
        MorphogeneticEvolutionEngine transfer_engine(16, 42, SeedInitMode::HANDCRAFTED_PROGENITOR);
        // 将预演化的母体拓扑注入整个种群作为母体先验
        for (size_t i = 0; i < transfer_engine.population().size(); ++i) {
            transfer_engine.population()[i] = pre_adapted_org;
            if (i > 0) {
                transfer_engine.mutate(transfer_engine.population()[i]);
            }
        }

        int transfer_gen = max_generations;
        for (int g = 1; g <= max_generations; ++g) {
            auto& pop = transfer_engine.population();
            double best_fit = -1e9;
            for (auto& org : pop) {
                auto m = target_task.evaluate_organism(org, task_seeds, 80, true);
                org.fitness_score = m.mean_fitness;
                if (m.mean_fitness > best_fit) best_fit = m.mean_fitness;
            }
            if (best_fit >= target_fitness_threshold) {
                transfer_gen = g;
                break;
            }
            transfer_engine.evolve_generation();
        }

        report.scratch_convergence_generations = scratch_gen;
        report.transfer_convergence_generations = transfer_gen;
        report.acceleration_ratio = static_cast<double>(scratch_gen) / static_cast<double>(std::max(1, transfer_gen));
        report.passes_m3_gate = (report.acceleration_ratio >= 1.50 || (transfer_gen == 1 && scratch_gen >= 2));

        std::ostringstream oss;
        oss << "Cross-Domain Transfer [" << report.source_domain << " -> " << report.target_domain
            << "]: Scratch Generations=" << scratch_gen << ", Transfer Generations=" << transfer_gen
            << ", Speedup Ratio=" << std::fixed << std::setprecision(2) << report.acceleration_ratio
            << "x. M3 Gate=" << (report.passes_m3_gate ? "PASSED (>= 1.5x)" : "FAILED (< 1.5x)");
        report.summary = oss.str();

        return report;
    }
};

} // namespace kun
