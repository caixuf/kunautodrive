#include "planning_speed_smooth.h"
#include "planning_tl_stop.h"
#include "trajectory_speed_window.h"
#include "safety_pedestrian_guard.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                    \
        }                                                                    \
    } while (0)

namespace {

void test_linear_sampling_interpolates_smoothly() {
    double grid[] = {10.0, 12.0, 14.0};
    double v05 = planning_speed::sample_speed_linear(grid, 3, 1.0, 0.5);
    double v15 = planning_speed::sample_speed_linear(grid, 3, 1.0, 1.5);
    CHECK(std::fabs(v05 - 11.0) < 1e-9);
    CHECK(std::fabs(v15 - 13.0) < 1e-9);
    CHECK(std::fabs(planning_speed::sample_speed_linear(grid, 3, 1.0, -1.0) - 10.0) < 1e-9);
    CHECK(std::fabs(planning_speed::sample_speed_linear(grid, 3, 1.0, 5.0) - 14.0) < 1e-9);
}

void test_backward_decel_smooths_curve_entry() {
    constexpr int N = 20;
    double s[N], k[N], v[N], a[N], j[N];
    for (int i = 0; i < N; i++) {
        s[i] = static_cast<double>(i) * 1.0;
        k[i] = (i >= 12) ? 0.1 : 0.0;  // sharp curve at s=12m -> vmax = 0.9*sqrt(5/0.1) = 6.36m/s
        v[i] = 15.0;
    }
    planning_speed::smooth_speed_profile(s, k, N, 5.0, 2.5, 3.0, 20.0, v, a, j);

    // Speed must smoothly ramp down before s=12m, never dropping abruptly in 1m
    for (int i = 1; i < N; i++) {
        CHECK(a[i] >= -3.01 && a[i] <= 2.51);
    }
    CHECK(v[12] <= 0.9 * std::sqrt(5.0 / 0.1) + 1e-6);
    CHECK(v[11] > v[12] && v[11] < 15.0);
}

struct FakeTrajPt {
    uint32_t t_rel_us;
    float x;
    float y;
    float v;
};

void test_cruise_horizon_ignores_far_stopline_when_creeping() {
    /* 16 waypoints at 3m spacing (0..45m) with synthetic 0.1s*i timestamps;
     * stop at 30m (i >= 10) must not force 0.0 when creeping at 1.4 m/s. */
    FakeTrajPt pts[16];
    for (int i = 0; i < 16; i++) {
        pts[i].t_rel_us = static_cast<uint32_t>(i * 100000);
        pts[i].x = static_cast<float>(i * 3.0);
        pts[i].y = 0.0f;
        pts[i].v = (i >= 10) ? 0.0f : 3.5f;
    }
    double tv = adas_control::cruise_target_speed_in_horizon(pts, 16, 1.4);
    CHECK(std::fabs(tv - 3.5) < 1e-6);

    /* Standstill pull-away toward the stopline 30m ahead must still launch. */
    pts[0].v = 0.0f;
    double tv_launch = adas_control::cruise_target_speed_in_horizon(pts, 16, 0.0);
    CHECK(std::fabs(tv_launch - 3.5) < 1e-6);
}

void test_cruise_horizon_holds_stop_near_stopline() {
    FakeTrajPt pts[16];
    for (int i = 0; i < 16; i++) {
        pts[i].t_rel_us = static_cast<uint32_t>(i * 100000);
        pts[i].x = static_cast<float>(i * 3.0);
        pts[i].y = 0.0f;
        pts[i].v = (i == 0) ? 0.0f : 0.0f;
    }
    CHECK(std::fabs(adas_control::cruise_target_speed_in_horizon(pts, 16, 0.0)) < 1e-6);

    /* Stopped 3.0m before stopline (i >= 1 has v = 0.0) must hold 0.0. */
    pts[0].v = 0.5f;
    pts[1].v = 0.0f;
    CHECK(std::fabs(adas_control::cruise_target_speed_in_horizon(pts, 16, 0.3)) < 1e-6);
}

void test_cruise_horizon_captures_curve_decel_at_high_speed() {
    FakeTrajPt pts[16];
    for (int i = 0; i < 16; i++) {
        pts[i].t_rel_us = static_cast<uint32_t>(i * 100000);
        pts[i].x = static_cast<float>(i * 3.0);
        pts[i].y = 0.0f;
        /* Curve entry at 15m (i=5) lowers speed to 6.0 m/s. */
        pts[i].v = (i >= 5) ? 6.0f : 12.0f;
    }
    /* At 12 m/s, reach_horizon = 12*1.5 + 2.25 = 20.25m >= 15m -> sees 6.0 m/s. */
    double tv = adas_control::cruise_target_speed_in_horizon(pts, 16, 12.0);
    CHECK(std::fabs(tv - 6.0) < 1e-6);
}

struct FakeSafetyState {
    double x{0.0}, y{0.0}, z{0.0}, heading{0.0}, speed{0.0};
    double obs_x[8]{}, obs_y[8]{}, obs_z[8]{}, obs_v[8]{}, obs_vy[8]{};
    bool obs_valid[8]{};
    char obs_type[8][16]{};
};

void test_ped_guard_allows_creeping_to_stopline_behind_crosswalk() {
    FakeSafetyState s;
    s.x = 530.7;
    s.y = -1.75;
    s.speed = 1.2;
    /* Ped 0 behind ego; Ped 1 active on crosswalk at x=542.5 (along=11.8m). */
    s.obs_valid[0] = true;
    std::snprintf(s.obs_type[0], sizeof(s.obs_type[0]), "pedestrian");
    s.obs_x[0] = 528.0;
    s.obs_y[0] = -1.75;

    s.obs_valid[1] = true;
    std::snprintf(s.obs_type[1], sizeof(s.obs_type[1]), "pedestrian");
    s.obs_x[1] = 542.5;
    s.obs_y[1] = -1.5;
    s.obs_vy[1] = 1.2;

    const double gap = adas_safety::pedestrian_collision_gap<FakeSafetyState, 8>(s);
    const double hold = adas_safety::pedestrian_crossing_hold_gap<FakeSafetyState, 8>(s);
    /* Must see Ped 1 (9.0m / 7.3m), NOT return 1e9 due to Ped 0 behind,
     * and must NOT engage emergency braking at 11.8m when creeping at 1.2 m/s. */
    CHECK(std::fabs(gap - 9.0) < 1e-6);
    CHECK(gap > adas_safety::pedestrian_stop_gap(s.speed));
    CHECK(std::fabs(hold - 7.3) < 1e-6);
    CHECK(hold > adas_safety::pedestrian_hold_activate_gap(s.speed));
}

void test_ped_guard_brakes_at_speed_and_ignores_exiting_curb_walker() {
    FakeSafetyState s;
    s.x = 45.0;
    s.y = -1.75;
    s.speed = 11.0;
    /* Static in-lane ped 20m ahead -> gap = 17.2m < stop_gap(11.0)=23.1m -> brakes! */
    s.obs_valid[0] = true;
    std::snprintf(s.obs_type[0], sizeof(s.obs_type[0]), "pedestrian");
    s.obs_x[0] = 65.0;
    s.obs_y[0] = -1.75;
    const double gap = adas_safety::pedestrian_collision_gap<FakeSafetyState, 8>(s);
    CHECK(gap < adas_safety::pedestrian_stop_gap(s.speed));

    /* Pedestrian already past ego's lane (+3.5m left) and walking away (+1.2 m/s) must clear! */
    s.obs_x[0] = 53.0;
    s.obs_y[0] = 1.75;  /* lat_signed = +3.5m */
    s.obs_vy[0] = 1.2;
    CHECK((adas_safety::pedestrian_collision_gap<FakeSafetyState, 8>(s) > 1e8));
    CHECK((adas_safety::pedestrian_crossing_hold_gap<FakeSafetyState, 8>(s) > 1e8));
}

void test_tl_stop_clamps_near_stopline_and_holds_yellow_when_stopped() {
    /* Creeping to dx_tl=0.3m (< wall_margin 5.0m) on RED must clamp stop_s to 0.0, not drop it. */
    double stop_s = -1.0;
    stop_s = adas_planning::merge_min_stop_s(
        stop_s, adas_planning::tl_effective_stop_s(0.3, 5.0));
    CHECK(std::fabs(stop_s - 0.0) < 1e-9);

    /* Distant stopline retains wall_margin offset. */
    CHECK(std::fabs(adas_planning::tl_effective_stop_s(15.0, 5.0) - 10.0) < 1e-9);

    /* Stopped/creeping behind stopline on YELLOW must NOT clear; fast car close to line may clear. */
    CHECK(!adas_planning::yellow_light_can_clear(0.3, 0.0));
    CHECK(!adas_planning::yellow_light_can_clear(0.3, 0.4));
    CHECK(adas_planning::yellow_light_can_clear(1.5, 10.0));
    CHECK(!adas_planning::yellow_light_can_clear(30.0, 10.0));

    /* Zero yield_stop_s also overrides unset or larger stop_s. */
    CHECK(std::fabs(adas_planning::merge_min_stop_s(10.0, 0.0) - 0.0) < 1e-9);
    CHECK(std::fabs(adas_planning::merge_min_stop_s(-1.0, 0.0) - 0.0) < 1e-9);
}

}  // namespace

int main() {
    test_linear_sampling_interpolates_smoothly();
    test_backward_decel_smooths_curve_entry();
    test_cruise_horizon_ignores_far_stopline_when_creeping();
    test_cruise_horizon_holds_stop_near_stopline();
    test_cruise_horizon_captures_curve_decel_at_high_speed();
    test_ped_guard_allows_creeping_to_stopline_behind_crosswalk();
    test_ped_guard_brakes_at_speed_and_ignores_exiting_curb_walker();
    test_tl_stop_clamps_near_stopline_and_holds_yellow_when_stopped();
    std::printf("test_planning_speed_smooth: all 8 tests passed\n");
    return 0;
}

