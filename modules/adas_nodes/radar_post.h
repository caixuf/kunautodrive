#ifndef RADAR_POST_H
#define RADAR_POST_H

/*
 * 雷达后处理 — RadarFrame → ObstacleList（方向一：多模态感知冗余）。
 *
 * 为什么需要：`radar_model.h` 只负责"生成一帧雷达量测"，但全仓此前**没有消费者** ——
 * `enable_radar=1` 打开后 `sensor/radar` 发进总线，perception/fusion 都不订阅，
 * 雷达等于发进虚空。方向一要求"任一模态失效时剩余模态仍能支撑安全"，
 * 第一步就是让雷达目标能进入与 LiDAR/视觉同构的障碍物契约。
 *
 * 与 `radar_model.h` / `lidar_scan.h` 同一策略：纯函数、header-only 抽取，
 * 节点与单测链接**同一份实现**，无副本漂移。
 *
 * 坐标约定（与 `Obstacle` 契约一致）：
 *   - 输入 RadarTarget：range_m / azimuth_deg（正 = 左）/ range_rate_mps（正 = 远离）
 *   - 输出 Obstacle：x = 纵向（车头前为正），y = 横向（左为正），单位 m
 *   - az→body：x = r·cos(az), y = r·sin(az)
 *   - 相对速度：雷达只有**径向**分量。在"目标沿视线方向无横向相对运动"的
 *     标准假设下，v_rel ≈ range_rate · (cos az, sin az)。这是雷达跟踪的常规
 *     保守假设，不是凭空补全 —— 横向速度未知时置 0 会伪造"横着飘"的假象。
 *     ⚠ range_rate 本身由 `radar_build_frame` 生成时只投影了 ego 速度（仿真输入
 *     没有各障碍物速度，见 radar_model.h 注释），运动目标的速度分量不完整。
 *
 * 不伪造的量：宽/长/类型/车道归属。雷达单帧给不出这些，一律置未知
 * （width=length=0, type=OBJ_TYPE_UNKNOWN, lane_id=-1），由下游融合按需补。
 */

#include "adas_msgs_gen.h"
#include "radar_model.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 雷达目标在融合后的 ID 命名空间。高位置 1，避免与视觉/点云目标的
 * 小整数 ID 撞号（消费者合并多路时靠它区分来源）。 */
#define RADAR_OBSTACLE_ID_NAMESPACE 0x80000000u

/**
 * 确定性置信度：距离越远越低，弱 RCS 再降一档。
 *
 *   t    = clamp(1 − range/range_max, 0, 1)
 *   span = (rcs >= 0 dBsm) ? 0.5 : 0.25
 *   conf = 0.5 + span·t          → [0.5, 1.0]（强目标）/ [0.5, 0.75]（弱目标）
 *
 * 性质（单测锁定）：对 range 单调不增；同一输入恒等；结果恒在 [0,1]。
 */
static inline float radar_obstacle_confidence(float range_m,
                                              float range_max_m,
                                              float rcs_dbsm) {
    if (!(range_max_m > 0.0f) || !(range_m >= 0.0f)) {
        return 0.0f;
    }
    float t = 1.0f - range_m / range_max_m;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float span = (rcs_dbsm >= 0.0f) ? 0.5f : 0.25f;
    return 0.5f + span * t;
}

/**
 * 一帧雷达量测 → 车体系 ObstacleList。
 *
 * @param frame       雷达帧（targets[0..count-1] 有效）
 * @param range_max_m 量程上限（仅用于置信度；量程门已在建模侧做过）
 * @param out         输出（先清零；最多写 ObstacleList 的 128 槽）
 * @return 写入的障碍物数量
 */
static inline uint32_t radar_frame_to_obstacles(const RadarFrame* frame,
                                                double range_max_m,
                                                ObstacleList* out) {
    if (!frame || !out) return 0;

    memset(out, 0, sizeof(*out));
    out->timestamp_us = frame->timestamp_us;
    out->frame_id = frame->frame_id;

    uint32_t cap = frame->count;
    if (cap > RADAR_MAX_TARGETS) cap = RADAR_MAX_TARGETS;
    if (cap > 128u) cap = 128u;

    uint32_t n = 0;
    for (uint32_t i = 0; i < cap; i++) {
        const RadarTarget* t = &frame->targets[i];
        if (!(t->range_m > 0.0f)) continue;          /* 空槽/无效量测 */
        if (!isfinite(t->range_m) || !isfinite(t->azimuth_deg) ||
            !isfinite(t->range_rate_mps)) {
            continue;                                 /* NaN/Inf 不得污染下游 */
        }

        const double az = (double)t->azimuth_deg * M_PI / 180.0;
        const float c = (float)cos(az);
        const float s = (float)sin(az);

        Obstacle* o = &out->obstacles[n];
        o->id = RADAR_OBSTACLE_ID_NAMESPACE |
                ((frame->frame_id * RADAR_MAX_TARGETS + i) & 0x7FFFFFFFu);
        o->x = t->range_m * c;
        o->y = t->range_m * s;
        o->vx = t->range_rate_mps * c;
        o->vy = t->range_rate_mps * s;
        o->width = 0.0f;
        o->length = 0.0f;
        o->lane_id = -1;
        o->type = OBJ_TYPE_UNKNOWN;
        o->confidence = radar_obstacle_confidence(t->range_m,
                                                  (float)range_max_m,
                                                  t->rcs_dbsm);
        o->obs_lane_match_hint = 0;   /* 雷达目标不打判定性 hint */
        n++;
    }
    out->count = n;
    return n;
}

/**
 * 把雷达障碍物并入感知输出（方向一：冗余模态不重复计数）。
 *
 * 规则：雷达目标若与**已有**障碍物（LiDAR/视觉，或先并入的雷达目标）在车体系
 * 平面距离 < dedupe_radius_m，视为同一物理目标 → 跳过；否则追加。
 * 这是"冗余不重复"的最小实现：不做置信度加权/航迹融合（那需要跨模态关联与
 * 时间同步，属后续融合层；本函数只保证数量语义 —— 同一目标不会因为多一个
 * 模态而变成两个）。
 *
 * 不修改 radar 输入；out 的 frame_id/timestamp 保持感知侧原值。
 * @return 实际追加的障碍物数
 */
static inline uint32_t radar_merge_into_obstacles(const ObstacleList* radar,
                                                  ObstacleList* out,
                                                  float dedupe_radius_m) {
    if (!radar || !out) return 0;
    if (!(dedupe_radius_m >= 0.0f)) dedupe_radius_m = 0.0f;
    const float r2 = dedupe_radius_m * dedupe_radius_m;

    uint32_t added = 0;
    for (uint32_t i = 0; i < radar->count && i < 128u; i++) {
        if (out->count >= 128u) break;
        const Obstacle* cand = &radar->obstacles[i];

        int duplicate = 0;
        for (uint32_t k = 0; k < out->count; k++) {
            const Obstacle* have = &out->obstacles[k];
            const float dx = cand->x - have->x;
            const float dy = cand->y - have->y;
            if (dx * dx + dy * dy <= r2) { duplicate = 1; break; }
        }
        if (duplicate) continue;

        out->obstacles[out->count++] = *cand;
        added++;
    }
    return added;
}

#ifdef __cplusplus
}
#endif

#endif /* RADAR_POST_H */