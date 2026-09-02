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
#include "kun/cellular/autonomous_replicator.hpp"

namespace kun {

// ============================================================================
// 1. 生态位公会与表型策略分类 (Ecological Guilds & Metabolic Phenotypes)
// ============================================================================
enum class SpeciesGuild : uint8_t {
    FAST_FORAGER = 0,     // 强效觅食型 (高吸收、高能耗、高繁衍速度、对饥荒敏感)
    FRUGAL_SURVIVOR = 1,  // 节俭耐受型 (低能耗、抗饥荒、深度休眠韧性、繁衍速度慢)
    DETRITUS_SCAVENGER = 2 // 腐生分解型 (能降解毒性废物并转化为养分，具有生态净化功能)
};

inline const char* to_string(SpeciesGuild guild) {
    switch (guild) {
        case SpeciesGuild::FAST_FORAGER:      return "FAST_FORAGER";
        case SpeciesGuild::FRUGAL_SURVIVOR:   return "FRUGAL_SURVIVOR";
        case SpeciesGuild::DETRITUS_SCAVENGER: return "DETRITUS_SCAVENGER";
        default: return "UNKNOWN_GUILD";
    }
}

// ============================================================================
// 2. 多生态位多策略个体 (Multi-Species Ecological Organism)
// ============================================================================
class EcologicalOrganism {
public:
    EcologicalOrganism(uint64_t id, uint64_t parent_id, uint32_t gen, uint32_t comp_id, 
                       SpeciesGuild guild, ReplicableGenome genome, double initial_energy)
        : organism_id_(id), parent_id_(parent_id), generation_(gen), 
          compartment_id_(comp_id), guild_(guild), genome_(std::move(genome)) {
        
        homeostasis_.cell_id = static_cast<uint32_t>(id);
        homeostasis_.compartment_id = comp_id;
        homeostasis_.energy_reserve = initial_energy;
        homeostasis_.is_alive = true;

        // 根据所属生态位公会配置特化表型动力学参数
        switch (guild_) {
            case SpeciesGuild::FAST_FORAGER:
                homeostasis_.max_energy_capacity = 220.0;
                homeostasis_.basal_metabolic_rate = 0.35; // 高基础能耗
                max_intake_rate_ = 3.5;                  // 强吸收能力
                migration_tendency_ = 0.25;              // 高迁移意愿
                break;

            case SpeciesGuild::FRUGAL_SURVIVOR:
                homeostasis_.max_energy_capacity = 140.0;
                homeostasis_.basal_metabolic_rate = 0.10; // 极低能耗
                max_intake_rate_ = 1.0;                  // 低吸收
                migration_tendency_ = 0.05;              // 定居倾向高
                break;

            case SpeciesGuild::DETRITUS_SCAVENGER:
                homeostasis_.max_energy_capacity = 160.0;
                homeostasis_.basal_metabolic_rate = 0.20;
                max_intake_rate_ = 1.8;
                detox_efficiency_ = 0.40;                // 强力净化转化毒素
                migration_tendency_ = 0.15;
                break;
        }
    }

    uint64_t get_id() const { return organism_id_; }
    uint64_t get_parent_id() const { return parent_id_; }
    uint32_t get_generation() const { return generation_; }
    uint32_t get_compartment_id() const { return compartment_id_; }
    void set_compartment_id(uint32_t cid) { compartment_id_ = cid; homeostasis_.compartment_id = cid; }
    SpeciesGuild get_guild() const { return guild_; }

    CellHomeostasisNode& get_homeostasis() { return homeostasis_; }
    const CellHomeostasisNode& get_homeostasis() const { return homeostasis_; }
    const ReplicableGenome& get_genome() const { return genome_; }

    double get_max_intake() const { return max_intake_rate_; }
    double get_detox_efficiency() const { return detox_efficiency_; }
    double get_migration_tendency() const { return migration_tendency_; }

