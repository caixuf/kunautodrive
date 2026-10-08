#ifndef SAFETY_EVIDENCE_H
#define SAFETY_EVIDENCE_H

/*
 * Machine-readable evidence for a safety fault and the resulting action.
 * The returned JSON is allocated by cJSON; release it with cJSON_free().
 *
 * Two record kinds share this struct, distinguished by `periodic`:
 *   - fault-triggered (periodic=false, default): emitted when a real fault is
 *     injected/detected. `fault.id`/`type` describe the fault; `injected`
 *     says whether it came from the test fault injector.
 *   - periodic state snapshot (periodic=true): emitted continuously by
 *     safety_control so every evaluation run carries evidence of the current
 *     degrade/action state, even with no fault. `fault_id` must be "none",
 *     `injected` is false, and injected_at_us/detected_at_us are 0.
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
    bool periodic;          /* true = 周期状态快照（非故障），fault_id 须为 "none" */
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
