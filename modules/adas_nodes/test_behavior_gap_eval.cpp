#include "behavior_gap_eval.h"

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

behavior_gap::GapEvalConfig base_cfg() {
    behavior_gap::GapEvalConfig cfg;
    cfg.ego_x = 100.0;
    cfg.ego_y = -1.75;
    cfg.ego_v = 15.0;
    cfg.ego_heading = 0.0;
    cfg.ego_lane_d = -1.75;
    cfg.target_speed = 18.0;
    cfg.min_gap = 25.0;
    cfg.lc_gap_mult = 1.5;  // required front gap = 37.5m
    cfg.rear_safe_min_m = 15.0;
    cfg.rear_safe_time_s = 2.0;
    cfg.lat_tol = 2.0;
    cfg.front_ttc_min_s = 3.0;
    return cfg;
}

bool no_proj(double, double, double&) { return false; }

void test_open_lane_accepted() {
    auto cfg = base_cfg();
    auto res = behavior_gap::evaluate_target_lane_gap(
        cfg, 1.75, true, true, nullptr, nullptr, nullptr, nullptr, 0, no_proj);
    CHECK(res.same_side);
    CHECK(res.rear_safe);
    CHECK(res.front_ttc_safe);
    CHECK(res.ok);
}

void test_fast_rear_approach_rejected() {
    auto cfg = base_cfg();
    // Rear car 30m behind, approaching at 32 m/s (rrs = 17 m/s -> min_rd = 34m > 30m)
    double ox[] = {70.0};
    double oy[] = {1.75};
    double ovx[] = {32.0};
    double ovy[] = {0.0};
    auto res = behavior_gap::evaluate_target_lane_gap(
        cfg, 1.75, true, true, ox, oy, ovx, ovy, 1, no_proj);
    CHECK(!res.rear_safe);
    CHECK(!res.ok);
}

void test_slow_front_car_ttc_rejected() {
    auto cfg = base_cfg();
    // Front car 40m ahead (> 37.5m min_gap*mult), but stopped (ego_v=15 -> TTC = 2.67s < 3.0s)
    double ox[] = {140.0};
    double oy[] = {1.75};
    double ovx[] = {0.0};
    double ovy[] = {0.0};
    auto res = behavior_gap::evaluate_target_lane_gap(
        cfg, 1.75, true, true, ox, oy, ovx, ovy, 1, no_proj);
    CHECK(res.rear_safe);
    CHECK(!res.front_ttc_safe);
    CHECK(!res.ok);
}

void test_safe_front_and_rear_accepted() {
    auto cfg = base_cfg();
    // Front car 55m ahead at 12 m/s (TTC = 55/3 = 18.3s), rear car 30m behind at 14 m/s
    double ox[] = {155.0, 70.0};
    double oy[] = {1.75, 1.75};
    double ovx[] = {12.0, 14.0};
    double ovy[] = {0.0, 0.0};
    auto res = behavior_gap::evaluate_target_lane_gap(
        cfg, 1.75, true, true, ox, oy, ovx, ovy, 2, no_proj);
    CHECK(res.rear_safe);
    CHECK(res.front_ttc_safe);
    CHECK(res.ok);
    CHECK(res.front_gap > 54.9 && res.front_gap < 55.1);
}

void test_wrong_side_rejected() {
    auto cfg = base_cfg();
    auto res = behavior_gap::evaluate_target_lane_gap(
        cfg, 1.75, false, true, nullptr, nullptr, nullptr, nullptr, 0, no_proj);
    CHECK(!res.ok);
}

void test_behind_ego_curve_lead_rejected() {
    // Ego at (544.5, -1.2), heading 0.14 rad; ped behind on inner corner at (542.5, +1.5).
    const double ego_x = 544.5;
    const double ego_y = -1.2;
    const double ego_h = 0.14;
    const double ped_x = 542.5;
    const double ped_y = 1.5;
    const double phys_along = (ped_x - ego_x) * std::cos(ego_h) +
                              (ped_y - ego_y) * std::sin(ego_h);
    CHECK(phys_along < 0.0);
    // Curve projection near corner can still report small non-negative delta-s.
    CHECK(!behavior_gap::is_valid_forward_lead(0.2, phys_along));
    CHECK(!behavior_gap::is_valid_forward_lead(-0.5, 5.0));
    // Genuine vehicle ahead on turn is accepted, including long lookahead (>12m).
    CHECK(behavior_gap::is_valid_forward_lead(6.0, 5.8));
    CHECK(behavior_gap::is_valid_forward_lead(25.0, -2.0));
}

