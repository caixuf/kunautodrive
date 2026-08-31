#pragma once

#include "kun/cellular/cellular_genome.hpp"
#include "kun/cellular/quantum_radiation_field.hpp"
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <cmath>
#include <random>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace kun {

/**
 * @brief 物种生态位门类 (Species Niche Role)
 */
enum class SpeciesNiche : uint8_t {
    PRODUCER = 0,    ///< 🌿 生产者 (做市商/流动性挂单提供者)
    HERBIVORE = 1,   ///< 🐇 初级消费者 (趋势/动量跟随者)
    PREDATOR = 2,    ///< 🦅 顶级掠食者 (统计套利/高频价差夹逼者)
    DECOMPOSER = 3   ///< 🍄 分解者 (清算风控/破损资本回收者)
};

inline const char* species_niche_to_string(SpeciesNiche niche) {
    switch (niche) {
        case SpeciesNiche::PRODUCER:   return "Producer(做市商)";
        case SpeciesNiche::HERBIVORE:  return "Herbivore(趋势派)";
        case SpeciesNiche::PREDATOR:   return "Predator(套利派)";
        case SpeciesNiche::DECOMPOSER: return "Decomposer(清算派)";
        default: return "Unknown";
    }
}

/**
 * @brief 宏观生境气候季相 (Biome Climate Regime)
 */
enum class BiomeClimate : uint8_t {
    SPRING_TREND = 0,    ///< 🌸 春暖单边季: 波动温和、单边趋势清晰 (趋势派繁荣)
    SUMMER_STORM = 1,    ///< ⚡ 盛夏风暴季: 极端高波动、剧烈洗盘 (高波与风控活跃)
    AUTUMN_DROUGHT = 2,  ///< 🍂 金秋干旱季: 流动性收缩、均值震荡 (做市商繁茂)
    WINTER_FREEZE = 3    ///< ❄️ 严冬极寒季: 流动性冰冻、价差剧增 (分解者主导/冬眠保护)
};

inline const char* biome_climate_to_string(BiomeClimate climate) {
    switch (climate) {
        case BiomeClimate::SPRING_TREND:   return "SpringTrend(单边牛熊季)";
        case BiomeClimate::SUMMER_STORM:   return "SummerStorm(高波风暴季)";
        case BiomeClimate::AUTUMN_DROUGHT: return "AutumnDrought(震荡干旱季)";
        case BiomeClimate::WINTER_FREEZE:  return "WinterFreeze(极寒冰冻季)";
        default: return "Unknown";
    }
}

/**
 * @brief 生态个体元数据 (EcoAgent)
 * 包装单个 CellularOrganism，赋予其生态能量、存活代谢与捕食关系
 */
struct EcoAgent {
    uint64_t id{0};
    SpeciesNiche niche{SpeciesNiche::HERBIVORE};
    std::string species_name;
    CellularOrganism organism;
    
    double energy{100.0};            ///< 当前生命能量 (对应资本权益/流动性余额)
    double metabolic_rate{0.1};      ///< 基础代谢率 (每秒消耗能量)
    double predation_yield{0.0};     ///< 捕食收益累积
    bool is_alive{true};             ///< 是否存活
    uint64_t age_ticks{0};           ///< 存活时间步
    
    // 3D 空间坐标与视觉光斑
    float x{0.0f}, y{0.0f}, z{0.0f};
    float glow_intensity{1.0f};
};

/**
 * @brief 捕食与共生能量流动事件 (TrophicEnergyTransfer)
 */
struct TrophicEnergyTransfer {
    uint64_t predator_id{0};
    uint64_t prey_id{0};
    double energy_amount{0.0};
    std::string transfer_type; // "PREDATION", "LIQUIDITY_GRAZING", "DECOMPOSITION"
};

/**
 * @brief 生态圈生境区 (Biome Zone)
 * 代表一个特定资产大类或智驾场景的生态栖息地
 */
class BiomeZone {
public:
    BiomeZone(std::string name, BiomeClimate climate = BiomeClimate::SPRING_TREND)
        : name_(std::move(name)), climate_(climate) {}

    const std::string& name() const { return name_; }
    BiomeClimate climate() const { return climate_; }
    void set_climate(BiomeClimate c) { climate_ = c; }

