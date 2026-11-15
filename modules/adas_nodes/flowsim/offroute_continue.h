/**
 * offroute_continue.h — Transition/loop helpers for off-route NPCs (route_dir == 0)
 * when RoadPosition::advance() reaches an unlinked road boundary.
 */

#ifndef FLOWSIM_OFFROUTE_CONTINUE_H
#define FLOWSIM_OFFROUTE_CONTINUE_H

#include "entity.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace flowsim {

struct FlowRoadNetwork;
class Route;

struct RouteEndpointCand {
    int    route_idx{0};
    int    road_id{0};
    bool   is_virtual{false};
    double s0{0.0};
    double length{0.0};
    double start_x{0.0}, start_y{0.0}, start_z{0.0}, start_h{0.0};
    double end_x{0.0},   end_y{0.0},   end_z{0.0},   end_h{0.0};
};

struct OffrouteContinuation {
    bool   matched{false};
    int    route_idx{-1};
    int    road_id{-1};
    int    route_dir{0};
    double s_local{0.0};
    double dist_m{1e9};
};

inline double normalize_angle_pm_pi(double h) {
    while (h > M_PI)  h -= 2.0 * M_PI;
    while (h < -M_PI) h += 2.0 * M_PI;
    return h;
}

/**
 * Selects the closest aligned Route segment endpoint (<10m seam, matching or
 * oncoming heading) to seamlessly continue an off-route NPC onto the central route.
 */
inline OffrouteContinuation select_offroute_route_continuation(
    double npc_x, double npc_y, double npc_z, double npc_heading,
    const RouteEndpointCand* cands, int cand_count) {
    OffrouteContinuation best;
    if (!cands || cand_count <= 0) {
        return best;
    }
    for (int i = 0; i < cand_count; ++i) {
        const RouteEndpointCand& c = cands[i];
        if (c.is_virtual || c.road_id < 0 || c.length < 1.0) {
            continue;
        }
        const double eps = std::min(0.5, c.length * 0.1);
        if (std::fabs(npc_z - c.start_z) <= 2.2) {
            const double d = std::hypot(npc_x - c.start_x, npc_y - c.start_y);
            const double cos_h = std::cos(c.start_h - npc_heading);
            if (d < 10.0 && cos_h > 0.85 && d < best.dist_m) {
                best.matched = true;
                best.route_idx = c.route_idx;
                best.road_id = c.road_id;
                best.route_dir = 1;
                best.s_local = c.s0 + eps;
                best.dist_m = d;
            }
        }
        if (std::fabs(npc_z - c.end_z) <= 2.2) {
            const double d = std::hypot(npc_x - c.end_x, npc_y - c.end_y);
            const double cos_h = std::cos(c.end_h - npc_heading);
            if (d < 10.0 && cos_h < -0.85 && d < best.dist_m) {
                best.matched = true;
                best.route_idx = c.route_idx;
                best.road_id = c.road_id;
                best.route_dir = -1;
                best.s_local = c.s0 + std::max(0.0, c.length - eps);
                best.dist_m = d;
            }
        }
    }
    return best;
}

/**
 * Picks a drivable lane ID with the requested OpenDRIVE sign (-1 for forward
 * right-hand lanes, +1 for oncoming lanes), preserving preferred magnitude when
 * available or falling back to the closest lane to centerline.
 */
inline int pick_directional_lane(const std::vector<int>& lanes,
                                 int preferred_abs_lane,
                                 int want_sign) {
    if (lanes.empty() || want_sign == 0) {
        return 0;
    }
    const int target = (want_sign < 0) ? -std::max(1, preferred_abs_lane)
                                       :  std::max(1, preferred_abs_lane);
    int best = 0;
    int best_abs = 1000000;
    for (int id : lanes) {
        if ((want_sign < 0 && id >= 0) || (want_sign > 0 && id <= 0)) {
            continue;
        }
        if (id == target) {
            return id;
        }
        if (std::abs(id) < best_abs) {
            best_abs = std::abs(id);
            best = id;
        }
    }
    return best;
}

/**
 * Returns true if no active vehicle (same elevation layer) lies within min_dist_m.
 */
inline bool has_spawn_clearance(const EntityPool& pool, int self_id,
                                double x, double y, double z,
                                double min_dist_m = 15.0) {
    for (int i = 0; i < pool.size(); ++i) {
        const Entity& o = pool[i];
        if (!o.active || o.id == self_id || !o.is_vehicle()) {
            continue;
        }
        if (std::fabs(o.z - z) > 2.2) {
            continue;
        }
        if (std::hypot(o.x - x, o.y - y) < min_dist_m) {
            return false;
        }
    }
    return true;
}

/**
 * Generates candidate local s offsets along an isolated stub road for looping
 * off-route NPCs so transient congestion at s=2m does not force a route flip.
 */
inline std::vector<double> stub_loop_candidates(double road_length) {
    std::vector<double> out;
    if (road_length < 10.0) {
        return out;
    }
    const double max_s = std::min(road_length - 8.0, 96.0);
    for (double s = 2.0; s <= max_s + 1e-6; s += 12.0) {
        out.push_back(s);
    }
    if (out.empty()) {
        out.push_back(2.0);
    }
    return out;
}

