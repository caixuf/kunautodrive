/**
 * prediction_node.c — 地图与交互感知的多模态运动预测节点插件 (Milestone M2)
 *
 * 订阅:
 *   perception/tracked_objects  — 跨帧跟踪目标 (cJSON TrackedObjectList)
 *   fusion/localization         — 自车定位 (cJSON ego pose)
 *   road/geometry               — 道路弯道几何参数 (cJSON)
 *   road/ref_path               — 道路中心参考线折线 (cJSON)
 *
 * 输出:
 *   prediction/tracks           — 多模态轨迹预测列表 (cJSON)
 *
 * 预测模态:
 *   1. Lane Keeping (LK)      — 沿道路曲率与当前车道偏移行驶
 *   2. Cut-in (Left / Right)  — 横向切入自车中心线，评估 t_enter
 *   3. Yield / Decel          — 减速让行模态
 *
 * 参考线三级优雅回退:
 *   Level 1: road/ref_path 样点折线
 *   Level 2: road/geometry 弯道平滑曲线
 *   Level 3: 车体系切线推演
 */

#include "node_plugin.h"
#include "logger.h"
#include "clock_service.h"
#include <cjson/cJSON.h>

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* ── Constants ────────────────────────────────────────────────── */
#define PRED_TOPIC_IN_TRACKS    "perception/tracked_objects"
#define PRED_TOPIC_IN_LOC       "fusion/localization"
#define PRED_TOPIC_IN_GEOM      "road/geometry"
#define PRED_TOPIC_IN_REF_PATH  "road/ref_path"
#define PRED_TOPIC_OUT          "prediction/tracks"
#define PRED_DEFAULT_HZ         10.0f

#define PRED_N_WAYPOINTS        10u     /**< 每个模态预测 10 个未来样点 */
#define PRED_WAYPOINT_DT_S      0.5f    /**< 点间距 0.5s */
#define PRED_HORIZON_S          5.0f    /**< 总时域 5.0s */
#define PRED_MAX_KEY_OBJECTS    12u     /**< 最多输出关键动态目标数，控制 JSON 体积 < 16KB */
#define PRED_MAX_REF_PTS        128u    /**< 参考线缓存点上限 */

/* ── 参考线样点结构 ─────────────────────────────────────────── */
typedef struct {
    double x;
    double y;
    double h;
    double kappa;
    double s;
} RefPoint;

/* ── Module state ─────────────────────────────────────────────── */
static struct {
    Transport*        transport;
    DiscoveryManager* discovery;
    Scheduler*        scheduler;

    TaskBase    taskbase;
    float       rate_hz;
    uint32_t    frame_id;

    /* ── Latch: tracked objects ── */
    char*         tracks_json;
    volatile int  has_tracks;

    /* ── Latch: ego state (world frame) ── */
    double ego_x, ego_y, ego_v, ego_heading;
    volatile int has_ego;

    /* ── Latch: road/geometry ── */
    double curve_start_x;
    double curve_length_m;
    double curve_offset_m;
    double lane_width;
    volatile int has_geom;

    /* ── Latch: road/ref_path ── */
    RefPoint ref_pts[PRED_MAX_REF_PTS];
    int      n_ref_pts;
    volatile int has_ref_path;

    pthread_mutex_t mutex;
} g;

/* ── Callbacks ────────────────────────────────────────────────── */
static void on_tracked_objects(const Message* msg, void* user_data) {
    (void)user_data;
    if (!msg || msg->data_size == 0) return;

    char* copy = strdup((const char*)msg->data);
    if (!copy) return;

    pthread_mutex_lock(&g.mutex);
    if (g.tracks_json) free(g.tracks_json);
    g.tracks_json = copy;
    g.has_tracks  = 1;
    pthread_mutex_unlock(&g.mutex);
}

