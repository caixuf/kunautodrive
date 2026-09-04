#ifndef TASKS_ADAS_SDSC_ADAS_ADAPTER_H_
#define TASKS_ADAS_SDSC_ADAS_ADAPTER_H_

#if __has_include("kun/cellular/sdsc_cortex.h")
#include "kun/cellular/sdsc_cortex.h"
#else
#include "sdsc_cortex.h"
#endif

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDSC_STEER_CELL SDSC_OUT_CELL_PRIMARY
#define SDSC_ACCEL_CELL SDSC_OUT_CELL_SECONDARY

static inline void sdsc_cortex_init_default_adas(SdscCortex* ctx) {
    sdsc_cortex_reset(ctx);
}

/**
 * ── 【具身适配层】ADAS 自动驾驶轨迹跟踪感知编码适配器 ───────────────
 * 将车规 6 维物理感知量打包投影至 12 通道细胞受体
 */
static inline void sdsc_adas_encode_receptors(
    const float* SDSC_RESTRICT inputs,
    float* SDSC_RESTRICT receptors
) {
    const float cte_n    = inputs[0];
    const float dpsi_n   = inputs[1];
    const float kappa_n  = inputs[2];
    const float v_n      = inputs[3];
    const float verr_n   = inputs[4];
    const float danger_n = inputs[5];

    receptors[0]  = fmaxf(0.0f, -cte_n);
    receptors[1]  = fmaxf(0.0f,  cte_n);
    receptors[2]  = fmaxf(0.0f, -cte_n * 2.0f - 0.5f);
    receptors[3]  = fmaxf(0.0f,  cte_n * 2.0f - 0.5f);
    receptors[4]  = fminf(fmaxf(dpsi_n, -1.0f), 1.0f);
    receptors[5]  = fminf(fmaxf(dpsi_n * 1.5f, -1.0f), 1.0f);
    receptors[6]  = fminf(fmaxf(kappa_n, -1.0f), 1.0f);
    receptors[7]  = fminf(fmaxf(kappa_n * v_n, -1.0f), 1.0f);
    receptors[8]  = fminf(fmaxf(v_n, 0.0f), 1.0f);
    receptors[9]  = fminf(fmaxf(verr_n, -1.0f), 1.0f);
    receptors[10] = fminf(fmaxf(-verr_n, 0.0f), 1.0f);
    receptors[11] = fminf(fmaxf(danger_n, 0.0f), 1.0f);
}

/**
 * 具身端到端便捷接口（自动调用感知编码器 + 通用受体内核）
 */
static inline SDSC_HOT void sdsc_cortex_forward(
    SdscCortex* SDSC_RESTRICT ctx,
    const float* SDSC_RESTRICT inputs,
    float* SDSC_RESTRICT outputs
) {
    float recs[SDSC_RECEPTOR_COUNT];
    sdsc_adas_encode_receptors(inputs, recs);
    sdsc_cortex_forward_receptors(ctx, recs, outputs);
}

/** 速度相关转向限幅，与 control_node.cpp steer_limit_for_speed 一致。 */
static inline float sdsc_cortex_steer_limit(float v_mps, float max_lat_accel) {
    float s = (v_mps < 2.0f) ? 2.0f : v_mps;
    float lim = atanf(max_lat_accel * 2.7f / (s * s));
    if (lim < 0.016f) lim = 0.016f;
    if (lim > 0.16f)  lim = 0.16f;
    return lim;
}

#ifdef __cplusplus
}
#endif

#endif /* TASKS_ADAS_SDSC_ADAS_ADAPTER_H_ */
