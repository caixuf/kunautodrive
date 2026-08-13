/**
 * scenario_router.c — Lane-level graph routing engine.
 *
 * Implements A* search over a directed graph of lane segments.
 * Supports successor, left-neighbor, and right-neighbor edges.
 *
 * ALGORITHM_REFACTOR_PLAN §7: 路由层 → 车道级图搜索
 */

#include "scenario_router.h"
#include <cjson/cJSON.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ── A* internal helpers ────────────────────────────────────── */

/* Priority queue entry for A* open set */
typedef struct {
    int lane_id;
    double g;   /* cost from start */
    double f;   /* g + heuristic */
} AstarNode;

/* Simple array-based binary heap priority queue (max ROUTER_MAX_LANES) */
static int pq_push(AstarNode* heap, int* size, int lane_id, double g, double f) {
    if (*size >= ROUTER_MAX_LANES) return -1;
    int i = (*size)++;
    heap[i].lane_id = lane_id;
    heap[i].g = g;
    heap[i].f = f;
    /* bubble up */
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap[p].f <= heap[i].f) break;
        AstarNode tmp = heap[p];
        heap[p] = heap[i];
        heap[i] = tmp;
        i = p;
    }
    return 0;
}

static int pq_pop(AstarNode* heap, int* size, int* out_id, double* out_g) {
    if (*size == 0) return -1;
    *out_id = heap[0].lane_id;
    *out_g = heap[0].g;
    /* move last to root */
    (*size)--;
    if (*size > 0) {
        heap[0] = heap[*size];
        int i = 0;
        while (1) {
            int smallest = i;
            int left = 2 * i + 1;
            int right = 2 * i + 2;
            if (left < *size && heap[left].f < heap[smallest].f)
                smallest = left;
            if (right < *size && heap[right].f < heap[smallest].f)
                smallest = right;
            if (smallest == i) break;
            AstarNode tmp = heap[i];
            heap[i] = heap[smallest];
            heap[smallest] = tmp;
            i = smallest;
        }
    }
    return 0;
}

/* Closed set as bitmask: ROUTER_MAX_LANES lanes = ceil(ROUTER_MAX_LANES/64) × uint64_t */
#define CLOSED_SIZE ((ROUTER_MAX_LANES + 63) / 64)
static inline void closed_set_init(uint64_t* cs) {
    memset(cs, 0, CLOSED_SIZE * sizeof(uint64_t));
}
static inline void closed_set_add(uint64_t* cs, int id) {
    if (id >= 0 && id < ROUTER_MAX_LANES) cs[id / 64] |= (1ULL << (id % 64));
}
static inline int closed_set_has(uint64_t* cs, int id) {
    if (id < 0 || id >= ROUTER_MAX_LANES) return 0;
    return (cs[id / 64] >> (id % 64)) & 1ULL;
}

/* ── Euclidean heuristic: distance from lane center to goal lane center ── */
static double heuristic(const RouterGraph* g, int from_id, int to_id) {
    double cx_from = 0, cy_from = 0, cx_to = 0, cy_to = 0;
    int found_from = 0, found_to = 0;
    for (int i = 0; i < g->lane_count; i++) {
        if (g->lanes[i].id == from_id) {
            cx_from = (g->lanes[i].start_x + g->lanes[i].end_x) * 0.5;
            cy_from = (g->lanes[i].start_y + g->lanes[i].end_y) * 0.5;
            found_from = 1;
        }
        if (g->lanes[i].id == to_id) {
            cx_to = (g->lanes[i].start_x + g->lanes[i].end_x) * 0.5;
            cy_to = (g->lanes[i].start_y + g->lanes[i].end_y) * 0.5;
            found_to = 1;
        }
    }
    if (!found_from || !found_to) return 0.0;
    return hypot(cx_to - cx_from, cy_to - cy_from);
}

/* ── Find edge cost from from_id to to_id ── */
static double edge_cost(const RouterGraph* g, int from_id, int to_id) {
    for (int i = 0; i < g->edge_count; i++) {
        if (g->edges[i].from_id == from_id && g->edges[i].to_id == to_id)
            return g->edges[i].cost;
    }
    return 1e10; /* very large if not found */
}

