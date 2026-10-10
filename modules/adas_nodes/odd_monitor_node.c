/* odd_monitor_node.c — ODD（运行设计域）监控节点（L3-P1 方向六）。
 *
 * 职责（单一）：**无状态**地把当前一组输入判成 in-ODD / out-of-ODD，发 `odd/state`。
 * 不记忆、不决策、不发控制量——决策是 tor_manager 的事（模块职责铁律）。
 *
 * ODD 定义（4 项，与 tools/tor_mrm_sim.py::classify_odd 逐条对应，单一事实源）：
 *   1. 天气/能见度：environment/state 的 weather ∈ 白名单 且 visibility_m ≥ 阈值。
 *   2. 定位健康：fusion/localization 未 diverged 且 age < loc_max_age_ms。
 *   3. 有路由：navigation/path 在 route_max_age_ms 内到达。
 *   4. 系统健康：safety/evidence 的 degrade.level == 0。
 *
 * ODD 退出场景注入：读 scenario_file 的 `odd` 块；到 `visibility_drop_at_s` 后用
 * `degraded_visibility_m` **覆盖** environment/state 的能见度（模拟突起大雾），
 * 让分类器**真的**判 out——而非直接强制 out_odd（保持分类器诚实）。
 *
 * 时间基取**仿真钟**（vehicle/state 的 t_us），非墙钟（沿用评估器时间基修复纪律）。
 */
#include "node_plugin.h"
#include "clock_service.h"
#include "transport.h"
#include "discovery.h"
#include "logger.h"
#include "topic_registry.h"
#include <cjson/cJSON.h>

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ODD_MAX_VIOLATIONS 8

static struct {
    Transport* transport;
    DiscoveryManager* discovery;
    Scheduler* scheduler;
    double frequency_hz;

    /* 参数 */
    double odd_min_visibility_m;
    double loc_max_age_ms;
    double route_max_age_ms;
    char   allowed_weather[128];   /* 逗号分隔白名单 */

    /* 场景注入 */
    char   scenario_file[512];
    double vis_drop_at_s;          /* <0 = 不注入 */
    double degraded_visibility_m;

    /* 输入快照 */
    volatile int    has_env;
    char   weather[32];
    double visibility_m;
    volatile int    has_loc;
    int    loc_diverged;
    double loc_ts_us;
    volatile int    has_route;
    double route_ts_us;
    volatile int    has_evidence;
    int    degrade_level;
    double sim_t_us;               /* 仿真钟（vehicle/state.t_us）*/
    volatile uint64_t last_publish_us;

    TaskBase taskbase;             /* 托管模式：node_start_managed 派生线程 */
} g;

/* ───────────────────────── 场景注入读取 ───────────────────────── */
static void load_scenario_odd(const char* path) {
    g.vis_drop_at_s = -1.0;
    if (!path || !path[0]) return;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (16 * 1024 * 1024)) { fclose(f); return; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;
    cJSON* odd = cJSON_GetObjectItemCaseSensitive(root, "odd");
    if (cJSON_IsObject(odd)) {
        cJSON* j;
        if ((j = cJSON_GetObjectItemCaseSensitive(odd, "visibility_drop_at_s")) && cJSON_IsNumber(j))
            g.vis_drop_at_s = j->valuedouble;
        if ((j = cJSON_GetObjectItemCaseSensitive(odd, "degraded_visibility_m")) && cJSON_IsNumber(j))
            g.degraded_visibility_m = j->valuedouble;
    }
    cJSON_Delete(root);
}

/* ───────────────────────── 订阅回调 ───────────────────────── */
static void on_environment_state(const Message* msg, void* ud) {
    (void)ud;
    if (!msg) return;
    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;
    cJSON* w = cJSON_GetObjectItemCaseSensitive(root, "weather");
    if (cJSON_IsString(w)) {
        snprintf(g.weather, sizeof(g.weather), "%s", w->valuestring);
        g.has_env = 1;
    }
    cJSON* v = cJSON_GetObjectItemCaseSensitive(root, "visibility_m");
    if (cJSON_IsNumber(v)) { g.visibility_m = v->valuedouble; g.has_env = 1; }
    cJSON_Delete(root);
}

static void on_vehicle_state(const Message* msg, void* ud) {
    (void)ud;
    if (!msg) return;
    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;
    cJSON* j = cJSON_GetObjectItemCaseSensitive(root, "t_us");
    if (cJSON_IsNumber(j)) g.sim_t_us = j->valuedouble;
    cJSON_Delete(root);
}

