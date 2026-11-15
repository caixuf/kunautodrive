/**
 * test_world_engine.cpp — Unit & integration tests for flowsim::WorldEngine + Jolt3D colliders
 */

#include "flowsim/world_engine.h"
#include "flowsim/entity.h"
#if defined(FLOW_HAS_JOLT) && FLOW_HAS_JOLT
#include "flowsim/physics_world.h"
#endif

#include <cmath>
#include <cstdio>
#include <vector>

static int g_run = 0;
static int g_fail = 0;

#define CHECK(cond, fmt, ...)                                                      \
    do {                                                                           \
        ++g_run;                                                                   \
        if (!(cond)) {                                                             \
            ++g_fail;                                                              \
            std::fprintf(stderr, "  [FAIL] %s:%d (%s): " fmt "\n",                 \
                         __FILE__, __LINE__, #cond, ##__VA_ARGS__);                \
        } else {                                                                   \
            std::printf("  [PASS] " fmt "\n", ##__VA_ARGS__);                      \
        }                                                                          \
    } while (0)

static void test_procedural_corridor_and_overpass() {
    std::printf("\n=== [Test 1] Procedural Corridor, Curve & Overpass Pier Straddling ===\n");
    const char* rn = R"({
      "edges": [
        {
          "id": 0, "name": "avenue_west", "type": "urban",
          "lanes": 4, "lane_width": 3.5, "oneway": false, "speed_limit": 16.67,
          "nodes": [[0, 0, 0], [180, 0, 0]]
        },
        {
          "id": 1, "name": "avenue_east", "type": "urban",
          "lanes": 4, "lane_width": 3.5, "oneway": false, "speed_limit": 13.89,
          "nodes": [[180, 0, 0], [420, 40, 0], [600, 120, 0]]
        },
        {
          "id": 2, "name": "cross_north", "type": "urban",
          "lanes": 2, "lane_width": 3.5, "oneway": false,
          "nodes": [[180, 0, 0], [180, 180, 0]]
        },
        {
          "id": 3, "name": "cross_south", "type": "urban",
          "lanes": 2, "lane_width": 3.5, "oneway": false,
          "nodes": [[180, 0, 0], [180, -180, 0]]
        },
        {
          "id": 4, "name": "sky_viaduct", "type": "ramp_curve",
          "lanes": 2, "lane_width": 3.5, "oneway": true, "bridge": true,
          "nodes": [[80, -80, 6.5], [180, 0, 6.5], [280, 80, 6.5]]
        }
      ]
    })";

    flowsim::WorldEngineInput in;
    in.road_network_json = rn;
    in.random_seed = 42;
    auto layout = flowsim::compile_world(in);

    CHECK(layout.sanity.poles > 10, "Generated street poles (%d > 10)", layout.sanity.poles);
    CHECK(layout.sanity.trees > 20, "Generated roadside trees (%d > 20)", layout.sanity.trees);
    CHECK(layout.sanity.barrier_posts > 15, "Generated viaduct barrier posts (%d > 15)", layout.sanity.barrier_posts);
    CHECK(layout.sanity.piers >= 4, "Generated viaduct piers (%d >= 4)", layout.sanity.piers);
    CHECK(layout.sanity.signs >= 4, "Generated traffic signs (%d >= 4)", layout.sanity.signs);
    CHECK(layout.sanity.buildings > 15, "Generated procedural skyline buildings (%d > 15)", layout.sanity.buildings);
    CHECK(layout.sanity.remaining_asphalt_intrusions == 0,
          "Zero asphalt intrusions (%d)", layout.sanity.remaining_asphalt_intrusions);
    CHECK(layout.sanity.floating_count == 0, "Zero floating props (%d)", layout.sanity.floating_count);

    /* Pier crossing over lower ground road (x=180) must NOT place central pier column inside road y in [-7,7] */
    bool pier_blocks_lower_road = false;
    for (const auto& pr : layout.piers) {
        if (std::fabs(pr.x - 180.0) < 6.0 && std::fabs(pr.y) < 7.0 && pr.columns == 1) {
            pier_blocks_lower_road = true;
        }
    }
    CHECK(!pier_blocks_lower_road, "Overpass pier over avenue intersection straddles clear of lower lanes");

    /* Determinism check */
    auto layout2 = flowsim::compile_world(in);
    CHECK(layout.world_hash == layout2.world_hash, "Deterministic world_hash (%s)", layout.world_hash.c_str());
    CHECK(flowsim::serialize_world_layout_json(layout) == flowsim::serialize_world_layout_json(layout2),
          "Serialized world layout JSON is bit-identical");
}

