/**
 * bev_post.c — BEV 后处理实现：BevPostDet[] → ObstacleList
 *
 * 与 perception_node 的 ground_truth 分支输出契约严格对齐：
 *   Ob->x/y/vx/vy = 车体系；type = 生成的 ObstacleType；lane_id 由世界系 y 推导。
 *
 * 类型映射（内部编码 → 生成的 ObstacleType）：
 *   内部 0→车 1→行人 2→自行车 3→施工；外部 OBJ_TYPE_* 从 1 起，
 *   故 out = int_type + 1（等价于感知里的 BEV_OBJ_* 语义互换表）。
 */
#include "bev_post.h"

#include <stddef.h>

/* 依赖生成的 adas_msgs_gen.h，拿到 ObstacleList/Obstacle/ObstacleType 定义。
 * 由于本头文件在编译期强类型，且 perception_node 也是这么用的，故按项目惯例直接引。 */
#include "adas_msgs_gen.h"

int bev_post_to_obstacle_list(void* list,
                              const BevPostDet* dets, int n_dets,
                              uint32_t frame_id,
                              int n_lanes, double lane_width) {
    if (!list || !dets) return -1;
    if (n_dets < 0) n_dets = 0;
    if (n_dets > BEV_POST_MAX_DET) n_dets = BEV_POST_MAX_DET;

    ObstacleList* ol = (ObstacleList*)list;
    ol->frame_id = frame_id;
    if (n_lanes <= 0) n_lanes = 2;
    if (lane_width <= 0.0) lane_width = 3.5;

    int cnt = 0;
    for (int i = 0; i < n_dets; ++i) {
        const BevPostDet* d = &dets[i];
        Obstacle* ob = &ol->obstacles[cnt];
        ob->id   = (uint32_t)(frame_id * 100 + (uint32_t)cnt);
        ob->x    = d->x;
        ob->y    = d->y;
        ob->vx   = d->vx;
        ob->vy   = d->vy;
        ob->width  = d->width;
        ob->length = d->length;
        ob->confidence = d->confidence;

        /* 类型：内部 0..3 → ObstacleType 1..4 */
        switch (d->type) {
            case 0: ob->type = OBJ_TYPE_VEHICLE;    break;
            case 1: ob->type = OBJ_TYPE_PEDESTRIAN; break;
            case 2: ob->type = OBJ_TYPE_CYCLIST;    break;
            case 3: ob->type = OBJ_TYPE_CONSTRUCTION; break;
            default: ob->type = OBJ_TYPE_UNKNOWN;   break;
        }

        /* lane_id：世界系 y → 车道索引（与 perception_node 相同公式） */
        double offset = (-d->world_y) / lane_width + (n_lanes - 1) * 0.5;
        int idx = (int)(offset >= 0.0 ? offset + 0.5 : offset - 0.5);
        if (idx < 0) idx = 0;
        if (idx >= n_lanes) idx = n_lanes - 1;
        ob->lane_id = (int8_t)idx;

        cnt++;
    }
    ol->count = (uint32_t)cnt;
    return cnt;
}