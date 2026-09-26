/**
 * lane_match_helpers.cpp — lane_match M3 字段纯算法 helper 实现
 *
 * 对应 header: modules/adas_nodes/flowsim/lane_match_helpers.h
 * 单元测试入口: tests/test_adas_nodes_logic.c（extern "C" 声明调）
 *
 * 与 flowsim_node.cpp::compute_lane_match() 的关系：
 *   - compute_lane_match 做 esmini 路网查询（world_to_frenet / frenet_to_world /
 *     lane_width / drivable_lane_count / RM_GetDrivableLaneIdByIndex）后，
 *     把数据传给本文件里的纯算法函数做最终输出
 *   - 本文件的函数不知道 esmini 存在，可在 C 测试里直接喂合成数据验证
 *
 * 演化路径（D2-05 design §9）：
 *   - M3 step 2 起 `flowsim_lm_synthesize_lanelet_id` 退役，换
 *     `lanelet::Lanelet::id()`（要等 flowsim 加载 LaneletMap）
 *   - M3 step 3 起 `flowsim_lm_pick_adjacent_lane_id` 可换 LaneletMap 拓扑查
 */

#include "lane_match_helpers.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>   // std::abs

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 三点共线/重合的数值防护阈值（曲率分母 < 此值时返回 0）。
 * 选 1e-12 而不是 1e-9：Menger 公式分子分母都是边长乘积，量级约 |a|·|b|·|c|
 * ≈ 10^-3 量级（在 m 单位下），1e-12 给我们足够安全裕量但不放过真实共线。 */
static constexpr double LM_CURVATURE_DEGEN_EPS = 1e-12;

double flowsim_lm_curvature_3pt(double x1, double y1,
                                double x2, double y2,
                                double x3, double y3) {
    /* 边向量：a = P_2 - P_1, b = P_3 - P_1, c = P_3 - P_2
     * 三角形面积 = |a × b| / 2，a × b 的 z 分量 = ax*by - ay*bx
     * Menger 曲率 κ = 4·Area / (|a|·|b|·|c|) = 2·cross / (|a|·|b|·|c|) */
    const double ax = x2 - x1;
    const double ay = y2 - y1;
    const double bx = x3 - x1;
    const double by = y3 - y1;
    const double cx = x3 - x2;
    const double cy = y3 - y2;

    const double a_len = std::hypot(ax, ay);
    const double b_len = std::hypot(bx, by);
    const double c_len = std::hypot(cx, cy);

    /* 任一边长过小 ⇒ 两点重合 ⇒ 退化 → 0（无意义） */
    if (a_len < 1e-9 || b_len < 1e-9 || c_len < 1e-9) return 0.0;

    /* 分母 = |a|·|b|·|c|；太小 → 数值不稳定 → 0 */
    const double denom = a_len * b_len * c_len;
    if (denom < LM_CURVATURE_DEGEN_EPS) return 0.0;

    /* 分子 = 2·cross（z 分量）= 4·有向三角形面积 */
    const double cross = ax * by - ay * bx;

    /* 显式防 NaN/Inf：std::isfinite 检查（finite 值才会通过） */
    const double k = (2.0 * cross) / denom;
    if (!std::isfinite(k)) return 0.0;

    return k;
}

uint64_t flowsim_lm_synthesize_lanelet_id(int road_id, int lane_id) {
    /* M2 占位 / M3 step 1（仍无 Lanelet2）：road_id*1000 + (lane_id+500)
     * 见 D2-05 design §4 锁定。调用方对失败路径（world_to_frenet 返回
     * false 或 road_id<0）必须显式置 0，否则合成 llt_id=500 等假阳性。 */
    if (road_id < 0) return 0ULL;
    const long long lane_offset = static_cast<long long>(lane_id) + 500LL;
    if (lane_offset < 0) return 0ULL;   /* lane_id < -500 时合成负数 → 不合法 */
    const unsigned long long road_part =
        static_cast<unsigned long long>(road_id) * 1000ULL;
    return road_part + static_cast<unsigned long long>(lane_offset);
}

int flowsim_lm_pick_adjacent_lane_id(int current_lane,
                                     const int* candidate_ids, int n,
                                     int side) {
    if (candidate_ids == nullptr || n <= 0) return 0;
    if (side == 0) return 0;

    /* closest-on-side：side=+1（左邻）⇒ 选最小的 > current_lane 的 id；
     * side=-1（右邻）⇒ 选最大的 < current_lane 的 id。 */
    int best = 0;
    int best_dist = 0;
    for (int i = 0; i < n; ++i) {
        const int lid = candidate_ids[i];
        const int delta = lid - current_lane;
        if (delta == 0) continue;   /* 不和自己比（理论上候选不含 current，但防御） */
        if (side > 0) {
            if (delta <= 0) continue;   /* 必须严格在 current 右侧（id 更大 = 左边）*/
            if (best == 0 || delta < best_dist) { best = lid; best_dist = delta; }
        } else {
            if (delta >= 0) continue;   /* 必须严格在 current 左侧（id 更小 = 右边）*/
            if (best == 0 || -delta < best_dist) { best = lid; best_dist = -delta; }
        }
    }
    /* abs(best_dist) 防御：候选 id 与 current_lane 相距过远（>1e6）可能是脏数据，
     * 视为无邻。1e6 上限远高于实际车道 id 间距（OpenDRIVE 一般 |Δlane_id| < 20）。*/
    if (best != 0 && std::abs(best_dist) > 1000000) return 0;
    return best;
}

void flowsim_lm_zero_outputs_on_failure(
    uint64_t* out_llt_id, double* out_llt_s, double* out_llt_offset,
    double* out_heading_err, int* out_valid,
    double* out_curvature, double* out_lane_width,
    uint64_t* out_left_lanelet_id, uint64_t* out_right_lanelet_id,
    uint32_t* out_flags) {
    if (out_llt_id)            *out_llt_id = 0ULL;
    if (out_llt_s)             *out_llt_s = 0.0;
    if (out_llt_offset)        *out_llt_offset = 0.0;
    if (out_heading_err)       *out_heading_err = 0.0;
    if (out_valid)             *out_valid = 0;
    if (out_curvature)         *out_curvature = 0.0;
    if (out_lane_width)        *out_lane_width = 0.0;
    if (out_left_lanelet_id)   *out_left_lanelet_id = 0ULL;
    if (out_right_lanelet_id)  *out_right_lanelet_id = 0ULL;
    if (out_flags)             *out_flags = 0U;
}
