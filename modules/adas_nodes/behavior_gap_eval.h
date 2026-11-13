#pragma once

#include <cmath>
#include <cstddef>

namespace behavior_gap {

struct GapEvalConfig {
    double ego_x{0.0};
    double ego_y{0.0};
    double ego_v{0.0};
    double ego_heading{0.0};
    double ego_lane_d{0.0};
    double target_speed{0.0};
    double min_gap{25.0};
    double lc_gap_mult{1.5};
    double rear_safe_min_m{15.0};
    double rear_safe_time_s{2.0};
    double lat_tol{2.0};
    double front_ttc_min_s{3.0};
};

struct LaneGapResult {
    bool same_side{false};
    bool rear_safe{false};
    bool front_ttc_safe{true};
    bool ok{false};
    double front_gap{1e9};
    double front_speed{0.0};
};

/**
 * Evaluate gap acceptance on an adjacent target lane (center d = target_lane_d).
 *
 * ProjectFn signature: bool(double x, double y, double& out_d)
 * Returns true if (x, y) is near the road and sets out_d in Frenet lateral coords.
 */
template <typename ProjectFn>
inline LaneGapResult evaluate_target_lane_gap(
    const GapEvalConfig& cfg,
    double target_lane_d,
    bool same_side,
    bool has_ego_projection,
    const double* obs_x,
    const double* obs_y,
    const double* obs_vx,
    const double* obs_vy,
    int obs_count,
    ProjectFn&& project_d)
{
    LaneGapResult res;
    res.same_side = same_side;
    res.front_speed = cfg.target_speed;

    if (!same_side || !has_ego_projection) {
        return res;
    }

    const double fwd_x = std::cos(cfg.ego_heading);
    const double fwd_y = std::sin(cfg.ego_heading);

    res.rear_safe = true;
    for (int i = 0; i < obs_count; i++) {
        const double rx = obs_x[i] - cfg.ego_x;
        const double ry = obs_y[i] - cfg.ego_y;
        const double along = rx * fwd_x + ry * fwd_y;

        double obs_d = 0.0;
        if (!(along > 0.0 && project_d(obs_x[i], obs_y[i], obs_d))) {
            const double lat_left = -rx * fwd_y + ry * fwd_x;
            obs_d = cfg.ego_lane_d + lat_left;
        }
        if (std::fabs(obs_d - target_lane_d) > cfg.lat_tol) {
            continue;
        }

        const double obs_along_v = obs_vx[i] * fwd_x + obs_vy[i] * fwd_y;
        if (along > 0.0) {
            if (along < res.front_gap) {
                res.front_gap = along;
                res.front_speed = obs_along_v;
            }
        } else {
            const double rd = -along;
            const double rrs = obs_along_v - cfg.ego_v;
            const double rear_min_gap = std::fmax(cfg.min_gap, cfg.rear_safe_min_m);
            const double min_rd = (rrs > 0.0)
                ? std::fmax(rear_min_gap, rrs * cfg.rear_safe_time_s)
                : rear_min_gap;
            if (rd < min_rd) {
                res.rear_safe = false;
            }
        }
    }

    if (res.front_gap < 1e8 && cfg.ego_v > res.front_speed + 0.5) {
        const double ttc = res.front_gap / (cfg.ego_v - res.front_speed);
        if (ttc < cfg.front_ttc_min_s) {
            res.front_ttc_safe = false;
        }
    }

    res.ok = res.same_side && res.rear_safe && res.front_ttc_safe &&
             (res.front_gap > cfg.min_gap * cfg.lc_gap_mult);
    return res;
}

}  // namespace behavior_gap

