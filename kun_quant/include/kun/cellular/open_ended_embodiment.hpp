#pragma once

#include <vector>
#include <string>
#include <memory>
#include <random>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <stdexcept>
#include "kun/cellular/digital_homeostasis.hpp"
#include "kun/cellular/autonomous_replicator.hpp"
#include "kun/cellular/multispecies_ecology.hpp"

namespace kun {

// ============================================================================
// 1. 具身感知与执行器物理通道 (Embodied Sensory-Motor Interface)
// ============================================================================
struct EmbodiedSensors {
    double nutrient_gradient_x{0.0};  // 前方营养浓度空间梯度 X
    double nutrient_gradient_y{0.0};  // 前方营养浓度空间梯度 Y
    double hazard_proximity{0.0};     // 障碍物/毒性风暴逼近程度 [0.0, 1.0]
    double internal_energy_ratio{1.0};// 自身内部能量充盈比率 [0.0, 1.0]
};

struct EmbodiedActuators {
    double thrust_x{0.0};             // 物理推力 X [-1.0, 1.0]
    double thrust_y{0.0};             // 物理推力 Y [-1.0, 1.0]
    double feed_intake_effort{0.0};   // 主动觅食吞吐功率 [0.0, 1.0]
    bool   deploy_shield{false};      // 是否激活抗毒防护屏障 (消耗额外能量)
};

// ============================================================================
// 2. 开放式自发计算通路单元 (Compositional Emergent Pathway)
// ============================================================================
struct CompositionalPathway {
    uint32_t pathway_id{0};
    std::string channel_signature;    // 新涌现的信号通道特征指纹
    double temporal_oscillation_freq{0.1}; // 动力学自激振荡频率
    double sensory_coupling_gain{1.0};     // 感知耦合增益
    double motor_drive_gain{1.0};          // 运动执行增益
    bool is_novel_circuit{false};          // 是否为自发组合生成的新通路
};

// ============================================================================
// 3. 具身自主生命体个体 (Open-Ended Embodied Agent)
// ============================================================================
class OpenEndedEmbodiedAgent {
public:
    OpenEndedEmbodiedAgent(uint64_t id, uint64_t parent_id, uint32_t gen, double x, double y, ReplicableGenome genome, double initial_energy)
        : agent_id_(id), parent_id_(parent_id), generation_(gen), pos_x_(x), pos_y_(y), genome_(std::move(genome)) {
        
        homeostasis_.cell_id = static_cast<uint32_t>(id);
        homeostasis_.energy_reserve = initial_energy;
        homeostasis_.max_energy_capacity = 250.0;
        homeostasis_.basal_metabolic_rate = 0.25;
        homeostasis_.is_alive = true;

        // 初始化基础感知-执行通路
        pathway_.pathway_id = static_cast<uint32_t>(id);
        pathway_.channel_signature = "BASE_CIRCUIT_S2M";
        pathway_.temporal_oscillation_freq = 0.15;
        pathway_.sensory_coupling_gain = 1.2;
        pathway_.motor_drive_gain = 1.0;
        pathway_.is_novel_circuit = false;
    }

    uint64_t get_id() const { return agent_id_; }
    uint64_t get_parent_id() const { return parent_id_; }
    uint32_t get_generation() const { return generation_; }
    double get_x() const { return pos_x_; }
    double get_y() const { return pos_y_; }
    void set_pos(double x, double y) { pos_x_ = x; pos_y_ = y; }

    CellHomeostasisNode& get_homeostasis() { return homeostasis_; }
    const CellHomeostasisNode& get_homeostasis() const { return homeostasis_; }
    const CompositionalPathway& get_pathway() const { return pathway_; }
    CompositionalPathway& get_pathway() { return pathway_; }

