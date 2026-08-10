#include "safety_evidence.h"

#include <cjson/cJSON.h>

char* safety_evidence_to_json(const SafetyEvidence* evidence) {
    if (!evidence || !evidence->fault_id || !evidence->fault_type ||
        !evidence->component) {
        return NULL;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "evidence_type", "safety_fault");

    cJSON* fault = cJSON_AddObjectToObject(root, "fault");
    cJSON_AddStringToObject(fault, "id", evidence->fault_id);
    cJSON_AddStringToObject(fault, "type", evidence->fault_type);
    cJSON_AddStringToObject(fault, "component", evidence->component);
    cJSON_AddBoolToObject(fault, "injected", evidence->injected);
    cJSON_AddNumberToObject(fault, "injected_at_us",
                            (double)evidence->injected_at_us);
    cJSON_AddNumberToObject(fault, "detected_at_us",
                            (double)evidence->detected_at_us);
    cJSON_AddNumberToObject(fault, "last_input_age_ms",
                            (double)evidence->last_input_age_us / 1000.0);

    cJSON* degrade = cJSON_AddObjectToObject(root, "degrade");
    cJSON_AddNumberToObject(degrade, "level", evidence->action.degrade_level);
    cJSON_AddNumberToObject(degrade, "reason", evidence->action.degrade_reason);

    cJSON* action = cJSON_AddObjectToObject(root, "action");
    const char* action_name = evidence->action.immediate_stop ? "emergency_stop" :
                              evidence->action.mrm_stop ? "minimum_risk_maneuver" :
                              evidence->action.disable_lane_change ? "degraded_envelope" :
                              "none";
    cJSON_AddStringToObject(action, "name", action_name);
    cJSON_AddBoolToObject(action, "immediate_stop",
                          evidence->action.immediate_stop);
    cJSON_AddBoolToObject(action, "mrm_stop", evidence->action.mrm_stop);
    cJSON_AddBoolToObject(action, "disable_lane_change",
                          evidence->action.disable_lane_change);
    cJSON_AddNumberToObject(action, "speed_limit_mps",
                            evidence->action.speed_limit);
    cJSON_AddNumberToObject(action, "safety_margin",
                            evidence->action.safety_margin);

    cJSON* command = cJSON_AddObjectToObject(action, "command");
    cJSON_AddNumberToObject(command, "throttle", evidence->command_throttle);
    cJSON_AddNumberToObject(command, "brake", evidence->command_brake);
    cJSON_AddNumberToObject(command, "steer", evidence->command_steer);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
