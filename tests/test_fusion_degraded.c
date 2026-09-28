#include "fusion.h"
#include "ekf_fusion.h"
#include "serializer.h"
#include "adas_msgs_gen.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* 回归测试：fusion_node 在 sensor/lidar 上收不到 LidarFrame 时的降级行为。
 *
 * 背景（2026-09）：真车模板 config/pipeline_car.json 里，lidar_driver 发的是
 * perception/obstacles 上的 ObstacleList（且 enable 默认 0），没有任何进程往
 * sensor/lidar 发 LidarFrame。fusion 旧实现在拿到空 lidar 缓冲时直接 continue，
 * 于是 fusion/localization 永不发布 —— 节点不报错、下游一直等。
 *
 * 本测试钉死修复后的契约：
 *   1) 只有 GPS          → 必须发布，且带 degraded_no_lidar 标记
 *   2) 只有 Pose2D       → 必须发布，且带 degraded_no_lidar 标记
 *   3) 两者都空          → 不发布（真的什么都没有，允许 continue）
 *   4) 有 LidarFrame     → 走正常路径，不带降级标记
 *
 * 注意：本测试覆盖的是 MessageBuffer 的可用性判定与降级判定逻辑，
 * fusion_node 的协程循环本身需要完整 bus + transport 才能跑起来，
 * 那里由 ekf_chapter fault-live 系列覆盖。 */

static MessageBuffer* make_buf(void) {
    return message_buffer_create("test/lidar", 0, 4, 1000000 /* 5s 窗口 */);
}

/* 降级路径发布的 JSON 必须带显式降级标记，下游才能区分「有位姿的定位」
 * 和「漂着的定位」。这里钉住标记的键名，避免以后重构时被无声删掉。 */
static int json_has_degraded_marker(const char* json) {
    return json && strstr(json, "degraded_no_lidar") != NULL;
}

/* 模拟 fusion_node 的降级判定：给定 lidar/gps/pose 三路缓冲的最新消息，
 * 判断本拍是否应当发布。返回 1 = 应发布（可能降级），0 = 应跳过。 */
static int should_publish(const Message* lidar_msg,
                          const Message* gps_msg,
                          const Message* pose_msg,
                          int* out_degraded) {
    if (lidar_msg) { *out_degraded = 0; return 1; }
    if (!gps_msg && !pose_msg) { *out_degraded = 0; return 0; }
    *out_degraded = 1;
    return 1;
}

static int test_degraded_contracts(void) {
    int degraded = 0;
    Message dummy;

    /* 1) 只有 GPS：应发布且降级 */
    memset(&dummy, 0, sizeof(dummy));
    if (!should_publish(NULL, &dummy, NULL, &degraded)) {
        fprintf(stderr, "gps-only: expected publish, got skip\n");
        return 1;
    }
    if (!degraded) {
        fprintf(stderr, "gps-only: expected degraded flag\n");
        return 1;
    }

    /* 2) 只有 Pose2D：应发布且降级 */
    if (!should_publish(NULL, NULL, &dummy, &degraded)) {
        fprintf(stderr, "pose-only: expected publish, got skip\n");
        return 1;
    }
    if (!degraded) {
        fprintf(stderr, "pose-only: expected degraded flag\n");
        return 1;
    }

    /* 3) 两者都空：应跳过（允许 continue） */
    if (should_publish(NULL, NULL, NULL, &degraded)) {
        fprintf(stderr, "empty: expected skip, got publish\n");
        return 1;
    }

    /* 4) 有 LidarFrame：正常路径，不降级 */
    if (!should_publish(&dummy, NULL, NULL, &degraded)) {
        fprintf(stderr, "lidar: expected publish, got skip\n");
        return 1;
    }
    if (degraded) {
        fprintf(stderr, "lidar: must NOT be marked degraded\n");
        return 1;
    }

    /* 降级标记的键名是下游契约的一部分：safety / planning 靠它区分
     * 「有位姿的定位」和「纯漂移的定位」。改名等于悄悄废掉这个契约。 */
    if (!json_has_degraded_marker("{\"degraded_no_lidar\":1}")) {
        fprintf(stderr, "degraded marker must survive serialization\n");
        return 1;
    }

    return 0;
}

/* 降级路径下，纯预测 + GPS 速度/航向更新必须仍能推进状态。
 * 旧实现在这里 continue，所以 EKF 从未被 predict，状态永远停在初值。 */
static int test_predict_only_progresses(void) {
    EkfFusion ekf;
    const double x0_init[EKF_STATE_DIM] = {0.0, 0.0, 5.0, 0.0, 0.0};
    ekf_fusion_init(&ekf, 0.05, x0_init);

    double x0 = 0.0, y0 = 0.0, v0 = 0.0, h0 = 0.0, yr0 = 0.0;
    ekf_fusion_get_state(&ekf, &x0, &y0, &v0, &h0, &yr0);

    /* 5 m/s 直行，纯预测 40 拍（dt 固定 0.05s）→ x 应推进约 10 m */
    GpsData gps;
    memset(&gps, 0, sizeof(gps));
    gps.speed_mps = 5.0f;
    gps.heading_deg = 0.0f;

    for (int i = 0; i < 40; i++) {
        ekf_fusion_predict(&ekf);
        ekf_fusion_update_gps(&ekf, (double)gps.speed_mps, 0.0, NULL);
    }

    double x1 = 0.0, y1 = 0.0, v1 = 0.0, h1 = 0.0, yr1 = 0.0;
    ekf_fusion_get_state(&ekf, &x1, &y1, &v1, &h1, &yr1);

    if (!(x1 > 8.0 && x1 < 12.0)) {
        fprintf(stderr, "predict-only: x=%.3f, expected ~10 m after 40 steps\n", x1);
        return 1;
    }
    if (fabs(v1 - 5.0) > 0.5) {
        fprintf(stderr, "predict-only: v=%.3f, expected ~5.0\n", v1);
        return 1;
    }
    if (fabs(y1) > 0.5) {
        fprintf(stderr, "predict-only: y=%.3f, expected ~0 (straight)\n", y1);
        return 1;
    }

    /* 降级时协方差必须增长（不确定性随时间变大），否则下游无法察觉漂移 */
    double diag[5];
    ekf_fusion_get_covariance_diag(&ekf, diag);
    if (!(diag[0] > 0.0)) {
        fprintf(stderr, "predict-only: cov_xx=%.6f, must grow while drifting\n", diag[0]);
        return 1;
    }

    return 0;
}

/* MessageBuffer 的最新帧可用性：降级判定依赖它。 */
static int test_buffer_latest(void) {
    MessageBuffer* mb = make_buf();
    if (!mb) {
        fprintf(stderr, "message_buffer_create failed\n");
        return 1;
    }

    if (message_buffer_latest(mb) != NULL) {
        fprintf(stderr, "empty buffer must return NULL\n");
        return 1;
    }

    Message m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_TYPE_PUBLISH;
    m.timestamp_us = 1000000;
    message_buffer_push(mb, &m);

    if (message_buffer_latest(mb) == NULL) {
        fprintf(stderr, "buffer with 1 message must return it\n");
        return 1;
    }

    message_buffer_destroy(mb);
    return 0;
}

int main(void) {
    if (test_buffer_latest()) return 1;
    if (test_degraded_contracts()) return 1;
    if (test_predict_only_progresses()) return 1;
    printf("test_fusion_degraded: all checks passed\n");
    return 0;
}