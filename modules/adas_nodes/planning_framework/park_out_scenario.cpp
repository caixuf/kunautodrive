#include "park_out_scenario.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace flowengine::planning {

namespace {

inline double NormalizeAngle(double a) noexcept {
    while (a > M_PI) a -= 2.0 * M_PI;
    while (a < -M_PI) a += 2.0 * M_PI;
    return a;
}

bool HasReachedAisleExit(const FrameContext& context) noexcept {
    if (!context.park_out.enabled) return false;
    const auto& ego = context.vehicle_state;
    const auto& po = context.park_out;
    const double dx = ego.x - po.exit_x;
    const double dy = ego.y - po.exit_y;
    const double ce = std::cos(po.exit_heading);
    const double se = std::sin(po.exit_heading);
    const double along = dx * ce + dy * se;
    const double lat = std::fabs(-dx * se + dy * ce);
    const double dh = std::fabs(NormalizeAngle(ego.heading - po.exit_heading));
    const double dist = std::hypot(dx, dy);

    if (dist <= 2.4 && dh <= 0.30) {
        return true;
    }
    return (along >= -1.5 && along <= 8.0 && lat <= 1.25 && dh <= 0.25);
}

void BuildForwardCruiseLine(double start_x, double start_y, double heading,
                            double start_v, double target_v,
                            uint32_t seq, uint64_t stamp_us,
                            Trajectory& trajectory) noexcept {
    trajectory.seq = seq;
    trajectory.stamp_us = stamp_us;
    trajectory.point_count = 64;
    trajectory.valid = 1;
    const double ch = std::cos(heading);
    const double sh = std::sin(heading);
    const double ds = 0.55;
    double elapsed_us = 0.0;
    for (int i = 0; i < 64; ++i) {
        auto& pt = trajectory.points[i];
        const double s = static_cast<double>(i) * ds;
        const double blend = std::min(1.0, s / 10.0);
        const double v_i = start_v + (target_v - start_v) * blend;
        pt.t_rel_us = static_cast<uint32_t>(elapsed_us);
        pt.x = static_cast<float>(start_x + s * ch);
        pt.y = static_cast<float>(start_y + s * sh);
        pt.s = static_cast<float>(s);
        pt.l = 0.0f;
        pt.heading = static_cast<float>(heading);
        pt.kappa = 0.0f;
        pt.v = static_cast<float>(v_i);
        pt.a = 0.0f;
        pt.jerk = 0.0f;
        elapsed_us += (ds / std::max(0.5, v_i)) * 1e6;
    }
}

} // namespace

// ============================================================================
// Stage 1: ParkOutManeuverStage
// ============================================================================

void ParkOutManeuverStage::Reset() {
    Stage::Reset();
    trajectory_latched_ = false;
}

bool ParkOutManeuverStage::IsFinished(const FrameContext& context) const {
    return HasReachedAisleExit(context);
}