/**
 * Prevents forward route_dir>0 NPC recycling from landing in underground garage
 * or ramp segments (route_s < surface_start_s). Places behind ego once ego has
 * cleared the surface entry by >=50m, otherwise safely ahead on the surface.
 */
inline double adjust_recycle_target_for_surface(double raw_target, int route_dir,
                                                double ego_route_s, double total_len,
                                                double surface_start_s, int entity_id) {
    if (total_len <= 0.0) {
        return 0.0;
    }
    double clamped = std::clamp(raw_target, 0.0, total_len);
    if (route_dir <= 0 || surface_start_s <= 0.5 || total_len <= surface_start_s + 10.0) {
        return clamped;
    }
    if (clamped >= surface_start_s) {
        return clamped;
    }
    if (ego_route_s >= surface_start_s + 50.0) {
        return surface_start_s;
    }
    const double max_ahead = std::max(surface_start_s, total_len - 20.0);
    const double downstream = std::max(
        surface_start_s + 30.0,
        ego_route_s + 200.0 + (double)(std::abs(entity_id) % 5) * 25.0);
    return std::clamp(downstream, surface_start_s, max_ahead);
}

}  // namespace flowsim

#if defined(FLOWSIM_ROAD_NETWORK_H) && defined(FLOWSIM_ROUTE_H)
#include "lane_frenet.h"