static void test_declarative_world_gen_refinements() {
    std::printf("\n=== [Test 2] Declarative world_gen Overrides, Exclude Zones & Custom Props ===\n");
    const char* rn = R"({
      "edges": [
        {
          "id": 0, "name": "central_boulevard", "type": "urban",
          "lanes": 4, "lane_width": 3.5, "oneway": false,
          "nodes": [[0, 0, 0], [300, 0, 0]]
        },
        {
          "id": 1, "name": "quiet_park_lane", "type": "urban",
          "lanes": 2, "lane_width": 3.5, "oneway": false,
          "nodes": [[300, 0, 0], [500, 0, 5.0]]
        }
      ]
    })";
    const char* wg = R"({
      "world_gen": {
        "preset": "modern_cbd",
        "seed": 77,
        "exclude_zones": [
          { "x": 150.0, "y": 0.0, "radius": 45.0, "kinds": ["tree", "building"] }
        ],
        "edge_overrides": [
          { "edge": "central_boulevard", "trees": false, "barriers": true, "speed_limit_kph": 80 }
        ],
        "custom_props": [
          {
            "id": "welcome_arch",
            "kind": "welcome_gantry",
            "label": "NOA CORRIDOR",
            "placement": "road_s",
            "road": "central_boulevard",
            "s": 80.0,
            "d": 0.0,
            "width": 16.0,
            "height": 6.5,
            "collider": false
          },
          {
            "id": "ramp_bollard",
            "kind": "bollard",
            "placement": "world",
            "x": 400.0,
            "y": -6.0,
            "auto_z": true,
            "collider": true
          }
        ]
      }
    })";

    flowsim::WorldEngineInput in;
    in.road_network_json = rn;
    in.world_gen_json = wg;
    auto layout = flowsim::compile_world(in);

    CHECK(layout.preset == "modern_cbd", "Preset parsed as modern_cbd");

    int trees_on_central = 0;
    for (const auto& t : layout.trees) {
        if (t.x >= 0.0 && t.x <= 300.0) trees_on_central++;
    }
    CHECK(trees_on_central == 0, "edge_overrides disabled trees on central_boulevard (%d)", trees_on_central);

    int buildings_in_excl = 0;
    for (const auto& b : layout.buildings) {
        if (std::hypot(b.x - 150.0, b.y) <= 45.0) buildings_in_excl++;
    }
    CHECK(buildings_in_excl == 0, "exclude_zones removed buildings within 45m radius (%d)", buildings_in_excl);

    bool has_80_sign = false;
    for (const auto& s : layout.signs) {
        if (s.kind == "speed_limit" && s.speed_kph == 80) has_80_sign = true;
    }
    CHECK(has_80_sign, "edge_overrides speed_limit_kph=80 reflected in compiled speed_limit sign");

    bool arch_ok = false, bollard_z_ok = false;
    for (const auto& p : layout.props) {
        if (p.id == "welcome_arch" && std::fabs(p.x - 80.0) < 0.1 && std::fabs(p.y) < 0.1) arch_ok = true;
        if (p.id == "ramp_bollard" && std::fabs(p.z - 2.5) < 0.6) bollard_z_ok = true;
    }
    CHECK(arch_ok, "road_s custom prop snapped to central_boulevard s=80");
    CHECK(bollard_z_ok, "world custom prop with auto_z snapped to sloped deck height ~2.5m");

    bool bollard_in_colliders = false;
    for (const auto& c : layout.colliders) {
        if (c.kind == "bollard" && std::hypot(c.x - 400.0, c.y + 6.0) < 0.5) {
            bollard_in_colliders = true;
        }
    }
    CHECK(bollard_in_colliders, "Collidable custom prop exported to physics/sensor colliders");
}

