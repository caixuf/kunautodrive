#include "lidar_contract.h"
#include "lidar_scan.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

/* ── M4 新增：3D OBB 目标旋转测试（0° vs 90° vs 45° 投影变化） ── */
static int test_3d_obb_rotated_target(void) {
    printf("--- test_3d_obb_rotated_target ---\n");
    LidarScanPlan plan;
    memset(&plan, 0, sizeof(plan));
    plan.azimuth_rays   = 181;
    plan.channels       = 1;
    plan.hfov_deg       = 120.0;
    plan.range_max_m    = 50.0;
    plan.mount_height_m = 1.0;
    plan.noise_std_m    = 0.0;
    plan.noise_rel      = 0.0;
    plan.loss_rate      = 0.0;
    plan.intensity_tau_m= 30.0;

    uint64_t rng = lidar_scan_rng_seed(42);
    static LidarPointCloud cloud;

    /* 目标在正前方 15m，尺寸 4.6m (长) x 2.0m (宽) x 1.5m (高) */
    LidarScanTarget target;
    memset(&target, 0, sizeof(target));
    target.x = 15.0;
    target.y = 0.0;
    target.z = 0.0;
    target.length = 4.6;
    target.width  = 2.0;
    target.height = 1.5;
    target.has_heading = 1;

    /* Case A: heading = 0.0 (自车正对目标车尾)
     * 目标纵向长 4.6m 沿 x，横向宽 2.0m 沿 y。近端距离 = 15.0 - 2.3 = 12.7m */
    target.heading = 0.0;
    uint32_t count_0 = lidar_scan_generate(&plan, &target, 1, 0, 0, 0, 1, 1000, &rng, &cloud, NULL);
    if (count_0 == 0) { fprintf(stderr, "heading 0: no points generated\n"); return 1; }
    float min_x_0 = 1000.0f, min_y_0 = 1000.0f, max_y_0 = -1000.0f;
    for (uint32_t i = 0; i < cloud.count; i++) {
        if (cloud.points[i].x < min_x_0) min_x_0 = cloud.points[i].x;
        if (cloud.points[i].y < min_y_0) min_y_0 = cloud.points[i].y;
        if (cloud.points[i].y > max_y_0) max_y_0 = cloud.points[i].y;
    }
    float observed_width_0 = max_y_0 - min_y_0;
    printf("  heading=0.0 rad: min_distance=%.2fm, observed_width=%.2fm (points=%u)\n",
           min_x_0, observed_width_0, count_0);
    if (fabsf(min_x_0 - 12.7f) > 0.15f) {
        fprintf(stderr, "heading 0 distance error: expected ~12.7, got %.2f\n", min_x_0);
        return 1;
    }
    if (observed_width_0 < 1.7f || observed_width_0 > 2.05f) {
        fprintf(stderr, "heading 0 width error: expected ~1.75-2.0m, got %.2f\n", observed_width_0);
        return 1;
    }

    /* Case B: heading = π/2 (90° 横穿目标，自车正对目标侧面)
     * 目标长 4.6m 沿 y 轴，宽 2.0m 沿 x 轴。近端距离 = 15.0 - 1.0 = 14.0m */
    target.heading = M_PI / 2.0;
    uint32_t count_90 = lidar_scan_generate(&plan, &target, 1, 0, 0, 0, 2, 2000, &rng, &cloud, NULL);
    if (count_90 == 0) { fprintf(stderr, "heading 90: no points generated\n"); return 1; }
    float min_x_90 = 1000.0f, min_y_90 = 1000.0f, max_y_90 = -1000.0f;
    for (uint32_t i = 0; i < cloud.count; i++) {
        if (cloud.points[i].x < min_x_90) min_x_90 = cloud.points[i].x;
        if (cloud.points[i].y < min_y_90) min_y_90 = cloud.points[i].y;
        if (cloud.points[i].y > max_y_90) max_y_90 = cloud.points[i].y;
    }
    float observed_width_90 = max_y_90 - min_y_90;
    printf("  heading=90 deg: min_distance=%.2fm, observed_width=%.2fm (points=%u)\n",
           min_x_90, observed_width_90, count_90);
    if (fabsf(min_x_90 - 14.0f) > 0.15f) {
        fprintf(stderr, "heading 90 distance error: expected ~14.0, got %.2f\n", min_x_90);
        return 1;
    }
    if (observed_width_90 < 4.0f || observed_width_90 > 4.65f) {
        fprintf(stderr, "heading 90 width error: expected ~4.1-4.6m, got %.2f\n", observed_width_90);
        return 1;
    }
    if (observed_width_90 < observed_width_0 * 2.0f) {
        fprintf(stderr, "heading 90 should have >2x width of heading 0: %.2f vs %.2f\n",
                observed_width_90, observed_width_0);
        return 1;
    }

    /* Case C: heading = π/4 (45° 斜向角)
     * 目标凸角突向自车：最近角点距离 = 15.0 - (2.3*cos(45) + 1.0*sin(45)) ≈ 12.67m */
    target.heading = M_PI / 4.0;
    uint32_t count_45 = lidar_scan_generate(&plan, &target, 1, 0, 0, 0, 3, 3000, &rng, &cloud, NULL);
    if (count_45 == 0) { fprintf(stderr, "heading 45: no points generated\n"); return 1; }
    float min_x_45 = 1000.0f;
    for (uint32_t i = 0; i < cloud.count; i++) {
        if (cloud.points[i].x < min_x_45) min_x_45 = cloud.points[i].x;
    }
    printf("  heading=45 deg: corner min_distance=%.2fm (points=%u)\n", min_x_45, count_45);
    if (fabsf(min_x_45 - 12.67f) > 0.15f) {
        fprintf(stderr, "heading 45 corner distance error: expected ~12.67, got %.2f\n", min_x_45);
        return 1;
    }

    printf("  ok: 3D OBB rotation accurately matches geometric projections\n");
    return 0;
}

