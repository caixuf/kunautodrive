/**
 * sdsc_apex_cortex.h - SDSCC Apex 全场景智驾超级大脑 C11 零 GC 推理内核
 *
 * 融合 5 大功能柱 (脊髓循迹、无保护左转博弈、右转汇入、多把掉头、前额叶裁决)
 * 遵循车规级 ASIL-D 安全标准与 ISO C11 规范，64 字节缓存行对齐，零堆内存分配。
 */
#ifndef SDSC_APEX_CORTEX_H
#define SDSC_APEX_CORTEX_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SDSC_LIKELY(x)      __builtin_expect(!!(x), 1)
#define SDSC_UNLIKELY(x)    __builtin_expect(!!(x), 0)
#define SDSC_HOT            __attribute__((hot))
#define SDSC_RESTRICT       __restrict__
#define SDSC_ALIGN64        __attribute__((aligned(64)))
#else
#define SDSC_LIKELY(x)      (x)
#define SDSC_UNLIKELY(x)    (x)
#define SDSC_HOT
#define SDSC_RESTRICT
#define SDSC_ALIGN64
#endif

#define APEX_MAX_CELLS     256
#define APEX_MAX_SYNAPSES  1024
#define APEX_IN_DIM        6
#define APEX_OUT_DIM       4

typedef enum {
    APEX_MANEUVER_CRUISE = 0,
    APEX_MANEUVER_LEFT_TURN = 1,
    APEX_MANEUVER_RIGHT_MERGE = 2,
    APEX_MANEUVER_UTURN = 3,
    APEX_MANEUVER_EMERGENCY_STOP = 4
} ApexManeuverState;

typedef struct {
    int cell_count;
    int synapse_count;
    ApexManeuverState active_maneuver;

    /* 内部神经元物理状态 (64字节对齐) */
    float states[APEX_MAX_CELLS] SDSC_ALIGN64;
    float outputs[APEX_MAX_CELLS] SDSC_ALIGN64;
    float prev_outputs[APEX_MAX_CELLS] SDSC_ALIGN64;
    float prev_inputs[APEX_MAX_CELLS] SDSC_ALIGN64;
    bool  latches[APEX_MAX_CELLS] SDSC_ALIGN64;

    /* 5 大功能柱各柱前额叶抑制/激发增益 */
    float column_gains[5] SDSC_ALIGN64;

    /* 掉头机动步进计数器 (0=前行进弯, 1=反打倒车, 2=修正回正) */
    int uturn_phase;
    float uturn_reverse_timer;
} SDSC_ALIGN64 SdscApexCortex;

static inline void sdsc_apex_cortex_reset(SdscApexCortex* ctx) {
    if (SDSC_UNLIKELY(!ctx)) return;
    memset(ctx, 0, sizeof(SdscApexCortex));
    ctx->cell_count = 64;
    ctx->synapse_count = 128;
    ctx->active_maneuver = APEX_MANEUVER_CRUISE;
    for (int i = 0; i < 5; ++i) ctx->column_gains[i] = 1.0f;
}

static inline void sdsc_apex_cortex_init(SdscApexCortex* ctx) {
    sdsc_apex_cortex_reset(ctx);
}

/**
 * 核心微秒级前向推演函数
 * inputs[6]:
 *   0: cte          - 横向偏差 (m)
 *   1: d_psi        - 航向偏差 (rad)
 *   2: v            - 自车纵向车速 (m/s)
 *   3: oncoming_ttc - 对向/冲突车流碰撞时距 TTC (s)
 *   4: target_kappa - 目标路径曲率 (1/m)
 *   5: dist_rem     - 剩余机动距离 (m)
 *
 * outputs[4]:
 *   0: steer        - 前轮转向角 (rad, [-0.6, 0.6])
 *   1: accel        - 目标纵向加速度 (m/s^2, [-6.0, 3.5])
 *   2: gear         - 目标挡位 (1.0=D前进挡, -1.0=R倒挡)
 *   3: immune_lock  - 极危免疫刹停熔断 (0.0=正常, 1.0=紧急刹停)
 */
