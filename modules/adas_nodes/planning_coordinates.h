#ifndef FLOWENGINE_PLANNING_COORDINATES_H
#define FLOWENGINE_PLANNING_COORDINATES_H

#include "road_geometry.h"

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
    return ::lane_group_side_offset(lane_count, lane_width, road_oneway ? 1 : 0);
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
    /* 保留旧语义：单车道路段直接取车道组偏移（road_geometry 的
     * lane_center_y 在 N<=1 时返回 road_c，会丢掉 side_offset）。 */
    if (lane_count <= 1) return side_offset;
    return ::lane_center_y(lane_idx, lane_count, lane_width, 0.0, side_offset);
}

inline int first_legal_lane(int lane_count, bool road_oneway) {
    return road_oneway ? 0 : lane_count / 2;
}

/**
 * 从 OpenDRIVE 可行驶 lane_id 集合推导道路单双向标志。
 * 右行道 id<0 为顺向、id>0 为对向：若同含正负 id 则为双向（false），
 * 仅含单侧非零 id（如单向双车道 [-1, -2]）则为单向（true），空数组回退 fallback_oneway。
 */
inline bool infer_oneway_from_lane_ids(const int* lane_ids, int count,
                                       bool fallback_oneway) {
    if (!lane_ids || count <= 0) return fallback_oneway;
    bool has_pos = false;
    bool has_neg = false;
    for (int i = 0; i < count; ++i) {
        if (lane_ids[i] > 0) has_pos = true;
        else if (lane_ids[i] < 0) has_neg = true;
    }
    if (has_neg && has_pos) return false;
    if (has_neg || has_pos) return true;
    return fallback_oneway;
}

/** 本向可用车道数（单向路为全部 N，双向路为右侧半幅 N - N/2）。 */
inline int own_direction_lane_count(int lane_count, bool road_oneway) {
    if (lane_count <= 0) return 0;
    return road_oneway ? lane_count
                       : (lane_count - first_legal_lane(lane_count, false));
}

/**
 * 判断车道中心偏移 target_d 是否落在当前路段（含单向 side_offset 偏置）合法宽度内。
 */
