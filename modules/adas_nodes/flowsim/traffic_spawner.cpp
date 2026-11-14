#include "flowsim/traffic_spawner.h"

#include "flowsim/lane_frenet.h"
#include "flowsim/npc_ai.h"
#include "flowsim/physics.h"
#include "traffic_density_spawn.h"
#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_FATAL
#include "logger.h"

#include <cjson/cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdlib.h>
#include <utility>

namespace flowsim {

namespace {

bool is_in_protected_parking_zone(const ScenarioConfig* sc,
                                    double wx, double wy, double wz) {
    if (wz < -0.35) return true;
    if (!sc) return false;
    if (sc->park_out.enabled) {
        if (std::hypot(wx - sc->park_out.slot_x, wy - sc->park_out.slot_y) < 160.0 ||
            std::hypot(wx - sc->park_out.exit_x, wy - sc->park_out.exit_y) < 140.0) {
            return true;
        }
    }
    if (sc->parking_goal.enabled &&
        std::hypot(wx - sc->parking_goal.x, wy - sc->parking_goal.y) < 95.0) {
        return true;
    }
    for (int i = 0; i < sc->parking_slot_count; ++i) {
        const auto& ps = sc->parking_slots[i];
        if (std::hypot(wx - ps.x, wy - ps.y) < 95.0) {
            return true;
        }
    }
    return false;
}

bool is_near_surface_lot_apron(const ScenarioConfig* sc, double wx, double wy) {
    if (!sc) return false;
    if (sc->parking_goal.enabled &&
        std::hypot(wx - sc->parking_goal.x, wy - sc->parking_goal.y) < 70.0) {
        return true;
    }
    for (int i = 0; i < sc->parking_slot_count; ++i) {
        const auto& ps = sc->parking_slots[i];
        if (std::hypot(wx - ps.x, wy - ps.y) < 70.0) {
            return true;
        }
    }
    return false;
}

}  // namespace

void build_npc_routes_from_scenario(TrafficSpawnerContext& ctx) {
    if (!ctx.npc_routes) return;
    ctx.npc_routes->clear();
    if (!ctx.scenario || !ctx.scenario->road_network_json || !ctx.roads_loaded || !ctx.roads) {
        return;
    }
    cJSON* rn = cJSON_Parse(ctx.scenario->road_network_json);
    if (!rn) return;
    cJSON* chains = cJSON_GetObjectItemCaseSensitive(rn, "npc_route_chains");
    if (cJSON_IsArray(chains)) {
        cJSON* chain = nullptr;
        cJSON_ArrayForEach(chain, chains) {
            if (!cJSON_IsArray(chain)) continue;
            std::vector<int> ids;
            cJSON* v = nullptr;
            cJSON_ArrayForEach(v, chain) {
                if (cJSON_IsNumber(v)) ids.push_back((int)v->valuedouble);
            }
            if (ids.empty()) continue;
            Route r;
            if (r.build_from_chain(*ctx.roads, ids.data(), (int)ids.size()) && r.ok()) {
                ctx.npc_routes->push_back(std::move(r));
            } else {
                LOG_WARN("flowsim", "npc_routes: chain build failed (%d road(s))",
                         (int)ids.size());
            }
        }
    }
    cJSON_Delete(rn);
    if (!ctx.npc_routes->empty()) {
        LOG_INFO("flowsim", "npc multi-OD routes: %d chain(s), segs=%d",
                 (int)ctx.npc_routes->size(),
                 [&] { int n = 0; for (auto& r : *ctx.npc_routes) n += r.count(); return n; }());
    }
}

int auto_populate_traffic_density(TrafficSpawnerContext& ctx,
                                  const ScenarioConfig* sc,
                                  bool* pool_full_out) {
    if (pool_full_out) *pool_full_out = false;
    if (!sc || !ctx.roads || !ctx.route || !ctx.pool || !ctx.ped_rng ||
        !ctx.npc_routes || !ctx.npc_route_spawned) {
        return 0;
    }
    const ScenarioTrafficDensity* td = &sc->traffic_density;
    if (td->cars_per_km <= 0) return 0;
    if (!ctx.roads_loaded || !ctx.route->ok()) return 0;

    const int kPoolSoftLimit = MAX_ENTITIES - 2;
    const int kScenarioMaxNpc = (td->max_npcs > 0) ? td->max_npcs : 50;

    static unsigned int seed = 1u;
    seed = (unsigned int)(sc->random_seed ^ (uint32_t)td->random_seed_offset);
    if (seed == 0) seed = 1;

    int next_id = 1;
    for (int i = 0; i < sc->actor_count; i++) {
        if (sc->actors[i].id >= next_id) next_id = sc->actors[i].id + 1;
    }

    const double spacing_m = traffic_density_spacing_from_density(td->cars_per_km);
    const double jitter     = (td->spawn_jitter_m > 0.0) ? td->spawn_jitter_m : 0.0;
    const bool   do_spread  = (td->lane_spread != 0);
    int spawned = 0;
    *ctx.npc_route_spawned = 0;

    std::vector<std::pair<const Route*, int>> spawn_segs;
    {
        size_t max_segs = (size_t)ctx.route->count();
        for (const auto& r : *ctx.npc_routes) {
            if ((size_t)r.count() > max_segs) max_segs = (size_t)r.count();
        }
        for (size_t si = 0; si < max_segs; si++) {
            if ((int)si < ctx.route->count()) spawn_segs.push_back({ctx.route, (int)si});
            for (const auto& r : *ctx.npc_routes) {
                if ((int)si < r.count()) spawn_segs.push_back({&r, (int)si});
            }
        }
    }

    for (size_t sp_i = 0; sp_i < spawn_segs.size() && spawned < kScenarioMaxNpc; sp_i++) {
        const Route* seg_route = spawn_segs[sp_i].first;
        const int si = spawn_segs[sp_i].second;
        const bool on_central = (seg_route == ctx.route);
        const RouteSeg& seg = seg_route->seg(si);

        const double seg_scale = traffic_density_combined_scale(
            td->time_windows, td->time_window_count, 0.0,
            td->zones, td->zone_count, seg.s_start + seg.length * 0.5);
        const double spacing_eff = traffic_density_effective_spacing(
            spacing_m, seg_scale);
        if (spacing_eff <= 0.0) continue;
        const double raw_n = seg.length / spacing_eff;
        const int remaining = kScenarioMaxNpc - spawned;
        int n_in_seg = (raw_n > (double)remaining) ? remaining : (int)raw_n;
        if (n_in_seg < 1) continue;

        std::vector<int> dir_lanes = ctx.roads->drivable_lane_ids(seg.road_id, seg.s0);
        dir_lanes.erase(std::remove_if(dir_lanes.begin(), dir_lanes.end(),
                                       [](int lid) { return lid >= 0; }),
                        dir_lanes.end());
        std::sort(dir_lanes.begin(), dir_lanes.end(),
                  [](int a, int b) { return std::abs(a) < std::abs(b); });
        if (dir_lanes.empty()) dir_lanes.push_back(-1);

        for (int i = 0; i < n_in_seg && spawned < kScenarioMaxNpc; i++) {
            double s_local = (double)i * spacing_eff;
            if (jitter > 0.0) {
                double r = (double)rand_r(&seed) / (double)RAND_MAX;
                s_local += -jitter + r * (2.0 * jitter);
            }
            if (s_local < 0.0)        s_local = 0.0;
            if (s_local > seg.length) s_local = seg.length;

            const int slot = traffic_density_lane_slot(do_spread, i);
            const int lane_id = dir_lanes[(size_t)slot % dir_lanes.size()];
            const double s_esmini     = seg.s0 + s_local;
            const double route_s_cand = seg.s_start + s_local;

            WorldPos wp;
            if (!ctx.roads->frenet_to_world(seg.road_id, lane_id, s_esmini, 0.0, wp)) {
                if (spawned < 2) {
                    LOG_WARN("flowsim",
                             "traffic_density: road=%d lane=%d s=%.1f miss in esmini — skip",
                             seg.road_id, lane_id, s_esmini);
                }
                continue;
            }
            if (is_in_protected_parking_zone(sc, wp.x, wp.y, wp.z)) {
                continue;
            }

            {
                const Entity& ego = (*ctx.pool)[0];
                if (ego.active &&
                    std::hypot(ego.x - wp.x, ego.y - wp.y) < 25.0) {
                    continue;
                }
                bool too_close = false;
                for (int k = 1; k < ctx.pool->size(); ++k) {
                    const Entity& o = (*ctx.pool)[k];
                    if (!o.active || !o.is_vehicle()) continue;
                    if (on_central) {
                        if (std::fabs(o.route_s - route_s_cand) < 25.0) {
                            too_close = true; break;
                        }
                    } else if (std::hypot(o.x - wp.x, o.y - wp.y) < 25.0) {
                        too_close = true; break;
                    }
                }
                if (too_close) {
                    if (spawned < 2) {
                        LOG_WARN("flowsim",
                                 "traffic_density: route_s=%.1f too close to an active vehicle "
                                 "(<25m) — skip this candidate",
                                 route_s_cand);
                    }
                    continue;
                }
            }

            const bool spawn_ped =
                td->pedestrian_ratio > 0.0 &&
                sim_rng_uniform01(ctx.ped_rng) < td->pedestrian_ratio;
            if (spawn_ped) {
                double lw = ctx.roads->lane_width(seg.road_id, lane_id, s_esmini);
                if (!(lw > 1.0)) lw = 3.5;
                WorldPos wp_ped = wp;
                (void)ctx.roads->frenet_to_world(seg.road_id, lane_id, s_esmini,
                                                 lw * 0.5 - 0.25, wp_ped);
                EntityId pid = ctx.pool->alloc(EntityType::Pedestrian);
                if (pid == INVALID_ENTITY || pid >= kPoolSoftLimit) {
                    if (pool_full_out) *pool_full_out = true;
                    return spawned;
                }
                Entity& p = (*ctx.pool)[pid];
                p.scenario_id = next_id++;
                p.x = wp_ped.x; p.y = wp_ped.y; p.z = wp_ped.z;
                p.heading = wp_ped.h;
                p.length = 0.5; p.width = 0.5;
                p.vx = 0.8; p.vy = 0.0; p.speed = 0.8;
                p.target_vx = 0.8;
                p.route_dir = 0;
                p.offset = wp_ped.y;
                {
                    FrenetPos fp;
                    if (ctx.roads->world_to_frenet(wp_ped.x, wp_ped.y, fp)) {
                        p.road_id = fp.road_id;
                        p.lane_id = fp.lane_id;
                        p.s       = fp.s;
                        p.offset = offset_from_lane_internal(
                            *ctx.roads, fp.road_id, fp.lane_id, fp.s, fp.offset);
                    }
                }
                p.target_offset = p.offset;
                spawned++;
                continue;
            }

            EntityId id = ctx.pool->alloc(EntityType::Car);
            if (id == INVALID_ENTITY || id >= kPoolSoftLimit) {
                if (pool_full_out) *pool_full_out = true;
                if (spawned < 2) {
                    LOG_WARN("flowsim",
                             "traffic_density: pool full at %d NPCs — stop spawn",
                             spawned);
                }
                return spawned;
            }
            Entity& e = (*ctx.pool)[id];
            e.scenario_id = next_id++;
            e.x = wp.x;
            e.y = wp.y;
            e.z = wp.z;
            e.heading = wp.h;
            double vx0;
            {
                double r_v = (double)rand_r(&seed) / (double)RAND_MAX;
                vx0 = 8.0 + r_v * (14.0 - 8.0);
            }
            e.vx = vx0;
            e.vy = 0.0;
            e.speed = vx0;
            e.target_vx = vx0;
            e.length = 4.6;
            e.width = 2.0;
            e.wheelbase = 2.7;
            apply_vehicle_defaults(e);
            int r;
            {
                double r_r = (double)rand_r(&seed) / (double)RAND_MAX;
                r = (int)(r_r * 10.0);
            }
            e.turn_intent = (r < 6) ? 0 : ((r % 2) ? 1 : 2);
            if (ctx.roads_loaded && ctx.route->ok()) {
                FrenetPos fp;
                if (ctx.roads->world_to_frenet(e.x, e.y, fp)) {
                    e.road_id = fp.road_id;
                    e.lane_id = fp.lane_id;
                    e.s       = fp.s;
                    e.offset  = offset_from_lane_internal(
                                    *ctx.roads, fp.road_id, fp.lane_id, fp.s, fp.offset);
                    e.target_offset = e.offset;
                }
                if (e.road_id >= 0) {
                    npc_init_route(e, *ctx.route, 1);
                    if (!e.road_pos.init(*ctx.roads, e.road_id, e.lane_id, e.s, 0.0)) {
                        if (spawned < 2) {
                            LOG_WARN("flowsim",
                                     "traffic_density: NPC id=%d road_pos.init failed",
                                     (int)e.scenario_id);
                        }
                    }
                }
            }
            if (!on_central) (*ctx.npc_route_spawned)++;
            spawned++;
        }
    }
    return spawned;
}

void step_poisson_traffic(TrafficSpawnerContext& ctx,
                          double dt, double ego_route_s, double sim_time_s) {
    if (!ctx.enable_poisson_traffic || !ctx.roads_loaded || !ctx.roads ||
        !ctx.route || !ctx.route->ok() || !ctx.pool || !ctx.sim_rng) {
        return;
    }

    const Entity& ego = (*ctx.pool)[0];
    if (!ego.active) return;

    const double route_total = ctx.route->total_length();
    if (route_total < 80.0) return;

    /* 1. 超距与进入泊车围栏的动态车辆淘汰 */
    int active_dynamic_count = 0;
    for (int i = 1; i < ctx.pool->size(); ++i) {
        Entity& e = (*ctx.pool)[i];
        if (!e.active || !e.is_dynamic_traffic) continue;
        active_dynamic_count++;

        if (is_near_surface_lot_apron(ctx.scenario, e.x, e.y)) {
            ctx.pool->free(e.id);
            active_dynamic_count--;
            continue;
        }

        double dx = e.x - ego.x;
        double dy = e.y - ego.y;
        double dist_sq = dx * dx + dy * dy;
        if (dist_sq > 220.0 * 220.0) {
            ctx.pool->free(e.id);
            active_dynamic_count--;
        }
    }

    if (is_in_protected_parking_zone(ctx.scenario, ego.x, ego.y, ego.z)) return;

    /* 2. 检查配额与实体池余量 */
    if (active_dynamic_count >= ctx.poisson_max_vehicles) return;
    if (ctx.pool->active_count() >= MAX_ENTITIES - 10) return;

    /* 3. 泊松到达抽样 */
    double density_scale = 1.0;
    if (ctx.scenario) {
        const ScenarioTrafficDensity* td = &ctx.scenario->traffic_density;
        density_scale = traffic_density_combined_scale(
            td->time_windows, td->time_window_count, sim_time_s,
            td->zones, td->zone_count, ego_route_s);
    }
    double arrival_prob = ctx.poisson_rate * density_scale * dt;
    if (arrival_prob > 1.0) arrival_prob = 1.0;
    double sample = sim_rng_uniform01(ctx.sim_rng);
    if (sample >= arrival_prob) return;

    /* 4. 候选生成位置：75% 在 ego 前方 65-120m，25% 在 ego 后方 50-80m */
    double s_cand = 0.0;
    bool ahead = (sim_rng_below(ctx.sim_rng, 100) < 75);
    if (ahead) {
        double dist = 65.0 + (double)sim_rng_below(ctx.sim_rng, 55);
        s_cand = ego_route_s + dist;
    } else {
        double dist = 50.0 + (double)sim_rng_below(ctx.sim_rng, 30);
        s_cand = ego_route_s - dist;
    }

    if (s_cand < 15.0 || s_cand > route_total - 15.0) return;

    /* 5. 间距防重叠检查：距离任意活跃车辆 > 25m */
    for (int i = 0; i < ctx.pool->size(); ++i) {
        const Entity& o = (*ctx.pool)[i];
        if (!o.active || !o.is_vehicle()) continue;
        if (std::fabs(o.route_s - s_cand) < 25.0) return;
    }

    /* 6. 从路线反算 road_id 与局部坐标 */
    int rid = 0, ridx = -1;
    double s_local = 0.0;
    ctx.route->locate(s_cand, rid, s_local, ridx);
    WorldPos wp;
    if (!ctx.roads->frenet_to_world(rid, 0, s_local, 0.0, wp)) return;
    if (is_in_protected_parking_zone(ctx.scenario, wp.x, wp.y, wp.z)) return;

    /* 7. 分配实体并初始化 */
    EntityType et = EntityType::Car;
    int rtype = sim_rng_below(ctx.sim_rng, 10);
    if (rtype == 0) et = EntityType::Truck;
    else if (rtype <= 2) et = EntityType::SUV;

    EntityId id = ctx.pool->alloc(et);
    if (id == INVALID_ENTITY) return;

    Entity& e = (*ctx.pool)[id];
    e.is_dynamic_traffic = true;
    e.scenario_id = -1000 - id;
    e.x = wp.x;
    e.y = wp.y;
    e.z = wp.z;
    e.heading = wp.h;
    e.route_s = s_cand;
    e.route_dir = 1;
    e.last_teleport_cycle = ctx.cycle;

    FrenetPos fp;
    if (ctx.roads->world_to_frenet(wp.x, wp.y, fp)) {
        e.road_id = fp.road_id;
        e.lane_id = fp.lane_id;
        e.s = fp.s;
        e.offset = offset_from_lane_internal(
            *ctx.roads, fp.road_id, fp.lane_id, fp.s, fp.offset);
        e.target_offset = e.offset;
    } else {
        e.road_id = rid;
        e.lane_id = -1;
        e.s = s_local;
        e.offset = 0.0;
        e.target_offset = 0.0;
    }

    if (e.road_id >= 0) {
        e.road_pos.init(*ctx.roads, e.road_id, e.lane_id, e.s, 0.0);
    }

    double speed_var = 0.85 + 0.25 * sim_rng_uniform01(ctx.sim_rng);
    e.speed = ctx.target_speed * speed_var;
    e.vx = e.speed * std::cos(e.heading);
    e.vy = e.speed * std::sin(e.heading);
    e.target_vx = e.speed;
    e.state = NpcState::Cruise;
    e.turn_intent = (sim_rng_below(ctx.sim_rng, 10) < 6)
                    ? 0 : (sim_rng_below(ctx.sim_rng, 2) ? 1 : 2);

    apply_vehicle_defaults(e);
}

}  // namespace flowsim