bool ParkOutManeuverStage::PlanTrajectory(FrameContext& context) {
    const auto& ego = context.vehicle_state;
    const auto& po = context.park_out;

    VehicleParam veh;
    veh.wheelbase = ego.wheelbase > 1.0 ? ego.wheelbase : 2.7;
    veh.length = 4.8;
    veh.width = 2.0;
    veh.back_edge_to_center = 1.0;
    veh.max_steer = 0.60;
    veh.step_size = 0.8;

    HybridAStarConfig cfg;
    cfg.xy_grid_resolution = 0.30;
    cfg.phi_grid_resolution = 0.0872665;
    cfg.weight_reverse = 1.6;
    cfg.weight_gear_switch = 10.0;
    cfg.vehicle_safety_margin = 0.08;
    cfg.analytic_expansion_max_dist = 25.0;

    planner_.SetVehicleParam(veh);
    planner_.SetConfig(cfg);

    // Extend goal slightly down the exit aisle so speed stays non-zero through handoff.
    const double plan_goal_x = po.exit_x + 2.0 * std::cos(po.exit_heading);
    const double plan_goal_y = po.exit_y + 2.0 * std::sin(po.exit_heading);
    const double plan_goal_h = po.exit_heading;

    Environment env;
    env.min_x = std::min(ego.x, plan_goal_x) - 18.0;
    env.max_x = std::max(ego.x, plan_goal_x) + 18.0;
    env.min_y = std::min(ego.y, plan_goal_y) - 14.0;
    env.max_y = std::max(ego.y, plan_goal_y) + 14.0;

    for (size_t i = 0; i < context.ObstacleCount(); ++i) {
        const auto& obs = context.GetObstacle(i);
        if (std::hypot(obs.vx, obs.vy) < 0.6) {
            const double len = obs.length > 0.0 ? obs.length : 4.6;
            const double wid = obs.width > 0.0 ? obs.width : 2.0;
            bool has_h = false;
            double obs_h = static_cast<double>(context.GetObstacleHeading(i, &has_h));
            if (!has_h) obs_h = 0.0;
            env.obstacle_boxes.emplace_back(Vec2d(obs.x, obs.y), obs_h, len, wid);
        }
    }

    const double half_wb = veh.wheelbase * 0.5;
    const double start_xr = ego.x - half_wb * std::cos(ego.heading);
    const double start_yr = ego.y - half_wb * std::sin(ego.heading);
    const double goal_xr = plan_goal_x - half_wb * std::cos(plan_goal_h);
    const double goal_yr = plan_goal_y - half_wb * std::sin(plan_goal_h);

    HybridAStarResult result;
    bool success = planner_.Plan(start_xr, start_yr, ego.heading,
                                 goal_xr, goal_yr, plan_goal_h, env, &result);

    if (success && !result.path.empty()) {
        latched_trajectory_.seq = context.seq;
        latched_trajectory_.stamp_us = context.timestamp_us;
        OpenSpaceCruiseStage::DownsampleTrajectory(result.path, latched_trajectory_, veh.wheelbase);
        trajectory_latched_ = true;
        return true;
    }

    // Fallback kinematic exit curve if Hybrid A* hits grid or tight stall bounds.
    std::vector<PathPoint> fallback;
    double x = ego.x;
    double y = ego.y;
    double h = ego.heading;
    const double dt = 0.05;
    const double dh0 = NormalizeAngle(po.exit_heading - h);
    const bool reverse_out = std::fabs(dh0) > M_PI * 0.55;

    if (reverse_out) {
        // Reverse arc while counter-steering tail to line up near exit heading, then drive out forward.
        const double steer = (dh0 > 0.0) ? -0.45 : 0.45;
        const double v_rev = -1.2;
        const double yaw_rate = (v_rev / veh.wheelbase) * std::tan(steer);
        const int rev_steps = 55;
        for (int k = 0; k < rev_steps; ++k) {
            h = NormalizeAngle(h + yaw_rate * dt);
            x += (v_rev * std::cos(h) - half_wb * std::sin(h) * yaw_rate) * dt;
            y += (v_rev * std::sin(h) + half_wb * std::cos(h) * yaw_rate) * dt;
            fallback.push_back({x - half_wb * std::cos(h), y - half_wb * std::sin(h),
                                h, steer, -1, 0.0, std::tan(steer) / veh.wheelbase});
        }
    }

    const int fwd_steps = 70;
    for (int k = 1; k <= fwd_steps; ++k) {
        const double t = static_cast<double>(k) / static_cast<double>(fwd_steps);
        const double wx = x + (plan_goal_x - x) * t;
        const double wy = y + (plan_goal_y - y) * t;
        const double wh = NormalizeAngle(h + NormalizeAngle(plan_goal_h - h) * std::min(1.0, t * 1.4));
        fallback.push_back({wx - half_wb * std::cos(wh), wy - half_wb * std::sin(wh),
                            wh, 0.0, 1, 0.0, 0.0});
    }

    latched_trajectory_.seq = context.seq;
    latched_trajectory_.stamp_us = context.timestamp_us;
    OpenSpaceCruiseStage::DownsampleTrajectory(fallback, latched_trajectory_, veh.wheelbase);
    trajectory_latched_ = true;
    return true;
}

StageStatus ParkOutManeuverStage::Process(FrameContext& context, Trajectory& trajectory) {
    if (IsFinished(context)) {
        finished_ = true;
        return StageStatus::FINISHED;
    }

    if (!trajectory_latched_) {
        PlanTrajectory(context);
    }

    trajectory = latched_trajectory_;
    trajectory.seq = context.seq;
    trajectory.stamp_us = context.timestamp_us;
    return StageStatus::RUNNING;
}

// ============================================================================
// Stage 2: ParkOutCruiseStage
// ============================================================================

void ParkOutCruiseStage::Reset() {
    Stage::Reset();
    settled_frames_ = 0;
}

