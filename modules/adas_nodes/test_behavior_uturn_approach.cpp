#include "behavior_uturn_approach.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

namespace {

behavior_uturn::UturnApproachInput base_input() {
    behavior_uturn::UturnApproachInput in;
    in.ego_x = 100.0;
    in.ego_y = -1.75;
    in.ego_v = 15.0;
    in.road_end_x = 1000.0;
    in.ref_path_end_x = 1000.0;
    in.uturn_approach_dist_m = 120.0;
    in.uturn_max_trigger_speed = 5.0;
    in.cfg_cruise_speed = 15.0;
    in.uturn_cooldown = 0.0;
    in.on_return = 0;
    in.at_inner_lane = true;
    return in;
}

void test_far_from_road_end() {
    auto in = base_input();
    auto dec = behavior_uturn::evaluate_uturn_approach(in);
    CHECK(!dec.trigger);
    CHECK(!dec.approach_active);
    CHECK(!dec.override_lane_and_speed);
}

void test_high_speed_approach_decelerates() {
    auto in = base_input();
    in.ego_x = 800.0;  // within 3*120=360m of road_end_x=1000
    in.ego_v = 14.0;
    auto dec = behavior_uturn::evaluate_uturn_approach(in);
    CHECK(!dec.trigger);
    CHECK(dec.approach_active);
    CHECK(dec.override_lane_and_speed);
    CHECK(dec.target_lane == -1);
    CHECK(dec.target_speed < in.cfg_cruise_speed);
    CHECK(dec.target_speed >= in.uturn_max_trigger_speed);
    CHECK(dec.reason[0] != '\0');
}

void test_construction_zone_shortens_trigger() {
    auto in = base_input();
    double cz_x[] = {910.0};
    double cz_y[] = {-1.75};
    double cz_len[] = {20.0};  // front_x = 900.0
    double cz_wid[] = {3.5};
    in.cz_x = cz_x;
    in.cz_y = cz_y;
    in.cz_len = cz_len;
    in.cz_wid = cz_wid;
    in.cz_count = 1;
    in.ego_x = 800.0;  // 100m before front_x=900 (< 120m approach), slow enough to trigger
    in.ego_v = 4.5;
    auto dec = behavior_uturn::evaluate_uturn_approach(in);
    CHECK(dec.trigger);
    CHECK(dec.approach_active);
}

void test_outer_lane_holds_approach_without_trigger() {
    auto in = base_input();
    in.ego_x = 920.0;
    in.ego_v = 4.0;
    in.at_inner_lane = false;
    auto dec = behavior_uturn::evaluate_uturn_approach(in);
    CHECK(!dec.trigger);
    CHECK(dec.approach_active);
}

void test_return_trip_trigger_and_cooldown() {
    auto in = base_input();
    in.on_return = 1;
    in.ego_y = 1.75;
    in.ego_x = 90.0;  // < 120m
    in.ego_v = 4.5;
    auto dec = behavior_uturn::evaluate_uturn_approach(in);
    CHECK(dec.trigger);
    CHECK(dec.approach_active);

    in.uturn_cooldown = 2.5;
    auto cooled = behavior_uturn::evaluate_uturn_approach(in);
    CHECK(!cooled.trigger);
    CHECK(cooled.approach_active);
}

}  // namespace

int main() {
    test_far_from_road_end();
    test_high_speed_approach_decelerates();
    test_construction_zone_shortens_trigger();
    test_outer_lane_holds_approach_without_trigger();
    test_return_trip_trigger_and_cooldown();
    std::printf("test_behavior_uturn_approach: all checks passed\n");
    return 0;
}
