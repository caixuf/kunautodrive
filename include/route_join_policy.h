#pragma once

#include <cmath>

namespace flowengine {

/** 航向差绝对值，归一化到 [0, π]。route 桥接与单测共用。 */
inline double heading_delta_abs(double a, double b) {
    constexpr double kPi = 3.14159265358979323846;
    double d = a - b;
    while (d > kPi) d -= 2.0 * kPi;
    while (d < -kPi) d += 2.0 * kPi;
    return std::fabs(d);
}

/**
 * 匝道停在主路端点旁的加速车道上：横向分开、航向接近、沿路方向仍在端点附近。
 * 这种接口不生成横切主路的虚拟桥。
 */
inline bool beside_accel_join(double gap_m, double heading_delta_rad, double along_m) {
    /* Must be predominantly lateral. A 6-24m longitudinal gap (missing OSM
     * link / ramp stopping short of the mainline) is a bridge case: treating
     * it as "beside" skips the fillet and makes align_ref_path_step translate
     * the whole far side by the longitudinal jump. */
    if (gap_m <= 6.0 || gap_m >= 24.0 || heading_delta_rad >= 0.55) return false;
    if (std::fabs(along_m) > 0.5 * gap_m) return false;
    return std::fabs(along_m) < 35.0;
}

/**
 * 消除 beside_accel_join 接口在 road/ref_path 中产生的横向台阶跳变，
 * 避免 Catmull-Rom / Frenet 拟合在接口前生成虚假大曲率尖峰。
 * 根据自车当前位置所在侧，将另一侧折线平移对齐。
 */
inline bool align_ref_path_step(double* x, double* y, int n,
                                double ego_x, double ego_y) {
    if (!x || !y || n < 4) return false;
    int jump_k = -1;
    for (int k = 1; k + 2 < n; ++k) {
        const double dx = x[k + 1] - x[k];
        const double dy = y[k + 1] - y[k];
        const double gap = std::hypot(dx, dy);
        if (gap <= 6.0 || gap >= 24.0) continue;
        const double h0 = std::atan2(y[k] - y[k - 1], x[k] - x[k - 1]);
        const double h1 = std::atan2(y[k + 2] - y[k + 1], x[k + 2] - x[k + 1]);
        if (heading_delta_abs(h0, h1) < 0.55) {
            jump_k = k;
            break;
        }
    }
    if (jump_k < 0) return false;

    const double tx = x[jump_k] - x[jump_k - 1];
    const double ty = y[jump_k] - y[jump_k - 1];
    const double along = (ego_x - x[jump_k]) * tx + (ego_y - y[jump_k]) * ty;

    const double shift_x = x[jump_k + 1] - x[jump_k];
    const double shift_y = y[jump_k + 1] - y[jump_k];
    if (along <= 0.0) {
        for (int i = jump_k + 1; i < n; ++i) {
            x[i] -= shift_x;
            y[i] -= shift_y;
        }
    } else {
        for (int i = 0; i <= jump_k; ++i) {
            x[i] += shift_x;
            y[i] += shift_y;
        }
    }
    return true;
}

}  // namespace flowengine
