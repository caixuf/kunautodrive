#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <cmath>
#include <string>
#include <sstream>
#include <algorithm>
#include <random>
#include <chrono>

#include "kun/cellular/cellular_genome.hpp"

namespace kun {

// ============================================================================
// 1. 时空曲率演化档位 (Hyper-Warp Speed Modes)
// ============================================================================
enum class WarpSpeed : uint32_t {
    REALTIME_1X   = 1,      // 人眼观察模式 (~50 Hz 物理帧率, ~0.33 代/秒)
    WARP_100X     = 100,    // 快速演化模式 (~33 代/秒)
    WARP_1000X    = 1000,   // 涡轮加速模式 (~330 代/秒)
    WARP_UNLIMITED = 0      // 硅基极限超光速模式 (无锁满频, > 10,000 代/秒)
};

inline const char* to_string(WarpSpeed speed) {
    switch (speed) {
        case WarpSpeed::REALTIME_1X:   return "1x_Realtime";
        case WarpSpeed::WARP_100X:     return "100x_Fast";
        case WarpSpeed::WARP_1000X:    return "1000x_Turbo";
        case WarpSpeed::WARP_UNLIMITED: return "UNLIMITED_HyperWarp";
        default: return "Unknown";
    }
}

// ============================================================================
// 2. 红皇后对抗压力配置文件 (Red-Queen Adversarial Stress Profile)
// ============================================================================
struct AdversarialStressProfile {
    enum class Level { OFF = 0, LOW = 1, MEDIUM = 2, EXTREME = 3 };
    Level level{Level::OFF};
    double flash_crash_prob{0.0};    // 流动性闪崩与急跌概率
    double volatility_multiplier{1.0}; // 波动率放大倍数
    double cut_in_ttc_min{2.0};      // 自动驾驶极限切入最小 TTC (s)

    static AdversarialStressProfile make_profile(Level lvl) {
        AdversarialStressProfile p;
        p.level = lvl;
        if (lvl == Level::LOW) {
            p.flash_crash_prob = 0.05;
            p.volatility_multiplier = 1.8;
            p.cut_in_ttc_min = 1.5;
        } else if (lvl == Level::MEDIUM) {
            p.flash_crash_prob = 0.15;
            p.volatility_multiplier = 3.5;
            p.cut_in_ttc_min = 1.0;
        } else if (lvl == Level::EXTREME) {
            p.flash_crash_prob = 0.35;
            p.volatility_multiplier = 8.0;
            p.cut_in_ttc_min = 0.6;
        }
        return p;
    }
};

// ============================================================================
// 3. 独立演化岛生态单元 (Island Deme - 64-Byte Cache Aligned)
// ============================================================================
struct alignas(64) IslandDeme {
    uint32_t island_id{0};
    uint32_t core_id{0};
    MorphogeneticEvolutionEngine engine;

    std::atomic<uint64_t> generations_count{0};
    std::atomic<uint64_t> inferences_count{0};
    double local_best_fitness{-9999.0};
    uint32_t migration_in_count{0};
    uint32_t migration_out_count{0};

    // 独立随机数生成器以保证岛屿演化路径正交隔离
    std::mt19937_64 island_rng;
    std::normal_distribution<double> noise_dist;

    char cache_padding[64]; // 严防多核并发伪共享 (Anti-False Sharing)

    IslandDeme(uint32_t id, uint32_t core, uint32_t seed, SeedInitMode mode)
        : island_id(id), core_id(core), engine(20, seed, mode),
          island_rng(seed + id * 1013), noise_dist(0.0, 1.0) {
        std::memset(cache_padding, 0, sizeof(cache_padding));
    }
};

// ============================================================================
// 4. 多岛超加速形态发生演化网格 (IslandEvolutionGrid)
// ============================================================================
class IslandEvolutionGrid {
public:
    explicit IslandEvolutionGrid(size_t num_islands = 8, SeedInitMode default_mode = SeedInitMode::HANDCRAFTED_PROGENITOR)
        : warp_speed_(WarpSpeed::REALTIME_1X),
          stress_profile_(AdversarialStressProfile::make_profile(AdversarialStressProfile::Level::OFF)),
          total_migrations_(0) {
        
        islands_.reserve(num_islands);
        for (size_t i = 0; i < num_islands; ++i) {
            uint32_t seed = static_cast<uint32_t>(1337 + i * 997);
            islands_.emplace_back(std::make_unique<IslandDeme>(
                static_cast<uint32_t>(i),
                static_cast<uint32_t>(i % 8),
                seed,
                default_mode
            ));
        }
    }

    // 档位调节
    void set_warp_speed(WarpSpeed speed) {
        warp_speed_.store(speed, std::memory_order_release);
    }

    WarpSpeed get_warp_speed() const {
        return warp_speed_.load(std::memory_order_acquire);
    }

    // 对抗环境等级设置
    void set_stress_level(AdversarialStressProfile::Level level) {
        std::lock_guard<std::mutex> lk(grid_mutex_);
        stress_profile_ = AdversarialStressProfile::make_profile(level);
    }

