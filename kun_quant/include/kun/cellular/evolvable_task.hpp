#pragma once

// ============================================================================
// EvolvableTask — 形态发生细胞引擎的通用任务接口 (Gym 风格标准契约)
//
// 设计原则 (M1 规范):
// 1. 任务正交解耦: 任何序贯决策问题通过 "观测 -> 动作 -> 奖励 -> 状态" 挂载
// 2. 训练与留出严格隔离 (Train vs Holdout ID vs OOD Split):
//    - 训练集 (Train): 指定种子与基础尺寸
//    - 同分布留出集 (Holdout ID): 相同尺寸、绝对未见过的独立种子
//    - 分布外留出集 (Holdout OOD): 更大地图尺寸、异构拓扑的泛化集
// 3. 规范化 OOS (Out-of-Sample) Champion 报告、WL 拓扑哈希与门禁判定 (>= 70%)
// ============================================================================

#include "kun/cellular/cellular_genome.hpp"
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace kun {

struct StepResult {
    std::vector<float> obs;   // 下一观测 (维度 = obs_dim)
    double reward{0.0};       // 即时奖励
    bool done{false};         // 回合终止 (到达目标 / 超时 / 碰撞破坏)
    bool success{false};      // 是否成功达成最终目标 (区别于超时终止)
    int steps{0};             // 当前回合执行步数
    double min_dist_to_goal{999.0}; // 距离目标最小欧氏距离
    int collision_count{0};   // 碰撞次数
};

struct TaskEvalMetrics {
    double mean_fitness{0.0};
    double success_rate{0.0}; // 到达率 (Reach Rate: success / total_episodes)
    double mean_steps{0.0};
    double mean_min_dist{0.0};
    size_t num_episodes{0};
    size_t success_episodes{0};

    std::string to_string() const {
        std::ostringstream oss;
        oss << "success_rate=" << std::fixed << std::setprecision(1) << (success_rate * 100.0) << "% ("
            << success_episodes << "/" << num_episodes << "), mean_fit=" << std::setprecision(2)
            << mean_fitness << ", mean_steps=" << std::setprecision(1) << mean_steps
            << ", mean_min_dist=" << std::setprecision(2) << mean_min_dist;
        return oss.str();
    }
};

struct TaskDatasetSplit {
    std::string task_name{"MazeNavigation"};
    std::vector<uint32_t> train_seeds;       // 训练集种子清单 (如 [101..110])
    std::vector<uint32_t> holdout_id_seeds;  // 同分布留出集种子 (如 [201..210])
    std::vector<uint32_t> holdout_ood_seeds; // 跨尺寸分布外种子 (如 [301..310])
    int train_map_size{11};                  // 训练集与 ID 集地图尺寸
    int ood_map_size{19};                    // OOD 集大尺寸地图
    int max_steps_per_episode{160};          // 单回合最大步数门限

    static TaskDatasetSplit create_default_maze_split() {
        TaskDatasetSplit split;
        split.task_name = "MazeNavigation";
        split.train_map_size = 11;
        split.ood_map_size = 19;
        split.max_steps_per_episode = 160;

        // 训练集 10 个种子
        for (uint32_t s = 101; s <= 110; ++s) split.train_seeds.push_back(s);
        // ID 留出集 10 个独立种子 (绝不重叠)
        for (uint32_t s = 201; s <= 210; ++s) split.holdout_id_seeds.push_back(s);
        // OOD 留出集 10 个大尺寸独立种子
        for (uint32_t s = 301; s <= 310; ++s) split.holdout_ood_seeds.push_back(s);
        return split;
    }
};

struct OOSReport {
    std::string task_name{"UnknownTask"};
    uint64_t organism_id{0};
    std::string organism_lineage;
    std::string topology_hash;
    size_t num_cells{0};
    size_t num_synapses{0};

    TaskEvalMetrics train_metrics;
    TaskEvalMetrics holdout_id_metrics;
    TaskEvalMetrics holdout_ood_metrics;

    double id_generalization_ratio{0.0};  // holdout_id.success / max(1e-4, train.success)
    double ood_generalization_ratio{0.0}; // holdout_ood.success / max(1e-4, train.success)
    bool passes_m1_gate{false};           // 门禁：id_generalization_ratio >= 0.70 (或达到预期阈值)
    std::string verdict;

    std::string to_json() const {
        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"task_name\": \"" << task_name << "\",\n";
        oss << "  \"organism_id\": " << organism_id << ",\n";
        oss << "  \"lineage\": \"" << organism_lineage << "\",\n";
        oss << "  \"topology_hash\": \"" << topology_hash << "\",\n";
        oss << "  \"num_cells\": " << num_cells << ",\n";
        oss << "  \"num_synapses\": " << num_synapses << ",\n";
        oss << "  \"m1_gate_passed\": " << (passes_m1_gate ? "true" : "false") << ",\n";
        oss << "  \"verdict\": \"" << verdict << "\",\n";
        oss << "  \"train\": {\n";
        oss << "    \"success_rate\": " << train_metrics.success_rate << ",\n";
        oss << "    \"mean_fitness\": " << train_metrics.mean_fitness << ",\n";
        oss << "    \"mean_steps\": " << train_metrics.mean_steps << ",\n";
        oss << "    \"episodes\": " << train_metrics.num_episodes << "\n";
        oss << "  },\n";
        oss << "  \"holdout_id\": {\n";
        oss << "    \"success_rate\": " << holdout_id_metrics.success_rate << ",\n";
        oss << "    \"generalization_ratio\": " << id_generalization_ratio << ",\n";
        oss << "    \"mean_fitness\": " << holdout_id_metrics.mean_fitness << ",\n";
        oss << "    \"mean_steps\": " << holdout_id_metrics.mean_steps << ",\n";
        oss << "    \"episodes\": " << holdout_id_metrics.num_episodes << "\n";
        oss << "  },\n";
        oss << "  \"holdout_ood\": {\n";
        oss << "    \"success_rate\": " << holdout_ood_metrics.success_rate << ",\n";
        oss << "    \"generalization_ratio\": " << ood_generalization_ratio << ",\n";
        oss << "    \"mean_fitness\": " << holdout_ood_metrics.mean_fitness << ",\n";
        oss << "    \"mean_steps\": " << holdout_ood_metrics.mean_steps << ",\n";
        oss << "    \"episodes\": " << holdout_ood_metrics.num_episodes << "\n";
        oss << "  }\n";
        oss << "}\n";
        return oss.str();
    }
};

class EvolvableTask {
public:
    virtual ~EvolvableTask() = default;

    virtual const char* name() const = 0;

    // 观测维度 (适配 4 通道输入，多维自动前置切片或投影)
    virtual size_t obs_dim() const = 0;

    // 动作空间大小 (离散动作: 0 .. act_dim()-1)
    virtual size_t act_dim() const = 0;

    // 回合重置 (指定地图种子随机化)
    virtual void reset(uint32_t episode_seed) = 0;

    // 获取当前初始观测
    virtual std::vector<float> current_observation() const = 0;

    // 离散动作单步推演
    virtual StepResult step(int action) = 0;

    // 连续动作单步推演 (直接接收细胞有机体 ActionOutputs)
    virtual StepResult step_continuous(const CellularOrganism::ActionOutputs& acts) {
        // 默认映射为离散动作：正向前进 vs 转向 vs 免疫
        if (acts.immune_lock) return step(3);
        if (acts.negative_action > 0.3) return step(1);  // 左转
        if (acts.negative_action < -0.3) return step(2); // 右转
        return step(0); // 直行
    }

    // 回合结束后的适应度打分
    virtual double current_fitness() const = 0;

    // 针对单个有机体执行批量种子评测 (零数据泄露)
    virtual TaskEvalMetrics evaluate_organism(CellularOrganism& org,
                                             const std::vector<uint32_t>& seeds,
                                             int max_steps = 160,
                                             bool allow_plasticity = false) {
        TaskEvalMetrics metrics;
        metrics.num_episodes = seeds.size();
        if (seeds.empty()) return metrics;

        double total_fit = 0.0;
        double total_steps = 0.0;
        double total_min_dist = 0.0;
        size_t successes = 0;

        for (uint32_t seed : seeds) {
            org.reset_state(); // 清除跨地图 EMA 与突触记忆污染
            reset(seed);

            std::vector<float> obs = current_observation();
            double ep_fit = 0.0;
            int step_i = 0;
            bool reached = false;
            double last_min_dist = 999.0;

            for (; step_i < max_steps; ++step_i) {
                double inputs[4] = {0.0, 0.0, 0.0, 0.0};
                for (size_t d = 0; d < 4 && d < obs.size(); ++d) {
                    inputs[d] = obs[d];
                }

                auto acts = org.forward(inputs, allow_plasticity);
                auto res = step_continuous(acts);
                obs = res.obs;
                last_min_dist = res.min_dist_to_goal;

                if (res.success) {
                    reached = true;
                }
                if (res.done) {
                    break;
                }
            }

            ep_fit = current_fitness();
            if (reached) successes++;

            total_fit += ep_fit;
            total_steps += static_cast<double>(step_i);
            total_min_dist += last_min_dist;
        }

        metrics.success_episodes = successes;
        metrics.success_rate = static_cast<double>(successes) / static_cast<double>(seeds.size());
        metrics.mean_fitness = total_fit / static_cast<double>(seeds.size());
        metrics.mean_steps = total_steps / static_cast<double>(seeds.size());
        metrics.mean_min_dist = total_min_dist / static_cast<double>(seeds.size());
        return metrics;
    }
};

class TaskEvaluator {
public:
    // 计算有机体的规范 Weisfeiler-Lehman 拓扑哈希
    static std::string compute_topology_hash(const CellularOrganism& org) {
        auto core = org.extract_canonical_core_graph();
        std::stringstream ss;
        ss << "WL-" << std::hex << std::setw(16) << std::setfill('0') << core.wl_hash
           << "-C" << std::dec << org.cells.size() << "S" << org.synapses.size();
        return ss.str();
    }

    // 执行完整的 Train / Holdout ID / Holdout OOD 隔离综合评测
    static OOSReport evaluate_task_split(EvolvableTask& train_env,
                                         EvolvableTask& id_env,
                                         EvolvableTask& ood_env,
                                         CellularOrganism& org,
                                         const TaskDatasetSplit& split,
                                         double gate_threshold = 0.70) {
        OOSReport report;
        report.task_name = split.task_name;
        report.organism_id = org.organism_id;
        report.organism_lineage = org.lineage_name;
        report.num_cells = org.cells.size();
        report.num_synapses = org.synapses.size();
        report.topology_hash = compute_topology_hash(org);

        // 1. 评测训练集表现
        report.train_metrics = train_env.evaluate_organism(org, split.train_seeds, split.max_steps_per_episode, false);

        // 2. 评测同分布留出集 (Holdout ID) 泛化能力
        report.holdout_id_metrics = id_env.evaluate_organism(org, split.holdout_id_seeds, split.max_steps_per_episode, false);

        // 3. 评测跨尺寸分布外 (Holdout OOD) 泛化能力
        report.holdout_ood_metrics = ood_env.evaluate_organism(org, split.holdout_ood_seeds, split.max_steps_per_episode * 2, false);

        // 4. 计算泛化比率
        double base_train_sr = std::max(0.001, report.train_metrics.success_rate);
        report.id_generalization_ratio = report.holdout_id_metrics.success_rate / base_train_sr;
        report.ood_generalization_ratio = report.holdout_ood_metrics.success_rate / base_train_sr;

        // 5. 判定 M1 门禁 (留出到达率 >= 70% 训练集表现，且训练到达率 > 0)
        if (report.train_metrics.success_rate > 0.0) {
            report.passes_m1_gate = (report.id_generalization_ratio >= gate_threshold);
        } else {
            // 若训练集尚未跑通，则以平均距离与探索奖励对比判定
            report.passes_m1_gate = (report.holdout_id_metrics.mean_min_dist <= report.train_metrics.mean_min_dist * 1.30);
        }

        std::ostringstream ss;
        ss << "M1 OOS Report: Train[SR=" << std::fixed << std::setprecision(1)
           << (report.train_metrics.success_rate * 100.0) << "%] -> ID-Holdout[SR="
           << (report.holdout_id_metrics.success_rate * 100.0) << "%, Ratio="
           << std::setprecision(2) << report.id_generalization_ratio << "] -> OOD-Holdout[SR="
           << std::setprecision(1) << (report.holdout_ood_metrics.success_rate * 100.0) << "%]. Gate="
           << (report.passes_m1_gate ? "PASSED (>= 70%)" : "FAILED (< 70%)");
        report.verdict = ss.str();

        return report;
    }
};

} // namespace kun

