#pragma once

#include "kun/cellular/cellular_genome.hpp"
#include <vector>
#include <cmath>
#include <random>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace kun {

/**
 * @brief 空间相干平面波分量 (Wave Component for Interference Field)
 */
struct WaveSource {
    float kx{0.1f}, ky{0.1f}, kz{0.1f}; // 波数矢量 (Wave Vector k)
    float omega{1.0f};                  // 角频率 (Angular Frequency omega)
    float amplitude{1.0f};              // 振幅 (Amplitude A)
    float phase{0.0f};                  // 初相 (Initial Phase phi)
};

/**
 * @brief 离散宇宙高能辐射粒子束 (Cosmic Ray Strike Packet)
 */
struct CosmicRayParticle {
    uint64_t ray_id{0};
    float origin_x{0.0f}, origin_y{0.0f}, origin_z{0.0f};
    float dir_x{0.0f}, dir_y{0.0f}, dir_z{-1.0f};
    float energy{50.0f};        // 光子能量 E = h * nu
    float speed{120.0f};        // 粒子束飞行速度
    float current_dist{0.0f};   // 当前飞行距离
    float max_dist{150.0f};     // 最大射程
    bool is_active{true};
};

/**
 * @brief 电离撞击与基因诱变事件 (Ionization Strike Event)
 */
struct IonizationEvent {
    uint64_t agent_id{0};
    uint16_t cell_id{0};
    float strike_x{0.0f}, strike_y{0.0f}, strike_z{0.0f};
    float radiation_dose{0.0f};
    std::string mutation_type; // "SOFT_IONIZATION", "HARD_MUTATION", "QUANTUM_TUNNELING"
};

/**
 * @brief 量子波动干涉与辐射场引擎 (Quantum Wave-Particle Radiation Field)
 */
class QuantumRadiationField {
public:
    QuantumRadiationField(uint32_t seed = 1337)
        : rng_(seed) {
        // 初始化多源相干波源以产生空间干涉条纹 (干涉相长与干涉相消)
        waves_.push_back({0.08f, 0.04f, 0.02f, 1.2f, 1.0f, 0.0f});
        waves_.push_back({-0.05f, 0.07f, 0.03f, 1.5f, 0.8f, 1.04f});
        waves_.push_back({0.03f, -0.06f, 0.08f, 0.9f, 1.2f, 2.09f});
    }

    /**
     * @brief 计算空间任意位置处的波函数振幅与辐射干涉强度
     * Psi(r, t) = sum A_k * cos(k * r - omega * t + phi)
     * Intensity I(r, t) = |Psi(r, t)|^2
     */
    float evaluate_intensity(float x, float y, float z, float t) const {
        float psi = 0.0f;
        for (const auto& w : waves_) {
            float phase = (w.kx * x + w.ky * y + w.kz * z) - (w.omega * t) + w.phase;
            psi += w.amplitude * std::cos(phase);
        }
        // 强度正比于振幅平方 (干涉相长时高辐射，相消时低辐射)
        return (psi * psi);
    }

    /**
     * @brief 推进辐射场演化并生成随机宇宙高能粒子打击
     */
    void step(float dt) {
        time_ += dt;
        recent_events_.clear();

        // 1. 推进现有宇宙射线粒子
        for (auto& ray : cosmic_rays_) {
            if (!ray.is_active) continue;
            ray.current_dist += (ray.speed * dt);
            if (ray.current_dist >= ray.max_dist) {
                ray.is_active = false;
            }
        }

        // 清理非活跃射线
        cosmic_rays_.erase(
            std::remove_if(cosmic_rays_.begin(), cosmic_rays_.end(), [](const CosmicRayParticle& r) {
                return !r.is_active;
            }),
            cosmic_rays_.end()
        );

        // 2. 随机发射新的高能宇宙射线束 (概率脉冲)
        std::uniform_real_distribution<float> dist_unit(0.0f, 1.0f);
        if (dist_unit(rng_) < 0.35f && cosmic_rays_.size() < 8) {
            spawn_cosmic_ray();
        }
    }

