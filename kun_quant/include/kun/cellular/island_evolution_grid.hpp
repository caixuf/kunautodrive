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
    std::atomic<double> local_best_fitness{-9999.0};
    std::atomic<uint32_t> migration_in_count{0};
    std::atomic<uint32_t> migration_out_count{0};

    // 独立随机数生成器以保证岛屿演化路径正交隔离
    std::mt19937_64 island_rng;
    std::normal_distribution<double> noise_dist;

    mutable std::mutex deme_mutex; // 严防多线程并发读写 engine 造成悬垂指针与内存破坏

    char cache_padding[64]; // 严防多核并发伪共享 (Anti-False Sharing)

    IslandDeme(uint32_t id, uint32_t core, uint32_t seed, SeedInitMode mode)
        : island_id(id), core_id(core), engine(20, seed, mode),
          island_rng(seed + id * 1013), noise_dist(0.0, 1.0) {
        std::memset(cache_padding, 0, sizeof(cache_padding));
    }
};

// ============================================================================
// 4. 自然涌现焦点吸引子英灵殿 (Emergent Focal Sanctuary)
// 遵循道法自然：绝无人工硬编码与强行干预，由跨生境多维适者生存自发凝聚形成向心引力中心
// ============================================================================
struct EmergentFocalPoint {
    std::string lineage_hash;                 // 基因拓扑特征指纹
    CellularOrganism organism;                // 焦点超级个体全息快照
    double natural_gravitational_mass{0.0};   // 生态向心引力质量 (自然生存力 * 跨岛繁衍吸纳率)
    uint32_t multi_regime_survival_epochs{0}; // 跨环境/跨季相自然存活轮次
    double peak_cross_island_fitness{0.0};    // 跨岛全局最优适应度
    uint32_t discovered_generation{0};        // 自然涌现代际
    std::string natural_specialization;       // 自然特化标签 (自发分析出的反射/门控结构)
};

class EmergentFocalSanctuary {
public:
    explicit EmergentFocalSanctuary(size_t max_focal_points = 5)
        : max_points_(max_focal_points) {}

    // 自然焦点凝结与吸收 (自组织流动，绝无人工干预)
    void absorb_candidate(const CellularOrganism& candidate, uint32_t current_gen, double cross_island_fitness) {
        std::lock_guard<std::mutex> lk(sanctuary_mutex_);
        if (!candidate.is_compiled() || candidate.cells.empty() || !std::isfinite(cross_island_fitness)) return;

        // 计算拓扑特征指纹
        std::string hash_sig = compute_topology_signature(candidate);

        // 寻找是否已存在同源吸引子
        auto it = std::find_if(focal_points_.begin(), focal_points_.end(), [&](const EmergentFocalPoint& fp) {
            return fp.lineage_hash == hash_sig;
        });

        if (it != focal_points_.end()) {
            // 同源自然焦点向心引力增强 (持续在不同生境胜出，获得更高引力质量)
            it->multi_regime_survival_epochs++;
            it->peak_cross_island_fitness = std::max(it->peak_cross_island_fitness, cross_island_fitness);
            it->natural_gravitational_mass += (std::max(0.0, cross_island_fitness) * 0.05 + 10.0);
            it->organism = candidate; // 自然吸收后天更优演化权重 (鲍德温固化)
        } else {
            // 涌现新型自然候选焦点
            EmergentFocalPoint new_fp;
            new_fp.lineage_hash = hash_sig;
            new_fp.organism = candidate;
            new_fp.multi_regime_survival_epochs = 1;
            new_fp.peak_cross_island_fitness = cross_island_fitness;
            new_fp.natural_gravitational_mass = std::max(10.0, cross_island_fitness);
            new_fp.discovered_generation = current_gen;
            new_fp.natural_specialization = determine_natural_specialization(candidate);

            focal_points_.push_back(std::move(new_fp));
        }

        // 按自然引力质量降序自发重排 (真正的焦点自然浮现于顶部)
        std::sort(focal_points_.begin(), focal_points_.end(), [](const EmergentFocalPoint& a, const EmergentFocalPoint& b) {
            return a.natural_gravitational_mass > b.natural_gravitational_mass;
        });

        // 自然优胜劣汰，只保留最具向心引力的核心焦点
        if (focal_points_.size() > max_points_) {
            focal_points_.resize(max_points_);
        }
    }

    bool has_focal_point() const {
        std::lock_guard<std::mutex> lk(sanctuary_mutex_);
        return !focal_points_.empty();
    }

    EmergentFocalPoint get_primary_focal_point() const {
        std::lock_guard<std::mutex> lk(sanctuary_mutex_);
        if (focal_points_.empty()) return EmergentFocalPoint{};
        return focal_points_[0];
    }

    std::vector<EmergentFocalPoint> get_all_focal_points() const {
        std::lock_guard<std::mutex> lk(sanctuary_mutex_);
        return focal_points_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(sanctuary_mutex_);
        return focal_points_.size();
    }

private:
    static std::string compute_topology_signature(const CellularOrganism& org) {
        std::ostringstream ss;
        ss << org.cells.size() << "_" << org.synapses.size() << "_";
        for (const auto& c : org.cells) {
            ss << static_cast<int>(c.type) << ",";
        }
        return ss.str();
    }

