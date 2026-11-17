#include "planning_framework/commute_mission.h"

#include <cstdio>
#include <cmath>

using flowengine::planning::CommuteMission;
using flowengine::planning::CommutePhase;
using flowengine::planning::ParkingGoal;
using flowengine::planning::ParkOutGoal;

static int g_fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_fails; \
    } \
} while (0)

static ParkingGoal office_goal() {
    ParkingGoal g;
    g.enabled = true;
    g.x = 3225.5;
    g.y = -6.5;
    g.z = 0.0;
    g.heading = 1.5708;
    g.width = 2.8;
    g.length = 6.0;
    return g;
}

static ParkOutGoal home_depart() {
    ParkOutGoal p;
    p.enabled = true;
    p.slot_x = 12.0;
    p.slot_y = -17.3;
    p.slot_z = -4.5;
    p.slot_heading = 1.5708;
    p.exit_x = 26.0;
    p.exit_y = -12.25;
    p.exit_z = -4.5;
    p.exit_heading = 0.0;
    p.corridor_width = 6.0;
    return p;
}

static void test_dwell_then_return_targets_home() {
    CommuteMission m;
    m.dwell_s = 6.0;
    ParkingGoal goal = office_goal();
    ParkOutGoal park = home_depart();
    size_t slots = 3;
    m.observe_scenario(true, goal, park);
    CHECK(m.latched);
    CHECK(std::fabs(m.home.x - 12.0) < 1e-9);
    CHECK(std::fabs(m.home.z - (-4.5)) < 1e-9);

    const bool held = m.apply(10.0, 3225.5, -6.5, 0.0, 0.0, true,
                              3220.0, 0.0, 0.0, M_PI,
                              goal, park, slots);
    CHECK(!held);
    CHECK(m.phase == CommutePhase::Dwell);
    CHECK(slots == 3);
    CHECK(goal.x == 3225.5);

    const bool still = m.apply(14.0, 3225.5, -6.5, 0.0, 0.0, true,
                               3220.0, 0.0, 0.0, M_PI,
                               goal, park, slots);
    CHECK(!still);
    CHECK(m.phase == CommutePhase::Dwell);

    const bool left = m.apply(16.1, 3225.5, -6.5, 0.0, 0.0, true,
                              3220.0, 0.0, 0.0, M_PI,
                              goal, park, slots);
    CHECK(left);
    CHECK(m.pulse_reset);
    CHECK(m.phase == CommutePhase::Return);
    CHECK(slots == 0);
    CHECK(std::fabs(goal.x - 12.0) < 1e-9);
    CHECK(std::fabs(goal.y - (-17.3)) < 1e-9);
    CHECK(std::fabs(park.slot_x - 3225.5) < 1e-9);
    CHECK(std::fabs(park.exit_x - 3220.0) < 1e-9);
    CHECK(std::fabs(park.exit_heading - M_PI) < 1e-9);

    ParkingGoal synced = office_goal();
    ParkOutGoal synced_park = home_depart();
    size_t synced_slots = 3;
    m.apply(20.0, 3000.0, 0.0, 0.0, 8.0, false,
            0, 0, 0, 0, synced, synced_park, synced_slots);
    CHECK(m.phase == CommutePhase::Return);
    CHECK(!m.pulse_reset);
    CHECK(synced_slots == 0);
    CHECK(std::fabs(synced.x - 12.0) < 1e-9);
    CHECK(std::fabs(synced_park.exit_heading - M_PI) < 1e-9);

    ParkingGoal home = office_goal();
    ParkOutGoal back = home_depart();
    size_t n = 3;
    m.apply(400.0, 12.0, -17.3, -4.5, 0.0, true,
            0, 0, 0, 0, home, back, n);
    CHECK(m.phase == CommutePhase::Home);
    CHECK(std::fabs(home.x - 12.0) < 1e-9);
}

static void test_disabled_commute_keeps_office() {
    CommuteMission m;
    ParkingGoal goal = office_goal();
    ParkOutGoal park = home_depart();
    size_t slots = 2;
    m.observe_scenario(false, goal, park);
    CHECK(!m.latched);
    const bool over = m.apply(30.0, 3225.5, -6.5, 0.0, 0.0, true,
                              0, 0, 0, 0, goal, park, slots);
    CHECK(!over);
    CHECK(m.phase == CommutePhase::Outbound);
    CHECK(goal.x == 3225.5);
    CHECK(slots == 2);
}

int main() {
    test_dwell_then_return_targets_home();
    test_disabled_commute_keeps_office();
    if (g_fails) {
        std::fprintf(stderr, "%d commute mission checks failed\n", g_fails);
        return 1;
    }
    std::printf("PASS commute mission\n");
    return 0;
}