inline bool is_lane_d_in_legal_band(double target_d, int lane_count,
                                    double lane_width, double side_offset,
                                    double edge_margin_m = 0.5) {
    const double half_band = lane_count * 0.5 * lane_width - edge_margin_m;
    if (half_band < 0.0) return false;
    return std::fabs(target_d - side_offset) <= half_band;
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

/**
 * 巡航车道索引滞回。
 *
 * nearest_lane 在车道分界上把索引阶跃到邻道。贴线时候选每帧翻转，
 * 规划把整段横向参考锁到新中心，横向误差变号，车沿分界摇摆。
 *
 * held_idx < 0 或越界：尚未锁存，直接采纳候选。
 * 候选与锁存不同道时，自车必须比锁存中心更靠近候选中心至少一个
 * 0.25 车道宽才切换。3.5m 车道上即越过分界约 0.44m。
 * 显式变道不走这里，由调用方直接写入目标车道。
 */
inline int hold_lane_index(double ego_d, int candidate_idx, int held_idx,
                           int lane_count, double lane_width,
                           double side_offset = 0.0) {
    if (lane_count <= 0 || !(lane_width > 0.0)) return 0;
    if (candidate_idx < 0 || candidate_idx >= lane_count) candidate_idx = 0;
    if (held_idx < 0 || held_idx >= lane_count) return candidate_idx;
    if (candidate_idx == held_idx) return held_idx;
    const double cand = lane_center_d(candidate_idx, lane_count, lane_width, side_offset);
    const double held = lane_center_d(held_idx, lane_count, lane_width, side_offset);
    const double margin = lane_width * 0.25;
    if (std::fabs(ego_d - cand) + margin < std::fabs(ego_d - held))
        return candidate_idx;
    return held_idx;
}

/**
 * 参考系一致性：两个航向是否同向（|wrap(a − b)| ≤ max_misalign_rad）。
 *
 * 用于 `target_lane_offset` 的真消费前置条件。target = ego_d − llt_offset 只在
 * **参考线法向与 lanelet 法向同向**时成立：双向路返程半幅 / 掉头中段两法向
 * 反平行（夹角 π），减法变加法，误差 = −2·llt_offset（随自车偏离车道中心线性
 * 放大）。故判据取 ref 航向与 lane 航向，二者都是几何量、在掉头窗口内恒定，
 * 不会像"自车航向 vs 参考航向"那样随自车扫过阈值来回翻转（实测那样做会把
 * steer_flip_rate 从 0.111 推到 0.200）。
 *
 * 2026-10-23 实测（straight_road 开场掉头）：正常巡航/变道 ref 与 lane 夹角
 * ≤ 0.09 rad；掉头中段 3.00–3.14 rad —— 阈值 45°(0.785) 两侧余量充足。
 */
inline bool lane_ref_aligned(double a_heading, double b_heading,
                             double max_misalign_rad = 0.7853981633974483) {
    constexpr double kPi = 3.14159265358979323846;
    double d = a_heading - b_heading;
    while (d > kPi) d -= 2.0 * kPi;
    while (d < -kPi) d += 2.0 * kPi;
    return std::fabs(d) <= max_misalign_rad;
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

/**
 * 车道保持轨迹横向剖面去偏滞（抑制 Frenet-Control 闭环稳态误差放大）。
 *
 * `frenet_plan` 每周期以当前横向偏移 `ic.c_d = ego_ref_d` 为起点规划五次多项式；
 * 若末端目标仍落在无障碍干扰的车道中心（`|d_out[n_wp - 1]| <= 0.25m`），0.5s
 * 前瞻点仍保留约 90% 的 `ego_ref_d`，将弯道微小稳态偏差放大 ~10 倍并反复触发
 * `[LANE_EDGE]` 阶跃跳变。此时将整条车道保持剖面归零到车道中心，由下游控制器
 * 平稳收敛；仅在规划主动偏出中心（如车内避让）时保留 `safe_band` 边界裁剪。
 */
inline void resolve_lane_keep_d_profile(double* d_out, int n_wp,
                                        double safe_band,
                                        int* out_forced_count = nullptr) {
    if (out_forced_count) *out_forced_count = 0;
    if (!d_out || n_wp <= 0) return;

    constexpr double kUnobstructedTerminalTol = 0.25;
    if (std::fabs(d_out[n_wp - 1]) <= kUnobstructedTerminalTol) {
        for (int i = 0; i < n_wp; ++i) d_out[i] = 0.0;
        return;
    }

    const double clamped_band = safe_band > 0.0 ? safe_band : 0.0;
    int forced = 0;
    for (int i = 0; i < n_wp; ++i) {
        if (std::fabs(d_out[i]) > clamped_band) {
            d_out[i] = 0.0;
            forced++;
        }
    }
    if (out_forced_count) *out_forced_count = forced;
}

/**
 * 轨迹点航向角重构（消除弯道前向弦偏置，保留变道/避让 `dl/ds` 切向倾角）。
 *
 * 前向差分 `atan2(p[i+1] - p[i])` 在半径 R 弯道上产生 `-ds/(2R)` 的系统性内偏；
 * 改用：
 * - 内点 `1 <= i < n_pts - 1`：对称中心差分 `p[i+1] - p[i-1]`（圆弧切线零偏置，
 *   同时包含变道横向梯度）；
 * - 端点 `0` / `n_pts - 1`：当 `ref_valid[i]` 为真时以参考线切线 `ref_h[i]` 叠加
 *   邻点横向斜率 `atan2(dl, max(ds, 0.2))`；否则回退相邻弦方向。
 * 调用方应先将 `out_heading[i]` 初始化为回退航向（如 `points[i].heading`）。
 */
inline void refine_trajectory_headings(const double* x, const double* y,
                                       const double* s, const double* l,
                                       const double* ref_h,
                                       const bool* ref_valid,
                                       int n_pts,
                                       double* out_heading) {
    if (!x || !y || !s || !l || !out_heading || n_pts <= 0) return;
    if (n_pts == 1) {
        if (ref_h && ref_valid && ref_valid[0]) out_heading[0] = ref_h[0];
        return;
    }

    constexpr double kMinChordLen2 = 0.04;
    constexpr double kMinDs = 0.2;

    if (ref_h && ref_valid && ref_valid[0]) {
        const double ds01 = s[1] - s[0];
        const double dl01 = l[1] - l[0];
        out_heading[0] = ref_h[0] + (ds01 > kMinDs ? std::atan2(dl01, ds01) : 0.0);
    } else {
        const double dx = x[1] - x[0];
        const double dy = y[1] - y[0];
        if (dx * dx + dy * dy > kMinChordLen2) {
            out_heading[0] = std::atan2(dy, dx);
        }
    }

    for (int i = 1; i + 1 < n_pts; ++i) {
        const double dx = x[i + 1] - x[i - 1];
        const double dy = y[i + 1] - y[i - 1];
        if (dx * dx + dy * dy > kMinChordLen2) {
            out_heading[i] = std::atan2(dy, dx);
        } else if (ref_h && ref_valid && ref_valid[i]) {
            out_heading[i] = ref_h[i];
        }
    }

    const int last = n_pts - 1;
    if (ref_h && ref_valid && ref_valid[last]) {
        const double dsn = s[last] - s[last - 1];
        const double dln = l[last] - l[last - 1];
        out_heading[last] = ref_h[last] + (dsn > kMinDs ? std::atan2(dln, dsn) : 0.0);
    } else {
        out_heading[last] = out_heading[last - 1];
    }
}

/**
 * Clamp lane-relative lateral offset `rel_d` on two-way roads so Frenet
 * avoidance/nudge trajectories never cross the double-yellow centerline.
 */
inline double clamp_double_yellow_lateral(double target_lane_offset,
                                          double rel_d,
                                          bool road_oneway,
                                          double center_guard_m = 0.45) {
    if (road_oneway) return rel_d;
    const double abs_d = target_lane_offset + rel_d;
    if (target_lane_offset < -0.1 && abs_d > -center_guard_m) {
        return -center_guard_m - target_lane_offset;
    }
    if (target_lane_offset > 0.1 && abs_d < center_guard_m) {
        return center_guard_m - target_lane_offset;
    }
    return rel_d;
}

}  // namespace planning_coord

#endif
