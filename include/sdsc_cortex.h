/**
 * sdsc_cortex.h - 软件定义硅基细胞计算机 C11 零 GC 独立推理内核 (微架构加速版)
 * 
 * 自动生成代码 - 严禁手动修改。
 * 遵循车规级安全标准与 ISO C11 规范，64 字节缓存行对齐，零堆内存分配，确定性硬实时执行。
 */
#ifndef SDSC_CORTEX_H
#define SDSC_CORTEX_H

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

#define SDSC_MAX_CELLS     128
#define SDSC_MAX_SYNAPSES  512
#define SDSC_MAX_INPUTS    16
#define SDSC_MAX_OUTPUTS   8

typedef enum {
    SDSC_OP_INPUT = 0,
    SDSC_OP_EMA = 1,
    SDSC_OP_DIFF = 2,
    SDSC_OP_INTEGRAL = 3,
    SDSC_OP_SUM = 4,
    SDSC_OP_SUB = 5,
    SDSC_OP_MULTIPLY = 6,
    SDSC_OP_RATIO = 7,
    SDSC_OP_ABS = 8,
    SDSC_OP_OSCILLATOR = 9,
    SDSC_OP_QUADRATIC = 10,
    SDSC_OP_GATE_THRESHOLD = 11,
    SDSC_OP_GATE_HYSTERESIS = 12,
    SDSC_OP_GATE_DEADZONE = 13,
    SDSC_OP_ACT_POSITIVE = 14,
    SDSC_OP_ACT_NEGATIVE = 15,
    SDSC_OP_ACT_DEF_RESET = 16,
    SDSC_OP_ACT_IMMUNE_BLOCK = 17
} SdscOpType;

typedef struct {
    int cell_count;
    int synapse_count;
    int input_count;
    int output_count;
    
    uint8_t  op_types[SDSC_MAX_CELLS] SDSC_ALIGN64;
    float    params[SDSC_MAX_CELLS][2] SDSC_ALIGN64;
    float    states[SDSC_MAX_CELLS] SDSC_ALIGN64;       /* 内部积分/膜电位持久状态 */
    float    outputs[SDSC_MAX_CELLS] SDSC_ALIGN64;      /* 当前时刻放电电位 */
    float    prev_outputs[SDSC_MAX_CELLS] SDSC_ALIGN64; /* 上一时刻时序输出 (循环环路) */
    float    prev_inputs[SDSC_MAX_CELLS] SDSC_ALIGN64;  /* 微分算子上一时刻输入 */
    bool     latch_states[SDSC_MAX_CELLS] SDSC_ALIGN64; /* 迟滞门控内部锁存器 */

    /* 突触拓扑 [from_idx, to_idx, to_port] */
    uint16_t syn_from[SDSC_MAX_SYNAPSES] SDSC_ALIGN64;
    uint16_t syn_to[SDSC_MAX_SYNAPSES] SDSC_ALIGN64;
    uint8_t  syn_port[SDSC_MAX_SYNAPSES] SDSC_ALIGN64;
    float    syn_weight[SDSC_MAX_SYNAPSES] SDSC_ALIGN64;
    bool     syn_is_recurrent[SDSC_MAX_SYNAPSES] SDSC_ALIGN64;

    /* 拓扑执行序列 */
    uint16_t exec_order[SDSC_MAX_CELLS] SDSC_ALIGN64;
    int      exec_count;

    /* 端口映射 */
    uint16_t input_map[SDSC_MAX_INPUTS];
    uint16_t output_map[SDSC_MAX_OUTPUTS];
} SDSC_ALIGN64 SdscCortex;

static inline void sdsc_cortex_reset(SdscCortex* ctx) {
    if (SDSC_UNLIKELY(!ctx)) return;
    for (int i = 0; i < SDSC_MAX_CELLS; ++i) {
        ctx->states[i] = 0.0f;
        ctx->outputs[i] = 0.0f;
        ctx->prev_outputs[i] = 0.0f;
        ctx->prev_inputs[i] = 0.0f;
        ctx->latch_states[i] = false;
    }
}

