/* tor_manager_node.c — TOR（接管请求）状态机 + 内嵌 DMS 模型 + MRM 权威（L3-P1）。
 *
 * 职责（单一）：**离散决策**——"系统负责 / 请求接管 / 驾驶员接管 / 最小风险停车"
 * 的状态机。不发连续控制量（铁律：behavior 类节点只决定"做什么"）。MRM 通过
 * `safety/mrm_request` 请求，由 control（纵向减速）与 safety_control（制动下限+双闪）
 * 执行。
 *
 * TOR_TRANSITIONS 是 tools/tor_mrm_sim.py::TOR_TRANSITIONS 的**逐行镜像**（Python 是
 * 仿真单一事实源，`--check-tor-fsm` 已验可达性/无潜伏态）。
 *
 * ODD 退出场景：读 scenario_file 的 `driver` 块（attention/will_takeover/
 * takeover_latency_s）——接管 vs 超时进 MRM 的**唯一差异**在此。
 */
#include "node_plugin.h"
#include "clock_service.h"
#include "transport.h"
#include "discovery.h"
#include "logger.h"
#include "state_machine.h"
#include "topic_registry.h"
#include <cjson/cJSON.h>

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── TOR FSM（逐行镜像 tools/tor_mrm_sim.py）──────────────────────── */
enum {
    TOR_ST_OFF = 0, TOR_ST_ACTIVE, TOR_ST_TOR_REQUESTED,
    TOR_ST_DRIVER_TAKEOVER, TOR_ST_MRM,
};
enum {
    TOR_EV_ACTIVATE = 16, TOR_EV_ODD_EXIT, TOR_EV_ODD_REENTER,
    TOR_EV_DRIVER_TAKEOVER, TOR_EV_TOR_TIMEOUT, TOR_EV_SYSTEM_FAULT,
    TOR_EV_MRM_COMPLETE, TOR_EV_RESET,
};

static const TransitionRule TOR_TRANSITIONS[] = {
    { TOR_ST_OFF,             TOR_EV_ACTIVATE,        TOR_ST_ACTIVE,          "OFF + ACTIVATE -> ACTIVE", false },
    { TOR_ST_ACTIVE,          TOR_EV_ODD_EXIT,        TOR_ST_TOR_REQUESTED,   "ACTIVE + ODD_EXIT -> TOR_REQUESTED", false },
    { TOR_ST_ACTIVE,          TOR_EV_SYSTEM_FAULT,    TOR_ST_MRM,             "ACTIVE + SYSTEM_FAULT -> MRM", false },
    { TOR_ST_TOR_REQUESTED,   TOR_EV_DRIVER_TAKEOVER, TOR_ST_DRIVER_TAKEOVER, "TOR_REQUESTED + DRIVER_TAKEOVER -> DRIVER_TAKEOVER", false },
    { TOR_ST_TOR_REQUESTED,   TOR_EV_TOR_TIMEOUT,     TOR_ST_MRM,             "TOR_REQUESTED + TOR_TIMEOUT -> MRM", false },
    { TOR_ST_TOR_REQUESTED,   TOR_EV_ODD_REENTER,     TOR_ST_ACTIVE,          "TOR_REQUESTED + ODD_REENTER -> ACTIVE", false },
    { TOR_ST_TOR_REQUESTED,   TOR_EV_SYSTEM_FAULT,    TOR_ST_MRM,             "TOR_REQUESTED + SYSTEM_FAULT -> MRM", false },
    { TOR_ST_DRIVER_TAKEOVER, TOR_EV_RESET,           TOR_ST_ACTIVE,          "DRIVER_TAKEOVER + RESET -> ACTIVE", false },
    { TOR_ST_MRM,             TOR_EV_MRM_COMPLETE,    TOR_ST_OFF,             "MRM + MRM_COMPLETE -> OFF", false },
    { TOR_ST_MRM,             TOR_EV_RESET,           TOR_ST_ACTIVE,          "MRM + RESET -> ACTIVE", false },
    TRANSITION_TABLE_END,
};

