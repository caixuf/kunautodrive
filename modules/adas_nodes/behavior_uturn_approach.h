#pragma once

#include "construction_zones.h"

#include <cmath>
#include <cstdio>

namespace behavior_uturn {

struct UturnApproachInput {
    double ego_x{0.0};
    double ego_y{0.0};
    double ego_v{0.0};
    double road_end_x{1e9};
    double ref_path_end_x{1e9};
    double uturn_approach_dist_m{120.0};
    double uturn_max_trigger_speed{5.0};
    double cfg_cruise_speed{15.0};
    double uturn_cooldown{0.0};
    int    on_return{0};
    bool   at_inner_lane{false};

    const double* obs_x{nullptr};
    const double* obs_vx{nullptr};
    const double* obs_vy{nullptr};
    int           obs_count{0};

    const double* raw_obs_x{nullptr};
    const double* raw_obs_vx{nullptr};
    const double* raw_obs_vy{nullptr};
    int           raw_obs_count{0};

    const double* cz_x{nullptr};
    const double* cz_y{nullptr};
    const double* cz_len{nullptr};
    const double* cz_wid{nullptr};
    int           cz_count{0};
};

struct UturnApproachDecision {
    bool   trigger{false};
    bool   approach_active{false};
    double natural_target_speed{0.0};
    bool   override_lane_and_speed{false};
    int    target_lane{-1};
    double target_speed{0.0};
    char   reason[128]{};
};

inline double compute_natural_decel_target(double dist_ref,
                                           double approach_dist_m,
                                           double max_trigger_speed,
                                           double cfg_cruise_speed) {
    constexpr double kDecelMps2 = 1.0;
    double v_env = max_trigger_speed;
    if (dist_ref > approach_dist_m) {
        v_env = std::sqrt(2.0 * kDecelMps2 * (dist_ref - approach_dist_m) +
                          max_trigger_speed * max_trigger_speed);
    }
    return (v_env > cfg_cruise_speed) ? cfg_cruise_speed : v_env;
}

inline UturnApproachDecision evaluate_uturn_approach(const UturnApproachInput& in) {
    UturnApproachDecision out;

    const double approach3 = in.uturn_approach_dist_m * 3.0;
    const bool at_road_end = (in.road_end_x < 1e8 && in.ego_x > in.road_end_x - approach3);
    double uturn_ref_x = in.road_end_x;

    if (at_road_end) {
        if (in.obs_x && in.obs_vx && in.obs_vy) {
            for (int i = 0; i < in.obs_count; ++i) {
                if (std::fabs(in.obs_vx[i]) < 0.5 &&
                    std::fabs(in.obs_vy[i]) < 0.5 &&
                    in.obs_x[i] > in.ego_x && in.obs_x[i] < uturn_ref_x) {
                    uturn_ref_x = in.obs_x[i];
                }
            }
        }
        if (in.raw_obs_x && in.raw_obs_vx && in.raw_obs_vy) {
            for (int i = 0; i < in.raw_obs_count; ++i) {
                if (std::fabs(in.raw_obs_vx[i]) < 0.5 &&
                    std::fabs(in.raw_obs_vy[i]) < 0.5 &&
                    in.raw_obs_x[i] > in.ego_x && in.raw_obs_x[i] < uturn_ref_x) {
                    uturn_ref_x = in.raw_obs_x[i];
                }
            }
        }
        if (in.cz_x && in.cz_y && in.cz_len && in.cz_wid) {
            for (int i = 0; i < in.cz_count; ++i) {
                const double front = construction_zone_front_x(in.cz_x[i], in.cz_len[i]);
                if (front <= in.ego_x || front >= uturn_ref_x) continue;
                const double half_w = 0.5 * in.cz_wid[i] + 1.0;
                if (std::fabs(in.ego_y - in.cz_y[i]) > half_w) continue;
                uturn_ref_x = front;
            }
        }
    }

    out.natural_target_speed = compute_natural_decel_target(
        uturn_ref_x - in.ego_x,
        in.uturn_approach_dist_m,
        in.uturn_max_trigger_speed,
        in.cfg_cruise_speed);

    const bool uturn_zone =
        (in.on_return) ? (in.ego_x < approach3)
                       : (at_road_end && uturn_ref_x < 1e8 &&
                          in.ego_x > uturn_ref_x - approach3);
    if (uturn_zone) {
        out.approach_active = true;
    }

    if (!in.on_return && uturn_zone && in.at_inner_lane && in.uturn_cooldown <= 0.0) {
        if (in.ego_x > uturn_ref_x - 30.0 && in.ego_v <= 5.0) {
            out.trigger = true;
        } else if (in.ego_v > in.uturn_max_trigger_speed) {
            out.override_lane_and_speed = true;
            out.target_lane = -1;
            out.target_speed = out.natural_target_speed;
            out.approach_active = true;
            std::snprintf(out.reason, sizeof(out.reason),
                          "uturn approach: decelerate %.1f→%.1f m/s before trigger (x=%.1f end=%.1f)",
                          in.ego_v, in.uturn_max_trigger_speed, in.ego_x, in.ref_path_end_x);
        } else if (in.ego_x > uturn_ref_x - in.uturn_approach_dist_m) {
            out.trigger = true;
        } else {
            out.approach_active = true;
        }
    } else if (in.on_return && uturn_zone && in.at_inner_lane && in.uturn_cooldown <= 0.0) {
        out.natural_target_speed = compute_natural_decel_target(
            in.ego_x,
            in.uturn_approach_dist_m,
            in.uturn_max_trigger_speed,
            in.cfg_cruise_speed);
        if (in.ego_x < 30.0 && in.ego_v <= 7.0) {
            out.trigger = true;
        } else if (in.ego_v > in.uturn_max_trigger_speed) {
            out.override_lane_and_speed = true;
            out.target_lane = -1;
            out.target_speed = out.natural_target_speed;
            out.approach_active = true;
            std::snprintf(out.reason, sizeof(out.reason),
                          "uturn approach(return): decelerate %.1f→%.1f m/s",
                          in.ego_v, in.uturn_max_trigger_speed);
        } else if (in.ego_x < in.uturn_approach_dist_m) {
            out.trigger = true;
        } else {
            out.approach_active = true;
        }
    }

    return out;
}

}  // namespace behavior_uturn
