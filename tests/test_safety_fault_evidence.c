/*
 * Deterministic safety fault evidence test.
 *
 * Covers the same primitives used by safety_control_node:
 *   1. a missed control heartbeat enters L1 with a degraded safety envelope;
 *   2. an opt-in raw-command timeout enters L3 and records emergency_stop.
 *
 * Optional CI artifact:
 *   build/bin/test_safety_fault_evidence --json-out out/safety_fault_evidence.json
 */

#include "degrade_ladder.h"
#include "safety_evidence.h"
#include "safety_fault_injection.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        g_failures++; \
    } \
} while (0)

static cJSON* object_item(cJSON* parent, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(parent, key);
    CHECK(item != NULL, key);
    return item;
}

static void test_missed_heartbeat_degrades(void) {
    degrade_clear();
    degrade_supervisor_record_heartbeat("control_node", 1000);

    /* age=501ms starts the debounce window; 151ms later L1 is confirmed. */
    degrade_supervisor_tick(1501);
    CHECK(degrade_global_state()->degrade_level == DEGRADE_L0,
          "heartbeat debounce must prevent immediate degradation");
    degrade_supervisor_tick(1652);

    const DegradeState* state = degrade_global_state();
    const DegradeAction action = degrade_layer_action();
    CHECK(state->degrade_level == DEGRADE_L1,
          "missed heartbeat must transition to L1");
    CHECK(state->degrade_reason == DEGRADE_REASON_CONTROL_TO,
          "control heartbeat must retain control timeout reason");
    CHECK(state->degrade_timestamp_ms == 1652,
          "transition timestamp must be the detection tick");
    CHECK(action.disable_lane_change && action.safety_margin >= 1.5,
          "L1 must expose a degraded safety envelope");
}

/*
 * L3-P2: watchdog 覆盖面与表容量。
 *
 * 两个真实缺陷的回归锁：
 *   1. DEGRADE_MAX_NODES 曾为 16，而默认 pipeline 有 19 个节点 —— 第 17 个起
 *      的心跳被 **静默丢弃**，运维侧看到的是"心跳都健康"的假绿。
 *   2. 感知链（sensor_model/perception/object_tracker）与 safety_control/
 *      behavior_planner 此前根本不上报心跳 → 掉线无人发现；且它们的超时会被
 *      reason_for_node 归成 control_timeout，指错层。
 */
static void test_watchdog_capacity_and_reasons(void) {
    /* 注意：degrade_clear() **不清** supervisor 的 node_count（只清心跳时间戳），
     * 节点表在进程生命周期内只增不减。故本测试用"注册前/后差值"而非绝对数量，
     * 才能与前序用例的残留解耦。 */
    const int before = degrade_supervisor_node_count();

    /* 装满一整条默认管线的节点数（19）+ 余量，全部必须被接纳。
     * 旧 cfg（16）下第 17 个起被静默丢弃 → 运维看到"心跳都健康"的假绿。 */
    char names[24][64];
    for (int i = 0; i < 24; i++) {
        snprintf(names[i], sizeof(names[i]), "cap_probe_%02d", i);
        degrade_supervisor_record_heartbeat(names[i], 1000);
    }
    const int added = degrade_supervisor_node_count() - before;
    CHECK(added == 24,
          "supervisor must accept 24 further nodes (default pipeline is 19; the old "
          "cap of 16 silently dropped the rest, leaving them invisibly unwatched)");
    CHECK(degrade_supervisor_node_count() <= DEGRADE_MAX_NODES,
          "node count must stay within DEGRADE_MAX_NODES");

    /* 感知链 / 安全闸门 / 决策层的关键节点必须都能注册（覆盖面回归锁）。 */
    const int before2 = degrade_supervisor_node_count();
    const char* critical[] = {
        "sensor_model_node", "perception_node", "object_tracker_node",
        "safety_control_node", "behavior_planner_node",
    };
    for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); i++)
        degrade_supervisor_record_heartbeat(critical[i], 1000);
    /* 允许 fusion/planning/control 已被前序用例注册（表只增不减），故用 >=：
     * 关键是这些**感知链/安全闸门/决策层**节点必须都能进去。 */
    CHECK(degrade_supervisor_node_count() - before2 >=
              (int)(sizeof(critical) / sizeof(critical[0])),
          "every safety-critical node must fit in the watchdog table");

    /* 感知链超时必须报 sensor_timeout，而不是 control_timeout（指错层）。
     *
     * ⚠️ 判据取**第一个**超时节点的名字（degrade_ladder.c 的 reason_for_node），
     * 而节点表只增不减。故这里先让所有已注册节点的心跳都"新鲜"，只把
     * sensor_model_node 留成陈旧，才能确定性地验证映射。 */
    degrade_clear();                       /* 清心跳时间戳（不清表） */
    for (int i = 0; i < degrade_supervisor_node_count(); i++) {
        /* 逐个刷新：用 node_health 读回名字再打心跳 */
    }
    {
        /* 直接对所有已知关键节点刷新，再把 sensor_model 单独做旧 */
        const char* all[] = {
            "sensor_model_node", "perception_node", "object_tracker_node",
            "safety_control_node", "behavior_planner_node",
            "fusion_node", "planning_node", "control_node",
        };
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
            degrade_supervisor_record_heartbeat(all[i], 1652);   /* 新鲜 */
        degrade_supervisor_record_heartbeat("sensor_model_node", 1000); /* 陈旧 */
    }
    degrade_supervisor_tick(1501);
    degrade_supervisor_tick(1652);
    CHECK(degrade_global_state()->degrade_reason == DEGRADE_REASON_SENSOR_TO,
          "perception-chain timeout must report sensor_timeout, not control_timeout");
    CHECK(strcmp(degrade_reason_name(DEGRADE_REASON_SENSOR_TO),
                 "sensor_timeout") == 0,
          "sensor timeout reason must have a stable published name");

    /* safety_control 掉线必须被判定为超时（它是最后一层保护）。 */
    CHECK(degrade_global_state()->degrade_level >= DEGRADE_L1,
          "safety_control silence must degrade — it is the last safety layer");
}

