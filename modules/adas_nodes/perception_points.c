/**
 * perception_points.c — 感知点云消费的纯逻辑实现（详见 perception_points.h）
 */

#include "perception_points.h"

#include <math.h>
#include <string.h>

/* 容量必须与消息契约一致：LidarPointCloud.points[] 长度就是"一帧最多多少点"的
 * 权威。改了消息定义而忘了改这里（或反之）→ 编译即失败。 */
_Static_assert(PERCEPTION_MAX_CLOUD_POINTS ==
                   (sizeof(((LidarPointCloud*)0)->points) /
                    sizeof(((LidarPointCloud*)0)->points[0])),
               "PERCEPTION_MAX_CLOUD_POINTS 与 LidarPointCloud.points[] 容量不一致");

uint32_t perception_points_from_cloud(const LidarPointCloud* cloud,
                                      Point3D* out, uint32_t max_out,
                                      double max_range_m, double min_range_m) {
    if (!cloud || !out || max_out == 0) return 0;
    if (cloud->count > PERCEPTION_MAX_CLOUD_POINTS) return 0;  /* 契约违反 */
    if (max_range_m <= 0.0) max_range_m = 200.0;
    if (min_range_m < 0.0)  min_range_m = 0.0;
    const double max_range_sq = max_range_m * max_range_m;
    const double min_range_sq = min_range_m * min_range_m;

    uint32_t n = 0;
    for (uint32_t i = 0; i < cloud->count && n < max_out; i++) {
        const LidarPoint* p = &cloud->points[i];
        if (!isfinite(p->x) || !isfinite(p->y) || !isfinite(p->z)) continue;
        const double r_sq = (double)p->x * p->x + (double)p->y * p->y +
                            (double)p->z * p->z;
        if (r_sq > max_range_sq || r_sq < min_range_sq) continue;
        out[n].x         = p->x;
        out[n].y         = p->y;
        out[n].z         = p->z;
        out[n].intensity = p->intensity;
        n++;
    }
    return n;
}

/* 簇类型 → 障碍物类型。dbscan 的 fit_bounding_box() 只区分
 * 车/行人/骑行者（按包围盒尺寸），这里保持一一对应。 */
static int8_t cluster_class_to_obj_type(ClusterClass cls) {
    switch (cls) {
        case CLS_VEHICLE:    return OBJ_TYPE_VEHICLE;
        case CLS_PEDESTRIAN: return OBJ_TYPE_PEDESTRIAN;
        case CLS_CYCLIST:    return OBJ_TYPE_CYCLIST;
        default:             return OBJ_TYPE_UNKNOWN;
    }
}

/* ── 碎片合并（2026-09-22）────────────────────────────────────────
 *
 * 一个目标在斜视/近距离下会被 DBSCAN 切成多个簇（典型：车的"正面"主簇 +
 * "近侧面"细长条；8m/10° 实测 2001 点 + 47 点，见 docs/HANDOFF_2026-09-22.md §13）。
 * 一个目标报成多个障碍物本身就不对，而且碎片会直达 planning / behavior / safety
 * （三者都直接订阅 `perception/obstacles`）。这里把"同一目标的碎片"并回一个。
 *
 * ⚠️ **不要**把它和 `straight_road` 那条 FAIL 联系起来：我最初以为是碎片把 ego
 * 挤出路面，已被 docs/HANDOFF_2026-09-23.md §2/§4 的 A/B 推翻（真因是
 * `behavior_planner` 把**空**障碍物列表当成"感知没就绪"→ 整块决策被跳过）。
 * 本合并的 **E2E 收益未证实**，按"正确性 + 单测"收下，不作为任何东西的前置。
 *
 * 判据（两条同时满足）：
 *   1. 两簇包围盒的**间隙** ≤ FRAGMENT_MERGE_GAP_M。相邻车道中心距 3.5m、车宽
 *      2.0m → 空隙 1.5m，故取 1.0m 留 0.5m 余量。
 *   2. 并集包围盒仍像**一辆车**的占地（x ≤ 8.0m、y ≤ 3.5m）—— 第二道闸，防
 *      "把相邻车道的两辆车并成一辆"：两车 2.0m + 1.5m + 2.0m = 5.5m > 3.5m → 拒绝。
 *
 * ⚠️ ClusterBounds 的命名是反的：width = x 向跨度（径向）、length = y 向跨度（横向）。
 */
#define FRAGMENT_MERGE_GAP_M   1.0
#define FRAGMENT_MERGE_MAX_X_M 8.0
#define FRAGMENT_MERGE_MAX_Y_M 3.5

/* 两包围盒之间的最小间隙（重叠时为 0）。 */
static double fragment_box_gap(const ClusterBounds* a, const ClusterBounds* b) {
    const double dx = fabs((double)a->cx - (double)b->cx) -
                      0.5 * ((double)a->width + (double)b->width);
    const double dy = fabs((double)a->cy - (double)b->cy) -
                      0.5 * ((double)a->length + (double)b->length);
    const double gx = (dx > 0.0) ? dx : 0.0;
    const double gy = (dy > 0.0) ? dy : 0.0;
    return sqrt(gx * gx + gy * gy);
}

