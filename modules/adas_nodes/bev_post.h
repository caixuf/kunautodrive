/**
 * bev_post.h — BEV 后处理：模型输出 → perception/obstacles 老协议（纯函数）
 *
 * 把 BEV 检测头解码出的目标列表，映射成 msg_codegen 生成的 ObstacleList。
 * 只做「协议映射 + lane_id 计算」，不含网络解码细节；网络输出→目标列表的
 * 解码由调用方（bev_detection_node）按具体模型输出张量语义实现后喂给本层。
 *
 * 坐标约定与 perception_node 完全一致——Obstacle.x = 车体纵向（+前）、
 * y = 车体横向（+左）、vx/vy 为车体系速度、lane_id 按世界系 y 推导。
 * 依赖生成的 adas_msgs_gen.h（同 inference / perception 节点），编译期强类型。
 */
#ifndef FLOWSIM_BEV_POST_H
#define FLOWSIM_BEV_POST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BEV_POST_MAX_DET 128

/* 由模型解码得到的检测目标（车体坐标，已是浮点）。 */
typedef struct BevPostDet {
    float x, y;          /* 车体坐标系：x 纵向（+前），y 横向（+左） */
    float vx, vy;        /* 车体速度 */
    float width, length; /* 尺寸 m */
    int   type;          /* 0=车 1=行人 2=自行车 3=施工（对齐 ObstacleType 数值） */
    float confidence;    /* [0,1] */
    float world_y;       /* det 在世界系的 y（调用方解码时由 ego_y+旋转算好，供 lane_id 推导） */
} BevPostDet;

/*
 * 把检测目标填进 ObstacleList：
 *  - 对每个 det：填充 Ob->x/y/vx/vy/width/length/type/confidence；
 *  - lane_id 由 det.world_y（世界系 y，解码时已算好）推导，公式与
 *    perception_node 一致（offset=(-wy)/lw+(lc-1)*0.5）。
 *
 * @param list        目标 ObstacleList（count 会被重写）
 * @param dets        检测目标数组（车体坐标）
 * @param n_dets      有效目标数（>BEV_POST_MAX_DET 截断）
 * @param frame_id    ObstacleList.frame_id
 * @param n_lanes     车道数
 * @param lane_width  车道宽 m
 * @return 填充的障碍物数量；list/dets 非法返回 -1
 */
int bev_post_to_obstacle_list(void* list,
                              const BevPostDet* dets, int n_dets,
                              uint32_t frame_id,
                              int n_lanes, double lane_width);

#ifdef __cplusplus
}
#endif

#endif /* FLOWSIM_BEV_POST_H */