namespace flowsim {

/**
 * Computes road tangent heading at (rid, s, offset) using two-point finite
 * difference along the reference centerline so esmini lane-0 heading ambiguity
 * never flips orientation.
 */
inline bool compute_road_tangent_heading(FlowRoadNetwork& roads, int rid, double s,
                                         double offset, int route_dir, double& out_h) {
    WorldPos w0, w1;
    const double ds = 0.5;
    if (!roads.frenet_to_world(rid, 0, s, offset, w0)) {
        return false;
    }
    double hx = 0.0;
    double hy = 0.0;
    if (roads.frenet_to_world(rid, 0, s + ds, offset, w1)) {
        hx = w1.x - w0.x;
        hy = w1.y - w0.y;
    } else if (roads.frenet_to_world(rid, 0, std::max(0.0, s - ds), offset, w1)) {
        hx = w0.x - w1.x;
        hy = w0.y - w1.y;
    } else {
        return false;
    }
    if (hx * hx + hy * hy < 1e-10) {
        return false;
    }
    double h = std::atan2(hy, hx);
    if (route_dir < 0) {
        h += M_PI;
    }
    out_h = normalize_angle_pm_pi(h);
    return true;
}

inline bool apply_route_continuation(Entity& npc, const EntityPool& pool,
                                     FlowRoadNetwork& roads, const Route& route,
                                     uint32_t cycle) {
    std::vector<RouteEndpointCand> cands;
    cands.reserve((size_t)route.count());
    for (int i = 0; i < route.count(); ++i) {
        const RouteSeg& seg = route.seg(i);
        if (seg.is_virtual || seg.road_id < 0 || seg.length < 1.0) {
            continue;
        }
        WorldPos ws, we;
        if (!roads.frenet_to_world(seg.road_id, 0, seg.s0, 0.0, ws) ||
            !roads.frenet_to_world(seg.road_id, 0, seg.s0 + seg.length, 0.0, we)) {
            continue;
        }
        double sh = ws.h;
        double eh = we.h;
        (void)compute_road_tangent_heading(roads, seg.road_id, seg.s0, 0.0, 1, sh);
        (void)compute_road_tangent_heading(roads, seg.road_id, seg.s0 + seg.length, 0.0, 1, eh);
        RouteEndpointCand c;
        c.route_idx = i;
        c.road_id = seg.road_id;
        c.is_virtual = seg.is_virtual;
        c.s0 = seg.s0;
        c.length = seg.length;
        c.start_x = ws.x; c.start_y = ws.y; c.start_z = ws.z; c.start_h = sh;
        c.end_x = we.x;   c.end_y = we.y;   c.end_z = we.z;   c.end_h = eh;
        cands.push_back(c);
    }

    const OffrouteContinuation cont = select_offroute_route_continuation(
        npc.x, npc.y, npc.z, npc.heading, cands.data(), (int)cands.size());
    if (!cont.matched) {
        return false;
    }

    const int want_sign = (cont.route_dir > 0) ? -1 : 1;
    const int target_lane = pick_directional_lane(
        roads.drivable_lane_ids(cont.road_id, cont.s_local),
        std::max(1, std::abs(npc.lane_id)), want_sign);
    if (target_lane == 0) {
        return false;
    }

    const double target_offset = lane_center_t(roads, cont.road_id, target_lane, cont.s_local);
    WorldPos wp;
    if (!roads.frenet_to_world(cont.road_id, target_lane, cont.s_local, 0.0, wp)) {
        return false;
    }
    if (!has_spawn_clearance(pool, npc.id, wp.x, wp.y, wp.z, 15.0)) {
        return false;
    }
    if (cont.route_dir > 0 &&
        !npc.road_pos.relocate(roads, cont.road_id, target_lane, cont.s_local, 0.0)) {
        return false;
    }

    double h = normalize_angle_pm_pi(wp.h + (cont.route_dir < 0 ? M_PI : 0.0));
    (void)compute_road_tangent_heading(roads, cont.road_id, cont.s_local,
                                       target_offset, cont.route_dir, h);

    const double prev_x = npc.x;
    const double prev_y = npc.y;
    npc.road_id = cont.road_id;
    npc.lane_id = target_lane;
    npc.s = cont.s_local;
    npc.offset = target_offset;
    npc.target_offset = target_offset;
    npc.route_dir = cont.route_dir;
    npc.route_s = route.to_route_s(cont.route_idx, cont.s_local);
    npc.x = wp.x;
    npc.y = wp.y;
    npc.z = wp.z;
    npc.heading = h;
    npc.vx = npc.speed * std::cos(h);
    npc.vy = npc.speed * std::sin(h);
    if (std::hypot(wp.x - prev_x, wp.y - prev_y) > 1.5) {
        npc.last_teleport_cycle = cycle;
    }
    return true;
}

inline bool try_loop_offroute_stub(Entity& npc, const EntityPool& pool,
                                   FlowRoadNetwork& roads, uint32_t cycle) {
    RoadInfo ri;
    bool found = false;
    const int n_roads = roads.road_count();
    for (int i = 0; i < n_roads; ++i) {
        if (roads.road_info(i, ri) && (int)ri.id == npc.road_id) {
            found = true;
            break;
        }
    }
    if (!found || ri.length < 10.0) {
        return false;
    }

    const std::vector<double> cands = stub_loop_candidates(ri.length);
    for (double s_loop : cands) {
        const std::vector<int> lanes = roads.drivable_lane_ids(npc.road_id, s_loop);
        int loop_lane = npc.lane_id;
        if (loop_lane == 0 ||
            std::find(lanes.begin(), lanes.end(), loop_lane) == lanes.end()) {
            loop_lane = pick_directional_lane(lanes, 1, -1);
        }
        if (loop_lane == 0) {
            continue;
        }

        const double loop_offset = lane_center_t(roads, npc.road_id, loop_lane, s_loop);
        WorldPos wp;
        if (!roads.frenet_to_world(npc.road_id, loop_lane, s_loop, 0.0, wp)) {
            continue;
        }
        if (!has_spawn_clearance(pool, npc.id, wp.x, wp.y, wp.z, 10.0)) {
            continue;
        }
        if (!npc.road_pos.relocate(roads, npc.road_id, loop_lane, s_loop, 0.0)) {
            continue;
        }

        double h = normalize_angle_pm_pi(wp.h);
        (void)compute_road_tangent_heading(roads, npc.road_id, s_loop,
                                           loop_offset, 1, h);

        npc.lane_id = loop_lane;
        npc.s = s_loop;
        npc.offset = loop_offset;
        npc.target_offset = loop_offset;
        npc.x = wp.x;
        npc.y = wp.y;
        npc.z = wp.z;
        npc.heading = h;
        npc.vx = npc.speed * std::cos(h);
        npc.vy = npc.speed * std::sin(h);
        npc.last_teleport_cycle = cycle;
        return true;
    }
    return false;
}

/**
 * Finds the earliest route_s whose centerline elevation reaches `min_z`,
 * so underground garage/ramp starts are never targeted by surface recycling.
 */
inline double find_surface_route_s(const Route& route, FlowRoadNetwork* roads,
                                   double min_z = -0.35) {
    if (!route.ok() || !roads || !roads->loaded()) {
        return 0.0;
    }
    for (int i = 0; i < route.count(); ++i) {
        const RouteSeg& seg = route.seg(i);
        if (seg.is_virtual || seg.road_id < 0 || seg.length <= 0.0) {
            continue;
        }
        for (double ds = 0.0; ds <= seg.length + 1e-6; ds += 5.0) {
            const double s_loc = seg.s0 + std::min(ds, seg.length);
            WorldPos wp;
            if (roads->frenet_to_world(seg.road_id, 0, s_loc, 0.0, wp) &&
                wp.z >= min_z) {
                return route.to_route_s(i, s_loc);
            }
        }
    }
    return 0.0;
}

/**
 * Attempts to continue an off-route (route_dir == 0) NPC onto an aligned route
 * segment seam, or loop back onto its stub road with >=10m clearance.
 */
inline bool try_continue_offroute_npc(Entity& npc, const EntityPool& pool,
                                      FlowRoadNetwork* roads, const Route* route,
                                      uint32_t cycle) {
    if (npc.route_dir != 0 || !roads || !roads->loaded()) {
        return false;
    }
    if (route && route->ok() &&
        apply_route_continuation(npc, pool, *roads, *route, cycle)) {
        return true;
    }
    return try_loop_offroute_stub(npc, pool, *roads, cycle);
}

}  // namespace flowsim
#endif  // FLOWSIM_ROAD_NETWORK_H

#endif  // FLOWSIM_OFFROUTE_CONTINUE_H
