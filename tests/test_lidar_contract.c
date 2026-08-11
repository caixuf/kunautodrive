#include "lidar_contract.h"

#include <stdio.h>
#include <string.h>

static int expect_valid(LidarPointCloud* cloud, const char* label) {
    float density = 0.0f;
    const char* error = lidar_point_cloud_validate(cloud, 0, 0, 0, &density);
    if (error) {
        fprintf(stderr, "%s: expected valid, got %s\n", label, error);
        return 1;
    }
    if (!(density >= LIDAR_POINT_CLOUD_MIN_DENSITY)) {
        fprintf(stderr, "%s: invalid density %.4f\n", label, density);
        return 1;
    }
    return 0;
}

int main(void) {
    LidarPointCloud cloud;
    memset(&cloud, 0, sizeof(cloud));
    cloud.frame_id = 1;
    cloud.timestamp_us = 1000;
    cloud.count = 16;
    for (uint32_t i = 0; i < cloud.count; i++) {
        cloud.points[i].x = (float)i * 0.2f;
        cloud.points[i].y = (float)(i % 4) * 0.2f;
        cloud.points[i].z = 0.0f;
        cloud.points[i].intensity = 1.0f;
    }
    if (expect_valid(&cloud, "valid cloud")) return 1;

    float density = 0.0f;
    const char* error = lidar_point_cloud_validate(
        &cloud, 1, cloud.frame_id, cloud.timestamp_us, &density);
    if (!error || strcmp(error, "non-monotonic frame or timestamp") != 0) {
        fprintf(stderr, "sequence guard did not reject duplicate frame\n");
        return 1;
    }

    cloud.count = 2;
    error = lidar_point_cloud_validate(&cloud, 0, 0, 0, NULL);
    if (!error || strcmp(error, "too few points") != 0) {
        fprintf(stderr, "minimum point guard failed\n");
        return 1;
    }

    cloud.count = 16;
    cloud.points[0].x = 201.0f;
    error = lidar_point_cloud_validate(&cloud, 0, 0, 0, NULL);
    if (!error || strcmp(error, "point exceeds range") != 0) {
        fprintf(stderr, "range guard failed\n");
        return 1;
    }

    puts("lidar contract tests: PASS");
    return 0;
}
