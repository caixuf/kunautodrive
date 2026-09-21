/**
 * traj_safety.c — 轨迹扫掠碰撞检查实现（详见 traj_safety.h）
 */

#include "traj_safety.h"

#include <math.h>

/* 线段 (x0,y0)→(x1,y1) 与 AABB [cx±hx, cy±hy] 是否相交（slab 法）。 */
static int seg_aabb_hit(double x0, double y0, double x1, double y1,
                        double cx, double cy, double hx, double hy) {
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    double tmin = 0.0, tmax = 1.0;

    if (fabs(dx) < 1e-12) {
        if (x0 < cx - hx || x0 > cx + hx) return 0;   /* 平行且在 slab 外 */
    } else {
        double t1 = (cx - hx - x0) / dx;
        double t2 = (cx + hx - x0) / dx;
        if (t1 > t2) { double t = t1; t1 = t2; t2 = t; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return 0;
    }

    if (fabs(dy) < 1e-12) {
        if (y0 < cy - hy || y0 > cy + hy) return 0;
    } else {
        double t1 = (cy - hy - y0) / dy;
        double t2 = (cy + hy - y0) / dy;
        if (t1 > t2) { double t = t1; t1 = t2; t2 = t; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return 0;
    }

    return 1;
}

static int obs_valid(const double* ox, const double* oy,
                     const double* ow, const double* ol, int i) {
    const double hx = ol[i] * 0.5;
    const double hy = ow[i] * 0.5;
    if (!isfinite(ox[i]) || !isfinite(oy[i]) || !isfinite(hx) || !isfinite(hy)) return 0;
    if (hx <= 0.0 || hy <= 0.0) return 0;   /* 退化盒忽略 */
    return 1;
}

TrajSweptResult traj_swept_check(const float* xs, const float* ys, int n_pts,
                                 const double* ox, const double* oy,
                                 const double* ow, const double* ol, int n_obs) {
    TrajSweptResult r = {0, -1, -1};
    if (!xs || !ys || n_pts < 2 || !ox || !oy || !ow || !ol || n_obs <= 0) return r;

    for (int s = 0; s + 1 < n_pts; s++) {
        for (int i = 0; i < n_obs; i++) {
            if (!obs_valid(ox, oy, ow, ol, i)) continue;
            if (seg_aabb_hit((double)xs[s], (double)ys[s],
                             (double)xs[s + 1], (double)ys[s + 1],
                             ox[i], oy[i], ol[i] * 0.5, ow[i] * 0.5)) {
                if (r.hits == 0) { r.seg_idx = s; r.obs_idx = i; }
                r.hits++;
            }
        }
    }
    return r;
}

int traj_point_test_hits(const float* xs, const float* ys, int n_pts,
                         const double* ox, const double* oy,
                         const double* ow, const double* ol, int n_obs) {
    int hits = 0;
    if (!xs || !ys || n_pts < 1 || !ox || !oy || !ow || !ol || n_obs <= 0) return 0;

    for (int s = 0; s < n_pts; s++) {
        for (int i = 0; i < n_obs; i++) {
            if (!obs_valid(ox, oy, ow, ol, i)) continue;
            const double hx = ol[i] * 0.5;
            const double hy = ow[i] * 0.5;
            if (fabs((double)xs[s] - ox[i]) <= hx &&
                fabs((double)ys[s] - oy[i]) <= hy) {
                hits++;
            }
        }
    }
    return hits;
}
