#include "planning_coordinates.h"

#include <cassert>
#include <cmath>

static bool nearly_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

int main() {
    assert(nearly_equal(planning_coord::lane_center_d(0, 4, 3.5), 5.25));
    assert(nearly_equal(planning_coord::lane_center_d(2, 4, 3.5), -1.75));
    assert(planning_coord::first_legal_lane(4, false) == 2);
    assert(planning_coord::first_legal_lane(4, true) == 0);
    assert(planning_coord::nearest_own_lane(6.0, 4, 3.5) == 2);
    assert(planning_coord::nearest_own_lane(-6.0, 4, 3.5) == 3);
    assert(planning_coord::nearest_lane(1.75, 4, 3.5, false) == 1);

    /* ── 单向路车道组 side_offset：车道全在 -y 侧，idx0=最内侧（y=-w/2） ── */
    assert(nearly_equal(planning_coord::lane_group_side_offset(4, 3.5, true), -7.0));
    assert(nearly_equal(planning_coord::lane_group_side_offset(4, 3.5, false), 0.0));
    assert(nearly_equal(planning_coord::lane_group_side_offset(2, 3.5, true), -3.5));
    assert(nearly_equal(planning_coord::lane_group_side_offset(1, 3.5, true), 0.0));

    const double so1 = planning_coord::lane_group_side_offset(4, 3.5, true);
    /* 4 车道单向：idx0=-1.75, idx1=-5.25, idx2=-8.75, idx3=-12.25（全在 -y） */
    assert(nearly_equal(planning_coord::lane_center_d(0, 4, 3.5, so1), -1.75));
    assert(nearly_equal(planning_coord::lane_center_d(1, 4, 3.5, so1), -5.25));
    assert(nearly_equal(planning_coord::lane_center_d(2, 4, 3.5, so1), -8.75));
    assert(nearly_equal(planning_coord::lane_center_d(3, 4, 3.5, so1), -12.25));

    /* round-trip：nearest_lane(lane_center_d(i)) == i（单向，N∈{1,2,3,4,6,8}） */
    for (int N = 1; N <= 8; ++N) {
        const double so = planning_coord::lane_group_side_offset(N, 3.5, true);
        for (int i = 0; i < N; ++i) {
            const double y = planning_coord::lane_center_d(i, N, 3.5, so);
            assert(planning_coord::nearest_lane(y, N, 3.5, false, so) == i);
        }
    }
    /* ego 起步位 y=-1.75 → 单向 4 车道 idx0（最内侧真实车道） */
    assert(planning_coord::nearest_lane(-1.75, 4, 3.5, false, so1) == 0);

    const double x[] = {0.0, 10.0, 20.0};
    const double y[] = {0.0, 10.0, 20.0};
    const double s[] = {0.0, std::sqrt(200.0), 2.0 * std::sqrt(200.0)};
    planning_coord::Projection p;
    assert(planning_coord::project_to_path(5.0, 7.0, x, y, s, 3, p));
    assert(nearly_equal(p.d, std::sqrt(2.0)));
    assert(nearly_equal(p.ref_x, 6.0));
    assert(nearly_equal(p.ref_y, 6.0));

    double d = 0.0;
    assert(planning_coord::quintic_lane_change(-5.25, -1.75, 50.0, 0.0, d));
    assert(nearly_equal(d, -5.25));
    assert(planning_coord::quintic_lane_change(-5.25, -1.75, 50.0, 25.0, d));
    assert(nearly_equal(d, -3.5));
    assert(planning_coord::quintic_lane_change(-5.25, -1.75, 50.0, 50.0, d));
    assert(nearly_equal(d, -1.75));
    return 0;
}
