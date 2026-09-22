/**
 * test_junction_reroute.c — M3-lite：单路口、路口前重规划冒烟。
 *
 * 从 maps/city_grid/map.json 切出十字路口 (200 m, 200 m) 的四条入射段，
 * 用运行时 router_build_from_map_json + router_astar 证明：
 *   - 北上接近路口的 ns_avenue_01_seg_00.lane.1 可以改去右转
 *     ew_avenue_01_seg_01 或左转 ew_avenue_01_seg_00（对向 lane.101）；
 *   - 该路径的下一跳是转向 successor，与“沿 ns_avenue_01 直行到 seg_01”不同；
 *   - 去掉跨大道 successor、只留同大道直行后，横向出路不可达，直行链仍可达。
 *
 * 运行：ctest --test-dir build -R junction_reroute --output-on-failure
 */
#include "scenario_router.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { printf("  %-52s ", name); fflush(stdout); } while (0)
#define PASS() do { printf("PASS\n"); g_passed++; } while (0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); g_failed++; } while (0)

/* 数组下标即 router 的数字 road id（edge.id 是字符串，builder 回退用下标）。 */
enum {
    ROAD_APPROACH = 0, /* ns_avenue_01_seg_00：沿 +y 进入路口 */
    ROAD_STRAIGHT = 1, /* ns_avenue_01_seg_01：同大道直行 */
    ROAD_RIGHT    = 2, /* ew_avenue_01_seg_01：右转，沿 +x */
    ROAD_LEFT     = 3  /* ew_avenue_01_seg_00：左转，沿 -x 走 lane.101 */
};

static const char* k_roads[] = {
    "ns_avenue_01_seg_00",
    "ns_avenue_01_seg_01",
    "ew_avenue_01_seg_01",
    "ew_avenue_01_seg_00",
};

