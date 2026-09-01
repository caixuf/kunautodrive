#pragma once

#include "kun/cellular/cellular_genome.hpp"
#include "kun/cellular/evolvable_task.hpp"
#include <vector>
#include <deque>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>
#include <iostream>

namespace kun {

/**
 * @brief 情节记忆转换样本 (Episodic Experience)
 */
struct EpisodicExperience {
    std::vector<float> state;
    CellularOrganism::ActionOutputs action;
    double reward{0.0};
    std::vector<float> next_state;
    bool done{false};
    double prediction_error{0.0};
};

/**
 * @brief 情节记忆回放缓冲池 (EpisodicMemoryBuffer)
 */
class EpisodicMemoryBuffer {
public:
    explicit EpisodicMemoryBuffer(size_t capacity = 5000) : capacity_(capacity) {}

    void push(const EpisodicExperience& exp) {
        if (buffer_.size() >= capacity_) {
            buffer_.pop_front();
        }
        buffer_.push_back(exp);
    }

    size_t size() const { return buffer_.size(); }
    bool empty() const { return buffer_.empty(); }

    const EpisodicExperience& back() const { return buffer_.back(); }
    const std::deque<EpisodicExperience>& all_experiences() const { return buffer_; }

    std::vector<EpisodicExperience> sample_batch(size_t batch_size, uint32_t seed = 42) const {
        std::vector<EpisodicExperience> batch;
        if (buffer_.empty()) return batch;
        std::mt19937 rng(seed);
        std::uniform_int_distribution<size_t> dist(0, buffer_.size() - 1);
        for (size_t i = 0; i < batch_size && i < buffer_.size(); ++i) {
            batch.push_back(buffer_[dist(rng)]);
        }
        return batch;
    }

    void clear() { buffer_.clear(); }

private:
    size_t capacity_{5000};
    std::deque<EpisodicExperience> buffer_;
};

/**
 * @brief 学习到的内部环境动力学世界模型 (LearnedWorldModel)
 * 基于在线经验回放与增量自回归学习，学习状态转移矩阵 T(s, a) -> s' 与奖励函数 R(s, a) -> r
 */
class LearnedWorldModel {
public:
    explicit LearnedWorldModel(size_t state_dim = 4, double learning_rate = 0.05)
        : state_dim_(state_dim), lr_(learning_rate) {
        // 输入特征维度 = state_dim (4) + positive_action (1) + negative_action (1) = 6
        feature_dim_ = state_dim_ + 2;
        w_trans_.assign(state_dim_ * feature_dim_, 0.0);
        b_trans_.assign(state_dim_, 0.0);
        
        // 初始自回归恒等先验 (s_{t+1} ≈ s_t)
        for (size_t i = 0; i < state_dim_; ++i) {
            w_trans_[i * feature_dim_ + i] = 0.85;
        }

        // 奖励预测权值
        w_reward_.assign(feature_dim_, 0.0);
        b_reward_ = 0.0;
    }

    void train_step(const std::vector<float>& s, 
                    const CellularOrganism::ActionOutputs& a, 
                    double r, 
                    const std::vector<float>& next_s) {
        if (s.size() < state_dim_ || next_s.size() < state_dim_) return;

        // 构建输入特征向量 x = [s_0..s_3, a_pos, a_neg]
        std::vector<double> x(feature_dim_, 0.0);
        for (size_t i = 0; i < state_dim_; ++i) x[i] = s[i];
        x[state_dim_] = a.positive_action;
        x[state_dim_ + 1] = a.negative_action;

        // 1. 预测并更新状态转移模型
        for (size_t d = 0; d < state_dim_; ++d) {
            double pred = b_trans_[d];
            for (size_t j = 0; j < feature_dim_; ++j) {
                pred += w_trans_[d * feature_dim_ + j] * x[j];
            }
            double target = next_s[d];
            double err = target - pred;

            // 梯度下降更新转移模型
            b_trans_[d] += lr_ * err;
            for (size_t j = 0; j < feature_dim_; ++j) {
                w_trans_[d * feature_dim_ + j] += lr_ * err * x[j];
            }
        }

        // 2. 预测并更新奖励模型
        double pred_r = b_reward_;
        for (size_t j = 0; j < feature_dim_; ++j) pred_r += w_reward_[j] * x[j];
        double err_r = r - pred_r;
        b_reward_ += lr_ * err_r;
        for (size_t j = 0; j < feature_dim_; ++j) {
            w_reward_[j] += lr_ * err_r * x[j];
        }

        total_training_steps_++;
    }

