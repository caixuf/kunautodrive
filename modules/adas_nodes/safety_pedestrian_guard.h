#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

namespace adas_safety {

/**
 * Evaluate minimum forward collision gap (meters) to pedestrians posing a
 * plausible conflict with ego's forward path.
 *
 * Unlike single-index proximity lookup (`ped_index`), scans all valid
 * same-elevation pedestrians so a nearby rear or sidewalk pedestrian never
 * masks an ahead in-lane pedestrian, and projects lateral velocity into ego's
 * body frame so pedestrians outside the lane corridor moving away do not lock
 * stationary traffic at intersections.
 */
template <typename StateT, int MaxObs>
inline double pedestrian_collision_gap(const StateT& state,
                                       double elev_tol_m = 2.2) {
    double best_gap = 1e9;
    const double fwd_x = std::cos(state.heading);
    const double fwd_y = std::sin(state.heading);

    for (int i = 0; i < MaxObs; ++i) {
        if (!state.obs_valid[i]) continue;
        if (std::strcmp(state.obs_type[i], "pedestrian") != 0) continue;
        if (std::fabs(state.obs_z[i] - state.z) > elev_tol_m) continue;

        const double ex = state.obs_x[i] - state.x;
        const double ey = state.obs_y[i] - state.y;
        const double along = ex * fwd_x + ey * fwd_y;
        if (along <= 0.0 || along > 70.0) continue;

        const double lat_signed = -ex * fwd_y + ey * fwd_x;
        const double lat = std::fabs(lat_signed);
        if (lat > 4.5) continue;

        const double lat_v = -state.obs_v[i] * fwd_y + state.obs_vy[i] * fwd_x;
        bool conflict = (lat <= 2.1);
        if (!conflict && (lat_signed * lat_v) < -0.05) {
            const double t_enter = (lat - 2.1) / std::fabs(lat_v);
            if (t_enter <= 2.5) conflict = true;
        }
        if (!conflict) continue;

        const double gap = along - 2.8;
        if (gap < best_gap) best_gap = gap;
    }
    return best_gap;
}

/**
 * Kinematic emergency braking activation gap (meters) for pedestrians.
 * Stops comfortably across high speeds (`~20m` at 10 m/s, `~38m` at 15 m/s)
 * without forcing full `brake=1.0` at 16m when ego is creeping/stopped behind
 * an intersection stopline (`v <= 1.5 m/s`).
 */
inline double pedestrian_stop_gap(double speed_mps) {
    const double v = std::max(0.0, speed_mps);
    return std::max(4.5, (v * v) / (2.0 * 4.0) + 0.5 * v + 2.5);
}

/**
 * Minimum distance deficit to hold behind an active crossing pedestrian.
 * At traffic speed the buffer covers the stop line + crosswalk width so the
 * ego stops before the line (observed at 4.2 m/s: old 4.5m buffer let it
 * roll to x=545, on the x≈546 crosswalk). Low-speed creep now keeps 7m:
 * the full 600s park_to_park_city run stopped with its nose at x=546.5
 * (light stop line 538.5, crosswalk ≈544.5..548) and a crossing pedestrian
 * walked into the front bumper (flowsim OBB invariant). 7m keeps the whole
 * 4.6m body behind the crosswalk even after a green-light roll-through.
 */
template <typename StateT, int MaxObs>
inline double pedestrian_crossing_hold_gap(const StateT& state,
                                           double elev_tol_m = 2.2) {
    double best_gap = 1e9;
    const double fwd_x = std::cos(state.heading);
    const double fwd_y = std::sin(state.heading);
    const double crossing_buffer_m = (state.speed >= 3.0) ? 10.0 : 7.0;

    for (int i = 0; i < MaxObs; ++i) {
        if (!state.obs_valid[i]) continue;
        if (std::strcmp(state.obs_type[i], "pedestrian") != 0) continue;
        if (std::fabs(state.obs_z[i] - state.z) > elev_tol_m) continue;

        const double ex = state.obs_x[i] - state.x;
        const double ey = state.obs_y[i] - state.y;
        const double along = ex * fwd_x + ey * fwd_y;
        /* Only hold for walkers still ahead of the ego. A walker that is
         * alongside/behind (along <= 1m) must not freeze the brakes: the
         * 600s park_to_park_city run stopped with its center at x=547.8
         * while a crossing pedestrian was at x=546.3, then sat on the
         * crosswalk with the walker against its rear quarter (OBB
         * invariant). Ignoring the behind case lets the ego clear the
         * crossing; the planner/safety still stop for walkers ahead. */
        if (along < 1.0 || along > 35.0) continue;

        const double lat_signed = -ex * fwd_y + ey * fwd_x;
        const double lat = std::fabs(lat_signed);
        const double lat_v = -state.obs_v[i] * fwd_y + state.obs_vy[i] * fwd_x;

        bool crossing_active = (lat < 2.2);
        if (!crossing_active && lat < 6.0 && (lat_signed * lat_v) < -0.05) {
            const double t_enter = (lat - 2.2) / std::fabs(lat_v);
            if (t_enter <= 3.5) crossing_active = true;
        }
        if (!crossing_active) continue;

        const double gap = along - crossing_buffer_m;
        if (gap < best_gap) best_gap = gap;
    }
    return best_gap;
}

/**
 * Activation window (meters past `kCrossingBufferM`) for crossing-line hold.
 * Allows low-speed creep up to the stopline (~6m before crosswalk) without
 * locking brakes 16m back.
 */
inline double pedestrian_hold_activate_gap(double speed_mps) {
    /* Braking-distance based, not speed*1.2. At 14 m/s the old 16.8m
     * window engaged long after the 34m stopping distance: the ego
     * entered the x≈546 crosswalk and stopped at x=548 on top of it
     * (the original incident frame). Low speed keeps a small window so
     * creeping traffic still stops for a pedestrian at the line. */
    return std::max(2.0, pedestrian_stop_gap(speed_mps) - 0.5);
}

}  // namespace adas_safety
