#ifndef FLOWSIM_NPC_YIELD_H
#define FLOWSIM_NPC_YIELD_H

#include "entity.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace flowsim {

/**
 * Counts drivable lanes moving in the NPC's route direction so one-way
 * multi-lane ramps (e.g. {-1, -2}) are not miscounted as total/2 == 1.
 */
inline int count_same_dir_lanes(const std::vector<int>& lane_ids, int route_dir) {
    int neg = 0;
    int pos = 0;
    for (int id : lane_ids) {
        if (id < 0) ++neg;
        else if (id > 0) ++pos;
    }
    if (route_dir > 0) return (neg > 0) ? neg : pos;
    if (route_dir < 0) return (pos > 0) ? pos : neg;
    return std::max(neg, pos);
}

/**
 * Computes road-reference lateral guardrail bounds [min_t, max_t] from the
 * actual positive/negative drivable lane counts (handles asymmetric one-way roads).
 */
inline void compute_guardrail_bounds(const std::vector<int>& lane_ids,
                                     double lane_width,
                                     double shoulder,
                                     double& out_min_t,
                                     double& out_max_t) {
    int neg = 0;
    int pos = 0;
    for (int id : lane_ids) {
        if (id < 0) ++neg;
        else if (id > 0) ++pos;
    }
    out_min_t = (neg > 0) ? -(neg * lane_width + shoulder) : -shoulder;
    out_max_t = (pos > 0) ?  (pos * lane_width + shoulder) :  shoulder;
}

struct CrossingYieldResult {
    bool should_yield{false};
    double speed_cap{1e9};
    EntityId yield_to{INVALID_ENTITY};
    double cpa_dist_m{1e9};
    double cpa_time_s{1e9};
};

/**
 * Predict crossing/merging conflict-zone CPA between `npc` and other active
 * vehicles in `pool` over horizon `horizon_s`.
 *
 * Priority rule:
 *   1. Ego vehicle (id == 0) has right-of-way when entering a shared conflict zone.
 *   2. Otherwise, the vehicle closer in time to the conflict zone (with 0.3s
 *      hysteresis, or smaller id on tie) has priority; the later vehicle yields.
 */