/* ── M4 新增：目标世界高程差 (Elevation Z) 命中测试 ── */
static int test_elevation_difference_hit(void) {
    printf("--- test_elevation_difference_hit ---\n");
    /* 目标在高架桥上：z_bottom = 4.0m, height = 2.0m (顶部 6.0m)，前方 15m */
    LidarScanTarget target;
    memset(&target, 0, sizeof(target));
    target.x = 15.0;
    target.y = 0.0;
    target.z = 4.0;
    target.length = 4.0;
    target.width  = 2.0;
    target.height = 2.0;
    target.has_heading = 0;

    uint64_t rng = lidar_scan_rng_seed(100);
    static LidarPointCloud cloud;

    /* 1. 单线水平扫描 (elevation = 0°): 射线在 z=1.0m 水平面发射，应完全扫空 */
    LidarScanPlan plan_flat;
    memset(&plan_flat, 0, sizeof(plan_flat));
    plan_flat.azimuth_rays   = 64;
    plan_flat.channels       = 1;
    plan_flat.hfov_deg       = 60.0;
    plan_flat.range_max_m    = 50.0;
    plan_flat.mount_height_m = 1.0;
    uint32_t cnt_miss = lidar_scan_generate(&plan_flat, &target, 1, 0, 0, 0, 1, 1000, &rng, &cloud, NULL);
    if (cnt_miss != 0) {
        fprintf(stderr, "flat scan should miss elevated target, got %u points\n", cnt_miss);
        return 1;
    }
    printf("  horizontal 1-channel scan: 0 hits (cleanly misses elevated target)\n");

    /* 2. 多线仰角扫描 (vfov 0° ~ 25°): 仰角覆盖到高架桥目标 */
    LidarScanPlan plan_up;
    memset(&plan_up, 0, sizeof(plan_up));
    plan_up.azimuth_rays   = 64;
    plan_up.channels       = 16;
    plan_up.hfov_deg       = 60.0;
    plan_up.vfov_min_deg   = 0.0;
    plan_up.vfov_max_deg   = 25.0;
    plan_up.range_max_m    = 50.0;
    plan_up.mount_height_m = 1.0;
    uint32_t cnt_hit = lidar_scan_generate(&plan_up, &target, 1, 0, 0, 0, 2, 2000, &rng, &cloud, NULL);
    if (cnt_hit == 0) {
        fprintf(stderr, "upward multi-channel scan failed to hit elevated target\n");
        return 1;
    }
    float avg_z = 0.0f;
    for (uint32_t i = 0; i < cloud.count; i++) avg_z += cloud.points[i].z;
    avg_z /= (float)cloud.count;
    printf("  upward 16-channel scan: %u hits, sensor-frame avg z=%.2fm (target relative z ~4m)\n",
           cnt_hit, avg_z);
    if (avg_z < 3.0f || avg_z > 5.0f) {
        fprintf(stderr, "unexpected hit elevation: avg z = %.2f\n", avg_z);
        return 1;
    }

    printf("  ok: elevation difference correctly discriminated\n");
    return 0;
}