    // 【身体-环境闭环计算】: 输入感知信号 -> 驱动内生神经回路 -> 输出动作指令并结算生理能耗
    EmbodiedActuators process_embodied_cycle(const EmbodiedSensors& sensors, double dt = 1.0) {
        EmbodiedActuators act;
        if (!homeostasis_.is_alive || homeostasis_.state == MetabolicState::APOPTOTIC) {
            return act;
        }

        // 1. 休眠态抑制动作输出，保留微量感知
        if (homeostasis_.state == MetabolicState::DORMANT) {
            homeostasis_.energy_reserve -= homeostasis_.basal_metabolic_rate * 0.15 * dt;
            return act;
        }

        // 2. 动力学通路推演 (基于感觉梯度 + 内部能量状态 + 通路动力学调制)
        internal_oscillation_phase_ += pathway_.temporal_oscillation_freq * dt;
        double osc = std::sin(internal_oscillation_phase_);

        // 趋利避害动力学推演
        double drive_x = sensors.nutrient_gradient_x * pathway_.sensory_coupling_gain + osc * 0.1;
        double drive_y = sensors.nutrient_gradient_y * pathway_.sensory_coupling_gain + std::cos(internal_oscillation_phase_) * 0.1;

        // 危险规避：如果风暴逼近，优先产生反向推力并考虑开启防护
        if (sensors.hazard_proximity > 0.4) {
            drive_x -= sensors.nutrient_gradient_x * 1.5;
            drive_y -= sensors.nutrient_gradient_y * 1.5;
            if (sensors.hazard_proximity > 0.7 && homeostasis_.energy_reserve > 30.0) {
                act.deploy_shield = true;
            }
        }

        act.thrust_x = std::clamp(drive_x * pathway_.motor_drive_gain, -1.0, 1.0);
        act.thrust_y = std::clamp(drive_y * pathway_.motor_drive_gain, -1.0, 1.0);
        
        // 觅食意愿受饥饿度调节
        act.feed_intake_effort = std::clamp(1.2 - sensors.internal_energy_ratio, 0.2, 1.0);

        // 3. 结算物理行动能耗 (Kinetic & Metabolic Costs)
        double kinetic_cost = (act.thrust_x * act.thrust_x + act.thrust_y * act.thrust_y) * 0.12 * dt;
        double shield_cost = act.deploy_shield ? (0.35 * dt) : 0.0;
        double total_step_cost = (homeostasis_.basal_metabolic_rate * dt) + kinetic_cost + shield_cost;

        homeostasis_.energy_reserve -= total_step_cost;

        // 4. 稳态生死状态机更新
        if (homeostasis_.energy_reserve <= 0.0 || homeostasis_.damage_level >= 100.0) {
            homeostasis_.state = MetabolicState::APOPTOTIC;
            homeostasis_.is_alive = false;
        } else if (homeostasis_.energy_reserve < 15.0) {
            homeostasis_.state = MetabolicState::DORMANT;
        } else {
            homeostasis_.state = MetabolicState::ACTIVE;
        }

        return act;
    }

    // 开放式自发结构重组变异 (Generates Novel Emerging Functional Circuits)
    std::unique_ptr<OpenEndedEmbodiedAgent> spawn_embodied_offspring(std::mt19937& rng, uint64_t next_id, double mutation_rate = 0.08) {
        double cost = 45.0; // 繁衍合成物理代价
        if (homeostasis_.energy_reserve <= cost + 25.0) return nullptr;

        homeostasis_.energy_reserve -= cost;
        double starter = homeostasis_.energy_reserve * 0.35;
        homeostasis_.energy_reserve -= starter;

        auto child_genome = genome_.replicate_with_mutation(rng, mutation_rate);

        // 略微偏移的初生坐标
        std::normal_distribution<double> pos_dist(0.0, 2.0);
        double child_x = pos_x_ + pos_dist(rng);
        double child_y = pos_y_ + pos_dist(rng);

        auto child = std::make_unique<OpenEndedEmbodiedAgent>(
            next_id,
            agent_id_,
            generation_ + 1,
            child_x,
            child_y,
            std::move(child_genome),
            starter
        );

        // 继承亲代通路并有概率发生自发结构重组与新通道涌现
        child->pathway_ = this->pathway_;
        child->pathway_.pathway_id = static_cast<uint32_t>(next_id);

        std::uniform_real_distribution<double> dist(0.0, 1.0);
        std::normal_distribution<double> norm(0.0, 0.2);

        if (dist(rng) < mutation_rate) {
            // 参数连续漂移
            child->pathway_.temporal_oscillation_freq = std::clamp(child->pathway_.temporal_oscillation_freq + norm(rng) * 0.05, 0.02, 0.80);
            child->pathway_.sensory_coupling_gain = std::clamp(child->pathway_.sensory_coupling_gain + norm(rng), 0.2, 4.0);
            child->pathway_.motor_drive_gain = std::clamp(child->pathway_.motor_drive_gain + norm(rng), 0.2, 3.0);
        }

        // 【开放式新通道涌现】 (Emergence of Novel Channel Topology)
        if (dist(rng) < (mutation_rate * 0.35)) {
            child->pathway_.is_novel_circuit = true;
            std::stringstream ss;
            ss << "EMERGENT_CIRCUIT_GEN" << (generation_ + 1) << "_CH" << (rng() % 100);
            child->pathway_.channel_signature = ss.str();
        }

        return child;
    }

private:
    uint64_t agent_id_{0};
    uint64_t parent_id_{0};
    uint32_t generation_{0};
    double pos_x_{0.0};
    double pos_y_{0.0};
    double internal_oscillation_phase_{0.0};
    ReplicableGenome genome_;
    CellHomeostasisNode homeostasis_;
    CompositionalPathway pathway_;
};

// ============================================================================
// 4. 具身物理沙盒世界 (Embodied Continuous Physics Sandbox World - Phase 4)
// ============================================================================
class EmbodiedPhysicalSandboxWorld {
public:
    struct NutrientPatch {
        double x{0.0};
        double y{0.0};
        double radius{8.0};
        double energy_amount{500.0};
    };