inline CrossingYieldResult evaluate_crossing_conflict_yield(
    const Entity& npc,
    const EntityPool& pool,
    double horizon_s = 3.0,
    double conflict_radius_m = 3.5)
{
    CrossingYieldResult res;
    if (npc.speed < 0.2) return res;

    const double h1 = npc.heading;
    const double vx1 = npc.speed * std::cos(h1);
    const double vy1 = npc.speed * std::sin(h1);

    for (const auto& other : pool) {
        if (!other.active || other.id == npc.id) continue;
        if (std::fabs(other.z - npc.z) > 2.2) continue;

        if (other.type == EntityType::Pedestrian) {
            // 横穿斑马线行人无条件优先：预测其是否在车辆前向走廊附近横过
            const double ux = std::cos(h1), uy = std::sin(h1);
            const double dx0 = other.x - npc.x, dy0 = other.y - npc.y;
            const double along = dx0 * ux + dy0 * uy;
            if (along <= 0.5 || along > 26.0) continue;
            const double lat = -dx0 * uy + dy0 * ux;
            const double vx2 = other.vx, vy2 = other.vy;
            const double lat_v = -vx2 * uy + vy2 * ux;
            double min_lat = std::fabs(lat);
            if (std::fabs(lat_v) > 0.1) {
                double t_zero = -lat / lat_v;
                if (t_zero >= 0.0 && t_zero <= horizon_s) min_lat = 0.0;
                min_lat = std::min(min_lat, std::fabs(lat + lat_v * std::clamp(horizon_s, 0.0, 3.0)));
            }
            if (min_lat > 2.4) continue;
            const double safe_dist = std::max(0.0, along - 4.5);
            const double cap = std::max(0.0, std::min(npc.target_vx, safe_dist / 1.8));
            if (!res.should_yield || cap < res.speed_cap) {
                res.should_yield = true;
                res.speed_cap = cap;
                res.yield_to = other.id;
                res.cpa_dist_m = min_lat;
                res.cpa_time_s = along / std::max(npc.speed, 0.5);
            }
            continue;
        }

        // Only evaluate crossing or merging trajectories (|sin(dh)| > 0.35 -> > ~20 deg)
        const double dh = npc.heading - other.heading;
        if (std::fabs(std::sin(dh)) < 0.35) continue;

        const double dx0 = other.x - npc.x;
        const double dy0 = other.y - npc.y;
        // Skip vehicles far away
        if (dx0 * dx0 + dy0 * dy0 > 45.0 * 45.0) continue;

        const double vx2 = other.speed * std::cos(other.heading);
        const double vy2 = other.speed * std::sin(other.heading);
        const double rvx = vx2 - vx1;
        const double rvy = vy2 - vy1;
        const double rv2 = rvx * rvx + rvy * rvy;
        if (rv2 < 1e-4) continue;

        double t_cpa = -(dx0 * rvx + dy0 * rvy) / rv2;
        if (t_cpa < 0.0) t_cpa = 0.0;
        if (t_cpa > horizon_s) t_cpa = horizon_s;
        if (t_cpa < 0.05) continue;  // already past or overlapping at t=0

        const double cx1 = npc.x + vx1 * t_cpa;
        const double cy1 = npc.y + vy1 * t_cpa;
        const double cx2 = other.x + vx2 * t_cpa;
        const double cy2 = other.y + vy2 * t_cpa;
        const double d_cpa = std::hypot(cx2 - cx1, cy2 - cy1);
        if (d_cpa > conflict_radius_m) continue;

        // Determine if `other` is ahead/entering conflict or has priority
        const double d1 = std::hypot(cx1 - npc.x, cy1 - npc.y);
        const double d2 = std::hypot(cx1 - other.x, cy1 - other.y);
        const double t1 = d1 / std::max(npc.speed, 0.5);
        const double t2 = d2 / std::max(other.speed, 0.5);

        const bool other_has_priority =
            (other.id == 0) ||
            (t2 + 0.30 < t1) ||
            (std::fabs(t2 - t1) <= 0.30 && other.id < npc.id);

        if (!other_has_priority) continue;

        // Smooth speed cap: slow down proportionally as conflict approaches
        const double safe_dist = std::max(0.0, d1 - 6.0);
        const double cap = std::min(npc.target_vx, safe_dist / 1.8);
        if (!res.should_yield || cap < res.speed_cap) {
            res.should_yield = true;
            res.speed_cap = std::max(0.0, cap);
            res.yield_to = other.id;
            res.cpa_dist_m = d_cpa;
            res.cpa_time_s = t_cpa;
        }
    }
    return res;
}

/**
 * Checks whether changing into `target_offset` has sufficient front, rear, and
 * side-by-side clearance (including vehicles simultaneously changing lanes into
 * `target_offset`, elevation-isolated vehicles, and world-coordinate traffic
 * such as ego or off-route cars).
 */