/* ── M4 新增：16 线束多通道俯仰角分布测试 ── */
static int test_16_channel_beam_distribution(void) {
    printf("--- test_16_channel_beam_distribution ---\n");
    LidarScanPlan plan;
    memset(&plan, 0, sizeof(plan));
    plan.azimuth_rays   = 32;
    plan.channels       = 16;
    plan.hfov_deg       = 60.0;
    plan.vfov_min_deg   = -15.0;
    plan.vfov_max_deg   =  15.0;
    plan.range_max_m    = 50.0;
    plan.mount_height_m = 2.0;

    /* 正前方 10m 的大面积高墙 (-5m ~ 10m) */
    LidarScanTarget wall;
    memset(&wall, 0, sizeof(wall));
    wall.x = 10.0;
    wall.y = 0.0;
    wall.z = -5.0;
    wall.length = 2.0;
    wall.width  = 20.0;
    wall.height = 15.0;

    uint64_t rng = lidar_scan_rng_seed(200);
    static LidarPointCloud cloud;
    uint32_t cnt = lidar_scan_generate(&plan, &wall, 1, 0, 0, 0, 1, 1000, &rng, &cloud, NULL);
    if (cnt == 0) { fprintf(stderr, "16-channel wall scan got 0 hits\n"); return 1; }

    /* 检查垂直分布层数 */
    float z_layers[16];
    int n_layers = 0;
    for (uint32_t i = 0; i < cloud.count; i++) {
        float pz = cloud.points[i].z;
        int found = 0;
        for (int k = 0; k < n_layers; k++) {
            if (fabsf(pz - z_layers[k]) < 0.05f) { found = 1; break; }
        }
        if (!found && n_layers < 16) {
            z_layers[n_layers++] = pz;
        }
    }
    printf("  16 channels wall scan: %u points across %d distinct elevation layers\n", cnt, n_layers);
    if (n_layers < 14) {
        fprintf(stderr, "expected ~16 distinct elevation layers, got %d\n", n_layers);
        return 1;
    }
    printf("  ok: 16-channel beam distribution verified\n");
    return 0;
}

/* ── M4 新增：地面射线命中 (Ground Hits) 测试 ── */
static int test_ground_hits(void) {
    printf("--- test_ground_hits ---\n");
    LidarScanPlan plan;
    memset(&plan, 0, sizeof(plan));
    plan.azimuth_rays   = 32;
    plan.channels       = 8;
    plan.hfov_deg       = 60.0;
    plan.vfov_min_deg   = -25.0;
    plan.vfov_max_deg   = -5.0;  // 全部俯视发射
    plan.range_max_m    = 50.0;
    plan.mount_height_m = 1.5;   // 离地 1.5m
    plan.noise_std_m    = 0.0;
    plan.noise_rel      = 0.0;

    uint64_t rng = lidar_scan_rng_seed(300);
    static LidarPointCloud cloud;

    /* 1. enable_ground_hits = 0: 无目标时输出 0 点 */
    plan.enable_ground_hits = 0;
    uint32_t cnt_off = lidar_scan_generate(&plan, NULL, 0, 0, 0, 0, 1, 1000, &rng, &cloud, NULL);
    if (cnt_off != 0) {
        fprintf(stderr, "expected 0 ground hits when disabled, got %u\n", cnt_off);
        return 1;
    }
    printf("  enable_ground_hits=0: 0 points (disabled by default for 2D DBSCAN)\n");

    /* 2. enable_ground_hits = 1: 向下射线全部击中局部地面 z=-mount (-1.5m) */
    plan.enable_ground_hits = 1;
    uint32_t cnt_on = lidar_scan_generate(&plan, NULL, 0, 0, 0, 0, 2, 2000, &rng, &cloud, NULL);
    if (cnt_on == 0) {
        fprintf(stderr, "expected ground hits when enabled, got 0\n");
        return 1;
    }
    float max_z_err = 0.0f;
    for (uint32_t i = 0; i < cloud.count; i++) {
        float err = fabsf(cloud.points[i].z - (-1.5f));
        if (err > max_z_err) max_z_err = err;
    }
    printf("  enable_ground_hits=1: %u ground points, max z error from -1.5m: %.4fm\n",
           cnt_on, max_z_err);
    if (max_z_err > 0.01f) {
        fprintf(stderr, "ground point z plane error too large: %.4f\n", max_z_err);
        return 1;
    }

    printf("  ok: ground hit raycasting verified\n");
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

    /* M4 单测集 */
    if (test_3d_obb_rotated_target()) return 1;
    if (test_elevation_difference_hit()) return 1;
    if (test_16_channel_beam_distribution()) return 1;
    if (test_ground_hits()) return 1;

    puts("lidar contract tests: PASS");
    return 0;
}
