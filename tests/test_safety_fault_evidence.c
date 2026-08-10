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