#if defined(FLOW_HAS_JOLT) && FLOW_HAS_JOLT
static void test_jolt_physics_collider_sync() {
    std::printf("\n=== [Test 3] WorldEngine -> Jolt3D Collider Physics Parity ===\n");
    const char* rn = R"({
      "edges": [
        { "id": 0, "name": "test_corridor", "type": "urban", "lanes": 2, "lane_width": 3.5,
          "oneway": false, "nodes": [[0, 0, 0], [200, 0, 0]] }
      ]
    })";
    const char* wg = R"({
      "world_gen": {
        "preset": "urban_arterial",
        "custom_props": [
          { "id": "test_bollard", "kind": "bollard", "placement": "road_s",
            "road": "test_corridor", "s": 40.0, "d": -1.75, "length": 0.8, "width": 0.8, "height": 1.4, "collider": true }
        ]
      }
    })";

    flowsim::WorldEngineInput in;
    in.road_network_json = rn;
    in.world_gen_json = wg;
    auto layout = flowsim::compile_world(in);

    std::vector<flowsim::StaticBoxObstacle> jolt_obs;
    for (const auto& c : layout.colliders) {
        flowsim::StaticBoxObstacle o;
        o.x = c.x; o.y = c.y; o.z = c.z;
        o.len = c.len; o.wid = c.wid; o.height = c.height;
        o.heading = c.heading; o.kind = c.kind; o.is_cylinder = c.is_cylinder;
        jolt_obs.push_back(o);
    }

    flowsim::PhysicsWorld pw;
    CHECK(pw.ready(), "PhysicsWorld ready");
    CHECK(pw.build_static_world(nullptr, nullptr, {}, jolt_obs), "Built static Jolt world with WorldEngine colliders");
    pw.add_ground_slab(40.0, 0.0, 0.0, 60.0, 20.0, 1.0);

    flowsim::Entity ego;
    ego.active = true; ego.id = 0; ego.type = flowsim::EntityType::Ego;
    ego.length = 4.6; ego.width = 2.0; ego.height = 1.5; ego.wheelbase = 2.7;
    ego.mass = 1500.0; ego.cg_height = 0.55; ego.yaw_inertia = 2250.0;
    ego.drag_coeff = 0.30; ego.pacejka_b = 10.0; ego.pacejka_c = 1.9;
    ego.pacejka_e = 0.97; ego.pacejka_mu = 0.95; ego.surface_mu_scale = 1.0;
    ego.x = 18.0; ego.y = -1.75; ego.z = 0.05; ego.heading = 0.0;
    ego.speed = 12.0; ego.vx = 12.0;
    CHECK(pw.register_ego(ego, true), "Registered ego driving toward procedural bollard at x=40");

    pw.set_vehicle_input(0, 0.5, 0.0, 0.0, 1, 1.0);
    bool hit_obstacle = false;
    for (int step = 0; step < 240; ++step) {
        pw.step(1.0 / 60.0);
        for (const auto& c : pw.consume_contacts()) {
            if (c.kind_b == "bollard") {
                hit_obstacle = true;
                break;
            }
        }
        if (hit_obstacle) break;
    }
    CHECK(hit_obstacle, "Jolt3D ego physically collided with WorldEngine procedural bollard");
}
#endif

