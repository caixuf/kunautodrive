/**
 * test_planning_fallback_when_match_invalid.cpp — D2-06 / A11 验收单测:
 *
 * 验证 localization/lane_match 真消费 + 失效降级 (A11 铁律)：
 *   - 当 valid=1 且 llt_id!=0 且新鲜 且 |llt_offset|<=lane_w/2-0.1：
 *       target = ego_lane_d - llt_offset（lanelet 中心，不再网格取整）
 *   - 当 valid=0 时：走 fallback 巡航 (nearest_lane 网格中心绝对 d)
 *   - 当 lane_match 超时 (>500ms) 或越界 (|offset| > max_lane_offset_m) 时：防御性回退
 *   - 单向路与双向路下 fallback 均严格收敛在对应合法车道中心
 */

#include "planning_coordinates.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", #expr, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

struct LaneMatchSnapshot {
    bool valid{false};
    uint64_t llt_id{0};
    double llt_s{0.0};
    double llt_offset{0.0};
    double llt_heading_err_rad{0.0};
    double curvature{0.0};
    double lane_width{3.5};
    uint64_t last_us{0};
};

/* 纯算法函数：计算 target_lane_offset，对齐 planning_node.cpp D2-06 Prefer */
static double compute_target_lane_offset(
    double ego_lane_d,
    int lane_count,
    double lane_w,
    double side_offset,
    const LaneMatchSnapshot& lm,
    uint64_t now_us,
    bool* used_lane_match = nullptr)
{
    const double lw_gate = (lm.lane_width > 1.0) ? lm.lane_width : lane_w;
    const double max_lane_offset_m = lw_gate * 0.5 - 0.1;
    const bool lane_match_fresh = lm.valid && (lm.llt_id != 0) && (lm.last_us != 0) &&
                                  (now_us - lm.last_us < 500000ULL) &&
                                  (std::fabs(lm.llt_offset) <= max_lane_offset_m);
    if (used_lane_match) *used_lane_match = lane_match_fresh;

    if (lane_match_fresh) {
        return ego_lane_d - lm.llt_offset;
    } else {
        /* A11 fallback: nearest_lane 网格中心（绝对 d，与 planning_node 一致） */
        const int lane_idx = planning_coord::nearest_lane(ego_lane_d, lane_count, lane_w, false, side_offset);
        return planning_coord::lane_center_d(lane_idx, lane_count, lane_w, side_offset);
    }
}

