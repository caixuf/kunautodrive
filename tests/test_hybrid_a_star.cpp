#include <iostream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <cassert>
#include <chrono>

#include "modules/adas_nodes/planning_framework/open_space/box2d.h"
#include "modules/adas_nodes/planning_framework/open_space/reeds_shepp_path.h"
#include "modules/adas_nodes/planning_framework/open_space/grid_search.h"
#include "modules/adas_nodes/planning_framework/open_space/hybrid_a_star.h"

using namespace flowengine::planning;

#define TEST_CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAILED] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            std::exit(1); \
        } else { \
            std::cout << "[PASS] " << msg << std::endl; \
        } \
    } while (0)

void TestCollisionDetection() {
    std::cout << "\n=== 1. Testing OBB Collision Detection Correctness ===" << std::endl;

    // Test 1.1: OBB vs OBB overlap
    Box2d b1(Vec2d(0.0, 0.0), 0.0, 4.0, 2.0);
    Box2d b2(Vec2d(1.0, 0.5), 0.0, 4.0, 2.0); // overlapping
    Box2d b3(Vec2d(5.0, 0.0), 0.0, 4.0, 2.0); // separated along X
    Box2d b4(Vec2d(0.0, 3.0), 0.0, 4.0, 2.0); // separated along Y

    TEST_CHECK(b1.HasOverlap(b2), "Overlapping boxes must return true");
    TEST_CHECK(!b1.HasOverlap(b3), "Separated boxes along X must return false");
    TEST_CHECK(!b1.HasOverlap(b4), "Separated boxes along Y must return false");

    // Test 1.2: Rotated OBB SAT
    Box2d b_rot1(Vec2d(0.0, 0.0), M_PI / 4.0, 4.0, 2.0);
    Box2d b_rot2(Vec2d(2.0, 2.0), 0.0, 2.0, 2.0); // Penetrating corner
    Box2d b_rot3(Vec2d(4.0, 4.0), 0.0, 2.0, 2.0); // Far away
    TEST_CHECK(b_rot1.HasOverlap(b_rot2), "Rotated box penetrating corner must return true");
    TEST_CHECK(!b_rot1.HasOverlap(b_rot3), "Separated rotated box must return false");

    // Test 1.3: Point in Box
    TEST_CHECK(b1.IsPointIn(Vec2d(0.0, 0.0)), "Center point must be in box");
    TEST_CHECK(b1.IsPointIn(Vec2d(1.8, 0.8)), "Inside point near corner must be in box");
    TEST_CHECK(!b1.IsPointIn(Vec2d(2.5, 0.0)), "Outside point must not be in box");

    // Test 1.4: Line Segment vs Box
    LineSegment2d seg1(Vec2d(-3.0, 0.0), Vec2d(3.0, 0.0)); // passing through
    LineSegment2d seg2(Vec2d(1.0, 0.5), Vec2d(1.5, 0.8));   // completely inside
    LineSegment2d seg3(Vec2d(3.0, 3.0), Vec2d(5.0, 5.0));   // completely outside
    TEST_CHECK(b1.HasOverlap(seg1), "Line passing through box must return true");
    TEST_CHECK(b1.HasOverlap(seg2), "Line inside box must return true");
    TEST_CHECK(!b1.HasOverlap(seg3), "Line outside box must return false");

    // Test 1.5: Ego vehicle collision checking against environment
    VehicleParam veh;
    veh.wheelbase = 2.85;
    veh.length = 4.8;
    veh.width = 2.0;
    veh.back_edge_to_center = 1.0;

    HybridAStarConfig cfg;
    cfg.vehicle_safety_margin = 0.1;
    HybridAStar planner(veh, cfg);

    Environment env;
    env.min_x = -20.0; env.max_x = 20.0;
    env.min_y = -20.0; env.max_y = 20.0;

    // Obstacle 1: Box at (5, 0)
    env.obstacle_boxes.emplace_back(Vec2d(5.0, 0.0), 0.0, 2.0, 2.0);

    // Ego at (0, 0, 0) -> vehicle extends from x = -1.0 to x = 3.8. Obstacle is at [4.0, 6.0].
    TEST_CHECK(!planner.CheckVehicleCollision(0.0, 0.0, 0.0, env), "Ego at (0, 0, 0) should be collision-free");

    // Ego at (2.0, 0.0, 0) -> vehicle front reaches 2.0 + 3.8 = 5.8m, penetrates obstacle [4, 6]!
    TEST_CHECK(planner.CheckVehicleCollision(2.0, 0.0, 0.0, env), "Ego moved forward must collide with obstacle");

    // Ego out of environment bounds
    TEST_CHECK(planner.CheckVehicleCollision(25.0, 0.0, 0.0, env), "Ego out of bounds must report collision");
}

