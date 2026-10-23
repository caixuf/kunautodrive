#include "emergency_stop_scenario.h"
#include <cmath>
#include <algorithm>

namespace flowengine::planning {

TaskStatus EmergencyStopDeciderTask::Execute(FrameContext& context, Trajectory& /*trajectory*/) {
    const auto& l3 = context.l3_state;
    const auto& ego = context.vehicle_state;
    double ego_v = (ego.v > 0.0) ? ego.v : 0.0;

    double decel = default_brake_decel;
    std::string_view stop_mode = "EMERGENCY_STOP";

    if (l3.degrade_level >= 3 || l3.collision_risk || l3.min_ttc_s < 1.0) {
        // Critical emergency: maximum safe deceleration
        decel = hard_brake_decel;
        stop_mode = "HARD_BRAKE";
    } else if (l3.mrm_requested || l3.degrade_level == 2) {
        // L3 Minimum Risk Maneuver: controlled in-lane deceleration to safe stop
        decel = mrm_brake_decel;
        stop_mode = "MRM_STOP";
    }

    double abs_decel = std::abs(decel);
    double stop_distance = (abs_decel > 1e-3) ? ((ego_v * ego_v) / (2.0 * abs_decel)) : 0.0;

    // Populate Blackboard
    context.blackboard.Set("emergency_stop", true);
    context.blackboard.Set("hazard_warning", true);
    context.blackboard.Set("target_speed", 0.0);
    context.blackboard.Set("emergency_decel", decel);
    context.blackboard.Set("stop_distance", stop_distance);
    context.blackboard.Set("stop_mode", stop_mode);
    context.blackboard.Set("behavior_mode", "EMERGENCY");

    return TaskStatus::SUCCESS;
}

TaskStatus EmergencyStopTrajectoryTask::Execute(FrameContext& context, Trajectory& trajectory) {
    const auto& ego = context.vehicle_state;
    double ego_v = (ego.v > 0.0) ? ego.v : 0.0;
    double ego_x = ego.x;
    double ego_y = ego.y;
    double ego_h = ego.heading;
    double cos_h = std::cos(ego_h);
    double sin_h = std::sin(ego_h);

    double decel = context.blackboard.GetOrDefault<double>("emergency_decel", -3.5);
    double abs_decel = std::abs(decel);
    double t_stop = (abs_decel > 1e-3 && ego_v > 0.05) ? (ego_v / abs_decel) : 0.0;
    double s_stop = (abs_decel > 1e-3) ? ((ego_v * ego_v) / (2.0 * abs_decel)) : 0.0;

    trajectory.seq = context.seq;
    trajectory.stamp_us = context.timestamp_us;
    trajectory.ref_line_id = 0;
    trajectory.point_count = 64;
    trajectory.planner_state = 3; // Failsafe / emergency
    trajectory.is_stitched = 0;
    trajectory.valid = 1;

    for (uint16_t i = 0; i < 64; ++i) {
        double dt = i * 0.1; // 10Hz sampling over 6.3s
        double pt_v = 0.0;
        double s = 0.0;
        double a = 0.0;

        if (dt < t_stop) {
            pt_v = ego_v - abs_decel * dt;
            if (pt_v < 0.0) pt_v = 0.0;
            s = ego_v * dt - 0.5 * abs_decel * dt * dt;
            a = -abs_decel;
        } else {
            pt_v = 0.0;
            s = s_stop;
            a = 0.0;
        }

        double pt_x = ego_x + s * cos_h;
        double pt_y = ego_y + s * sin_h;

        TrajectoryPoint& p = trajectory.points[i];
        p.t_rel_us = static_cast<uint32_t>(dt * 1e6);
        p.x = static_cast<float>(pt_x);
        p.y = static_cast<float>(pt_y);
        p.s = static_cast<float>(s);
        p.l = 0.0f;
        p.heading = static_cast<float>(ego_h);
        p.kappa = static_cast<float>(ego.kappa);
        p.v = static_cast<float>(pt_v);
        p.a = static_cast<float>(a);
        p.jerk = 0.0f;
    }

    return TaskStatus::SUCCESS;
}

EmergencyStopStage::EmergencyStopStage() {
    AddTask(&decider_task_);
    AddTask(&trajectory_task_);
}

StageStatus EmergencyStopStage::Process(FrameContext& context, Trajectory& trajectory) {
    for (size_t i = 0; i < task_count_; ++i) {
        if (tasks_[i] != nullptr) {
            TaskStatus st = tasks_[i]->Execute(context, trajectory);
            if (st == TaskStatus::FAILED) {
                return StageStatus::ERROR;
            }
        }
    }

    if (IsFinished(context)) {
        finished_ = true;
        return StageStatus::FINISHED;
    }
    return StageStatus::RUNNING;
}

bool EmergencyStopStage::IsFinished(const FrameContext& context) const {
    return (context.vehicle_state.v < 0.05); // Vehicle brought to standstill
}

EmergencyStopScenario::EmergencyStopScenario() {
    AddStage(&emergency_stop_stage_);
}

bool EmergencyStopScenario::IsApplicable(const FrameContext& context) const {
    const auto& l3 = context.l3_state;
    return (l3.emergency_stop_requested ||
            l3.mrm_requested ||
            l3.degrade_level >= 2 ||
            l3.collision_risk ||
            l3.min_ttc_s < 1.5);
}

ScenarioStatus EmergencyStopScenario::Run(FrameContext& context, Trajectory& trajectory) {
    Stage* stage = CurrentStage();
    if (!stage) {
        return ScenarioStatus::ERROR;
    }
    StageStatus st = stage->Process(context, trajectory);
    if (st == StageStatus::ERROR) {
        return ScenarioStatus::ERROR;
    }
    if (st == StageStatus::FINISHED) {
        finished_ = true;
        return ScenarioStatus::FINISHED;
    }
    return ScenarioStatus::RUNNING;
}

void EmergencyStopScenario::Reset() {
    finished_ = false;
    current_stage_idx_ = 0;
    emergency_stop_stage_.Reset();
}

} // namespace flowengine::planning
