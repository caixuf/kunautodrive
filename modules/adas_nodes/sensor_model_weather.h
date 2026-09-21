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

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_MODEL_WEATHER_H */
