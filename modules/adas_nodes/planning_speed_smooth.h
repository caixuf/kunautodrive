#pragma once

#include <cmath>
#include <cstddef>

namespace planning_speed {

/**
 * Linearly sample a 1D speed profile `v_grid[0..grid_n-1]` defined at
 * `s = i * s_res` for query arc-length `s_query`.
 */
inline double sample_speed_linear(const double* v_grid, int grid_n, double s_res, double s_query) {
    if (!v_grid || grid_n <= 0) return 0.0;
    if (grid_n == 1 || s_res <= 1e-9 || s_query <= 0.0) return v_grid[0];
    const double pos = s_query / s_res;
    const int i0 = static_cast<int>(std::floor(pos));
    if (i0 >= grid_n - 1) return v_grid[grid_n - 1];
    if (i0 < 0) return v_grid[0];
    const double frac = pos - static_cast<double>(i0);
    return v_grid[i0] + frac * (v_grid[i0 + 1] - v_grid[i0]);
}

/**
 * Apply curvature lateral-acceleration cap + forward/backward kinematic
 * acceleration/deceleration smoothing to a trajectory's speed profile, and
 * populate `a_out[i]` and `jerk_out[i]` consistently.
 *
 * Works in-place on `v_io[0..n-1]`.
 */
inline void smooth_speed_profile(
    const double* s_pts,
    const double* kappa_pts,
    int n,
    double a_lat_max,
    double a_accel_max,
    double a_decel_max,
    double env_v_max,
    double* v_io,
    double* a_out = nullptr,
    double* jerk_out = nullptr)
{
    if (!s_pts || !v_io || n <= 0) return;

    for (int i = 0; i < n; i++) {
        if (v_io[i] < 0.0) v_io[i] = 0.0;
        if (env_v_max > 0.0 && v_io[i] > env_v_max) v_io[i] = env_v_max;
        if (kappa_pts) {
            const double k = std::fabs(kappa_pts[i]);
            if (k > 1e-6 && a_lat_max > 0.0) {
                const double vmax = 0.9 * std::sqrt(a_lat_max / k);
                if (v_io[i] > vmax) v_io[i] = vmax;
            }
        }
    }

    // Forward pass: limit longitudinal acceleration dv^2 / (2 ds) <= a_accel_max
    if (a_accel_max > 0.0) {
        for (int i = 1; i < n; i++) {
            const double ds = s_pts[i] - s_pts[i - 1];
            if (ds > 1e-4) {
                const double v_cap = std::sqrt(v_io[i - 1] * v_io[i - 1] + 2.0 * a_accel_max * ds);
                if (v_io[i] > v_cap) v_io[i] = v_cap;
            }
        }
    }

    // Backward pass: limit longitudinal deceleration so curve/stop braking starts smoothly
    if (a_decel_max > 0.0) {
        for (int i = n - 2; i >= 0; i--) {
            const double ds = s_pts[i + 1] - s_pts[i];
            if (ds > 1e-4) {
                const double v_cap = std::sqrt(v_io[i + 1] * v_io[i + 1] + 2.0 * a_decel_max * ds);
                if (v_io[i] > v_cap) v_io[i] = v_cap;
            }
        }
    }

    if (a_out) {
        for (int i = 0; i < n; i++) a_out[i] = 0.0;
        for (int i = 1; i < n; i++) {
            const double ds = s_pts[i] - s_pts[i - 1];
            if (ds > 1e-4) {
                a_out[i] = (v_io[i] * v_io[i] - v_io[i - 1] * v_io[i - 1]) / (2.0 * ds);
            }
        }
        if (n >= 2) a_out[0] = a_out[1];
    }

    if (jerk_out && a_out) {
        for (int i = 0; i < n; i++) jerk_out[i] = 0.0;
        for (int i = 1; i < n; i++) {
            const double ds = s_pts[i] - s_pts[i - 1];
            const double v_avg = 0.5 * (v_io[i] + v_io[i - 1]);
            if (ds > 1e-4 && v_avg > 0.1) {
                const double dt = ds / v_avg;
                jerk_out[i] = (a_out[i] - a_out[i - 1]) / dt;
            }
        }
    }
}

}  // namespace planning_speed

