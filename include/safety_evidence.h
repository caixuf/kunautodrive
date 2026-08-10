#ifndef SAFETY_EVIDENCE_H
#define SAFETY_EVIDENCE_H

/*
 * Machine-readable evidence for a safety fault and the resulting action.
 * The returned JSON is allocated by cJSON; release it with cJSON_free().
 */

#include "degrade_ladder.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* fault_id;
    const char* fault_type;
    const char* component;
    bool injected;
    uint64_t injected_at_us;
    uint64_t detected_at_us;
    uint64_t last_input_age_us;
    DegradeAction action;
    double command_throttle;
    double command_brake;
    double command_steer;
} SafetyEvidence;

char* safety_evidence_to_json(const SafetyEvidence* evidence);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_EVIDENCE_H */
