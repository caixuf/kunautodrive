#include "accel_lane_policy.h"

#include <cmath>
#include <cstdio>

static int g_fails = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL %s\n", msg);                           \
            ++g_fails;                                                        \
        }                                                                     \
    } while (0)

static flowengine::AccelLaneInput base_out() {
    flowengine::AccelLaneInput in;
    in.lane_count = 8;
    in.oneway = false;
    in.on_return = false;
    in.current_idx = 7;
    in.cruise_mps = 100.0 / 3.6;
    in.speed_mps = in.cruise_mps * 0.5;
    in.inner_lane_empty = true;
    return in;
}

static void test_hold_and_step() {
    const double cruise = 100.0 / 3.6;
    flowengine::AccelLaneInput in = base_out();
    in.speed_mps = 14.0; /* above 85% of the 15 m/s fallback, below 85% of 27.8 */
    auto d = flowengine::decide_accel_lane(in);
    CHECK(d.active && d.holding && !d.merge_inward, "slow outbound holds");
    CHECK(d.target_lane == 7, "already on the outer lane stays there");

    in.current_idx = 5;
    d = flowengine::decide_accel_lane(in);
    CHECK(d.holding && d.target_lane == 6, "outbound steps one lane toward the outside");
    CHECK(!d.merge_inward, "below 85% does not merge inward");

    in = base_out();
    in.on_return = true;
    in.current_idx = 0;
    in.speed_mps = cruise * 0.5;
    d = flowengine::decide_accel_lane(in);
    CHECK(d.holding && d.target_lane == 0 && !d.merge_inward, "return holds lane 0");

    in.current_idx = 2;
    d = flowengine::decide_accel_lane(in);
    CHECK(d.holding && d.target_lane == 1, "return steps one lane toward lane 0");
}

static void test_release() {
    const double cruise = 100.0 / 3.6;
    flowengine::AccelLaneInput in = base_out();
    in.speed_mps = cruise * 0.85;
    in.inner_lane_empty = true;
    auto d = flowengine::decide_accel_lane(in);
    CHECK(!d.holding && d.merge_inward && d.target_lane == 6,
          "at 85% an empty inner lane is taken");

    in.inner_lane_empty = false;
    in.inner_lead_mps = cruise * 0.70;
    d = flowengine::decide_accel_lane(in);
    CHECK(d.merge_inward && d.target_lane == 6, "a fast inner lead is taken");

    in.inner_lead_mps = cruise * 0.50;
    d = flowengine::decide_accel_lane(in);
    CHECK(d.active && !d.holding && !d.merge_inward, "a slow inner lead is refused");

    in.on_return = true;
    in.current_idx = 0;
    in.speed_mps = cruise * 0.85;
    in.inner_lane_empty = true;
    d = flowengine::decide_accel_lane(in);
    CHECK(d.merge_inward && d.target_lane == 1, "return merges inward one lane");

    in.inner_lane_empty = false;
    in.inner_lead_mps = cruise * 0.4;
    d = flowengine::decide_accel_lane(in);
    CHECK(!d.merge_inward, "return also refuses a slow inner lead");
}

static void test_inactive() {
    flowengine::AccelLaneInput in = base_out();
    in.lane_count = 4;
    auto d = flowengine::decide_accel_lane(in);
    CHECK(!d.active, "narrower roads are outside this policy");
    in.lane_count = 8;
    in.oneway = true;
    d = flowengine::decide_accel_lane(in);
    CHECK(!d.active, "oneway roads are outside this policy");
}

int main() {
    test_hold_and_step();
    test_release();
    test_inactive();
    if (g_fails) {
        std::fprintf(stderr, "FAIL accel lane (%d)\n", g_fails);
        return 1;
    }
    std::printf("PASS accel lane\n");
    return 0;
}
