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
#include "kun/cellular/digital_homeostasis.hpp"

namespace kun {

// ============================================================================
// 1. 结构基因单元 (Structural Gene Unit)
// ============================================================================
struct GeneLocus {
    uint32_t gene_id{0};
    uint8_t op_type{0};             // 细胞算子原语类型
    double base_metabolic_rate{0.20}; // 先天代谢基线
    double firing_cost{0.08};       // 先天放电能耗
    double weight_param{1.0};       // 突触突触权重 / 传导强度
    uint32_t target_compartment{0}; // 空间隔室亲和力
};

// ============================================================================
// 2. 自包含可复制基因组 (Self-Contained Replicable Genome)
// ============================================================================
struct ReplicableGenome {
    uint64_t genome_id{0};
    std::string lineage_hash;       // 谱系遗传指纹
    std::vector<GeneLocus> loci;    // 基因位点序列
    double base_replication_cost{35.0}; // 复制单位后代所需的基准能量消耗 (ATP)
    double replication_threshold{85.0}; // 触发自主繁衍的储能阈值 (ATP)

    // 基因组复制与变异 (内生复制过程，带可变突变率)
    ReplicableGenome replicate_with_mutation(std::mt19937& rng, double mutation_rate = 0.05) const {
        ReplicableGenome child = *this;
        child.genome_id = rng();

        std::uniform_real_distribution<double> dist(0.0, 1.0);
        std::normal_distribution<double> norm(0.0, 0.15);

        for (auto& locus : child.loci) {
            if (dist(rng) < mutation_rate) {
                // 点突变：权重扰动
                locus.weight_param += norm(rng);
                locus.weight_param = std::clamp(locus.weight_param, -3.0, 3.0);
            }
            if (dist(rng) < (mutation_rate * 0.4)) {
                // 代谢基因突变：代谢能耗微调
                locus.base_metabolic_rate *= (1.0 + norm(rng) * 0.1);
                locus.base_metabolic_rate = std::clamp(locus.base_metabolic_rate, 0.05, 0.80);
            }
        }

        // 基因插入/缺失变异 (Indel Mutation)
        if (dist(rng) < (mutation_rate * 0.3) && child.loci.size() < 32) {
            GeneLocus new_locus{
                static_cast<uint32_t>(child.loci.size()),
                static_cast<uint8_t>(rng() % 8),
                0.20,
                0.08,
                norm(rng),
                0
            };
            child.loci.push_back(new_locus);
        } else if (dist(rng) < (mutation_rate * 0.2) && child.loci.size() > 3) {
            child.loci.pop_back();
        }

        // 重新生成后代谱系指纹
        std::stringstream ss;
        ss << std::hex << (rng() ^ (child.genome_id << 4));
        child.lineage_hash = ss.str().substr(0, 8);
        return child;
    }
};

// ============================================================================
// 3. 自主复制生命体个体 (Autonomous Replicator Organism)
// ============================================================================
class AutonomousReplicatorOrganism {
public:
    AutonomousReplicatorOrganism(uint64_t id, uint64_t parent_id, uint32_t gen, uint32_t comp_id, ReplicableGenome genome, double initial_energy)
        : organism_id_(id), parent_id_(parent_id), generation_(gen), compartment_id_(comp_id), genome_(std::move(genome)) {
        homeostasis_.cell_id = static_cast<uint32_t>(id);
        homeostasis_.compartment_id = comp_id;
        homeostasis_.energy_reserve = initial_energy;
        homeostasis_.max_energy_capacity = 180.0;
        homeostasis_.basal_metabolic_rate = 0.20;
        homeostasis_.is_alive = true;
    }

    uint64_t get_id() const { return organism_id_; }
    uint64_t get_parent_id() const { return parent_id_; }
    uint32_t get_generation() const { return generation_; }
    uint32_t get_compartment_id() const { return compartment_id_; }
    void set_compartment_id(uint32_t cid) { compartment_id_ = cid; homeostasis_.compartment_id = cid; }

    CellHomeostasisNode& get_homeostasis() { return homeostasis_; }
    const CellHomeostasisNode& get_homeostasis() const { return homeostasis_; }
    const ReplicableGenome& get_genome() const { return genome_; }

    // 自主繁衍判定：内部能量达到繁衍阈值，且隔室未超载
    bool should_replicate(const SpatialCompartment& comp, size_t current_comp_population) const {
        if (!homeostasis_.is_alive || homeostasis_.state != MetabolicState::ACTIVE) {
            return false;
        }
        // 能量充盈度检查
        if (homeostasis_.energy_reserve < genome_.replication_threshold) {
            return false;
        }
        // 隔室环境承载力密度阻滞 (Carrying Capacity Crowding Inhibition)
        if (current_comp_population >= comp.carrying_capacity) {
            return false;
        }
        return true;
    }