/* ══════════════════════════════════════════════════════════════ */
/*  Public API                                                    */
/* ══════════════════════════════════════════════════════════════ */

void router_graph_init(RouterGraph* g) {
    memset(g, 0, sizeof(*g));
}

int router_add_lane(RouterGraph* g, int id, double start_x, double end_x,
                    int lane_idx, int road_id, double speed_limit) {
    return router_add_lane_xy(g, id, start_x, end_x, 0.0, 0.0,
                              lane_idx, road_id, speed_limit);
}

int router_add_lane_xy(RouterGraph* g, int id, double start_x, double end_x,
                       double start_y, double end_y,
                       int lane_idx, int road_id, double speed_limit) {
    if (g->lane_count >= ROUTER_MAX_LANES) return -1;
    RouterLane* l = &g->lanes[g->lane_count++];
    l->id = id;
    l->start_x = start_x;
    l->end_x = end_x;
    l->start_y = start_y;
    l->end_y = end_y;
    l->lane_idx = lane_idx;
    l->road_id = road_id;
    l->speed_limit = speed_limit;
    l->length = hypot(end_x - start_x, end_y - start_y);
    return 0;
}

int router_add_edge(RouterGraph* g, int from_id, int to_id, int type) {
    if (g->edge_count >= ROUTER_MAX_EDGES) return -1;
    RouterEdge* e = &g->edges[g->edge_count++];
    e->from_id = from_id;
    e->to_id = to_id;
    e->type = type;
    /* cost is set later by build_topology or user */
    return 0;
}