static inline void sdsc_cortex_init_default_adas(SdscCortex* ctx) {
    if (SDSC_UNLIKELY(!ctx)) return;
    memset(ctx, 0, sizeof(SdscCortex));
    
    // 4 输入: [0: distance, 1: rel_speed, 2: lane_offset, 3: ttc]
    ctx->input_count = 4;
    ctx->output_count = 4; // [0: accel, 1: brake, 2: steer, 3: immune_block]
    
    // 配置 8 个核心细胞
    ctx->cell_count = 8;
    ctx->exec_count = 8;

    // 0~3: Input receptors
    ctx->op_types[0] = SDSC_OP_INPUT; ctx->params[0][0] = 0.1f;
    ctx->op_types[1] = SDSC_OP_INPUT; ctx->params[1][0] = 0.1f;
    ctx->op_types[2] = SDSC_OP_INPUT; ctx->params[2][0] = 1.0f;
    ctx->op_types[3] = SDSC_OP_INPUT; ctx->params[3][0] = 0.5f;

    // 4: EMA 平滑阻尼
    ctx->op_types[4] = SDSC_OP_EMA; ctx->params[4][0] = 0.45f;

    // 5: 施密特双阈值迟滞 (防横向震颤)
    ctx->op_types[5] = SDSC_OP_GATE_HYSTERESIS; ctx->params[5][0] = -0.25f; ctx->params[5][1] = 0.25f;

    // 6: 紧急避障动作
    ctx->op_types[6] = SDSC_OP_ACT_POSITIVE; ctx->params[6][0] = 1.0f;

    // 7: 免疫熔断刹车 (AEB)
    ctx->op_types[7] = SDSC_OP_ACT_IMMUNE_BLOCK; ctx->params[7][0] = 1.0f;

    for (int i = 0; i < 8; ++i) ctx->exec_order[i] = (uint16_t)i;

    // 突触连接
    ctx->synapse_count = 6;
    // 0 (dist) -> 4 (ema)
    ctx->syn_from[0] = 0; ctx->syn_to[0] = 4; ctx->syn_port[0] = 0; ctx->syn_weight[0] = 1.0f; ctx->syn_is_recurrent[0] = false;
    // 4 (ema) -> 6 (act_pos)
    ctx->syn_from[1] = 4; ctx->syn_to[1] = 6; ctx->syn_port[1] = 0; ctx->syn_weight[1] = 0.8f; ctx->syn_is_recurrent[1] = false;
    // 2 (lane_offset) -> 5 (hysteresis)
    ctx->syn_from[2] = 2; ctx->syn_to[2] = 5; ctx->syn_port[2] = 0; ctx->syn_weight[2] = 1.2f; ctx->syn_is_recurrent[2] = false;
    // 3 (ttc) -> 7 (immune_block)
    ctx->syn_from[3] = 3; ctx->syn_to[3] = 7; ctx->syn_port[3] = 0; ctx->syn_weight[3] = -2.5f; ctx->syn_is_recurrent[3] = false;
    // 1 (rel_speed) -> 7 (immune_block, port 1)
    ctx->syn_from[4] = 1; ctx->syn_to[4] = 7; ctx->syn_port[4] = 1; ctx->syn_weight[4] = -1.5f; ctx->syn_is_recurrent[4] = false;
    // 5 (hysteresis) -> 4 (EMA 反馈阻尼) - 时序循环
    ctx->syn_from[5] = 5; ctx->syn_to[5] = 4; ctx->syn_port[5] = 1; ctx->syn_weight[5] = 0.3f; ctx->syn_is_recurrent[5] = true;

    ctx->input_map[0] = 0;
    ctx->input_map[1] = 1;
    ctx->input_map[2] = 2;
    ctx->input_map[3] = 3;

    ctx->output_map[0] = 6; // Accel
    ctx->output_map[1] = 4; // Brake/Decel
    ctx->output_map[2] = 5; // Steer
    ctx->output_map[3] = 7; // ImmuneBlock
}