    std::vector<float> predict_next_state(const std::vector<float>& s, const CellularOrganism::ActionOutputs& a) const {
        std::vector<float> next_s(state_dim_, 0.0f);
        if (s.size() < state_dim_) return next_s;

        std::vector<double> x(feature_dim_, 0.0);
        for (size_t i = 0; i < state_dim_; ++i) x[i] = s[i];
        x[state_dim_] = a.positive_action;
        x[state_dim_ + 1] = a.negative_action;

        for (size_t d = 0; d < state_dim_; ++d) {
            double val = b_trans_[d];
            for (size_t j = 0; j < feature_dim_; ++j) {
                val += w_trans_[d * feature_dim_ + j] * x[j];
            }
            // 激光测距物理截断在 [0, 1]，航向角截断在 [-1, 1]
            if (d < 3) {
                next_s[d] = static_cast<float>(std::clamp(val, 0.0, 1.0));
            } else {
                next_s[d] = static_cast<float>(std::clamp(val, -1.0, 1.0));
            }
        }
        return next_s;
    }

    double predict_reward(const std::vector<float>& s, const CellularOrganism::ActionOutputs& a) const {
        if (s.size() < state_dim_) return 0.0;
        std::vector<double> x(feature_dim_, 0.0);
        for (size_t i = 0; i < state_dim_; ++i) x[i] = s[i];
        x[state_dim_] = a.positive_action;
        x[state_dim_ + 1] = a.negative_action;

        double r = b_reward_;
        for (size_t j = 0; j < feature_dim_; ++j) r += w_reward_[j] * x[j];
        return r;
    }

    size_t get_training_steps() const { return total_training_steps_; }

private:
    size_t state_dim_{4};
    size_t feature_dim_{6};
    double lr_{0.05};
    size_t total_training_steps_{0};
    std::vector<double> w_trans_;
    std::vector<double> b_trans_;
    std::vector<double> w_reward_;
    double b_reward_{0.0};
};

/**
 * @brief 闭门心理推演结果包 (MentalRolloutReport)
 */
struct MentalRolloutReport {
    struct Step {
        std::vector<float> state;
        CellularOrganism::ActionOutputs action;
        double predicted_reward{0.0};
        std::vector<float> next_state;
    };
    std::vector<Step> steps;
    double cumulative_imagined_reward{0.0};
    double total_free_energy{0.0};
    std::string decision_verdict;
};

/**
 * @brief 认知闭环控制器 (CognitiveLoopController)
 * 整合 "感知 -> 世界模型推演 -> 情节记忆 -> 目标决策 -> 行动 -> 突触学习 -> 跨代固化"
 */
class CognitiveLoopController {
public:
    CognitiveLoopController(size_t state_dim = 4)
        : world_model_(state_dim), memory_(2000) {}

    // 执行世界模型引导的心理推演 (Mental Rollout with Learned World Model)
    MentalRolloutReport simulate_mental_rollout(CellularOrganism& org,
                                              const std::vector<float>& initial_state,
                                              int rollout_steps = 5) {
        MentalRolloutReport report;
        report.steps.reserve(rollout_steps);

        std::vector<float> curr_s = initial_state;
        double cum_r = 0.0;

        for (int k = 0; k < rollout_steps; ++k) {
            double inputs[4] = {0.0, 0.0, 0.0, 0.0};
            for (size_t d = 0; d < 4 && d < curr_s.size(); ++d) inputs[d] = curr_s[d];

            // 心理推演中前向推导，不直接更新突触物理权重
            auto acts = org.forward(inputs, false);
            auto next_s = world_model_.predict_next_state(curr_s, acts);
            double pred_r = world_model_.predict_reward(curr_s, acts);

            cum_r += pred_r;
            report.steps.push_back({curr_s, acts, pred_r, next_s});
            curr_s = next_s;
        }

        report.cumulative_imagined_reward = cum_r;
        report.total_free_energy = std::max(0.0, -cum_r);
        report.decision_verdict = (cum_r > 0.0) ? "IMAGINED_PATH_VIABLE" : "IMAGINED_PATH_BLOCKED";
        return report;
    }

