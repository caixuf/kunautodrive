#pragma once

#include "kun/cellular/cellular_genome.hpp"
#include <cstdint>
#include <string>
#include <sstream>
#include <cmath>
#include <algorithm>

namespace kun {

/**
 * @brief 自动驾驶 ADAS 细胞形态演化决策适配器 (AdasCellularAdapter)
 * 接受前向激光雷达/视觉距离、相对速度、车道偏移与 TTC 时距，输出纵横向控制与 AEB 应急信号
 * 100% 白盒可解释、零 GC、车规级实时计算
 */
class AdasCellularAdapter {
public:
    explicit AdasCellularAdapter(CellularOrganism organism)
        : organism_(std::move(organism)) {}

    enum class Pathway : uint8_t {
        NORMAL,
        AEB_IMMUNE_LOCK
    };

    struct AdasControlOutput {
        double target_accel_mps2{0.0};  // 目标加速度 (>0 加速, <0 制动减速)
        double steering_curvature{0.0}; // 横向转向曲率指令
        bool   is_aeb_triggered{false}; // 是否触发紧急 AEB 制动
        Pathway pathway{Pathway::NORMAL};
        const char* active_pathway{"NORMAL"}; // 固定标签，实时路径无分配
    };

    AdasControlOutput process_perception(
        double distance_to_lead_m,
        double rel_velocity_mps,
        double lane_offset_m,
        double ttc_seconds
    ) {
        double inputs[4];
        inputs[0] = distance_to_lead_m;
        inputs[1] = rel_velocity_mps;
        inputs[2] = lane_offset_m;
        inputs[3] = ttc_seconds;

        auto outputs = organism_.forward(inputs, /*enable_hebbian=*/false);

        AdasControlOutput ctl;
        // 执行器饱和是物理限幅，不是控制律。控制律只来自细胞输出。
        if (outputs.immune_lock) {
            ctl.is_aeb_triggered = true;
            ctl.target_accel_mps2 = -6.0;
            ctl.pathway = Pathway::AEB_IMMUNE_LOCK;
            ctl.active_pathway = "[AEB_IMMUNE_LOCK] Act_ImmuneBlock";
            return ctl;
        }

        ctl.target_accel_mps2 = std::clamp(outputs.positive_action - outputs.negative_action, -6.0, 2.0);
        ctl.steering_curvature = std::clamp(-outputs.defensive_reset, -0.60, 0.60);
        return ctl;
    }

    // 非实时辅助路径：按需生成包含数值的可解释文本。
    static std::string describe_pathway(const AdasControlOutput& ctl) {
        if (ctl.pathway == Pathway::AEB_IMMUNE_LOCK) {
            return "[AEB_IMMUNE_LOCK] Act_ImmuneBlock";
        }
        std::ostringstream text;
        text << "Acc=" << ctl.target_accel_mps2
             << "m/s2, SteerCurv=" << ctl.steering_curvature;
        return text.str();
    }

    const CellularOrganism& get_organism() const { return organism_; }
    CellularOrganism& get_organism() { return organism_; }

private:
    CellularOrganism organism_;
};

} // namespace kun