    // 自主繁衍判定 (不同公会具备不同的储能门槛)
    bool should_replicate(const SpatialCompartment& comp, size_t current_comp_pop) const {
        if (!homeostasis_.is_alive || homeostasis_.state != MetabolicState::ACTIVE) return false;
        if (current_comp_pop >= comp.carrying_capacity) return false;

        double threshold = (guild_ == SpeciesGuild::FAST_FORAGER) ? 75.0 : 
                           (guild_ == SpeciesGuild::FRUGAL_SURVIVOR) ? 95.0 : 85.0;
        return (homeostasis_.energy_reserve >= threshold);
    }

    // 执行子代分裂繁殖 (可能发生跨公会表型变异)
    std::unique_ptr<EcologicalOrganism> spawn_offspring(std::mt19937& rng, uint64_t next_id, double mutation_rate = 0.05) {
        double cost = (guild_ == SpeciesGuild::FAST_FORAGER) ? 30.0 : 40.0;
        if (homeostasis_.energy_reserve <= cost) return nullptr;

        homeostasis_.energy_reserve -= cost;
        double starter = homeostasis_.energy_reserve * 0.30;
        homeostasis_.energy_reserve -= starter;

        auto child_genome = genome_.replicate_with_mutation(rng, mutation_rate);

        // 极小概率表型漂移变异 (Phenotypic Speciation)
        SpeciesGuild child_guild = guild_;
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng) < (mutation_rate * 0.2)) {
            child_guild = static_cast<SpeciesGuild>(rng() % 3);
        }

        return std::make_unique<EcologicalOrganism>(
            next_id,
            organism_id_,
            generation_ + 1,
            compartment_id_,
            child_guild,
            std::move(child_genome),
            starter
        );
    }

    // 自主跨隔室空间迁徙决策 (Spatial Migration Decision)
    bool try_migrate(std::mt19937& rng, std::vector<SpatialCompartment>& comps) {
        if (comps.size() <= 1 || homeostasis_.energy_reserve < 25.0) return false;

        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng) > migration_tendency_) return false;

        // 寻找局部邻近或富集隔室
        uint32_t target_comp = (compartment_id_ + 1 + (rng() % (comps.size() - 1))) % comps.size();
        
        // 如果目标隔室营养显著高于当前隔室，执行迁徙
        if (comps[target_comp].nutrient_concentration > comps[compartment_id_].nutrient_concentration * 1.2) {
            double migration_cost = 5.0; // 迁徙耗费物理动能
            homeostasis_.energy_reserve -= migration_cost;
            compartment_id_ = target_comp;
            homeostasis_.compartment_id = target_comp;
            return true;
        }
        return false;
    }

private:
    uint64_t organism_id_{0};
    uint64_t parent_id_{0};
    uint32_t generation_{0};
    uint32_t compartment_id_{0};
    SpeciesGuild guild_{SpeciesGuild::FAST_FORAGER};
    ReplicableGenome genome_;
    CellHomeostasisNode homeostasis_;

    double max_intake_rate_{2.0};
    double detox_efficiency_{0.0};
    double migration_tendency_{0.10};
};

// ============================================================================
// 3. 多种群生态位演化世界 (Multi-Species Ecosystem World - Phase 3 Core)
// ============================================================================
class MultiSpeciesEcosystemWorld {
public:
    struct MultiSpeciesTelemetry {
        uint64_t tick{0};
        size_t total_population{0};
        size_t forager_count{0};
        size_t survivor_count{0};
        size_t scavenger_count{0};
        size_t total_migrations{0};
        double global_waste_purified{0.0};
        double total_nutrients{0.0};
        double total_toxicity{0.0};
    };

    MultiSpeciesEcosystemWorld(size_t pop_per_guild = 8, size_t num_compartments = 4, uint32_t seed = 42)
        : rng_(seed) {
        init_ecosystem(pop_per_guild, num_compartments);
    }

