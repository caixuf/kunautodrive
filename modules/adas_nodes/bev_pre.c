/**
 * bev_pre.c — BEV 前处理实现（纯函数栅格化）
 *
 * 坐标约定与 perception_node.cpp 完全一致：
 *   x_body =  dx*cos(h) + dy*sin(h)    （前，+x）
 *   y_body = -dx*sin(h) + dy*cos(h)    （左，+y）
 * 栅格 origin（fit[0][0][0] = ch0,row0,col0）位于车体系 (x=-range_x, y=+range_y_half)，
 * 即 col（ix）随 +x 递增、row（iy）随 -y 递增（正 y 在网格顶行 → iy 递增向下）。
 * 这与感知/渲染的「上北/前上」直觉一致，且与 Phase 3 后处理（BEV 车体系→世界）对齐。
 */
#include "bev_pre.h"

#include <string.h>

void bev_pre_config_default(BevPreConfig* cfg) {
    if (!cfg) return;
    cfg->w            = BEV_PRE_DEFAULT_W;
    cfg->h            = BEV_PRE_DEFAULT_H;
    cfg->range_x      = BEV_PRE_DEFAULT_RANGE_X;
    cfg->range_y_half = BEV_PRE_DEFAULT_RANGE_Y_HALF;
    cfg->v_scale      = BEV_PRE_DEFAULT_V_SCALE;
    cfg->channels     = 4;
}

/* 连续格坐标 → 整数下标。
 *
 * 裸 floor() 会把「数学上恰好落在格线」的点，在 libm 残差下推进相邻格：
 *   heading=π/2 时 cos(π/2)≈6e-17 而非 0，世界系正前方 (0, +10m)
 *   得到 y_body≈+6e-16。y=0 是 row 边界（闭端），floor 后 iy 从 4 变成 3。
 * 距最近整数 ≤1e-9 格（典型分辨率下约纳米级，只覆盖旋转残差）时吸附回去，
 * 与精确 0 所在的闭边界格子一致。 */
static int cell_index(double v) {
    const double nearest = round(v);
    if (fabs(v - nearest) <= 1e-9)
        v = nearest;
    return (int)floor(v);
}

/* 障碍物落到哪个格。返回 0 表示在图外。 */
static int project_cell(const BevPreConfig* cfg,
                        double x_body, double y_body,
                        int* ix, int* iy) {
    const double res_x = (2.0 * cfg->range_x) / cfg->w;
    const double res_y = (2.0 * cfg->range_y_half) / cfg->h;

    int c = cell_index((x_body + cfg->range_x) / res_x);
    int r = cell_index((cfg->range_y_half - y_body) / res_y);
    if (c < 0 || c >= cfg->w || r < 0 || r >= cfg->h) return 0;
    *ix = c;
    *iy = r;
    return 1;
}

/* 通道索引：通道优先 NCHW，index = ((ch*H) + r)*W + c */
static size_t idx(const BevPreConfig* cfg, int ch, int r, int c) {
    return ((size_t)ch * (size_t)cfg->h + (size_t)r) * (size_t)cfg->w + (size_t)c;
}

size_t bev_pre_rasterize(const BevPreConfig* cfg,
                         double ego_x, double ego_y, double ego_heading,
                         const BevPreObs* obs, int n_obs,
                         float* feat) {
    if (!cfg || !feat || cfg->w <= 0 || cfg->h <= 0 || cfg->channels < 1) return 0;
    if (n_obs < 0) n_obs = 0;
    if (obs == NULL) n_obs = 0;

    const int W = cfg->w, H = cfg->h, C = cfg->channels;
    const size_t total = (size_t)C * (size_t)H * (size_t)W;
    memset(feat, 0, total * sizeof(float));

    const double ch = cos(ego_heading), sh = sin(ego_heading);
    const double res_x = (2.0 * cfg->range_x) / W;
    /* 简化：单个 bbox 均匀覆盖一格，速度/类型写入该格 */
    for (int i = 0; i < n_obs; ++i) {
        const BevPreObs* o = &obs[i];
        double dx = o->x - ego_x, dy = o->y - ego_y;
        double xb =  dx * ch + dy * sh;   /* 前 +x */
        double yb = -dx * sh + dy * ch;   /* 左 +y */

        int ix, iy;
        if (!project_cell(cfg, xb, yb, &ix, &iy)) continue;

        /* ch0 占据 = 1（饱和但不叠加溢出） */
        float* occ = &feat[idx(cfg, 0, iy, ix)];
        *occ = 1.0f;

        /* ch1 速度：世界速度→车体速度投影，取前向幅值归一化 */
        double vx_b = o->vx * ch + o->vy * sh;
        double vy_b = -o->vx * sh + o->vy * ch;
        double spd = hypot(vx_b, vy_b);
        float vn = (float)(spd / (cfg->v_scale > 0 ? cfg->v_scale : 30.0));
        if (vn > 1.0f) vn = 1.0f;
        float* vcell = &feat[idx(cfg, 1, iy, ix)];
        if (vn > *vcell) *vcell = vn;

        /* ch2 类型弱先验：车 0.9 / 行人 0.5 / 自行车 0.7 / 施工 0.3 */
        float type_prior = 0.3f;
        if (o->type == BEV_OBJ_VEHICLE)      type_prior = 0.9f;
        else if (o->type == BEV_OBJ_PEDESTRIAN) type_prior = 0.5f;
        else if (o->type == BEV_OBJ_CYCLIST) type_prior = 0.7f;
        float* tcell = &feat[idx(cfg, 2, iy, ix)];
        if (type_prior > *tcell) *tcell = type_prior;

        /* ch3：若存在，写入中心强度（可叠加信号强） */
        if (C >= 4) feat[idx(cfg, 3, iy, ix)] += 0.5f;
    }

    (void)res_x; /* 保留公式，目前 bbox 单格代表 */
    return total;
}