    static std::string determine_natural_specialization(const CellularOrganism& org) {
        bool has_osc = false, has_diff = false, has_hyst = false, has_immune = false;
        for (const auto& c : org.cells) {
            if (c.type == CellType::OP_OSCILLATOR) has_osc = true;
            if (c.type == CellType::OP_DIFF) has_diff = true;
            if (c.type == CellType::GATE_HYSTERESIS) has_hyst = true;
            if (c.type == CellType::ACT_IMMUNE_BLOCK) has_immune = true;
        }
        std::string spec = "Emergent(";
        if (has_osc) spec += "Pacemaker+";
        if (has_diff) spec += "DiffMomentum+";
        if (has_hyst) spec += "Hysteresis+";
        if (has_immune) spec += "ImmuneLock+";
        if (spec.back() == '+') spec.pop_back();
        if (spec.back() == '(') spec += "ReflexKernel";
        spec += ")";
        return spec;
    }

    size_t max_points_{5};
    std::vector<EmergentFocalPoint> focal_points_;
    mutable std::mutex sanctuary_mutex_;
};

// ============================================================================
// 5. 多岛超加速形态发生演化网格 (IslandEvolutionGrid)
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
        std::lock_guard<std::mutex> lk_deme(deme.deme_mutex);

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
            deme.local_best_fitness.store(champ.fitness_score, std::memory_order_relaxed);
            // 自然向心焦点吸引子吸收：无人工指令，纯粹由个体在恶劣工况下的卓越表现自发凝结
            sanctuary_.absorb_candidate(champ, static_cast<uint32_t>(deme.generations_count.load()), champ.fitness_score);
        }
    }

    // 环形拓扑跨岛基因大迁徙 (Torus / Ring Elite Migration)
    void migrate_elites() {
        std::lock_guard<std::mutex> lk(grid_mutex_);
        if (islands_.size() <= 1) return;

        size_t n = islands_.size();
        std::vector<CellularOrganism> elites(n);
        for (size_t i = 0; i < n; ++i) {
            std::lock_guard<std::mutex> lk_src(islands_[i]->deme_mutex);
            elites[i] = islands_[i]->engine.get_champion();
            elites[i].lineage_name = "Migrant-I" + std::to_string(i) + "->I" + std::to_string((i + 1) % n);
        }

        for (size_t i = 0; i < n; ++i) {
            size_t next_i = (i + 1) % n;
            auto& src_deme = *islands_[i];
            auto& dst_deme = *islands_[next_i];
            std::lock_guard<std::mutex> lk_dst(dst_deme.deme_mutex);

            // 迁徙注入至目标岛 (替换其最弱个体)
            auto& dst_pop = dst_deme.engine.population();
            if (!dst_pop.empty()) {
                dst_pop.back() = elites[i];
                dst_pop.back().compile();
                src_deme.migration_out_count.fetch_add(1, std::memory_order_relaxed);
                dst_deme.migration_in_count.fetch_add(1, std::memory_order_relaxed);
                total_migrations_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // 自然涌现焦点圣殿接口 (Emergent Focal Sanctuary)
    EmergentFocalSanctuary& sanctuary() { return sanctuary_; }
    const EmergentFocalSanctuary& sanctuary() const { return sanctuary_; }

    // 获取全岛网格全局最强生命体 (Global Champion)
    CellularOrganism get_global_champion() const {
        std::lock_guard<std::mutex> lk(grid_mutex_);
        CellularOrganism global_champ;
        double best_fitness = -1e9;
        bool found = false;

        for (const auto& deme : islands_) {
            CellularOrganism c;
            {
                std::lock_guard<std::mutex> lk_deme(deme->deme_mutex);
                c = deme->engine.get_champion();
            }
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
            size_t num_cells = 0;
            size_t num_synapses = 0;
            {
                std::lock_guard<std::mutex> lk_deme(d.deme_mutex);
                const auto& champ = d.engine.get_champion();
                num_cells = champ.cells.size();
                num_synapses = champ.synapses.size();
            }
            double best_fit = d.local_best_fitness.load(std::memory_order_relaxed);

            ss << "    {\n"
               << "      \"island_id\": " << d.island_id << ",\n"
               << "      \"core_id\": " << d.core_id << ",\n"
               << "      \"generations\": " << d.generations_count.load(std::memory_order_relaxed) << ",\n"
               << "      \"inferences\": " << d.inferences_count.load(std::memory_order_relaxed) << ",\n"
               << "      \"best_fitness\": " << (std::isfinite(best_fit) ? best_fit : 0.0) << ",\n"
               << "      \"champion_cells\": " << num_cells << ",\n"
               << "      \"champion_synapses\": " << num_synapses << ",\n"
               << "      \"migration_in\": " << d.migration_in_count.load(std::memory_order_relaxed) << ",\n"
               << "      \"migration_out\": " << d.migration_out_count.load(std::memory_order_relaxed) << "\n"
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
    EmergentFocalSanctuary sanctuary_;
    mutable std::mutex grid_mutex_;
};

} // namespace kun
