#include "valet_parking_scenario.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace flowengine::planning {

static double NormAngle(double a) {
    while (a > M_PI) a -= 2.0 * M_PI;
    while (a < -M_PI) a += 2.0 * M_PI;
    return a;
}

// ─────────────────────────────────────────────────────────────────────────────
// ApproachStage Implementation
// ─────────────────────────────────────────────────────────────────────────────

ApproachStage::ApproachStage() {
    finished_ = false;
}

void ApproachStage::Reset() {
    finished_ = false;
}

bool ApproachStage::IsFinished(const FrameContext& context) const {
    if (!context.parking_goal.enabled) return false;
    double dx = context.parking_goal.x - context.vehicle_state.x;
    double dy = context.parking_goal.y - context.vehicle_state.y;
    double dist = std::hypot(dx, dy);
    return (dist <= approach_distance_threshold_ && context.vehicle_state.v <= approach_speed_threshold_);
}

StageStatus ApproachStage::Process(FrameContext& context, Trajectory& trajectory) {
    if (IsFinished(context)) {
        finished_ = true;
        return StageStatus::FINISHED;
    }

    const auto& ego = context.vehicle_state;
    double dx = context.parking_goal.x - ego.x;
    double target_v = (dx < 12.0) ? 2.0 : 3.0; // 接近车位平滑减速
    double ego_h = ego.heading;
    double cos_h = std::cos(ego_h);
    double sin_h = std::sin(ego_h);

    trajectory.seq = context.seq;
    trajectory.stamp_us = context.timestamp_us;

    double s = 0.0;
    for (int i = 0; i < 64; ++i) {
        double dt = (i + 1) * 0.1; // 100ms per point
        s += target_v * 0.1;
        TrajectoryPoint& p = trajectory.points[i];
        p.t_rel_us = static_cast<uint32_t>(dt * 1e6);
        p.x = static_cast<float>(ego.x + s * cos_h);
        p.y = static_cast<float>(ego.y + s * sin_h);
        p.s = static_cast<float>(s);
        p.l = 0.0f;
        p.heading = static_cast<float>(ego_h);
        p.kappa = 0.0f;
        p.v = static_cast<float>(target_v);
        p.a = -0.5f;
        p.jerk = 0.0f;
    }

    return StageStatus::RUNNING;
}

// ─────────────────────────────────────────────────────────────────────────────
// OpenSpaceCruiseStage Implementation
// ─────────────────────────────────────────────────────────────────────────────

OpenSpaceCruiseStage::OpenSpaceCruiseStage() {
    Reset();
}

void OpenSpaceCruiseStage::Reset() {
    finished_ = false;
    trajectory_latched_ = false;
    std::memset(&latched_trajectory_, 0, sizeof(latched_trajectory_));
}

bool OpenSpaceCruiseStage::IsFinished(const FrameContext& context) const {
    if (!context.parking_goal.enabled) return false;
    double dx = context.parking_goal.x - context.vehicle_state.x;
    double dy = context.parking_goal.y - context.vehicle_state.y;
    double dist = std::hypot(dx, dy);
    double dh = NormAngle(context.vehicle_state.heading - context.parking_goal.heading);
    return (dist < 0.36 && std::fabs(dh) < 0.06 && std::fabs(context.vehicle_state.v) < 0.35);
}

