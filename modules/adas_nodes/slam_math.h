#ifndef SLAM_MATH_H
#define SLAM_MATH_H

/**
 * slam_math.h — SLAM 节点的纯数学件（无 IO / 无全局量）
 *
 * 抽出来的理由与 imu_protocol / perception_points / pwm_map 相同：节点里的纯逻辑
 * 只能靠测试副本覆盖，会漂移。这里收口两处：
 *   1. 航向归一化到 (-π, π] —— 原在 ekf_slam.c 里逐处内联了 4 遍
 *   2. dry-run 圆轨迹位姿 —— 原在 slam_node.cpp 的 publish 循环里内联
 */

#include <stdint.h>
#include "adas_msgs_gen.h"   /* Pose2D */

#ifdef __cplusplus
extern "C" {
#endif

/** SLAM 位姿来源：与 slam_node.cpp 的 pose.source 约定一致。 */
#define SLAM_POSE_SOURCE_SLAM 2u

/**
 * 把角度规约到 (-π, π]。
 *
 * 用 fmodf 而不是 `while (a > π) a -= 2π`：后者对 |angle| 很大时要转很多圈，
 * 对 NaN 更是死循环（原 ekf_slam.c 的 4 处内联都是这么写的）。语义与旧实现
 * 在有限值上逐位一致（+π 保持 +π，-π 保持 -π）。
 */
float slam_wrap_pi(float angle);

/**
 * dry-run 圆轨迹位姿（半径 R，角速度 1 rad/s，起始相位 0）。
 *
 *   t = poses_published / publish_hz        （publish_hz <= 0 时按 20 处理）
 *   x = R·cos t, y = R·sin t, heading = t + π/2
 *
 * 协方差与 converged 用固定值（dry-run 无真实估计）；source 固定
 * SLAM_POSE_SOURCE_SLAM。
 */
void slam_dry_run_pose(uint64_t poses_published, int publish_hz, float radius_m,
                       Pose2D* out);

#ifdef __cplusplus
}
#endif

#endif /* SLAM_MATH_H */