void router_build_topology(RouterGraph* g, double lane_change_penalty) {
    /* First pass: assign costs to existing edges */
    for (int i = 0; i < g->edge_count; i++) {
        RouterEdge* e = &g->edges[i];
        /* Find the source lane */
        double len = 0;
        for (int j = 0; j < g->lane_count; j++) {
            if (g->lanes[j].id == e->from_id) {
                len = g->lanes[j].length;
                break;
            }
        }
        if (e->type == 0) {
            e->cost = len;  /* successor: length cost */
        } else {
            e->cost = lane_change_penalty;  /* lane change: penalty */
        }
    }

    /* Auto-build successor edges: same lane_idx, sorted by start_x */
    for (int a = 0; a < g->lane_count; a++) {
        for (int b = 0; b < g->lane_count; b++) {
            if (a == b) continue;
            if (g->lanes[a].lane_idx == g->lanes[b].lane_idx &&
                g->lanes[a].road_id == g->lanes[b].road_id &&
                fabs(g->lanes[a].end_x - g->lanes[b].start_x) < 0.1) {
                /* Check if edge already exists */
                int found = 0;
                for (int k = 0; k < g->edge_count; k++) {
                    if (g->edges[k].from_id == g->lanes[a].id &&
                        g->edges[k].to_id == g->lanes[b].id &&
                        g->edges[k].type == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    if (g->edge_count >= ROUTER_MAX_EDGES) return;
                    RouterEdge* e = &g->edges[g->edge_count++];
                    e->from_id = g->lanes[a].id;
                    e->to_id = g->lanes[b].id;
                    e->type = 0;
                    e->cost = g->lanes[a].length;
                }
            }
        }
    }

    /* Auto-build neighbor edges: same road_id, adjacent lane_idx */
    for (int a = 0; a < g->lane_count; a++) {
        /* Find overlapping lanes on adjacent lanes */
        for (int b = 0; b < g->lane_count; b++) {
            if (a == b) continue;
            if (g->lanes[a].road_id != g->lanes[b].road_id) continue;
            int dlan = abs(g->lanes[a].lane_idx - g->lanes[b].lane_idx);
            if (dlan != 1) continue;  /* must be immediate neighbors */

            /* Check overlap in x range */
            double a_min = g->lanes[a].start_x < g->lanes[a].end_x ?
                           g->lanes[a].start_x : g->lanes[a].end_x;
            double a_max = g->lanes[a].start_x > g->lanes[a].end_x ?
                           g->lanes[a].start_x : g->lanes[a].end_x;
            double b_min = g->lanes[b].start_x < g->lanes[b].end_x ?
                           g->lanes[b].start_x : g->lanes[b].end_x;
            double b_max = g->lanes[b].start_x > g->lanes[b].end_x ?
                           g->lanes[b].start_x : g->lanes[b].end_x;
            double overlap_min = (a_min > b_min) ? a_min : b_min;
            double overlap_max = (a_max < b_max) ? a_max : b_max;
            if (overlap_max - overlap_min < 0.1) continue;  /* no overlap */

            /* Add left/right edges */
            int type = (g->lanes[b].lane_idx == g->lanes[a].lane_idx - 1) ? 1 : 2;

            /* a → b */
            int found_ab = 0;
            for (int k = 0; k < g->edge_count; k++) {
                if (g->edges[k].from_id == g->lanes[a].id &&
                    g->edges[k].to_id == g->lanes[b].id &&
                    g->edges[k].type == type) {
                    found_ab = 1;
                    break;
                }
            }
            if (!found_ab) {
                if (g->edge_count >= ROUTER_MAX_EDGES) return;
                RouterEdge* e = &g->edges[g->edge_count++];
                e->from_id = g->lanes[a].id;
                e->to_id = g->lanes[b].id;
                e->type = type;
                e->cost = lane_change_penalty;
            }

            /* b → a (bidirectional) */
            int rev_type = (g->lanes[a].lane_idx == g->lanes[b].lane_idx - 1) ? 1 : 2;
            int found_ba = 0;
            for (int k = 0; k < g->edge_count; k++) {
                if (g->edges[k].from_id == g->lanes[b].id &&
                    g->edges[k].to_id == g->lanes[a].id &&
                    g->edges[k].type == rev_type) {
                    found_ba = 1;
                    break;
                }
            }
            if (!found_ba) {
                if (g->edge_count >= ROUTER_MAX_EDGES) return;
                RouterEdge* e = &g->edges[g->edge_count++];
                e->from_id = g->lanes[b].id;
                e->to_id = g->lanes[a].id;
                e->type = rev_type;
                e->cost = lane_change_penalty;
            }
        }
    }
}

int router_astar(const RouterGraph* g, int from_id, int to_id, RouterPath* path) {
    if (!g || !path) return -1;
    path->count = 0;
    path->total_cost = 0.0;

    if (from_id == to_id) {
        path->lane_ids[0] = from_id;
        path->count = 1;
        return 0;
    }

    /* Open set (binary heap) */
    AstarNode open_set[ROUTER_MAX_LANES];
    int open_size = 0;

    /* Closed set */
    uint64_t closed_set[CLOSED_SIZE];
    closed_set_init(closed_set);

    /* g_score and parent for each lane */
    double g_score[ROUTER_MAX_LANES];
    int parent[ROUTER_MAX_LANES];
    int in_open[ROUTER_MAX_LANES];
    for (int i = 0; i < ROUTER_MAX_LANES; i++) {
        g_score[i] = 1e15;
        parent[i] = -1;
        in_open[i] = 0;
    }

    double h0 = heuristic(g, from_id, to_id);
    g_score[from_id] = 0.0;
    pq_push(open_set, &open_size, from_id, 0.0, h0);
    in_open[from_id] = 1;

    while (open_size > 0) {
        int current;
        double cur_g;
        if (pq_pop(open_set, &open_size, &current, &cur_g) != 0) break;
        in_open[current] = 0;
        closed_set_add(closed_set, current);

        if (current == to_id) {
            /* Reconstruct path */
            int stack[ROUTER_MAX_PATH];
            int sp = 0;
            int node = current;
            while (node >= 0 && sp < ROUTER_MAX_PATH) {
                stack[sp++] = node;
                node = parent[node];
            }
            /* Reverse */
            path->count = 0;
            for (int i = sp - 1; i >= 0 && path->count < ROUTER_MAX_PATH; i--)
                path->lane_ids[path->count++] = stack[i];
            path->total_cost = cur_g;
            return 0;
        }

        /* Expand neighbors */
        for (int ei = 0; ei < g->edge_count; ei++) {
            const RouterEdge* e = &g->edges[ei];
            if (e->from_id != current) continue;
            int neighbor = e->to_id;
            if (closed_set_has(closed_set, neighbor)) continue;

            double tentative_g = cur_g + e->cost;
            if (tentative_g >= g_score[neighbor]) continue;

            g_score[neighbor] = tentative_g;
            parent[neighbor] = current;
            double h = heuristic(g, neighbor, to_id);
            if (!in_open[neighbor]) {
                pq_push(open_set, &open_size, neighbor, tentative_g, tentative_g + h);
                in_open[neighbor] = 1;
            }
        }
    }

    /* No path found */
    return -1;
}

int router_lane_at(const RouterGraph* g, int road_id, int lane_idx, double x) {
    for (int i = 0; i < g->lane_count; i++) {
        const RouterLane* l = &g->lanes[i];
        if (l->road_id != road_id || l->lane_idx != lane_idx) continue;
        double xmin = (l->start_x < l->end_x) ? l->start_x : l->end_x;
        double xmax = (l->start_x > l->end_x) ? l->start_x : l->end_x;
        if (x >= xmin - 0.01 && x <= xmax + 0.01)
            return l->id;
    }
    return -1;
}

/* ══════════════════════════════════════════════════════════════ */
/*  Map JSON builder — lane successors 带进运行时（A* 接入主循环）  */
/* ══════════════════════════════════════════════════════════════ */

/* 字符串 lane id → 图 int id 映射（本构建器局部使用，strdup 持有）。 */
typedef struct {
    char* key;   /* lane 字符串 id，如 "ns_avenue_00_seg_00.lane.1" */
    int   id;    /* 图 int id */
} RouterStrId;

typedef struct {
    RouterStrId* items;
    int          count;
    int          cap;
} RouterStrIdMap;

static void strid_map_free(RouterStrIdMap* m) {
    for (int i = 0; i < m->count; i++) free(m->items[i].key);
    free(m->items);
    m->items = NULL;
    m->count = 0;
    m->cap = 0;
}

static int strid_map_add(RouterStrIdMap* m, const char* key, int id) {
    if (m->count >= m->cap) {
        int ncap = m->cap ? m->cap * 2 : 64;
        RouterStrId* nitems = (RouterStrId*)realloc(m->items, (size_t)ncap * sizeof(RouterStrId));
        if (!nitems) return -1;
        m->items = nitems;
        m->cap = ncap;
    }
    m->items[m->count].key = strdup(key);
    m->items[m->count].id = id;
    m->count++;
    return 0;
}

static int strid_map_find(const RouterStrIdMap* m, const char* key) {
    for (int i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].key, key) == 0) return m->items[i].id;
    }
    return -1;
}

