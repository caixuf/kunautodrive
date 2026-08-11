#ifndef FLOWENGINE_LIDAR_CONTRACT_H
#define FLOWENGINE_LIDAR_CONTRACT_H

#include "adas_msgs_gen.h"

#include <math.h>
#include <stdint.h>

#define LIDAR_POINT_CLOUD_MIN_POINTS       3u
#define LIDAR_POINT_CLOUD_MAX_RANGE_M      200.0f
#define LIDAR_POINT_CLOUD_MIN_DENSITY      0.01f

static inline uint32_t lidar_point_cloud_capacity(void) {
    return (uint32_t)(sizeof(((LidarPointCloud*)0)->points) /
                      sizeof(((LidarPointCloud*)0)->points[0]));
}

/*
 * Validate the transport-level contract before a point cloud reaches a LIO
 * backend. A NULL return means valid; the string is stable and log-friendly.
 */
static inline const char* lidar_point_cloud_validate(
        const LidarPointCloud* cloud,
        int has_previous,
        uint32_t previous_frame_id,
        uint64_t previous_timestamp_us,
        float* density_out) {
    if (density_out) *density_out = 0.0f;
    if (!cloud) return "null cloud";
    if (cloud->timestamp_us == 0) return "missing timestamp";
    if (has_previous &&
        (cloud->frame_id <= previous_frame_id ||
         cloud->timestamp_us <= previous_timestamp_us)) {
        return "non-monotonic frame or timestamp";
    }

    uint32_t capacity = lidar_point_cloud_capacity();
    if (cloud->count < LIDAR_POINT_CLOUD_MIN_POINTS) return "too few points";
    if (cloud->count > capacity) return "point count exceeds capacity";

    float min_x = INFINITY, max_x = -INFINITY;
    float min_y = INFINITY, max_y = -INFINITY;
    const float max_range_sq = LIDAR_POINT_CLOUD_MAX_RANGE_M *
                               LIDAR_POINT_CLOUD_MAX_RANGE_M;
    for (uint32_t i = 0; i < cloud->count; i++) {
        const LidarPoint* point = &cloud->points[i];
        if (!isfinite(point->x) || !isfinite(point->y) ||
            !isfinite(point->z) || !isfinite(point->intensity)) {
            return "non-finite point";
        }
        if (point->x * point->x + point->y * point->y +
            point->z * point->z > max_range_sq) {
            return "point exceeds range";
        }
        if (point->x < min_x) min_x = point->x;
        if (point->x > max_x) max_x = point->x;
        if (point->y < min_y) min_y = point->y;
        if (point->y > max_y) max_y = point->y;
    }

    float area = (max_x - min_x) * (max_y - min_y);
    if (!isfinite(area) || area <= 1.0e-6f) return "degenerate point spread";
    float density = (float)cloud->count / area;
    if (density_out) *density_out = density;
    if (density < LIDAR_POINT_CLOUD_MIN_DENSITY) return "point density too low";
    return NULL;
}

#endif
