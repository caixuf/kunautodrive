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

}  // namespace

int main() {
    test_open_lane_accepted();
    test_fast_rear_approach_rejected();
    test_slow_front_car_ttc_rejected();
    test_safe_front_and_rear_accepted();
    test_wrong_side_rejected();
    test_behind_ego_curve_lead_rejected();
    std::printf("test_behavior_gap_eval: all 6 tests passed\n");
    return 0;
}