static const char* k_straight_lane = "ns_avenue_01_seg_01.lane.1";
static const char* k_right_lane = "ew_avenue_01_seg_01.lane.1";
static const char* k_left_lane = "ew_avenue_01_seg_00.lane.101";

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static void copy_span(const char* s, size_t n, char* out, size_t cap) {
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

static void avenue_stem(const char* road_id, char* out, size_t cap) {
    const char* seg = strstr(road_id, "_seg_");
    size_t n = seg ? (size_t)(seg - road_id) : strlen(road_id);
    copy_span(road_id, n, out, cap);
}

static void road_of_lane_id(const char* lane_id, char* out, size_t cap) {
    const char* dot = strstr(lane_id, ".lane.");
    size_t n = dot ? (size_t)(dot - lane_id) : strlen(lane_id);
    copy_span(lane_id, n, out, cap);
}

static cJSON* find_road(cJSON* roads, const char* id) {
    int n = cJSON_GetArraySize(roads);
    for (int i = 0; i < n; i++) {
        cJSON* road = cJSON_GetArrayItem(roads, i);
        cJSON* jid = cJSON_GetObjectItemCaseSensitive(road, "id");
        if (cJSON_IsString(jid) && strcmp(jid->valuestring, id) == 0) return road;
    }
    return NULL;
}

static int succ_has(cJSON* lane, const char* id) {
    cJSON* succ = cJSON_GetObjectItemCaseSensitive(lane, "successors");
    if (!cJSON_IsArray(succ)) return 0;
    int n = cJSON_GetArraySize(succ);
    for (int i = 0; i < n; i++) {
        cJSON* item = cJSON_GetArrayItem(succ, i);
        if (cJSON_IsString(item) && strcmp(item->valuestring, id) == 0) return 1;
    }
    return 0;
}

/* 只保留同一大道（ns_avenue_XX / ew_avenue_XX）上的 successor，丢掉左/右转。 */
static void keep_straight_only(cJSON* edge) {
    cJSON* jid = cJSON_GetObjectItemCaseSensitive(edge, "id");
    if (!cJSON_IsString(jid) || !jid->valuestring) return;
    char stem[64];
    avenue_stem(jid->valuestring, stem, sizeof(stem));

    cJSON* lanes = cJSON_GetObjectItemCaseSensitive(edge, "lanes");
    if (!cJSON_IsArray(lanes)) return;
    int n = cJSON_GetArraySize(lanes);
    for (int i = 0; i < n; i++) {
        cJSON* lane = cJSON_GetArrayItem(lanes, i);
        cJSON* succ = cJSON_GetObjectItemCaseSensitive(lane, "successors");
        if (!cJSON_IsArray(succ)) continue;
        cJSON* kept = cJSON_CreateArray();
        int ns = cJSON_GetArraySize(succ);
        for (int s = 0; s < ns; s++) {
            cJSON* item = cJSON_GetArrayItem(succ, s);
            if (!cJSON_IsString(item) || !item->valuestring) continue;
            char road[96];
            char succ_stem[64];
            road_of_lane_id(item->valuestring, road, sizeof(road));
            avenue_stem(road, succ_stem, sizeof(succ_stem));
            if (strcmp(stem, succ_stem) == 0)
                cJSON_AddItemToArray(kept, cJSON_CreateString(item->valuestring));
        }
        cJSON_DeleteItemFromObject(lane, "successors");
        cJSON_AddItemToObject(lane, "successors", kept);
    }
}

static int build_graph(cJSON* edges, int strip, RouterGraph* g, int* lane_count) {
    cJSON* copy = cJSON_Duplicate(edges, 1);
    if (!copy) return -1;
    if (strip) {
        int n = cJSON_GetArraySize(copy);
        for (int i = 0; i < n; i++)
            keep_straight_only(cJSON_GetArrayItem(copy, i));
    }
    cJSON* net = cJSON_CreateObject();
    cJSON_AddItemToObject(net, "edges", copy);
    char* text = cJSON_PrintUnformatted(net);
    cJSON_Delete(net);
    if (!text) return -1;
    int rc = router_build_from_map_json(g, text, 8.0, lane_count);
    free(text);
    return rc;
}

static int has_successor_edge(const RouterGraph* g, int from_id, int to_id) {
    for (int i = 0; i < g->edge_count; i++) {
        if (g->edges[i].from_id == from_id && g->edges[i].to_id == to_id &&
            g->edges[i].type == 0)
            return 1;
    }
    return 0;
}

static cJSON* approach_lane1(cJSON* edges) {
    cJSON* road = cJSON_GetArrayItem(edges, ROAD_APPROACH);
    cJSON* lanes = road ? cJSON_GetObjectItemCaseSensitive(road, "lanes") : NULL;
    if (!cJSON_IsArray(lanes)) return NULL;
    int n = cJSON_GetArraySize(lanes);
    for (int i = 0; i < n; i++) {
        cJSON* lane = cJSON_GetArrayItem(lanes, i);
        cJSON* jid = cJSON_GetObjectItemCaseSensitive(lane, "id");
        if (cJSON_IsString(jid) && jid->valuestring &&
            strcmp(jid->valuestring, "ns_avenue_01_seg_00.lane.1") == 0)
            return lane;
    }
    return NULL;
}

static void test_pre_junction_reroute(void) {
    TEST("city_grid pre-junction reroute uses turn successor");
    char* buf = NULL;
    cJSON* root = NULL;
    cJSON* edges = NULL;
    RouterGraph full;
    RouterGraph straight_only;
    memset(&full, 0, sizeof(full));
    memset(&straight_only, 0, sizeof(straight_only));

    buf = read_file(CITY_GRID_MAP_JSON);
    if (!buf) { FAIL("cannot read %s", CITY_GRID_MAP_JSON); goto cleanup; }
    root = cJSON_Parse(buf);
    if (!root) { FAIL("map.json parse failed"); goto cleanup; }
    cJSON* roads = cJSON_GetObjectItemCaseSensitive(root, "roads");
    if (!cJSON_IsArray(roads)) { FAIL("map.json has no roads[]"); goto cleanup; }

    edges = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON* road = find_road(roads, k_roads[i]);
        if (!road) { FAIL("missing road %s", k_roads[i]); goto cleanup; }
        cJSON_AddItemToArray(edges, cJSON_Duplicate(road, 1));
    }
    cJSON* approach = approach_lane1(edges);
    if (!approach) { FAIL("approach lane.1 missing"); goto cleanup; }
    if (!succ_has(approach, k_straight_lane) || !succ_has(approach, k_right_lane) ||
        !succ_has(approach, k_left_lane)) {
        FAIL("approach successors missing straight/right/left");
        goto cleanup;
    }

    router_graph_init(&full);
    router_graph_init(&straight_only);
    int full_lanes = 0;
    int straight_lanes = 0;
    if (build_graph(edges, 0, &full, &full_lanes) != 0) {
        FAIL("router_build_from_map_json failed");
        goto cleanup;
    }
    if (build_graph(edges, 1, &straight_only, &straight_lanes) != 0) {
        FAIL("straight-only graph build failed");
        goto cleanup;
    }
    if (full_lanes != 16 || straight_lanes != 16) {
        FAIL("expected 16 lanes, got full=%d straight=%d", full_lanes, straight_lanes);
        goto cleanup;
    }

    int start = router_lane_id_in_road(&full, ROAD_APPROACH, 1, 1);
    int straight = router_lane_id_in_road(&full, ROAD_STRAIGHT, 1, 1);
    int right = router_lane_id_in_road(&full, ROAD_RIGHT, 1, 1);
    int left = router_lane_id_in_road(&full, ROAD_LEFT, -1, 101);
    if (start < 0 || straight < 0 || right < 0 || left < 0) {
        FAIL("lane lookup start=%d straight=%d right=%d left=%d",
             start, straight, right, left);
        goto cleanup;
    }
    if (right == straight || left == straight || right == left) {
        FAIL("turn lanes alias each other (straight=%d right=%d left=%d)",
             straight, right, left);
        goto cleanup;
    }

    RouterPath turn;
    RouterPath stay;
    memset(&turn, 0, sizeof(turn));
    memset(&stay, 0, sizeof(stay));
    if (router_astar(&full, start, right, &turn) != 0 || turn.count != 2 ||
        turn.lane_ids[0] != start || turn.lane_ids[1] != right) {
        FAIL("right reroute path count=%d", turn.count);
        goto cleanup;
    }
    if (router_astar(&full, start, left, &turn) != 0 || turn.count != 2 ||
        turn.lane_ids[1] != left) {
        FAIL("left reroute path count=%d", turn.count);
        goto cleanup;
    }
    if (!has_successor_edge(&full, start, right) || !has_successor_edge(&full, start, left)) {
        FAIL("turn hop is not a type=0 successor edge");
        goto cleanup;
    }
    if (router_astar(&full, start, straight, &stay) != 0 || stay.count != 2 ||
        stay.lane_ids[1] != straight) {
        FAIL("straight chain path count=%d", stay.count);
        goto cleanup;
    }
    if (turn.lane_ids[1] == stay.lane_ids[1]) {
        FAIL("reroute collapsed to the straight chain");
        goto cleanup;
    }

    int start_s = router_lane_id_in_road(&straight_only, ROAD_APPROACH, 1, 1);
    int straight_s = router_lane_id_in_road(&straight_only, ROAD_STRAIGHT, 1, 1);
    int right_s = router_lane_id_in_road(&straight_only, ROAD_RIGHT, 1, 1);
    int left_s = router_lane_id_in_road(&straight_only, ROAD_LEFT, -1, 101);
    RouterPath blocked;
    if (router_astar(&straight_only, start_s, right_s, &blocked) != -1 ||
        router_astar(&straight_only, start_s, left_s, &blocked) != -1) {
        FAIL("cross-road still reachable without turn successors");
        goto cleanup;
    }
    if (router_astar(&straight_only, start_s, straight_s, &stay) != 0 ||
        stay.lane_ids[1] != straight_s) {
        FAIL("same-avenue chain broke after stripping turns");
        goto cleanup;
    }

    PASS();
cleanup:
    router_graph_free(&full);
    router_graph_free(&straight_only);
    cJSON_Delete(edges);
    cJSON_Delete(root);
    free(buf);
}

int main(void) {
    printf("=== city_grid junction reroute smoke ===\n");
    test_pre_junction_reroute();
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