static void on_localization(const Message* msg, void* user_data) {
    (void)user_data;
    if (!msg || msg->data_size == 0) return;

    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;

    cJSON* j;
    double x = 0, y = 0, v = 0, heading = 0;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "x")) && cJSON_IsNumber(j))
        x = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "y")) && cJSON_IsNumber(j))
        y = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "v")) && cJSON_IsNumber(j))
        v = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "heading")) && cJSON_IsNumber(j))
        heading = j->valuedouble;
    cJSON_Delete(root);

    pthread_mutex_lock(&g.mutex);
    g.ego_x       = x;
    g.ego_y       = y;
    g.ego_v       = v;
    g.ego_heading = heading;
    g.has_ego     = 1;
    pthread_mutex_unlock(&g.mutex);
}

static void on_road_geometry(const Message* msg, void* user_data) {
    (void)user_data;
    if (!msg || msg->data_size == 0) return;

    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;

    cJSON* j;
    double csx = 0, clen = 0, coff = 0, lw = 3.75;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "curve_start_x")) && cJSON_IsNumber(j))
        csx = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "curve_length_m")) && cJSON_IsNumber(j))
        clen = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "curve_offset_m")) && cJSON_IsNumber(j))
        coff = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "lane_width")) && cJSON_IsNumber(j) && j->valuedouble > 1.0)
        lw = j->valuedouble;
    cJSON_Delete(root);

    pthread_mutex_lock(&g.mutex);
    g.curve_start_x  = csx;
    g.curve_length_m = clen;
    g.curve_offset_m = coff;
    g.lane_width     = lw;
    g.has_geom       = 1;
    pthread_mutex_unlock(&g.mutex);
}

static void on_ref_path(const Message* msg, void* user_data) {
    (void)user_data;
    if (!msg || msg->data_size == 0) return;

    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;

    cJSON* pts_arr = cJSON_GetObjectItemCaseSensitive(root, "points");
    if (cJSON_IsArray(pts_arr)) {
        int n = cJSON_GetArraySize(pts_arr);
        if (n > (int)PRED_MAX_REF_PTS) n = (int)PRED_MAX_REF_PTS;

        pthread_mutex_lock(&g.mutex);
        g.n_ref_pts = 0;
        double accum_s = 0.0;
        double prev_x = 0.0, prev_y = 0.0;

        for (int i = 0; i < n; i++) {
            cJSON* pt = cJSON_GetArrayItem(pts_arr, i);
            if (!pt) continue;
            cJSON* jx = cJSON_GetObjectItemCaseSensitive(pt, "x");
            cJSON* jy = cJSON_GetObjectItemCaseSensitive(pt, "y");
            cJSON* jh = cJSON_GetObjectItemCaseSensitive(pt, "h");
            cJSON* jk = cJSON_GetObjectItemCaseSensitive(pt, "kappa");
            cJSON* js = cJSON_GetObjectItemCaseSensitive(pt, "rs");
            if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) continue;

            double px = jx->valuedouble;
            double py = jy->valuedouble;
            double ph = cJSON_IsNumber(jh) ? jh->valuedouble : 0.0;
            double pk = cJSON_IsNumber(jk) ? jk->valuedouble : 0.0;

            if (i == 0) {
                accum_s = cJSON_IsNumber(js) ? js->valuedouble : 0.0;
            } else {
                double ds = hypot(px - prev_x, py - prev_y);
                accum_s += ds;
            }
            prev_x = px;
            prev_y = py;

            g.ref_pts[g.n_ref_pts].x = px;
            g.ref_pts[g.n_ref_pts].y = py;
            g.ref_pts[g.n_ref_pts].h = ph;
            g.ref_pts[g.n_ref_pts].kappa = pk;
            g.ref_pts[g.n_ref_pts].s = accum_s;
            g.n_ref_pts++;
        }
        g.has_ref_path = (g.n_ref_pts >= 2);
        pthread_mutex_unlock(&g.mutex);
    }
    cJSON_Delete(root);
}