/* 从 JSON 数组取 [x, y] 点；失败返回 0。 */
static int json_pt_xy(cJSON* pt, double* ox, double* oy) {
    if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) < 2) return 0;
    cJSON* jx = cJSON_GetArrayItem(pt, 0);
    cJSON* jy = cJSON_GetArrayItem(pt, 1);
    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) return 0;
    *ox = jx->valuedouble;
    *oy = jy->valuedouble;
    return 1;
}

int router_build_from_map_json(RouterGraph* g, const char* road_net_json,
                               double lane_change_penalty, int* out_lane_count) {
    if (out_lane_count) *out_lane_count = 0;
    if (!g || !road_net_json) return -1;

    cJSON* rn = cJSON_Parse(road_net_json);
    if (!rn) return -1;
    cJSON* jedges = cJSON_GetObjectItemCaseSensitive(rn, "edges");
    if (!cJSON_IsArray(jedges)) { cJSON_Delete(rn); return -1; }

    RouterStrIdMap strmap = {0};
    int n_edges = cJSON_GetArraySize(jedges);
    int lane_count = 0;

    /* Pass 1：注册所有 lane（分配图 id，记录几何/road_id/direction/index） */
    for (int ei = 0; ei < n_edges; ei++) {
        cJSON* edge = cJSON_GetArrayItem(jedges, ei);
        if (!cJSON_IsObject(edge)) continue;
        /* road_id = edge 数字 id（resolve_map_reference 已写入 legacy_id 或索引，
         * 与 json_to_xodr 的 xodr road id 一致） */
        int road_id = ei;
        cJSON* jrid = cJSON_GetObjectItemCaseSensitive(edge, "id");
        if (cJSON_IsNumber(jrid)) road_id = (int)jrid->valuedouble;

        double speed = 15.0;
        cJSON* jspd = cJSON_GetObjectItemCaseSensitive(edge, "speed_limit");
        if (cJSON_IsNumber(jspd)) speed = jspd->valuedouble;

        /* edge 中心线回退几何 */
        double esx = 0, esy = 0, eex = 0, eey = 0;
        int edge_geom = 0;
        cJSON* jedge_cl = cJSON_GetObjectItemCaseSensitive(edge, "centerline");
        if (cJSON_IsArray(jedge_cl) && cJSON_GetArraySize(jedge_cl) >= 1) {
            cJSON* p0 = cJSON_GetArrayItem(jedge_cl, 0);
            cJSON* p1 = cJSON_GetArrayItem(jedge_cl, cJSON_GetArraySize(jedge_cl) - 1);
            if (json_pt_xy(p0, &esx, &esy) && json_pt_xy(p1, &eex, &eey))
                edge_geom = 1;
        }

        cJSON* jlanes = cJSON_GetObjectItemCaseSensitive(edge, "lanes");
        if (!cJSON_IsArray(jlanes)) continue;
        int n_lanes = cJSON_GetArraySize(jlanes);
        for (int li = 0; li < n_lanes; li++) {
            cJSON* lane = cJSON_GetArrayItem(jlanes, li);
            if (!cJSON_IsObject(lane)) continue;
            cJSON* jname = cJSON_GetObjectItemCaseSensitive(lane, "id");
            if (!cJSON_IsString(jname) || !jname->valuestring) continue;

            int direction = 1;
            cJSON* jdir = cJSON_GetObjectItemCaseSensitive(lane, "direction");
            if (cJSON_IsNumber(jdir) && jdir->valuedouble < 0.0) direction = -1;
            int idx = li + 1;
            cJSON* jidx = cJSON_GetObjectItemCaseSensitive(lane, "index");
            if (cJSON_IsNumber(jidx)) idx = (int)jidx->valuedouble;
            /* 对向 lane index 与 id 约定对齐（lane.101 = idx 101，见
             * map_compiler/extract_city_map 命名），便于同 road 内左右邻检测
             * 只连同向相邻 lane、禁止跨方向变道。map.json 的 index 字段对
             * 对向 lane 只给 1（与 id 前缀 101 不一致），须按 direction 补偏移。 */
            if (direction == -1 && idx < 100) idx += 100;

            /* lane 中心线首末点；缺失回退 edge 中心线（0 长度 → 单点） */
            double sx = 0, sy = 0, ex = 0, ey = 0;
            int has_geom = 0;
            cJSON* jcl = cJSON_GetObjectItemCaseSensitive(lane, "centerline");
            if (cJSON_IsArray(jcl) && cJSON_GetArraySize(jcl) >= 1) {
                cJSON* p0 = cJSON_GetArrayItem(jcl, 0);
                cJSON* p1 = cJSON_GetArrayItem(jcl, cJSON_GetArraySize(jcl) - 1);
                if (json_pt_xy(p0, &sx, &sy) && json_pt_xy(p1, &ex, &ey)) has_geom = 1;
            }
            if (!has_geom && edge_geom) { sx = esx; sy = esy; ex = eex; ey = eey; }

            int lane_id = g->lane_count;
            if (router_add_lane_xy(g, lane_id, sx, ex, sy, ey,
                                   idx, road_id, speed) != 0) {
                /* 图已满，放弃剩余 lane */
                goto done;
            }
            if (strid_map_add(&strmap, jname->valuestring, lane_id) != 0) goto done;
            lane_count++;
        }
    }

    /* Pass 2：显式 successors（type=0）+ 同 road 同方向相邻 lane（type=1/2） */
    int seq = 0;
    for (int ei = 0; ei < n_edges && seq < g->lane_count; ei++) {
        cJSON* edge = cJSON_GetArrayItem(jedges, ei);
        if (!cJSON_IsObject(edge)) continue;
        cJSON* jlanes = cJSON_GetObjectItemCaseSensitive(edge, "lanes");
        if (!cJSON_IsArray(jlanes)) continue;

        int n_lanes = cJSON_GetArraySize(jlanes);
        for (int li = 0; li < n_lanes; li++) {
            cJSON* lane = cJSON_GetArrayItem(jlanes, li);
            if (!cJSON_IsObject(lane)) continue;
            if (seq >= g->lane_count) break;
            int graph_id = seq++;

            /* successors：字符串 id → 图 id */
            cJSON* jsucc = cJSON_GetObjectItemCaseSensitive(lane, "successors");
            if (cJSON_IsArray(jsucc)) {
                int ns = cJSON_GetArraySize(jsucc);
                for (int si = 0; si < ns; si++) {
                    cJSON* js = cJSON_GetArrayItem(jsucc, si);
                    if (!cJSON_IsString(js) || !js->valuestring) continue;
                    int tid = strid_map_find(&strmap, js->valuestring);
                    if (tid < 0) continue;   /* 指向图外 road（route 过滤掉的段） */
                    router_add_edge(g, graph_id, tid, 0);
                }
            }
        }
    }

    /* 同 road 同方向相邻 lane 左右邻边（双向，代价 = lane_change_penalty） */
    for (int a = 0; a < g->lane_count; a++) {
        for (int b = a + 1; b < g->lane_count; b++) {
            const RouterLane* la = &g->lanes[a];
            const RouterLane* lb = &g->lanes[b];
            if (la->road_id != lb->road_id) continue;
            int da = (la->lane_idx >= 100) ? -1 : 1;
            int db = (lb->lane_idx >= 100) ? -1 : 1;
            if (da != db) continue;   /* 对向 lane 不互邻（禁止越线变道） */
            if (abs(la->lane_idx - lb->lane_idx) != 1) continue;
            /* 同方向相邻：a→b 与 b→a 双向 */
            router_add_edge(g, a, b, 1);
            router_add_edge(g, b, a, 2);
        }
    }

    /* 赋边代价：后继 = 源 lane 长度；邻边 = lane_change_penalty */
    for (int i = 0; i < g->edge_count; i++) {
        RouterEdge* e = &g->edges[i];
        if (e->type == 0) {
            double len = 0;
            for (int j = 0; j < g->lane_count; j++) {
                if (g->lanes[j].id == e->from_id) { len = g->lanes[j].length; break; }
            }
            e->cost = len;
        } else {
            e->cost = lane_change_penalty;
        }
    }

done:
    strid_map_free(&strmap);
    cJSON_Delete(rn);
    if (out_lane_count) *out_lane_count = lane_count;
    return 0;
}

int router_lane_id_in_road(const RouterGraph* g, int road_id, int direction,
                           int lane_idx) {
    int best = -1;
    for (int i = 0; i < g->lane_count; i++) {
        const RouterLane* l = &g->lanes[i];
        if (l->road_id != road_id) continue;
        int d = (l->lane_idx >= 100) ? -1 : 1;
        if (d != direction) continue;
        if (lane_idx > 0 && l->lane_idx == lane_idx) return l->id;
        if (lane_idx <= 0) {
            /* 取该方向 index 最小（= 最内侧第一车道） */
            if (best < 0 || l->lane_idx < g->lanes[best].lane_idx) best = i;
        }
    }
    return best >= 0 ? g->lanes[best].id : -1;
}