    double nutrient_pool() const { return nutrient_pool_; }
    void add_nutrient(double amount) { nutrient_pool_ = std::max(0.0, nutrient_pool_ + amount); }
    bool consume_nutrient(double amount) {
        if (nutrient_pool_ >= amount) {
            nutrient_pool_ -= amount;
            return true;
        }
        return false;
    }

private:
    std::string name_;
    BiomeClimate climate_{BiomeClimate::SPRING_TREND};
    double nutrient_pool_{1000.0}; // 环境公有流动性养分池
};

/**
 * @brief 太初 · 宏观自适应生态圈引擎 (Macro EcoBiosphere Engine)
 */
class EcoBiosphere {
public:
    EcoBiosphere(size_t initial_population_per_niche = 10, uint32_t seed = 42)
        : rng_(seed) {
        // 初始化四大生境
        biomes_.push_back(std::make_shared<BiomeZone>("Biome-BlackMetal", BiomeClimate::SPRING_TREND));
        biomes_.push_back(std::make_shared<BiomeZone>("Biome-PreciousMetals", BiomeClimate::SUMMER_STORM));
        biomes_.push_back(std::make_shared<BiomeZone>("Biome-EnergyChemical", BiomeClimate::AUTUMN_DROUGHT));
        biomes_.push_back(std::make_shared<BiomeZone>("Biome-ADASPerception", BiomeClimate::WINTER_FREEZE));

        // 播种四大物种群落
        seed_population(initial_population_per_niche);
    }

    /**
     * @brief 驱动生态圈一轮完整的生命周期迭代
     * 包含: 代谢消耗 -> 市场盈亏与环境反馈 -> 捕食与流动性共生 -> 气候季相变迁 -> 洛特卡-沃尔泰拉动态平衡 -> 凋亡分解与重注
     */
    void step_ecosystem(double dt = 1.0, double market_volatility = 0.15, double market_trend = 0.0, double market_pnl = 0.0) {
        std::uniform_real_distribution<float> dist_pos(-50.0f, 50.0f);
        recent_transfers_.clear();

        // 1. 各个体前向计算与生命代谢 (连通真实市场反馈)
        for (auto& agent : agents_) {
            if (!agent.is_alive) continue;

            agent.age_ticks++;
            agent.energy -= (agent.metabolic_rate * dt);

            // 市场损益反馈机制: 个体若有正向收益则补充生命能量，亏损则加剧代谢
            if (market_pnl > 0.0 && agent.niche == SpeciesNiche::HERBIVORE && market_trend > 0.0) {
                agent.energy += std::min(10.0, market_pnl * 0.05);
            } else if (market_pnl < 0.0 && agent.niche == SpeciesNiche::PREDATOR) {
                agent.energy += std::min(10.0, -market_pnl * 0.05); // 掠食者在极端风险中获利
            }

            // 物理力场模拟与位置更新
            agent.organism.step_force_field_physics(static_cast<float>(dt * 0.016));

            // 能量耗尽判定
            if (agent.energy <= 0.0) {
                agent.is_alive = false;
                // 能量分解归还环境养分池
                if (!biomes_.empty()) {
                    biomes_[0]->add_nutrient(10.0);
                }
            }
        }

        // 2. 推进量子波动干涉场与宇宙射线辐射演化
        radiation_field_.step(static_cast<float>(dt));
        for (auto& agent : agents_) {
            if (!agent.is_alive) continue;
            radiation_field_.irradiate_organism(
                agent.organism, agent.x, agent.y, agent.z, static_cast<uint32_t>(agent.age_ticks)
            );
        }

        // 3. 模拟物种间捕食与共生交互 (Lotka-Volterra 启发式)
        simulate_trophic_interactions();

        // 4. 气候季相与生境状态变迁 (动态响应外部市场波动与时间流逝)
        step_count_++;
        if (market_volatility > 0.35) {
            for (auto& b : biomes_) b->set_climate(BiomeClimate::SUMMER_STORM);
        } else if (std::abs(market_trend) > 0.05) {
            for (auto& b : biomes_) b->set_climate(BiomeClimate::SPRING_TREND);
        } else if (market_volatility < 0.08) {
            for (auto& b : biomes_) b->set_climate(BiomeClimate::AUTUMN_DROUGHT);
        } else if (market_pnl < -200.0) {
            for (auto& b : biomes_) b->set_climate(BiomeClimate::WINTER_FREEZE);
        } else if (step_count_ % 200 == 0) {
            rotate_biome_climates();
        }

        // 5. 物种多样性与繁殖 (保持生态位不灭绝, 真实继承父代基因)
        maintain_niche_equilibrium();
    }

