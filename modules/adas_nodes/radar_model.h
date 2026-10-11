#ifndef RADAR_MODEL_H
#define RADAR_MODEL_H

/*
 * 毫米波雷达观测模型 — L3-P2 冗余（方向六）。
 *
 * 为什么需要：P2 要求「冗余（**雷达模型**、定位交叉校验、看门狗）」。
 * `sensor_model` 此前只有 lidar/gps/camera —— 相机在夜间/逆光/恶劣天气显著退化，
 * LiDAR 量程有限（默认 60m）。雷达补的正是这两块短板：测距远（默认 200m）、
 * **不受光照影响**，构成"任一模态退化时剩余模态仍能支撑安全"的冗余。
 *
 * 与 LiDAR 的差异（决定它是冗余而非重复）：
 *   1. 只报方位角（无高度）→ 目标数远少于点云；
 *   2. 天气只影响 RCS 幅度（雨衰），**不影响可达性** —— 与相机的乘性可见度衰减
 *      是两种退化模式，这正是冗余的价值；
 *   3. 报径向速度（range_rate），是 LiDAR 单帧点云所没有的直接量测。
 *
 * 抽离成独立模块（而非留在 sensor_model_node.c 里做 static）的理由：
 * 与 lidar_scan / sensor_model_weather 同一策略 —— 节点与单测链接**同一份实现**，
 * 没有"副本漂移"风险（见 tests/test_adas_nodes_logic.c 的测试策略说明）。
 */

#include "adas_msgs_gen.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_MAX_TARGETS 64

/** 雷达配置（由 sensor_model 的 params 填充）。 */
typedef struct {
    double range_max_m;          /* 量程上限 */
    double hfov_deg;             /* 水平视场角（以车头为中心，对称） */
    double range_noise_std_m;    /* 测距噪声（均匀分布半宽） */
    double azimuth_noise_std_deg;/* 方位噪声（均匀分布半宽） */
} RadarConfig;

/**
 * 一个待观测目标（世界系 ENU 位置 + 尺寸，仅用于 RCS 幅度）。
 */
typedef struct {
    double x;
    double y;
    double length;
    double width;
} RadarObsTarget;

/**
 * 自车状态（生成径向速度所需）。
 */
typedef struct {
    double x;
    double y;
    double heading;   /* 弧度，ENU */
    double speed;     /* m/s，沿车头 */
} RadarEgo;

/**
 * 生成一帧雷达目标。
 *
 * @param cfg        配置
 * @param ego        自车状态
 * @param targets    障碍物（世界系 ENU）
 * @param n_targets  障碍物数量
 * @param weather_attenuation  天气衰减 [0,1]，只作用于 RCS 幅度（不影响可达性）
 * @param rng_state  确定性 RNG 状态（调用方持有；与全局 rand 解耦，保证可复现）
 * @param out        输出帧（其 count 被写入；targets 最多 RADAR_MAX_TARGETS）
 * @return 本帧有效目标数
 */
static inline uint32_t radar_build_frame(const RadarConfig* cfg,
                                        const RadarEgo* ego,
                                        const RadarObsTarget* targets,
                                        int n_targets,
                                        double weather_attenuation,
                                        uint64_t* rng_state,
                                        RadarFrame* out) {
    if (!cfg || !ego || !out) return 0;

    memset(out, 0, sizeof(*out));
    const double half_fov = cfg->hfov_deg * 0.5 * M_PI / 180.0;
    const double range_max = cfg->range_max_m;
    const double rcs_atten = 1.0 - 0.3 * weather_attenuation;
    uint32_t n = 0;

    for (int i = 0; i < n_targets && n < RADAR_MAX_TARGETS; i++) {
        const double dx = targets[i].x - ego->x;
        const double dy = targets[i].y - ego->y;
        const double range = hypot(dx, dy);
        if (range > range_max) continue;
        if (range < 1e-6) continue;

        /* 方位角以车头为基准，正 = 左（+y 侧），归一化到 (-pi, pi] */
        double az = atan2(dy, dx) - ego->heading;
        while (az >  M_PI) az -= 2.0 * M_PI;
        while (az < -M_PI) az += 2.0 * M_PI;
        if (fabs(az) > half_fov) continue;

        /* 径向速度：沿视线方向的相对速度，正 = 远离。
         * 障碍物只报位置（vehicle/state 无各障碍速度），故用 ego 速度投影。 */
        const double los_x = dx / range;
        const double los_y = dy / range;
        const double ego_vx = ego->speed * cos(ego->heading);
        const double ego_vy = ego->speed * sin(ego->heading);
        const double range_rate = -(ego_vx * los_x + ego_vy * los_y);

        /* 确定性 RNG（xorshift64）：不碰全局 rand，可复现 */
        uint64_t r = rng_state ? *rng_state : 0x9E3779B97F4A7C15ULL;
        if (r == 0) r = 0x9E3779B97F4A7C15ULL;
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        const double u1 = (double)((r >> 11) & 0xFFFFFFu) / (double)0x1000000u;
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        const double u2 = (double)((r >> 11) & 0xFFFFFFu) / (double)0x1000000u;
        if (rng_state) *rng_state = r;

        const double n_range   = (u1 * 2.0 - 1.0) * cfg->range_noise_std_m;
        const double n_azimuth = (u2 * 2.0 - 1.0) * cfg->azimuth_noise_std_deg;

        out->targets[n].range_m        = (float)(range + n_range);
        out->targets[n].azimuth_deg    = (float)(az * 180.0 / M_PI + n_azimuth);
        out->targets[n].range_rate_mps = (float)range_rate;
        const double area = (targets[i].width > 0.0 && targets[i].length > 0.0)
                          ? (double)targets[i].width * (double)targets[i].length
                          : 4.0;
        out->targets[n].rcs_dbsm       = (float)(10.0 * log10(area) * rcs_atten);
        n++;
    }
    out->count = n;
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* RADAR_MODEL_H */