    struct HazardStorm {
        double x{0.0};
        double y{0.0};
        double radius{12.0};
        double toxicity_intensity{20.0};
        double vel_x{0.2};
        double vel_y{0.1};
    };

    struct EmbodiedTelemetry {
        uint64_t tick{0};
        size_t alive_agents{0};
        uint32_t max_generation{0};
        size_t novel_circuits_count{0};
        double total_biomass_energy{0.0};
        double total_world_nutrients{0.0};
        double average_agent_speed{0.0};
    };

    EmbodiedPhysicalSandboxWorld(size_t initial_agents = 12, double world_size = 100.0, uint32_t seed = 42)
        : world_size_(world_size), rng_(seed) {
        init_sandbox(initial_agents);
    }

    void init_sandbox(size_t initial_agents) {
        if (initial_agents > kPopulationCapacity) {
            throw std::invalid_argument("initial agent population exceeds sandbox capacity");
        }
        if (world_size_ <= 0.0) {
            throw std::invalid_argument("sandbox world size must be positive");
        }
        agents_.clear();
        patches_.clear();
        storms_.clear();
        history_.clear();
        next_agent_id_ = 1;

        // 生成 4 个营养富集斑块 (Dynamic Food Patches)
        patches_.push_back(NutrientPatch{25.0, 25.0, 10.0, 1200.0});
        patches_.push_back(NutrientPatch{75.0, 75.0, 10.0, 1200.0});
        patches_.push_back(NutrientPatch{25.0, 75.0, 8.0, 800.0});
        patches_.push_back(NutrientPatch{75.0, 25.0, 8.0, 800.0});

        // 生成 1 个移动危险毒性风暴 (Moving Hazard Storm)
        storms_.push_back(HazardStorm{50.0, 50.0, 15.0, 25.0, 0.3, -0.2});

        // 随机分布初生具身个体
        std::uniform_real_distribution<double> pos_dist(10.0, world_size_ - 10.0);
        for (size_t i = 0; i < initial_agents; ++i) {
            ReplicableGenome genome;
            genome.genome_id = next_agent_id_;
            genome.lineage_hash = "embodied_root";
            for (uint32_t g = 0; g < 6; ++g) {
                genome.loci.push_back(GeneLocus{g, static_cast<uint8_t>(g % 4), 0.25, 0.10, 1.0, 0});
            }
            auto agent = std::make_unique<OpenEndedEmbodiedAgent>(
                next_agent_id_++,
                0,
                0,
                pos_dist(rng_),
                pos_dist(rng_),
                std::move(genome),
                80.0
            );
            agents_.push_back(std::move(agent));
        }
    }

