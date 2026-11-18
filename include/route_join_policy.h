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
 *
 * 根据自车当前位置所在侧平移另一侧折线，消除平行 beside-join 的横向
 * 台阶。仅处理道路/匝道级别的接口（gap > 9m）；泊车位到车道 ~6.5m 的
 * 横向跳变不是道路接口，若按接口平移会把整条返程路线横移一个车位
 * 宽度（实测 office park-out 后西行路线整体南移 6.5m，卡在高架边缘）。
 */
inline bool align_ref_path_step(double* x, double* y, int n,
                                double ego_x, double ego_y,
                                bool taper_far_side = false,
                                bool force_far_side = false) {
    if (!x || !y || n < 4) return false;
    int jump_k = -1;
    for (int k = 1; k + 2 < n; ++k) {
        const double dx = x[k + 1] - x[k];
        const double dy = y[k + 1] - y[k];
        const double gap = std::hypot(dx, dy);
        /* Only road/ramp beside-joins (≈12m lane-group offset). A parking
         * slot-to-aisle hop is ~6.5m; treating that as a join shifts the
         * entire return route sideways (observed: office park-out shifted
         * the westbound route 6.5m south, ego wedged at the viaduct edge). */
        if (gap <= 9.0 || gap >= 24.0) continue;
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
    if (force_far_side || along <= 0.0) {
        for (int i = jump_k + 1; i < n; ++i) {
            x[i] -= shift_x;
            y[i] -= shift_y;
        }
        /* Return leg: the raw route jumps from the ramp lane to the viaduct
         * westbound lane. Aligning the far side keeps the path continuous at
         * the join, but staying on the entry carriageway is wrong-way. Blend
         * back to the true far-side lane over the next ~40m so the ego merges
         * across instead of taking a 12m step (which it cannot track at
         * speed and ends in an off-rails U-turn). */
        if (taper_far_side && !force_far_side) {
            constexpr int kTaperPoints = 8;
            for (int t = 1; t <= kTaperPoints && jump_k + 1 + t < n; ++t) {
                const double w = static_cast<double>(t) / static_cast<double>(kTaperPoints);
                x[jump_k + 1 + t] += shift_x * w;
                y[jump_k + 1 + t] += shift_y * w;
            }
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