static bool nearly_equal(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

int main() {
    const double lane_w = 3.5;
    const int lane_count = 4;
    const double side_offset = planning_coord::lane_group_side_offset(lane_count, lane_w, true); // -7.0
    const uint64_t t0 = 1000000ULL;

    /* ── 1. 正常权威匹配路径 (valid=1, 新鲜, 合理 offset) ── */
    {
        LaneMatchSnapshot lm;
        lm.valid = true;
        lm.llt_id = 1001;
        lm.llt_offset = 0.25; // 自车偏左 0.25m
        lm.lane_width = lane_w;
        lm.last_us = t0;

        bool used_lm = false;
        double ego_d = -1.50; // 自车在 -1.50，相对目标车道中心 -1.75 偏左 0.25m
        double target_offset = compute_target_lane_offset(ego_d, lane_count, lane_w, side_offset, lm, t0 + 10000ULL, &used_lm);
        CHECK(used_lm == true);
        // target_lane_offset = ego_d - llt_offset = -1.50 - 0.25 = -1.75
        CHECK(nearly_equal(target_offset, -1.75));
    }

    /* ── 2. A11 核心: valid=0 匹配失效降级 ── */
    {
        LaneMatchSnapshot lm;
        lm.valid = false; // 显式匹配失效（如驶出路网、路口断链、遮挡）
        lm.llt_id = 0;
        lm.llt_offset = 0.0;
        lm.last_us = t0;

        bool used_lm = true;
        double ego_d = -1.50;
        double target_offset = compute_target_lane_offset(ego_d, lane_count, lane_w, side_offset, lm, t0 + 10000ULL, &used_lm);
        CHECK(used_lm == false);
        // fallback: nearest to -1.50 is lane 0 center -1.75 (absolute d)
        CHECK(nearly_equal(target_offset, -1.75));
        CHECK(!std::isnan(target_offset) && !std::isinf(target_offset));
    }

    /* ── 3. A11 核心: 匹配超时降级 (last_us 过旧 > 500ms) ── */
    {
        LaneMatchSnapshot lm;
        lm.valid = true;
        lm.llt_id = 1001;
        lm.llt_offset = 0.10;
        lm.last_us = t0;

        bool used_lm = true;
        double ego_d = -5.25; // 车道 1
        // now = t0 + 600ms (> 500ms)
        double target_offset = compute_target_lane_offset(ego_d, lane_count, lane_w, side_offset, lm, t0 + 600000ULL, &used_lm);
        CHECK(used_lm == false);
        // nearest_lane to -5.25 is lane 1 center -5.25 (absolute d)
        CHECK(nearly_equal(target_offset, -5.25));
    }

    /* ── 4. 防御性过滤: 越出 max_lane_offset_m (=lane_w/2-0.1) ── */
    {
        LaneMatchSnapshot lm;
        lm.valid = true;
        lm.llt_id = 9999;
        lm.llt_offset = 6.0; // 远大于 1.5 * 3.5 = 5.25m
        lm.last_us = t0;

        bool used_lm = true;
        double ego_d = -1.75;
        double target_offset = compute_target_lane_offset(ego_d, lane_count, lane_w, side_offset, lm, t0 + 10000ULL, &used_lm);
        CHECK(used_lm == false);
        // ego at -1.75 → lane 0 center -1.75 absolute
        CHECK(nearly_equal(target_offset, -1.75));
    }

    /* ── 5. 多车道遍历与双向路兼容性验证 ── */
    {
        LaneMatchSnapshot lm_invalid{}; // 全部默认 0, valid=false
        const double two_way_so = planning_coord::lane_group_side_offset(lane_count, lane_w, false); // 0.0

        for (int i = 0; i < lane_count; ++i) {
            double center = planning_coord::lane_center_d(i, lane_count, lane_w, two_way_so);
            bool used_lm = true;
            double off = compute_target_lane_offset(center, lane_count, lane_w, two_way_so, lm_invalid, t0, &used_lm);
            CHECK(used_lm == false);
            CHECK(nearly_equal(off, center));
        }
    }

    /* ── 6. 门禁边界: |llt_offset| == max_lane_offset_m 仍消费；越界回退 ── */
    {
        const double max_off = lane_w * 0.5 - 0.1; // 1.65
        LaneMatchSnapshot lm;
        lm.valid = true;
        lm.llt_id = 1001;
        lm.lane_width = lane_w;
        lm.last_us = t0;
        lm.llt_offset = max_off;
        bool used_lm = false;
        double ego_d = -1.75 + max_off;
        double target_offset = compute_target_lane_offset(ego_d, lane_count, lane_w, side_offset, lm, t0 + 10000ULL, &used_lm);
        CHECK(used_lm == true);
        CHECK(nearly_equal(target_offset, -1.75));

        lm.llt_offset = max_off + 0.01;
        used_lm = true;
        target_offset = compute_target_lane_offset(ego_d, lane_count, lane_w, side_offset, lm, t0 + 10000ULL, &used_lm);
        CHECK(used_lm == false);
        CHECK(nearly_equal(target_offset, -1.75));
    }

    /* ── 7. 参考系一致性前置条件（2026-10-23 横向摇摆根因） ──
     *
     * target = ego_d − llt_offset 只在 ref 法向与 lanelet 法向同向时成立。
     * 双向路返程半幅 / 掉头中段两法向反平行，误差 = −2·llt_offset。
     * 判据取 **ref 航向 vs lane 航向**（lane_h = ego_h − llt_heading_err），
     * 二者都是几何量，掉头窗口内恒定 —— 不用自车航向，否则自车扫过阈值会让
     * 分支来回翻转（实测 steer_flip_rate 0.111 → 0.200）。
     *
     * 实测锚点（straight_road 开场掉头，全部来自 demo 采样）：
     *   t=16.2 巡航  ref=0.000 lane=-0.090 → 夹角 0.090 rad → 真消费
     *   t=36.0 巡航  ref=0.016 lane= 0.011 → 夹角 0.005 rad → 真消费
     *   t= 6.8 掉头  ref=3.142 lane= 0.000 → 夹角 3.142 rad → 必须回退
     *   t= 5.5 掉头  ref=3.142 lane=-0.140 → 夹角 3.002 rad → 必须回退
     */
    {
        /* 7a. 纯函数：正常巡航/变道（ref 与 lane 同向）判 aligned */
        CHECK(planning_coord::lane_ref_aligned(0.000, -0.090));   /* 实测巡航 */
        CHECK(planning_coord::lane_ref_aligned(0.016,  0.011));   /* 实测巡航 */
        CHECK(planning_coord::lane_ref_aligned(3.14159, 3.14159)); /* 西行段同向 */

        /* 7b. 纯函数：掉头中段（ref 与 lane 反平行）必须判 not aligned */
        CHECK(!planning_coord::lane_ref_aligned(3.142,  0.000));  /* 实测掉头中段 */
        CHECK(!planning_coord::lane_ref_aligned(3.142, -0.140));  /* 实测掉头中段 */
        CHECK(!planning_coord::lane_ref_aligned(0.0, 3.14159));   /* 纯反向 */

        /* 7c. 边界：45° 内 aligned，超出 not aligned；环绕不误判 */
        const double k45 = 0.7853981633974483;
        CHECK(planning_coord::lane_ref_aligned(0.0, k45));
        CHECK(planning_coord::lane_ref_aligned(0.0, -k45));
        CHECK(!planning_coord::lane_ref_aligned(0.0, k45 + 1e-3));
        CHECK(planning_coord::lane_ref_aligned(3.0, -3.0));       /* wrap 后 0.283 rad */
        CHECK(planning_coord::lane_ref_aligned(-3.14159, 3.14159)); /* ±π 同向 */

        /* 7d. 掉头中段实测值：真消费会给出骑线值，回退给网格中心 */
        {
            const double ego_d = 4.66;          /* t=6.8 实测 */
            const double llt_offset = 0.570;    /* t=6.8 实测 */
            const double bogus = ego_d - llt_offset;   /* = 4.090，该路无此车道中心 */
            CHECK(nearly_equal(bogus, 4.090, 1e-3));
            /* 反平行下正确值应为 ego_d + llt_offset = 5.23（lanelet 中心在 ref 系） */
            CHECK(nearly_equal(ego_d + llt_offset, 5.230, 1e-3));
            /* 回退路径（A11 网格中心）给出合法车道中心：必须落在 4 条车道中心集合内 */
            const int fb = planning_coord::nearest_lane(
                ego_d, lane_count, lane_w, false, side_offset);
            const double fb_d = planning_coord::lane_center_d(
                fb, lane_count, lane_w, side_offset);
            bool on_grid = false;
            for (int k = 0; k < lane_count; ++k) {
                if (nearly_equal(fb_d,
                        planning_coord::lane_center_d(k, lane_count, lane_w, side_offset))) {
                    on_grid = true;
                }
            }
            CHECK(on_grid);
            /* 且回退值不等于真消费给出的骑线值 */
            CHECK(!nearly_equal(fb_d, bogus, 1e-3));
        }
    }

    printf("PASS: test_planning_fallback_when_match_invalid (all 7 A11 test suites passed)\n");
    return 0;
}