void TestForwardObstacleAvoidance() {
    std::cout << "\n=== 2. Testing Forward Obstacle Avoidance Planning ===" << std::endl;

    VehicleParam veh;
    veh.wheelbase = 2.85;
    veh.length = 4.8;
    veh.width = 2.0;
    veh.back_edge_to_center = 1.0;
    veh.max_steer = 0.6;
    veh.step_size = 1.2;

    HybridAStarConfig cfg;
    cfg.xy_grid_resolution = 0.3;
    cfg.analytic_expansion_max_dist = 15.0;
    cfg.vehicle_safety_margin = 0.1;

    HybridAStar planner(veh, cfg);

    Environment env;
    env.min_x = -5.0; env.max_x = 35.0;
    env.min_y = -10.0; env.max_y = 10.0;

    // Place an obstacle box right on the straight centerline x in [10, 14], y in [-1.5, 1.5]
    env.obstacle_boxes.emplace_back(Vec2d(12.0, 0.0), 0.0, 4.0, 3.0);

    double start_x = 0.0, start_y = 0.0, start_phi = 0.0;
    double goal_x = 24.0, goal_y = 0.0, goal_phi = 0.0;

    HybridAStarResult result;
    bool success = planner.Plan(start_x, start_y, start_phi, goal_x, goal_y, goal_phi, env, &result);

    TEST_CHECK(success, "Forward avoidance planning must succeed");
    TEST_CHECK(!result.path.empty(), "Path must not be empty");

    // Verify all path points are collision free
    bool any_collision = false;
    double max_lateral_deviation = 0.0;
    for (const auto& pt : result.path) {
        if (planner.CheckVehicleCollision(pt.x, pt.y, pt.phi, env)) {
            any_collision = true;
            break;
        }
        max_lateral_deviation = std::max(max_lateral_deviation, std::abs(pt.y));
    }
    TEST_CHECK(!any_collision, "Every path point in forward avoidance must be collision-free");
    TEST_CHECK(max_lateral_deviation > 2.0, "Vehicle must deviate laterally around the obstacle");

    const auto& last = result.path.back();
    double pos_err = std::hypot(last.x - goal_x, last.y - goal_y);
    double yaw_err = std::abs(std::fmod(last.phi - goal_phi + M_PI, 2.0 * M_PI) - M_PI);
    TEST_CHECK(pos_err < 0.3, "Goal position error < 0.3m");
    TEST_CHECK(yaw_err < 0.2, "Goal heading error < 0.2 rad");

    std::cout << "  -> Plan stats: Length=" << result.total_length
              << "m, Gears=" << result.gear_switches
              << ", Expanded=" << result.expanded_nodes
              << ", Time=" << result.search_time_ms << "ms" << std::endl;
}