    /**
     * @brief 计算当前生态圈的香农生物多样性指数 (Shannon Diversity Index)
     * H = - \sum (p_i * ln(p_i))
     */
    double calculate_shannon_diversity() const {
        std::unordered_map<SpeciesNiche, size_t> counts;
        size_t total_alive = 0;
        for (const auto& a : agents_) {
            if (a.is_alive) {
                counts[a.niche]++;
                total_alive++;
            }
        }
        if (total_alive == 0) return 0.0;

        double h = 0.0;
        for (const auto& [niche, count] : counts) {
            if (count > 0) {
                double p = static_cast<double>(count) / total_alive;
                h -= p * std::log(p);
            }
        }
        return h;
    }

    /**
     * @brief 获取各物种存活个体统计
     */
    std::unordered_map<SpeciesNiche, size_t> get_niche_population() const {
        std::unordered_map<SpeciesNiche, size_t> counts;
        for (const auto& a : agents_) {
            if (a.is_alive) {
                counts[a.niche]++;
            }
        }
        return counts;
    }

    const std::vector<EcoAgent>& agents() const { return agents_; }
    const std::vector<std::shared_ptr<BiomeZone>>& biomes() const { return biomes_; }
    const std::vector<TrophicEnergyTransfer>& recent_transfers() const { return recent_transfers_; }
    const QuantumRadiationField& radiation_field() const { return radiation_field_; }
    uint64_t step_count() const { return step_count_; }

    /**
     * @brief 导出生态圈全息状态 JSON (供 3D 前端全息生态球渲染)
     */
    std::string to_json() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "{\n";
        ss << "  \"step\": " << step_count_ << ",\n";
        ss << "  \"shannon_diversity\": " << calculate_shannon_diversity() << ",\n";

        // 生境信息
        ss << "  \"biomes\": [\n";
        for (size_t i = 0; i < biomes_.size(); ++i) {
            const auto& b = biomes_[i];
            ss << "    {\"name\": \"" << b->name() << "\", \"climate\": \"" 
               << biome_climate_to_string(b->climate()) << "\", \"nutrient\": " << b->nutrient_pool() << "}"
               << (i + 1 < biomes_.size() ? "," : "") << "\n";
        }
        ss << "  ],\n";

        // 辐射场状态 (单一唯一键)
        ss << "  \"radiation\": " << radiation_field_.to_json() << ",\n";

        // 物种统计
        auto counts = get_niche_population();
        ss << "  \"niche_counts\": {\n";
        ss << "    \"producers\": " << counts[SpeciesNiche::PRODUCER] << ",\n";
        ss << "    \"herbivores\": " << counts[SpeciesNiche::HERBIVORE] << ",\n";
        ss << "    \"predators\": " << counts[SpeciesNiche::PREDATOR] << ",\n";
        ss << "    \"decomposers\": " << counts[SpeciesNiche::DECOMPOSER] << "\n";
        ss << "  },\n";

        // 存活有机体光斑与能量
        ss << "  \"agents\": [\n";
        size_t alive_idx = 0;
        for (const auto& a : agents_) {
            if (!a.is_alive) continue;
            if (alive_idx > 0) ss << ",\n";
            ss << "    {\"id\": " << a.id 
               << ", \"niche\": \"" << species_niche_to_string(a.niche) << "\""
               << ", \"energy\": " << a.energy 
               << ", \"age\": " << a.age_ticks
               << ", \"x\": " << a.x << ", \"y\": " << a.y << ", \"z\": " << a.z 
               << ", \"cells_count\": " << a.organism.cells.size() << "}";
            alive_idx++;
        }
        ss << "\n  ]\n";
        ss << "}\n";
        return ss.str();
    }