    /**
     * @brief 施加辐射照射并诱导有机体发生软/硬变异或量子隧穿
     */
    void irradiate_organism(CellularOrganism& org, float x, float y, float z, uint32_t stagnation_ticks = 0) {
        float background_intensity = evaluate_intensity(x, y, z, time_);
        std::uniform_real_distribution<float> dist_prob(0.0f, 1.0f);
        std::normal_distribution<double> dist_norm(0.0, 0.05);

        // 1. 严谨 WKB 量子势垒隧穿积分 (Wentzel-Kramers-Brillouin Barrier Tunneling Integral)
        // T_WKB = exp( - (2 * L * sqrt(2 * m * Delta_V)) / (hbar_eff * (1.0 + I_radiation)) )
        if (stagnation_ticks > 50 && background_intensity > 0.2f) {
            const float m_eff = 1.0f;       // 有效拓扑质量
            const float hbar_eff = 1.8f;    // 有效普朗克常数
            const float L_barrier = 0.75f;  // 参数拓扑势垒宽度 (Hamming/Weight Distance)
            
            // 停滞时间越长，势垒相对动能差越大 (Delta_V)
            float delta_v = std::clamp(static_cast<float>(stagnation_ticks - 50) * 0.05f, 0.1f, 4.0f);
            float action_integral = (2.0f * L_barrier * std::sqrt(2.0f * m_eff * delta_v));
            float tunneling_prob = std::exp(-action_integral / (hbar_eff * (1.0f + 0.5f * background_intensity)));
            
            tunneling_prob = std::clamp(tunneling_prob, 0.01f, 0.65f);

            if (dist_prob(rng_) < tunneling_prob) {
                perform_quantum_tunneling(org);
                perform_morphogenetic_differentiation(org);
                recent_events_.push_back({
                    org.organism_id, 0, x, y, z, background_intensity, "QUANTUM_TUNNELING"
                });
                return;
            }
        }

        // 2. 宇宙射线粒子束直接轰击判定 (Particle Strike)
        for (const auto& ray : cosmic_rays_) {
            if (!ray.is_active) continue;
            // 计算点到射线的垂直距离
            float rx = ray.origin_x + ray.dir_x * ray.current_dist;
            float ry = ray.origin_y + ray.dir_y * ray.current_dist;
            float rz = ray.origin_z + ray.dir_z * ray.current_dist;
            float dist_sq = (x - rx) * (x - rx) + (y - ry) * (y - ry) + (z - rz) * (z - rz);

            if (dist_sq < 36.0f) { // 命中 6 单位半径散射截面
                perform_hard_mutation(org);
                if (ray.energy > 70.0f && stagnation_ticks > 5) {
                    perform_morphogenetic_differentiation(org);
                }
                recent_events_.push_back({
                    org.organism_id, 0, x, y, z, ray.energy, "HARD_MUTATION"
                });
                break;
            }
        }

        // 3. 背景干涉辐射场的软电离微突变 (Soft Ionization)
        if (background_intensity > 1.2f && dist_prob(rng_) < 0.15f) {
            perform_soft_ionization(org);
            recent_events_.push_back({
                org.organism_id, 0, x, y, z, background_intensity, "SOFT_IONIZATION"
            });
        }
    }

    const std::vector<CosmicRayParticle>& cosmic_rays() const { return cosmic_rays_; }
    const std::vector<IonizationEvent>& recent_events() const { return recent_events_; }
    float time() const { return time_; }

    /**
     * @brief 导出辐射场状态 JSON
     */
    std::string to_json() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "{\n";
        ss << "  \"field_time\": " << time_ << ",\n";
        ss << "  \"wave_sources_count\": " << waves_.size() << ",\n";
        ss << "  \"wave_sources\": [\n";
        for (size_t i = 0; i < waves_.size(); ++i) {
            const auto& w = waves_[i];
            ss << "    {\"kx\": " << w.kx << ", \"ky\": " << w.ky << ", \"kz\": " << w.kz
               << ", \"omega\": " << w.omega << ", \"amplitude\": " << w.amplitude
               << ", \"phase\": " << w.phase << "}" << (i + 1 < waves_.size() ? "," : "") << "\n";
        }
        ss << "  ],\n";
        ss << "  \"active_cosmic_rays\": [\n";
        for (size_t i = 0; i < cosmic_rays_.size(); ++i) {
            const auto& r = cosmic_rays_[i];
            ss << "    {\"id\": " << r.ray_id 
               << ", \"ox\": " << r.origin_x << ", \"oy\": " << r.origin_y << ", \"oz\": " << r.origin_z
               << ", \"dx\": " << r.dir_x << ", \"dy\": " << r.dir_y << ", \"dz\": " << r.dir_z
               << ", \"energy\": " << r.energy << ", \"dist\": " << r.current_dist << "}"
               << (i + 1 < cosmic_rays_.size() ? "," : "") << "\n";
        }
        ss << "  ],\n";
        ss << "  \"recent_events_count\": " << recent_events_.size() << "\n";
        ss << "}\n";
        return ss.str();
    }

