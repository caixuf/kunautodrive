#include <unistd.h>
/**
 * test_traffic_density_scenario.c — D3-2 scenario 契约单测
 *
 * 覆盖（REQ_L3_DIR2_HDMAP.md D3-1 / D3-2）：
 *   1. 场景含 traffic_density 块 → 5 字段全部解析正确
 *   2. 场景不含 traffic_density → 全部默认（disabled）
 *   3. 字段类型错 → 静默 fallback 到默认，不阻塞场景加载
 *   4. scenario_to_json 序列化：cars_per_km > 0 时输出块，=0 时不输出
 *
 * 与 flowsim_node.cpp 内的 auto-populate 函数解耦：本档只验证契约层
 * （JSON 解析/默认值/序列化），执行路径在 flowsim 节点单跑或集成测试
 * 覆盖（受限于 esminiRoadManager 的预构建依赖）。
 */

#include "scenario_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { printf("  %-58s ", name); fflush(stdout); } while (0)
#define PASS() do { printf("PASS\n"); g_passed++; } while (0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); g_failed++; } while (0)
#define ASSERT(cond, fmt, ...) do { if (!(cond)) { FAIL(fmt, ##__VA_ARGS__); return; } } while (0)

/* 写临时场景 JSON 文件，路径由 caller 负责 unlink */
static void write_file(const char* path, const char* body) {
    FILE* f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(body, f);
    fclose(f);
}

/* ── Test 1: traffic_density 5 字段全解析 ── */
static void test_traffic_density_parse_full(void) {
    TEST("traffic_density: 完整块 5 字段解析正确");
    const char* path = "/tmp/_td_full.json";
    write_file(path,
        "{\"name\":\"td_full\",\"actors\":[],"
        "\"traffic_density\":{"
            "\"cars_per_km\":15,"
            "\"spawn_jitter_m\":3.5,"
            "\"lane_spread\":true,"
            "\"max_npcs\":42,"
            "\"random_seed_offset\":7"
        "}}");
    ScenarioConfig* sc = scenario_load(path);
    ASSERT(sc != NULL, "scenario_load returned NULL");
    if (!sc) { unlink(path); return; }
    ASSERT(sc->traffic_density.cars_per_km        == 15, "cars_per_km");
    ASSERT(sc->traffic_density.spawn_jitter_m     >  3.4 && sc->traffic_density.spawn_jitter_m < 3.6,
           "spawn_jitter_m ~= 3.5, got %.2f", sc->traffic_density.spawn_jitter_m);
    ASSERT(sc->traffic_density.lane_spread        == 1, "lane_spread=true → 1");
    ASSERT(sc->traffic_density.max_npcs           == 42, "max_npcs");
    ASSERT(sc->traffic_density.random_seed_offset == 7, "random_seed_offset");
    scenario_free(sc);
    unlink(path);
    PASS();
}

/* ── Test 2: 无 traffic_density → 全默认 ── */
static void test_traffic_density_parse_absent(void) {
    TEST("traffic_density 缺省 → 全默认（disabled）");
    const char* path = "/tmp/_td_absent.json";
    write_file(path, "{\"name\":\"td_absent\",\"actors\":[]}");
    ScenarioConfig* sc = scenario_load(path);
    ASSERT(sc != NULL, "scenario_load returned NULL");
    if (!sc) { unlink(path); return; }
    ASSERT(sc->traffic_density.cars_per_km        == 0, "cars_per_km default 0");
    ASSERT(sc->traffic_density.spawn_jitter_m     >  1.9 && sc->traffic_density.spawn_jitter_m < 2.1,
           "spawn_jitter_m default 2.0, got %.2f", sc->traffic_density.spawn_jitter_m);
    ASSERT(sc->traffic_density.lane_spread        == 1, "lane_spread default 1");
    ASSERT(sc->traffic_density.max_npcs           == 50, "max_npcs default 50");
    ASSERT(sc->traffic_density.random_seed_offset == 0, "random_seed_offset default 0");
    scenario_free(sc);
    unlink(path);
    PASS();
}