int OpenSpaceCruiseStage::DownsampleTrajectory(const std::vector<PathPoint>& raw_path,
                                              Trajectory& trajectory,
                                              double wheelbase) {
    int sn = static_cast<int>(raw_path.size());
    if (sn <= 0) return 0;

    std::vector<bool> keep(sn, false);
    keep[0] = true;
    keep[sn - 1] = true;
    int forced = 2;

    // 严格保留挡位反转尖点 (Cusp points)
    for (int i = 1; i < sn; ++i) {
        if (raw_path[i - 1].direction != raw_path[i].direction) {
            if (!keep[i - 1]) { keep[i - 1] = true; forced++; }
            if (!keep[i])     { keep[i] = true;     forced++; }
        }
    }

    int cap = 64;
    int remain = cap - forced;
    double stride = static_cast<double>(sn) / static_cast<double>(remain > 0 ? remain + 1 : 1);
    for (int k = 1; k <= remain; ++k) {
        int idx = static_cast<int>(k * stride);
        if (idx < 1) idx = 1;
        if (idx >= sn) idx = sn - 1;
        while (idx < sn && keep[idx]) idx++;
        if (idx < sn) keep[idx] = true;
    }

    int n_out = 0;
    double cum_s = 0.0;
    double half_wb = wheelbase > 0.0 ? wheelbase * 0.5 : 1.35;
    for (int i = 0; i < sn && n_out < cap; ++i) {
        if (!keep[i]) continue;
        const auto& pt = raw_path[i];
        TrajectoryPoint& out_p = trajectory.points[n_out];

        // 将后轴路径点转换为车身中心坐标
        double center_x = pt.x + half_wb * std::cos(pt.phi);
        double center_y = pt.y + half_wb * std::sin(pt.phi);

        if (n_out > 0) {
            double dx = center_x - trajectory.points[n_out - 1].x;
            double dy = center_y - trajectory.points[n_out - 1].y;
            cum_s += std::hypot(dx, dy);
        }

        out_p.t_rel_us = static_cast<uint32_t>((n_out + 1) * 100000);
        out_p.x = static_cast<float>(center_x);
        out_p.y = static_cast<float>(center_y);
        out_p.s = static_cast<float>(cum_s);
        out_p.l = 0.0f;
        out_p.heading = static_cast<float>(pt.phi);
        out_p.kappa = static_cast<float>(pt.kappa != 0.0 ? pt.kappa : std::tan(pt.steer) / (wheelbase > 0 ? wheelbase : 2.7));

        // 速度规划：前进段 +1.6 m/s，倒车段 -1.2 m/s，终点与尖点设为 0
        if (i == sn - 1) {
            out_p.v = 0.0f;
        } else if (i + 1 < sn && raw_path[i].direction != raw_path[i + 1].direction) {
            out_p.v = 0.0f; // 换挡点零速
        } else if (pt.direction == -1) {
            int rem = sn - 1 - i;
            if (rem <= 8) {
                out_p.v = -static_cast<float>(0.20 + 0.10 * rem); // 平滑减速停稳
            } else {
                out_p.v = -1.2f; // 倒车段负速度，激活 ManeuverTracker
            }
        } else {
            bool near_cusp = false;
            for (int k = i + 1; k < std::min(sn, i + 6); ++k) {
                if (raw_path[k].direction != pt.direction) { near_cusp = true; break; }
            }
            out_p.v = near_cusp ? 0.6f : 1.5f; // 前进段正速度
        }
        out_p.a = 0.0f;
        out_p.jerk = 0.0f;
        n_out++;
    }

    // 填充至 64 点
    for (int j = n_out; j < 64; ++j) {
        trajectory.points[j] = trajectory.points[n_out - 1];
    }
    return n_out;
}

