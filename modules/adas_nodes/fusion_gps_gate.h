#ifndef FUSION_GPS_GATE_H
#define FUSION_GPS_GATE_H

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GPS 多径/野值跳变前置门控：
 * 在 EKF 收敛初期（update_count < 100 时内部跳过 χ² gating）防止单帧
 * 70 m/s 级别 GPS 速度尖峰直接污染状态向量。
 *
 * @return 1 表示观测可信、允许喂入 EKF；0 表示野值拒绝。
 */
static inline int fusion_gps_observation_plausible(
    double gps_speed_mps,
    double fused_speed_mps,
    int has_converged_state)
{
    if (!isfinite(gps_speed_mps)) return 0;
    if (gps_speed_mps < -5.0 || gps_speed_mps > 55.0) return 0;
    if (has_converged_state && fabs(gps_speed_mps - fused_speed_mps) > 10.0) {
        return 0;
    }
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* FUSION_GPS_GATE_H */
