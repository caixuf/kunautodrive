/* test_bev_pre_post.c — BEV 前/后处理单元测试（纯算法，无 transport/param 依赖）
 *
 * 覆盖（镜像 bev_pre.c / bev_post.c 的不变量）：
 *   1. 前处理：车体正前方 20m 的障碍物 → BEV ch0=1、ch1(速度)>0、ch2 类型先验
 *   2. 前处理：坐标旋转——世界系正前方障碍物，在 ego 朝 +y（北）时应落在网格前向行
 *   3. 前处理：图外障碍物被丢弃（缓冲区外）
 *   4. 前处理：NCHW 布局、输出尺寸 = channels*H*W
 *   5. 后处理：det → ObstacleList 字段映射、type+1、lane_id 推导
 *   6. 后处理：越界 det（>128）被截断
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bev_pre.h"
#include "bev_post.h"
#include "bev_cnn_native.h"
#include "adas_msgs_gen.h"

static void test_pre_basic(void) {
    BevPreConfig cfg;
    bev_pre_config_default(&cfg);
    cfg.h = cfg.w = 8;
    cfg.range_x = 16.0;        /* 前后各 16m */
    cfg.range_y_half = 8.0;    /* 左右各 8m */

    float feat[8 * 8 * 8]; /* 4ch * 8 * 8 充裕 */
    /* ego 朝 +x（东），正前方 (20,0) 世界 → 车体 x=20 超出 range_x=16 → 图外 */
    BevPreObs obs[] = { {20.0, 0.0, 5.0, 0.0, BEV_OBJ_VEHICLE} };
    size_t n = bev_pre_rasterize(&cfg, 0, 0, 0.0, obs, 1, feat);
    assert(n == (size_t)(cfg.channels * cfg.h * cfg.w));

    (void)n;
    /* 前向 10m → x_body=10 ∈ [0,16) → 对应 ix=floor((10+16)/4)=6，iy=floor((8-0)/2)=4 */
    BevPreObs obs2[] = { {10.0, 0.0, 5.0, 0.0, BEV_OBJ_VEHICLE} };
    bev_pre_rasterize(&cfg, 0, 0, 0.0, obs2, 1, feat);
    size_t i_occ = ((0 * 8 + 4) * 8 + 6);    /* ch0,row=4,col=6 */
    assert(fabsf(feat[i_occ] - 1.0f) < 1e-4);
    /* 速度 5/30 ≈ 0.167 */
    size_t i_v = ((1 * 8 + 4) * 8 + 6);
    assert(fabsf(feat[i_v] - 5.0f / 30.0f) < 0.02);
    /* 类型先验 0.9 */
    size_t i_t = ((2 * 8 + 4) * 8 + 6);
    assert(fabsf(feat[i_t] - 0.9f) < 1e-4);

    printf("PASS test_pre_basic (occ=%f v=%f type=%f)\n", feat[i_occ], feat[i_v], feat[i_t]);
}

static void test_pre_rotation(void) {
    BevPreConfig cfg;
    bev_pre_config_default(&cfg);
    cfg.h = cfg.w = 8;
    cfg.range_x = 16.0;
    cfg.range_y_half = 8.0;

    float feat[8 * 8 * 8];
    /* ego 朝 +y（北, heading=+π/2）。世界系正前方 (0,+10) → 车体前向 = 北向。
     * 数学上 xb=10、yb=0，应与 test_pre_basic 的朝东前方 10m 落在同一格 (row=4,col=6)。
     * 回归：libm 的 cos(π/2)≠0 曾把 yb 推到格线开侧，floor 后 iy=3，本断言失败。 */
    BevPreObs obs[] = { {0.0, 10.0, 0.0, 5.0, BEV_OBJ_VEHICLE} };
    bev_pre_rasterize(&cfg, 0, 0, M_PI / 2.0, obs, 1, feat);
    size_t i_occ = ((0 * 8 + 4) * 8 + 6);
    (void)i_occ;
    assert(fabsf(feat[i_occ] - 1.0f) < 1e-4);
    printf("PASS test_pre_rotation\n");
}

static void test_pre_outside(void) {
    BevPreConfig cfg;
    bev_pre_config_default(&cfg);
    cfg.h = cfg.w = 8;
    cfg.range_x = 16.0;
    cfg.range_y_half = 8.0;

    float feat[8 * 8 * 8];
    memset(feat, 0, sizeof(feat));
    BevPreObs obs[] = { {50.0, 50.0, 0.0, 0.0, BEV_OBJ_VEHICLE} }; /* 图外 */
    bev_pre_rasterize(&cfg, 0, 0, 0.0, obs, 1, feat);
    for (size_t i = 0; i < sizeof(feat) / sizeof(feat[0]); ++i)
        assert(fabsf(feat[i]) < 1e-6);
    printf("PASS test_pre_outside\n");
}

