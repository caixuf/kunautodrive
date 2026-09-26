/**
 * fusion_lane_hint.h — `perception_fusion` lane_match hint 计算 helper
 *
 * D2-07 阶段 2（obs_lane_match_hint 接线）：
 *   perception_fusion 订阅 `localization/lane_match`，cache 最新 ego 车道
 *   信息，融合输出 obstacle 时给每个 obs 打 `obs_lane_match_hint: bool`
 *   （hint = 该 obs 与 ego 在同一 lanelet 或相邻车道 ±1）。
 *
 * 本头文件按仓库既定 pattern（imu_protocol.h / perception_points.h /
 * lidar_scan.h 等）抽出纯算法到 header-only 模块，让 perception_fusion_node.cpp
 * 和 tests/test_adas_nodes_logic.c **共用同一份实现，无副本漂移**。
 *
 * 设计原则：
 *   - `fusion_compute_hint()` 是**纯函数**（无副作用、无外部状态），可直接单测
 *   - `LaneMatchCache` 是 producer/consumer 共享状态，但本头文件不持有 mutex
 *     —— 线程安全由调用方负责（fusion_node 中已用 pthread_mutex）
 *   - 所有 defensive 检查在函数内做：cache.valid=false / obs.lane_id==-1 /
 *     lane_width<=0 全部 → false（向后兼容：M2 时期没 lane_match 输入时
 *     fusion 必须仍工作）
 *
 * Spec 契约（REQ_L3_DIR2_HDMAP.md §4.2 FR-RT-05）：
 *   - hint=true ⇒ obs 在 ego 同车道或相邻车道 ±1 lane 内
 *   - 仅 metadata，不下结论；planning 自己消费 hint + lane_match 决策
 *   - obs 总数不变（仅设 hint，不删 obs）
 */

#ifndef FUSION_LANE_HINT_H
#define FUSION_LANE_HINT_H

#include "adas_msgs_gen.h"   /* Obstacle struct（由 sub-task A 加入 obs_lane_match_hint 字段）*/

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════ */
/* LaneMatchCache — ego 当前车道匹配缓存                     */
/* ══════════════════════════════════════════════════════════ */
/* 写入方：on_lane_match 回调（transport 线程）
 * 读取方：fusion 主协程（apply_lane_hint）
 *
 * 字段语义：
 *   valid      — 来自 lane_match.valid + 防御性 lane_width>0 检查
 *                (lane_match 失效或宽度非法 → false，compute_hint 直接 false)
 *   lane_width — ego 当前车道宽度（米，来自 lane_match.lane_width M3 字段）
 *   llt_offset — ego 相对当前车道中心线的横向偏移（米，左正右负）
 *   stamp_us   — 最新收到的 lane_match 消息时间戳（用于陈旧度检测）
 *   fresh      — 上次 freshness check 时相对 now 是否新鲜
 *                (false 时 compute_hint 直接 false)
 *
 * 注意：本头文件不持有 mutex —— fusion_node.cpp 在 apply_lane_hint 前
 * 显式调 `fusion_lane_hint_cache_check_freshness`，回调里 mutex 保护
 * valid/lane_width/llt_offset/stamp_us 写入；fresh 由调用方独占写。 */
typedef struct {
    bool     valid;
    double   lane_width;
    double   llt_offset;
    uint64_t stamp_us;
    bool     fresh;
} LaneMatchCache;

/** 初始化 cache 为 invalid 默认态（M2 时期没 lane_match 输入时 → 所有 hint=false）。 */
static inline void fusion_lane_hint_cache_init(LaneMatchCache* c) {
    if (!c) return;
    c->valid      = false;
    c->lane_width = 0.0;
    c->llt_offset = 0.0;
    c->stamp_us   = 0ULL;
    c->fresh      = false;
}

/**
 * 陈旧度检查：在每次 fusion publish 前调一次，更新 `c->fresh`。
 *
 * @param c           cache 指针
 * @param now_us      当前时间戳（CLOCK_MONOTONIC 微秒，与 lane_match.stamp_us 同源）
 * @param max_age_us  最大允许陈旧时间（默认 1s；flowsim 10Hz lane_match + 60Hz fusion 时
 *                    6 个 fusion 周期内 cache 仍 fresh）
 *
 * 副作用：修改 c->fresh。**仅由 fusion 主协程调用**，无锁。
 */
static inline void fusion_lane_hint_cache_check_freshness(LaneMatchCache* c,
                                                           uint64_t now_us,
                                                           uint64_t max_age_us) {
    if (!c) return;
    if (!c->valid) { c->fresh = false; return; }
    if (now_us <= c->stamp_us) { c->fresh = true; return; }  /* future ts = fresh (defensive) */
    c->fresh = (now_us - c->stamp_us) <= max_age_us;
}

/**
 * 计算单个 obs 的 lane_match hint（**纯函数**，无副作用）。
 *
 * 算法（spec §4.2 FR-RT-05 v1 简化版）：
 *   1. cache 失效或 lane_width 非法 → false（向后兼容 + defensive）
 *   2. cache 不新鲜 → false（陈旧数据不参与判断）
 *   3. obs 未分配车道（lane_id == -1）→ false（让 planning 自己决定）
 *   4. 启发式：obs 相对 ego 当前车道中心线 lateral 距离 < 3×lane_width
 *      ego 当前车道中心线 lateral 偏移 = lm.llt_offset
 *      obs 相对 ego 中心的横向距离（车体坐标系）= obs.y
 *      obs 相对车道中心线的横向距离 = obs.y - llt_offset
 *      阈值 3×lane_width 覆盖：ego 车道（|y-offset| < 0.5·lane_width）+ 两侧 ±1 lane
 *
 * @param obs  障碍物指针（不可 NULL）
 * @param lm   lane_match cache 指针（不可 NULL，必须先调 check_freshness）
 * @return     hint flag；true = 与 ego 同车道或相邻 ±1 lane
 */
static inline bool fusion_compute_hint(const Obstacle* obs,
                                        const LaneMatchCache* lm) {
    if (!obs || !lm) return false;
    /* 防御 1：cache 失效或宽度非法 → 一律 false */
    if (!lm->valid || !lm->fresh || lm->lane_width <= 0.0) return false;
    /* 防御 2：obs 未分配车道 → 一律 false */
    if (obs->lane_id == -1) return false;

    const double lane_center_offset    = lm->llt_offset;
    const double obs_lateral_to_center = (double)obs->y - lane_center_offset;
    const double abs_lat               = fabs(obs_lateral_to_center);

    /* 同车道（|y - offset| < 0.5·lane_width）+ 两侧 ±1 lane（< 3·lane_width） */
    return abs_lat < 3.0 * lm->lane_width;
}

/**
 * 把 hint 批量写入 ObstacleList（设置 obs.obs_lane_match_hint）。
 *
 * 注意：Obstacle.obs_lane_match_hint 字段由 msg_codegen 自动从 .msg 重生，
 * 本函数假定该字段已存在（M3 D2-07 阶段 1 由 sub-task A 加）。
 *
 * 纯遍历函数：每帧 O(N)（N ≤ 128），可忽略开销。
 */
static inline void fusion_apply_hint(ObstacleList* list,
                                      const LaneMatchCache* lm) {
    if (!list || !lm) return;
    for (uint32_t i = 0; i < list->count; i++) {
        list->obstacles[i].obs_lane_match_hint =
            fusion_compute_hint(&list->obstacles[i], lm) ? 1 : 0;
    }
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* FUSION_LANE_HINT_H */
