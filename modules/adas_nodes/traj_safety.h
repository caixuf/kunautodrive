#ifndef TRAJ_SAFETY_H
#define TRAJ_SAFETY_H

/**
 * traj_safety.h — 轨迹与障碍物的"扫掠"碰撞检查（纯逻辑，无 IO / 无全局量）
 *
 * 背景（handoff §4.4 规划 W3）：
 *   Frenet planner 内部的碰撞判定是**点测** —— 它按采样点问"这个点落在障碍物里吗"，
 *   而采样间距可达数米（`frenet_bridge.cpp` 的 `d_t_s=2.0`/FOT 的采样步长）。障碍物若
 *   足够薄，就可能整根落在两个采样点之间：红灯虚拟墙恰恰是 `ol=0.5` 的薄墙，
 *   于是"点测通过、实际撞墙"是可能的，而且**不会报错**（FOT 认为该轨迹有效）。
 *
 * 本模块把判定换成**扫掠**：拿相邻两个轨迹点连成的线段去和障碍物 AABB 求交，
 * 只要线段穿过就命中 —— 采样间距多粗都不会漏。用途是**诊断**：对最终发布的轨迹
 * 做一次扫掠检查，统计"点测会漏、扫掠能抓到"的次数，从而判断 W3 是理论问题还是
 * 实际问题（不改变规划行为）。
 *
 * AABB 约定与 `frenet_bridge.cpp` 一致（世界系轴对齐）：`ol` 沿世界 x、`ow` 沿世界 y。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 扫掠检查结果。 */
typedef struct {
    int hits;    /**< 线段×障碍物 命中次数（同一段可能命中多个障碍物） */
    int seg_idx; /**< 首次命中的段序号（段 i = 点 i→点 i+1），-1 = 无命中 */
    int obs_idx; /**< 首次命中的障碍物序号，-1 = 无命中 */
} TrajSweptResult;

/**
 * 扫掠碰撞检查：相邻轨迹点线段 vs 所有障碍物 AABB。
 *
 * @param xs,ys   轨迹点世界坐标（n_pts 个）
 * @param n_pts   点数（< 2 → 无命中）
 * @param ox,oy   障碍物中心（世界系）
 * @param ow,ol   障碍物宽（沿 y）/ 长（沿 x）
 * @param n_obs   障碍物数量
 */
TrajSweptResult traj_swept_check(const float* xs, const float* ys, int n_pts,
                                 const double* ox, const double* oy,
                                 const double* ow, const double* ol, int n_obs);

/**
 * 点测命中数（**对照组**，复刻 Frenet 内部"只看采样点"的判定）。
 * 仅用于测试/诊断对比：`traj_point_test_hits()==0 && traj_swept_check().hits>0`
 * 就是 W3 定义的"点测漏检"。
 */
int traj_point_test_hits(const float* xs, const float* ys, int n_pts,
                         const double* ox, const double* oy,
                         const double* ow, const double* ol, int n_obs);

#ifdef __cplusplus
}
#endif

#endif /* TRAJ_SAFETY_H */