/* ── 参考线几何与 Frenet 投影查询 (三级回退) ─────────────────── */
static void query_ref_line(double s, double* out_rx, double* out_ry, double* out_rh) {
    /* Level 1: road/ref_path 折线样点 */
    if (g.has_ref_path && g.n_ref_pts >= 2) {
        if (s <= g.ref_pts[0].s) {
            *out_rx = g.ref_pts[0].x;
            *out_ry = g.ref_pts[0].y;
            *out_rh = g.ref_pts[0].h;
            return;
        }
        if (s >= g.ref_pts[g.n_ref_pts - 1].s) {
            int last = g.n_ref_pts - 1;
            *out_rx = g.ref_pts[last].x;
            *out_ry = g.ref_pts[last].y;
            *out_rh = g.ref_pts[last].h;
            return;
        }
        for (int i = 0; i < g.n_ref_pts - 1; i++) {
            if (s >= g.ref_pts[i].s && s <= g.ref_pts[i + 1].s) {
                double ds = g.ref_pts[i + 1].s - g.ref_pts[i].s;
                double u = (ds > 1e-6) ? (s - g.ref_pts[i].s) / ds : 0.0;
                *out_rx = g.ref_pts[i].x + u * (g.ref_pts[i + 1].x - g.ref_pts[i].x);
                *out_ry = g.ref_pts[i].y + u * (g.ref_pts[i + 1].y - g.ref_pts[i].y);
                *out_rh = g.ref_pts[i].h;
                return;
            }
        }
    }

    /* Level 2: road/geometry 平滑弯道曲线 */
    if (g.has_geom && g.curve_length_m > 1.0) {
        double csx = g.curve_start_x;
        double clen = g.curve_length_m;
        double coff = g.curve_offset_m;
        if (s <= csx) {
            *out_rx = s;
            *out_ry = 0.0;
            *out_rh = 0.0;
        } else if (s >= csx + clen) {
            *out_rx = s;
            *out_ry = coff;
            *out_rh = 0.0;
        } else {
            double u = (s - csx) / clen;
            *out_rx = s;
            *out_ry = coff * (3.0 * u * u - 2.0 * u * u * u);
            double dy_ds = (coff * (6.0 * u - 6.0 * u * u)) / clen;
            *out_rh = atan2(dy_ds, 1.0);
        }
        return;
    }

    /* Level 3: 自车车体切线基线 */
    *out_rx = g.ego_x + s * cos(g.ego_heading);
    *out_ry = g.ego_y + s * sin(g.ego_heading);
    *out_rh = g.ego_heading;
}

static void cartesian_to_frenet(double wx, double wy, double* out_s, double* out_d, double* out_rh) {
    if (g.has_ref_path && g.n_ref_pts >= 2) {
        double min_dist_sq = 1e12;
        double best_s = 0.0;
        double best_rx = 0.0, best_ry = 0.0, best_rh = 0.0;

        for (int i = 0; i < g.n_ref_pts - 1; i++) {
            double x1 = g.ref_pts[i].x, y1 = g.ref_pts[i].y;
            double x2 = g.ref_pts[i + 1].x, y2 = g.ref_pts[i + 1].y;
            double seg_dx = x2 - x1, seg_dy = y2 - y1;
            double seg_len2 = seg_dx * seg_dx + seg_dy * seg_dy;
            double u = (seg_len2 > 1e-6) ? ((wx - x1) * seg_dx + (wy - y1) * seg_dy) / seg_len2 : 0.0;
            if (u < 0.0) u = 0.0;
            if (u > 1.0) u = 1.0;
            double px = x1 + u * seg_dx;
            double py = y1 + u * seg_dy;
            double d2 = (wx - px) * (wx - px) + (wy - py) * (wy - py);
            if (d2 < min_dist_sq) {
                min_dist_sq = d2;
                best_rx = px;
                best_ry = py;
                best_rh = g.ref_pts[i].h;
                best_s = g.ref_pts[i].s + u * hypot(seg_dx, seg_dy);
            }
        }
        *out_s = best_s;
        *out_rh = best_rh;
        double dx = wx - best_rx;
        double dy = wy - best_ry;
        *out_d = -dx * sin(best_rh) + dy * cos(best_rh);
        return;
    }

    if (g.has_geom && g.curve_length_m > 1.0) {
        double s = wx;
        double rx, ry, rh;
        query_ref_line(s, &rx, &ry, &rh);
        *out_s = s;
        *out_rh = rh;
        double dx = wx - rx;
        double dy = wy - ry;
        *out_d = -dx * sin(rh) + dy * cos(rh);
        return;
    }

    /* Level 3: 切线投影 */
    double dx = wx - g.ego_x;
    double dy = wy - g.ego_y;
    *out_s = dx * cos(g.ego_heading) + dy * sin(g.ego_heading);
    *out_d = -dx * sin(g.ego_heading) + dy * cos(g.ego_heading);
    *out_rh = g.ego_heading;
}

