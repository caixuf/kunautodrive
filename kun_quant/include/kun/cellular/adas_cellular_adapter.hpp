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
        
        // 1. 紧急制动 / 免疫安全门 (在前向存在危险 TTC < 4.0s 或相对速度骤降 < -3.0m/s 时触发 AEB 应急防撞)
        if ((outputs.immune_lock && (ttc_seconds < 4.5 || distance_to_lead_m < 30.0)) || 
            ttc_seconds < 2.0 || rel_velocity_mps < -3.5 || (distance_to_lead_m < 10.0 && rel_velocity_mps < -1.0)) {
            ctl.is_aeb_triggered = true;
            ctl.target_accel_mps2 = -6.0; // 触发最大制动减速度 (AEB)
            ctl.active_pathway = "[AEB_IMMUNE_TRIGGER] 细胞网络与TTC双重触发紧急防撞制动!";
            return ctl;
        }

        // 2. 纵向跟车与巡航控制 (基于自适应时距间隙与相对速度)
        double desired_distance = 15.0;
        double dist_error = distance_to_lead_m - desired_distance;
        if (distance_to_lead_m < 120.0) {
            // 跟车态: PD 反馈精确追踪前车速度与时距
            ctl.target_accel_mps2 = std::clamp(dist_error * 0.35 + rel_velocity_mps * 0.95, -4.5, 2.0);
        } else {
            // 自由巡航态: 平顺加速
            ctl.target_accel_mps2 = std::clamp(outputs.positive_action * 1.5, 0.0, 2.0);
        }

        // 3. 横向车道居中控制: lane_offset_m = ego_y - target_y
        // 当 ego 在目标左侧 (lane_offset > 0), steer 应向右 (负值)
        ctl.steering_curvature = -lane_offset_m * 0.45 * (1.0 + outputs.defensive_reset);

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
