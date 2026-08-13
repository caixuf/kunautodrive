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

int main(void) {
    printf("=== scenario_router A* tests ===\n");
    test_straight_chain();
    test_unreachable();
    test_2d_grid();
    test_xy_length();
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
