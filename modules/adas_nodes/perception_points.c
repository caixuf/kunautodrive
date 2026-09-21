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

    uint32_t n = 0;
    for (int ci = 0; ci < n_clusters && n < PERCEPTION_MAX_OBSTACLES; ci++) {
        const ClusterBounds* cb = &clusters[ci];
        if (cb->point_count < 3) continue;

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