static void test_post_mapping(void) {
    ObstacleList ol;
    memset(&ol, 0, sizeof(ol));
    BevPostDet dets[] = {
        { 10.0f, 0.0f, 5.0f, 0.0f, 2.0f, 4.6f, 0, 0.9f,  0.0f },  /* 车, 世界y=0 → lane index */
        { 5.0f, 2.0f, 1.0f, 0.0f, 0.5f, 0.4f, 1, 0.8f, -3.5f },    /* 行人, 世界y=-3.5 */
    };
    int n = bev_post_to_obstacle_list(&ol, dets, 2, 42, 2, 3.5, 0);
    assert(n == 2);
    assert(ol.count == 2);
    assert(ol.frame_id == 42);
    assert(fabsf(ol.obstacles[0].x - 10.0f) < 1e-4);
    assert(fabsf(ol.obstacles[0].y - 0.0f) < 1e-4);
    assert(ol.obstacles[0].type == OBJ_TYPE_VEHICLE);
    assert(ol.obstacles[0].confidence == 0.9f);
    assert(fabsf(ol.obstacles[0].length - 4.6f) < 1e-4);
    assert(ol.obstacles[1].type == OBJ_TYPE_PEDESTRIAN);
    /* lane_id: world_y=0, 2 车道宽3.5 → offset=0+(1)*0.5=0.5 → idx=1 */
    assert(ol.obstacles[0].lane_id == 1);
    /* world_y=-3.5 → offset=(3.5)/3.5+0.5=1.5 → idx=1 */
    assert(ol.obstacles[1].lane_id == 1);
    printf("PASS test_post_mapping (objs=%d lane0=%d lane1=%d)\n", n, ol.obstacles[0].lane_id, ol.obstacles[1].lane_id);
}

static void test_post_lane_oneway(void) {
    /* 单向 4 车道：车道中心 -1.75/-5.25/-8.75/-12.25（最内侧 idx0）。
     * 旧公式会把 -1.75 算成 lane 2、-5.25 算成 lane 3。 */
    ObstacleList ol;
    memset(&ol, 0, sizeof(ol));
    BevPostDet dets[] = {
        { 10.0f, 0.0f, 5.0f, 0.0f, 2.0f, 4.6f, 0, 0.9f, -1.75f },
        { 10.0f, 0.0f, 5.0f, 0.0f, 2.0f, 4.6f, 0, 0.9f, -5.25f },
    };
    int n = bev_post_to_obstacle_list(&ol, dets, 2, 43, 4, 3.5, 1);
    assert(n == 2);
    assert(ol.obstacles[0].lane_id == 0);  /* y=-1.75，含 side_offset */
    assert(ol.obstacles[1].lane_id == 1);  /* y=-5.25 */
    printf("PASS test_post_lane_oneway (n=%d lane0=%d lane1=%d)\n", n,
           ol.obstacles[0].lane_id, ol.obstacles[1].lane_id);
}

static void test_post_truncate(void) {
    ObstacleList ol;
    memset(&ol, 0, sizeof(ol));
    BevPostDet dets[200];
    for (int i = 0; i < 200; ++i) {
        dets[i].x = (float)i; dets[i].y = 0; dets[i].vx = 0; dets[i].vy = 0;
        dets[i].width = 2; dets[i].length = 4.6; dets[i].type = 0;
        dets[i].confidence = 0.5f; dets[i].world_y = 0.0f;
    }
    int n = bev_post_to_obstacle_list(&ol, dets, 200, 1, 2, 3.5, 0);
    assert(n == BEV_POST_MAX_DET);   /* 截断到 128 */
    assert(ol.count == BEV_POST_MAX_DET);
    printf("PASS test_post_truncate (%d)\n", n);
}