static const char* tor_state_name(int s) {
    switch (s) {
        case TOR_ST_OFF: return "OFF";
        case TOR_ST_ACTIVE: return "ACTIVE";
        case TOR_ST_TOR_REQUESTED: return "TOR_REQUESTED";
        case TOR_ST_DRIVER_TAKEOVER: return "DRIVER_TAKEOVER";
        case TOR_ST_MRM: return "MRM";
        default: return "?";
    }
}

static struct {
    Transport* transport;
    DiscoveryManager* discovery;
    Scheduler* scheduler;
    double frequency_hz;

    ReflectiveStateMachine sm;

    /* 参数 */
    double tor_countdown_s;
    double tor_odd_exit_dwell_s;
    double tor_odd_reenter_dwell_s;
    double mrm_settle_s;
    double startup_grace_s;
    double mrm_decel_mps2;   /* 仅上报给消费方/调试 */
    char   scenario_file[512];

    /* DMS 内嵌模型 */
    int    dms_attention;
    int    dms_will_takeover;
    double dms_takeover_latency_s;

    /* 输入快照 */
    volatile int has_odd;
    int    odd_in;
    char   odd_reason[32];
    double sim_t_us;
    volatile int has_vehicle;
    double speed;
    volatile int has_system_fault;   /* degrade>0 视为系统故障（简化）*/

    /* 运行时 */
    double sim_t_s;            /* 上次处理的仿真时刻 */
    double out_dwell_s;
    double in_dwell_s;
    double t_tor_entry_s;
    double t_mrm_entry_s;
    double mrm_stop_hold_s;
    double since_start_s;
    int    activated;

    TaskBase taskbase;         /* 托管模式 */
} g;

/* ───────────────────────── 场景 driver 块 ───────────────────────── */
static void load_scenario_driver(const char* path) {
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
    cJSON* drv = cJSON_GetObjectItemCaseSensitive(root, "driver");
    if (cJSON_IsObject(drv)) {
        cJSON* j;
        if ((j = cJSON_GetObjectItemCaseSensitive(drv, "attention")) && cJSON_IsBool(j))
            g.dms_attention = cJSON_IsTrue(j) ? 1 : 0;
        if ((j = cJSON_GetObjectItemCaseSensitive(drv, "will_takeover")) && cJSON_IsBool(j))
            g.dms_will_takeover = cJSON_IsTrue(j) ? 1 : 0;
        if ((j = cJSON_GetObjectItemCaseSensitive(drv, "takeover_latency_s")) && cJSON_IsNumber(j))
            g.dms_takeover_latency_s = j->valuedouble;
    }
    cJSON_Delete(root);
}

/* ───────────────────────── 订阅回调 ───────────────────────── */
static void on_odd_state(const Message* msg, void* ud) {
    (void)ud;
    if (!msg) return;
    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;
    cJSON* in = cJSON_GetObjectItemCaseSensitive(root, "in_odd");
    if (cJSON_IsBool(in)) { g.odd_in = cJSON_IsTrue(in) ? 1 : 0; g.has_odd = 1; }
    cJSON* vs = cJSON_GetObjectItemCaseSensitive(root, "violations");
    if (cJSON_IsArray(vs) && cJSON_GetArraySize(vs) > 0) {
        cJSON* first = cJSON_GetArrayItem(vs, 0);
        if (cJSON_IsString(first)) snprintf(g.odd_reason, sizeof(g.odd_reason), "%s", first->valuestring);
    } else {
        g.odd_reason[0] = '\0';
    }
    cJSON* ts = cJSON_GetObjectItemCaseSensitive(root, "t_s");
    if (cJSON_IsNumber(ts)) g.sim_t_us = ts->valuedouble * 1e6;
    cJSON_Delete(root);
}

