#include "flowsim/npc_yield.h"
#include "flowsim/offroute_continue.h"
#include "traffic_light.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                    \
        }                                                                    \
    } while (0)

namespace {

flowsim::Entity& add_veh(flowsim::EntityPool& pool,
                         double x, double y, double h, double v) {
    flowsim::EntityId id = pool.alloc(flowsim::EntityType::Car);
    flowsim::Entity& e = pool[id];
    e.x = x;
    e.y = y;
    e.heading = h;
    e.speed = v;
    e.target_vx = v;
    return e;
}

void test_crossing_yields_to_ego() {
    flowsim::EntityPool pool;
    // Ego (id=0) heading +X at (0, 0), v=10 m/s -> arrives at (20, 0) in 2.0s
    add_veh(pool, 0.0, 0.0, 0.0, 10.0);
    // NPC (id=1) heading +Y at (20, -20), v=10 m/s -> arrives at (20, 0) in 2.0s
    const auto& npc = add_veh(pool, 20.0, -20.0, 1.57079632679, 10.0);

    auto res = flowsim::evaluate_crossing_conflict_yield(npc, pool);
    CHECK(res.should_yield);
    CHECK(res.yield_to == 0);
    CHECK(res.cpa_dist_m < 0.5);
    CHECK(res.speed_cap < 10.0);
}

void test_parallel_same_lane_does_not_trigger_crossing_yield() {
    flowsim::EntityPool pool;
    add_veh(pool, 20.0, 0.0, 0.0, 10.0);
    const auto& npc = add_veh(pool, 0.0, 0.0, 0.0, 10.0);

    auto res = flowsim::evaluate_crossing_conflict_yield(npc, pool);
    CHECK(!res.should_yield);
}

void test_earlier_npc_does_not_yield_to_later_npc() {
    flowsim::EntityPool pool;
    // id=0 dummy parked far away
    add_veh(pool, 1000.0, 1000.0, 0.0, 0.0);
    // NPC 1 is 14m from intersection (1.4s away), NPC 2 is 17m away (1.7s away)
    const auto& npc1 = add_veh(pool, 0.0, -14.0, 1.57079632679, 10.0);
    const auto& npc2 = add_veh(pool, -17.0, 0.0, 0.0, 10.0);

    auto res1 = flowsim::evaluate_crossing_conflict_yield(npc1, pool);
    auto res2 = flowsim::evaluate_crossing_conflict_yield(npc2, pool);
    CHECK(!res1.should_yield);
    CHECK(res2.should_yield);
    CHECK(res2.yield_to == 1);
}

void test_npc_yields_to_crosswalk_pedestrian() {
    flowsim::EntityPool pool;
    add_veh(pool, 1000.0, 1000.0, 0.0, 0.0);
    const auto& npc = add_veh(pool, 0.0, -1.75, 0.0, 8.0);
    flowsim::EntityId pid = pool.alloc(flowsim::EntityType::Pedestrian);
    flowsim::Entity& ped = pool[pid];
    ped.x = 12.0;
    ped.y = -4.0;
    ped.vx = 0.0;
    ped.vy = 1.4;
    ped.speed = 1.4;

    auto res = flowsim::evaluate_crossing_conflict_yield(npc, pool);
    CHECK(res.should_yield);
    CHECK(res.yield_to == pid);
    CHECK(res.speed_cap < 8.0);
}

void test_left_turn_arrow_signal_contract() {
    const char* json =
        "{\"lights\":["
        "{\"x\":100,\"y\":-1.75,\"lane_offset\":-1.75,\"approach_heading\":0.0,"
        "\"state\":\"green\",\"has_left_arrow\":true,\"left_state\":\"red\"},"
        "{\"x\":120,\"y\":-18.0,\"lane_offset\":-1.75,\"approach_heading\":1.57079632679,"
        "\"state\":\"red\",\"has_left_arrow\":false}"
        "]}";
    TrafficLightCache c;
    traffic_lights_parse(json, &c);
    CHECK(c.valid == 1 && c.count == 2);
    CHECK(c.has_left_arrow[0] == 1);
    CHECK(c.state[0] == TL_GREEN);
    CHECK(c.left_state[0] == TL_RED);
    CHECK(c.has_approach_heading[0] == 1);

    // 东向主线与信号 0 对齐，拒绝横向北行信号 1
    CHECK(traffic_light_matches_route_tangent(
              c.approach_heading[0], c.has_approach_heading[0], c.lane_offset[0],
              0, 0.0, 1.75, 7.0) == 1);
    CHECK(traffic_light_matches_route_tangent(
              c.approach_heading[1], c.has_approach_heading[1], c.lane_offset[1],
              0, 0.0, 18.0, 7.0) == 0);

    // 左转轨迹识别 + 生效相位切换（左转走 left_state=RED，直行走 state=GREEN）
    const double rx[5] = {80.0, 95.0, 108.0, 118.0, 120.0};
    const double ry[5] = {0.0, 0.0, 3.0, 15.0, 35.0};
    const double rs[5] = {0.0, 15.0, 28.34, 43.96, 64.06};
    const int is_left = traffic_light_detect_left_turn_ahead(rx, ry, rs, 5, 20.0, 8.0, 48.0);
    CHECK(is_left == 1);
    CHECK(traffic_light_effective_state(c.state[0], c.has_left_arrow[0], c.left_state[0], is_left) == TL_RED);
    CHECK(traffic_light_effective_state(c.state[0], c.has_left_arrow[0], c.left_state[0], 0) == TL_GREEN);

    // regression: 5m 间隔 road/ref_path 从 ego 起点发布时 ref_s ≈ 0.0，首段中点 mid_s = 2.5 > ref_s + 2.0
    double rs5[21], rx5[21], ry5[21];
    for (int i = 0; i < 21; i++) {
        rs5[i] = i * 5.0;
        if (i <= 10) {
            rx5[i] = 510.0 + i * 5.0;
            ry5[i] = 0.0;
        } else {
            rx5[i] = 560.0;
            ry5[i] = (i - 10) * 5.0;
        }
    }
    CHECK(traffic_light_detect_left_turn_ahead(rx5, ry5, rs5, 21, 0.0, 6.0, 90.0) == 1);
}

void test_offroute_continue_helpers() {
    flowsim::RouteEndpointCand cands[3] = {};
    // seg 0: virtual junction polyline (must be skipped)
    cands[0].route_idx = 0;
    cands[0].road_id = -1;
    cands[0].is_virtual = true;
    cands[0].length = 30.0;
    cands[0].start_x = 560.0; cands[0].start_y = 0.0; cands[0].start_h = 1.5707963;
    // seg 1: real road starting at (560, 0) heading +Y (northbound)
    cands[1].route_idx = 1;
    cands[1].road_id = 3;
    cands[1].s0 = 2.0;
    cands[1].length = 200.0;
    cands[1].start_x = 560.0; cands[1].start_y = 0.0; cands[1].start_h = 1.5707963;
    cands[1].end_x = 560.0;   cands[1].end_y = 200.0; cands[1].end_h = 1.5707963;
    // seg 2: real road ending at (540, 0) heading +X (eastbound)
    cands[2].route_idx = 2;
    cands[2].road_id = 2;
    cands[2].s0 = 0.0;
    cands[2].length = 100.0;
    cands[2].start_x = 440.0; cands[2].start_y = 0.0; cands[2].start_h = 0.0;
    cands[2].end_x = 540.0;   cands[2].end_y = 0.0; cands[2].end_h = 0.0;

    // 1. Forward continuation onto seg 1 start
    auto m_fwd = flowsim::select_offroute_route_continuation(
        561.5, -2.0, 0.0, 1.57, cands, 3);
    CHECK(m_fwd.matched && m_fwd.road_id == 3 && m_fwd.route_dir == 1);
    CHECK(std::fabs(m_fwd.s_local - 2.5) < 1e-6);

    // 2. Oncoming continuation onto seg 2 end (heading west PI against +X end)
    auto m_rev = flowsim::select_offroute_route_continuation(
        541.5, 1.75, 0.0, 3.14, cands, 3);
    CHECK(m_rev.matched && m_rev.road_id == 2 && m_rev.route_dir == -1);
    CHECK(std::fabs(m_rev.s_local - 99.5) < 1e-6);

    // 3. Perpendicular approach at (540, 0) heading north does NOT falsely match seg 2 end
    auto m_perp = flowsim::select_offroute_route_continuation(
        540.0, -1.0, 0.0, 1.57, cands, 3);
    CHECK(!m_perp.matched);

    // 4. Elevation isolation prevents z=0 matching z=6 viaduct
    auto m_elev = flowsim::select_offroute_route_continuation(
        560.0, -1.0, 6.0, 1.57, cands, 3);
    CHECK(!m_elev.matched);

    // 5. Directional lane selection & 15m clearance check
    std::vector<int> lanes = {2, 1, -1, -2};
    CHECK(flowsim::pick_directional_lane(lanes, 2, -1) == -2);
    CHECK(flowsim::pick_directional_lane(lanes, 3, -1) == -1);
    CHECK(flowsim::pick_directional_lane(lanes, 1, 1) == 1);
    CHECK(flowsim::pick_directional_lane({-1, -2}, 1, 1) == 0);

    flowsim::EntityPool pool;
    add_veh(pool, 100.0, 0.0, 0.0, 5.0);
    CHECK(!flowsim::has_spawn_clearance(pool, 99, 110.0, 0.0, 0.0, 15.0));
    CHECK(flowsim::has_spawn_clearance(pool, 99, 120.0, 0.0, 0.0, 15.0));
    CHECK(flowsim::has_spawn_clearance(pool, 99, 110.0, 0.0, 6.0, 15.0));

    // 6. Stub loop scan yields multiple spaced offsets along stub roads
    auto cands_200 = flowsim::stub_loop_candidates(200.0);
    CHECK(cands_200.size() >= 4 && std::fabs(cands_200[0] - 2.0) < 1e-6 &&
          std::fabs(cands_200[1] - 14.0) < 1e-6);
    CHECK(flowsim::stub_loop_candidates(8.0).empty());
}

void test_surface_recycle_and_lane_change_safety() {
    // 1. Surface recycle guard never targets underground (< 240m) when surface starts at 240m
    CHECK(std::fabs(flowsim::adjust_recycle_target_for_surface(0.0, 1, 350.0, 3200.0, 240.0, 28) - 240.0) < 1e-6);
    const double ahead_when_in_garage =
        flowsim::adjust_recycle_target_for_surface(0.0, 1, 20.0, 3200.0, 240.0, 28);
    CHECK(ahead_when_in_garage >= 270.0 && ahead_when_in_garage <= 3180.0);
    CHECK(std::fabs(flowsim::adjust_recycle_target_for_surface(900.0, 1, 1000.0, 3200.0, 240.0, 28) - 900.0) < 1e-6);

    // 2. Target lane safety: blocks side-by-side, concurrent merge, fast follower, and ego;
    //    allows elevation-separated or well-spaced vehicles.
    flowsim::EntityPool pool;
    auto& ego = add_veh(pool, 100.0, -5.25, 0.0, 10.0);
    ego.route_dir = 0; ego.offset = -5.25; ego.length = 4.6;

    auto& npc = add_veh(pool, 150.0, -1.75, 0.0, 10.0);
    npc.route_dir = 1; npc.route_s = 150.0; npc.offset = -1.75; npc.length = 4.6;

    // Ego at x=100 is 50m behind npc -> changing into -5.25 is safe
    CHECK(flowsim::is_target_lane_safe_for_change(npc, -5.25, pool, 2.0));

    // Concurrent lane-changer from -8.75 to -5.25 beside npc at route_s=154.0 -> blocked
    auto& other = add_veh(pool, 154.0, -8.75, 0.0, 10.0);
    other.route_dir = 1; other.route_s = 154.0; other.offset = -8.75; other.length = 4.6;
    other.state = flowsim::NpcState::LaneChange; other.target_offset = -5.25;
    CHECK(!flowsim::is_target_lane_safe_for_change(npc, -5.25, pool, 2.0));

    // Move concurrent lane-changer onto viaduct z=6.0 -> elevation-isolated, safe
    other.z = 6.0;
    CHECK(flowsim::is_target_lane_safe_for_change(npc, -5.25, pool, 2.0));

    // Side-by-side exact ahead==0 in target lane -> blocked
    other.z = 0.0; other.state = flowsim::NpcState::Cruise;
    other.offset = -5.25; other.target_offset = -5.25; other.route_s = 150.0;
    CHECK(!flowsim::is_target_lane_safe_for_change(npc, -5.25, pool, 2.0));

    // Move ego right alongside target lane at (149, -5.25) -> blocked even with route_dir==0
    other.active = false;
    ego.x = 149.0; ego.y = -5.25;
    CHECK(!flowsim::is_target_lane_safe_for_change(npc, -5.25, pool, 2.0));

    // 3. One-way 2-lane ramps {-1, -2} retain 2 same-dir lanes and [-7.6, +0.6] guardrail
    const std::vector<int> oneway2 = {-1, -2};
    CHECK(flowsim::count_same_dir_lanes(oneway2, 1) == 2);
    double min_t = 0.0, max_t = 0.0;
    flowsim::compute_guardrail_bounds(oneway2, 3.5, 0.6, min_t, max_t);
    CHECK(std::fabs(min_t - (-7.6)) < 1e-6 && std::fabs(max_t - 0.6) < 1e-6);

    const std::vector<int> twoway4 = {-1, -2, 1, 2};
    CHECK(flowsim::count_same_dir_lanes(twoway4, 1) == 2);
    CHECK(flowsim::count_same_dir_lanes(twoway4, -1) == 2);
    flowsim::compute_guardrail_bounds(twoway4, 3.5, 0.6, min_t, max_t);
    CHECK(std::fabs(min_t - (-7.6)) < 1e-6 && std::fabs(max_t - 7.6) < 1e-6);
}

static void test_guardrail_clamp_lane_drop_and_pedestrian_yield() {
    // 1) Guardrail trigger epsilon + inset avoids floating-point re-trigger lock at -7.60 / -4.10
    auto ok_edge = flowsim::compute_guardrail_clamp_action(-7.6001, -7.6, 7.6);
    CHECK(!ok_edge.triggered);
    auto over = flowsim::compute_guardrail_clamp_action(-7.68, -7.6, 7.6);
    CHECK(over.triggered && over.clamped_t > -7.6 && over.clamped_t <= -7.50);
    CHECK(!flowsim::compute_guardrail_clamp_action(over.clamped_t, -7.6, 7.6).triggered);

    // 2) Upstream lane-drop lookahead begins inward merge before entering 1-lane road 7
    CHECK(flowsim::compute_eff_lane_drop_same_dir_lanes(2, 1, 80.0, 65.0) == 2);
    CHECK(flowsim::compute_eff_lane_drop_same_dir_lanes(2, 1, 50.0, 65.0) == 1);

    // 3) Pedestrian at crosswalk must wait when stepping closer into a queued vehicle
    flowsim::EntityPool pool;
    auto& car = add_veh(pool, 578.5, 0.0, M_PI, 0.0);
    car.z = 0.0;

    flowsim::EntityId pid = pool.alloc(flowsim::EntityType::Pedestrian);
    flowsim::Entity& ped = pool[pid];
    ped.active = true;
    ped.x = 578.5;
    ped.y = 1.8;
    ped.z = 0.0;
    ped.length = 0.5;
    ped.width = 0.5;
    CHECK(flowsim::is_pedestrian_step_blocked_by_vehicle(ped, 578.5, 1.6, pool));
    CHECK(!flowsim::is_pedestrian_step_blocked_by_vehicle(ped, 578.5, 2.0, pool));

    ped.z = -4.5;
    CHECK(!flowsim::is_pedestrian_step_blocked_by_vehicle(ped, 578.5, 1.6, pool));

    // 4) Cross-street / off-route vehicles with road_pos stay road-tracked cleanly
    CHECK(flowsim::is_road_tracked_vehicle(30, 0, true));
    CHECK(flowsim::is_road_tracked_vehicle(2, 1, false));
    CHECK(!flowsim::is_road_tracked_vehicle(-1, 0, false));
    CHECK(!flowsim::is_road_tracked_vehicle(30, 0, false));
    const auto own_proj = flowsim::project_onto_road_tangent(
        561.75, -8.5, 0.0, 560.0, -8.5, 0.0, M_PI / 2.0);
    CHECK(own_proj.valid);
    CHECK(std::fabs(own_proj.along) < 1e-6);
    CHECK(std::fabs(own_proj.lat - (-1.75)) < 1e-6);

    // 5) Corridor intrusion yield during perpendicular parking swing-out:
    // Eastbound lane -1 NPC sees ego swung at y=+1.56 (lane_id=+1, h=0.92),
    // westbound lane +1 NPC sees ego via negative-X projection, and once ego
    // parks inside bay y=-6.48 the road lane resumes without obstruction.
    {
        flowsim::EntityPool cpool;
        auto& ego = add_veh(cpool, 3226.5, 1.56, 0.92, 0.9);
        ego.length = 4.6; ego.width = 2.0; ego.road_id = 8; ego.lane_id = 1; ego.offset = 1.56;

        auto& eb_npc = add_veh(cpool, 3219.0, -1.75, 0.0, 6.0);
        eb_npc.length = 4.6; eb_npc.width = 1.9; eb_npc.road_id = 8; eb_npc.lane_id = -1;
        eb_npc.route_dir = 1; eb_npc.offset = -1.75;

        double gap = 1e9;
        CHECK(flowsim::compute_corridor_lead_gap(eb_npc, ego, false, true, 60.0, gap));
        CHECK(gap > 2.5 && gap < 4.0);

        auto& wb_npc = add_veh(cpool, 3234.0, 1.75, M_PI, 6.0);
        wb_npc.length = 4.6; wb_npc.width = 1.9; wb_npc.road_id = 8; wb_npc.lane_id = 1;
        wb_npc.route_dir = -1; wb_npc.offset = 1.75;
        double wb_gap = 1e9;
        CHECK(flowsim::compute_corridor_lead_gap(wb_npc, ego, true, true, 60.0, wb_gap));
        CHECK(wb_gap > 2.5 && wb_gap < 4.0);

        // Once ego finishes backing into bay (y=-6.48, h=1.53), lane -1 is unobstructed
        ego.x = 3225.6; ego.y = -6.48; ego.heading = 1.53; ego.speed = 0.0; ego.offset = -6.48;
        double parked_gap = 1e9;
        CHECK(!flowsim::compute_corridor_lead_gap(eb_npc, ego, false, true, 60.0, parked_gap));

        // Adjacent parallel vehicle on y=+1.75 during normal cruise does not trigger intrusion
        ego.x = 3226.5; ego.y = 1.75; ego.heading = 0.0; ego.speed = 8.0; ego.offset = 1.75;
        double adj_gap = 1e9;
        CHECK(!flowsim::compute_corridor_lead_gap(eb_npc, ego, false, false, 60.0, adj_gap));
    }
}

void test_phantom_route_lead_and_low_speed_zipper() {
    flowsim::EntityPool pool;
    flowsim::EntityId npc_id = pool.alloc(flowsim::EntityType::Car);
    flowsim::Entity& npc = pool[npc_id];
    npc.active = true;
    npc.id = 74;
    npc.x = 562.4;
    npc.y = 150.4;
    npc.z = 0.0;
    npc.heading = 1.51;
    npc.speed = 0.0;
    npc.length = 4.6;
    npc.width = 1.9;
    npc.route_dir = 1;
    npc.route_s = 700.0;
    npc.offset = -1.75;

    // Phantom off-route car: nearby route_s (+5m) but 120m away in 3D world space
    flowsim::EntityId ph_id = pool.alloc(flowsim::EntityType::Car);
    flowsim::Entity& phantom = pool[ph_id];
    phantom.active = true;
    phantom.id = 88;
    phantom.x = 680.0;
    phantom.y = 10.0;
    phantom.z = 0.0;
    phantom.heading = 0.0;
    phantom.speed = 0.0;
    phantom.length = 4.6;
    phantom.width = 1.9;
    phantom.route_dir = 1;
    phantom.route_s = 705.0;
    phantom.offset = -1.75;

    double gap = 1e9;
    CHECK(!flowsim::compute_corridor_lead_gap(npc, phantom, true, false, 60.0, gap));
    CHECK(flowsim::is_target_lane_safe_for_change(npc, -1.75, pool, 1.5, 6.0, 6.0));

    // Low-speed zipper merge: real vehicle 9.6m ahead (5.0m bumper gap) at <3 m/s
    phantom.x = 562.9;
    phantom.y = 160.0;
    phantom.route_s = 709.6;
    phantom.speed = 0.5;
    npc.speed = 0.5;
    CHECK(flowsim::is_target_lane_safe_for_change(npc, -1.75, pool, 1.5, 6.0, 6.0));
}

}  // namespace

int main() {
    test_crossing_yields_to_ego();
    test_parallel_same_lane_does_not_trigger_crossing_yield();
    test_earlier_npc_does_not_yield_to_later_npc();
    test_npc_yields_to_crosswalk_pedestrian();
    test_left_turn_arrow_signal_contract();
    test_offroute_continue_helpers();
    test_surface_recycle_and_lane_change_safety();
    test_guardrail_clamp_lane_drop_and_pedestrian_yield();
    test_phantom_route_lead_and_low_speed_zipper();
    std::printf("test_npc_yield: all 9 tests passed\n");
    return 0;
}
