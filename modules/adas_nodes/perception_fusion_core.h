#ifndef MODULES_ADAS_NODES_PERCEPTION_FUSION_CORE_H
#define MODULES_ADAS_NODES_PERCEPTION_FUSION_CORE_H

/**
 * perception_fusion_core.h — Pure 3D elevation-isolated fusion & layer helpers
 * shared between perception_fusion_node and CTest verification.
 */

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "adas_msgs_gen.h"

static constexpr double kSameElevationLayerTolM = 2.2;

static inline bool pf_same_elevation_layer(double z_a, double z_b,
                                           double tol_m = kSameElevationLayerTolM) {
    return std::fabs(z_a - z_b) <= tol_m;
}

/** Layer check with a distance-proportional grade allowance.
 *
 * A lead on a 6-9% ramp accumulates 2.2m of height difference at 24-37m,
 * which made ACC lose it long before the designed following distance. Allow
 * up to 2m extra (2.2 -> 4.2m) so same-ramp leads remain visible, while a
 * 4.5m B1/ground deck separation is still isolated at short range.
 */
static inline bool pf_same_elevation_layer_along(
    double z_a, double z_b, double along_m,
    double tol_m = kSameElevationLayerTolM,
    double grade = 0.10, double max_extra_m = 2.0) {
    const double extra = std::min(max_extra_m, grade * std::fabs(along_m));
    return std::fabs(z_a - z_b) <= tol_m + extra;
}

typedef struct PfBox2d {
    double cx, cy;
    double half_l, half_w;
    double cos_h, sin_h;
} PfBox2d;

static inline PfBox2d pf_make_box2d(double cx, double cy, double len, double wid,
                                    double heading) {
    PfBox2d b;
    b.cx = cx;
    b.cy = cy;
    b.half_l = 0.5 * (len > 0.05 ? len : 0.05);
    b.half_w = 0.5 * (wid > 0.05 ? wid : 0.05);
    b.cos_h = std::cos(heading);
    b.sin_h = std::sin(heading);
    return b;
}

static inline bool pf_obb_overlap_axis(const PfBox2d& a, const PfBox2d& b,
                                       double dx, double dy,
                                       double ux, double uy) {
    const double dist = std::fabs(dx * ux + dy * uy);
    const double ra = a.half_l * std::fabs(a.cos_h * ux + a.sin_h * uy) +
                      a.half_w * std::fabs(-a.sin_h * ux + a.cos_h * uy);
    const double rb = b.half_l * std::fabs(b.cos_h * ux + b.sin_h * uy) +
                      b.half_w * std::fabs(-b.sin_h * ux + b.cos_h * uy);
    return dist <= ra + rb;
}

/** 4-axis SAT overlap test for arbitrarily oriented 2D OBBs. */
static inline bool pf_obb_overlap_2d(const PfBox2d& a, const PfBox2d& b) {
    const double dx = b.cx - a.cx;
    const double dy = b.cy - a.cy;
    return pf_obb_overlap_axis(a, b, dx, dy,  a.cos_h,  a.sin_h) &&
           pf_obb_overlap_axis(a, b, dx, dy, -a.sin_h,  a.cos_h) &&
           pf_obb_overlap_axis(a, b, dx, dy,  b.cos_h,  b.sin_h) &&
           pf_obb_overlap_axis(a, b, dx, dy, -b.sin_h,  b.cos_h);
}

static inline bool pf_is_low_ground_curb(double obs_len, double obs_wid,
                                         double obs_height) {
    if (obs_height > 0.05) {
        return obs_height <= 0.25;
    }
    const double min_d = std::fmin(obs_len, obs_wid);
    const double max_d = std::fmax(obs_len, obs_wid);
    return (min_d <= 0.22 && max_d >= 0.80);
}

/**
 * Check whether an arbitrarily rotated 3D obstacle OBB intrudes into ego's
 * forward (or reverse when `backing`) safety corridor while enforcing vertical
 * layer isolation (`|obs_z - ego_z| <= tol_z_m`).
 */
static inline bool pf_3d_obb_safety_hit(
    double ego_x, double ego_y, double ego_z, double ego_heading,
    double obs_x, double obs_y, double obs_z,
    double obs_len, double obs_wid, double obs_height, double obs_heading,
    bool backing, double tol_z_m = kSameElevationLayerTolM) {
    if (!pf_same_elevation_layer(obs_z, ego_z, tol_z_m)) {
        return false;
    }
    const double olen = (obs_len > 0.10) ? obs_len : 0.10;
    const double owid = (obs_wid > 0.10) ? obs_wid : 0.10;
    const bool low_curb = pf_is_low_ground_curb(olen, owid, obs_height);
    const double ego_half_l = low_curb ? 1.55 : 2.30;
    const double lon_margin = low_curb ? 0.15 : 0.35;
    const double total_fwd = ego_half_l + lon_margin;
    const double dir = backing ? -1.0 : 1.0;
    const double ch = std::cos(ego_heading);
    const double sh = std::sin(ego_heading);
    const double sweep_cx = ego_x + dir * 0.5 * total_fwd * ch;
    const double sweep_cy = ego_y + dir * 0.5 * total_fwd * sh;
    const PfBox2d sweep_box = pf_make_box2d(sweep_cx, sweep_cy, total_fwd, 2.0, ego_heading);
    const PfBox2d obs_box = pf_make_box2d(obs_x, obs_y, olen, owid, obs_heading);
    return pf_obb_overlap_2d(sweep_box, obs_box);
}