static void on_vehicle_state(const Message* msg, void* ud) {
    (void)ud;
    if (!msg) return;
    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;
    cJSON* j = cJSON_GetObjectItemCaseSensitive(root, "t_us");
    if (cJSON_IsNumber(j)) g.sim_t_us = j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(root, "speed");
    if (cJSON_IsNumber(j)) { g.speed = j->valuedouble; g.has_vehicle = 1; }
    cJSON_Delete(root);
}

static void on_safety_evidence(const Message* msg, void* ud) {
    (void)ud;
    if (!msg) return;
    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;
    cJSON* deg = cJSON_GetObjectItemCaseSensitive(root, "degrade");
    if (cJSON_IsObject(deg)) {
        cJSON* lv = cJSON_GetObjectItemCaseSensitive(deg, "level");
        if (cJSON_IsNumber(lv)) g.has_system_fault = (lv->valueint > 0) ? 1 : 0;
    }
    cJSON_Delete(root);
}

/* ───────────────────────── 发布 ───────────────────────── */
static void publish_mrm(void) {
    int active = (statem_current(&g.sm) == TOR_ST_MRM);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", active);
    cJSON_AddNumberToObject(root, "target_speed", 0.0);
    cJSON_AddBoolToObject(root, "hold", active);
    cJSON_AddStringToObject(root, "reason", active ? (g.odd_reason[0] ? g.odd_reason : "system_fault") : "");
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) { transport_publish(g.transport, TOPIC_SAFETY_MRM, (const uint8_t*)s, (uint32_t)strlen(s) + 1); free(s); }
}

static void publish_tor(void) {
    int st = statem_current(&g.sm);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", tor_state_name(st));
    double cd = 0.0;
    if (st == TOR_ST_TOR_REQUESTED) cd = g.tor_countdown_s - (g.sim_t_s - g.t_tor_entry_s);
    cJSON_AddNumberToObject(root, "countdown_s", cd > 0.0 ? cd : 0.0);
    cJSON_AddBoolToObject(root, "odd_in", g.odd_in);
    cJSON_AddStringToObject(root, "reason", g.odd_reason[0] ? g.odd_reason : (st == TOR_ST_ACTIVE ? "" : "odd"));
    cJSON* dms = cJSON_AddObjectToObject(root, "dms");
    cJSON_AddBoolToObject(dms, "attention", g.dms_attention);
    cJSON_AddBoolToObject(dms, "will_takeover", g.dms_will_takeover);
    cJSON_AddNumberToObject(dms, "takeover_latency_s", g.dms_takeover_latency_s);
    cJSON* mrm = cJSON_AddObjectToObject(root, "mrm");
    cJSON_AddBoolToObject(mrm, "active", st == TOR_ST_MRM);
    cJSON_AddNumberToObject(mrm, "target_speed", 0.0);
    cJSON_AddNumberToObject(root, "speed", g.speed);
    cJSON_AddNumberToObject(root, "t_s", g.sim_t_s);
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) { transport_publish(g.transport, TOPIC_TOR_STATE, (const uint8_t*)s, (uint32_t)strlen(s) + 1); free(s); }
}

static void send_ev(int ev) { (void)statem_send_event(&g.sm, ev, NULL); }