static void frenet_to_cartesian(double s, double d, double* out_wx, double* out_wy) {
    double rx, ry, rh;
    query_ref_line(s, &rx, &ry, &rh);
    *out_wx = rx - d * sin(rh);
    *out_wy = ry + d * cos(rh);
}

/* ── 动态关键目标排序结构 ─────────────────────────────────────── */
typedef struct {
    int    object_id;
    double bx, by, bvx, bvy;
    double wx, wy, wvx, wvy;
    double track_age;
    double confidence;
    double dist;
} KeyObject;

static int cmp_key_object(const void* a, const void* b) {
    const KeyObject* ka = (const KeyObject*)a;
    const KeyObject* kb = (const KeyObject*)b;
    if (ka->dist < kb->dist) return -1;
    if (ka->dist > kb->dist) return 1;
    return 0;
}

/* ── 协程执行主循环 ─────────────────────────────────────────── */
static int prediction_execute(TaskBase* task) {
    pthread_setname_np(pthread_self(), "prediction");

    const long period_us = (long)(1000000.0 / g.rate_hz);

    while (!task->should_stop) {
        usleep((unsigned long)period_us);
        if (task->should_stop) break;

        /* ── 快照输入数据 ── */
        pthread_mutex_lock(&g.mutex);
        const int has_data = g.has_tracks;
        char* tracks_copy = (has_data && g.tracks_json) ? strdup(g.tracks_json) : NULL;
        const double ego_x_snap = g.ego_x;
        const double ego_y_snap = g.ego_y;
        const double ego_v_snap = g.ego_v;
        const double ego_h_snap = g.ego_heading;
        const int has_ego_snap = g.has_ego;
        pthread_mutex_unlock(&g.mutex);

        if (!has_data || !tracks_copy) {
            if (tracks_copy) free(tracks_copy);
            continue;
        }

        cJSON* root = cJSON_Parse(tracks_copy);
        free(tracks_copy);
        if (!root) continue;

        cJSON* j_objects = cJSON_GetObjectItemCaseSensitive(root, "objects");
        if (!cJSON_IsArray(j_objects)) {
            cJSON_Delete(root);
            continue;
        }

        const int n_objects = cJSON_GetArraySize(j_objects);
        KeyObject key_objs[128];
        int n_key = 0;

        const double ch = cos(ego_h_snap);
        const double sh = sin(ego_h_snap);

        /* 过滤静态目标并按距离挑选关键目标 */
        for (int i = 0; i < n_objects && n_key < 128; i++) {
            cJSON* obj = cJSON_GetArrayItem(j_objects, i);
            if (!obj) continue;

            cJSON* j = cJSON_GetObjectItemCaseSensitive(obj, "is_static");
            if (cJSON_IsBool(j) && cJSON_IsTrue(j)) continue;

            KeyObject ko;
            memset(&ko, 0, sizeof(ko));

            j = cJSON_GetObjectItemCaseSensitive(obj, "id");
            ko.object_id = (j && cJSON_IsNumber(j)) ? (int)j->valuedouble : (i + 1);

            j = cJSON_GetObjectItemCaseSensitive(obj, "x");
            ko.bx = (j && cJSON_IsNumber(j)) ? j->valuedouble : 0.0;

            j = cJSON_GetObjectItemCaseSensitive(obj, "y");
            ko.by = (j && cJSON_IsNumber(j)) ? j->valuedouble : 0.0;

            j = cJSON_GetObjectItemCaseSensitive(obj, "vx");
            ko.bvx = (j && cJSON_IsNumber(j)) ? j->valuedouble : 0.0;

            j = cJSON_GetObjectItemCaseSensitive(obj, "vy");
            ko.bvy = (j && cJSON_IsNumber(j)) ? j->valuedouble : 0.0;

            j = cJSON_GetObjectItemCaseSensitive(obj, "track_age");
            ko.track_age = (j && cJSON_IsNumber(j)) ? j->valuedouble : 1.0;

            j = cJSON_GetObjectItemCaseSensitive(obj, "confidence");
            ko.confidence = (j && cJSON_IsNumber(j)) ? j->valuedouble : 0.8;

            /* 车体系 (bx, by) -> 世界系 (wx, wy) */
            if (has_ego_snap) {
                ko.wx  = ego_x_snap + ko.bx * ch - ko.by * sh;
                ko.wy  = ego_y_snap + ko.bx * sh + ko.by * ch;
                ko.wvx = ko.bvx * ch - ko.bvy * sh;
                ko.wvy = ko.bvx * sh + ko.bvy * ch;
            } else {
                ko.wx  = ko.bx;
                ko.wy  = ko.by;
                ko.wvx = ko.bvx;
                ko.wvy = ko.bvy;
            }

            ko.dist = hypot(ko.bx, ko.by);
            key_objs[n_key++] = ko;
        }

        /* 按与自车距离排序，截取前 PRED_MAX_KEY_OBJECTS 个目标控制 JSON 大小 */
        if (n_key > 1) {
            qsort(key_objs, (size_t)n_key, sizeof(KeyObject), cmp_key_object);
        }
        if (n_key > (int)PRED_MAX_KEY_OBJECTS) {
            n_key = (int)PRED_MAX_KEY_OBJECTS;
        }

        /* ── 构建输出 cJSON ── */
        cJSON* out_root = cJSON_CreateObject();
        cJSON* j_tracks = cJSON_AddArrayToObject(out_root, "tracks");

        cJSON_AddNumberToObject(out_root, "frame_id",     (double)g.frame_id);
        cJSON_AddNumberToObject(out_root, "timestamp_us", (double)clock_now_us());

        double ego_s = 0.0, ego_d = 0.0, ego_rh = 0.0;
        cartesian_to_frenet(ego_x_snap, ego_y_snap, &ego_s, &ego_d, &ego_rh);

        for (int i = 0; i < n_key; i++) {
            const KeyObject* ko = &key_objs[i];

            double s = 0.0, d = 0.0, ref_h = 0.0;
            cartesian_to_frenet(ko->wx, ko->wy, &s, &d, &ref_h);

            /* Frenet 纵向与横向速度 */
            double vs = ko->wvx * cos(ref_h) + ko->wvy * sin(ref_h);
            double vd = -ko->wvx * sin(ref_h) + ko->wvy * cos(ref_h);

            double delta_s = s - ego_s;
            double delta_v = vs - ego_v_snap;

            /* 动态贝叶斯多模态概率评估 */
            const int is_left = (d > 0.0);
            const double vd_towards_ego = is_left ? -vd : vd;

            double p_cutin = 0.05;
            if (fabs(d) >= 1.2) {
                double z = 4.0 * (vd_towards_ego - 0.25);
                p_cutin = 1.0 / (1.0 + exp(-fmax(-8.0, fmin(8.0, z))));
            }
            if (p_cutin < 0.05) p_cutin = 0.05;
            if (p_cutin > 0.85) p_cutin = 0.85;

            double p_yield = 0.05;
            if (delta_s > 0.0 && delta_s < 35.0 && delta_v < -0.5) {
                p_yield = 0.25 * (1.0 - delta_s / 35.0);
            }
            if (p_yield < 0.05) p_yield = 0.05;
            if (p_yield > 0.30) p_yield = 0.30;

            double p_lk = 1.0 - p_cutin - p_yield;
            if (p_lk < 0.05) p_lk = 0.05;
            double p_sum = p_lk + p_cutin + p_yield;
            p_lk /= p_sum;
            p_cutin /= p_sum;
            p_yield /= p_sum;

            cJSON* track_entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(track_entry, "object_id",  (double)ko->object_id);
            cJSON_AddNumberToObject(track_entry, "confidence", ko->confidence);
            cJSON_AddNumberToObject(track_entry, "horizon_s",  (double)PRED_HORIZON_S);
            cJSON_AddNumberToObject(track_entry, "wx",         ko->wx);
            cJSON_AddNumberToObject(track_entry, "wy",         ko->wy);

            cJSON* j_trajs = cJSON_AddArrayToObject(track_entry, "trajectories");

            /* 模态 1: Lane Keeping (沿道路曲率与当前偏移保持) */
            {
                cJSON* traj = cJSON_CreateObject();
                cJSON_AddNumberToObject(traj, "prob", p_lk);
                cJSON_AddStringToObject(traj, "intent", "lane_keep");
                cJSON_AddNumberToObject(traj, "t_enter", 0.0);

                cJSON* wps = cJSON_CreateArray();
                cJSON* world_wps = cJSON_CreateArray();
                for (unsigned int step = 1; step <= PRED_N_WAYPOINTS; step++) {
                    double t = (double)step * (double)PRED_WAYPOINT_DT_S;
                    double st = s + vs * t;
                    double dt_lat = d;
                    double wxt, wyt;
                    frenet_to_cartesian(st, dt_lat, &wxt, &wyt);

                    /* 转回车体系 (bx, by) 保持兼容 */
                    double dx = wxt - ego_x_snap;
                    double dy = wyt - ego_y_snap;
                    double bxt = dx * ch + dy * sh;
                    double byt = -dx * sh + dy * ch;

                    cJSON* wp = cJSON_CreateArray();
                    cJSON_AddItemToArray(wp, cJSON_CreateNumber(bxt));
                    cJSON_AddItemToArray(wp, cJSON_CreateNumber(byt));
                    cJSON_AddItemToArray(wp, cJSON_CreateNumber(vs));
                    cJSON_AddItemToArray(wps, wp);

                    cJSON* wwp = cJSON_CreateArray();
                    cJSON_AddItemToArray(wwp, cJSON_CreateNumber(wxt));
                    cJSON_AddItemToArray(wwp, cJSON_CreateNumber(wyt));
                    cJSON_AddItemToArray(wwp, cJSON_CreateNumber(vs));
                    cJSON_AddItemToArray(world_wps, wwp);
                }
                cJSON_AddItemToObject(traj, "waypoints", wps);
                cJSON_AddItemToObject(traj, "world_waypoints", world_wps);
                cJSON_AddItemToArray(j_trajs, traj);
            }

            /* 模态 2: Cut-in (切入自车中心线 d -> 0) */
            {
                cJSON* traj = cJSON_CreateObject();
                cJSON_AddNumberToObject(traj, "prob", p_cutin);
                cJSON_AddStringToObject(traj, "intent", is_left ? "cut_in_left" : "cut_in_right");

                double t_enter = 2.0; /* 默认 2.0s */
                cJSON* wps = cJSON_CreateArray();
                cJSON* world_wps = cJSON_CreateArray();
                for (unsigned int step = 1; step <= PRED_N_WAYPOINTS; step++) {
                    double t = (double)step * (double)PRED_WAYPOINT_DT_S;
                    double st = s + vs * t;
                    /* 平滑五次多项式切入到中心线 */
                    double tau = fmin(1.0, t / 2.5);
                    double dt_lat = d * (1.0 - (10.0 * tau * tau * tau - 15.0 * tau * tau * tau * tau + 6.0 * tau * tau * tau * tau * tau));
                    if (fabs(dt_lat) <= 1.0 && t_enter > 10.0) {
                        t_enter = t;
                    }

                    double wxt, wyt;
                    frenet_to_cartesian(st, dt_lat, &wxt, &wyt);
                    double dx = wxt - ego_x_snap;
                    double dy = wyt - ego_y_snap;
                    double bxt = dx * ch + dy * sh;
                    double byt = -dx * sh + dy * ch;

                    cJSON* wp = cJSON_CreateArray();
                    cJSON_AddItemToArray(wp, cJSON_CreateNumber(bxt));
                    cJSON_AddItemToArray(wp, cJSON_CreateNumber(byt));
                    cJSON_AddItemToArray(wp, cJSON_CreateNumber(vs));
                    cJSON_AddItemToArray(wps, wp);

                    cJSON* wwp = cJSON_CreateArray();
                    cJSON_AddItemToArray(wwp, cJSON_CreateNumber(wxt));
                    cJSON_AddItemToArray(wwp, cJSON_CreateNumber(wyt));
                    cJSON_AddItemToArray(wwp, cJSON_CreateNumber(vs));
                    cJSON_AddItemToArray(world_wps, wwp);
                }
                cJSON_AddNumberToObject(traj, "t_enter", t_enter);
                cJSON_AddItemToObject(traj, "waypoints", wps);
                cJSON_AddItemToObject(traj, "world_waypoints", world_wps);
                cJSON_AddItemToArray(j_trajs, traj);
            }

            /* 模态 3: Yield / Decel (让行减速) */
            {
                cJSON* traj = cJSON_CreateObject();
                cJSON_AddNumberToObject(traj, "prob", p_yield);
                cJSON_AddStringToObject(traj, "intent", "yield_decel");
                cJSON_AddNumberToObject(traj, "t_enter", 0.0);

                cJSON* wps = cJSON_CreateArray();
                cJSON* world_wps = cJSON_CreateArray();
                for (unsigned int step = 1; step <= PRED_N_WAYPOINTS; step++) {
                    double t = (double)step * (double)PRED_WAYPOINT_DT_S;
                    double vt = fmax(0.0, vs - 1.2 * t);
                    double st = s + vs * t - 0.5 * 1.2 * t * t;
                    double dt_lat = d;

                    double wxt, wyt;
                    frenet_to_cartesian(st, dt_lat, &wxt, &wyt);
                    double dx = wxt - ego_x_snap;
                    double dy = wyt - ego_y_snap;
                    double bxt = dx * ch + dy * sh;
                    double byt = -dx * sh + dy * ch;

                    cJSON* wp = cJSON_CreateArray();
                    cJSON_AddItemToArray(wp, cJSON_CreateNumber(bxt));
                    cJSON_AddItemToArray(wp, cJSON_CreateNumber(byt));
                    cJSON_AddItemToArray(wp, cJSON_CreateNumber(vt));
                    cJSON_AddItemToArray(wps, wp);

                    cJSON* wwp = cJSON_CreateArray();
                    cJSON_AddItemToArray(wwp, cJSON_CreateNumber(wxt));
                    cJSON_AddItemToArray(wwp, cJSON_CreateNumber(wyt));
                    cJSON_AddItemToArray(wwp, cJSON_CreateNumber(vt));
                    cJSON_AddItemToArray(world_wps, wwp);
                }
                cJSON_AddItemToObject(traj, "waypoints", wps);
                cJSON_AddItemToObject(traj, "world_waypoints", world_wps);
                cJSON_AddItemToArray(j_trajs, traj);
            }

            cJSON_AddItemToArray(j_tracks, track_entry);
        }

        /* ── 发布预测结果 ── */
        char* json_str = cJSON_PrintUnformatted(out_root);
        if (json_str) {
            transport_publish(g.transport, PRED_TOPIC_OUT,
                              (const uint8_t*)json_str, (uint32_t)strlen(json_str) + 1);
            free(json_str);
        }

        cJSON_Delete(out_root);
        cJSON_Delete(root);
        g.frame_id++;
    }

    LOG_INFO("prediction", "stopped (%u frames)", g.frame_id);
    return 0;
}

