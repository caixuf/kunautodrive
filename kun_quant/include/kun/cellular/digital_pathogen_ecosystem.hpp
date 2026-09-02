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
#include "kun/cellular/multispecies_ecology.hpp"

namespace kun {

// ============================================================================
// 1. 数字病原体毒株 (Digital Pathogen Strain / Viral Parasite)
// ============================================================================
struct PathogenStrain {
    uint32_t strain_id{0};
    uint32_t antigen_signature{0xABCD}; // 抗原特征指纹 (用于宿主免疫识别)
    double virulence{1.5};               // 毒力 (每 tick 盗取宿主能量与造成损伤)
    double transmissibility{0.35};      // 局部空间传染率 (密度越高传播越剧烈)
    double mutation_rate{0.08};          // 病原体自身的变异漂移率 (抗原漂移)

    // 病原体抗原漂移变异 (Antigenic Drift)
    PathogenStrain mutate_strain(std::mt19937& rng) const {
        PathogenStrain child = *this;
        child.strain_id = rng();
        // 随机翻转抗原特征指纹的位点，逃避宿主既有免疫抗体
        child.antigen_signature ^= (1u << (rng() % 16));
        std::normal_distribution<double> norm(0.0, 0.2);
        child.virulence = std::clamp(child.virulence + norm(rng), 0.5, 4.0);
        child.transmissibility = std::clamp(child.transmissibility + norm(rng) * 0.05, 0.10, 0.80);
        return child;
    }
};

// ============================================================================
// 2. 宿主免疫防御与抗原受体表位 (Host Immune Repertoire & CRISPR/Antibody Memory)
// ============================================================================
struct HostImmuneSystem {
    std::vector<uint32_t> antibody_memory; // 获得性免疫抗体记忆库 (已中和抗原)
    double basal_immune_maintenance_cost{0.05}; // 维持免疫系统运转的能量税
    bool is_infected{false};
    uint32_t current_infection_strain_id{0};
    double infection_load{0.0};            // 病毒载量

    // 免疫识别与中和判定：检查是否拥有匹配的抗体
    bool recognize_and_neutralize(uint32_t antigen) {
        for (auto ab : antibody_memory) {
            // 汉明距离或位点重合度匹配
            uint32_t diff = ab ^ antigen;
            int bit_diff = 0;
            for (int b = 0; b < 16; ++b) if ((diff >> b) & 1) bit_diff++;
            if (bit_diff <= 2) {
                return true; // 识别并中和病毒
            }
        }
        return false;
    }

    // 产生新抗体并建立免疫记忆 (需消耗额外能量)
    void acquire_immunity(uint32_t antigen) {
        if (antibody_memory.size() < 16) {
            antibody_memory.push_back(antigen);
        } else {
            antibody_memory[0] = antigen; // 覆盖最旧记忆
        }
    }
};

// ============================================================================
// 3. 宿主-病原体协同演化生命体 (Host Organism with Pathogen Dynamics)
// ============================================================================
class HostEcoOrganism {
public:
    HostEcoOrganism(uint64_t id, uint64_t parent_id, uint32_t gen, uint32_t comp_id, 
                    SpeciesGuild guild, ReplicableGenome genome, double initial_energy)
        : organism_id_(id), parent_id_(parent_id), generation_(gen), 
          compartment_id_(comp_id), guild_(guild), genome_(std::move(genome)) {
        
        homeostasis_.cell_id = static_cast<uint32_t>(id);
        homeostasis_.compartment_id = comp_id;
        homeostasis_.energy_reserve = initial_energy;
        homeostasis_.is_alive = true;
    }

    uint64_t get_id() const { return organism_id_; }
    uint64_t get_parent_id() const { return parent_id_; }
    uint32_t get_generation() const { return generation_; }
    uint32_t get_compartment_id() const { return compartment_id_; }
    void set_compartment_id(uint32_t cid) { compartment_id_ = cid; homeostasis_.compartment_id = cid; }
    SpeciesGuild get_guild() const { return guild_; }

    CellHomeostasisNode& get_homeostasis() { return homeostasis_; }
    const CellHomeostasisNode& get_homeostasis() const { return homeostasis_; }
    HostImmuneSystem& get_immune() { return immune_; }
    const HostImmuneSystem& get_immune() const { return immune_; }
    const ReplicableGenome& get_genome() const { return genome_; }

    // 尝试感染宿主
    bool try_infect(const PathogenStrain& strain) {
        if (!homeostasis_.is_alive) return false;

        // 检查获得性免疫抗体
        if (immune_.recognize_and_neutralize(strain.antigen_signature)) {
            return false; // 免疫免疫防御成功
        }

        // 感染成功
        immune_.is_infected = true;
        immune_.current_infection_strain_id = strain.strain_id;
        immune_.infection_load = std::min(10.0, immune_.infection_load + 2.0);
        return true;
    }