    // 接收真实物理交互反馈，更新世界模型并存入情节记忆
    void record_interaction(const std::vector<float>& s,
                            const CellularOrganism::ActionOutputs& a,
                            double r,
                            const std::vector<float>& next_s,
                            bool done) {
        world_model_.train_step(s, a, r, next_s);
        memory_.push({s, a, r, next_s, done, 0.0});
    }

    LearnedWorldModel& world_model() { return world_model_; }
    const LearnedWorldModel& world_model() const { return world_model_; }
    EpisodicMemoryBuffer& episodic_memory() { return memory_; }
    const EpisodicMemoryBuffer& episodic_memory() const { return memory_; }

private:
    LearnedWorldModel world_model_;
    EpisodicMemoryBuffer memory_;
};

/**
 * @brief 持续学习与灾难性遗忘基准评测器 (ContinualLearningBenchmark)
 * 协议: Task A (初始基线) -> Task B (新环境自适应迁移) -> Task A (返场留存测试)
 * 门禁: 遗忘率 (Forgetting Rate) < 25%
 */
class ContinualLearningBenchmark {
public:
    struct BenchmarkResult {
        double task_a_initial_score{0.0};
        double task_b_score{0.0};
        double task_a_retained_score{0.0};
        double forgetting_rate{0.0};       // max(0, A_init - A_retained) / max(1.0, A_init)
        double retention_rate{1.0};        // 1.0 - forgetting_rate
        bool passes_m2_gate{false};        // forgetting_rate < 0.25
        std::string verdict;
    };

    static BenchmarkResult run_continual_learning_eval(CellularOrganism& org,
                                                       EvolvableTask& task_a,
                                                       EvolvableTask& task_b,
                                                       const std::vector<uint32_t>& task_a_seeds,
                                                       const std::vector<uint32_t>& task_b_seeds,
                                                       int adaptation_steps = 80) {
        BenchmarkResult res;

        // Step 1: 测定 Task A 初始性能基线
        auto m_a0 = task_a.evaluate_organism(org, task_a_seeds, adaptation_steps, false);
        res.task_a_initial_score = m_a0.mean_fitness;

        // Step 2: 在 Task B 中进行在线自适应学习 (允许 Oja Hebbian 突触塑形)
        auto m_b = task_b.evaluate_organism(org, task_b_seeds, adaptation_steps, true);
        res.task_b_score = m_b.mean_fitness;

        // 鲍德温跨代突触固化
        org.crystallize_plasticity(0.30);

        // Step 3: 返回 Task A 进行返场留存测试 (评估是否发生灾难性遗忘)
        auto m_a1 = task_a.evaluate_organism(org, task_a_seeds, adaptation_steps, false);
        res.task_a_retained_score = m_a1.mean_fitness;

        // 计算遗忘率
        double delta = std::max(0.0, res.task_a_initial_score - res.task_a_retained_score);
        double denom = std::max(1.0, std::abs(res.task_a_initial_score));
        res.forgetting_rate = delta / denom;
        res.retention_rate = std::max(0.0, 1.0 - res.forgetting_rate);

        // 门禁判定：遗忘率 < 25% (留存率 >= 75%)
        res.passes_m2_gate = (res.forgetting_rate < 0.25);

        std::ostringstream oss;
        oss << "Continual Learning A->B->A: A_init=" << std::fixed << std::setprecision(2)
            << res.task_a_initial_score << " -> B_adapt=" << res.task_b_score
            << " -> A_retained=" << res.task_a_retained_score
            << ", ForgettingRate=" << std::setprecision(1) << (res.forgetting_rate * 100.0)
            << "% (Retention=" << (res.retention_rate * 100.0) << "%). M2 Gate="
            << (res.passes_m2_gate ? "PASSED (< 25%)" : "FAILED (>= 25%)");
        res.verdict = oss.str();

        return res;
    }
};

} // namespace kun