static const TaskInterface prediction_vtable = {
    .execute = prediction_execute,
};

/* ── NodePlugin lifecycle ────────────────────────────────────── */
static const char* s_inputs[]  = {
    PRED_TOPIC_IN_TRACKS,
    PRED_TOPIC_IN_LOC,
    PRED_TOPIC_IN_GEOM,
    PRED_TOPIC_IN_REF_PATH,
    NULL
};
static const char* s_outputs[] = { PRED_TOPIC_OUT, NULL };

static NodePlugin s_plugin;

static int prediction_init(MessageBus* bus, Transport* transport,
                           DiscoveryManager* discovery, Scheduler* scheduler,
                           const char* params_json) {
    (void)bus;

    memset(&g, 0, sizeof(g));
    g.transport   = transport;
    g.discovery   = discovery;
    g.scheduler   = scheduler;

    g.rate_hz = PRED_DEFAULT_HZ;

    if (params_json) {
        cJSON* p = cJSON_Parse(params_json);
        if (p) {
            cJSON* j;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "rate_hz")) && cJSON_IsNumber(j))
                g.rate_hz = (float)j->valuedouble;
            cJSON_Delete(p);
        }
    }

    /* ── Subscriptions ── */
    transport_subscribe(transport, PRED_TOPIC_IN_TRACKS,   on_tracked_objects, NULL);
    transport_subscribe(transport, PRED_TOPIC_IN_LOC,      on_localization,    NULL);
    transport_subscribe(transport, PRED_TOPIC_IN_GEOM,     on_road_geometry,   NULL);
    transport_subscribe(transport, PRED_TOPIC_IN_REF_PATH, on_ref_path,        NULL);
    transport_advertise(transport, PRED_TOPIC_OUT, 0x3A7B1C2Du);

    /* ── Discovery ── */
    discovery_advertise(discovery, PRED_TOPIC_IN_TRACKS,   0xDA7A7A11u, CAP_SUBSCRIBER, 0);
    discovery_advertise(discovery, PRED_TOPIC_IN_LOC,      0xF0ED10C0u, CAP_SUBSCRIBER, 0);
    discovery_advertise(discovery, PRED_TOPIC_IN_GEOM,     0u,          CAP_SUBSCRIBER, 0);
    discovery_advertise(discovery, PRED_TOPIC_IN_REF_PATH, 0u,          CAP_SUBSCRIBER, 0);
    discovery_advertise(discovery, PRED_TOPIC_OUT,         0xB8A3C4D5u, CAP_PUBLISHER,  g.rate_hz);

    pthread_mutex_init(&g.mutex, NULL);

    TaskConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.name, sizeof(cfg.name), "prediction");
    cfg.priority         = TASK_PRIORITY_NORMAL;
    cfg.max_frequency_hz = (double)g.rate_hz;
    cfg.enable_stats     = true;
    if (task_base_init(&g.taskbase, &prediction_vtable, &cfg) != 0) {
        LOG_WARN("prediction", "task_base_init failed");
        return -1;
    }

    LOG_INFO("prediction", "initialized (%.1f Hz, map-aware multi-modal)", g.rate_hz);
    return 0;
}

