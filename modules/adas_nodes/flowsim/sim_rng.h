#ifndef FLOWSIM_SIM_RNG_H
#define FLOWSIM_SIM_RNG_H

/*
 * 仿真自持随机源（方向五：确定性）。header-only，纯函数，可单测。
 *
 * 为什么不用全局 rand()：它是**进程级**状态。dlopen 单进程模式下
 * flowsim 用 srand(random_seed) 播种后，sensor_model / perception 在各自
 * init 里又 srand(42) 把它覆盖；npc_ai 的路口转向意图、泊松到达抽样都吃
 * 这条被污染的流 → 同 seed 不保证同序列（2026-11-07 故障表 §6）。
 *
 * 解决方式：每个模块持有自己的 xorshift64 状态，播种一次后只在自身逻辑里
 * 推进。与 `lidar_scan.h` 的 xorshift 是同一手法（那边已有"同种子同点云"单测）。
 *
 * 注意：RNG 隔离只解决"随机流互相踩"；墙钟节拍与线程调度仍会让整条 demo
 * 的**涌现指标**不可复现（见 docs/FAILURE_MODES.md §6 的同一条）。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t state;
} SimRng;

static inline void sim_rng_seed(SimRng* rng, uint64_t seed) {
    /* splitmix64 混淆种子：xorshift64 直接从 42 这类小状态起步时，前几次
     * 上移后的高 32 位严重偏小（早期 uniform01/整数抽样有肉眼可见偏差）。
     * 先打散再存；0 状态仍用黄金比例常数兜底。 */
    uint64_t z = seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    rng->state = z ? z : 0x9E3779B97F4A7C15ULL;
}

static inline uint32_t sim_rng_next_u32(SimRng* rng) {
    uint64_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng->state = x;
    return (uint32_t)(x >> 32);
}

/** [0,1) 均匀分布。 */
static inline double sim_rng_uniform01(SimRng* rng) {
    return (double)(sim_rng_next_u32(rng) & 0xFFFFFFu) / 16777216.0;
}

/** [0, n) 整数；n<=0 返回 0（不改变 RNG 状态，调用方应保证 n>0）。 */
static inline int sim_rng_below(SimRng* rng, int n) {
    if (n <= 0) return 0;
    return (int)(sim_rng_next_u32(rng) % (uint32_t)n);
}

#ifdef __cplusplus
}
#endif

#endif /* FLOWSIM_SIM_RNG_H */