bool ParkOutCruiseStage::IsFinished(const FrameContext& context) const {
    if (!context.park_out.enabled) return true;
    const auto& ego = context.vehicle_state;
    const auto& po = context.park_out;
    const double dx = ego.x - po.exit_x;
    const double dy = ego.y - po.exit_y;
    const double along = dx * std::cos(po.exit_heading) + dy * std::sin(po.exit_heading);
    const double dh = std::fabs(NormalizeAngle(ego.heading - po.exit_heading));
    if (along >= 2.0 && dh <= 0.25) return true;
    return (settled_frames_ >= 3 && ego.v >= 1.2 && dh <= 0.25);
}

StageStatus ParkOutCruiseStage::Process(FrameContext& context, Trajectory& trajectory) {
    const auto& ego = context.vehicle_state;
    const auto& po = context.park_out;
    const double start_v = std::max(1.8, std::max(0.0, ego.v));
    BuildForwardCruiseLine(ego.x, ego.y, po.exit_heading,
                           start_v, 3.5,
                           context.seq, context.timestamp_us,
                           trajectory);
    if (ego.v >= 1.0 && std::fabs(NormalizeAngle(ego.heading - po.exit_heading)) <= 0.30) {
        settled_frames_++;
    }
    if (IsFinished(context)) {
        finished_ = true;
        return StageStatus::FINISHED;
    }
    return StageStatus::RUNNING;
}

// ============================================================================
// ParkOutScenario
// ============================================================================

ParkOutScenario::ParkOutScenario() {
    AddStage(&maneuver_stage_);
    AddStage(&cruise_stage_);
}

void ParkOutScenario::RearmIfAtStartSlot(const FrameContext& context) noexcept {
    if (!completed_once_ || is_active_ || !context.park_out.enabled) {
        return;
    }
    const auto& ego = context.vehicle_state;
    const auto& po = context.park_out;
    const double dist_slot = std::hypot(ego.x - po.slot_x, ego.y - po.slot_y);
    const double dz = std::fabs(ego.z - po.slot_z);
    if (dist_slot <= 1.8 && dz <= 1.0 && std::fabs(ego.v) < 0.25) {
        completed_once_ = false;
    }
}

bool ParkOutScenario::IsApplicable(const FrameContext& context) const {
    if (!context.park_out.enabled || finished_ || completed_once_) {
        return false;
    }
    const auto& ego = context.vehicle_state;
    const auto& po = context.park_out;
    if (std::fabs(ego.z - po.slot_z) > 2.5) {
        return false;
    }
    if (!is_active_) {
        const double dx = ego.x - po.exit_x;
        const double dy = ego.y - po.exit_y;
        const double along_exit = dx * std::cos(po.exit_heading) + dy * std::sin(po.exit_heading);
        const double dist_slot = std::hypot(ego.x - po.slot_x, ego.y - po.slot_y);
        if (along_exit > 2.0 || dist_slot > 28.0) {
            return false;
        }
    }
    return true;
}

void ParkOutScenario::Reset() {
    current_stage_idx_ = 0;
    finished_ = false;
    is_active_ = false;
    maneuver_stage_.Reset();
    cruise_stage_.Reset();
}

std::string_view ParkOutScenario::CurrentStageName() const noexcept {
    Stage* stage = CurrentStage();
    return stage ? stage->Name() : "UNKNOWN";
}

ScenarioStatus ParkOutScenario::Run(FrameContext& context, Trajectory& trajectory) {
    if (finished_) {
        is_active_ = false;
        completed_once_ = true;
        return ScenarioStatus::FINISHED;
    }

    is_active_ = true;
    Stage* stage = CurrentStage();
    if (!stage) {
        is_active_ = false;
        return ScenarioStatus::ERROR;
    }

    StageStatus st = stage->Process(context, trajectory);
    if (st == StageStatus::FINISHED) {
        if (current_stage_idx_ + 1 < stage_count_) {
            current_stage_idx_++;
            Stage* next_stage = CurrentStage();
            if (next_stage) {
                StageStatus nst = next_stage->Process(context, trajectory);
                if (nst == StageStatus::FINISHED && current_stage_idx_ + 1 >= stage_count_) {
                    finished_ = true;
                    is_active_ = false;
                    completed_once_ = true;
                    return ScenarioStatus::FINISHED;
                }
            }
            return ScenarioStatus::RUNNING;
        }
        finished_ = true;
        is_active_ = false;
        completed_once_ = true;
        return ScenarioStatus::FINISHED;
    }
    if (st == StageStatus::ERROR) {
        is_active_ = false;
        return ScenarioStatus::ERROR;
    }
    return ScenarioStatus::RUNNING;
}

} // namespace flowengine::planning