static void test_pre_asymmetric(void) {
    BevPreConfig cfg;
    bev_pre_config_default(&cfg);
    cfg.w = 16;  /* 宽 = 16 (前向网格数) */
    cfg.h = 8;   /* 高 = 8 (横向网格数) */
    cfg.range_x = 32.0;       /* 前后 32m -> res_x = 64/16 = 4m/格 */
    cfg.range_y_half = 8.0;   /* 左右各 8m -> res_y = 16/8 = 2m/格 */

    float feat[4 * 8 * 16];
    /* 前方 10m，左侧 2m: xb=10 -> c = floor((10+32)/4) = 10, yb=2 -> r = floor((8-2)/2) = 3 */
    BevPreObs obs[] = { {10.0, 2.0, 0.0, 0.0, BEV_OBJ_VEHICLE} };
    bev_pre_rasterize(&cfg, 0, 0, 0.0, obs, 1, feat);
    size_t i_occ = ((0 * 8 + 3) * 16 + 10);
    assert(fabsf(feat[i_occ] - 1.0f) < 1e-4);
    printf("PASS test_pre_asymmetric (w=16,h=8 idx=%zu)\n", i_occ);
}

static void test_pre_sensors(void) {
    BevPreConfig cfg;
    bev_pre_config_default(&cfg);
    cfg.channels = 6;

    float feat[6 * 88 * 88];
    BevSensorPoint pts[] = {
        { 10.0f,  0.0f, 0.0f, 0.8f },
        { 10.0f,  0.0f, 0.2f, 0.9f },
        { 10.0f,  0.0f, -0.1f, 0.7f },
        { 10.0f,  0.0f, 0.0f, 0.85f }, /* 4 points -> occ = 1.0 */
        { -20.0f, 5.0f, 0.0f, 0.5f }   /* 后方点 -> no cam_vis */
    };

    BevSensorInput sin;
    sin.points = pts;
    sin.n_points = 5;
    sin.lidar_intensity = 0.85f;
    sin.camera_visibility = 0.9f;
    sin.ego_speed = 15.0f;

    size_t total = bev_pre_rasterize_sensors(&cfg, &sin, feat);
    (void)total;
    assert(total == 6 * 88 * 88);

    /* 前方 10m, 0m: c = floor((10 + 60) / (120/88)) = 51, r = floor((20 - 0) / (40/88)) = 44 */
    const double res_x = 120.0 / 88.0;
    const double res_y = 40.0 / 88.0;
    int c1 = (int)floor((10.0 + 60.0) / res_x);
    int r1 = (int)floor((20.0 - 0.0) / res_y);

    size_t idx0 = ((0 * 88 + r1) * 88 + c1);
    assert(fabsf(feat[idx0] - 1.0f) < 1e-4);

    size_t idx1 = ((1 * 88 + r1) * 88 + c1);
    (void)idx1;
    assert(feat[idx1] > 0.5f);

    size_t idx2 = ((2 * 88 + r1) * 88 + c1);
    assert(feat[idx2] > 0.0f);

    size_t idx3 = ((3 * 88 + r1) * 88 + c1);
    assert(fabsf(feat[idx3] - 0.5f) < 1e-4); /* 15 / 30 = 0.5 */

    size_t idx5 = ((5 * 88 + r1) * 88 + c1);
    assert(feat[idx5] > 0.0f);

    /* 后方点验证：x=-20 < 0，无相机视野 */
    int c2 = (int)floor((-20.0 + 60.0) / res_x);
    int r2 = (int)floor((20.0 - 5.0) / res_y);
    size_t idx2_rear = ((2 * 88 + r2) * 88 + c2);
    (void)idx2_rear;
    assert(fabsf(feat[idx2_rear]) < 1e-6);

    printf("PASS test_pre_sensors (occ=%f, cam=%f, spd=%f, smooth=%f)\n",
           feat[idx0], feat[idx2], feat[idx3], feat[idx5]);
}