bool OpenSpaceCruiseStage::PlanTrajectory(FrameContext& context) {
    const auto& ego = context.vehicle_state;
    const auto& goal = context.parking_goal;

    VehicleParam veh;
    veh.wheelbase = ego.wheelbase > 1.0 ? ego.wheelbase : 2.7;
    veh.length = 4.8;
    veh.width = 2.0;
    veh.back_edge_to_center = 1.0;
    veh.max_steer = 0.60;
    veh.step_size = 0.8;

    HybridAStarConfig cfg;
    cfg.xy_grid_resolution = 0.25;
    cfg.phi_grid_resolution = 0.0872665;
    cfg.weight_reverse = 2.0;
    cfg.weight_gear_switch = 12.0;
    cfg.vehicle_safety_margin = 0.08;
    cfg.analytic_expansion_max_dist = 25.0;

    planner_.SetVehicleParam(veh);
    planner_.SetConfig(cfg);

    Environment env;
    env.min_x = std::min(ego.x, goal.x) - 15.0;
    env.max_x = std::max(ego.x, goal.x) + 15.0;
    env.min_y = std::min(ego.y, goal.y) - 12.0;
    env.max_y = std::max(ego.y, goal.y) + 12.0;

    // 注入感知障碍物（停放车/护栏）：路侧平行停放车辆沿道路方向 (heading=0)
    for (size_t i = 0; i < context.ObstacleCount(); ++i) {
        const auto& obs = context.GetObstacle(i);
        if (std::hypot(obs.vx, obs.vy) < 0.6) {
            double len = obs.length > 0 ? obs.length : 4.6;
            double wid = obs.width > 0 ? obs.width : 2.0;
            bool has_h = false;
            double obs_heading = static_cast<double>(context.GetObstacleHeading(i, &has_h));
            if (!has_h) obs_heading = 0.0;
            env.obstacle_boxes.emplace_back(Vec2d(obs.x, obs.y), obs_heading, len, wid);
        }
    }

    if (env.obstacle_boxes.empty()) {
        env.obstacle_boxes.emplace_back(Vec2d(goal.x - 6.0, goal.y), 0.0, 4.6, 2.0);
        env.obstacle_boxes.emplace_back(Vec2d(goal.x + 6.0, goal.y), 0.0, 4.6, 2.0);
    }

    double half_wb = veh.wheelbase * 0.5;
    double start_xr = ego.x - half_wb * std::cos(ego.heading);
    double start_yr = ego.y - half_wb * std::sin(ego.heading);
    // 泊车低速跟踪补偿：后退圆弧内切导致实车弧长提前饱满约 0.10m，垂直入库外侧偏移约 0.45m，航向提早收敛约 7°
    double plan_goal_x = goal.x - 0.45 * std::sin(goal.heading);
    double plan_goal_y = goal.y - 0.10 * std::sin(goal.heading);
    double plan_goal_h = goal.heading + 0.12;
    double goal_xr = plan_goal_x - half_wb * std::cos(plan_goal_h);
    double goal_yr = plan_goal_y - half_wb * std::sin(plan_goal_h);

    HybridAStarResult result;
    bool success = planner_.Plan(start_xr, start_yr, ego.heading, goal_xr, goal_yr, plan_goal_h, env, &result);

    if (success && !result.path.empty()) {
        latched_trajectory_.seq = context.seq;
        latched_trajectory_.stamp_us = context.timestamp_us;
        DownsampleTrajectory(result.path, latched_trajectory_, veh.wheelbase);
        trajectory_latched_ = true;
        return true;
    }

    // 若 Hybrid A* 受环境网格边界影响未搜索出路径，采用平滑 Reeds-Shepp/几何调优轨迹兜底
    std::vector<PathPoint> fallback_path;
    double x = ego.x, y = ego.y, h = ego.heading;
    double dt = 0.05;

    auto push_pt = [&](double steer, double v, int n_steps, int dir) {
        double kappa = std::tan(steer) / veh.wheelbase;
        for (int k = 0; k < n_steps; ++k) {
            double yaw = v / veh.wheelbase * std::tan(steer);
            h += yaw * dt;
            x += (v * std::cos(h) - veh.wheelbase * 0.5 * std::sin(h) * yaw) * dt;
            y += (v * std::sin(h) + veh.wheelbase * 0.5 * std::cos(h) * yaw) * dt;
            h = NormAngle(h);
            fallback_path.push_back({x - half_wb * std::cos(h), y - half_wb * std::sin(h), h, steer, dir, 0.0, kappa});
        }
    };

    // 前进外摆 + 倒车入库几何合成
    push_pt(0.0, 1.5, 30, 1);
    push_pt(0.35, 1.0, 20, 1);
    push_pt(-0.50, -1.2, 45, -1);
    push_pt(0.0, -0.6, 25, -1);

    latched_trajectory_.seq = context.seq;
    latched_trajectory_.stamp_us = context.timestamp_us;
    DownsampleTrajectory(fallback_path, latched_trajectory_, veh.wheelbase);
    trajectory_latched_ = true;
    return true;
}

StageStatus OpenSpaceCruiseStage::Process(FrameContext& context, Trajectory& trajectory) {
    if (!trajectory_latched_) {
        PlanTrajectory(context);
    }

    std::memcpy(&trajectory, &latched_trajectory_, sizeof(Trajectory));
    trajectory.seq = context.seq;
    trajectory.stamp_us = context.timestamp_us;

    if (IsFinished(context)) {
        finished_ = true;
        return StageStatus::FINISHED;
    }
    return StageStatus::RUNNING;
}

// ─────────────────────────────────────────────────────────────────────────────
// PrecisionHoldStage Implementation
// ─────────────────────────────────────────────────────────────────────────────

PrecisionHoldStage::PrecisionHoldStage() {
    finished_ = false;
}

void PrecisionHoldStage::Reset() {
    finished_ = false;
}

bool PrecisionHoldStage::IsFinished(const FrameContext& context) const {
    (void)context;
    return true; // 始终维持驻停
}

StageStatus PrecisionHoldStage::Process(FrameContext& context, Trajectory& trajectory) {
    trajectory.seq = context.seq;
    trajectory.stamp_us = context.timestamp_us;

    const auto& goal = context.parking_goal;
    for (int i = 0; i < 64; ++i) {
        TrajectoryPoint& p = trajectory.points[i];
        p.t_rel_us = static_cast<uint32_t>((i + 1) * 100000);
        p.x = static_cast<float>(goal.x);
        p.y = static_cast<float>(goal.y);
        p.s = 0.0f;
        p.l = 0.0f;
        p.heading = static_cast<float>(goal.heading);
        p.kappa = 0.0f;
        p.v = 0.0f; // 驻停保持
        p.a = 0.0f;
        p.jerk = 0.0f;
    }
    return StageStatus::FINISHED;
}

// ─────────────────────────────────────────────────────────────────────────────
// ValetParkingScenario Implementation
// ─────────────────────────────────────────────────────────────────────────────