static void on_localization(const Message* msg, void* ud) {
    (void)ud;
    if (!msg) return;
    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;
    cJSON* d = cJSON_GetObjectItemCaseSensitive(root, "diverged");
    if (cJSON_IsBool(d)) g.loc_diverged = cJSON_IsTrue(d) ? 1 : 0;
    cJSON* t = cJSON_GetObjectItemCaseSensitive(root, "timestamp_us");
    if (cJSON_IsNumber(t)) { g.loc_ts_us = t->valuedouble; g.has_loc = 1; }
    cJSON_Delete(root);
}

static void on_navigation_path(const Message* msg, void* ud) {
    (void)ud;
    if (!msg) return;
    g.route_ts_us = (double)clock_now_us();
    g.has_route = 1;
}

static void on_safety_evidence(const Message* msg, void* ud) {
    (void)ud;
    if (!msg) return;
    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;
    cJSON* deg = cJSON_GetObjectItemCaseSensitive(root, "degrade");
    if (cJSON_IsObject(deg)) {
        cJSON* lv = cJSON_GetObjectItemCaseSensitive(deg, "level");
        if (cJSON_IsNumber(lv)) { g.degrade_level = lv->valueint; g.has_evidence = 1; }
    }
    cJSON_Delete(root);
}

/* ───────────────────────── 分类 + 发布 ───────────────────────── */
static int weather_allowed(const char* w) {
    /* 白名单逗号分隔；空 = 全放行 */
    if (!g.allowed_weather[0]) return 1;
    char buf[sizeof(g.allowed_weather)];
    snprintf(buf, sizeof(buf), "%s", g.allowed_weather);
    char* save = NULL;
    for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        size_t n = strlen(tok);
        while (n && tok[n - 1] == ' ') tok[--n] = '\0';
        if (strcmp(tok, w) == 0) return 1;
    }
    return 0;
}

static void odd_publish(void) {
    double now_us = (double)clock_now_us();
    double sim_s = g.sim_t_us / 1e6;

    /* 场景注入：到点后用降级能见度覆盖（模拟突起大雾）*/
    double eff_visibility = g.visibility_m;
    if (g.vis_drop_at_s >= 0.0 && sim_s >= g.vis_drop_at_s)
        eff_visibility = g.degraded_visibility_m;

    const char* viols[ODD_MAX_VIOLATIONS];
    int nv = 0;
    if (!weather_allowed(g.weather))                 viols[nv++] = "weather";
    if (eff_visibility < g.odd_min_visibility_m)     viols[nv++] = "visibility";
    if (g.loc_diverged)                              viols[nv++] = "loc_diverged";
    double loc_age_ms = g.has_loc ? (now_us - g.loc_ts_us) / 1000.0 : 1e9;
    if (loc_age_ms > g.loc_max_age_ms)               viols[nv++] = "loc_stale";
    double route_age_ms = g.has_route ? (now_us - g.route_ts_us) / 1000.0 : 1e9;
    if (route_age_ms > g.route_max_age_ms)           viols[nv++] = "route_stale";
    if (g.degrade_level != 0)                        viols[nv++] = "degraded";
    int in_odd = (nv == 0);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "in_odd", in_odd);
    cJSON* arr = cJSON_AddArrayToObject(root, "violations");
    for (int i = 0; i < nv; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(viols[i]));
    cJSON_AddNumberToObject(root, "visibility_m", eff_visibility);
    cJSON_AddStringToObject(root, "weather", g.weather);
    cJSON_AddNumberToObject(root, "loc_age_ms", loc_age_ms);
    cJSON_AddBoolToObject(root, "loc_diverged", g.loc_diverged);
    cJSON_AddNumberToObject(root, "route_age_ms", route_age_ms);
    cJSON_AddNumberToObject(root, "degrade_level", g.degrade_level);
    cJSON_AddNumberToObject(root, "t_s", sim_s);
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) {
        transport_publish(g.transport, TOPIC_ODD_STATE, (const uint8_t*)s, (uint32_t)strlen(s) + 1);
        free(s);
    }
    g.last_publish_us = (uint64_t)now_us;
}

/* ───────────────────────── 执行（托管任务）───────────────────────── */
static int odd_monitor_execute(TaskBase* task) {
    pthread_setname_np(pthread_self(), "odd_monitor");
    long period_us = (g.frequency_hz > 0.0) ? (long)(1000000.0 / g.frequency_hz) : 100000L;
    while (!task->should_stop) {
        usleep((unsigned long)period_us);
        if (task->should_stop) break;
        odd_publish();
    }
    return 0;
}

static const TaskInterface odd_monitor_vtable = { .execute = odd_monitor_execute };

