/**
 * lidar_scan.c — LiDAR 观测模型纯逻辑实现（详见 lidar_scan.h）
 */

#include "lidar_scan.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LIDAR_SCAN_DEG2RAD (M_PI / 180.0)

/* ── xorshift64*：自持状态，不碰全局 rand() ───────────────────── */

uint64_t lidar_scan_rng_seed(uint32_t seed) {
    /* splitmix 常量做一次混合，0 种子也得到非零状态 */
    uint64_t s = (uint64_t)seed * 0x9E3779B97F4A7C15ULL + 0xBF58476D1CE4E5B9ULL;
    if (s == 0) s = 0x2545F4914F6CDD1DULL;
    return s;
}

static uint64_t rng_next(uint64_t* state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

double lidar_scan_rng_uniform01(uint64_t* state) {
    /* 取高 53 位映射到 [0,1)：double 尾数精度 */
    return (double)(rng_next(state) >> 11) * (1.0 / 9007199254740992.0);
}

double lidar_scan_rng_gauss(uint64_t* state) {
    double u1 = lidar_scan_rng_uniform01(state);
    if (u1 < 1e-12) u1 = 1e-12;   /* log(0) 保护 */
    const double u2 = lidar_scan_rng_uniform01(state);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ── 几何 ───────────────────────────────────────────────────── */

double lidar_scan_height_from_width(double width_m) {
    if (!(width_m > 0.0)) return 1.5;
    return (width_m < 1.0) ? 1.7 : 1.5;
}

/* 单轴 slab 测试：收窄 [*tmin, *tmax]。射线与该轴范围无交 → 0。 */
static int slab(double o, double d, double lo, double hi,
                double* tmin, double* tmax) {
    const double EPS = 1e-12;
    if (fabs(d) < EPS) {
        return (o >= lo && o <= hi) ? 1 : 0;
    }
    double t1 = (lo - o) / d;
    double t2 = (hi - o) / d;
    if (t1 > t2) { const double tmp = t1; t1 = t2; t2 = tmp; }
    if (t1 > *tmin) *tmin = t1;
    if (t2 < *tmax) *tmax = t2;
    return (*tmin <= *tmax) ? 1 : 0;
}

/* 3D 射线 vs 3D AABB：返回正 hit 距离，无 hit 返回 -1。
 * 射线原点在盒内时返回出射距离（tmax），与旧 2D 实现语义一致。 */
static double ray_aabb_3d(double ox, double oy, double oz,
                          double dx, double dy, double dz,
                          double cx, double cy, double cz,
                          double hx, double hy, double hz) {
    double tmin = -INFINITY;
    double tmax =  INFINITY;
    if (!slab(ox, dx, cx - hx, cx + hx, &tmin, &tmax)) return -1.0;
    if (!slab(oy, dy, cy - hy, cy + hy, &tmin, &tmax)) return -1.0;
    if (!slab(oz, dz, cz - hz, cz + hz, &tmin, &tmax)) return -1.0;
    if (tmax < 0.0) return -1.0;
    return (tmin > 0.0) ? tmin : tmax;
}

/* ── plan 夹紧 ─────────────────────────────────────────────── */

#define CLAMP_FIELD(field, lo, hi)                       \
    do {                                                 \
        if (plan->field < (lo)) { plan->field = (lo); changed = 1; } \
        else if (plan->field > (hi)) { plan->field = (hi); changed = 1; } \
    } while (0)

int lidar_scan_plan_sanitize(LidarScanPlan* plan) {
    if (!plan) return 0;
    int changed = 0;

    CLAMP_FIELD(azimuth_rays, 1, 2048);
    CLAMP_FIELD(channels, 1, 64);
    CLAMP_FIELD(hfov_deg, 1.0, 360.0);
    CLAMP_FIELD(vfov_min_deg, -90.0, 90.0);
    CLAMP_FIELD(vfov_max_deg, -90.0, 90.0);
    CLAMP_FIELD(range_max_m, 1.0, LIDAR_SCAN_MAX_RANGE_M);
    CLAMP_FIELD(mount_height_m, 0.0, 5.0);
    CLAMP_FIELD(noise_std_m, 0.0, 5.0);
    CLAMP_FIELD(noise_rel, 0.0, 0.1);
    CLAMP_FIELD(loss_rate, 0.0, 1.0);
    CLAMP_FIELD(intensity_tau_m, 0.1, 1000.0);
    CLAMP_FIELD(sweep_us, 0.0, 1.0e6);

    /* 仰角必须留出非零跨度，否则多层退化成单层（channels 也会被规范） */
    if (plan->vfov_max_deg <= plan->vfov_min_deg) {
        plan->vfov_max_deg = plan->vfov_min_deg + 1.0;
        if (plan->vfov_max_deg > 90.0) {
            plan->vfov_max_deg = 90.0;
            plan->vfov_min_deg = 89.0;
        }
        changed = 1;
    }
    return changed;
}

#undef CLAMP_FIELD

/* ── 点云生成 ───────────────────────────────────────────────── */

uint32_t lidar_scan_generate(const LidarScanPlan* plan,
                             const LidarScanTarget* targets, int n_targets,
                             double ego_x, double ego_y, double ego_heading,
                             uint32_t frame_id, uint64_t timestamp_us,
                             uint64_t* rng_state,
                             LidarPointCloud* out, uint32_t* dropped_out) {
    if (dropped_out) *dropped_out = 0;
    if (!plan || !out || !rng_state) return 0;
    if (n_targets < 0) n_targets = 0;
    if (n_targets > LIDAR_SCAN_MAX_TARGETS) n_targets = LIDAR_SCAN_MAX_TARGETS;
    if (plan->azimuth_rays < 1 || plan->channels < 1) return 0;

    const uint32_t capacity =
        (uint32_t)(sizeof(out->points) / sizeof(out->points[0]));
    memset(out, 0, sizeof(*out));
    out->frame_id     = frame_id;
    out->timestamp_us = timestamp_us;
    out->sensor_id    = 0;

    const int    az_rays   = plan->azimuth_rays;
    const int    channels  = plan->channels;
    const double hfov      = plan->hfov_deg * LIDAR_SCAN_DEG2RAD;
    const double vmin      = plan->vfov_min_deg * LIDAR_SCAN_DEG2RAD;
    const double vmax      = plan->vfov_max_deg * LIDAR_SCAN_DEG2RAD;
    const double mount     = plan->mount_height_m;
    const double range_max = plan->range_max_m;
    const double tau       = (plan->intensity_tau_m > 0.0) ? plan->intensity_tau_m : 30.0;

    /* 自车姿态三角函数（ENU 车体系右手定则：x前 y左 z上） */
    const double cos_yaw   = cos(ego_heading);
    const double sin_yaw   = sin(ego_heading);
    const double cos_pitch = cos(plan->ego_pitch);
    const double sin_pitch = sin(plan->ego_pitch);
    const double cos_roll  = cos(plan->ego_roll);
    const double sin_roll  = sin(plan->ego_roll);

    /* 传感器世界坐标 */
    const double sensor_world_x = ego_x;
    const double sensor_world_y = ego_y;
    const double sensor_world_z = plan->ego_z + mount;

    /* 目标预处理：转入世界坐标与自身局部坐标系（支持 3D OBB 旋转框） */
    double t_ox[LIDAR_SCAN_MAX_TARGETS], t_oy[LIDAR_SCAN_MAX_TARGETS],
           t_oz[LIDAR_SCAN_MAX_TARGETS];
    double t_hx[LIDAR_SCAN_MAX_TARGETS], t_hy[LIDAR_SCAN_MAX_TARGETS],
           t_hz[LIDAR_SCAN_MAX_TARGETS];
    double t_cos_h[LIDAR_SCAN_MAX_TARGETS], t_sin_h[LIDAR_SCAN_MAX_TARGETS];
    int    t_active[LIDAR_SCAN_MAX_TARGETS];

    for (int i = 0; i < n_targets; i++) {
        const double len = (targets[i].length > 0.0) ? targets[i].length : 4.6;
        const double wid = (targets[i].width  > 0.0) ? targets[i].width  : 2.0;
        const double hgt = (targets[i].height > 0.0) ? targets[i].height : 1.5;

        t_hx[i] = 0.5 * len;
        t_hy[i] = 0.5 * wid;
        t_hz[i] = 0.5 * hgt;

        const double center_world_x = targets[i].x;
        const double center_world_y = targets[i].y;
        const double center_world_z = targets[i].z + 0.5 * hgt;

        /* 传感器原点相对目标中心的矢量（世界系） */
        const double delta_x = sensor_world_x - center_world_x;
        const double delta_y = sensor_world_y - center_world_y;
        const double delta_z = sensor_world_z - center_world_z;

        /* 目标朝向角 (若无显式 heading 且为 0 则为 0) */
        const double th = (targets[i].has_heading || targets[i].heading != 0.0)
                        ? targets[i].heading : 0.0;
        t_cos_h[i] = cos(th);
        t_sin_h[i] = sin(th);

        /* 将传感器相对原点旋入目标局部坐标系 */
        t_ox[i] =  delta_x * t_cos_h[i] + delta_y * t_sin_h[i];
        t_oy[i] = -delta_x * t_sin_h[i] + delta_y * t_cos_h[i];
        t_oz[i] =  delta_z;

        /* 外接球预筛：相对距离超量程则跳过 */
        const double r_center = sqrt(delta_x * delta_x + delta_y * delta_y + delta_z * delta_z);
        const double r_bound  = sqrt(t_hx[i] * t_hx[i] + t_hy[i] * t_hy[i] + t_hz[i] * t_hz[i]);
        t_active[i] = (r_center - r_bound <= range_max) ? 1 : 0;
    }

    uint32_t dropped = 0;
    for (int c = 0; c < channels; c++) {
        /* channels==1 → 单一水平层（与旧实现一致：elevation 固定 0） */
        const double el = (channels == 1)
                        ? 0.0
                        : vmin + (vmax - vmin) * (double)c / (double)(channels - 1);
        const double cos_el = cos(el);
        const double dz_s   = sin(el);

        for (int r = 0; r < az_rays; r++) {
            const double frac = (az_rays == 1) ? 0.5
                                               : (double)r / (double)(az_rays - 1);
            const double az = -hfov * 0.5 + frac * hfov;
            const double dx_s = cos_el * cos(az);
            const double dy_s = cos_el * sin(az);

            /* 传感器系射线 (dx_s, dy_s, dz_s) 旋转至世界系 (dx_w, dy_w, dz_w)
             * 旋转顺序：R_z(yaw) * R_y(pitch) * R_x(roll) */
            const double x1 = dx_s;
            const double y1 = dy_s * cos_roll - dz_s * sin_roll;
            const double z1 = dy_s * sin_roll + dz_s * cos_roll;

            const double x2 =  x1 * cos_pitch + z1 * sin_pitch;
            const double y2 =  y1;
            const double z2 = -x1 * sin_pitch + z1 * cos_pitch;

            const double dx_w = x2 * cos_yaw - y2 * sin_yaw;
            const double dy_w = x2 * sin_yaw + y2 * cos_yaw;
            const double dz_w = z2;

            double t_best  = range_max;
            int    hit_idx = -1;

            /* 与全部 3D OBB 目标求交 */
            for (int i = 0; i < n_targets; i++) {
                if (!t_active[i]) continue;
                /* 将世界系射线方向旋入目标局部坐标系 */
                const double d_tx =  dx_w * t_cos_h[i] + dy_w * t_sin_h[i];
                const double d_ty = -dx_w * t_sin_h[i] + dy_w * t_cos_h[i];
                const double d_tz =  dz_w;

                const double t = ray_aabb_3d(t_ox[i], t_oy[i], t_oz[i],
                                             d_tx, d_ty, d_tz,
                                             0.0, 0.0, 0.0,
                                             t_hx[i], t_hy[i], t_hz[i]);
                if (t > 0.0 && t < t_best) {
                    t_best  = t;
                    hit_idx = i;
                }
            }

            /* 可选地面射线求交（enable_ground_hits）：向下的射线与局部水平地面相交 */
            if (plan->enable_ground_hits && dz_w < -1e-6) {
                const double t_ground = mount / (-dz_w);
                if (t_ground > 0.0 && t_ground < t_best && t_ground <= range_max) {
                    t_best  = t_ground;
                    hit_idx = 999999; /* 地面命中标记 */
                }
            }

            if (hit_idx < 0) continue;

            if (plan->loss_rate > 0.0 &&
                lidar_scan_rng_uniform01(rng_state) < plan->loss_rate) {
                continue;
            }

            /* 测距噪声：σ = 绝对项 + 相对项·range */
            double t_noisy = t_best;
            const double sigma = plan->noise_std_m + plan->noise_rel * t_best;
            if (sigma > 0.0) {
                t_noisy += sigma * lidar_scan_rng_gauss(rng_state);
            }
            if (t_noisy < 0.1)         t_noisy = 0.1;
            if (t_noisy > range_max)   t_noisy = range_max;

            if (out->count >= capacity) {
                dropped++;   /* 容量守卫：绝不越界写 */
                continue;
            }
            LidarPoint* p = &out->points[out->count];
            p->x = (float)(dx_s * t_noisy);
            p->y = (float)(dy_s * t_noisy);
            p->z = (float)(dz_s * t_noisy);
            p->intensity      = (float)exp(-t_best / tau);
            p->time_offset_us = (uint32_t)(plan->sweep_us * frac);
            out->count++;
        }
    }

    if (dropped_out) *dropped_out = dropped;
    return out->count;
}

/* ── 可达距离（标定用近似解析式）───────────────────────────── */

double lidar_scan_detectable_range(const LidarScanPlan* plan,
                                   double width, double length, double height,
                                   double eps_m, int min_pts) {
    if (!plan) return 0.0;
    if (!(width  > 0.0)) width  = 2.0;
    if (!(length > 0.0)) length = 4.6;
    if (!(height > 0.0)) height = 1.5;
    if (!(eps_m  > 0.0)) eps_m  = 2.0;
    if (min_pts < 1)     min_pts = 4;
    if (plan->azimuth_rays < 1 || plan->range_max_m <= 0.0) return 0.0;

    const double d_az = (plan->hfov_deg * LIDAR_SCAN_DEG2RAD) /
                        (double)plan->azimuth_rays;
    if (!(d_az > 0.0)) return 0.0;

    const int channels = (plan->channels > 0) ? plan->channels : 1;
    const double d_el = (channels > 1)
        ? ((plan->vfov_max_deg - plan->vfov_min_deg) * LIDAR_SCAN_DEG2RAD) /
          (double)(channels - 1)
        : 0.0;

    /* 约束 b：相邻方位命中点的横向间距 = r·d_az，必须 <= eps 才能连通 */
    const double r_eps = eps_m / d_az;

    for (double r = plan->range_max_m; r >= 1.0; r -= 0.25) {
        if (r > r_eps) continue;
        const double hits_az = floor(width  / (r * d_az)) + 1.0;
        const double hits_el = (d_el > 0.0)
                             ? floor(height / (r * d_el)) + 1.0
                             : 1.0;
        if (hits_az * hits_el >= (double)min_pts) return r;
    }
    return 0.0;
}
