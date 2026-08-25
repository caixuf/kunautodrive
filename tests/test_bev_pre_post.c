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
#include <string.h>

#include "bev_pre.h"
#include "bev_post.h"
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
    /* ego 朝 +y（北, heading=+π/2）。世界系正前方 (+10,+?) → 车体前向 = 北向 */
    /* 世界系 (0,+10) 在 ego(0,0,heading=π/2) 看来：dx=0,dy=10 → xb=0*cos+10*sin≈10, yb≈-0*sin+10*cos≈0 */
    BevPreObs obs[] = { {0.0, 10.0, 0.0, 5.0, BEV_OBJ_VEHICLE} };
    bev_pre_rasterize(&cfg, 0, 0, M_PI / 2.0, obs, 1, feat);
    size_t i_occ = ((0 * 8 + 4) * 8 + 6);
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
    int n = bev_post_to_obstacle_list(&ol, dets, 2, 42, 2, 3.5);
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

static void test_post_truncate(void) {
    ObstacleList ol;
    memset(&ol, 0, sizeof(ol));
    BevPostDet dets[200];
    for (int i = 0; i < 200; ++i) {
        dets[i].x = (float)i; dets[i].y = 0; dets[i].vx = 0; dets[i].vy = 0;
        dets[i].width = 2; dets[i].length = 4.6; dets[i].type = 0;
        dets[i].confidence = 0.5f; dets[i].world_y = 0.0f;
    }
    int n = bev_post_to_obstacle_list(&ol, dets, 200, 1, 2, 3.5);
    assert(n == BEV_POST_MAX_DET);   /* 截断到 128 */
    assert(ol.count == BEV_POST_MAX_DET);
    printf("PASS test_post_truncate (%d)\n", n);
}

int main(void) {
    test_pre_basic();
    test_pre_rotation();
    test_pre_outside();
    test_post_mapping();
    test_post_truncate();
    printf("ALL PASS test_bev_pre_post\n");
    return 0;
}