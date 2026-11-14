#include <cmath>
#include <cstdio>

#include "park_out_scenario.h"
#include "scenario_manager.h"
#include "valet_parking_scenario.h"

using namespace flowengine::planning;

static int g_failures = 0;

#define TEST_CHECK(cond, msg)                                                  \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg));      \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static void test_valet_distance_and_elevation_gate() {
    std::printf("[RUN] test_valet_distance_and_elevation_gate\n");
    ValetParkingScenario valet;
    FrameContext ctx;
    ctx.parking_goal.enabled = true;
    ctx.parking_goal.x = 3200.0;
    ctx.parking_goal.y = -4.0;
    ctx.parking_goal.z = 0.0;
    ctx.parking_goal.heading = M_PI * 0.5;

    // Far away at B1 underground (-4.5m, 3200m away): must NOT preempt route
    ctx.vehicle_state.x = 0.0;
    ctx.vehicle_state.y = -6.0;
    ctx.vehicle_state.z = -4.5;
    TEST_CHECK(!valet.IsApplicable(ctx), "ValetParking must not apply 3200m away or at z=-4.5m");

    // Horizontally close (20m) but still on different floor (|dz| > 2.5m): must NOT apply
    ctx.vehicle_state.x = 3180.0;
    ctx.vehicle_state.y = -2.0;
    ctx.vehicle_state.z = -4.5;
    TEST_CHECK(!valet.IsApplicable(ctx), "ValetParking must not apply across vertical floors");

    // Within 48m on same elevation: must apply (and preserves valet_parking.json ~30.7m behavior)
    ctx.vehicle_state.x = 3168.0;
    ctx.vehicle_state.y = -1.75;
    ctx.vehicle_state.z = 0.0;
    TEST_CHECK(valet.IsApplicable(ctx), "ValetParking must apply within 48m on same elevation");
    std::printf("[PASS] test_valet_distance_and_elevation_gate\n");
}

static void test_slot_occupancy_search_and_oriented_obstacles() {
    std::printf("[RUN] test_slot_occupancy_search_and_oriented_obstacles\n");
    ValetParkingScenario valet;
    FrameContext ctx;
    ctx.parking_goal.enabled = true;
    ctx.parking_goal.x = 100.0;
    ctx.parking_goal.y = -5.5;
    ctx.parking_goal.z = 0.0;
    ctx.parking_goal.heading = M_PI * 0.5;
    ctx.parking_goal.width = 2.8;
    ctx.parking_goal.length = 5.8;

    // 3 candidate slots spaced 3.8m apart
    ctx.parking_slot_count = 3;
    for (int i = 0; i < 3; ++i) {
        ParkingGoal slot{};
        slot.enabled = true;
        slot.x = 96.0 + i * 3.8;
        slot.y = -5.5;
        slot.z = 0.0;
        slot.heading = M_PI * 0.5;
        slot.width = 2.8;
        slot.length = 5.8;
        ctx.parking_slots[static_cast<size_t>(i)] = slot;
    }

    // Slot 0 and Slot 1 are occupied by perpendicular parked cars (heading = M_PI/2)
    Obstacle occ0{};
    occ0.id = 1;
    occ0.x = 96.0f;
    occ0.y = -5.5f;
    occ0.length = 4.6f;
    occ0.width = 1.9f;
    ctx.AddObstacle(occ0, true, static_cast<float>(M_PI * 0.5));

    Obstacle occ1{};
    occ1.id = 2;
    occ1.x = 99.8f;
    occ1.y = -5.5f;
    occ1.length = 4.6f;
    occ1.width = 1.9f;
    ctx.AddObstacle(occ1, true, static_cast<float>(M_PI * 0.5));

    // Approach within activation gate (<18m to enter OPEN_SPACE_CRUISE and lock slot 2)
    ctx.vehicle_state.x = 90.0;
    ctx.vehicle_state.y = 0.0;
    ctx.vehicle_state.z = 0.0;
    ctx.vehicle_state.v = 2.0;
    ctx.vehicle_state.heading = 0.0;

    TEST_CHECK(valet.IsApplicable(ctx), "ValetParking should be applicable near candidate slots");
    Trajectory traj{};
    ScenarioStatus st = valet.Run(ctx, traj);
    TEST_CHECK(st == ScenarioStatus::RUNNING, "ValetParking should run cleanly into OpenSpaceCruise");
    TEST_CHECK(valet.SelectedSlotIndex() == 2, "Must skip occupied slots 0 & 1 and select free slot 2");
    TEST_CHECK(std::fabs(valet.ActiveGoal().x - 103.6) < 1e-4, "Active goal X must match slot 2");

    // Verify locked slot stays invariant even during approach/cruise
    TEST_CHECK(ctx.selected_slot_index == 2, "FrameContext selected_slot_index must reflect slot 2");
    std::printf("[PASS] test_slot_occupancy_search_and_oriented_obstacles\n");
}

