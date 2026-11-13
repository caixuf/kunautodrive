#ifndef FLOWSIM_NPC_YIELD_H
#define FLOWSIM_NPC_YIELD_H

#include "entity.h"

#include <algorithm>
#include <cmath>

namespace flowsim {

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
        if (other.type == EntityType::Pedestrian) continue;

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

}  // namespace flowsim

#endif  // FLOWSIM_NPC_YIELD_H