    // 宿主单步生理与病毒盗能代谢更新
    void tick_host_pathology(const PathogenStrain* current_strain, double dt = 1.0) {
        if (!homeostasis_.is_alive) return;

        // 1. 免疫系统基础维持能量税
        homeostasis_.energy_reserve -= immune_.basal_immune_maintenance_cost * dt;

        // 2. 病毒感染期病理盗能与损伤
        if (immune_.is_infected) {
            double v_damage = (current_strain ? current_strain->virulence : 1.5) * dt;
            // 病毒盗取宿主储能并造成组织损伤
            homeostasis_.energy_reserve -= v_damage * 0.8;
            homeostasis_.damage_level += v_damage * 1.2;

            // 宿主免疫应激：消耗能量尝试产生新抗体清除病毒
            if (homeostasis_.energy_reserve > 25.0) {
                homeostasis_.energy_reserve -= 5.0; // 抗体合成能耗
                if (current_strain) {
                    immune_.acquire_immunity(current_strain->antigen_signature);
                }
                immune_.is_infected = false; // 清除感染
                immune_.infection_load = 0.0;
            }
        }

        // 3. 稳态更新与凋亡
        if (homeostasis_.energy_reserve <= 0.0 || homeostasis_.damage_level >= 100.0) {
            homeostasis_.state = MetabolicState::APOPTOTIC;
            homeostasis_.is_alive = false;
        }
    }

    // 子代繁衍与基因水平转移 (Horizontal Gene Transfer HGT)
    std::unique_ptr<HostEcoOrganism> spawn_with_immunity_and_hgt(std::mt19937& rng, uint64_t next_id, double mutation_rate = 0.05) {
        double cost = 40.0;
        if (homeostasis_.energy_reserve <= cost) return nullptr;

        homeostasis_.energy_reserve -= cost;
        double starter = homeostasis_.energy_reserve * 0.30;
        homeostasis_.energy_reserve -= starter;

        auto child_genome = genome_.replicate_with_mutation(rng, mutation_rate);

        // 【病毒介导的基因水平转移 HGT】: 感染幸存者有概率将病毒片段重组为自身新功能基因
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (!immune_.antibody_memory.empty() && dist(rng) < 0.15) {
            GeneLocus hgt_locus{
                static_cast<uint32_t>(child_genome.loci.size()),
                static_cast<uint8_t>(rng() % 8),
                0.15, // 演化出更节能的代谢位点
                0.05,
                1.5,  // 强连接
                compartment_id_
            };
            child_genome.loci.push_back(hgt_locus);
        }

        auto child = std::make_unique<HostEcoOrganism>(
            next_id,
            organism_id_,
            generation_ + 1,
            compartment_id_,
            guild_,
            std::move(child_genome),
            starter
        );

        // 垂直遗传母本部分抗体记忆 (Maternal Antibody Transfer)
        if (!immune_.antibody_memory.empty()) {
            child->immune_.acquire_immunity(immune_.antibody_memory.back());
        }

        return child;
    }

private:
    uint64_t organism_id_{0};
    uint64_t parent_id_{0};
    uint32_t generation_{0};
    uint32_t compartment_id_{0};
    SpeciesGuild guild_{SpeciesGuild::FAST_FORAGER};
    ReplicableGenome genome_;
    CellHomeostasisNode homeostasis_;
    HostImmuneSystem immune_;
};

// ============================================================================
// 4. 病原体-宿主协同演化生态世界 (Pathogen-Host Co-Evolution World)
// ============================================================================
class PathogenCoEvolutionWorld {
public:
    struct PathogenTelemetry {
        uint64_t tick{0};
        size_t total_hosts{0};
        size_t infected_hosts{0};
        size_t immune_resistant_hosts{0};
        size_t pathogen_strains_count{0};
        size_t disease_culls_count{0};
        double average_host_energy{0.0};
    };

    PathogenCoEvolutionWorld(size_t initial_hosts = 24, size_t num_compartments = 3, uint32_t seed = 42)
        : rng_(seed) {
        init_world(initial_hosts, num_compartments);
    }

    void init_world(size_t initial_hosts, size_t num_compartments) {
        compartments_.clear();
        hosts_.clear();
        pathogens_.clear();
        history_.clear();
        next_id_ = 1;

        for (uint32_t i = 0; i < num_compartments; ++i) {
            SpatialCompartment comp;
            comp.compartment_id = i;
            comp.nutrient_concentration = 2200.0;
            comp.waste_toxicity = 0.0;
            comp.membrane_permeability = 0.12;
            comp.carrying_capacity = 60;
            compartments_.push_back(comp);
        }

        // 初始化宿主种群
        for (size_t i = 0; i < initial_hosts; ++i) {
            ReplicableGenome genome;
            genome.genome_id = next_id_;
            genome.lineage_hash = "host_root";
            for (uint32_t locus = 0; locus < 6; ++locus) {
                genome.loci.push_back(GeneLocus{locus, static_cast<uint8_t>(locus % 4), 0.20, 0.08, 1.0, static_cast<uint32_t>(i % num_compartments)});
            }
            auto host = std::make_unique<HostEcoOrganism>(
                next_id_++,
                0,
                0,
                static_cast<uint32_t>(i % num_compartments),
                static_cast<SpeciesGuild>(i % 3),
                std::move(genome),
                70.0
            );
            hosts_.push_back(std::move(host));
        }

        // 释放初始先祖病原体毒株 (Progenitor Virus Strain)
        PathogenStrain virus0{1, 0xA1B2, 2.0, 0.45, 0.08};
        pathogens_.push_back(virus0);
    }

