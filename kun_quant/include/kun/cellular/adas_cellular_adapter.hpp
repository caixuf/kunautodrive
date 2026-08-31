#pragma once

#include "kun/cellular/cellular_genome.hpp"
#include <string>
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

    struct AdasControlOutput {
        double target_accel_mps2{0.0};  // 目标加速度 (>0 加速, <0 制动减速)
        double steering_curvature{0.0}; // 横向转向曲率指令
        bool   is_aeb_triggered{false}; // 是否触发紧急 AEB 制动
        std::string active_pathway;     // 细胞放电通路可解释性追踪
    };

    AdasControlOutput process_perception(
        double distance_to_lead_m,
        double rel_velocity_mps,
        double lane_offset_m,
        double ttc_seconds
    ) {
        double inputs[4];
        inputs[0] = distance_to_lead_m; // 输入 0: 目标距离
        inputs[1] = rel_velocity_mps;   // 输入 1: 相对速度
        inputs[2] = lane_offset_m;      // 输入 2: 车道偏移
        inputs[3] = (ttc_seconds > 0.0 && ttc_seconds < 10.0) ? (10.0 - ttc_seconds) : 0.0; // 输入 3: TTC 危险度

        auto outputs = organism_.forward(inputs);

        AdasControlOutput ctl;
        
        // 1. 紧急制动 / 免疫安全门
        if (outputs.immune_lock || ttc_seconds < 1.2) {
            ctl.is_aeb_triggered = true;
            ctl.target_accel_mps2 = -6.0; // 触发最大制动减速度 (AEB)
            ctl.active_pathway = "[AEB_IMMUNE_TRIGGER] 细胞网络与TTC双重触发紧急防撞制动!";
            return ctl;
        }

        // 2. 纵向跟车与超车控制
        if (outputs.positive_action > 0.3) {
            ctl.target_accel_mps2 = std::clamp(outputs.positive_action * 2.0, 0.0, 2.5); // 平顺加速
        } else if (outputs.negative_action > 0.3 || outputs.positive_action < -0.3) {
            ctl.target_accel_mps2 = -std::clamp(std::abs(outputs.negative_action) * 3.0, 0.0, 4.5); // 减速跟随
        }

        // 3. 横向车道居中控制 (受 defensive_reset 调节)
        ctl.steering_curvature = -lane_offset_m * 0.5 * (1.0 + outputs.defensive_reset);

        ctl.active_pathway = "Acc=" + std::to_string(ctl.target_accel_mps2) + 
                             "m/s2, SteerCurv=" + std::to_string(ctl.steering_curvature);
        return ctl;
    }

    const CellularOrganism& get_organism() const { return organism_; }
    CellularOrganism& get_organism() { return organism_; }

private:
    CellularOrganism organism_;
};

} // namespace kun
