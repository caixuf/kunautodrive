#ifndef FLOWENGINE_PLANNING_COORDINATES_H
#define FLOWENGINE_PLANNING_COORDINATES_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace planning_coord {

struct Projection {
    double s{0.0};
    double d{0.0};
    double ref_x{0.0};
    double ref_y{0.0};
    double heading{0.0};
};

/**
 * 车道组整体横向偏移（相对道路参考线 y=road_c）。
 *
 * 双向路：车道对称铺在参考线两侧，偏移 0（既有行为）。
 * 单向路：OpenDRIVE/靠右行驶下所有车道都在参考线的 -y 侧（lane_id=-1..-N），
 *   参考线是道路**最内侧车道的内边界**而非几何中心。此时车道组整体向 -y
 *   偏移 -N·w/2，使 idx0 落在最内侧真实车道中心（y=-w/2），
 *   idx N-1 落在最外侧（y=-(N-1)·w-w/2）。
 *
 * 与 include/road_geometry.h::lane_center_y(..., side_offset) 同一语义（单一事实源）：
 *   road_geometry.h docstring 例：N=4, lane_width=3.5, side_offset=-7.0
 *   → 车道中心 -1.75/-5.25/-8.75/-12.25，正是单向 4 车道物理布局。
 *
 * @param lane_count 车道数 N（≥1）
 * @param lane_width 单车道宽度 (m)
 * @param road_oneway 1=单向（全部车道同向），0=双向（跨参考线=对向）
 */
inline double lane_group_side_offset(int lane_count, double lane_width,
                                     bool road_oneway) {
    if (!road_oneway || lane_count <= 1) return 0.0;
    return -lane_count * lane_width * 0.5;
}

/**
 * 车道中心相对道路参考线的横向偏移。
 *
 *   lane_center_d = side_offset - (idx - (N-1)/2)·w
 *
 * side_offset 见 lane_group_side_offset：双向路取 0（对称，历史行为不变）；
 * 单向路取 -N·w/2（车道组整体偏到 -y 侧，与 flowsim 物理车道对齐）。
 *
 * 约定：idx 增大 → y 减小（向右/外侧）。单向路 idx0 = 最内侧（y 最接近 0），
 * idx N-1 = 最外侧（y 最负）。
 */
inline double lane_center_d(int lane_idx, int lane_count, double lane_width,
                            double side_offset = 0.0) {
    if (lane_count <= 1) return side_offset;
    return side_offset - (lane_idx - (lane_count - 1) * 0.5) * lane_width;
}

inline int first_legal_lane(int lane_count, bool road_oneway) {
    return road_oneway ? 0 : lane_count / 2;
}

/**
 * 由横向偏移 d 反推最近车道索引 [0, lane_count-1]。
 *
 * 为 lane_center_d 的严格逆（round-trip 必须成立），同样接受 side_offset：
 *   raw = (side_offset - d) / w + (N-1)/2
 *
 * own_side_only=true 时 clamp 下限到 N/2（双向路本向半幅）；单向路调用方传
 * false，使全部车道可选（此时 clamp 到 [0, N-1]）。
 */
inline int nearest_lane(double d, int lane_count, double lane_width,
                        bool own_side_only = true, double side_offset = 0.0) {
    if (lane_count <= 0 || lane_width <= 0.0) return 0;
    double raw = (side_offset - d) / lane_width + (lane_count - 1) * 0.5;
    int lane = static_cast<int>(raw >= 0.0 ? raw + 0.5 : raw - 0.5);
    lane = std::max(own_side_only ? lane_count / 2 : 0, lane);
    return std::min(lane_count - 1, lane);
}

inline int nearest_own_lane(double d, int lane_count, double lane_width) {
    return nearest_lane(d, lane_count, lane_width, true);
}

inline bool project_to_path(double x, double y,
                            const double* ref_x, const double* ref_y,
                            const double* ref_s, int count,
                            Projection& out) {
    if (!ref_x || !ref_y || !ref_s || count < 2) return false;

    double best_dist2 = 1e300;
    bool found = false;
    for (int i = 0; i + 1 < count; ++i) {
        const double vx = ref_x[i + 1] - ref_x[i];
        const double vy = ref_y[i + 1] - ref_y[i];
        const double len2 = vx * vx + vy * vy;
        if (len2 <= 1e-9) continue;

        const double t = std::clamp(
            ((x - ref_x[i]) * vx + (y - ref_y[i]) * vy) / len2, 0.0, 1.0);
        const double px = ref_x[i] + t * vx;
        const double py = ref_y[i] + t * vy;
        const double dx = x - px;
        const double dy = y - py;
        const double dist2 = dx * dx + dy * dy;
        if (dist2 >= best_dist2) continue;

        const double heading = std::atan2(vy, vx);
        out.s = ref_s[i] + t * std::sqrt(len2);
        out.d = -dx * std::sin(heading) + dy * std::cos(heading);
        out.ref_x = px;
        out.ref_y = py;
        out.heading = heading;
        best_dist2 = dist2;
        found = true;
    }
    return found;
}

inline bool quintic_lane_change(double start_d, double target_d,
                                double length, double s, double& out_d) {
    if (length <= 1e-6) return false;
    const double delta = target_d - start_d;
    if (std::fabs(delta) <= 0.2 || std::fabs(delta) >= 8.0) return false;
    const double u = std::clamp(s / length, 0.0, 1.0);
    const double blend = u * u * u * (10.0 + u * (-15.0 + 6.0 * u));
    out_d = start_d + delta * blend;
    return true;
}

}  // namespace planning_coord

#endif