static void test_park_out_lifecycle_and_arbitration() {
    std::printf("[RUN] test_park_out_lifecycle_and_arbitration\n");
    ScenarioManager mgr;
    ParkOutScenario park_out;
    ValetParkingScenario valet;
    mgr.SetDebounceThreshold(1);
    mgr.RegisterScenario(&valet);
    mgr.RegisterScenario(&park_out);

    FrameContext ctx;
    ctx.park_out.enabled = true;
    ctx.park_out.slot_x = 0.0;
    ctx.park_out.slot_y = -6.0;
    ctx.park_out.slot_z = -4.5;
    ctx.park_out.slot_heading = -M_PI * 0.5;
    ctx.park_out.exit_x = 18.0;
    ctx.park_out.exit_y = 0.0;
    ctx.park_out.exit_z = -4.5;
    ctx.park_out.exit_heading = 0.0;
    ctx.park_out.corridor_width = 6.0;

    // Distant park-in goal on ground level
    ctx.parking_goal.enabled = true;
    ctx.parking_goal.x = 3200.0;
    ctx.parking_goal.y = -5.0;
    ctx.parking_goal.z = 0.0;
    ctx.parking_goal.heading = M_PI * 0.5;

    ctx.vehicle_state.x = 0.0;
    ctx.vehicle_state.y = -6.0;
    ctx.vehicle_state.z = -4.5;
    ctx.vehicle_state.heading = -M_PI * 0.5;
    ctx.vehicle_state.v = 0.0;

    // Flanking parked cars in neighboring B1 bays
    Obstacle left_car{};
    left_car.id = 10;
    left_car.x = -3.8f;
    left_car.y = -6.0f;
    left_car.length = 4.6f;
    left_car.width = 1.9f;
    ctx.AddObstacle(left_car, true, static_cast<float>(-M_PI * 0.5));

    Obstacle right_car{};
    right_car.id = 11;
    right_car.x = 3.8f;
    right_car.y = -6.0f;
    right_car.length = 4.6f;
    right_car.width = 1.9f;
    ctx.AddObstacle(right_car, true, static_cast<float>(-M_PI * 0.5));

    Trajectory traj{};
    mgr.Update(ctx, traj);
    TEST_CHECK(mgr.CurrentScenario() == &park_out, "ParkOutScenario must arbitrate above ValetParking and LaneFollow");
    TEST_CHECK(park_out.IsActive(), "ParkOutScenario must be active on B1 start");
    TEST_CHECK(park_out.CurrentStageName() == "PARK_OUT_MANEUVER", "Stage 1 must be PARK_OUT_MANEUVER");

    bool has_nonzero_traj = false;
    for (int i = 0; i < 64; ++i) {
        if (std::fabs(traj.points[i].v) > 0.2f) {
            has_nonzero_traj = true;
            break;
        }
    }
    TEST_CHECK(has_nonzero_traj, "ParkOut maneuver trajectory must produce motion points");

    // Advance vehicle to aisle exit pose
    ctx.vehicle_state.x = 18.0;
    ctx.vehicle_state.y = 0.0;
    ctx.vehicle_state.heading = 0.0;
    ctx.vehicle_state.v = 1.5;
    mgr.Update(ctx, traj);
    TEST_CHECK(park_out.CurrentStageName() == "PARK_OUT_CRUISE", "Stage 2 must transition to PARK_OUT_CRUISE");
    TEST_CHECK(traj.points[0].v > 1.0f, "Cruise handoff trajectory must command positive velocity");

    // Move slightly past exit along aisle -> completes ParkOutScenario
    ctx.vehicle_state.x = 20.5;
    ctx.vehicle_state.y = 0.0;
    ctx.vehicle_state.heading = 0.0;
    ctx.vehicle_state.v = 2.5;
    ScenarioStatus final_st = park_out.Run(ctx, traj);
    TEST_CHECK(final_st == ScenarioStatus::FINISHED, "ParkOutScenario must finish after joining aisle");
    TEST_CHECK(!park_out.IsApplicable(ctx), "Completed ParkOutScenario must stop being applicable");

    // Step ScenarioManager after completion (triggers SwitchToScenario(nullptr) -> Reset())
    mgr.Update(ctx, traj);

    // Advance ego past exit_x + 8.0 (former limit-cycle re-entry zone) and confirm no re-trigger
    ctx.vehicle_state.x = 32.0;
    ctx.vehicle_state.y = 0.0;
    ctx.vehicle_state.heading = 0.0;
    ctx.vehicle_state.v = 3.0;
    mgr.Update(ctx, traj);
    mgr.Update(ctx, traj);
    TEST_CHECK(!park_out.IsApplicable(ctx), "ParkOut must stay inapplicable down aisle after SwitchToScenario(nullptr)");
    TEST_CHECK(!park_out.IsActive(), "ParkOut must remain inactive down aisle after completion");
    TEST_CHECK(mgr.CurrentScenario() != &park_out, "ScenarioManager must not re-select completed ParkOutScenario");

    // Return to start slot at standstill (simulating scenario reload) -> RearmIfAtStartSlot rearms ParkOut
    ctx.vehicle_state.x = 0.0;
    ctx.vehicle_state.y = -6.0;
    ctx.vehicle_state.z = -4.5;
    ctx.vehicle_state.heading = -M_PI * 0.5;
    ctx.vehicle_state.v = 0.0;
    park_out.RearmIfAtStartSlot(ctx);
    TEST_CHECK(park_out.IsApplicable(ctx), "ParkOut must re-arm when ego reloads at start slot");
    std::printf("[PASS] test_park_out_lifecycle_and_arbitration\n");
}

int main() {
    test_valet_distance_and_elevation_gate();
    test_slot_occupancy_search_and_oriented_obstacles();
    test_park_out_lifecycle_and_arbitration();
    if (g_failures > 0) {
        std::printf("FAILED with %d errors\n", g_failures);
        return 1;
    }
    std::printf("ALL PARK_OUT AND SLOT SEARCH TESTS PASSED\n");
    return 0;
}
