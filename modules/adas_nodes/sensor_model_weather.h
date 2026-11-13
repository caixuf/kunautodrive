#ifndef SENSOR_MODEL_WEATHER_H
#define SENSOR_MODEL_WEATHER_H

/**
 * sensor_model_weather.h — 天气/可见度 → 传感器衰减系数的纯逻辑
 *
 * 从 sensor_model_node.c 的 on_environment_state 抽出来（同 imu_protocol /
 * perception_points 样板）：节点与 tests/test_adas_nodes_logic.c 编同一份实现，
 * 不再维护"副本 + 行号标注"。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 可见度 → 相机可见度因子，钳位到 [0.1, 1.0]（200m 视为满可见）。 */
double sensor_model_camera_visibility(double visibility_m);

/**
 * 可见度 + 天气串 → 传感器衰减系数 ∈ [0, 1]。
 *
 *   1) factor = clamp(visibility_m / 200, 0.1, 1.0)
 *   2) att    = 1 - factor
 *   3) weather 含 rain/fog/snow 时 att 下限 0.3（降水/雾霾的散射地板）
 *
 * @param visibility_m 能见度（米）；非正/非有限值按最差处理（factor 取 0.1）
 * @param weather      天气串（可 NULL / 空）
 */
double sensor_model_weather_attenuation(double visibility_m, const char* weather);

/**
 * 相机退化模型（方向一：多模态感知的"视觉退化"侧）。
 *
 * 此前相机只是 LiDAR 帧副本 × 可见度 —— 夜间/逆光对相机的影响完全没建模，
 * 于是"雷达冗余"缺少一个可对比的退化模态。本函数给出三个确定性分量：
 *
 *   visibility = clamp(visibility_m / 200, 0.1, 1.0)     （雾/雨/雪 → 能见度）
 *   contrast   = 天气串查表（clear 1.0 / rain 0.75 / snow 0.65 / fog 0.45）
 *   lighting   = 光照串查表（day 1.0 / dusk 0.65 / night 0.30；未知按 day）
 *   glare      = dusk 逆光额外损失 0.15；其它 0
 *   effective  = clamp(visibility * contrast * lighting - glare, 0.05, 1.0)
 *
 * 性质（单测锁定）：各分量 ∈[0,1]，effective 对 visibility 单调不减；
 * 同一输入恒等；天气/光照串大小写不敏感。
 */
typedef struct {
    double visibility;
    double contrast;
    double lighting;
    double glare;
    double effective;
} SensorCameraDegradation;

SensorCameraDegradation sensor_model_camera_degradation(
    double visibility_m, const char* weather, const char* lighting);

/**
 * 雨雾天气对 LiDAR 有效探测距离的衰减：
 *   att=0 (clear) → 100% base_range_m；
 *   att=1 (dense fog) → 45% base_range_m（最小保留 15m）。
 */
double sensor_model_lidar_effective_range(double base_range_m, double weather_attenuation);

/**
 * 雨雾天气对 LiDAR 丢点率的影响：
 *   att=0 (clear) → 保持 base_loss_rate；
 *   att>0 (rain/fog/snow) → 线性叠加最多 +0.35 丢点率（上限 0.85）。
 */
double sensor_model_lidar_loss_rate(double base_loss_rate, double weather_attenuation);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_MODEL_WEATHER_H */
