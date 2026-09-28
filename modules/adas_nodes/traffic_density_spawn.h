/**
 * traffic_density_spawn.h — D3-2 traffic_density auto-spawn 纯算法
 *
 * 把 flowsim_auto_populate_traffic_density() 里的纯算法抽出来：
 *   - 计算某 RouteSeg 上 spawn 数量
 *   - 计算第 i 个 spawn 在该段内的 s_local（带 Poisson 抖动）
 *   - 计算 lane_id（lane_spread 模式下交替 -1 / 0）
 *
 * 这些是 header-only + 标量算术，可直接单测。
 * 实际 esmini frenet_to_world 投影留给 flowsim_node.cpp（依赖 esminiRMLib）。
 *
 * 与 planning_ttc_candidate.h / fusion_lane_hint.h 同 pattern：
 *   节点内纯算法 → 抽 header-only → tests/test_adas_nodes_logic.c 共编
 */

#ifndef TRAFFIC_DENSITY_SPAWN_H
#define TRAFFIC_DENSITY_SPAWN_H

#include <math.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 段长 L、间距 spacing、抖动 jitter 下，第 i 个 spawn 在段内的 s_local。
 *
 * @param L             段长 (m)，> 0
 * @param spacing_m     每辆车 s-spacing (m)，> 0；通常 = 1000/cars_per_km
 * @param jitter_m      spawn_jitter_m，>= 0
 * @param i             spawn 索引（0-based）
 * @param rand_uniform  uniform(lo, hi) RNG；本函数调一次
 * @return              s_local ∈ [0, L]；超出 cap 到段边界
 */
static inline double traffic_density_compute_s_local(
    double L, double spacing_m, double jitter_m, int i,
    double (*rand_uniform)(double, double)
) {
    double s = (double)i * spacing_m;
    if (jitter_m > 0.0 && rand_uniform) {
        /* rand_uniform(lo, hi)：本函数约定 rand_uniform(-jitter, +jitter)
         * 直接传值即可。 */
        s += rand_uniform(-jitter_m, jitter_m);
    }
    if (s < 0.0)        s = 0.0;
    if (s > L)          s = L;
    return s;
}

/** 给定 cars_per_km，算 spacing_m（米/辆）。 */
static inline double traffic_density_spacing_from_density(int cars_per_km) {
    if (cars_per_km <= 0) return 1e9;  /* disabled: spacing=∞ */
    return 1000.0 / (double)cars_per_km;
}

/** 段长 L + spacing_m 下，本段 spawn 数 = floor(L/spacing)。
 *  L < spacing → 0（避免同段重叠）。 */
static inline int traffic_density_seg_spawn_count(double L, double spacing_m) {
    if (L < spacing_m || spacing_m <= 0.0) return 0;
    return (int)(L / spacing_m);
}

/** lane_spread 模式下第 i 个 spawn 的 lane_id。
 *  spread=true → 0 与 -1 交替；spread=false → 全部 0。 */
static inline int traffic_density_lane_id(bool spread, int i) {
    if (!spread) return 0;
    return (i % 2 == 0) ? 0 : -1;
}

#ifdef __cplusplus
}
#endif

#endif /* TRAFFIC_DENSITY_SPAWN_H */
