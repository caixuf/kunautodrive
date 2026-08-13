/**
 * test_scenario_router.c — 车道级 A* 路由单测。
 *
 * 覆盖：
 *   1. 直线链 A* 求最短路径 + 代价
 *   2. 二维网格大地图（>256 lane，逼近 city_grid 规模）A* 求含转向路径
 *   3. 断链不可达
 *   4. router_add_lane_xy 长度用二维欧氏
 */
#include "scenario_router.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { printf("  %-52s ", name); fflush(stdout); } while (0)
#define PASS() do { printf("PASS\n"); g_passed++; } while (0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); g_failed++; } while (0)
#define ASSERT(cond, fmt, ...) do { if (!(cond)) { FAIL(fmt, ##__VA_ARGS__); return; } } while (0)

/* 直线链：n 段，每段 100m，id 连续，后继边 i->i+1 */
static void test_straight_chain(void) {
    TEST("straight chain shortest path");
    RouterGraph g;
    router_graph_init(&g);
    int n = 5;
    for (int i = 0; i < n; i++) {
        router_add_lane_xy(&g, i, i * 100.0, (i + 1) * 100.0, 0.0, 0.0, 0, 0, 20.0);
    }
    for (int i = 0; i < n - 1; i++) {
        router_add_edge(&g, i, i + 1, 0); /* successor */
    }
    router_build_topology(&g, 8.0); /* 赋 cost：后继=length，变道=penalty */
    RouterPath p;
    int rc = router_astar(&g, 0, n - 1, &p);
    ASSERT(rc == 0, "A* should succeed on straight chain");
    ASSERT(p.count == n, "path should contain all %d lanes, got %d", n, p.count);
    ASSERT(fabs(p.total_cost - 400.0) < 1.0, "cost should be 400m, got %.1f", p.total_cost);
    for (int i = 0; i < n; i++) ASSERT(p.lane_ids[i] == i, "lane order wrong at %d", i);
    PASS();
}

/* 断链不可达 */
static void test_unreachable(void) {
    TEST("disconnected chain unreachable");
    RouterGraph g;
    router_graph_init(&g);
    router_add_lane_xy(&g, 0, 0, 100, 0, 0, 0, 0, 20.0);
    router_add_lane_xy(&g, 1, 100, 200, 0, 0, 0, 0, 20.0);
    router_add_lane_xy(&g, 2, 200, 300, 0, 0, 0, 0, 20.0);
    router_add_edge(&g, 0, 1, 0);
    /* 1 -> 2 断开 */
    RouterPath p;
    ASSERT(router_astar(&g, 0, 2, &p) == -1, "disconnected should return -1");
    PASS();
}

/* 二维网格：N x N 交叉口，每段 100m。测从 (0,0) 网格到 (2,2) 网格含转向。
 * lane id 编码 = row * N + col，行沿 x、列沿 y。 */
static void test_2d_grid(void) {
    TEST("2D grid >256 lanes finds turn path");
    const int N = 20;             /* 20x20 = 400 lane > 256 */
    RouterGraph g;
    router_graph_init(&g);
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            int id = r * N + c;
            /* 每格代表一条沿 y 的竖向 lane（x 固定，y 从 c*100 到 (c+1)*100） */
            router_add_lane_xy(&g, id, r * 100.0, r * 100.0,
                               c * 100.0, (c + 1) * 100.0, 0, 0, 20.0);
        }
    }
    /* 后继：同一 row，c -> c+1（沿 y 前进）；跨 row 需转向（这里同 row 直行） */
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N - 1; c++) {
            router_add_edge(&g, r * N + c, r * N + c + 1, 0);
        }
    }
    RouterPath p;
    int rc = router_astar(&g, 0, 0 * N + (N - 1), &p); /* (0,0) -> (0,19) */
    ASSERT(rc == 0, "grid A* should succeed");
    ASSERT(p.count == N, "vertical path should have %d lanes", N);
    PASS();
}

/* router_add_lane_xy 二维长度 */
static void test_xy_length(void) {
    TEST("router_add_lane_xy 2D length");
    RouterGraph g;
    router_graph_init(&g);
    router_add_lane_xy(&g, 7, 0, 0, 0, 30, 0, 0, 20.0); /* 30m 竖向 */
    int found = 0;
    for (int i = 0; i < g.lane_count; i++) {
        if (g.lanes[i].id == 7) {
            ASSERT(fabs(g.lanes[i].length - 30.0) < 1e-6, "length should be 30");
            found = 1;
        }
    }
    ASSERT(found, "lane 7 not found");
    PASS();
}

/* ── map JSON 建图（router_build_from_map_json）── */
/* 两条 road 直链：r0 -> r1（正向 lane.1 后继指向 r1.lane.1）。
 * 每条 road 4 lane：正向 lane.1/lane.2 + 对向 lane.101/lane.102。 */