    // 执行内生自主分裂复制 (Spawns Child Organism, Deducts Parental Energy)
    std::unique_ptr<AutonomousReplicatorOrganism> spawn_offspring(std::mt19937& rng, uint64_t next_id, double mutation_rate = 0.05) {
        double cost = genome_.base_replication_cost;
        if (homeostasis_.energy_reserve <= cost) {
            return nullptr;
        }

        // 亲本扣除合成后代所消耗的底质能耗
        homeostasis_.energy_reserve -= cost;

        // 亲本将剩余储能的 30% 分配赠予初生子代作为启动资金
        double starter_energy = homeostasis_.energy_reserve * 0.30;
        homeostasis_.energy_reserve -= starter_energy;

        // 复制基因组并引入变异
        auto child_genome = genome_.replicate_with_mutation(rng, mutation_rate);

        return std::make_unique<AutonomousReplicatorOrganism>(
            next_id,
            organism_id_,
            generation_ + 1,
            compartment_id_,
            std::move(child_genome),
            starter_energy
        );
    }

private:
    uint64_t organism_id_{0};
    uint64_t parent_id_{0};
    uint32_t generation_{0};
    uint32_t compartment_id_{0};
    ReplicableGenome genome_;
    CellHomeostasisNode homeostasis_;
};

// ============================================================================
// 4. 去中心化自主繁衍数字生态世界 (Decentralized Ecological World)
// ============================================================================
class DecentralizedEcologicalWorld {
public:
    struct WorldTelemetry {
        uint64_t tick{0};
        size_t total_population{0};
        size_t total_births{0};
        size_t total_deaths{0};
        uint32_t max_generation{0};
        double average_energy{0.0};
        double global_nutrient_reserves{0.0};
        size_t unique_lineages_count{0};
    };

    DecentralizedEcologicalWorld(size_t initial_pop, size_t num_compartments = 4, uint32_t seed = 42)
        : rng_(seed) {
        init_world(initial_pop, num_compartments);
    }

    void init_world(size_t initial_pop, size_t num_compartments) {
        compartments_.clear();
        organisms_.clear();
        history_.clear();
        next_org_id_ = 1;

        for (uint32_t i = 0; i < num_compartments; ++i) {
            SpatialCompartment comp;
            comp.compartment_id = i;
            comp.nutrient_concentration = 2500.0;
            comp.waste_toxicity = 0.0;
            comp.membrane_permeability = 0.12;
            comp.carrying_capacity = 60; // 单隔室承载力上限
            compartments_.push_back(comp);
        }

        for (size_t i = 0; i < initial_pop; ++i) {
            ReplicableGenome genome;
            genome.genome_id = next_org_id_;
            genome.lineage_hash = "root_0";
            for (uint32_t g = 0; g < 6; ++g) {
                genome.loci.push_back(GeneLocus{g, static_cast<uint8_t>(g % 4), 0.20, 0.08, 1.0, static_cast<uint32_t>(i % num_compartments)});
            }
            auto org = std::make_unique<AutonomousReplicatorOrganism>(
                next_org_id_++,
                0, // 无父代 (Progenitor)
                0, // 第 0 代
                i % num_compartments,
                std::move(genome),
                70.0 // 初始充足储能
            );
            organisms_.push_back(std::move(org));
        }
    }