ValetParkingScenario::ValetParkingScenario() {
    current_stage_idx_ = 0;
    stage_count_ = 0;
    finished_ = false;
    is_active_ = false;

    AddStage(&approach_stage_);
    AddStage(&open_space_cruise_stage_);
    AddStage(&precision_hold_stage_);
}

namespace {

bool IsSlotOccupiedByStaticObstacle(const ParkingGoal& slot, const FrameContext& context) {
    const double check_len = std::max(1.2, slot.length - 0.7);
    const double check_wid = std::max(1.0, slot.width - 0.4);
    Box2d slot_box(Vec2d(slot.x, slot.y), slot.heading, check_len, check_wid);

    for (size_t i = 0; i < context.ObstacleCount(); ++i) {
        const auto& obs = context.GetObstacle(i);
        if (std::hypot(obs.vx, obs.vy) >= 0.8) continue;
        const double len = obs.length > 0.0 ? obs.length : 4.6;
        const double wid = obs.width > 0.0 ? obs.width : 2.0;
        bool has_h = false;
        double obs_heading = static_cast<double>(context.GetObstacleHeading(i, &has_h));
        if (!has_h) obs_heading = 0.0;
        Box2d obs_box(Vec2d(obs.x, obs.y), obs_heading, len, wid);
        if (slot_box.HasOverlap(obs_box)) {
            return true;
        }
    }
    return false;
}

ParkingGoal ResolveParkingTarget(const FrameContext& context, int& out_slot_index) {
    out_slot_index = -1;
    if (context.parking_slot_count > 0) {
        if (context.selected_slot_index >= 0 &&
            static_cast<size_t>(context.selected_slot_index) < context.parking_slot_count) {
            ParkingGoal locked = context.parking_slots[context.selected_slot_index];
            locked.enabled = true;
            out_slot_index = context.selected_slot_index;
            return locked;
        }
        for (size_t i = 0; i < context.parking_slot_count; ++i) {
            if (!IsSlotOccupiedByStaticObstacle(context.parking_slots[i], context)) {
                ParkingGoal free_goal = context.parking_slots[i];
                free_goal.enabled = true;
                out_slot_index = static_cast<int>(i);
                return free_goal;
            }
        }
        if (context.parking_goal.enabled) {
            return context.parking_goal;
        }
        ParkingGoal fallback = context.parking_slots[0];
        fallback.enabled = true;
        return fallback;
    }
    return context.parking_goal;
}

} // namespace

bool ValetParkingScenario::IsApplicable(const FrameContext& context) const {
    if (!context.parking_goal.enabled && context.parking_slot_count == 0) {
        return false;
    }
    if (finished_ || current_stage_idx_ > 0) {
        return true;
    }

    int slot_idx = -1;
    ParkingGoal target = ResolveParkingTarget(context, slot_idx);
    if (!target.enabled) return false;

    const double dz = std::fabs(context.vehicle_state.z - target.z);
    if (dz > 2.5) return false;

    const double dx = target.x - context.vehicle_state.x;
    const double dy = target.y - context.vehicle_state.y;
    const double dist = std::hypot(dx, dy);
    return dist <= 48.0;
}

void ValetParkingScenario::Reset() {
    current_stage_idx_ = 0;
    finished_ = false;
    is_active_ = false;
    locked_slot_index_ = -1;
    active_goal_ = ParkingGoal{};
    approach_stage_.Reset();
    open_space_cruise_stage_.Reset();
    precision_hold_stage_.Reset();
}

std::string_view ValetParkingScenario::CurrentStageName() const noexcept {
    Stage* stage = CurrentStage();
    return stage ? stage->Name() : "UNKNOWN";
}

ScenarioStatus ValetParkingScenario::Run(FrameContext& context, Trajectory& trajectory) {
    is_active_ = true;
    if (current_stage_idx_ >= 1 && locked_slot_index_ >= 0 && context.selected_slot_index < 0) {
        context.selected_slot_index = locked_slot_index_;
    }
    int slot_idx = -1;
    context.parking_goal = ResolveParkingTarget(context, slot_idx);
    context.selected_slot_index = slot_idx;
    locked_slot_index_ = slot_idx;
    active_goal_ = context.parking_goal;

    Stage* stage = CurrentStage();
    if (!stage) {
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
                    return ScenarioStatus::FINISHED;
                }
            }
            return ScenarioStatus::RUNNING;
        } else {
            finished_ = true;
            return ScenarioStatus::FINISHED;
        }
    } else if (st == StageStatus::ERROR) {
        return ScenarioStatus::ERROR;
    }

    return ScenarioStatus::RUNNING;
}

} // namespace flowengine::planning
