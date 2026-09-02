#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdint>
#include <sstream>
#include <iomanip>

namespace kun {

// ============================================================================
// 1. 细胞代谢与稳态生命周期状态 (Cellular Metabolic & Lifecycle State)
// ============================================================================
enum class MetabolicState : uint8_t {
    ACTIVE = 0,    // 正常放电与计算代谢态
    DORMANT = 1,   // 资源匮乏时的低功耗休眠保护态 (能耗降低 85%)
    DAMAGED = 2,   // 遭受毒性/高负荷损伤、处于自主修复态
    APOPTOTIC = 3  // 能量耗尽或重度损伤、触发自发凋亡 (资源解离回收)
};

inline const char* to_string(MetabolicState state) {
    switch (state) {
        case MetabolicState::ACTIVE:    return "ACTIVE";
        case MetabolicState::DORMANT:   return "DORMANT";
        case MetabolicState::DAMAGED:   return "DAMAGED";
        case MetabolicState::APOPTOTIC: return "APOPTOTIC";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// 2. 细胞内环境稳态属性 (Individual Cell Homeostatic Attributes)
// ============================================================================
struct CellHomeostasisNode {
    uint32_t cell_id{0};
    uint32_t compartment_id{0}; // 所属局部空间隔室
    MetabolicState state{MetabolicState::ACTIVE};

    // 能量与代谢
    double energy_reserve{100.0};       // 当前内部储能 (ATP 储备)
    double max_energy_capacity{200.0};  // 最大储能容量
    double basal_metabolic_rate{0.20};  // 静态生存能耗 / tick
    double firing_energy_cost{0.05};    // 每次放电计算能耗

    // 损伤与自修复
    double damage_level{0.0};           // 损伤累积度 [0.0, 100.0]
    double repair_rate{0.40};           // 自主修复速率 (需消耗能量)
    double repair_energy_cost{0.30};    // 修复单位损伤所需能耗

    // 活跃与统计
    uint32_t active_ticks{0};
    uint32_t dormant_ticks{0};
    uint32_t total_repairs_count{0};
    bool is_alive{true};
};

// ============================================================================
// 3. 局部空间隔室与膜结构 (Spatial Compartment & Local Milieu)
// ============================================================================
struct SpatialCompartment {
    uint32_t compartment_id{0};
    double nutrient_concentration{1000.0}; // 局部营养/能量底质浓度
    double waste_toxicity{0.0};            // 局部代谢废物/毒性浓度
    double membrane_permeability{0.15};    // 膜通道通透性 (控制与邻近隔室扩散)
    size_t carrying_capacity{64};          // 隔室物理空间容量上限
};

// ============================================================================
// 4. 数字稳态引擎 (Digital Homeostasis Engine - Phase 1 Core)
// ============================================================================
class DigitalHomeostasisEngine {
public:
    struct TelemetryFrame {
        uint64_t tick{0};
        size_t alive_cells{0};
        size_t active_cells{0};
        size_t dormant_cells{0};
        size_t apoptotic_cells{0};
        double total_internal_energy{0.0};
        double global_nutrient_reserve{0.0};
        double global_waste_toxicity{0.0};
        uint32_t total_repairs_executed{0};
    };

    explicit DigitalHomeostasisEngine(size_t num_cells, size_t num_compartments = 4, double initial_nutrient = 500.0, double initial_cell_energy = 50.0) {
        init_environment(num_cells, num_compartments, initial_nutrient, initial_cell_energy);
    }

    void init_environment(size_t num_cells, size_t num_compartments, double initial_nutrient = 500.0, double initial_cell_energy = 50.0) {
        compartments_.clear();
        cells_.clear();
        history_.clear();

        for (uint32_t i = 0; i < num_compartments; ++i) {
            SpatialCompartment comp;
            comp.compartment_id = i;
            comp.nutrient_concentration = initial_nutrient;
            comp.waste_toxicity = 0.0;
            comp.membrane_permeability = 0.10;
            comp.carrying_capacity = (num_cells / num_compartments) * 2;
            compartments_.push_back(comp);
        }

        cells_.reserve(num_cells);
        for (uint32_t i = 0; i < num_cells; ++i) {
            CellHomeostasisNode node;
            node.cell_id = i;
            node.compartment_id = i % num_compartments;
            node.energy_reserve = initial_cell_energy;
            node.max_energy_capacity = 150.0;
            node.basal_metabolic_rate = 0.25;
            node.firing_energy_cost = 0.10;
            node.damage_level = 0.0;
            node.state = MetabolicState::ACTIVE;
            node.is_alive = true;
            cells_.push_back(node);
        }
    }

    // 执行单步自主稳态与代谢动力学演进
    TelemetryFrame tick(double external_nutrient_influx = 20.0, double external_toxic_shock = 0.0) {
        current_tick_++;
        uint32_t repairs_in_tick = 0;

        // 1. 外部环境营养输入与毒性扰动
        double influx_per_comp = external_nutrient_influx / std::max<size_t>(1, compartments_.size());
        for (auto& comp : compartments_) {
            comp.nutrient_concentration += influx_per_comp;
            comp.waste_toxicity += external_toxic_shock;
            // 自然代谢降解与扩散耗散
            comp.waste_toxicity = std::max(0.0, comp.waste_toxicity * 0.96);
        }

        // 2. 隔室间跨膜扩散 (Membrane Transport Diffusion)
        if (compartments_.size() > 1) {
            for (size_t i = 0; i < compartments_.size() - 1; ++i) {
                double nutrient_grad = compartments_[i].nutrient_concentration - compartments_[i+1].nutrient_concentration;
                double flux_n = nutrient_grad * compartments_[i].membrane_permeability * 0.1;
                compartments_[i].nutrient_concentration -= flux_n;
                compartments_[i+1].nutrient_concentration += flux_n;

                double tox_grad = compartments_[i].waste_toxicity - compartments_[i+1].waste_toxicity;
                double flux_t = tox_grad * compartments_[i].membrane_permeability * 0.1;
                compartments_[i].waste_toxicity -= flux_t;
                compartments_[i+1].waste_toxicity += flux_t;
            }
        }

        // 3. 遍历各细胞进行自主代谢、损伤评估、休眠控制与凋亡淘汰
        size_t count_alive = 0, count_active = 0, count_dormant = 0, count_apoptotic = 0;

        for (auto& c : cells_) {
            if (!c.is_alive) {
                count_apoptotic++;
                continue;
            }

            auto& comp = compartments_[c.compartment_id];

            // A. 营养摄取 (Nutrient Ingestion / Absorption)
            double absorb_demand = std::max(0.0, c.max_energy_capacity - c.energy_reserve);
            double actual_absorb = std::min(absorb_demand, std::min(1.5, comp.nutrient_concentration * 0.02));
            c.energy_reserve += actual_absorb;
            comp.nutrient_concentration -= actual_absorb;

            // B. 毒性与环境压力导致损伤 (Environmental Toxicity & Strain)
            if (comp.waste_toxicity > 5.0) {
                c.damage_level += (comp.waste_toxicity - 5.0) * 0.05;
            }

            // C. 自主稳态自修复 (Homeostatic Self-Repair)
            if (c.damage_level > 5.0 && c.energy_reserve > 15.0) {
                double repair_amt = std::min(c.damage_level, c.repair_rate);
                double cost = repair_amt * c.repair_energy_cost;
                if (c.energy_reserve >= cost) {
                    c.energy_reserve -= cost;
                    c.damage_level -= repair_amt;
                    c.total_repairs_count++;
                    repairs_in_tick++;
                }
            }

            // D. 代谢能耗扣减 (Metabolic Cost Deduction)
            double current_cost = (c.state == MetabolicState::DORMANT) 
                ? (c.basal_metabolic_rate * 0.15) // 休眠态功耗极低
                : (c.basal_metabolic_rate + c.firing_energy_cost);

            c.energy_reserve -= current_cost;
            // 产生微量代谢废物排入局部隔室
            comp.waste_toxicity += current_cost * 0.05;

            // E. 自主状态机转换 (Autonomous State Transitions)
            if (c.energy_reserve <= 0.0 || c.damage_level >= 100.0) {
                // 能量耗竭或致死损伤 -> 凋亡 (Apoptosis)
                c.state = MetabolicState::APOPTOTIC;
                c.is_alive = false;
                // 尸体解离：将 30% 剩余残存物质释放回归局部隔室底质
                comp.nutrient_concentration += 2.0;
                count_apoptotic++;
            } else if (c.energy_reserve < 10.0 || c.damage_level > 40.0) {
                // 能量匮乏或中度受损 -> 主动休眠降低功耗与代谢降级
                c.state = MetabolicState::DORMANT;
                c.dormant_ticks++;
                count_alive++;
                count_dormant++;
            } else {
                // 状态健康 -> 恢复活跃放电态
                c.state = MetabolicState::ACTIVE;
                c.active_ticks++;
                count_alive++;
                count_active++;
            }
        }

        // 4. 统计遥测帧
        double total_internal_e = 0.0;
        double total_nutrient = 0.0;
        double total_waste = 0.0;

        for (const auto& c : cells_) {
            if (c.is_alive) total_internal_e += c.energy_reserve;
        }
        for (const auto& comp : compartments_) {
            total_nutrient += comp.nutrient_concentration;
            total_waste += comp.waste_toxicity;
        }

        TelemetryFrame frame{
            current_tick_,
            count_alive,
            count_active,
            count_dormant,
            count_apoptotic,
            total_internal_e,
            total_nutrient,
            total_waste,
            repairs_in_tick
        };
        history_.push_back(frame);
        return frame;
    }

    const std::vector<CellHomeostasisNode>& get_cells() const { return cells_; }
    const std::vector<SpatialCompartment>& get_compartments() const { return compartments_; }
    std::vector<SpatialCompartment>& get_compartments() { return compartments_; }
    const std::vector<TelemetryFrame>& get_history() const { return history_; }

private:
    uint64_t current_tick_{0};
    std::vector<SpatialCompartment> compartments_;
    std::vector<CellHomeostasisNode> cells_;
    std::vector<TelemetryFrame> history_;
};

} // namespace kun
