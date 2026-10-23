#include "lane_follow_scenario.h"
#include <cmath>
#include <algorithm>

namespace flowengine::planning {

TaskStatus LaneFollowDeciderTask::Execute(FrameContext& context, Trajectory& /*trajectory*/) {
    const auto& ego = context.vehicle_state;
    const auto& road = context.road_geometry;
    double ego_x = ego.x;
    double ego_y = ego.y;
    double ego_h = ego.heading;
    double cos_h = std::cos(ego_h);
    double sin_h = std::sin(ego_h);

    double cruise_speed = context.blackboard.GetOrDefault<double>("cfg_cruise_speed", default_cruise_speed);
    double lane_w = (road.lane_width > 0.0) ? road.lane_width : 3.5;
    double half_lane_tol = lane_w * 0.5 + 0.6; // half-lane width + lateral tolerance

    // Search for closest lead vehicle in ego lane
    double closest_along = max_follow_distance;
    int lead_idx = -1;

    for (size_t i = 0; i < context.ObstacleCount(); ++i) {
        const auto& obs = context.GetObstacle(i);
        double dx = obs.x - ego_x;
        double dy = obs.y - ego_y;
        double along = dx * cos_h + dy * sin_h;
        double lat   = -dx * sin_h + dy * cos_h;

        // Must be ahead within search range and laterally inside ego lane
        if (along > 0.5 && along < closest_along && std::abs(lat) <= half_lane_tol) {
            closest_along = along;
            lead_idx = static_cast<int>(i);
        }
    }

    if (lead_idx >= 0) {
        // Follow mode with lead vehicle
        const auto& lead_obs = context.GetObstacle(lead_idx);
        double gap = closest_along - (lead_obs.length * 0.5);
        if (gap < 0.1) gap = 0.1;

        double desired_gap = acc_standoff + acc_time_headway * ego.v;
        double gap_err = gap - desired_gap;
        gap_err = std::clamp(gap_err, -acc_gap_err_clamp, acc_gap_err_clamp);

        double lead_v = lead_obs.vx * cos_h + lead_obs.vy * sin_h;
        if (lead_v < 0.0) lead_v = 0.0;

        double target_speed = lead_v + acc_k_gap * gap_err;
        target_speed = std::clamp(target_speed, 0.0, cruise_speed);

        // Store decisions in Blackboard
        context.blackboard.Set("is_following", true);
        context.blackboard.Set("follow_obs_id", lead_obs.id);
        context.blackboard.Set("lead_gap", gap);
        context.blackboard.Set("desired_gap", desired_gap);
        context.blackboard.Set("lead_speed", lead_v);
        context.blackboard.Set("target_speed", target_speed);
        context.blackboard.Set("behavior_mode", "FOLLOW");

        context.l3_state.lead_obs_id = lead_obs.id;
        context.l3_state.lead_gap_m = gap;
    } else {
        // Free cruising mode
        context.blackboard.Set("is_following", false);
        context.blackboard.Set("follow_obs_id", 0u);
        context.blackboard.Set("lead_gap", 1e9);
        context.blackboard.Set("desired_gap", 0.0);
        context.blackboard.Set("lead_speed", 0.0);
        context.blackboard.Set("target_speed", cruise_speed);
        context.blackboard.Set("behavior_mode", "CRUISE");

        context.l3_state.lead_obs_id = 0;
        context.l3_state.lead_gap_m = 1e9;
    }

    return TaskStatus::SUCCESS;
}

TaskStatus LaneFollowTrajectoryTask::Execute(FrameContext& context, Trajectory& trajectory) {
    const auto& ego = context.vehicle_state;
    double target_speed = context.blackboard.GetOrDefault<double>("target_speed", 15.0);
    double ego_v = ego.v;
    double ego_x = ego.x;
    double ego_y = ego.y;
    double ego_h = ego.heading;
    double cos_h = std::cos(ego_h);
    double sin_h = std::sin(ego_h);

    double a_target = (target_speed - ego_v) / 2.0;
    a_target = std::clamp(a_target, -3.0, 2.0);
    if (std::abs(target_speed - ego_v) < 0.05) {
        a_target = 0.0;
    }

    trajectory.seq = context.seq;
    trajectory.stamp_us = context.timestamp_us;
    trajectory.ref_line_id = 0;
    trajectory.point_count = 64;
    trajectory.planner_state = 1; // Normal sampling / following
    trajectory.is_stitched = 0;
    trajectory.valid = 1;

    for (uint16_t i = 0; i < 64; ++i) {
        double dt = i * 0.1; // 10Hz sampling over 6.3s
        double pt_v = ego_v + a_target * dt;
        if (a_target > 0.0 && pt_v > target_speed) pt_v = target_speed;
        if (a_target < 0.0 && pt_v < target_speed) pt_v = target_speed;
        if (pt_v < 0.0) pt_v = 0.0;

        double s = ego_v * dt + 0.5 * a_target * dt * dt;
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
        p.a = static_cast<float>(a_target);
        p.jerk = 0.0f;
    }

    return TaskStatus::SUCCESS;
}

LaneFollowStage::LaneFollowStage() {
    AddTask(&decider_task_);
    AddTask(&trajectory_task_);
}

StageStatus LaneFollowStage::Process(FrameContext& context, Trajectory& trajectory) {
    for (size_t i = 0; i < task_count_; ++i) {
        if (tasks_[i] != nullptr) {
            TaskStatus st = tasks_[i]->Execute(context, trajectory);
            if (st == TaskStatus::FAILED) {
                return StageStatus::ERROR;
            }
        }
    }
    return StageStatus::RUNNING;
}

bool LaneFollowStage::IsFinished(const FrameContext& /*context*/) const {
    return false; // Lane follow is continuous
}

LaneFollowScenario::LaneFollowScenario() {
    AddStage(&lane_follow_stage_);
}

bool LaneFollowScenario::IsApplicable(const FrameContext& context) const {
    const auto& l3 = context.l3_state;
    // Normal cruising/following is applicable only if no critical emergency or MRM is active
    if (l3.degrade_level >= 2 || l3.mrm_requested || l3.emergency_stop_requested || l3.collision_risk) {
        return false;
    }
    return true;
}

ScenarioStatus LaneFollowScenario::Run(FrameContext& context, Trajectory& trajectory) {
    Stage* stage = CurrentStage();
    if (!stage) {
        return ScenarioStatus::ERROR;
    }
    StageStatus st = stage->Process(context, trajectory);
    if (st == StageStatus::ERROR) {
        return ScenarioStatus::ERROR;
    }
    return ScenarioStatus::RUNNING;
}

void LaneFollowScenario::Reset() {
    finished_ = false;
    current_stage_idx_ = 0;
    lane_follow_stage_.Reset();
}

} // namespace flowengine::planning