static int prediction_start(void) {
    int rc = node_start_managed(&s_plugin, g.scheduler);
    if (rc != 0) {
        LOG_WARN("prediction", "node_start_managed failed: %d", rc);
        return rc;
    }
    LOG_INFO("prediction", "started (managed)");
    node_announce_self(g.transport, &s_plugin);
    return 0;
}

static void prediction_stop(void) {
    task_stop(&g.taskbase);
}

static void prediction_cleanup(void) {
    task_stop(&g.taskbase);
    task_base_destroy(&g.taskbase);
    pthread_mutex_destroy(&g.mutex);
    if (g.tracks_json) {
        free(g.tracks_json);
        g.tracks_json = NULL;
    }
    LOG_INFO("prediction", "cleanup done");
}

static int prediction_health(void) {
    return 0;
}

static NodePlugin s_plugin = {
    .api_version   = NODE_PLUGIN_API_VERSION,
    .name          = "prediction",
    .version       = "1.0.0",
    .description   = "Map-aware multi-modal motion prediction (LK/Cut-in/Yield)",
    .input_topics  = s_inputs,
    .output_topics = s_outputs,
    .init          = prediction_init,
    .start         = prediction_start,
    .stop          = prediction_stop,
    .cleanup       = prediction_cleanup,
    .health        = prediction_health,
    .taskbase      = &g.taskbase,
};

NodePlugin* node_get_plugin(void) { return &s_plugin; }