/* ───────────────────────── NodePlugin lifecycle ───────────────────────── */
static const char* s_inputs[] = {
    "environment/state", TOPIC_VEHICLE_STATE, TOPIC_FUSION_LOCALIZATION,
    TOPIC_NAVIGATION_PATH, "safety/evidence", NULL,
};
static const char* s_outputs[] = { TOPIC_ODD_STATE, NULL };

static NodePlugin s_plugin;

static int odd_monitor_init(MessageBus* bus, Transport* transport,
                            DiscoveryManager* discovery, Scheduler* scheduler,
                            const char* params_json) {
    (void)bus;
    memset(&g, 0, sizeof(g));
    g.transport = transport;
    g.discovery = discovery;
    g.scheduler = scheduler;
    g.frequency_hz = 10.0;
    g.odd_min_visibility_m = 150.0;
    g.loc_max_age_ms = 500.0;
    g.route_max_age_ms = 1000.0;
    snprintf(g.allowed_weather, sizeof(g.allowed_weather), "clear,overcast,cloudy");
    g.vis_drop_at_s = -1.0;
    g.degraded_visibility_m = 80.0;

    if (params_json) {
        cJSON* p = cJSON_Parse(params_json);
        if (p) {
            cJSON* j;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "frequency_hz")) && cJSON_IsNumber(j))
                g.frequency_hz = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "odd_min_visibility_m")) && cJSON_IsNumber(j))
                g.odd_min_visibility_m = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "loc_max_age_ms")) && cJSON_IsNumber(j))
                g.loc_max_age_ms = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "route_max_age_ms")) && cJSON_IsNumber(j))
                g.route_max_age_ms = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "allowed_weather")) && cJSON_IsString(j))
                snprintf(g.allowed_weather, sizeof(g.allowed_weather), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "scenario_file")) && cJSON_IsString(j))
                snprintf(g.scenario_file, sizeof(g.scenario_file), "%s", j->valuestring);
            cJSON_Delete(p);
        }
    }
    load_scenario_odd(g.scenario_file);

    transport_subscribe(transport, "environment/state", on_environment_state, NULL);
    transport_subscribe(transport, TOPIC_VEHICLE_STATE, on_vehicle_state, NULL);
    transport_subscribe(transport, TOPIC_FUSION_LOCALIZATION, on_localization, NULL);
    transport_subscribe(transport, TOPIC_NAVIGATION_PATH, on_navigation_path, NULL);
    transport_subscribe(transport, "safety/evidence", on_safety_evidence, NULL);

    transport_advertise(transport, TOPIC_ODD_STATE, 0u);
    discovery_advertise(discovery, TOPIC_ODD_STATE, 0u, CAP_PUBLISHER, g.frequency_hz);

    TaskConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.name, sizeof(cfg.name), "odd_monitor");
    cfg.priority         = TASK_PRIORITY_NORMAL;
    cfg.max_frequency_hz = g.frequency_hz;
    cfg.enable_stats     = true;
    if (task_base_init(&g.taskbase, &odd_monitor_vtable, &cfg) != 0) {
        LOG_WARN("odd_monitor", "task_base_init failed");
        return -1;
    }
    LOG_INFO("odd_monitor", "initialized (%.1f Hz, vis_floor=%.0fm, drop_at=%.1fs)",
             g.frequency_hz, g.odd_min_visibility_m, g.vis_drop_at_s);
    return 0;
}

static int odd_monitor_start(void) {
    int rc = node_start_managed(&s_plugin, g.scheduler);
    if (rc != 0) { LOG_WARN("odd_monitor", "node_start_managed failed: %d", rc); return rc; }
    LOG_INFO("odd_monitor", "started (managed)");
    node_announce_self(g.transport, &s_plugin);
    return 0;
}

static void odd_monitor_stop(void) { task_stop(&g.taskbase); }
static void odd_monitor_cleanup(void) { task_stop(&g.taskbase); task_base_destroy(&g.taskbase); }
static int odd_monitor_health(void) { return g.has_env ? 0 : 1; }

static NodePlugin s_plugin = {
    .api_version   = NODE_PLUGIN_API_VERSION,
    .name          = "odd_monitor",
    .version       = "1.0.0",
    .description   = "ODD (Operational Design Domain) monitor (L3-P1)",
    .input_topics  = s_inputs,
    .output_topics = s_outputs,
    .init          = odd_monitor_init,
    .start         = odd_monitor_start,
    .stop          = odd_monitor_stop,
    .cleanup       = odd_monitor_cleanup,
    .health        = odd_monitor_health,
    .taskbase      = &g.taskbase,
};

NodePlugin* node_get_plugin(void) { return &s_plugin; }