size_t bev_pre_rasterize_sensors(const BevPreConfig* cfg,
                                 const BevSensorInput* input,
                                 float* feat) {
    if (!cfg || !feat || cfg->w <= 0 || cfg->h <= 0) return 0;
    const int W = cfg->w, H = cfg->h;
    const size_t total = 6 * (size_t)H * (size_t)W;
    memset(feat, 0, total * sizeof(float));

    if (!input || !input->points || input->n_points <= 0) return total;

    const double res_x = (2.0 * cfg->range_x) / W;
    const double res_y = (2.0 * cfg->range_y_half) / H;

    for (int i = 0; i < input->n_points; ++i) {
        const BevSensorPoint* p = &input->points[i];
        int ix, iy;
        if (!project_cell(cfg, (double)p->x, (double)p->y, &ix, &iy)) continue;

        /* ch0: 占据响应（每命中一采样点累加 0.25，饱和至 1.0） */
        size_t i0 = idx(cfg, 0, iy, ix);
        if (feat[i0] < 1.0f) {
            feat[i0] = fminf(1.0f, feat[i0] + 0.25f);
        }

        /* ch1: 强度与归一化高度响应（[-2.0, +2.0]m 线性映射至 [0, 1]） */
        size_t i1 = idx(cfg, 1, iy, ix);
        float norm_z = (p->z + 2.0f) / 4.0f;
        if (norm_z < 0.0f) norm_z = 0.0f;
        if (norm_z > 1.0f) norm_z = 1.0f;
        float val1 = 0.5f * (p->intensity + norm_z);
        if (val1 > feat[i1]) feat[i1] = val1;

        /* ch2: 相机前向锥能见度响应（x > 0 且 |y| <= 1.732 * x） */
        size_t i2 = idx(cfg, 2, iy, ix);
        if (p->x > 0.0f && fabsf(p->y) <= p->x * 1.7320508f) {
            float dist = sqrtf(p->x * p->x + p->y * p->y);
            float cam_vis = input->camera_visibility * (1.0f - dist / 70.0f);
            if (cam_vis < 0.0f) cam_vis = 0.0f;
            if (cam_vis > feat[i2]) feat[i2] = cam_vis;
        }

        /* ch3: 纵向运动/速度先验 */
        size_t i3 = idx(cfg, 3, iy, ix);
        float v_norm = fabsf(input->ego_speed) / (float)(cfg->v_scale > 0 ? cfg->v_scale : 30.0);
        if (v_norm > 1.0f) v_norm = 1.0f;
        if (v_norm > feat[i3]) feat[i3] = v_norm;

        /* ch4: 横向位置线索 */
        size_t i4 = idx(cfg, 4, iy, ix);
        float lat_norm = 1.0f - fabsf(p->y) / (float)cfg->range_y_half;
        if (lat_norm < 0.0f) lat_norm = 0.0f;
        if (lat_norm > feat[i4]) feat[i4] = lat_norm;
    }

    /* ch5: 局部 3x3 邻域平滑先验（ch0 的 3x3 均值，为 CNN 提供空间渐变先验） */
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            float sum = 0.0f;
            for (int dr = -1; dr <= 1; ++dr) {
                int nr = r + dr;
                if (nr < 0) nr = 0; else if (nr >= H) nr = H - 1;
                for (int dc = -1; dc <= 1; ++dc) {
                    int nc = c + dc;
                    if (nc < 0) nc = 0; else if (nc >= W) nc = W - 1;
                    sum += feat[idx(cfg, 0, nr, nc)];
                }
            }
            feat[idx(cfg, 5, r, c)] = sum / 9.0f;
        }
    }

    (void)res_x;
    (void)res_y;
    return total;
}
