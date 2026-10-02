/**
 * traffic_density_spawn.h — D3-2 traffic_density auto-spawn 纯算法
 *
 * 把 flowsim_auto_populate_traffic_density() 里的纯算法抽出来：
 *   - 计算某 RouteSeg 上 spawn 数量
 *   - 计算第 i 个 spawn 在该段内的 s_local（带 Poisson 抖动）
 *   - 计算车道槽位（lane_spread 模式下 0/1 交替；真实 lane_id 由调用方查路网决定）
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

/** lane_spread 模式下第 i 个 spawn 的**车道槽位**（0-based，恒 >= 0）。
 *
 * 返回槽位而非 lane_id：真实 lane_id 取决于路网（哪几条是同向可行驶车道），
 * 只能由 flowsim_node 查 drivable_lane_ids 决定；本函数只管"第 i 辆落在第几槽"。
 *   spread=true  → 0,1,0,1,…（交替两条同向车道）
 *   spread=false → 恒 0（单车道压力测试）
 *
 * ⚠️ 本函数历史上直接返回 lane_id（0 与 -1 交替），是 D3-2 首个 invariant 爆炸的
 *    根因：lane 0 是 OpenDRIVE 参考线（type="none"，不可行驶），esmini 对它的
 *    pd.h 恒为 π —— 落上去的同向 NPC 车头朝后逆向行驶（motion_direction / Δs
 *    invariant 失败；与 npc_ai.cpp P3 是同一族 bug）。故本函数不再触碰 lane_id
 *    语义，映射到真实车道是调用方的事，且永远不要落回 0。 */
static inline int traffic_density_lane_slot(bool spread, int i) {
    if (!spread) return 0;
    return i % 2;
}

#ifdef __cplusplus
}
#endif

#endif /* TRAFFIC_DENSITY_SPAWN_H */