static inline SDSC_HOT void sdsc_cortex_forward(
    SdscCortex* SDSC_RESTRICT ctx,
    const float* SDSC_RESTRICT inputs,
    float* SDSC_RESTRICT outputs
) {
    if (SDSC_UNLIKELY(!ctx || !inputs || !outputs)) return;

    float port_inputs[SDSC_MAX_CELLS * 2] SDSC_ALIGN64;
    memset(port_inputs, 0, sizeof(port_inputs));

    const int syn_count = ctx->synapse_count;
    // 1. 循环突触注入 (Recurrent Synapses)
    for (int i = 0; i < syn_count; ++i) {
        if (ctx->syn_is_recurrent[i]) {
            uint16_t from = ctx->syn_from[i];
            uint16_t to = ctx->syn_to[i];
            uint8_t port = ctx->syn_port[i];
            port_inputs[to * 2 + port] += ctx->prev_outputs[from] * ctx->syn_weight[i];
        }
    }

    // 2. 前向拓扑推进 (Forward Topological Execution)
    const int exec_count = ctx->exec_count;
    for (int k = 0; k < exec_count; ++k) {
        uint16_t idx = ctx->exec_order[k];
        uint8_t op = ctx->op_types[idx];
        float in0 = port_inputs[idx * 2 + 0];
        float in1 = port_inputs[idx * 2 + 1];
        float out = 0.0f;

        switch (op) {
            case SDSC_OP_INPUT:
                out = inputs[idx] * ctx->params[idx][0];
                break;
            case SDSC_OP_EMA: {
                float alpha = ctx->params[idx][0];
                ctx->states[idx] = alpha * in0 + (1.0f - alpha) * ctx->states[idx];
                out = ctx->states[idx];
                break;
            }
            case SDSC_OP_DIFF:
                out = in0 - ctx->prev_inputs[idx];
                ctx->prev_inputs[idx] = in0;
                break;
            case SDSC_OP_INTEGRAL:
                ctx->states[idx] += in0 * ctx->params[idx][0];
                out = ctx->states[idx];
                break;
            case SDSC_OP_SUM:
                out = in0 + in1;
                break;
            case SDSC_OP_SUB:
                out = in0 - in1;
                break;
            case SDSC_OP_MULTIPLY:
                out = in0 * in1;
                break;
            case SDSC_OP_ABS:
                out = fabsf(in0);
                break;
            case SDSC_OP_GATE_THRESHOLD:
                out = (in0 > ctx->params[idx][0]) ? 1.0f : 0.0f;
                break;
            case SDSC_OP_GATE_HYSTERESIS: {
                float low = ctx->params[idx][0];
                float high = ctx->params[idx][1];
                if (in0 > high) ctx->latch_states[idx] = true;
                else if (in0 < low) ctx->latch_states[idx] = false;
                out = ctx->latch_states[idx] ? 1.0f : -1.0f;
                break;
            }
            case SDSC_OP_GATE_DEADZONE: {
                float d = ctx->params[idx][0];
                out = (fabsf(in0) < d) ? 0.0f : in0;
                break;
            }
            case SDSC_OP_ACT_POSITIVE:
            case SDSC_OP_ACT_NEGATIVE:
            case SDSC_OP_ACT_DEF_RESET:
                out = in0 * ctx->params[idx][0];
                break;
            case SDSC_OP_ACT_IMMUNE_BLOCK:
                out = (in0 > 1.0f || in1 > 1.0f) ? 1.0f : 0.0f;
                break;
            default:
                out = in0;
                break;
        }

        ctx->outputs[idx] = out;

        // 向前传播到后续端口 (Feedforward Propagation)
        for (int s = 0; s < syn_count; ++s) {
            if (!ctx->syn_is_recurrent[s] && ctx->syn_from[s] == idx) {
                uint16_t to = ctx->syn_to[s];
                uint8_t port = ctx->syn_port[s];
                port_inputs[to * 2 + port] += out * ctx->syn_weight[s];
            }
        }
    }

    // 3. 更新上一时刻状态
    for (int i = 0; i < ctx->cell_count; ++i) {
        ctx->prev_outputs[i] = ctx->outputs[i];
    }

    // 4. 提取输出
    for (int j = 0; j < ctx->output_count; ++j) {
        outputs[j] = ctx->outputs[ctx->output_map[j]];
    }
}

#ifdef __cplusplus
}
#endif

#endif /* SDSC_CORTEX_H */
