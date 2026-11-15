#ifndef PLANNING_TL_STOP_H
#define PLANNING_TL_STOP_H

namespace adas_planning {

/* Decide if an approaching yellow signal permits clearing the intersection
 * without unsafe hard braking. A stopped or creeping vehicle (v < 0.5 m/s)
 * has negligible stopping distance and must hold behind the stopline. */
inline bool yellow_light_can_clear(double dx_tl_m, double ego_speed_mps) {
    if (dx_tl_m <= 0.0) return true;
    const double v = (ego_speed_mps > 0.0) ? ego_speed_mps : 0.0;
    if (v < 0.5) return false;
    const double safe_stop_dist = v * 0.8 + v * v / 8.0 + 2.0;
    return dx_tl_m < safe_stop_dist;
}

/* Compute ego-relative ST stop distance from stopline distance and safety margin.
 * Clamps positive stopline distances inside [0, wall_margin] to 0.0 so a vehicle
 * creeping right behind the stopline holds stopped rather than dropping the wall. */
inline double tl_effective_stop_s(double stopline_s_m, double wall_margin_m) {
    if (stopline_s_m < 0.0) return -1.0;
    const double d = stopline_s_m - wall_margin_m;
    return (d > 0.0) ? d : 0.0;
}

/* Merge a candidate non-negative stop distance into an optional min stop_s (<0 unset). */
inline double merge_min_stop_s(double current_stop_s, double candidate_stop_s) {
    if (candidate_stop_s < 0.0) return current_stop_s;
    if (current_stop_s < 0.0 || candidate_stop_s < current_stop_s) {
        return candidate_stop_s;
    }
    return current_stop_s;
}

}  // namespace adas_planning

#endif /* PLANNING_TL_STOP_H */
