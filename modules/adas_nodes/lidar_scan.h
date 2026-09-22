#ifndef LIDAR_SCAN_H
#define LIDAR_SCAN_H

/**
 * lidar_scan.h — LiDAR 观测模型纯逻辑（无 IO / 无日志 / 无全局量）
 *
 * 从 sensor_model_node.c 的 `publish_raycast_points()` 抽出来。理由与
 * imu_protocol / perception_points / pwm_map / sensor_model_weather / slam_math /
 * safety_arbiter / traj_safety 相同：节点内的 static 纯逻辑无法被 CI 覆盖，
 * 只能靠测试副本 + 行号标注（会漂移）。抽成 .c 后 tests/test_adas_nodes_logic.c
 * 直接编同一份实现。
 *
 * 相对旧实现的三个改进（2026-09 观测模型标定，见 docs/HANDOFF_<date>.md）：
 *   1. **3D 扫描**：旧实现只有方位角射线、`p.z` 写死 0（点云是一张平面，
 *      ClusterBounds.min_z/max_z 恒 0 → 无法做地面分割 / 3D 分类）。这里支持
 *      多仰角层（channels），输出真 3D 点。
 *   2. **容量守卫**：旧实现 `cloud.points[cloud.count++]` 无上限检查；只在
 *      n_rays=240 < 容量 2048 时侥幸没炸。射线数一提（尤其密集场景）就越界写。
 *      这里写满即停并把丢弃数上报（`dropped_out`），绝不静默越界。
 *   3. **自带 RNG**：不再依赖全局 `rand()`。单进程 dlopen 模式下 flowsim 会
 *      `srand(random_seed)`、sensor_model 会 `srand(42)`，两者互相踩；自带
 *      xorshift64* 状态让观测模型与场景随机化解耦，且同种子必然复现同点云。
 *
 * 坐标系：**传感器（车体）系** —— 原点在传感器安装点，x 前、y 左、z 上。
 * 因此 z 是"相对传感器的"，地面点 z≈-mount_height_m（本模型不投地面回波）。
 *
 * 量程上限必须与传输契约一致：`LIDAR_POINT_CLOUD_MAX_RANGE_M`
 * （modules/adas_nodes/lidar_contract.h，超了 slam 的 validate 会拒帧）。
 * 测试里有 `LIDAR_SCAN_MAX_RANGE_M == LIDAR_POINT_CLOUD_MAX_RANGE_M` 的断言防漂移。
 */

#include <stdint.h>

#include "adas_msgs_gen.h"   /* LidarPointCloud, LidarPoint */