private:
    void spawn_cosmic_ray() {
        std::uniform_real_distribution<float> dist_bound(-60.0f, 60.0f);
        std::uniform_real_distribution<float> dist_energy(30.0f, 100.0f);
        std::uniform_real_distribution<float> dist_norm(-1.0f, 1.0f);

        CosmicRayParticle ray;
        ray.ray_id = ++ray_id_seq_;
        ray.origin_x = dist_bound(rng_);
        ray.origin_y = dist_bound(rng_);
        ray.origin_z = 70.0f; // 从生态穹顶上方射入

        float dx = dist_norm(rng_) * 0.3f;
        float dy = dist_norm(rng_) * 0.3f;
        float dz = -1.0f;
        float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        ray.dir_x = dx / len;
        ray.dir_y = dy / len;
        ray.dir_z = dz / len;

        ray.energy = dist_energy(rng_);
        ray.speed = 100.0f + (dist_unit_(rng_) * 50.0f);
        ray.current_dist = 0.0f;
        ray.max_dist = 160.0f;
        ray.is_active = true;

        cosmic_rays_.push_back(ray);
    }

    void perform_soft_ionization(CellularOrganism& org) {
        std::normal_distribution<double> dist_norm(0.0, 0.08);
        for (auto& syn : org.synapses) {
            syn.weight += dist_norm(rng_);
            syn.weight = std::clamp(syn.weight, -3.0, 3.0);
        }
        for (auto& c : org.cells) {
            c.param1 += dist_norm(rng_) * 0.1;
            c.param1 = std::clamp(c.param1, 0.01, 0.99);
        }
        org.compile();
    }

    void perform_hard_mutation(CellularOrganism& org) {
        if (org.cells.empty()) return;
        std::uniform_int_distribution<size_t> dist_cell(0, org.cells.size() - 1);
        size_t idx = dist_cell(rng_);
        auto& target = org.cells[idx];

        // 若不是受体感知细胞，突变为常规代谢、算术或基础门控原语 (绝不直接凭空撒入特化免疫效应器)
        if (target.type != CellType::SENSE_RAW_INPUT_0 && target.type != CellType::SENSE_RAW_INPUT_1) {
            static const CellType pool[] = {
                CellType::OP_EMA, CellType::OP_DIFF, CellType::OP_INTEGRAL,
                CellType::OP_SUB, CellType::OP_SUM, CellType::GATE_HYSTERESIS, CellType::GATE_INHIBIT
            };
            target.type = pool[rng_() % 7];
        }

        // 突触断裂与重连 (Rewiring)
        if (!org.synapses.empty() && org.cells.size() >= 3) {
            size_t syn_idx = rng_() % org.synapses.size();
            org.synapses[syn_idx].to_cell_id = org.cells[rng_() % org.cells.size()].id;
        }
        org.compile();
    }

    void perform_quantum_tunneling(CellularOrganism& org) {
        // 量子隧穿：拓扑重构与突变跃迁
        std::uniform_real_distribution<float> dist_w(-1.5f, 1.5f);
        for (auto& syn : org.synapses) {
            syn.weight = dist_w(rng_);
        }
        // 分裂并插入一个全新量子门控细胞
        uint16_t new_id = static_cast<uint16_t>(org.cells.size() + 100);
        Cell quantum_cell{new_id, CellType::GATE_HYSTERESIS, 0.05, -0.05};
        quantum_cell.x = 0.0f; quantum_cell.y = 0.0f; quantum_cell.z = 0.0f;
        org.cells.push_back(quantum_cell);

        if (!org.cells.empty()) {
            org.synapses.push_back({org.cells[0].id, new_id, 0, 1.2, true, 60.0f, -1.0f});
        }
        org.compile();
    }

    /**
     * @brief 形态发生细胞特化 (Morphogenetic Epigenetic Differentiation)
     * 当机体具备门控结构且承受持续外部逆向应激时，门控下游分化出免疫自锁/防御熔断效应器
     */
    void perform_morphogenetic_differentiation(CellularOrganism& org) {
        for (const auto& c : org.cells) {
            if (c.type == CellType::ACT_IMMUNE_BLOCK) return; // 已存在则无需重复分化
        }
        // 寻找机体内的门控前驱 (GATE_HYSTERESIS 或 GATE_INHIBIT)
        int gate_idx = -1;
        for (size_t i = 0; i < org.cells.size(); ++i) {
            if (org.cells[i].type == CellType::GATE_HYSTERESIS || org.cells[i].type == CellType::GATE_INHIBIT) {
                gate_idx = static_cast<int>(i);
                break;
            }
        }
        if (gate_idx >= 0) {
            uint16_t immune_id = static_cast<uint16_t>(org.cells.size() + 200);
            Cell immune_cell{immune_id, CellType::ACT_IMMUNE_BLOCK, 1.0, 0.0};
            immune_cell.x = org.cells[gate_idx].x + 2.0f;
            immune_cell.y = org.cells[gate_idx].y;
            immune_cell.z = org.cells[gate_idx].z;
            org.cells.push_back(immune_cell);
            org.synapses.push_back({org.cells[gate_idx].id, immune_id, 0, 1.0, true, 60.0f, -1.0f});
            org.compile();
        }
    }

    std::mt19937 rng_;
    std::uniform_real_distribution<float> dist_unit_{0.0f, 1.0f};
    std::vector<WaveSource> waves_;
    std::vector<CosmicRayParticle> cosmic_rays_;
    std::vector<IonizationEvent> recent_events_;
    float time_{0.0f};
    uint64_t ray_id_seq_{0};
};

} // namespace kun
