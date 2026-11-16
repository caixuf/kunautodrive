#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include "perception_fusion_core.h"

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
    double ego_z{0.0};
    const double* obs_z{nullptr};
    double layer_tol_m{kSameElevationLayerTolM};
    double road_half_w{0.0};
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
        if (cfg.obs_z && !pf_same_elevation_layer(cfg.obs_z[i], cfg.ego_z, cfg.layer_tol_m)) {
            continue;
        }
        const double rx = obs_x[i] - cfg.ego_x;
        const double ry = obs_y[i] - cfg.ego_y;
        const double along = rx * fwd_x + ry * fwd_y;

        double obs_d = 0.0;
        if (!(along > 0.0 && project_d(obs_x[i], obs_y[i], obs_d))) {
            const double lat_left = -rx * fwd_y + ry * fwd_x;
            obs_d = cfg.ego_lane_d + lat_left;
        }
        if (cfg.road_half_w > 0.0 && std::fabs(obs_d) >= cfg.road_half_w - 0.05) {
            continue;
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

/// Returns true when a forward-lane candidate is genuinely ahead along the
/// route. Within the near curve window (<12m), also requires positive physical
/// forward projection in ego's frame to reject behind-ego inner-corner hits.
inline bool is_valid_forward_lead(double along_s, double phys_along) {
    if (along_s < 0.0) {
        return false;
    }
    if (along_s < 12.0 && phys_along <= 0.0) {
        return false;
    }
    return true;
}

struct CorridorConfig {
    double ego_x{0.0};
    double ego_y{0.0};
    double ego_z{0.0};
    double ego_heading{0.0};
    double road_d_min{-3.5};
    double road_d_max{0.0};
    double ego_width{2.0};
    double lateral_margin{0.35};
    double s_lo{0.0};
    double s_hi{35.0};
    double target_d{-1.75};
    double default_obs_half_w{0.95};
    const double* obs_z{nullptr};
    const double* obs_half_w{nullptr};
    double layer_tol_m{kSameElevationLayerTolM};
};

struct CorridorResult {
    bool passable{false};
    bool target_passable{false};
    double best_lo{0.0};
    double best_hi{0.0};
    double best_width{0.0};
    double best_center{0.0};
    double target_width{0.0};
    double target_center{0.0};
};

/**
 * Evaluate continuous lateral free-space corridor on [road_d_min, road_d_max].
 * Subtracts every obstacle's lateral envelope within [s_lo, s_hi] and checks
 * whether the remaining clear width fits (ego_width + lateral_margin).
 */
template <typename ProjectFn>
inline CorridorResult evaluate_lateral_corridor(
    const CorridorConfig& cfg,
    const double* obs_x,
    const double* obs_y,
    int obs_count,
    ProjectFn&& project_sd)
{
    CorridorResult out;
    if (cfg.road_d_max <= cfg.road_d_min) {
        return out;
    }

    struct Interval { double lo; double hi; };
    static constexpr int kMaxIntervals = 64;
    std::array<Interval, kMaxIntervals> intervals{};
    int n_intervals = 1;
    intervals[0] = {cfg.road_d_min, cfg.road_d_max};

    for (int i = 0; i < obs_count; i++) {
        if (cfg.obs_z && !pf_same_elevation_layer(cfg.obs_z[i], cfg.ego_z, cfg.layer_tol_m)) {
            continue;
        }
        double obs_s = 0.0;
        double obs_d = 0.0;
        if (!project_sd(obs_x[i], obs_y[i], obs_s, obs_d)) {
            continue;
        }
        if (obs_s < cfg.s_lo || obs_s > cfg.s_hi) {
            continue;
        }
        const double hw = cfg.obs_half_w ? cfg.obs_half_w[i] : cfg.default_obs_half_w;
        const double cut_lo = obs_d - hw;
        const double cut_hi = obs_d + hw;
        if (cut_hi <= cfg.road_d_min || cut_lo >= cfg.road_d_max) {
            continue;
        }

        std::array<Interval, kMaxIntervals> next{};
        int n_next = 0;
        for (int k = 0; k < n_intervals; k++) {
            const Interval iv = intervals[k];
            if (cut_hi <= iv.lo || cut_lo >= iv.hi) {
                if (n_next < kMaxIntervals) next[n_next++] = iv;
                continue;
            }
            if (cut_lo > iv.lo && n_next < kMaxIntervals) {
                next[n_next++] = {iv.lo, cut_lo};
            }
            if (cut_hi < iv.hi && n_next < kMaxIntervals) {
                next[n_next++] = {cut_hi, iv.hi};
            }
        }
        intervals = next;
        n_intervals = n_next;
        if (n_intervals == 0) break;
    }

    const double req_w = cfg.ego_width + cfg.lateral_margin;
    for (int k = 0; k < n_intervals; k++) {
        const double w = intervals[k].hi - intervals[k].lo;
        if (w > out.best_width) {
            out.best_width = w;
            out.best_lo = intervals[k].lo;
            out.best_hi = intervals[k].hi;
            out.best_center = 0.5 * (intervals[k].lo + intervals[k].hi);
        }
        if (cfg.target_d >= intervals[k].lo && cfg.target_d <= intervals[k].hi) {
            if (w > out.target_width) {
                out.target_width = w;
                out.target_center = 0.5 * (intervals[k].lo + intervals[k].hi);
            }
        }
    }
    out.passable = (out.best_width >= req_w);
    out.target_passable = (out.target_width >= req_w);
    return out;
}

}  // namespace behavior_gap

