#ifndef PERCEPTION_POINTS_H
#define PERCEPTION_POINTS_H

/**
 * perception_points.h — 感知点云消费的纯逻辑（无 IO / 无日志 / 无全局量）
 *
 * 从 perception_node.cpp 抽出来，理由与 imu_protocol.{h,c} 相同：节点内的
 * 纯逻辑无法被 CI 覆盖，只能靠测试副本 + 行号标注（会漂移）。抽成 .c 后
 * tests/test_adas_nodes_logic.c 直接编同一份实现。
 *
 * 覆盖两段逻辑：
 *   1. LidarPointCloud（传感器/车体系）→ DBSCAN 输入点，含有限性/量程守卫
 *   2. DBSCAN 聚类 → ObstacleList（sensor 模式），含 lane_id 反算与类型映射
 */

#include <stdint.h>

#include "adas_msgs_gen.h"   /* LidarPointCloud, ObstacleList */
#include "dbscan_cluster.h"  /* Point3D, ClusterBounds */

#ifdef __cplusplus
extern "C" {
#endif

/** 单帧点云点数上限。必须等于 lidar_point_cloud_capacity()（.c 内有 _Static_assert）。 */
#define PERCEPTION_MAX_CLOUD_POINTS 2048u

/** 单帧 ObstacleList 容量（ObstacleList.obstacles[] 长度）。 */
#define PERCEPTION_MAX_OBSTACLES    128u

/**
 * 点云 → DBSCAN 输入点。
 *
 * 输入点按 LidarPointCloud 契约已在**传感器（车体）系**：x 前、y 左、z 上，
 * 因此这里不做任何位姿变换。
 *
 * 守卫（丢弃）：
 *   - x/y/z 非有限值（NaN/Inf）
 *   - 量程外：range > max_range_m 或 range < min_range_m（近场自反射）
 * 契约违反（cloud->count 超过 PERCEPTION_MAX_CLOUD_POINTS）时整帧返回 0，
 * 由调用方报错 —— 不静默截断，否则"点云被砍一半"这种故障没有任何信号。
 *
 * @param cloud        输入点云（NULL → 返回 0）
 * @param out          输出点数组
 * @param max_out      out 容量
 * @param max_range_m  最大有效距离（米）
 * @param min_range_m  最小有效距离（米）
 * @return             写入 out 的点数
 */
uint32_t perception_points_from_cloud(const LidarPointCloud* cloud,
                                      Point3D* out, uint32_t max_out,
                                      double max_range_m, double min_range_m);

/** 聚类 → Obstacle 映射所需的位姿/车道上下文。 */
typedef struct {
    uint32_t frame_id;      /**< 输出 ObstacleList.frame_id */
    double   ego_y;         /**< ego 世界 y（车体系簇中心反算世界 y 用） */
    double   ego_heading;   /**< ego 世界朝向（弧度） */
    int      lane_count;    /**< 车道数（<=0 时按 2 处理） */
    double   lane_width;    /**< 车道宽（<=0 时按 3.5m 处理） */
} PerceptionFramePose;

/**
 * DBSCAN 聚类 → ObstacleList（sensor 模式）。
 *
 * 与 ground_truth 分支的关键差别：类型只能由簇的几何尺寸启发式判定，
 * 速度不由这里给出（由 object_tracker 的 KF 跨帧关联得到）。
 * point_count < 3 的簇视为噪声丢弃；输出上限 PERCEPTION_MAX_OBSTACLES。
 *
 * @param clusters    dbscan_get_cluster() 的连续簇数组
 * @param n_clusters  簇数量
 * @param pose        位姿/车道上下文（NULL → 返回 0）
 * @param out         输出（会被 memset）
 * @return            写入的障碍物数量
 */
uint32_t perception_clusters_to_obstacles(const ClusterBounds* clusters,
                                          int n_clusters,
                                          const PerceptionFramePose* pose,
                                          ObstacleList* out);

#ifdef __cplusplus
}
#endif

#endif /* PERCEPTION_POINTS_H */