    void init_ecosystem(size_t pop_per_guild, size_t num_compartments) {
        compartments_.clear();
        organisms_.clear();
        history_.clear();
        next_org_id_ = 1;

        for (uint32_t i = 0; i < num_compartments; ++i) {
            SpatialCompartment comp;
            comp.compartment_id = i;
            comp.nutrient_concentration = 2000.0;
            comp.waste_toxicity = 0.0;
            comp.membrane_permeability = 0.15;
            comp.carrying_capacity = 50;
            compartments_.push_back(comp);
        }

        // 初始化三类不同生态位个体
        SpeciesGuild guilds[3] = {SpeciesGuild::FAST_FORAGER, SpeciesGuild::FRUGAL_SURVIVOR, SpeciesGuild::DETRITUS_SCAVENGER};
        for (auto g : guilds) {
            for (size_t i = 0; i < pop_per_guild; ++i) {
                ReplicableGenome genome;
                genome.genome_id = next_org_id_;
                genome.lineage_hash = std::string("lineage_") + to_string(g);
                for (uint32_t locus = 0; locus < 6; ++locus) {
                    genome.loci.push_back(GeneLocus{locus, static_cast<uint8_t>(locus % 4), 0.20, 0.08, 1.0, static_cast<uint32_t>(i % num_compartments)});
                }
                auto org = std::make_unique<EcologicalOrganism>(
                    next_org_id_++,
                    0,
                    0,
                    static_cast<uint32_t>(i % num_compartments),
                    g,
                    std::move(genome),
                    65.0
                );
                organisms_.push_back(std::move(org));
            }
        }
    }