    // 单岛迭代一步 (支持物理松弛、前向计算与自适应适应度累加)
    void step_island(size_t island_idx, const double inputs[4], double simulated_market_delta = 0.0) {
        if (island_idx >= islands_.size()) return;
        auto& deme = *islands_[island_idx];

        // 引入红皇后对抗扰动
        double actual_delta = simulated_market_delta;
        if (stress_profile_.level != AdversarialStressProfile::Level::OFF) {
            std::uniform_real_distribution<double> u_dist(0.0, 1.0);
            if (u_dist(deme.island_rng) < stress_profile_.flash_crash_prob) {
                // 瞬间闪崩剧烈冲击
                actual_delta -= (15.0 * stress_profile_.volatility_multiplier);
            } else {
                actual_delta *= stress_profile_.volatility_multiplier;
            }
        }

        // 物理松弛与前向传导
        for (auto& org : deme.engine.population()) {
            org.step_force_field_physics(0.02f);
            auto actions = org.forward(inputs);
            deme.inferences_count.fetch_add(1, std::memory_order_relaxed);

            // 适应度评估 (多头收益 - 空头损失 - 免疫锁惩罚/奖励)
            double pnl = (actions.positive_action - actions.negative_action) * actual_delta * 100.0;
            if (actions.immune_lock && actual_delta < -5.0) {
                pnl += 50.0; // 成功避险奖励
            }
            if (!std::isfinite(pnl)) pnl = 0.0;
            if (!std::isfinite(org.fitness_score)) org.fitness_score = 0.0;
            org.fitness_score = std::clamp(org.fitness_score * 0.95 + pnl, -2000.0, 10000.0);
        }

        // 推进代际演化
        deme.engine.evolve_generation();
        deme.generations_count.fetch_add(1, std::memory_order_relaxed);

        const auto& champ = deme.engine.get_champion();
        if (std::isfinite(champ.fitness_score)) {
            deme.local_best_fitness = champ.fitness_score;
        }
    }

    // 环形拓扑跨岛基因大迁徙 (Torus / Ring Elite Migration)
    void migrate_elites() {
        std::lock_guard<std::mutex> lk(grid_mutex_);
        if (islands_.size() <= 1) return;

        size_t n = islands_.size();
        for (size_t i = 0; i < n; ++i) {
            size_t next_i = (i + 1) % n;
            auto& src_deme = *islands_[i];
            auto& dst_deme = *islands_[next_i];

            // 提取源岛冠军
            CellularOrganism elite = src_deme.engine.get_champion();
            elite.lineage_name = "Migrant-I" + std::to_string(i) + "->I" + std::to_string(next_i);

            // 迁徙注入至目标岛 (替换其最弱个体)
            auto& dst_pop = dst_deme.engine.population();
            if (!dst_pop.empty()) {
                dst_pop.back() = elite;
                dst_pop.back().compile();
                src_deme.migration_out_count++;
                dst_deme.migration_in_count++;
                total_migrations_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // 获取全岛网格全局最强生命体 (Global Champion)
    CellularOrganism get_global_champion() const {
        std::lock_guard<std::mutex> lk(grid_mutex_);
        CellularOrganism global_champ;
        double best_fitness = -1e9;
        bool found = false;

        for (const auto& deme : islands_) {
            const auto& c = deme->engine.get_champion();
            if (!found || (std::isfinite(c.fitness_score) && c.fitness_score > best_fitness)) {
                best_fitness = c.fitness_score;
                global_champ = c;
                found = true;
            }
        }
        return global_champ;
    }

    // 导出 8 岛并行态势 JSON
    std::string to_json() const {
        std::lock_guard<std::mutex> lk(grid_mutex_);
        std::ostringstream ss;

        uint64_t total_gens = 0;
        uint64_t total_infs = 0;
        for (const auto& d : islands_) {
            total_gens += d->generations_count.load(std::memory_order_relaxed);
            total_infs += d->inferences_count.load(std::memory_order_relaxed);
        }

        ss << "{\n";
        ss << "  \"warp_mode\": \"" << to_string(warp_speed_.load(std::memory_order_relaxed)) << "\",\n";
        ss << "  \"stress_level\": " << static_cast<int>(stress_profile_.level) << ",\n";
        ss << "  \"total_islands\": " << islands_.size() << ",\n";
        ss << "  \"total_generations\": " << total_gens << ",\n";
        ss << "  \"total_inferences\": " << total_infs << ",\n";
        ss << "  \"total_migrations\": " << total_migrations_.load(std::memory_order_relaxed) << ",\n";
        ss << "  \"islands\": [\n";

        for (size_t i = 0; i < islands_.size(); ++i) {
            const auto& d = *islands_[i];
            const auto& champ = d.engine.get_champion();
            ss << "    {\n"
               << "      \"island_id\": " << d.island_id << ",\n"
               << "      \"core_id\": " << d.core_id << ",\n"
               << "      \"generations\": " << d.generations_count.load(std::memory_order_relaxed) << ",\n"
               << "      \"inferences\": " << d.inferences_count.load(std::memory_order_relaxed) << ",\n"
               << "      \"best_fitness\": " << (std::isfinite(d.local_best_fitness) ? d.local_best_fitness : 0.0) << ",\n"
               << "      \"champion_cells\": " << champ.cells.size() << ",\n"
               << "      \"champion_synapses\": " << champ.synapses.size() << ",\n"
               << "      \"migration_in\": " << d.migration_in_count << ",\n"
               << "      \"migration_out\": " << d.migration_out_count << "\n"
               << "    }" << (i + 1 < islands_.size() ? "," : "") << "\n";
        }
        ss << "  ]\n";
        ss << "}";
        return ss.str();
    }

    size_t num_islands() const { return islands_.size(); }
    IslandDeme& get_island(size_t idx) { return *islands_[idx]; }

private:
    std::atomic<WarpSpeed> warp_speed_;
    AdversarialStressProfile stress_profile_;
    std::vector<std::unique_ptr<IslandDeme>> islands_;
    std::atomic<uint64_t> total_migrations_;
    mutable std::mutex grid_mutex_;
};

} // namespace kun