/* 并集：包围盒取并、点数为和、z 取并、类型取点数多的那半（正面主簇才是"车"）。 */
static ClusterBounds fragment_union(const ClusterBounds* a, const ClusterBounds* b) {
    const double ax0 = (double)a->cx - 0.5 * (double)a->width;
    const double ax1 = (double)a->cx + 0.5 * (double)a->width;
    const double ay0 = (double)a->cy - 0.5 * (double)a->length;
    const double ay1 = (double)a->cy + 0.5 * (double)a->length;
    const double bx0 = (double)b->cx - 0.5 * (double)b->width;
    const double bx1 = (double)b->cx + 0.5 * (double)b->width;
    const double by0 = (double)b->cy - 0.5 * (double)b->length;
    const double by1 = (double)b->cy + 0.5 * (double)b->length;
    const double x0 = (ax0 < bx0) ? ax0 : bx0;
    const double x1 = (ax1 > bx1) ? ax1 : bx1;
    const double y0 = (ay0 < by0) ? ay0 : by0;
    const double y1 = (ay1 > by1) ? ay1 : by1;

    ClusterBounds m = *a;
    m.cx     = (float)(0.5 * (x0 + x1));
    m.cy     = (float)(0.5 * (y0 + y1));
    m.width  = (float)(x1 - x0);
    m.length = (float)(y1 - y0);
    m.min_z  = (a->min_z < b->min_z) ? a->min_z : b->min_z;
    m.max_z  = (a->max_z > b->max_z) ? a->max_z : b->max_z;
    m.point_count = a->point_count + b->point_count;
    m.confidence  = (a->confidence > b->confidence) ? a->confidence : b->confidence;
    m.cls = (a->point_count >= b->point_count) ? a->cls : b->cls;
    return m;
}

/* 就地合并（数组被改写、有效元素被挪到前部）；返回合并后的簇数。 */
static int fragment_merge_inplace(ClusterBounds* cs, int n) {
    if (n < 2) return n;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                if (fragment_box_gap(&cs[i], &cs[j]) > FRAGMENT_MERGE_GAP_M) continue;
                const ClusterBounds m = fragment_union(&cs[i], &cs[j]);
                if ((double)m.width  > FRAGMENT_MERGE_MAX_X_M ||
                    (double)m.length > FRAGMENT_MERGE_MAX_Y_M) {
                    continue;   /* 并起来不像一辆车 → 不并（防跨车道误并） */
                }
                cs[i] = m;
                cs[j] = cs[n - 1];
                n--;
                j--;
                changed = 1;
            }
        }
    }
    return n;
}

uint32_t perception_clusters_to_obstacles(const ClusterBounds* clusters,
                                          int n_clusters,
                                          const PerceptionFramePose* pose,
                                          ObstacleList* out) {
    if (!clusters || !pose || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->frame_id = pose->frame_id;

    const int    lc = (pose->lane_count > 0) ? pose->lane_count : 2;
    const double lw = (pose->lane_width > 0.0) ? pose->lane_width : 3.5;
    const double ch = cos(pose->ego_heading);
    const double sh = sin(pose->ego_heading);

    /* 先收有效簇，再把同一目标的碎片并回一个（见上面的说明） */
    ClusterBounds merged[PERCEPTION_MAX_OBSTACLES];
    int n_merged = 0;
    for (int ci = 0; ci < n_clusters && n_merged < (int)PERCEPTION_MAX_OBSTACLES; ci++) {
        if (clusters[ci].point_count < 3) continue;
        merged[n_merged++] = clusters[ci];
    }
    n_merged = fragment_merge_inplace(merged, n_merged);

    uint32_t n = 0;
    for (int ci = 0; ci < n_merged && n < PERCEPTION_MAX_OBSTACLES; ci++) {
        const ClusterBounds* cb = &merged[ci];

        Obstacle* ob = &out->obstacles[n];
        ob->id    = (uint32_t)(pose->frame_id * 100u + (uint32_t)ci);
        ob->x     = cb->cx;   /* 车体系：x 前 */
        ob->y     = cb->cy;   /* 车体系：y 左 */
        ob->width  = cb->width;
        ob->length = cb->length;
        ob->confidence = cb->confidence;
        ob->type  = cluster_class_to_obj_type(cb->cls);
        /* 速度由 object_tracker 的 KF 跨帧关联提供，这里恒 0 */

        /* lane_id：车体系簇中心 → 世界 y → 车道索引（最左 = 0，最右 = lc-1）。
         * 车体→世界：dy = rx·sin(h) + ry·cos(h)。 */
        const double wy = pose->ego_y + (double)cb->cx * sh + (double)cb->cy * ch;
        double offset = (-wy) / lw + (double)(lc - 1) * 0.5;
        int idx = (int)(offset >= 0.0 ? offset + 0.5 : offset - 0.5);
        if (idx < 0)  idx = 0;
        if (idx >= lc) idx = lc - 1;
        ob->lane_id = (int8_t)idx;

        n++;
    }
    out->count = n;
    return n;
}