/* ── Test 3: 字段类型错 → 静默 fallback ── */
static void test_traffic_density_parse_type_errors(void) {
    TEST("traffic_density: 字段类型错 → 静默 fallback，不阻塞加载");
    const char* path = "/tmp/_td_typeerr.json";
    write_file(path,
        "{\"name\":\"td_typeerr\",\"actors\":[],"
        "\"traffic_density\":{"
            "\"cars_per_km\":\"not_a_number\","     /* 类型错 */
            "\"spawn_jitter_m\":true,"              /* 类型错 */
            "\"lane_spread\":\"yes\","              /* 类型错 */
            "\"max_npcs\":-5,"                      /* 负数拒绝 */
            "\"random_seed_offset\":null"           /* 类型错 */
        "}}");
    ScenarioConfig* sc = scenario_load(path);
    ASSERT(sc != NULL, "type errors must NOT block scenario_load");
    if (!sc) { unlink(path); return; }
    /* 全 default */
    ASSERT(sc->traffic_density.cars_per_km        == 0, "type err cars_per_km → 0");
    ASSERT(sc->traffic_density.spawn_jitter_m     > 1.9 && sc->traffic_density.spawn_jitter_m < 2.1,
           "type err spawn_jitter_m → 2.0");
    ASSERT(sc->traffic_density.lane_spread        == 1, "type err lane_spread → 1 (default)");
    ASSERT(sc->traffic_density.max_npcs           == 50, "negative max_npcs → 50 (default)");
    ASSERT(sc->traffic_density.random_seed_offset == 0, "null random_seed_offset → 0");
    scenario_free(sc);
    unlink(path);
    PASS();
}

/* ── Test 4: 序列化 round-trip ── */
static void test_traffic_density_serialize_when_enabled(void) {
    TEST("scenario_to_json: cars_per_km > 0 时输出 traffic_density 块");
    ScenarioConfig sc;
    memset(&sc, 0, sizeof(sc));
    snprintf(sc.name, sizeof(sc.name), "td_ser");
    snprintf(sc.description, sizeof(sc.description), "td_ser test");
    sc.traffic_density.cars_per_km        = 8;
    sc.traffic_density.spawn_jitter_m     = 1.5;
    sc.traffic_density.lane_spread        = 0;
    sc.traffic_density.max_npcs           = 12;
    sc.traffic_density.random_seed_offset = 3;
    char* js = scenario_to_json(&sc);
    ASSERT(js != NULL, "scenario_to_json returned NULL");
    if (!js) return;
    ASSERT(strstr(js, "\"traffic_density\"") != NULL,
           "serialized JSON must contain traffic_density block when cars_per_km > 0");
    /* cJSON_Print tab 分隔，用 strchr 跳过 tab/换行 */
    const char* pk = strstr(js, "\"cars_per_km\"");
    ASSERT(pk != NULL, "serialized JSON must contain cars_per_km key");
    if (pk) {
        pk = strchr(pk, ':');
        ASSERT(pk && strchr(pk, '8') != NULL,
               "serialized JSON must contain cars_per_km value 8");
    }
    const char* pm = strstr(js, "\"max_npcs\"");
    ASSERT(pm != NULL, "serialized JSON must contain max_npcs key");
    if (pm) {
        pm = strchr(pm, ':');
        ASSERT(pm && strchr(pm, '1') && strchr(pm, '2'),
               "serialized JSON must contain max_npcs value 12");
    }
    free(js);
    PASS();
}

static void test_traffic_density_serialize_when_disabled(void) {
    TEST("scenario_to_json: cars_per_km = 0 时不输出 traffic_density 块");
    ScenarioConfig sc;
    memset(&sc, 0, sizeof(sc));
    snprintf(sc.name, sizeof(sc.name), "td_off");
    snprintf(sc.description, sizeof(sc.description), "td_off test");
    /* cars_per_km = 0（缺省） */
    char* js = scenario_to_json(&sc);
    ASSERT(js != NULL, "scenario_to_json returned NULL");
    if (!js) return;
    ASSERT(strstr(js, "\"traffic_density\"") == NULL,
           "serialized JSON must NOT contain traffic_density block when cars_per_km = 0");
    free(js);
    PASS();
}

int main(void) {
    printf("═══ traffic_density scenario 契约 (D3-2) ═══\n");
    test_traffic_density_parse_full();
    test_traffic_density_parse_absent();
    test_traffic_density_parse_type_errors();
    test_traffic_density_serialize_when_enabled();
    test_traffic_density_serialize_when_disabled();
    printf("\n════════════════════════════════════\n");
    printf("  Total: %d  ✅ Passed: %d  ❌ Failed: %d\n",
           g_passed + g_failed, g_passed, g_failed);
    printf("════════════════════════════════════\n\n");
    return g_failed > 0 ? 1 : 0;
}