    // 执行单步多种群竞争、共生净化与空间演化
    MultiSpeciesTelemetry tick(double external_nutrient_influx = 50.0, double external_toxic_shock = 0.0, double mutation_rate = 0.05) {
        current_tick_++;
        size_t migrations_in_tick = 0;
        double waste_purified_in_tick = 0.0;

        // 1. 外部环境营养输入与毒性耗散
        double influx_per_comp = external_nutrient_influx / std::max<size_t>(1, compartments_.size());
        for (auto& comp : compartments_) {
            comp.nutrient_concentration += influx_per_comp;
            comp.waste_toxicity += external_toxic_shock;
            comp.waste_toxicity = std::max(0.0, comp.waste_toxicity * 0.96);
        }

        // 2. 统计各隔室实时种群密度
        std::vector<size_t> comp_population(compartments_.size(), 0);
        for (const auto& org : organisms_) {
            if (org->get_homeostasis().is_alive) {
                comp_population[org->get_compartment_id()]++;
            }
        }

        // 3. 代谢、共生分解、自主修复与存活评估
        for (auto& org : organisms_) {
            if (!org->get_homeostasis().is_alive) continue;

            auto& h = org->get_homeostasis();
            auto& comp = compartments_[org->get_compartment_id()];

            // A. 腐生分解者共生净化：将毒素分解为可用营养物质 (Bioremediation)
            if (org->get_guild() == SpeciesGuild::DETRITUS_SCAVENGER && comp.waste_toxicity > 0.5) {
                double detox_amt = std::min(comp.waste_toxicity, org->get_detox_efficiency());
                comp.waste_toxicity -= detox_amt;
                comp.nutrient_concentration += detox_amt * 1.5; // 净化再生营养
                h.energy_reserve += detox_amt * 0.8;            // 分解者自身获能
                waste_purified_in_tick += detox_amt;
            }

            // B. 差异化养分摄取
            double absorb_demand = std::max(0.0, h.max_energy_capacity - h.energy_reserve);
            double actual_absorb = std::min(absorb_demand, std::min(org->get_max_intake(), comp.nutrient_concentration * 0.02));
            h.energy_reserve += actual_absorb;
            comp.nutrient_concentration -= actual_absorb;

            // C. 毒性侵蚀 (耐受型与分解者具备高抗毒)
            double tox_mult = (org->get_guild() == SpeciesGuild::FRUGAL_SURVIVOR) ? 0.02 : 
                              (org->get_guild() == SpeciesGuild::DETRITUS_SCAVENGER) ? 0.01 : 0.06;
            if (comp.waste_toxicity > 3.0) {
                h.damage_level += (comp.waste_toxicity - 3.0) * tox_mult;
            }

            // D. 自修复
            if (h.damage_level > 5.0 && h.energy_reserve > 18.0) {
                double repair = std::min(h.damage_level, h.repair_rate);
                double cost = repair * h.repair_energy_cost;
                if (h.energy_reserve >= cost) {
                    h.energy_reserve -= cost;
                    h.damage_level -= repair;
                }
            }

            // E. 代谢消耗
            double cost = (h.state == MetabolicState::DORMANT) ? (h.basal_metabolic_rate * 0.15) : h.basal_metabolic_rate;
            h.energy_reserve -= cost;
            comp.waste_toxicity += cost * 0.02;

            // F. 存活状态机
            if (h.energy_reserve <= 0.0 || h.damage_level >= 100.0) {
                h.state = MetabolicState::APOPTOTIC;
                h.is_alive = false;
                comp.nutrient_concentration += 3.0; // 残存物质回收
            } else if (h.energy_reserve < 12.0) {
                h.state = MetabolicState::DORMANT;
            } else {
                h.state = MetabolicState::ACTIVE;
            }
        }

        // 4. 自主跨隔室迁徙 (Autonomous Spatial Migration)
        for (auto& org : organisms_) {
            if (org->get_homeostasis().is_alive && org->get_homeostasis().state == MetabolicState::ACTIVE) {
                if (org->try_migrate(rng_, compartments_)) {
                    migrations_in_tick++;
                }
            }
        }

        // 5. 自主繁衍与子代产生
        std::vector<std::unique_ptr<EcologicalOrganism>> new_offspring;
        for (auto& org : organisms_) {
            if (!org->get_homeostasis().is_alive) continue;

            uint32_t cid = org->get_compartment_id();
            if (org->should_replicate(compartments_[cid], comp_population[cid])) {
                auto child = org->spawn_offspring(rng_, next_org_id_++, mutation_rate);
                if (child) {
                    comp_population[cid]++;
                    new_offspring.push_back(std::move(child));
                }
            }
        }

        for (auto& child : new_offspring) {
            organisms_.push_back(std::move(child));
        }

        // 6. 清理凋亡个体
        organisms_.erase(
            std::remove_if(organisms_.begin(), organisms_.end(), [](const auto& org) {
                return !org->get_homeostasis().is_alive;
            }),
            organisms_.end()
        );

        // 7. 遥测统计
        size_t c_forager = 0, c_survivor = 0, c_scavenger = 0;
        for (const auto& org : organisms_) {
            switch (org->get_guild()) {
                case SpeciesGuild::FAST_FORAGER: c_forager++; break;
                case SpeciesGuild::FRUGAL_SURVIVOR: c_survivor++; break;
                case SpeciesGuild::DETRITUS_SCAVENGER: c_scavenger++; break;
            }
        }

        double total_n = 0.0, total_t = 0.0;
        for (const auto& comp : compartments_) {
            total_n += comp.nutrient_concentration;
            total_t += comp.waste_toxicity;
        }

        MultiSpeciesTelemetry frame{
            current_tick_,
            organisms_.size(),
            c_forager,
            c_survivor,
            c_scavenger,
            migrations_in_tick,
            waste_purified_in_tick,
            total_n,
            total_t
        };
        history_.push_back(frame);
        return frame;
    }

    const std::vector<std::unique_ptr<EcologicalOrganism>>& get_organisms() const { return organisms_; }
    const std::vector<SpatialCompartment>& get_compartments() const { return compartments_; }
    std::vector<SpatialCompartment>& get_compartments() { return compartments_; }
    const std::vector<MultiSpeciesTelemetry>& get_history() const { return history_; }

private:
    uint64_t current_tick_{0};
    uint64_t next_org_id_{1};
    std::mt19937 rng_;
    std::vector<SpatialCompartment> compartments_;
    std::vector<std::unique_ptr<EcologicalOrganism>> organisms_;
    std::vector<MultiSpeciesTelemetry> history_;
};

} // namespace kun