/* ───────────────────────── 执行 ───────────────────────── */
static void tor_manager_tick(void) {
    double t_s = g.sim_t_us / 1e6;
    double dt = (g.sim_t_s > 0.0) ? (t_s - g.sim_t_s) : 0.05;
    if (dt <= 0.0 || dt > 1.0) dt = 0.05;
    g.sim_t_s = t_s;
    g.since_start_s += dt;

    /* 去抖累计 */
    if (g.has_odd && !g.odd_in) g.out_dwell_s += dt; else g.out_dwell_s = 0.0;
    if (g.has_odd && g.odd_in)  g.in_dwell_s += dt;  else g.in_dwell_s = 0.0;

    int st = statem_current(&g.sm);

    /* 启动宽限后激活一次 */
    if (st == TOR_ST_OFF && !g.activated && g.since_start_s >= g.startup_grace_s) {
        send_ev(TOR_EV_ACTIVATE); g.activated = 1;
    }
    st = statem_current(&g.sm);

    if (st == TOR_ST_ACTIVE) {
        if (g.has_system_fault) send_ev(TOR_EV_SYSTEM_FAULT);
        else if (g.out_dwell_s >= g.tor_odd_exit_dwell_s) {
            send_ev(TOR_EV_ODD_EXIT); g.t_tor_entry_s = t_s;
        }
    } else if (st == TOR_ST_TOR_REQUESTED) {
        if (g.has_system_fault) {
            send_ev(TOR_EV_SYSTEM_FAULT);
        } else if (g.in_dwell_s >= g.tor_odd_reenter_dwell_s) {
            send_ev(TOR_EV_ODD_REENTER);
        } else {
            double in_tor = t_s - g.t_tor_entry_s;
            int takeover = g.dms_will_takeover && g.dms_attention && (in_tor >= g.dms_takeover_latency_s);
            if (takeover) send_ev(TOR_EV_DRIVER_TAKEOVER);
            else if (in_tor >= g.tor_countdown_s) send_ev(TOR_EV_TOR_TIMEOUT);
        }
    } else if (st == TOR_ST_MRM) {
        if (g.t_mrm_entry_s <= 0.0) { g.t_mrm_entry_s = t_s; g.mrm_stop_hold_s = 0.0; }
        if (fabs(g.speed) < 0.1) g.mrm_stop_hold_s += dt; else g.mrm_stop_hold_s = 0.0;
        if (g.mrm_stop_hold_s >= g.mrm_settle_s) { send_ev(TOR_EV_MRM_COMPLETE); g.t_mrm_entry_s = 0.0; }
    } else if (st == TOR_ST_DRIVER_TAKEOVER) {
        /* 驾驶员接管后，ODD 恢复即可复位（简化：接管即视为已退出 TOR）*/
        if (g.in_dwell_s >= g.tor_odd_reenter_dwell_s) send_ev(TOR_EV_RESET);
    }

    publish_mrm();
    publish_tor();
}

static int tor_manager_execute(TaskBase* task) {
    pthread_setname_np(pthread_self(), "tor_manager");
    long period_us = (g.frequency_hz > 0.0) ? (long)(1000000.0 / g.frequency_hz) : 50000L;
    while (!task->should_stop) {
        usleep((unsigned long)period_us);
        if (task->should_stop) break;
        tor_manager_tick();
    }
    return 0;
}

static const TaskInterface tor_manager_vtable = { .execute = tor_manager_execute };

/* ───────────────────────── NodePlugin lifecycle ───────────────────────── */
static const char* s_inputs[] = {
    TOPIC_ODD_STATE, TOPIC_VEHICLE_STATE, "safety/evidence", NULL,
};
static const char* s_outputs[] = { TOPIC_TOR_STATE, TOPIC_SAFETY_MRM, NULL };

static NodePlugin s_plugin;