static const char* TWO_ROAD_CHAIN_JSON =
    "{\"edges\":["
    "{\"id\":0,\"speed_limit\":15.0,"
    "\"centerline\":[[0,0],[100,0]],"
    "\"lanes\":["
    "{\"id\":\"r0.lane.1\",\"index\":1,\"direction\":1,\"centerline\":[[0,-1.75],[100,-1.75]],\"successors\":[\"r1.lane.1\"]},"
    "{\"id\":\"r0.lane.2\",\"index\":2,\"direction\":1,\"centerline\":[[0,1.75],[100,1.75]],\"successors\":[\"r1.lane.2\"]},"
    "{\"id\":\"r0.lane.101\",\"index\":1,\"direction\":-1,\"centerline\":[[0,3.5],[100,3.5]],\"successors\":[]},"
    "{\"id\":\"r0.lane.102\",\"index\":2,\"direction\":-1,\"centerline\":[[0,6.5],[100,6.5]],\"successors\":[]}"
    "]},"
    "{\"id\":1,\"speed_limit\":15.0,"
    "\"centerline\":[[100,0],[200,0]],"
    "\"lanes\":["
    "{\"id\":\"r1.lane.1\",\"index\":1,\"direction\":1,\"centerline\":[[100,-1.75],[200,-1.75]],\"successors\":[]},"
    "{\"id\":\"r1.lane.2\",\"index\":2,\"direction\":1,\"centerline\":[[100,1.75],[200,1.75]],\"successors\":[]},"
    "{\"id\":\"r1.lane.101\",\"index\":1,\"direction\":-1,\"centerline\":[[100,3.5],[200,3.5]],\"successors\":[]},"
    "{\"id\":\"r1.lane.102\",\"index\":2,\"direction\":-1,\"centerline\":[[100,6.5],[200,6.5]],\"successors\":[]}"
    "]}]}";

static void test_map_json_build_and_astar(void) {
    TEST("map json: build graph + A* straight chain");
    RouterGraph g;
    router_graph_init(&g);
    int lane_count = 0;
    ASSERT(router_build_from_map_json(&g, TWO_ROAD_CHAIN_JSON, 8.0, &lane_count) == 0,
           "build should succeed");
    ASSERT(lane_count == 8, "expected 8 lanes, got %d", lane_count);

    /* 正向第一车道 r0.lane.1 -> r1.lane.1 */
    int start = router_lane_id_in_road(&g, 0, 1, 0);
    int goal  = router_lane_id_in_road(&g, 1, 1, 0);
    ASSERT(start >= 0 && goal >= 0, "start/goal lane lookup failed (%d/%d)", start, goal);
    RouterPath p;
    ASSERT(router_astar(&g, start, goal, &p) == 0, "A* should succeed");
    ASSERT(p.count == 2, "path should be 2 lanes, got %d", p.count);
    ASSERT(p.lane_ids[0] == start && p.lane_ids[1] == goal, "path order wrong");
    ASSERT(fabs(p.total_cost - 100.0) < 1.0, "cost should be ~100m, got %.1f", p.total_cost);
    PASS();
}

static void test_map_json_no_cross_direction_neighbors(void) {
    TEST("map json: same-direction neighbors only");
    RouterGraph g;
    router_graph_init(&g);
    int lane_count = 0;
    ASSERT(router_build_from_map_json(&g, TWO_ROAD_CHAIN_JSON, 8.0, &lane_count) == 0,
           "build should succeed");
    /* r0.lane.1 (idx 1) 与 r0.lane.2 (idx 2) 同向相邻 → 有边 */
    int l1 = router_lane_id_in_road(&g, 0, 1, 1);
    int l2 = router_lane_id_in_road(&g, 0, 1, 2);
    int l101 = router_lane_id_in_road(&g, 0, -1, 101);  /* 对向 lane index 约定 = 101 */
    ASSERT(l1 >= 0 && l2 >= 0 && l101 >= 0, "lane lookup failed");
    int has_12 = 0, has_101 = 0;
    for (int i = 0; i < g.edge_count; i++) {
        if (g.edges[i].from_id == l1 && g.edges[i].to_id == l2) has_12 = 1;
        if (g.edges[i].from_id == l1 && g.edges[i].to_id == l101) has_101 = 1;
        if (g.edges[i].from_id == l101 && g.edges[i].to_id == l1) has_101 = 1;
    }
    ASSERT(has_12, "same-direction neighbors should connect");
    ASSERT(!has_101, "cross-direction must NOT connect (禁止越线变道)");
    PASS();
}

/* 转向图：r0 正向 lane.1 同时指向 r1.lane.1（直行）与 r2.lane.1（右转）。
 * A* 到 r2 应产出经 r0 -> r2 的转向路径（road 链含跨 road 转向）。 */
static void test_map_json_turn_route(void) {
    TEST("map json: A* finds turn route across roads");
    RouterGraph g;
    router_graph_init(&g);
    /* 手工构图：r0(3 lanes: lane.1/lane.2 正向) → r1(直行), r2(右转)。
     * r2 是竖向 road（y 方向），检验 2D 启发式能导向它。 */
    int r0_1 = 0, r0_2 = 1, r1_1 = 2, r2_1 = 3;
    router_add_lane_xy(&g, r0_1, 0, 100, -1.75, -1.75, 1, 0, 15.0);
    router_add_lane_xy(&g, r0_2, 0, 100, 1.75, 1.75, 2, 0, 15.0);
    router_add_lane_xy(&g, r1_1, 100, 200, -1.75, -1.75, 1, 1, 15.0);
    router_add_lane_xy(&g, r2_1, 100, 100, -1.75, -101.75, 1, 2, 15.0); /* 右转，向南 100m */
    router_add_edge(&g, r0_1, r1_1, 0);
    router_add_edge(&g, r0_1, r2_1, 0);
    router_add_edge(&g, r0_2, r1_1, 0);
    for (int i = 0; i < g.edge_count; i++) g.edges[i].cost = 100.0; /* 简化代价 */
    RouterPath p;
    ASSERT(router_astar(&g, r0_1, r2_1, &p) == 0, "turn A* should succeed");
    ASSERT(p.count == 2 && p.lane_ids[0] == r0_1 && p.lane_ids[1] == r2_1,
           "turn path wrong (count=%d)", p.count);
    PASS();
}

int main(void) {
    printf("=== scenario_router A* tests ===\n");
    test_straight_chain();
    test_unreachable();
    test_2d_grid();
    test_xy_length();
    test_map_json_build_and_astar();
    test_map_json_no_cross_direction_neighbors();
    test_map_json_turn_route();
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
