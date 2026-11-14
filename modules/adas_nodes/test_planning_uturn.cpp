#include "planning_uturn.h"

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

planning_uturn::UturnEnv make_base_env() {
    planning_uturn::UturnEnv env;
    env.lane_count = 4;
    env.lane_width = 3.5;
    env.road_oneway = 0;
    env.cfg_target_speed = 15.0;
    return env;
}

double norm_heading(double h) {
    while (h > M_PI) h -= 2.0 * M_PI;
    while (h < -M_PI) h += 2.0 * M_PI;
    return h;
}

void test_construction_checks() {
    auto env = make_base_env();
    CHECK(!planning_uturn::body_hits_construction(env, 100.0, -1.75, 0.0, 2.3, 1.0, 0.5));
    CHECK(planning_uturn::construction_forward_space(env, 100.0, -1.75, 2.0) > 1e8);

    double cz_x[] = {125.0, 140.0};
    double cz_y[] = {-1.75, 5.25};  // second is far lateral from -1.75
    double cz_len[] = {10.0, 10.0}; // front_x = 120.0, 135.0
    double cz_wid[] = {3.0, 3.0};
    env.cz_x = cz_x;
    env.cz_y = cz_y;
    env.cz_len = cz_len;
    env.cz_wid = cz_wid;
    env.cz_count = 2;

    double fwd = planning_uturn::construction_forward_space(env, 100.0, -1.75, 2.0);
    // front_x=120.0 -> d = 120.0 - 100.0 - 2.0 = 18.0
    CHECK(std::fabs(fwd - 18.0) < 1e-6);

    // Ego at x=122 inside zone 0 AABB should hit
    CHECK(planning_uturn::body_hits_construction(env, 122.0, -1.75, 0.0, 2.3, 1.0, 0.5));
    // Ego off-lane at x=122, y=5.25 hits zone 1, whereas y=1.75 (front_x=120..130, y=-3.75..0.25) clears
    CHECK(!planning_uturn::body_hits_construction(env, 122.0, 2.0, 0.0, 2.3, 1.0, 0.5));
}

void test_normal_four_lane_uturn() {
    auto env = make_base_env();
    TrajectoryPoint pts[512];
    const int n = planning_uturn::generate_trajectory(
        env, pts, 512, 100.0, -1.75, 0.0, 3.0, 2.7, 40.0);
    CHECK(n > 64);

    // Road half width = 4 * 0.5 * 3.5 = 7.0m; during U-turn maneuver (before Phase 5 straight fill)
    // vehicle center stays cleanly bounded inside road half-width
    double max_turn_abs_y = 0.0;
    for (int i = 0; i < n; ++i) {
        if (pts[i].v >= (float)(env.cfg_target_speed - 0.5)) continue;
        double ay = std::fabs((double)pts[i].y);
        if (ay > max_turn_abs_y) max_turn_abs_y = ay;
    }
    CHECK(max_turn_abs_y <= 7.0);
    // Final heading aligns near +-pi (opposite carriageway heading) and final v > 0
    CHECK(std::fabs(std::fabs(norm_heading((double)pts[n - 1].heading)) - M_PI) < 0.25);
    CHECK(pts[n - 1].v > 1.0f);
}

void test_tight_space_phase0_reverse_and_downsample() {
    auto env = make_base_env();
    TrajectoryPoint raw[512];
    const int raw_n = planning_uturn::generate_trajectory(
        env, raw, 512, 200.0, -1.75, 0.0, 0.0, 2.7, 4.0);
    CHECK(raw_n > 64);

    bool has_reverse = false;
    bool has_stop_hold = false;
    for (int i = 0; i < raw_n; ++i) {
        if (raw[i].v < -0.5f) has_reverse = true;
        if (std::fabs((double)raw[i].v) < 0.05) has_stop_hold = true;
    }
    CHECK(has_reverse);
    CHECK(has_stop_hold);

    TrajectoryPoint ds[64];
    const int ds_n = downsample_uturn(raw, raw_n, ds, 64);
    CHECK(ds_n > 10 && ds_n <= 64);
    CHECK(ds[ds_n - 1].v > 0.0f);
}

void test_construction_blocks_phase5_cruise() {
    auto env = make_base_env();
    // Place construction barrier behind ego on the return lane (+y=1.75, x=70) so Phase 5 halts before it
    double cz_x[] = {70.0};
    double cz_y[] = {1.75};
    double cz_len[] = {6.0};
    double cz_wid[] = {3.5};
    env.cz_x = cz_x;
    env.cz_y = cz_y;
    env.cz_len = cz_len;
    env.cz_wid = cz_wid;
    env.cz_count = 1;

    TrajectoryPoint pts[512];
    const int n = planning_uturn::generate_trajectory(
        env, pts, 512, 100.0, -1.75, 0.0, 2.0, 2.7, 40.0);
    CHECK(n > 10);
    CHECK(n < 512);  // Phase 5 stops early before construction barrier
    CHECK(pts[n - 1].v == 0.0f);
    for (int i = 0; i < n; ++i) {
        CHECK(!planning_uturn::body_hits_construction(
            env, (double)pts[i].x, (double)pts[i].y, (double)pts[i].heading,
            2.3, 1.0, 0.5));
    }
}

}  // namespace

int main() {
    test_construction_checks();
    test_normal_four_lane_uturn();
    test_tight_space_phase0_reverse_and_downsample();
    test_construction_blocks_phase5_cruise();
    std::printf("test_planning_uturn: all checks passed\n");
    return 0;
}