    // 释放新突发疫情冲击 (Epidemic Outbreak Trigger)
    void release_pathogen_outbreak(uint32_t antigen, double virulence = 2.5) {
        PathogenStrain outbreak{static_cast<uint32_t>(pathogens_.size() + 1), antigen, virulence, 0.50, 0.10};
        pathogens_.push_back(outbreak);
    }

    // 执行单步宿主-病原体对抗演化循环
    PathogenTelemetry tick(double nutrient_influx = 60.0, double mutation_rate = 0.05) {
        current_tick_++;
        size_t culls_in_tick = 0;

        // 1. 营养供给
        double influx_per_comp = nutrient_influx / std::max<size_t>(1, compartments_.size());
        for (auto& comp : compartments_) {
            comp.nutrient_concentration += influx_per_comp;
            comp.waste_toxicity = std::max(0.0, comp.waste_toxicity * 0.96);
        }

        // 2. 病原体接触传染传播 (Density-Dependent Epidemic Transmission)
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        for (auto& pathogen : pathogens_) {
            for (auto& host : hosts_) {
                if (!host->get_homeostasis().is_alive) continue;
                if (!host->get_immune().is_infected && dist(rng_) < pathogen.transmissibility) {
                    host->try_infect(pathogen);
                }
            }
        }

        // 3. 宿主生理、代谢、病理盗能与淘汰结算
        for (auto& host : hosts_) {
            if (!host->get_homeostasis().is_alive) continue;

            auto& h = host->get_homeostasis();
            auto& comp = compartments_[host->get_compartment_id()];

            // 养分摄取
            double absorb = std::min(150.0 - h.energy_reserve, std::min(2.0, comp.nutrient_concentration * 0.02));
            h.energy_reserve += absorb;
            comp.nutrient_concentration -= absorb;

            // 基础代谢
            h.energy_reserve -= h.basal_metabolic_rate;

            // 查找当前感染毒株
            const PathogenStrain* current_strain = nullptr;
            for (const auto& p : pathogens_) {
                if (p.strain_id == host->get_immune().current_infection_strain_id) {
                    current_strain = &p;
                    break;
                }
            }

            // 执行病理结算
            host->tick_host_pathology(current_strain, 1.0);

            if (!h.is_alive) {
                culls_in_tick++;
                comp.nutrient_concentration += 4.0; // 解离回归
            }
        }

        // 4. 病原体抗原漂移自主演化 (Antigenic Drift)
        if (dist(rng_) < 0.10 && pathogens_.size() < 8) {
            size_t idx = rng_() % pathogens_.size();
            pathogens_.push_back(pathogens_[idx].mutate_strain(rng_));
        }

        // 5. 宿主自发内生繁衍与子代产生 (带免疫继承与 HGT)
        std::vector<std::unique_ptr<HostEcoOrganism>> new_children;
        for (auto& host : hosts_) {
            if (!host->get_homeostasis().is_alive || host->get_homeostasis().state != MetabolicState::ACTIVE) continue;

            if (host->get_homeostasis().energy_reserve >= 85.0 && hosts_.size() < 70) {
                auto child = host->spawn_with_immunity_and_hgt(rng_, next_id_++, mutation_rate);
                if (child) {
                    new_children.push_back(std::move(child));
                }
            }
        }

        for (auto& c : new_children) hosts_.push_back(std::move(c));

        // 6. 清理凋亡个体
        hosts_.erase(
            std::remove_if(hosts_.begin(), hosts_.end(), [](const auto& h) {
                return !h->get_homeostasis().is_alive;
            }),
            hosts_.end()
        );

        // 7. 遥测统计
        size_t infected_count = 0;
        size_t resistant_count = 0;
        double total_e = 0.0;
        for (const auto& h : hosts_) {
            if (h->get_immune().is_infected) infected_count++;
            if (!h->get_immune().antibody_memory.empty()) resistant_count++;
            total_e += h->get_homeostasis().energy_reserve;
        }

        PathogenTelemetry frame{
            current_tick_,
            hosts_.size(),
            infected_count,
            resistant_count,
            pathogens_.size(),
            culls_in_tick,
            hosts_.empty() ? 0.0 : (total_e / hosts_.size())
        };
        history_.push_back(frame);
        return frame;
    }

    const std::vector<std::unique_ptr<HostEcoOrganism>>& get_hosts() const { return hosts_; }
    const std::vector<PathogenStrain>& get_pathogens() const { return pathogens_; }
    const std::vector<PathogenTelemetry>& get_history() const { return history_; }

private:
    uint64_t current_tick_{0};
    uint64_t next_id_{1};
    std::mt19937 rng_;
    std::vector<SpatialCompartment> compartments_;
    std::vector<PathogenStrain> pathogens_;
    std::vector<std::unique_ptr<HostEcoOrganism>> hosts_;
    std::vector<PathogenTelemetry> history_;
};

} // namespace kun