void TestReverseParkingAndUTurn() {
    std::cout << "\n=== 3. Testing Reverse Parking & Narrow U-Turn Planning ===" << std::endl;

    VehicleParam veh;
    veh.wheelbase = 2.85;
    veh.length = 4.8;
    veh.width = 2.0;
    veh.back_edge_to_center = 1.0;
    veh.max_steer = 0.6;
    veh.step_size = 1.0;

    HybridAStarConfig cfg;
    cfg.xy_grid_resolution = 0.25;
    cfg.phi_grid_resolution = 0.0872665;
    cfg.weight_reverse = 2.0;
    cfg.weight_gear_switch = 15.0;
    cfg.vehicle_safety_margin = 0.10;
    cfg.analytic_expansion_max_dist = 12.0;

    HybridAStar planner(veh, cfg);

    // -------------------------------------------------------------
    // Scenario 3.1: Perpendicular Reverse Parking (AVP)
    // -------------------------------------------------------------
    std::cout << "--- 3.1 Perpendicular Reverse Parking ---" << std::endl;
    Environment park_env;
    park_env.min_x = -10.0; park_env.max_x = 30.0;
    park_env.min_y = -10.0; park_env.max_y = 10.0;

    // Slot is at x in [8.0, 12.0], y in [-6.0, 0.0]
    // Left parked car at x=6.0, y=-3.0, heading=pi/2, len=4.6, wid=2.0
    park_env.obstacle_boxes.emplace_back(Vec2d(6.0, -3.0), M_PI * 0.5, 4.6, 2.0);
    // Right parked car at x=14.0, y=-3.0, heading=pi/2, len=4.6, wid=2.0
    park_env.obstacle_boxes.emplace_back(Vec2d(14.0, -3.0), M_PI * 0.5, 4.6, 2.0);
    // Curb wall on opposite side at y = 5.0
    park_env.obstacle_lines.emplace_back(Vec2d(-10.0, 5.0), Vec2d(30.0, 5.0));

    // Vehicle starts driving along aisle at (0.0, 2.0, 0.0)
    // Target is reverse-parked neatly inside slot facing out at (10.0, -3.5, +pi/2)
    double start_x = 0.0, start_y = 2.0, start_phi = 0.0;
    double goal_x = 10.0, goal_y = -3.5, goal_phi = M_PI * 0.5;

    HybridAStarResult park_result;
    bool park_ok = planner.Plan(start_x, start_y, start_phi, goal_x, goal_y, goal_phi, park_env, &park_result);

    TEST_CHECK(park_ok, "Reverse parking planning must succeed");
    TEST_CHECK(!park_result.path.empty(), "Parking path must not be empty");
    TEST_CHECK(park_result.gear_switches >= 1, "Must contain at least 1 gear switch for reverse parking");

    bool has_reverse = false;
    bool has_collision = false;
    for (const auto& pt : park_result.path) {
        if (pt.direction == -1) has_reverse = true;
        if (planner.CheckVehicleCollision(pt.x, pt.y, pt.phi, park_env)) {
            has_collision = true;
            break;
        }
    }
    TEST_CHECK(has_reverse, "Parking path MUST contain reverse motion points (direction == -1)");
    TEST_CHECK(!has_collision, "Parking path must have zero collisions with parked vehicles and curb");

    const auto& park_last = park_result.path.back();
    double park_pos_err = std::hypot(park_last.x - goal_x, park_last.y - goal_y);
    double park_yaw_err = std::abs(std::fmod(park_last.phi - goal_phi + M_PI, 2.0 * M_PI) - M_PI);
    TEST_CHECK(park_pos_err < 0.35, "Parking goal position error < 0.35m");
    TEST_CHECK(park_yaw_err < 0.25, "Parking goal heading error < 0.25 rad");

    std::cout << "  -> Reverse parking stats: Length=" << park_result.total_length
              << "m, Gears=" << park_result.gear_switches
              << ", Expanded=" << park_result.expanded_nodes
              << ", Time=" << park_result.search_time_ms << "ms" << std::endl;

    // -------------------------------------------------------------
    // Scenario 3.2: Narrow Corridor 3-Point U-Turn
    // -------------------------------------------------------------
    std::cout << "--- 3.2 Narrow Corridor 3-Point U-Turn ---" << std::endl;
    Environment uturn_env;
    uturn_env.min_x = -15.0; uturn_env.max_x = 25.0;
    uturn_env.min_y = -4.5;  uturn_env.max_y = 4.5; // Narrow 2-lane road 9m wide (car turning diameter is ~12m!)

    // Start heading East on South side, Target heading West on North side
    double u_start_x = 0.0, u_start_y = -1.5, u_start_phi = 0.0;
    double u_goal_x = 0.0,  u_goal_y = 1.5,  u_goal_phi = M_PI;

    HybridAStarResult uturn_result;
    bool uturn_ok = planner.Plan(u_start_x, u_start_y, u_start_phi, u_goal_x, u_goal_y, u_goal_phi, uturn_env, &uturn_result);

    TEST_CHECK(uturn_ok, "Narrow corridor U-turn planning must succeed");
    TEST_CHECK(uturn_result.gear_switches >= 2, "Narrow U-turn requires multi-point turns (gear switches >= 2)");

    bool u_has_collision = false;
    for (const auto& pt : uturn_result.path) {
        if (planner.CheckVehicleCollision(pt.x, pt.y, pt.phi, uturn_env)) {
            u_has_collision = true;
            break;
        }
    }
    TEST_CHECK(!u_has_collision, "Narrow U-turn path must stay strictly inside road corridor boundaries");

    std::cout << "  -> U-turn stats: Length=" << uturn_result.total_length
              << "m, Gears=" << uturn_result.gear_switches
              << ", Expanded=" << uturn_result.expanded_nodes
              << ", Time=" << uturn_result.search_time_ms << "ms" << std::endl;
}

