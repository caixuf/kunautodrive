#include "planning_coordinates.h"

#include <cassert>
#include <cmath>
#include <cstdio>

static int g_fails = 0;

static void expect(bool ok, const char* msg) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", msg);
    g_fails++;
}

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

    /* 20:49 实测：自车贴在 idx2(-1.75) 与 idx3(-5.25) 的分界 -3.5 上，
     * nearest_lane 每帧翻转，横向参考阶跃。滞回必须保持原车道。 */
    {
        int prev = planning_coord::nearest_lane(-3.52, 4, 3.5, false, 0.0);
        int held = prev;
        int flips_nearest = 0;
        int flips_hold = 0;
        for (int i = 0; i < 40; ++i) {
            const double ego = (i % 2) ? -3.48 : -3.52;
            const int cand = planning_coord::nearest_lane(ego, 4, 3.5, false, 0.0);
            if (cand != prev) flips_nearest++;
            prev = cand;
            const int next = planning_coord::hold_lane_index(
                ego, cand, held, 4, 3.5, 0.0);
            if (next != held) flips_hold++;
            held = next;
        }
        expect(flips_nearest >= 10, "nearest_lane must chatter across the lane boundary");
        expect(flips_hold == 0, "hold_lane_index must not chatter on the boundary");
        expect(held == planning_coord::nearest_lane(-3.52, 4, 3.5, false, 0.0),
               "hold stays on the lane it started on");
    }

    /* 从 -1.75 开到 -5.25 再开回来：每个方向只切换一次，且在越过分界之后。 */
    {
        int held = 2;
        int switches = 0;
        double switch_at = 0.0;
        for (double ego = -1.75; ego >= -5.25; ego -= 0.05) {
            const int cand = planning_coord::nearest_lane(ego, 4, 3.5, false, 0.0);
            const int next = planning_coord::hold_lane_index(
                ego, cand, held, 4, 3.5, 0.0);
            if (next != held) {
                switches++;
                switch_at = ego;
                held = next;
            }
        }
        expect(switches == 1, "outbound sweep switches once");
        expect(held == 3, "outbound sweep ends on lane 3");
        expect(switch_at < -3.5, "outbound switch is past the boundary");

        switches = 0;
        for (double ego = -5.25; ego <= -1.75; ego += 0.05) {
            const int cand = planning_coord::nearest_lane(ego, 4, 3.5, false, 0.0);
            const int next = planning_coord::hold_lane_index(
                ego, cand, held, 4, 3.5, 0.0);
            if (next != held) {
                switches++;
                switch_at = ego;
                held = next;
            }
        }
        expect(switches == 1, "return sweep switches once");
        expect(held == 2, "return sweep ends on lane 2");
        expect(switch_at > -3.5, "return switch is past the boundary");
    }

    /* 未锁存（-1）立刻采纳候选。 */
    expect(planning_coord::hold_lane_index(-5.25, 3, -1, 4, 3.5, 0.0) == 3,
           "unset hold adopts the candidate");

    /* Prefer offgrid-fix / deepen2 RCA: ref vs lanelet anti-parallel must reject.
     * target = ego_d - llt_offset only holds for co-directed normals; U-turn mid
     * angle ~pi must fall through to A11 lane-center grid. Geometric headings only. */
    {
        constexpr double kPi = 3.14159265358979323846;
        expect(planning_coord::lane_ref_aligned(0.0, 0.05),
               "co-directed small misalign stays aligned");
        expect(planning_coord::lane_ref_aligned(0.0, 0.09),
               "cruise/LC misalign <=0.09 stays aligned");
        expect(!planning_coord::lane_ref_aligned(0.0, kPi),
               "exact anti-parallel (U-turn mid) rejected");
        expect(!planning_coord::lane_ref_aligned(0.0, 3.00),
               "deepen2 measured U-turn mid ~3.00 rad rejected");
        expect(!planning_coord::lane_ref_aligned(0.0, 3.14),
               "deepen2 measured U-turn mid ~3.14 rad rejected");
        expect(!planning_coord::lane_ref_aligned(0.1, 0.1 + kPi),
               "return-half anti-parallel with small offset rejected");
        expect(planning_coord::lane_ref_aligned(kPi - 0.05, -kPi + 0.05),
               "wrap-around near +/-pi still co-directed");
        expect(!planning_coord::lane_ref_aligned(0.0, 0.9),
               "above 45deg default threshold rejected");

        const double lane_w = 3.5;
        const double ego_d = 4.66;
        const double llt_offset = 0.570;
        const double bad_target = ego_d - llt_offset;
        const double phase = std::fmod(std::fabs(bad_target), lane_w);
        const double half = lane_w * 0.5;
        const double phase_err = std::min(std::fabs(phase - half),
                                          std::min(phase, lane_w - phase));
        expect(phase_err > 0.5, "anti-parallel formula lands off lane-center grid");

        const int cand = planning_coord::nearest_lane(ego_d, 4, lane_w, false, 0.0);
        const double grid = planning_coord::lane_center_d(cand, 4, lane_w, 0.0);
        const double gphase = std::fmod(std::fabs(grid), lane_w);
        const double gerr = std::min(std::fabs(gphase - half),
                                     std::min(gphase, lane_w - gphase));
        expect(gerr <= 1e-9, "A11 nearest-lane center stays on legal grid");
        (void)bad_target;
    }

    return g_fails == 0 ? 0 : 1;
}
