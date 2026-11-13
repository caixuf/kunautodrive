#include "planning_speed_smooth.h"

#include <cmath>
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

}  // namespace

int main() {
    test_linear_sampling_interpolates_smoothly();
    test_backward_decel_smooths_curve_entry();
    std::printf("test_planning_speed_smooth: all 2 tests passed\n");
    return 0;
}