/**
 * Compute generic longitudinal free space ahead of ego up to a rotated 3D OBB,
 * returning 1e9 when on another Z deck or laterally clear of `corridor_half_w_m`.
 */
static inline double pf_obb_forward_space_m(
    double ego_x, double ego_y, double ego_z, double ego_heading,
    double obs_x, double obs_y, double obs_z,
    double obs_len, double obs_wid, double obs_height, double obs_heading,
    double corridor_half_w_m = 3.0, double safety_margin_m = 0.8,
    double tol_z_m = kSameElevationLayerTolM) {
    (void)obs_height;
    if (!pf_same_elevation_layer(obs_z, ego_z, tol_z_m)) {
        return 1e9;
    }
    const double olen = (obs_len > 0.10) ? obs_len : 0.8;
    const double owid = (obs_wid > 0.10) ? obs_wid : 0.8;
    const double ch = std::cos(ego_heading), sh = std::sin(ego_heading);
    const double rel_h = obs_heading - ego_heading;
    const double cr = std::cos(rel_h), sr = std::sin(rel_h);
    const double dx = obs_x - ego_x, dy = obs_y - ego_y;
    const double c_lon =  dx * ch + dy * sh;
    const double c_lat = -dx * sh + dy * ch;
    const double half_l = 0.5 * olen, half_w = 0.5 * owid;
    const double ext_lon = half_l * std::fabs(cr) + half_w * std::fabs(sr);
    const double ext_lat = half_l * std::fabs(sr) + half_w * std::fabs(cr);
    if (std::fabs(c_lat) - ext_lat > corridor_half_w_m) {
        return 1e9;
    }
    const double front_edge = c_lon - ext_lon;
    if (c_lon + ext_lon <= 0.0) {
        return 1e9;
    }
    const double d = front_edge - safety_margin_m;
    return (d > 0.0) ? d : 0.0;
}

typedef struct { Obstacle obs; } PfObsEntry;

static inline int pf_cmp_obs_by_x(const void* a, const void* b) {
    const PfObsEntry* ea = (const PfObsEntry*)a;
    const PfObsEntry* eb = (const PfObsEntry*)b;
    if (ea->obs.x < eb->obs.x) return -1;
    if (ea->obs.x > eb->obs.x) return  1;
    return 0;
}

/**
 * Merge detections from two ObstacleList sources within `merge_dist`,
 * enforcing strict vertical isolation (`|z_a - z_b| <= 2.2m`) so overpass and
 * underpass targets at identical (x, y) never merge into one object.
 */
static inline int pf_fuse_obstacles(const ObstacleList* a, const ObstacleList* b,
                                    double merge_dist, ObstacleList* out) {
    if (!out) return 0;
    PfObsEntry pool[64];
    int n = 0;

    if (a) {
        for (uint32_t i = 0; i < a->count && n < 64; i++) {
            pool[n++].obs = a->obstacles[i];
        }
    }
    if (b) {
        for (uint32_t i = 0; i < b->count && n < 64; i++) {
            pool[n++].obs = b->obstacles[i];
        }
    }
    if (n == 0) {
        std::memset(out, 0, sizeof(*out));
        return 0;
    }

    std::qsort(pool, (size_t)n, sizeof(PfObsEntry), pf_cmp_obs_by_x);

    Obstacle selected[64];
    int sel_n = 0;
    int merges = 0;

    for (int i = 0; i < n; i++) {
        const Obstacle* cand = &pool[i].obs;
        int merged = 0;
        for (int j = 0; j < sel_n; j++) {
            if (!pf_same_elevation_layer((double)cand->z, (double)selected[j].z)) {
                continue;
            }
            const double dx = (double)cand->x - (double)selected[j].x;
            const double dy = (double)cand->y - (double)selected[j].y;
            const double dist = std::sqrt(dx * dx + dy * dy);
            if (dist < merge_dist) {
                if (cand->confidence > selected[j].confidence) {
                    selected[j].x = cand->x;
                    selected[j].y = cand->y;
                    selected[j].z = cand->z;
                    selected[j].heading = cand->heading;
                    selected[j].confidence = cand->confidence;
                    selected[j].vx = cand->vx;
                    selected[j].vy = cand->vy;
                    selected[j].type = cand->type;
                }
                if (cand->width  > selected[j].width)  selected[j].width  = cand->width;
                if (cand->length > selected[j].length) selected[j].length = cand->length;
                if (cand->height > selected[j].height) selected[j].height = cand->height;
                merges++;
                merged = 1;
                break;
            }
        }
        if (!merged && sel_n < 64) {
            selected[sel_n++] = *cand;
        }
    }

    std::memset(out, 0, sizeof(*out));
    out->count = (uint32_t)sel_n;
    for (int i = 0; i < sel_n; i++) {
        selected[i].id = (uint32_t)i;
        out->obstacles[i] = selected[i];
    }
    return merges;
}

#endif /* MODULES_ADAS_NODES_PERCEPTION_FUSION_CORE_H */