static void test_ramp_touchdown_and_podium_clearance() {
    std::printf("\n=== [Test 4] Ramp Touchdown & 1.35x Podium Building Clearance ===\n");
    const char* rn = R"({
      "edges": [
        { "id": 0, "name": "garage_exit_merge", "type": "urban", "lanes": 2, "lane_width": 3.5,
          "oneway": false, "nodes": [[100, 0, -2.0], [180, 12, 0.0], [260, 26, 0.0]] },
        { "id": 1, "name": "ground_signal_avenue", "type": "urban", "lanes": 4, "lane_width": 3.5,
          "oneway": false, "nodes": [[260, 26, 0.0], [520, 26, 0.0]] },
        { "id": 2, "name": "viaduct_ramp_entry", "type": "ramp_curve", "lanes": 2, "lane_width": 3.5,
          "oneway": true, "bridge": true, "nodes": [[520, 26, 0.0], [600, 50, 1.5], [700, 90, 7.5]] }
      ]
    })";
    const char* wg = R"({ "world_gen": { "preset": "modern_cbd", "seed": 42 } })";
    flowsim::WorldEngineInput in;
    in.road_network_json = rn;
    in.world_gen_json = wg;
    in.random_seed = 42;
    auto layout = flowsim::compile_world(in);

    struct Seg { double ax, ay, az, bx, by, bz, hw; bool bridge; };
    const std::vector<Seg> segs = {
        {100, 0, -2.0, 180, 12, 0.0, 3.5, false},
        {180, 12, 0.0, 260, 26, 0.0, 3.5, false},
        {260, 26, 0.0, 520, 26, 0.0, 7.0, false},
        {520, 26, 0.0, 600, 50, 1.5, 3.5, true},
        {600, 50, 1.5, 700, 90, 7.5, 3.5, true},
    };
    auto dist_to_seg = [](double px, double py, const Seg& s) {
        const double dx = s.bx - s.ax, dy = s.by - s.ay;
        const double l2 = dx * dx + dy * dy;
        if (l2 < 1e-9) return std::hypot(px - s.ax, py - s.ay);
        const double t = std::max(0.0, std::min(1.0, ((px - s.ax) * dx + (py - s.ay) * dy) / l2));
        return std::hypot(px - (s.ax + t * dx), py - (s.ay + t * dy));
    };

    int footprint_intrusions = 0;
    for (const auto& b : layout.buildings) {
        const double mul = b.height > 28.0 ? 1.35 : 1.0;
        const double hd = b.depth * mul * 0.5;
        const double hw = b.width * mul * 0.5;
        const double fx = std::cos(b.heading), fy = std::sin(b.heading);
        const double rx = fy, ry = -fx;
        const double samples[9][2] = {
            {0, 0}, {-hd, -hw}, {hd, -hw}, {hd, hw}, {-hd, hw},
            {0, -hw}, {0, hw}, {-hd, 0}, {hd, 0}
        };
        for (const auto& sp : samples) {
            const double px = b.x + fx * sp[0] + rx * sp[1];
            const double py = b.y + fy * sp[0] + ry * sp[1];
            for (const auto& s : segs) {
                if (s.bridge && std::min(s.az, s.bz) >= 2.8) continue;
                if (dist_to_seg(px, py, s) < s.hw + 2.5) {
                    footprint_intrusions++;
                }
            }
        }
    }
    CHECK(layout.buildings.size() > 10, "Generated buildings along corridor+ramp (%zu > 10)",
          layout.buildings.size());
    CHECK(footprint_intrusions == 0,
          "Zero 1.35x podium corner intrusions on corridor or ramp touchdown (%d)",
          footprint_intrusions);
}

int main() {
    test_procedural_corridor_and_overpass();
    test_declarative_world_gen_refinements();
    test_ramp_touchdown_and_podium_clearance();
#if defined(FLOW_HAS_JOLT) && FLOW_HAS_JOLT
    test_jolt_physics_collider_sync();
#endif
    std::printf("\nSummary: %d/%d checks passed (%d failed)\n",
                g_run - g_fail, g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