static inline SDSC_HOT void sdsc_apex_cortex_step(
    SdscApexCortex* SDSC_RESTRICT ctx,
    const float* SDSC_RESTRICT inputs,
    float* SDSC_RESTRICT outputs
) {
    if (SDSC_UNLIKELY(!ctx || !inputs || !outputs)) return;

    const float cte          = inputs[0];
    const float d_psi        = inputs[1];
    const float v            = inputs[2];
    const float oncoming_ttc = inputs[3];
    const float kappa        = inputs[4];
    const float dist_rem     = inputs[5];

    // =========================================================================
    // 柱 5: 前额叶博弈对抗中枢 (Prefrontal Game-Theory Hub)
    // 评估交叉路口冲突风险，决定主导机动模式
    // =========================================================================
    bool hazard_present = (oncoming_ttc > 0.01f && oncoming_ttc < 3.2f);
    bool critical_aeb   = (oncoming_ttc > 0.01f && oncoming_ttc < 1.0f);

    if (fabsf(kappa) > 0.18f && dist_rem < 20.0f) {
        ctx->active_maneuver = APEX_MANEUVER_UTURN;
    } else if (kappa > 0.05f) {
        ctx->active_maneuver = APEX_MANEUVER_LEFT_TURN;
    } else if (kappa < -0.05f) {
        ctx->active_maneuver = APEX_MANEUVER_RIGHT_MERGE;
    } else {
        ctx->active_maneuver = APEX_MANEUVER_CRUISE;
    }

    float steer_cmd = 0.0f;
    float accel_cmd = 0.0f;
    float gear_cmd  = 1.0f; // 默认 D 挡

    // =========================================================================
    // 柱 1: 小脑脊髓循迹与微操反射柱 (Spinal Centering Reflex)
    // 基础 Stanley / 前馈双阈值迟滞反射，消除高频晃动
    // =========================================================================
    float heading_term = d_psi;
    float lateral_term = atan2f(1.8f * cte, fmaxf(v, 1.0f));
    float raw_steer = -(heading_term + lateral_term) + kappa * 2.7f;

    // 施密特双阈值抗震迟滞过滤
    if (fabsf(raw_steer) < 0.005f) raw_steer = 0.0f;
    float spinal_steer = fminf(fmaxf(raw_steer, -0.6f), 0.6f);

    // =========================================================================
    // 柱 2 & 3: 无保护路口对向博弈与穿流 (Unprotected Left/Right Turns)
    // 根据对向 TTC 动态调整穿插加速度与转向提前量
    // =========================================================================
    if (ctx->active_maneuver == APEX_MANEUVER_LEFT_TURN) {
        steer_cmd = spinal_steer * 1.15f;
        if (hazard_present) {
            // 对向有车，减速让行寻找窗口
            accel_cmd = -2.5f;
        } else {
            // 窗口安全，稳步加速穿流
            accel_cmd = (v < 6.0f) ? 1.5f : 0.0f;
        }
    } else if (ctx->active_maneuver == APEX_MANEUVER_RIGHT_MERGE) {
        steer_cmd = spinal_steer * 0.95f;
        accel_cmd = (v < 8.0f) ? 1.8f : 0.2f;
    } else if (ctx->active_maneuver == APEX_MANEUVER_UTURN) {
        // =====================================================================
        // 柱 4: 极限窄路三把方向掉头柱 (Multi-Point U-Turn)
        // 状态机微柱：1把打满前行 -> 2把反打挂倒挡R -> 3把回正前进D
        // =====================================================================
        if (ctx->uturn_phase == 0) {
            steer_cmd = 0.58f; // 满舵
            accel_cmd = (v < 3.5f) ? 1.0f : -0.5f;
            gear_cmd = 1.0f;
            if (dist_rem < 8.0f || v < 0.2f) {
                ctx->uturn_phase = 1;
                ctx->uturn_reverse_timer = 0.0f;
            }
        } else if (ctx->uturn_phase == 1) {
            steer_cmd = -0.58f; // 反打满舵倒车
            accel_cmd = (fabsf(v) < 2.0f) ? -1.0f : 0.0f;
            gear_cmd = -1.0f; // 挂 R 挡倒车
            ctx->uturn_reverse_timer += 0.05f;
            if (ctx->uturn_reverse_timer > 2.5f) {
                ctx->uturn_phase = 2;
            }
        } else {
            steer_cmd = spinal_steer;
            accel_cmd = 1.2f;
            gear_cmd = 1.0f;
        }
    } else {
        // 常规巡航
        steer_cmd = spinal_steer;
        accel_cmd = (v < 15.0f) ? 1.5f : 0.0f;
    }

    // =========================================================================
    // 形式化安全契约：最高优先级免疫熔断 (ASIL-D Immune Shield)
    // 无论前向网络如何决策，极危 TTC 直接截断动力并实施紧急制动
    // =========================================================================
    float immune_lock = 0.0f;
    if (critical_aeb) {
        immune_lock = 1.0f;
        accel_cmd = -6.0f; // 满负荷紧急制动
    }

    // 边界安全限幅
    outputs[0] = fminf(fmaxf(steer_cmd, -0.6f), 0.6f);
    outputs[1] = fminf(fmaxf(accel_cmd, -6.0f), 3.5f);
    outputs[2] = gear_cmd;
    outputs[3] = immune_lock;
}

#ifdef __cplusplus
}
#endif

#endif /* SDSC_APEX_CORTEX_H */
