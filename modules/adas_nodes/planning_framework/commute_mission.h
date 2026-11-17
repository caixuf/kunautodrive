#pragma once

#include "planning_context.h"

#include <cmath>

namespace flowengine::planning {

/**
 * 通勤任务：去程泊入公司车位，停留后泊出，沿 route 的返程方向回家，再泊入出发车位。
 * 返程几何不在这里生成。车头调到 route 的反方向后，flowsim 既有的
 * ref_path.reverse 会沿同一条 route 往回采样。
 */
enum class CommutePhase {
    Outbound = 0,
    Dwell = 1,
    Return = 2,
    Home = 3,
};

struct CommuteMission {
    bool enabled{false};
    CommutePhase phase{CommutePhase::Outbound};
    double dwell_s{6.0};
    double dwell_start_s{-1.0};
    bool latched{false};
    bool pulse_reset{false};
    ParkingGoal office{};
    ParkingGoal home{};
    ParkOutGoal depart{};
    ParkOutGoal return_depart{};

    void observe_scenario(bool commute_flag,
                          const ParkingGoal& goal,
                          const ParkOutGoal& park_out) {
        enabled = commute_flag;
        if (latched || !commute_flag || !goal.enabled || !park_out.enabled) return;
        office = goal;
        depart = park_out;
        home.enabled = true;
        home.x = park_out.slot_x;
        home.y = park_out.slot_y;
        home.z = park_out.slot_z;
        home.heading = park_out.slot_heading;
        home.width = 2.6;
        home.length = 5.6;
        latched = true;
    }

    /** 覆盖本帧的泊车目标。返回 true 表示 goal / park_out / slot_count 已改写。 */
    bool apply(double now_s,
               double ego_x, double ego_y, double ego_z, double ego_v,
               bool valet_finished,
               double exit_x, double exit_y, double exit_z, double exit_h,
               ParkingGoal& goal, ParkOutGoal& park_out, size_t& slot_count) {
        pulse_reset = false;
        if (!enabled || !latched) return false;

        const double dist_office = std::hypot(ego_x - office.x, ego_y - office.y);
        const double dz_office = std::fabs(ego_z - office.z);
        if (phase == CommutePhase::Outbound) {
            if (valet_finished && ego_v < 0.35 && dist_office < 2.2 && dz_office < 1.2) {
                phase = CommutePhase::Dwell;
                dwell_start_s = now_s;
            }
            return false;
        }
        if (phase == CommutePhase::Dwell) {
            if (dwell_start_s < 0.0 || now_s - dwell_start_s < dwell_s) return false;
            phase = CommutePhase::Return;
            return_depart.enabled = true;
            return_depart.slot_x = office.x;
            return_depart.slot_y = office.y;
            return_depart.slot_z = office.z;
            return_depart.slot_heading = office.heading;
            return_depart.exit_x = exit_x;
            return_depart.exit_y = exit_y;
            return_depart.exit_z = exit_z;
            return_depart.exit_heading = exit_h;
            return_depart.corridor_width =
                depart.corridor_width > 0.0 ? depart.corridor_width : 6.0;
            pulse_reset = true;
        }

        const double dist_home = std::hypot(ego_x - home.x, ego_y - home.y);
        const double dz_home = std::fabs(ego_z - home.z);
        if (phase == CommutePhase::Return && valet_finished && ego_v < 0.35 &&
            dist_home < 2.2 && dz_home < 1.2) {
            phase = CommutePhase::Home;
        }
        goal = home;
        park_out = return_depart;
        slot_count = 0;
        return true;
    }
};

}  // namespace flowengine::planning
