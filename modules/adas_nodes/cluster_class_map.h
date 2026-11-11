#ifndef FLOWENGINE_CLUSTER_CLASS_MAP_H
#define FLOWENGINE_CLUSTER_CLASS_MAP_H

/*
 * cluster_class_map.h — DBSCAN ClusterClass → ADAS ObstacleType 的唯一映射。
 *
 * 三处消费者（lidar_driver_node / stereo_vision_node / perception_points）
 * 此前各自维护一份逐字相同的 switch。映射一旦有一份漂移，同一种聚类在
 * LiDAR 链路和双目链路上会得到不同的障碍物类型，下游 behavior/planning
 * 的类型判据随之分叉。收敛到本头文件，只此一份。
 */

#include "dbscan_cluster.h"
#include "adas_enums.h"

static inline int8_t cluster_class_to_obs_type(ClusterClass cls) {
    switch (cls) {
        case CLS_VEHICLE:    return OBJ_TYPE_VEHICLE;
        case CLS_PEDESTRIAN: return OBJ_TYPE_PEDESTRIAN;
        case CLS_CYCLIST:    return OBJ_TYPE_CYCLIST;
        default:             return OBJ_TYPE_UNKNOWN;
    }
}

#endif /* FLOWENGINE_CLUSTER_CLASS_MAP_H */