    // 执行单步具身物理与自主演化循环
    EmbodiedTelemetry tick(double food_regeneration_rate = 15.0, double mutation_rate = 0.08) {
        current_tick_++;

        // 1. 物理环境动力学演进：营养再生与风暴移动 (Toroidal World Wrap)
        for (auto& patch : patches_) {
            patch.energy_amount = std::min(1500.0, patch.energy_amount + food_regeneration_rate / patches_.size());
        }
        for (auto& storm : storms_) {
            storm.x += storm.vel_x;
            storm.y += storm.vel_y;
            if (storm.x < 0.0) storm.x += world_size_;
            if (storm.x >= world_size_) storm.x -= world_size_;
            if (storm.y < 0.0) storm.y += world_size_;
            if (storm.y >= world_size_) storm.y -= world_size_;
        }

        // 2. 遍历个体：提取物理局域感知 -> 具身决策 -> 物理位移与环境交互
        double total_speed_sum = 0.0;
        for (auto& agent : agents_) {
            if (!agent->get_homeostasis().is_alive) continue;

            double ax = agent->get_x();
            double ay = agent->get_y();

            // A. 计算最近营养斑块的方向梯度
            double nearest_patch_dist = 1e9;
            double grad_x = 0.0, grad_y = 0.0;
            NutrientPatch* closest_patch = nullptr;

            for (auto& patch : patches_) {
                double dx = patch.x - ax;
                double dy = patch.y - ay;
                double d = std::sqrt(dx * dx + dy * dy);
                if (d < nearest_patch_dist) {
                    nearest_patch_dist = d;
                    closest_patch = &patch;
                    if (d > 0.001) {
                        grad_x = dx / d;
                        grad_y = dy / d;
                    }
                }
            }

            // B. 计算风暴危险逼近度
            double hazard_prox = 0.0;
            for (const auto& storm : storms_) {
                double dx = storm.x - ax;
                double dy = storm.y - ay;
                double d = std::sqrt(dx * dx + dy * dy);
                if (d < storm.radius) {
                    hazard_prox = std::max(hazard_prox, (storm.radius - d) / storm.radius);
                }
            }

            // C. 组装感觉信号并执行具身计算
            EmbodiedSensors sensors{
                grad_x,
                grad_y,
                hazard_prox,
                agent->get_homeostasis().energy_reserve / agent->get_homeostasis().max_energy_capacity
            };

            auto actuators = agent->process_embodied_cycle(sensors, 1.0);

            // D. 物理运动推演 (Kinematics & Toroidal Boundary)
            double vx = actuators.thrust_x * 2.0;
            double vy = actuators.thrust_y * 2.0;
            double next_x = ax + vx;
            double next_y = ay + vy;
            if (next_x < 0.0) next_x += world_size_;
            if (next_x >= world_size_) next_x -= world_size_;
            if (next_y < 0.0) next_y += world_size_;
            if (next_y >= world_size_) next_y -= world_size_;
            agent->set_pos(next_x, next_y);
            total_speed_sum += std::sqrt(vx * vx + vy * vy);

            // E. 物理接触觅食交互 (Physical Ingestion)
            if (closest_patch && nearest_patch_dist <= closest_patch->radius && closest_patch->energy_amount > 1.0) {
                double absorb_demand = std::max(0.0, agent->get_homeostasis().max_energy_capacity - agent->get_homeostasis().energy_reserve);
                double ingest_amount = std::min(absorb_demand, std::min(4.0 * actuators.feed_intake_effort, closest_patch->energy_amount * 0.05));
                agent->get_homeostasis().energy_reserve += ingest_amount;
                closest_patch->energy_amount -= ingest_amount;
            }

            // F. 危险风暴毒性损伤 (Shielding Barrier Defense)
            if (hazard_prox > 0.0) {
                double damage_incoming = hazard_prox * 8.0;
                if (actuators.deploy_shield) {
                    damage_incoming *= 0.15; // 护盾抵消 85% 伤害
                }
                agent->get_homeostasis().damage_level += damage_incoming;
            }
        }

        // 3. 自发内生繁衍与子代产生
        std::vector<std::unique_ptr<OpenEndedEmbodiedAgent>> new_offspring;
        size_t projected_population = agents_.size();
        for (auto& agent : agents_) {
            if (!agent->get_homeostasis().is_alive || agent->get_homeostasis().state != MetabolicState::ACTIVE) continue;

            if (agent->get_homeostasis().energy_reserve >= 110.0 &&
                projected_population < kPopulationCapacity) {
                ++projected_population;
                auto child = agent->spawn_embodied_offspring(rng_, next_agent_id_++, mutation_rate);
                if (child) {
                    new_offspring.push_back(std::move(child));
                } else {
                    --projected_population;
                }
            }
        }

        for (auto& child : new_offspring) {
            agents_.push_back(std::move(child));
        }

        // 4. 清理凋亡个体
        agents_.erase(
            std::remove_if(agents_.begin(), agents_.end(), [](const auto& a) {
                return !a->get_homeostasis().is_alive;
            }),
            agents_.end()
        );

        // 5. 遥测统计
        uint32_t max_gen = 0;
        size_t novel_circuits = 0;
        double total_biomass = 0.0;
        for (const auto& a : agents_) {
            if (a->get_generation() > max_gen) max_gen = a->get_generation();
            if (a->get_pathway().is_novel_circuit) novel_circuits++;
            total_biomass += a->get_homeostasis().energy_reserve;
        }

        double total_nutrients = 0.0;
        for (const auto& p : patches_) total_nutrients += p.energy_amount;

        EmbodiedTelemetry telemetry{
            current_tick_,
            agents_.size(),
            max_gen,
            novel_circuits,
            total_biomass,
            total_nutrients,
            agents_.empty() ? 0.0 : (total_speed_sum / agents_.size())
        };
        history_.push_back(telemetry);
        return telemetry;
    }

    const std::vector<std::unique_ptr<OpenEndedEmbodiedAgent>>& get_agents() const { return agents_; }
    const std::vector<NutrientPatch>& get_patches() const { return patches_; }
    const std::vector<EmbodiedTelemetry>& get_history() const { return history_; }

private:
    static constexpr size_t kPopulationCapacity = 60;
    double world_size_{100.0};
    uint64_t current_tick_{0};
    uint64_t next_agent_id_{1};
    std::mt19937 rng_;
    std::vector<NutrientPatch> patches_;
    std::vector<HazardStorm> storms_;
    std::vector<std::unique_ptr<OpenEndedEmbodiedAgent>> agents_;
    std::vector<EmbodiedTelemetry> history_;
};

} // namespace kun