private:
    void seed_population(size_t n_per_niche) {
        agents_.clear();
        uint64_t id_counter = 1;
        std::uniform_real_distribution<float> dist_pos(-40.0f, 40.0f);

        for (int niche_int = 0; niche_int < 4; ++niche_int) {
            auto niche = static_cast<SpeciesNiche>(niche_int);
            for (size_t i = 0; i < n_per_niche; ++i) {
                EcoAgent agent;
                agent.id = id_counter++;
                agent.niche = niche;
                agent.species_name = std::string(species_niche_to_string(niche)) + "-" + std::to_string(i);
                agent.energy = 80.0 + (rng_() % 40);
                agent.metabolic_rate = 0.05 + (rng_() % 10) * 0.01;
                agent.is_alive = true;
                agent.x = dist_pos(rng_);
                agent.y = dist_pos(rng_);
                agent.z = dist_pos(rng_);

                // 构建初始 3~5 细胞的基础有机体
                agent.organism.organism_id = agent.id;
                agent.organism.lineage_name = agent.species_name;
                
                // 生物原语分化
                Cell c0{0, CellType::SENSE_RAW_INPUT_0, 0.1, 0.0};
                c0.x = -2.0f; c0.y = 0.0f; c0.z = 0.0f;

                Cell c1{1, (niche == SpeciesNiche::PREDATOR) ? CellType::OP_DIFF : CellType::OP_EMA, 0.2, 0.0};
                c1.x = 0.0f; c1.y = 0.0f; c1.z = 0.0f;

                Cell c2{2, CellType::ACT_PRIMARY_POSITIVE, 1.0, 0.0};
                c2.x = 2.0f; c2.y = -1.0f; c2.z = 0.0f;

                Cell c3{3, CellType::ACT_PRIMARY_NEGATIVE, 1.0, 0.0};
                c3.x = 2.0f; c3.y = 1.0f; c3.z = 0.0f;

                agent.organism.cells = {c0, c1, c2, c3};
                agent.organism.synapses = {
                    Synapse{0, 1, 0, 0.6, true, 60.0f, -1.0f},
                    Synapse{1, 2, 0, 0.8, true, 60.0f, -1.0f},
                    Synapse{1, 3, 0, -0.8, true, 60.0f, -1.0f}
                };
                agent.organism.compile();

                agents_.push_back(std::move(agent));
            }
        }
    }

    void simulate_trophic_interactions() {
        // 掠食者猎杀初级消费者 (Predator hunts Herbivore)
        std::vector<EcoAgent*> predators;
        std::vector<EcoAgent*> herbivores;
        std::vector<EcoAgent*> producers;

        for (auto& a : agents_) {
            if (!a.is_alive) continue;
            if (a.niche == SpeciesNiche::PREDATOR) predators.push_back(&a);
            else if (a.niche == SpeciesNiche::HERBIVORE) herbivores.push_back(&a);
            else if (a.niche == SpeciesNiche::PRODUCER) producers.push_back(&a);
        }

        // 1. 初级消费者吸纳生产者的流动性 (Herbivore grazes Producer)
        for (auto* herb : herbivores) {
            if (producers.empty()) break;
            auto* prod = producers[rng_() % producers.size()];
            double yield = std::min(5.0, prod->energy * 0.05);
            if (yield > 0.1) {
                prod->energy -= yield;
                herb->energy += yield;
                herb->predation_yield += yield;
                recent_transfers_.push_back({herb->id, prod->id, yield, "LIQUIDITY_GRAZING"});
            }
        }

        // 2. 顶级掠食者猎杀初级消费者的单腿暴露 (Predator hunts Herbivore)
        for (auto* pred : predators) {
            if (herbivores.empty()) break;
            auto* prey = herbivores[rng_() % herbivores.size()];
            double loot = std::min(15.0, prey->energy * 0.2);
            if (loot > 0.5) {
                prey->energy -= loot;
                pred->energy += loot;
                pred->predation_yield += loot;
                recent_transfers_.push_back({pred->id, prey->id, loot, "PREDATION"});
            }
        }
    }

    void rotate_biome_climates() {
        for (auto& b : biomes_) {
            uint8_t next_c = (static_cast<uint8_t>(b->climate()) + 1) % 4;
            b->set_climate(static_cast<BiomeClimate>(next_c));
        }
    }

    void maintain_niche_equilibrium() {
        auto counts = get_niche_population();
        std::uniform_real_distribution<float> dist_pos(-40.0f, 40.0f);
        std::normal_distribution<double> dist_mutate(0.0, 0.12);

        for (int niche_int = 0; niche_int < 4; ++niche_int) {
            auto niche = static_cast<SpeciesNiche>(niche_int);
            // 若某生态位存活少于 3 个，利用环境养分池繁育新生代 (优先继承亲代优良基因，保留进化积累)
            if (counts[niche] < 3) {
                // 1. 寻找同生态位或全局最强健亲代
                const EcoAgent* best_parent = nullptr;
                double max_energy = -1.0;
                for (const auto& a : agents_) {
                    if (a.is_alive && a.niche == niche && a.energy > max_energy) {
                        max_energy = a.energy;
                        best_parent = &a;
                    }
                }
                if (!best_parent) {
                    for (const auto& a : agents_) {
                        if (a.is_alive && a.energy > max_energy) {
                            max_energy = a.energy;
                            best_parent = &a;
                        }
                    }
                }

                EcoAgent newborn;
                newborn.id = agents_.size() + 1000 + (rng_() % 1000);
                newborn.niche = niche;
                newborn.energy = 90.0;
                newborn.metabolic_rate = 0.06;
                newborn.is_alive = true;
                newborn.x = dist_pos(rng_);
                newborn.y = dist_pos(rng_);
                newborn.z = dist_pos(rng_);

                if (best_parent && !best_parent->organism.cells.empty()) {
                    // 继承亲代拓扑与细胞基因 (True Lineage Accumulation)
                    newborn.organism = best_parent->organism;
                    newborn.organism.organism_id = newborn.id;
                    newborn.organism.generation = best_parent->organism.generation + 1;
                    newborn.species_name = best_parent->species_name + "-G" + std::to_string(newborn.organism.generation);

                    // 施加点变异与微重组 (突触权重扰动与参数漂移)
                    for (auto& syn : newborn.organism.synapses) {
                        if ((rng_() % 100) < 60) {
                            syn.weight += dist_mutate(rng_);
                            syn.weight = std::clamp(syn.weight, -3.0, 3.0);
                        }
                    }
                    for (auto& c : newborn.organism.cells) {
                        if ((rng_() % 100) < 40) {
                            c.param1 += dist_mutate(rng_) * 0.05;
                            c.param1 = std::clamp(c.param1, 0.01, 0.99);
                        }
                    }
                    newborn.organism.compile();
                } else {
                    // 无亲代时创建生态位基础种子
                    newborn.species_name = std::string(species_niche_to_string(niche)) + "-Seed";
                    Cell c0{0, CellType::SENSE_RAW_INPUT_0, 0.1, 0.0};
                    c0.x = -2.0f; c0.y = 0.0f; c0.z = 0.0f;

                    Cell c1{1, (niche == SpeciesNiche::PREDATOR) ? CellType::OP_DIFF : CellType::OP_EMA, 0.15, 0.0};
                    c1.x = 0.0f; c1.y = 0.0f; c1.z = 0.0f;

                    Cell c2{2, CellType::ACT_PRIMARY_POSITIVE, 1.0, 0.0};
                    c2.x = 2.0f; c2.y = -1.0f; c2.z = 0.0f;

                    Cell c3{3, CellType::ACT_PRIMARY_NEGATIVE, 1.0, 0.0};
                    c3.x = 2.0f; c3.y = 1.0f; c3.z = 0.0f;

                    newborn.organism.cells = {c0, c1, c2, c3};
                    newborn.organism.synapses = {
                        Synapse{0, 1, 0, 0.5, true, 60.0f, -1.0f},
                        Synapse{1, 2, 0, 0.7, true, 60.0f, -1.0f},
                        Synapse{1, 3, 0, -0.7, true, 60.0f, -1.0f}
                    };
                    newborn.organism.compile();
                }

                agents_.push_back(std::move(newborn));
            }
        }
    }

    std::mt19937 rng_;
    QuantumRadiationField radiation_field_{54321};
    std::vector<EcoAgent> agents_;
    std::vector<std::shared_ptr<BiomeZone>> biomes_;
    std::vector<TrophicEnergyTransfer> recent_transfers_;
    uint64_t step_count_{0};
};

} // namespace kun
