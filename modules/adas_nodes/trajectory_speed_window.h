#pragma once

#include <cmath>
#include <cstdint>

namespace adas_control {

/**
 * Compute cruise target speed within the physical 1.5s longitudinal preview window.
 *
 * Some trajectory generators populate fixed `0.1s * i` relative timestamps
 * regardless of waypoint spacing (so `1.5s` spans ~45m at 3m spacing), causing
 * PID to command zero speed ~30m before a stopline or crosswalk when creeping.
 * Bounding the preview by physical reach distance
 * `max(4.0, |v| * 1.5 + 2.25)` keeps stops crisp near the stopline without
 * premature braking, while still allowing standstill pull-away toward near
 * waypoints even when a downstream stop falls inside the full trajectory.
 */
template <typename PointT>
inline double cruise_target_speed_in_horizon(const PointT* pts, uint32_t n_pts,
                                             double current_speed_mps) {
    if (!pts || n_pts == 0) return 0.0;
    if (n_pts == 1) return static_cast<double>(pts[0].v);

    uint32_t start_i = 0;
    /* Standstill pull-away: index 0 can be 0 due to ST/smoothing at s=0; skip
     * it when index 1 clearly commands forward acceleration, even if a stop
     * exists farther down the trajectory (`pts[n_pts - 1].v == 0`). */
    if (static_cast<double>(pts[0].v) < 0.5 &&
        static_cast<double>(pts[1].v) > static_cast<double>(pts[0].v) + 0.1 &&
        static_cast<double>(pts[1].v) > 0.5) {
        start_i = 1;
    }

    const double reach_horizon_m =
        std::fmax(4.0, std::fabs(current_speed_mps) * 1.5 + 2.25);

    double tv = static_cast<double>(pts[start_i].v);
    double cum_dist_m = 0.0;
    for (uint32_t i = 1; i < n_pts; i++) {
        cum_dist_m += std::hypot(
            static_cast<double>(pts[i].x) - static_cast<double>(pts[i - 1].x),
            static_cast<double>(pts[i].y) - static_cast<double>(pts[i - 1].y));
        if (i < start_i) continue;
        if (i > start_i &&
            (static_cast<double>(pts[i].t_rel_us) > 1500000.0 ||
             cum_dist_m > reach_horizon_m)) {
            break;
        }
        const double vi = static_cast<double>(pts[i].v);
        if (vi < tv) tv = vi;
    }
    return tv;
}

}  // namespace adas_control