    // 执行单步去中心化生态演化闭环
    WorldTelemetry tick(double external_nutrient_influx = 60.0, double external_stress = 0.0, double mutation_rate = 0.05) {
        current_tick_++;
        size_t births_in_tick = 0;
        size_t deaths_in_tick = 0;

        // 1. 外部环境营养输入与毒性耗散
        double influx_per_comp = external_nutrient_influx / std::max<size_t>(1, compartments_.size());
        for (auto& comp : compartments_) {
            comp.nutrient_concentration += influx_per_comp;
            comp.waste_toxicity += external_stress;
            comp.waste_toxicity = std::max(0.0, comp.waste_toxicity * 0.95);
        }

        // 2. 统计各隔室实时种群密度
        std::vector<size_t> comp_population(compartments_.size(), 0);
        for (const auto& org : organisms_) {
            if (org->get_homeostasis().is_alive) {
                comp_population[org->get_compartment_id()]++;
            }
        }

        // 3. 代谢、损伤、自修复与存活评估
        for (auto& org : organisms_) {
            if (!org->get_homeostasis().is_alive) continue;

            auto& h = org->get_homeostasis();
            auto& comp = compartments_[org->get_compartment_id()];

            // 摄取养分
            double absorb_demand = std::max(0.0, h.max_energy_capacity - h.energy_reserve);
            double actual_absorb = std::min(absorb_demand, std::min(2.5, comp.nutrient_concentration * 0.02));
            h.energy_reserve += actual_absorb;
            comp.nutrient_concentration -= actual_absorb;

            // 环境毒性侵蚀
            if (comp.waste_toxicity > 4.0) {
                h.damage_level += (comp.waste_toxicity - 4.0) * 0.04;
            }

            // 自主修复
            if (h.damage_level > 5.0 && h.energy_reserve > 20.0) {
                double repair = std::min(h.damage_level, h.repair_rate);
                double cost = repair * h.repair_energy_cost;
                if (h.energy_reserve >= cost) {
                    h.energy_reserve -= cost;
                    h.damage_level -= repair;
                }
            }

            // 代谢消耗
            double cost = (h.state == MetabolicState::DORMANT) ? (h.basal_metabolic_rate * 0.15) : h.basal_metabolic_rate;
            h.energy_reserve -= cost;
            comp.waste_toxicity += cost * 0.03;

            // 存活状态机
            if (h.energy_reserve <= 0.0 || h.damage_level >= 100.0) {
                h.state = MetabolicState::APOPTOTIC;
                h.is_alive = false;
                comp.nutrient_concentration += 4.0; // 物质回归
                deaths_in_tick++;
            } else if (h.energy_reserve < 15.0) {
                h.state = MetabolicState::DORMANT;
            } else {
                h.state = MetabolicState::ACTIVE;
            }
        }

        // 4. 自主繁殖与去中心化子代生成 (Autonomous Reproduction)
        std::vector<std::unique_ptr<AutonomousReplicatorOrganism>> new_offspring;
        for (auto& org : organisms_) {
            if (!org->get_homeostasis().is_alive) continue;

            uint32_t cid = org->get_compartment_id();
            if (org->should_replicate(compartments_[cid], comp_population[cid])) {
                auto child = org->spawn_offspring(rng_, next_org_id_++, mutation_rate);
                if (child) {
                    comp_population[cid]++;
                    births_in_tick++;
                    new_offspring.push_back(std::move(child));
                }
            }
        }

        // 将子代并入世界
        for (auto& child : new_offspring) {
            organisms_.push_back(std::move(child));
        }

        // 5. 清理已死亡解离个体
        organisms_.erase(
            std::remove_if(organisms_.begin(), organisms_.end(), [](const auto& org) {
                return !org->get_homeostasis().is_alive;
            }),
            organisms_.end()
        );

        // 6. 遥测统计
        uint32_t max_gen = 0;
        double total_e = 0.0;
        std::vector<std::string> lineages;
        for (const auto& org : organisms_) {
            if (org->get_generation() > max_gen) max_gen = org->get_generation();
            total_e += org->get_homeostasis().energy_reserve;
            lineages.push_back(org->get_genome().lineage_hash);
        }
        std::sort(lineages.begin(), lineages.end());
        lineages.erase(std::unique(lineages.begin(), lineages.end()), lineages.end());

        double total_nutrients = 0.0;
        for (const auto& comp : compartments_) total_nutrients += comp.nutrient_concentration;

        WorldTelemetry telemetry{
            current_tick_,
            organisms_.size(),
            births_in_tick,
            deaths_in_tick,
            max_gen,
            organisms_.empty() ? 0.0 : (total_e / organisms_.size()),
            total_nutrients,
            lineages.size()
        };
        history_.push_back(telemetry);
        return telemetry;
    }

    const std::vector<std::unique_ptr<AutonomousReplicatorOrganism>>& get_organisms() const { return organisms_; }
    const std::vector<SpatialCompartment>& get_compartments() const { return compartments_; }
    std::vector<SpatialCompartment>& get_compartments() { return compartments_; }
    const std::vector<WorldTelemetry>& get_history() const { return history_; }

private:
    uint64_t current_tick_{0};
    uint64_t next_org_id_{1};
    std::mt19937 rng_;
    std::vector<SpatialCompartment> compartments_;
    std::vector<std::unique_ptr<AutonomousReplicatorOrganism>> organisms_;
    std::vector<WorldTelemetry> history_;
};

} // namespace kun