static int tor_manager_init(MessageBus* bus, Transport* transport,
                            DiscoveryManager* discovery, Scheduler* scheduler,
                            const char* params_json) {
    (void)bus;
    memset(&g, 0, sizeof(g));
    g.transport = transport;
    g.discovery = discovery;
    g.scheduler = scheduler;
    g.frequency_hz = 20.0;
    g.tor_countdown_s = 10.0;
    g.tor_odd_exit_dwell_s = 0.5;
    g.tor_odd_reenter_dwell_s = 1.0;
    g.mrm_settle_s = 1.0;
    g.startup_grace_s = 2.0;
    g.mrm_decel_mps2 = 1.5;
    g.dms_attention = 1;
    g.dms_will_takeover = 1;
    g.dms_takeover_latency_s = 1.5;

    if (params_json) {
        cJSON* p = cJSON_Parse(params_json);
        if (p) {
            cJSON* j;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "frequency_hz")) && cJSON_IsNumber(j))
                g.frequency_hz = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "tor_countdown_s")) && cJSON_IsNumber(j))
                g.tor_countdown_s = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "tor_odd_exit_dwell_s")) && cJSON_IsNumber(j))
                g.tor_odd_exit_dwell_s = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "tor_odd_reenter_dwell_s")) && cJSON_IsNumber(j))
                g.tor_odd_reenter_dwell_s = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "mrm_settle_s")) && cJSON_IsNumber(j))
                g.mrm_settle_s = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "startup_grace_s")) && cJSON_IsNumber(j))
                g.startup_grace_s = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "mrm_decel_mps2")) && cJSON_IsNumber(j))
                g.mrm_decel_mps2 = j->valuedouble;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "scenario_file")) && cJSON_IsString(j))
                snprintf(g.scenario_file, sizeof(g.scenario_file), "%s", j->valuestring);
            cJSON_Delete(p);
        }
    }
    load_scenario_driver(g.scenario_file);

    statem_init(&g.sm, TOR_TRANSITIONS, TOR_ST_OFF, "tor_manager");

    transport_subscribe(transport, TOPIC_ODD_STATE, on_odd_state, NULL);
    transport_subscribe(transport, TOPIC_VEHICLE_STATE, on_vehicle_state, NULL);
    transport_subscribe(transport, "safety/evidence", on_safety_evidence, NULL);

    transport_advertise(transport, TOPIC_TOR_STATE, 0u);
    transport_advertise(transport, TOPIC_SAFETY_MRM, 0u);
    discovery_advertise(discovery, TOPIC_TOR_STATE, 0u, CAP_PUBLISHER, g.frequency_hz);
    discovery_advertise(discovery, TOPIC_SAFETY_MRM, 0u, CAP_PUBLISHER, g.frequency_hz);

    TaskConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.name, sizeof(cfg.name), "tor_manager");
    cfg.priority         = TASK_PRIORITY_HIGH;
    cfg.max_frequency_hz = g.frequency_hz;
    cfg.enable_stats     = true;
    if (task_base_init(&g.taskbase, &tor_manager_vtable, &cfg) != 0) {
        LOG_WARN("tor_manager", "task_base_init failed");
        return -1;
    }
    LOG_INFO("tor_manager", "initialized (%.1f Hz, countdown=%.0fs, will_takeover=%d, latency=%.1fs)",
             g.frequency_hz, g.tor_countdown_s, g.dms_will_takeover, g.dms_takeover_latency_s);
    return 0;
}

static int tor_manager_start(void) {
    int rc = node_start_managed(&s_plugin, g.scheduler);
    if (rc != 0) { LOG_WARN("tor_manager", "node_start_managed failed: %d", rc); return rc; }
    LOG_INFO("tor_manager", "started (managed)");
    node_announce_self(g.transport, &s_plugin);
    return 0;
}

static void tor_manager_stop(void) { task_stop(&g.taskbase); }
static void tor_manager_cleanup(void) { task_stop(&g.taskbase); task_base_destroy(&g.taskbase); }
static int tor_manager_health(void) { return g.has_odd ? 0 : 1; }

static NodePlugin s_plugin = {
    .api_version   = NODE_PLUGIN_API_VERSION,
    .name          = "tor_manager",
    .version       = "1.0.0",
    .description   = "TOR state machine + DMS model + MRM authority (L3-P1)",
    .input_topics  = s_inputs,
    .output_topics = s_outputs,
    .init          = tor_manager_init,
    .start         = tor_manager_start,
    .stop          = tor_manager_stop,
    .cleanup       = tor_manager_cleanup,
    .health        = tor_manager_health,
    .taskbase      = &g.taskbase,
};

NodePlugin* node_get_plugin(void) { return &s_plugin; }