#ifdef __cplusplus
extern "C" {
#endif

/** 单帧一次扫描最多考虑的目标数（与 sensor_model 的 obs_* 数组容量一致）。 */
#define LIDAR_SCAN_MAX_TARGETS 128

/** 量程硬上限（米）。必须 == lidar_contract.h 的 LIDAR_POINT_CLOUD_MAX_RANGE_M。 */
#define LIDAR_SCAN_MAX_RANGE_M 200.0

/**
 * 扫描计划。全部为"传感器规格 + 安装参数"，与场景无关，可整体配置化。
 */
typedef struct {
    int    azimuth_rays;    /**< 方位射线数（在 hfov 内等分，>=1） */
    int    channels;        /**< 仰角层数（>=1；1 = 旧单层水平扫描，射线恒在传感器水平面，扫不到低于安装面的目标）
                             *   仰角间距必须足够细才能看见远处低于安装面的目标：
                             *   间距 <= (mount-0)/R 量级 —— 车(1.5m)在 R=120m 时张角约 0.7°。 */
    double hfov_deg;        /**< 水平 FOV（度） */
    double vfov_min_deg;    /**< 仰角下界（度，负 = 向下） */
    double vfov_max_deg;    /**< 仰角上界（度） */
    double range_max_m;     /**< 最大量程（米，<= LIDAR_SCAN_MAX_RANGE_M） */
    double mount_height_m;  /**< 传感器安装高度（米，决定地面/目标在 z 上的位置） */
    double noise_std_m;     /**< 绝对测距噪声 1σ（米）—— 与 obs_noise_std_m 同义 */
    double noise_rel;       /**< 相对测距噪声系数（1σ 增量/米）。总 σ = noise_std_m + noise_rel·range。
                             *   必须保持小（真实 LiDAR 为 2~5cm）：σ 一旦接近 DBSCAN eps 或
                             *   车道间距，相邻目标就会糊成一个宽簇（实测 σ=0.08·range 在 33m 处
                             *   达 2.6m，直接把相邻车道的两台车聚成 width=17m 的"车"）。 */
    double loss_rate;       /**< 每根射线的丢失率 [0,1] */
    double intensity_tau_m; /**< intensity 指数衰减常数（米） */
    double sweep_us;        /**< 扫描周期（微秒，用于 time_offset_us） */
} LidarScanPlan;

/** 一个被观测目标：世界系中心 + 尺寸（AABB 取车体轴，近似"与 ego 同向"）。 */
typedef struct {
    double x, y;      /**< 目标中心（世界系） */
    double length;    /**< 沿车体 x 的尺寸（米，>0） */
    double width;     /**< 沿车体 y 的尺寸（米，>0） */
    double height;    /**< z 向尺寸（米，地面 0 → 目标顶） */
} LidarScanTarget;

/* ── 自持 RNG（xorshift64*）────────────────────────────────────
 * 显式状态，不碰全局 rand()。同种子 + 同输入 ⇒ 同点云。 */

/** 由 32 位种子推导初始状态（0 种子也会得到非零状态）。 */
uint64_t lidar_scan_rng_seed(uint32_t seed);

/** [0,1) 均匀分布。 */
double lidar_scan_rng_uniform01(uint64_t* state);

/** 标准正态（Box-Muller）。 */
double lidar_scan_rng_gauss(uint64_t* state);

/* ── 纯函数 ─────────────────────────────────────────────────── */

/**
 * 由障碍物宽度启发式推断高度。
 *
 * `vehicle/state` 只发布 `ol%d/ow%d`，**不发布高度**，3D AABB 必须自己补。
 * 用宽度区分：窄的（<1.0m）按行人 1.7m，其余按乘用车 1.5m。
 */
double lidar_scan_height_from_width(double width_m);

/**
 * 把 plan 夹紧到自洽且不违反传输契约的取值域（含 range_max_m <= 200）。
 *
 * @return 1 表示至少有一个字段被夹紧（调用方据此打一条 WARN），0 表示原样。
 */
int lidar_scan_plan_sanitize(LidarScanPlan* plan);

/**
 * 生成一帧点云（车体系，真 3D）。
 *
 * @param plan        扫描计划（NULL → 返回 0）
 * @param targets     目标数组（可为 NULL 当 n_targets==0）
 * @param n_targets   目标数（> LIDAR_SCAN_MAX_TARGETS 时按上限截断）
 * @param ego_x       ego 世界 x
 * @param ego_y       ego 世界 y
 * @param ego_heading ego 世界朝向（弧度）
 * @param frame_id    输出 frame_id
 * @param timestamp_us 输出时间戳
 * @param rng_state   RNG 状态（NULL → 返回 0）；调用方负责跨帧保持
 * @param out         输出点云（会被 memset）
 * @param dropped_out 可选：因容量写满而丢弃的命中数
 * @return            写入的点数
 */
uint32_t lidar_scan_generate(const LidarScanPlan* plan,
                             const LidarScanTarget* targets, int n_targets,
                             double ego_x, double ego_y, double ego_heading,
                             uint32_t frame_id, uint64_t timestamp_us,
                             uint64_t* rng_state,
                             LidarPointCloud* out, uint32_t* dropped_out);

/**
 * 几何可达距离：给定 plan 与下游 DBSCAN(eps, min_pts)，尺寸为
 * (width, length, height) 的目标能在多远内被聚成一个簇。
 *
 * 两个约束取小：
 *   a) 命中点数 >= min_pts（方位命中数 × 仰角命中数，按角度跨度 / 射线间距估算）
 *   b) 相邻方位命中点间距 <= eps（否则点不连通，DBSCAN 会拆成多个噪声点）
 *
 * 用途：**参数标定的可执行文档**（"为什么选这组射线数"）+ 单测对象。
 * 是近似解析式，不是仿真；只保证单调性（量程↑ / 目标↑ ⇒ 结果不减小）。
 */
double lidar_scan_detectable_range(const LidarScanPlan* plan,
                                   double width, double length, double height,
                                   double eps_m, int min_pts);

#ifdef __cplusplus
}
#endif

#endif /* LIDAR_SCAN_H */