void test_corridor_guardrail_and_ebike_passable() {
    auto proj_xy = [](double x, double y, double& out_s, double& out_d) -> bool {
        out_s = x - 100.0;
        out_d = y;
        return true;
    };
    behavior_gap::CorridorConfig ccfg;
    ccfg.road_d_min = -3.5;
    ccfg.road_d_max = 0.0;
    ccfg.ego_width = 2.0;
    ccfg.lateral_margin = 0.35;
    ccfg.s_lo = 0.0;
    ccfg.s_hi = 30.0;
    ccfg.target_d = -1.75;

    // Guardrail post outside road (-3.8m) + e-bike riding along right curb (-3.1m, half_w=0.35):
    // remaining free corridor [-2.75, 0.0] has width 2.75m >= 2.35m -> passable!
    double ox[] = {105.0, 112.0};
    double oy[] = {-3.8, -3.1};
    double hw[] = {0.25, 0.35};
    ccfg.obs_half_w = hw;
    auto res = behavior_gap::evaluate_lateral_corridor(ccfg, ox, oy, 2, proj_xy);
    CHECK(res.passable);
    CHECK(res.target_passable);
    CHECK(res.best_width > 2.7 && res.best_width < 2.8);
}

void test_corridor_narrow_blocked() {
    auto proj_xy = [](double x, double y, double& out_s, double& out_d) -> bool {
        out_s = x - 100.0;
        out_d = y;
        return true;
    };
    behavior_gap::CorridorConfig ccfg;
    ccfg.road_d_min = -3.5;
    ccfg.road_d_max = 0.0;
    ccfg.ego_width = 2.0;
    ccfg.lateral_margin = 0.35;
    ccfg.s_lo = 0.0;
    ccfg.s_hi = 30.0;
    ccfg.target_d = -1.75;

    // Oncoming car in middle (-1.75, half_w=0.95) leaves [-3.5, -2.7] (0.8m) and [-0.8, 0.0] (0.8m) -> blocked!
    double ox[] = {115.0};
    double oy[] = {-1.75};
    double hw[] = {0.95};
    ccfg.obs_half_w = hw;
    auto res = behavior_gap::evaluate_lateral_corridor(ccfg, ox, oy, 1, proj_xy);
    CHECK(!res.passable);
    CHECK(!res.target_passable);
}

void test_corridor_partial_nudge_and_narrow_oncoming() {
    auto proj_xy = [](double x, double y, double& out_s, double& out_d) -> bool {
        out_s = x - 100.0;
        out_d = y;
        return true;
    };
    behavior_gap::CorridorConfig ccfg;
    ccfg.road_d_min = -4.8;
    ccfg.road_d_max = 0.0;
    ccfg.ego_width = 2.0;
    ccfg.lateral_margin = 0.25;
    ccfg.s_lo = 0.0;
    ccfg.s_hi = 30.0;
    ccfg.target_d = -1.75;

    // Double-parked car on right curb (-4.0, half_w=0.90) leaves [-3.1, 0.0] (3.1m free):
    // passable and free center shifts left toward -1.55m for nudge.
    double ox[] = {112.0};
    double oy[] = {-4.0};
    double hw[] = {0.90};
    ccfg.obs_half_w = hw;
    auto res = behavior_gap::evaluate_lateral_corridor(ccfg, ox, oy, 1, proj_xy);
    CHECK(res.passable);
    CHECK(res.best_width > 3.0);
    CHECK(res.best_center > -1.75);

    // Narrow-road oncoming car at +0.75 (half_w=0.90) vs no-lane-line road [-3.5, +2.0]:
    // free corridor [-3.5, -0.15] has width 3.35m >= 2.25m -> passable!
    ccfg.road_d_min = -3.5;
    ccfg.road_d_max = 2.0;
    oy[0] = 0.75;
    res = behavior_gap::evaluate_lateral_corridor(ccfg, ox, oy, 1, proj_xy);
    CHECK(res.passable);
    CHECK(res.best_width > 3.3);
}

void test_oneway_outer_lane_obstacle_not_dropped() {
    auto cfg = base_cfg();
    /* 4-lane one-way: lane centres are -1.75, -5.25, -8.75, -12.25.
     * The old symmetric |d| < N*w/2 band (7m) dropped lanes 0/1 entirely. */
    cfg.ego_y = -12.25;
    cfg.ego_lane_d = -12.25;
    cfg.road_half_w = 7.0;
    cfg.road_d_min = -14.0;
    cfg.road_d_max = 0.0;
    double ox[] = {155.0};
    double oy[] = {-12.25};
    double ovx[] = {12.0};
    double ovy[] = {0.0};
    auto res = behavior_gap::evaluate_target_lane_gap(
        cfg, -12.25, true, true, ox, oy, ovx, ovy, 1, no_proj);
    CHECK(res.front_gap < 60.0);
    CHECK(res.front_gap > 50.0);
    CHECK(res.ok);
}

}  // namespace

int main() {
    test_open_lane_accepted();
    test_fast_rear_approach_rejected();
    test_slow_front_car_ttc_rejected();
    test_safe_front_and_rear_accepted();
    test_wrong_side_rejected();
    test_behind_ego_curve_lead_rejected();
    test_corridor_guardrail_and_ebike_passable();
    test_corridor_narrow_blocked();
    test_corridor_partial_nudge_and_narrow_oncoming();
    test_oneway_outer_lane_obstacle_not_dropped();
    std::printf("test_behavior_gap_eval: all 10 tests passed\n");
    return 0;
}
