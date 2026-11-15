/**
 * test_jolt_physics.cpp — Headless 3D Jolt vehicle physics unit & invariant tests (Phase 6 P6-1..P6-6)
 */

#include "flowsim/physics_world.h"
#include "flowsim/entity.h"
#include "scenario_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define CHECK(cond, fmt, ...)                                                      \
    do {                                                                           \
        ++g_tests_run;                                                             \
        if (!(cond)) {                                                             \
            ++g_tests_failed;                                                      \
            std::fprintf(stderr, "  [FAIL] %s:%d (%s): " fmt "\n",                 \
                         __FILE__, __LINE__, #cond, ##__VA_ARGS__);                \
        } else {                                                                   \
            std::printf("  [PASS] " fmt "\n", ##__VA_ARGS__);                      \
        }                                                                          \
    } while (0)

static flowsim::Entity make_ego(double x, double y, double z,
                                double heading = 0.0, double speed = 0.0) {
    flowsim::Entity e;
    e.active = true;
    e.id = 0;
    e.type = flowsim::EntityType::Ego;
    e.length = 4.6;
    e.width = 2.0;
    e.height = 1.5;
    e.wheelbase = 2.7;
    e.mass = 1500.0;
    e.cg_height = 0.55;
    e.yaw_inertia = 2250.0;
    e.drag_coeff = 0.30;
    e.pacejka_b = 10.0;
    e.pacejka_c = 1.9;
    e.pacejka_e = 0.97;
    e.pacejka_mu = 0.95;
    e.surface_mu_scale = 1.0;
    e.x = x;
    e.y = y;
    e.z = z;
    e.heading = heading;
    e.speed = speed;
    e.vx = speed * std::cos(heading);
    e.vy = speed * std::sin(heading);
    return e;
}

/* 1. Gravity drop, elevated viaduct deck support, static weight split, and free-fall */
static void test_gravity_and_elevated_deck() {
    std::printf("\n=== [Test 1] Gravity Settling, Viaduct Deck & Free-Fall ===\n");
    flowsim::PhysicsWorld pw;
    CHECK(pw.ready(), "PhysicsWorld initialized cleanly");

    // Flat ground slab [-30, 80] x [-20, 20] at z=0.0, plus elevated viaduct deck at z=6.5
    pw.add_ground_slab(25.0, 0.0, 0.0, 55.0, 20.0, 1.0);
    pw.add_ground_slab(160.0, 0.0, 6.5, 25.0, 6.0, 1.0);

    // Drop ego from z=0.50m onto z=0.0m slab
    flowsim::Entity ego = make_ego(0.0, 0.0, 0.50, 0.0, 0.0);
    CHECK(pw.register_ego(ego, true), "Ego registered on Jolt world");
    pw.set_vehicle_input(0, 0.0, 1.0, 0.0, 1, 1.0);

    for (int i = 0; i < 90; ++i) pw.step(1.0 / 60.0);

    flowsim::VehiclePhysicsTelemetry tel;
    CHECK(pw.get_vehicle_telemetry(0, tel), "Telemetry read back after drop");
    CHECK(std::fabs(tel.z - 0.0) < 0.06,
          "Settled z near ground datum 0.0 (got %.4f m)", tel.z);
    CHECK(std::fabs(tel.vz) < 0.15, "Settled vz near 0 (got %.4f m/s)", tel.vz);
    bool all_grounded = tel.wheels[0].grounded && tel.wheels[1].grounded &&
                        tel.wheels[2].grounded && tel.wheels[3].grounded;
    CHECK(all_grounded, "All 4 wheels report ground contact");
    double fz_total = tel.F_zf + tel.F_zr;
    double mg = ego.mass * 9.81;
    CHECK(std::fabs(fz_total - mg) < 0.08 * mg,
          "Static tire normal sum Fzf+Fzr=%.1f N matches m*g=%.1f N", fz_total, mg);
    CHECK(tel.F_zf > tel.F_zr,
          "Front-biased CG carries higher static axle load (Fzf=%.1f > Fzr=%.1f)",
          tel.F_zf, tel.F_zr);

    // Move onto z=+6.5m viaduct deck (drop from 6.75m)
    pw.set_vehicle_pose(0, 160.0, 0.0, 6.75, 0.0, 0.0);
    for (int i = 0; i < 60; ++i) pw.step(1.0 / 60.0);
    pw.get_vehicle_telemetry(0, tel);
    CHECK(std::fabs(tel.z - 6.5) < 0.08,
          "Viaduct deck supports ego at z=6.5m (got %.4f m)", tel.z);

    // Move off the deck edge into empty air at (260, 0, 6.5) -> genuine gravity free-fall
    pw.set_vehicle_input(0, 0.0, 0.0, 0.0, 1, 1.0);
    pw.set_vehicle_pose(0, 260.0, 0.0, 6.5, 0.0, 5.0);
    for (int i = 0; i < 60; ++i) pw.step(1.0 / 60.0);
    pw.get_vehicle_telemetry(0, tel);
    bool any_grounded = tel.wheels[0].grounded || tel.wheels[1].grounded ||
                        tel.wheels[2].grounded || tel.wheels[3].grounded;
    CHECK(!any_grounded && tel.z < 2.5 && tel.vz < -7.0,
          "Driving off elevated deck enters true free-fall (z=%.2f, vz=%.2f)", tel.z, tel.vz);
}

/* 2. 6-DOF Steering Sign (Left=+), Roll Outward Lean, Squat (Accel), Dive (Brake) */
static void test_6dof_steering_and_load_transfer() {
    std::printf("\n=== [Test 2] 6-DOF Steering, Body Roll, Squat & Dive ===\n");
    flowsim::PhysicsWorld pw;
    pw.add_ground_slab(100.0, 0.0, 0.0, 200.0, 200.0, 1.0);

    // A) Steering left (+0.25 rad)
    flowsim::Entity ego = make_ego(0.0, 0.0, 0.02, 0.0, 10.0);
    pw.register_ego(ego, true);
    for (int i = 0; i < 10; ++i) pw.step(1.0 / 60.0);  // settle suspension
    pw.set_vehicle_pose(0, 0.0, 0.0, 0.0, 0.0, 12.0);
    pw.set_vehicle_input(0, 0.35, 0.0, +0.25, 1, 1.0);
    double max_roll = -1.0;
    flowsim::VehiclePhysicsTelemetry tel;
    for (int i = 0; i < 45; ++i) {
        pw.step(1.0 / 60.0);
        pw.get_vehicle_telemetry(0, tel);
        max_roll = std::max(max_roll, tel.roll);
    }
    double fz_left = tel.wheels[0].normal_force + tel.wheels[2].normal_force;
    double fz_right = tel.wheels[1].normal_force + tel.wheels[3].normal_force;
    CHECK(tel.heading > 0.20 && tel.y > 0.5,
          "Positive steer turns ENU LEFT (heading=%.3f rad, y=%.2f m)", tel.heading, tel.y);
    CHECK(max_roll > 0.004 && fz_right > fz_left,
          "Left cornering produces outward right roll (roll=%.4f) & outer load transfer (R=%.0f > L=%.0f)",
          max_roll, fz_right, fz_left);

    // B) Hard acceleration -> rear squat (pitch < 0) & rearward weight transfer
    pw.set_vehicle_pose(0, 0.0, 0.0, 0.0, 0.0, 0.0);
    pw.set_vehicle_input(0, 0.0, 1.0, 0.0, 1, 1.0);
    for (int i = 0; i < 30; ++i) pw.step(1.0 / 60.0);
    pw.get_vehicle_telemetry(0, tel);
    double static_fzr = tel.F_zr;

    pw.set_vehicle_input(0, 1.0, 0.0, 0.0, 1, 1.0);
    double min_pitch = 1.0, max_ax = -1.0, max_fzr = 0.0;
    for (int i = 0; i < 30; ++i) {
        pw.step(1.0 / 60.0);
        pw.get_vehicle_telemetry(0, tel);
        min_pitch = std::min(min_pitch, tel.pitch);
        max_ax = std::max(max_ax, tel.a_x_body);
        max_fzr = std::max(max_fzr, tel.F_zr);
    }
    CHECK(max_ax > 2.0 && min_pitch < -0.003 && max_fzr > static_fzr + 200.0,
          "Full throttle squats rear (pitch=%.4f < 0, ax=%.2f, Fzr=%.0f > static %.0f)",
          min_pitch, max_ax, max_fzr, static_fzr);

    // C) Hard braking -> nose dive (pitch > 0) & forward weight transfer
    pw.set_vehicle_pose(0, 0.0, 0.0, 0.0, 0.0, 18.0);
    pw.set_vehicle_input(0, 0.0, 1.0, 0.0, 1, 1.0);
    double max_pitch = -1.0, min_ax = 1.0, max_fzf = 0.0;
    for (int i = 0; i < 30; ++i) {
        pw.step(1.0 / 60.0);
        pw.get_vehicle_telemetry(0, tel);
        max_pitch = std::max(max_pitch, tel.pitch);
        min_ax = std::min(min_ax, tel.a_x_body);
        max_fzf = std::max(max_fzf, tel.F_zf);
    }
    CHECK(min_ax < -4.5 && max_pitch > 0.006 && max_fzf > tel.F_zr,
          "Hard braking pitches nose down (pitch=%.4f > 0, ax=%.2f, Fzf=%.0f)",
          max_pitch, min_ax, max_fzf);

    // D) Reverse gear -> accelerates backward with negative signed speed
    pw.set_vehicle_pose(0, 10.0, 0.0, 0.0, 0.0, 0.0);
    pw.set_vehicle_input(0, 0.85, 0.0, 0.0, -1, 1.0);
    for (int i = 0; i < 60; ++i) pw.step(1.0 / 60.0);
    pw.get_vehicle_telemetry(0, tel);
    CHECK(tel.speed < -2.0 && tel.x < 9.0,
          "Reverse gear accelerates backward (speed=%.2f m/s, x=%.2f m)", tel.speed, tel.x);
}

static double measure_stop_distance(double mu_scale) {
    flowsim::PhysicsWorld pw;
    pw.add_ground_slab(150.0, 0.0, 0.0, 200.0, 25.0, 1.0);
    if (mu_scale < 0.99) {
        pw.set_friction_zones({{-50.0, 350.0, -25.0, 25.0, mu_scale}});
    }
    flowsim::Entity ego = make_ego(0.0, 0.0, 0.0, 0.0, 18.0);
    pw.register_ego(ego, true);
    pw.set_vehicle_pose(0, 0.0, 0.0, 0.0, 0.0, 18.0);
    pw.set_vehicle_input(0, 0.0, 1.0, 0.0, 1, 1.0);

    flowsim::VehiclePhysicsTelemetry tel;
    for (int i = 0; i < 420; ++i) {
        pw.step(1.0 / 60.0);
        pw.get_vehicle_telemetry(0, tel);
        if (std::fabs(tel.speed) < 0.15) break;
    }
    return tel.x;
}

/* 3. Combined-slip Pacejka friction, wet/ice braking, split-mu pull, and 15% slope */
static void test_pacejka_friction_and_slopes() {
    std::printf("\n=== [Test 3] Friction Zones, Split-Mu & 15%% Slope Hold/Rollback ===\n");
    double d_dry = measure_stop_distance(1.0);
    double d_ice = measure_stop_distance(0.30);
    CHECK(d_dry > 10.0 && d_ice > d_dry * 1.5,
          "Ice braking distance (%.1f m @ mu=0.30) > 1.5x dry (%.1f m @ mu=1.0)",
          d_ice, d_dry);

    // Split-mu: left wheels (y > 0) on dry (mu=1.0), right wheels (y <= 0) on ice (mu=0.20)
    {
        flowsim::PhysicsWorld pw;
        pw.add_ground_slab(100.0, 0.0, 0.0, 150.0, 30.0, 1.0);
        pw.set_friction_zones({{-50.0, 250.0, -30.0, 0.0, 0.20}});
        flowsim::Entity ego = make_ego(0.0, 0.0, 0.0, 0.0, 16.0);
        pw.register_ego(ego, true);
        pw.set_vehicle_pose(0, 0.0, 0.0, 0.0, 0.0, 16.0);
        pw.set_vehicle_input(0, 0.0, 1.0, 0.0, 1, 1.0);
        flowsim::VehiclePhysicsTelemetry tel;
        for (int i = 0; i < 60; ++i) pw.step(1.0 / 60.0);
        pw.get_vehicle_telemetry(0, tel);
        CHECK(tel.heading > 0.02,
              "Split-mu braking pulls toward high-mu left side (heading=%.4f rad)", tel.heading);
    }

    // 15% Uphill Ramp: flat entry [-10..0, z=0] -> 15% ramp [0..60, z=0..9.0]
    {
        flowsim::PhysicsWorld pw;
        pw.add_ramp_surface(-15.0, 0.0, 0.0, 0.0, 0.0, 0.0, 12.0, 1.0);
        pw.add_ramp_surface(0.0, 0.0, 0.0, 60.0, 0.0, 9.0, 12.0, 1.0);

        // Spawn flat at x=-8, climb onto the 15% ramp
        flowsim::Entity ego = make_ego(-8.0, 0.0, 0.0, 0.0, 8.0);
        pw.register_ego(ego, true);
        pw.set_vehicle_pose(0, -8.0, 0.0, 0.0, 0.0, 8.0);
        pw.set_vehicle_input(0, 0.85, 0.0, 0.0, 1, 1.0);
        flowsim::VehiclePhysicsTelemetry tel;
        for (int i = 0; i < 150; ++i) pw.step(1.0 / 60.0);
        pw.get_vehicle_telemetry(0, tel);
        CHECK(tel.x > 6.0 && tel.z > 0.9 && tel.pitch < -0.05,
              "Ego climbs onto 15%% ramp cleanly (x=%.2f m, z=%.2f m, pitch=%.3f rad)",
              tel.x, tel.z, tel.pitch);

        // Brake to full stop on the 15% grade and test zero-creep hill-hold
        pw.set_vehicle_input(0, 0.0, 1.0, 0.0, 1, 1.0);
        for (int i = 0; i < 120; ++i) pw.step(1.0 / 60.0);
        pw.get_vehicle_telemetry(0, tel);
        double x_hold0 = tel.x;
        for (int i = 0; i < 90; ++i) pw.step(1.0 / 60.0);
        pw.get_vehicle_telemetry(0, tel);
        double creep = std::fabs(tel.x - x_hold0);
        CHECK(creep < 0.15 && std::fabs(tel.speed) < 0.10,
              "Hill-hold on 15%% slope prevents creep (|dx|=%.4f m, speed=%.3f m/s)",
              creep, tel.speed);

        // Release brake & throttle -> true gravity rollback downhill
        pw.set_vehicle_input(0, 0.0, 0.0, 0.0, 1, 1.0);
        for (int i = 0; i < 75; ++i) pw.step(1.0 / 60.0);
        pw.get_vehicle_telemetry(0, tel);
        CHECK(tel.speed < -0.7 && tel.x < x_hold0 - 0.3,
              "Releasing brake on 15%% slope rolls back under gravity (v=%.2f m/s, dx=%.2f m)",
              tel.speed, tel.x - x_hold0);
    }
}

/* 4. 3D Static Colliders, Gates, Wheel Stops & B1 Underground Garage Pit Cutout */
static void test_static_obstacles_gates_and_garage_pit() {
    std::printf("\n=== [Test 4] 3D Static Obstacles, Gate Toggle & Underground Pit ===\n");

    // A) Building wall & cylinder tree impact + contact events
    {
        flowsim::PhysicsWorld pw;
        pw.add_ground_slab(40.0, 0.0, 0.0, 80.0, 20.0, 1.0);
        flowsim::StaticBoxObstacle tree;
        tree.x = 22.0; tree.y = 0.0; tree.z = 0.0;
        tree.len = 0.9; tree.wid = 0.9; tree.height = 5.5;
        tree.kind = "tree"; tree.is_cylinder = true;
        pw.add_static_obstacle(tree, 7);

        // Low 0.18m wheel stop along driving lane at x=10 -> chassis clearance 0.22m should pass over
        flowsim::StaticBoxObstacle wstop;
        wstop.x = 10.0; wstop.y = 0.0; wstop.z = 0.0;
        wstop.len = 0.25; wstop.wid = 2.2; wstop.height = 0.18;
        wstop.kind = "wheel_stop";
        pw.add_static_obstacle(wstop, 8);

        int gh = pw.add_gate_barrier(40.0, 0.0, 0.0, 0.5, 4.0, 1.4, 0.0, 9);

        flowsim::Entity ego = make_ego(0.0, 0.0, 0.0, 0.0, 8.0);
        pw.register_ego(ego, true);
        pw.set_vehicle_pose(0, 0.0, 0.0, 0.0, 0.0, 8.0);
        pw.set_vehicle_input(0, 0.3, 0.0, 0.0, 1, 1.0);

        std::vector<flowsim::PhysicsContactEvent> contacts;
        for (int i = 0; i < 180; ++i) {
            pw.step(1.0 / 60.0);
            auto evs = pw.consume_contacts();
            contacts.insert(contacts.end(), evs.begin(), evs.end());
        }
        flowsim::VehiclePhysicsTelemetry tel;
        pw.get_vehicle_telemetry(0, tel);
        bool hit_tree = false;
        for (const auto& c : contacts) {
            if (c.kind_b == "tree" && c.impact_speed > 2.0) hit_tree = true;
        }
        CHECK(tel.x < 21.0 && hit_tree,
              "Cylindrical tree stops ego before x=21 (x=%.2f m) and fires 'tree' contact",
              tel.x);

        // Test ETC gate barrier open vs closed
        pw.set_vehicle_pose(0, 30.0, 0.0, 0.0, 0.0, 6.0);
        pw.set_vehicle_input(0, 0.3, 0.0, 0.0, 1, 1.0);
        pw.consume_contacts();
        for (int i = 0; i < 120; ++i) pw.step(1.0 / 60.0);
        auto gate_hits = pw.consume_contacts();
        pw.get_vehicle_telemetry(0, tel);
        bool hit_gate = false;
        for (const auto& c : gate_hits) {
            if (c.kind_b == "gate") hit_gate = true;
        }
        CHECK(tel.x < 39.5 && hit_gate,
              "Closed ETC gate blocks ego before x=40 (x=%.2f m)", tel.x);

        // Open gate -> ego drives right past x=45
        pw.set_gate_open(gh, true);
        pw.set_vehicle_pose(0, 32.0, 0.0, 0.0, 0.0, 8.0);
        pw.set_vehicle_input(0, 0.4, 0.0, 0.0, 1, 1.0);
        for (int i = 0; i < 120; ++i) pw.step(1.0 / 60.0);
        auto open_hits = pw.consume_contacts();
        pw.get_vehicle_telemetry(0, tel);
        CHECK(tel.x > 44.0 && open_hits.empty(),
              "Open ETC gate lets ego drive past x=44m cleanly (x=%.2f m, contacts=%zu)",
              tel.x, open_hits.size());
    }

    // B) Underground B1 garage (z=-4.5) + open_ramp pit cutout climbing through z=0.0
    {
        const char* rn_json =
            R"({"edges":[)"
            R"({"id":1,"type":"garage_hall","nodes":[[2.0,-14.0,-4.5],[28.0,-14.0,-4.5]]},)"
            R"({"id":2,"type":"open_ramp","nodes":[[28.0,-8.0,-4.5],[66.0,-8.0,0.0]]})"
            R"(]})";
        ScenarioConfig sc;
        std::memset(&sc, 0, sizeof(sc));
        sc.road_network_json = const_cast<char*>(rn_json);
        sc.parking_slot_count = 1;
        sc.parking_slots[0].id = 1;
        sc.parking_slots[0].x = 12.0;
        sc.parking_slots[0].y = -17.3;
        sc.parking_slots[0].z = -4.5;
        sc.parking_slots[0].width = 2.6;
        sc.parking_slots[0].length = 5.4;

        flowsim::PhysicsWorld pw;
        std::vector<flowsim::BuildingOBB> empty_b;
        std::vector<flowsim::StaticBoxObstacle> empty_f;
        CHECK(pw.build_static_world(nullptr, &sc, empty_b, empty_f),
              "build_static_world succeeds for underground garage + open_ramp cutout");

        // Supply ramp road surface + top exit surface apron matching open_ramp -> surface junction
        pw.add_ramp_surface(27.0, -8.0, -4.5, 66.0, -8.0, 0.0, 7.2, 1.0);
        pw.add_ground_slab(76.0, -8.0, 0.0, 10.0, 6.0, 1.0);

        // Start inside B1 hall aligned toward the open ramp at (22, -8, -4.5)
        flowsim::Entity ego = make_ego(22.0, -8.0, -4.5, 0.0, 5.0);
        pw.register_ego(ego, true);
        pw.set_vehicle_pose(0, 22.0, -8.0, -4.5, 0.0, 5.0);
        pw.set_vehicle_input(0, 0.85, 0.0, 0.0, 1, 1.0);
        pw.consume_contacts();

        for (int i = 0; i < 300; ++i) pw.step(1.0 / 60.0);
        auto pit_contacts = pw.consume_contacts();
        flowsim::VehiclePhysicsTelemetry tel;
        pw.get_vehicle_telemetry(0, tel);
        CHECK(tel.x > 64.0 && std::fabs(tel.z - 0.0) < 0.25 && pit_contacts.empty(),
              "Vehicle climbs B1 z=-4.5 ramp through z=0 cutout onto surface (x=%.2f, z=%.3f)",
              tel.x, tel.z);
    }
}

/* 5. NPC Raycast Support & 1000-Step Real-Time Performance Budget (< 2.0 ms/step) */
static void test_npc_raycast_and_benchmark() {
    std::printf("\n=== [Test 5] NPC Raycast Support & 1000-Tick Step Benchmark ===\n");
    flowsim::PhysicsWorld pw;
    pw.add_ground_slab(25.0, 0.0, 5.0, 25.0, 15.0, 1.0);  // deck over x=[0..50] at z=5.0

    std::vector<flowsim::StaticBoxObstacle> obs;
    for (int k = 0; k < 60; ++k) {
        flowsim::StaticBoxObstacle o;
        o.x = 5.0 + k * 4.0;
        o.y = 12.0;
        o.z = 0.0;
        o.len = 0.6;
        o.wid = 0.6;
        o.height = 4.5;
        o.kind = (k % 2 == 0) ? "tree" : "pole";
        o.is_cylinder = true;
        pw.add_static_obstacle(o, k);
    }

    flowsim::EntityPool pool;
    pool.clear();
    flowsim::EntityId ego_id = pool.alloc(flowsim::EntityType::Ego);
    pool[ego_id] = make_ego(10.0, 0.0, 5.0, 0.0, 8.0);
    pw.register_ego(pool[ego_id], true);

    std::vector<int> npc_ids;
    for (int i = 0; i < 8; ++i) {
        flowsim::EntityId nid = pool.alloc(flowsim::EntityType::Car);
        flowsim::Entity& npc = pool[nid];
        npc.active = true;
        npc.x = (i == 7) ? 45.0 : (5.0 + i * 5.0);
        npc.y = (i % 2 == 0) ? -3.5 : 3.5;
        npc.z = 5.0;
        npc.speed = 4.0;
        npc.length = 4.5;
        npc.width = 1.9;
        npc.height = 1.5;
        npc_ids.push_back(nid);
    }
    pw.sync_npcs_from_pool(pool, 0.0);

    // Move NPC #7 past edge (x>50) -> loses deck support and drops under gravity
    std::vector<double> step_ms;
    step_ms.reserve(1000);
    const double dt = 1.0 / 60.0;
    for (int tick = 0; tick < 1000; ++tick) {
        // Keep ego cruising comfortably on deck
        if (tick % 180 == 0) {
            pw.set_vehicle_pose(0, 10.0, 0.0, 5.0, 0.0, 8.0);
        }
        pw.set_vehicle_input(0, 0.25, 0.0, 0.0, 1, 1.0);
        for (int idx : npc_ids) {
            pool[idx].x += 0.04;
            if (pool[idx].x > 46.0 && idx != npc_ids.back()) {
                pool[idx].x = 5.0;
                pool[idx].z = 5.0;
            }
        }
        pw.sync_npcs_from_pool(pool, dt);
        pw.step(dt);
        step_ms.push_back(pw.last_step_time_ms());
    }

    const auto& grounded_npc = pool[npc_ids[0]];
    const auto& fallen_npc = pool[npc_ids.back()];
    CHECK(std::fabs(grounded_npc.z - 5.0) < 0.05 && fallen_npc.z < -2.0 && fallen_npc.vz < -5.0,
          "NPC raycast holds deck height (z=%.2f) while off-edge NPC falls (z=%.2f, vz=%.2f)",
          grounded_npc.z, fallen_npc.z, fallen_npc.vz);

    std::vector<double> sorted = step_ms;
    std::sort(sorted.begin(), sorted.end());
    double mean_ms = std::accumulate(step_ms.begin(), step_ms.end(), 0.0) / (double)step_ms.size();
    double p95_ms = sorted[(size_t)(sorted.size() * 0.95)];
    CHECK(mean_ms < 2.0 && p95_ms < 2.0,
          "1000-tick step latency under 2.0ms budget (mean=%.4f ms, p95=%.4f ms, bodies=%d)",
          mean_ms, p95_ms, pw.body_count());
}

int main() {
    std::printf("Running Phase 6 Jolt 3D Vehicle Physics Suite...\n");
    test_gravity_and_elevated_deck();
    test_6dof_steering_and_load_transfer();
    test_pacejka_friction_and_slopes();
    test_static_obstacles_gates_and_garage_pit();
    test_npc_raycast_and_benchmark();
    std::printf("\nSummary: %d/%d checks passed (%d failed)\n",
                g_tests_run - g_tests_failed, g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