inline bool is_target_lane_safe_for_change(
    const Entity& npc,
    double target_offset,
    const EntityPool& pool,
    double same_lane_tol,
    double min_front_gap = 12.0,
    double min_rear_gap = 12.0)
{
    const double lat_tol = std::max(same_lane_tol, 2.2);
    const double fx = std::cos(npc.heading);
    const double fy = std::sin(npc.heading);
    const double lat_shift = (npc.route_dir < 0)
        ? -(target_offset - npc.offset)
        : (target_offset - npc.offset);

    for (int i = 0; i < pool.size(); ++i) {
        const Entity& o = pool[i];
        if (!o.active || o.id == npc.id || !o.is_vehicle()) {
            continue;
        }
        if (std::fabs(o.z - npc.z) > 2.2) {
            continue;
        }
        const double half_sum = 0.5 * (npc.length + o.length);

        if (npc.route_dir != 0 && o.route_dir == npc.route_dir) {
            const bool hits_cur = std::fabs(o.offset - target_offset) < lat_tol;
            const bool hits_tgt =
                (o.state == NpcState::LaneChange || o.state == NpcState::CutIn) &&
                (std::fabs(o.target_offset - target_offset) < lat_tol);
            if (!hits_cur && !hits_tgt) {
                continue;
            }
            const double ahead = (o.route_s - npc.route_s) * (double)npc.route_dir;
            if (std::fabs(ahead) < half_sum + 1.5) {
                return false;
            }
            if (ahead > 0.0) {
                const double front_gap = ahead - half_sum;
                const double needed = min_front_gap + std::max(0.0, npc.speed - o.speed) * 1.2;
                if (front_gap < needed) {
                    return false;
                }
            } else {
                const double rear_gap = -ahead - half_sum;
                const double needed = min_rear_gap + std::max(0.0, o.speed - npc.speed) * 1.5;
                if (rear_gap < needed) {
                    return false;
                }
            }
        } else {
            const double dx = o.x - npc.x;
            const double dy = o.y - npc.y;
            const double long_d = dx * fx + dy * fy;
            const double lat_d = -dx * fy + dy * fx;
            const bool in_dest = std::fabs(lat_d - lat_shift) < lat_tol;
            const bool in_sweep = (std::fabs(lat_d - 0.5 * lat_shift) < 1.8) &&
                                  (std::fabs(long_d) < half_sum + 2.0);
            if (in_sweep) {
                return false;
            }
            if (in_dest &&
                long_d >= -(half_sum + min_rear_gap) &&
                long_d <= (half_sum + min_front_gap)) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Result of evaluating whether a lateral offset exceeds guardrail bounds.
 * Trigger tolerance avoids float-rounding lock onto clamped edges; inset keeps
 * clamped offsets cleanly inside bounds on subsequent ticks.
 */
struct GuardrailClampAction {
    bool   triggered{false};
    double clamped_t{0.0};
    double intrusion_m{0.0};
};

inline GuardrailClampAction compute_guardrail_clamp_action(
    double ref_t, double min_t, double max_t,
    double trigger_eps = 0.05, double inset = 0.08) {
    GuardrailClampAction act;
    if (ref_t >= min_t - trigger_eps && ref_t <= max_t + trigger_eps) {
        act.clamped_t = ref_t;
        return act;
    }
    const double safe_inset = std::min(inset, std::max(0.0, 0.25 * (max_t - min_t)));
    act.triggered = true;
    if (ref_t < min_t) {
        act.intrusion_m = min_t - ref_t;
        act.clamped_t = min_t + safe_inset;
    } else {
        act.intrusion_m = ref_t - max_t;
        act.clamped_t = max_t - safe_inset;
    }
    return act;
}

/**
 * Compute effective same-direction lane count with lookahead to the next route
 * segment so vehicles merge smoothly before entering a lane-drop boundary.
 */
inline int compute_eff_lane_drop_same_dir_lanes(
    int curr_same_dir, int next_same_dir,
    double dist_to_seg_end_m, double lookahead_m = 65.0) {
    if (curr_same_dir <= 0 || next_same_dir <= 0 ||
        dist_to_seg_end_m < 0.0 || dist_to_seg_end_m > lookahead_m) {
        return curr_same_dir;
    }
    return std::min(curr_same_dir, next_same_dir);
}

/**
 * Return true if advancing a pedestrian to `(next_x, next_y)` moves closer into
 * any active same-elevation vehicle's expanded safety envelope (crosswalk yield).
 */
inline bool is_pedestrian_step_blocked_by_vehicle(
    const Entity& ped, double next_x, double next_y, const EntityPool& pool,
    double pad_long = 0.55, double pad_lat = 0.45) {
    for (const auto& v : pool) {
        if (!v.active || !v.is_vehicle()) continue;
        if (std::fabs(v.z - ped.z) > 2.2) continue;
        const double ch = std::cos(v.heading);
        const double sh = std::sin(v.heading);
        const double hl = 0.5 * (v.length + ped.length) + pad_long;
        const double hw = 0.5 * (v.width + ped.width) + pad_lat;
        const double dx_n = next_x - v.x;
        const double dy_n = next_y - v.y;
        const double lon_n = dx_n * ch + dy_n * sh;
        const double lat_n = -dx_n * sh + dy_n * ch;
        if (std::fabs(lon_n) >= hl || std::fabs(lat_n) >= hw) continue;
        const double dx_c = ped.x - v.x;
        const double dy_c = ped.y - v.y;
        const double d2_n = dx_n * dx_n + dy_n * dy_n;
        const double d2_c = dx_c * dx_c + dy_c * dy_c;
        if (d2_n <= d2_c + 1e-9) {
            return true;
        }
    }
    return false;
}

/**
 * Result of projecting a vehicle onto a reference road centerline pose.
 */
struct OwnRoadProjection {
    bool   valid{false};
    double along{0.0};
    double lat{0.0};
};

/**
 * Project world position `(ex, ey, ez)` onto road tangent `(wp_x, wp_y, wp_z, wp_h)`.
 * Rejects mismatches across stacked viaduct decks (`|ez - wp_z| > max_dz`) or
 * longitudinal drift (`|along| > max_along`).
 */
inline OwnRoadProjection project_onto_road_tangent(
    double ex, double ey, double ez,
    double wp_x, double wp_y, double wp_z, double wp_h,
    double max_along = 8.0, double max_dz = 2.2) {
    OwnRoadProjection proj;
    if (std::fabs(ez - wp_z) > max_dz) return proj;
    const double dx = ex - wp_x;
    const double dy = ey - wp_y;
    const double ch = std::cos(wp_h);
    const double sh = std::sin(wp_h);
    proj.along = dx * ch + dy * sh;
    proj.lat = -dx * sh + dy * ch;
    proj.valid = (std::fabs(proj.along) <= max_along);
    return proj;
}

/**
 * Return true if the vehicle already tracks an OpenDRIVE road via route or RoadPosition,
 * so junction guardrail/recovery must not hijack it onto crossing roads.
 */
inline bool is_road_tracked_vehicle(int road_id, int route_dir, bool road_pos_ok) {
    return road_id >= 0 && (route_dir != 0 || road_pos_ok);
}

/**
 * Evaluate whether `other` acts as a forward longitudinal or encroaching
 * corridor lead for `npc`, and compute the signed bumper-to-bumper `out_gap_m`.
 *
 * Unifies `find_lead` and `step_npc_vehicle` gap calculation:
 * - Same-lane traffic: preserves `route_s` longitudinal order on curved routes
 *   over `look_ahead_m`, and projects off-route targets along `npc.heading` with
 *   `other`'s rotated OBB half-length.
 * - Cross-lane corridor intrusion (e.g. ego swinging out during perpendicular
 *   parking/U-turn so its center `lane_id` flips while its rotated body still
 *   blocks `npc`'s lane): checks Frenet lateral offset on same-road segments and
 *   short-range world lateral envelope across junctions.
 */
inline bool compute_corridor_lead_gap(
    const Entity& npc,
    const Entity& other,
    bool is_same_lane,
    bool ego_maneuvering,
    double look_ahead_m,
    double& out_gap_m)
{
    if (std::fabs(other.z - npc.z) > 2.2) return false;

    const double fx = std::cos(npc.heading);
    const double fy = std::sin(npc.heading);
    const double dx = other.x - npc.x;
    const double dy = other.y - npc.y;
    const double proj_ahead = dx * fx + dy * fy;

    if (other.type == EntityType::Pedestrian) {
        const double lat = -dx * fy + dy * fx;
        if (std::fabs(lat) > 1.6) return false;
        if (proj_ahead <= 0.0 || proj_ahead > look_ahead_m) return false;
        out_gap_m = proj_ahead - (other.length * 0.5 + npc.length * 0.5);
        return true;
    }

    const double dh = other.heading - npc.heading;
    const double cd = std::fabs(std::cos(dh));
    const double sd = std::fabs(std::sin(dh));
    const double o_half_long = 0.5 * (other.length * cd + other.width * sd);
    const double o_half_lat  = 0.5 * (other.length * sd + other.width * cd);

    if (is_same_lane) {
        double ahead = proj_ahead;
        if (npc.route_dir != 0 && other.route_dir != 0) {
            ahead = (other.route_s - npc.route_s) * static_cast<double>(npc.route_dir);
        }
        if (ahead <= 0.0 || ahead > look_ahead_m) return false;
        const double eff_half_long =
            (npc.route_dir != 0 && other.route_dir != 0) ? (other.length * 0.5) : o_half_long;
        out_gap_m = ahead - (eff_half_long + npc.length * 0.5);
        return true;
    }

    if (other.parked_fixed) return false;
    const double corridor_look_ahead = std::min(look_ahead_m, 25.0);
    if (proj_ahead <= 0.0 || proj_ahead > corridor_look_ahead) return false;

    double lat_dist = 1e9;
    if (npc.road_id >= 0 && other.road_id == npc.road_id) {
        lat_dist = std::fabs(other.offset - npc.offset);
    } else if (proj_ahead <= 16.0 && (other.id == 0 || sd > 0.35)) {
        lat_dist = std::fabs(-dx * fy + dy * fx);
    } else {
        return false;
    }

    const double margin = (other.id == 0 && ego_maneuvering) ? 0.75 : 0.40;
    const double lat_limit = o_half_lat + npc.width * 0.5 + margin;
    if (lat_dist >= lat_limit) return false;

    out_gap_m = proj_ahead - (o_half_long + npc.length * 0.5);
    return true;
}

}  // namespace flowsim

#endif  // FLOWSIM_NPC_YIELD_H