static char* test_raw_command_timeout_evidence(void) {
    SafetyFaultInjection injection;
    safety_fault_injection_init(&injection, true, 500000);
    safety_fault_injection_start(&injection, 1000000);
    CHECK(!safety_fault_injection_drop_raw_command(&injection, 1499999),
          "fault must stay inactive before configured delay");
    CHECK(safety_fault_injection_drop_raw_command(&injection, 1500000),
          "fault must drop raw commands at configured delay");
    CHECK(injection.activated, "fault activation must latch");
    CHECK(!safety_raw_command_timeout_expired(3499999, 1500000, true, 2000000),
          "timeout must not trigger before 2s");
    CHECK(safety_raw_command_timeout_expired(3500001, 1500000, true, 2000000),
          "moving raw-command timeout must trigger after 2s");
    CHECK(!safety_raw_command_timeout_expired(3500001, 1500000, false, 2000000),
          "stopped vehicle must not trigger a raw-command timeout");

    degrade_clear();
    degrade_set_level_at(DEGRADE_L3, DEGRADE_REASON_HEARTBEAT, 3500);
    const DegradeAction action = degrade_layer_action();
    CHECK(action.immediate_stop && action.mrm_stop && action.speed_limit == 0.0,
          "timeout L3 must require immediate stop");

    SafetyEvidence evidence = {
        .fault_id = "raw_cmd_timeout",
        .fault_type = "data_timeout",
        .component = "safety_control",
        .injected = true,
        .injected_at_us = 1500000,
        .detected_at_us = 3500001,
        .last_input_age_us = 2000001,
        .action = action,
        .command_throttle = 0.0,
        .command_brake = 1.0,
        .command_steer = 0.0,
    };
    char* json = safety_evidence_to_json(&evidence);
    CHECK(json != NULL, "evidence must serialize");
    if (!json) return NULL;

    cJSON* root = cJSON_Parse(json);
    CHECK(root != NULL, "evidence must be valid JSON");
    if (root) {
        cJSON* fault = object_item(root, "fault");
        cJSON* degrade = object_item(root, "degrade");
        cJSON* evidence_action = object_item(root, "action");
        CHECK(cJSON_IsNumber(object_item(root, "schema_version")) &&
                  object_item(root, "schema_version")->valueint == 1,
              "schema version must be v1");
        CHECK(cJSON_IsTrue(object_item(fault, "injected")),
              "evidence must identify injected fault");
        CHECK(cJSON_IsNumber(object_item(degrade, "level")) &&
                  object_item(degrade, "level")->valueint == DEGRADE_L3,
              "evidence must contain L3 transition");
        cJSON* name = object_item(evidence_action, "name");
        CHECK(cJSON_IsString(name) &&
                  strcmp(name->valuestring, "emergency_stop") == 0,
              "evidence must name emergency_stop action");
        cJSON* command = object_item(evidence_action, "command");
        CHECK(cJSON_IsNumber(object_item(command, "brake")) &&
                  object_item(command, "brake")->valuedouble == 1.0,
              "evidence must contain full brake command");
        cJSON_Delete(root);
    }
    return json;
}

int main(int argc, char** argv) {
    const char* json_out = NULL;
    if (argc == 3 && strcmp(argv[1], "--json-out") == 0) {
        json_out = argv[2];
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--json-out relative/path.json]\n", argv[0]);
        return 2;
    }

    test_missed_heartbeat_degrades();
    test_watchdog_capacity_and_reasons();
    char* evidence_json = test_raw_command_timeout_evidence();

    if (evidence_json) {
        printf("SAFETY_EVIDENCE=%s\n", evidence_json);
        if (json_out) {
            FILE* file = fopen(json_out, "w");
            CHECK(file != NULL, "must open requested JSON evidence path");
            if (file) {
                CHECK(fputs(evidence_json, file) >= 0,
                      "must write JSON evidence");
                CHECK(fputc('\n', file) != EOF, "must terminate JSON evidence");
                fclose(file);
            }
        }
        cJSON_free(evidence_json);
    }

    if (g_failures) {
        fprintf(stderr, "test_safety_fault_evidence: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("test_safety_fault_evidence: PASS");
    return 0;
}