static void test_cnn_native_inference(void) {
    const char* paths[] = {
        "models/bev_centernet_v1.bin",
        "../models/bev_centernet_v1.bin",
        "../../models/bev_centernet_v1.bin",
        NULL
    };
    const char* found_path = NULL;
    for (int i = 0; paths[i]; ++i) {
        FILE* fp = fopen(paths[i], "rb");
        if (fp) {
            fclose(fp);
            found_path = paths[i];
            break;
        }
    }
    if (!found_path) {
        printf("SKIP test_cnn_native_inference: models/bev_centernet_v1.bin not found\n");
        return;
    }

    BevCnnModel model;
    memset(&model, 0, sizeof(model));
    int rc = bev_cnn_load(&model, found_path);
    (void)rc;
    assert(rc == 0);
    assert(model.loaded == 1);
    assert(model.n_layers == 4);

    BevPreConfig cfg;
    bev_pre_config_default(&cfg);
    cfg.channels = 6;

    /* 构造包含 3 个目标的合成传感器点云：
     * 目标 1: 车，x=20m, y=0m
     * 目标 2: 车，x=35m, y=-3.5m
     * 目标 3: 行人，x=12m, y=2.0m */
    BevSensorPoint pts[128];
    int pt_idx = 0;

    /* 目标 1 (x=20, y=0) 表面点簇 */
    for (int i = 0; i < 20; ++i) {
        pts[pt_idx++] = (BevSensorPoint){ 20.0f + (float)(i % 5) * 0.4f - 0.8f,
                                          0.0f + (float)(i / 5) * 0.4f - 0.6f,
                                          0.0f, 0.85f };
    }
    /* 目标 2 (x=35, y=-3.5) 表面点簇 */
    for (int i = 0; i < 20; ++i) {
        pts[pt_idx++] = (BevSensorPoint){ 35.0f + (float)(i % 5) * 0.4f - 0.8f,
                                          -3.5f + (float)(i / 5) * 0.4f - 0.6f,
                                          0.0f, 0.85f };
    }
    /* 目标 3 (x=12, y=2.0) 表面点簇 */
    for (int i = 0; i < 15; ++i) {
        pts[pt_idx++] = (BevSensorPoint){ 12.0f + (float)(i % 3) * 0.2f - 0.2f,
                                          2.0f + (float)(i / 3) * 0.2f - 0.2f,
                                          0.5f, 0.9f };
    }

    BevSensorInput sin;
    sin.points = pts;
    sin.n_points = pt_idx;
    sin.lidar_intensity = 0.85f;
    sin.camera_visibility = 1.0f;
    sin.ego_speed = 10.0f;

    float feat[6 * 88 * 88];
    float out_feat[10 * 88 * 88];
    bev_pre_rasterize_sensors(&cfg, &sin, feat);

    /* 预热 1 次以消除 OpenMP 线程池初始化与页面故障耗时 */
    bev_cnn_forward(&model, feat, out_feat);

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    const int n_runs = 5;
    for (int run = 0; run < n_runs; ++run) {
        rc = bev_cnn_forward(&model, feat, out_feat);
        assert(rc == 0);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed_ms = ((double)(ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                        (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0) / n_runs;

    double max_latency_ms = 10.0;
#ifdef __SANITIZE_ADDRESS__
    max_latency_ms = 150.0;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    max_latency_ms = 150.0;
#endif
#endif
    printf("INFO bev_cnn_forward latency (avg of %d): %.3f ms (gate limit < %.1f ms)\n",
           n_runs, elapsed_ms, max_latency_ms);
    assert(elapsed_ms < max_latency_ms);

    BevPostDet dets[BEV_POST_MAX_DET];
    int n_dets = bev_cnn_decode(&model, out_feat, &cfg, 0.0, 0.0, dets, BEV_POST_MAX_DET);
    printf("INFO decoded detections: %d targets\n", n_dets);
    assert(n_dets >= 2);

    /* 验证检测坐标精度与 20m/35m/12m 目标的对齐度 */
    int hit_target1 = 0, hit_target2 = 0;
    for (int i = 0; i < n_dets; ++i) {
        printf("  det[%d]: x=%.2f, y=%.2f, type=%d, conf=%.3f, w=%.2f, l=%.2f\n",
               i, dets[i].x, dets[i].y, dets[i].type, dets[i].confidence,
               dets[i].width, dets[i].length);
        if (fabsf(dets[i].x - 20.0f) < 1.5f && fabsf(dets[i].y - 0.0f) < 1.5f) {
            hit_target1 = 1;
        }
        if (fabsf(dets[i].x - 35.0f) < 2.0f && fabsf(dets[i].y - (-3.5f)) < 1.5f) {
            hit_target2 = 1;
        }
    }
    assert(hit_target1 && hit_target2);
    (void)hit_target1;
    (void)hit_target2;

    bev_cnn_free(&model);
    printf("PASS test_cnn_native_inference (latency=%.3fms, hits=%d)\n", elapsed_ms, n_dets);
}

int main(void) {
    test_pre_basic();
    test_pre_rotation();
    test_pre_outside();
    test_pre_asymmetric();
    test_pre_sensors();
    test_cnn_native_inference();
    test_post_mapping();
    test_post_lane_oneway();
    test_post_truncate();
    printf("ALL PASS test_bev_pre_post\n");
    return 0;
}