void TestPerformanceTypicalParking() {
    std::cout << "\n=== 4. Performance Benchmark: Typical Parking Planning Time < 50ms ===" << std::endl;

    VehicleParam veh;
    veh.wheelbase = 2.85;
    veh.length = 4.8;
    veh.width = 2.0;
    veh.back_edge_to_center = 1.0;
    veh.max_steer = 0.6;
    veh.step_size = 1.0;

    HybridAStarConfig cfg;
    cfg.xy_grid_resolution = 0.25;
    cfg.phi_grid_resolution = 0.0872665;
    cfg.weight_reverse = 2.0;
    cfg.weight_gear_switch = 15.0;
    cfg.vehicle_safety_margin = 0.10;
    cfg.analytic_expansion_max_dist = 12.0;

    HybridAStar planner(veh, cfg);

    Environment park_env;
    park_env.min_x = -10.0; park_env.max_x = 30.0;
    park_env.min_y = -10.0; park_env.max_y = 10.0;

    // Adjacent parked vehicles
    park_env.obstacle_boxes.emplace_back(Vec2d(6.0, -3.0), M_PI * 0.5, 4.6, 2.0);
    park_env.obstacle_boxes.emplace_back(Vec2d(14.0, -3.0), M_PI * 0.5, 4.6, 2.0);
    park_env.obstacle_lines.emplace_back(Vec2d(-10.0, 5.0), Vec2d(30.0, 5.0));

    double start_x = 2.0, start_y = 2.2, start_phi = 0.0;
    double goal_x = 10.0, goal_y = -3.5, goal_phi = M_PI * 0.5;

    const int iterations = 20;
    std::vector<double> latencies;
    latencies.reserve(iterations);

    double total_time = 0.0;
    double max_time = 0.0;
    double min_time = 1e9;

    for (int i = 0; i < iterations; ++i) {
        HybridAStarResult res;
        auto t0 = std::chrono::high_resolution_clock::now();
        bool ok = planner.Plan(start_x, start_y, start_phi, goal_x, goal_y, goal_phi, park_env, &res);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        TEST_CHECK(ok, "Iteration must succeed");
        latencies.push_back(ms);
        total_time += ms;
        max_time = std::max(max_time, ms);
        min_time = std::min(min_time, ms);
    }

    double avg_time = total_time / iterations;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Benchmark (" << iterations << " runs):"
              << " Avg = " << avg_time << "ms"
              << ", Min = " << min_time << "ms"
              << ", Max = " << max_time << "ms" << std::endl;

    // ASan (~2-3x slowdown) inflates Hybrid A* avg beyond the Release 50ms gate
    // (CI saw ~63.8ms). Keep Release budget; relax only under AddressSanitizer.
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
    const double kAvgBudgetMs = 150.0;
#else
    const double kAvgBudgetMs = 50.0;
#endif
    TEST_CHECK(avg_time < kAvgBudgetMs,
               "Average planning time MUST be under budget (Release <50ms; ASan <150ms)");
}

int main() {
    std::cout << "==========================================================" << std::endl;
    std::cout << "   RUNNING HYBRID A* & REEDS-SHEPP UNIT & PERF TESTS     " << std::endl;
    std::cout << "==========================================================" << std::endl;

    TestCollisionDetection();
    TestForwardObstacleAvoidance();
    TestReverseParkingAndUTurn();
    TestPerformanceTypicalParking();

    std::cout << "\n==========================================================" << std::endl;
    std::cout << "   ALL HYBRID A* UNIT & BENCHMARK TESTS PASSED (100%)    " << std::endl;
    std::cout << "==========================================================" << std::endl;
    return 0;
}
