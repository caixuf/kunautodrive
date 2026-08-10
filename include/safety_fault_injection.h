#ifndef SAFETY_FAULT_INJECTION_H
#define SAFETY_FAULT_INJECTION_H

/*
 * Deterministic, opt-in raw-command timeout injection for safety-path tests.
 * It is inert unless a node explicitly enables it in its configuration.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    bool activated;
    uint64_t start_us;
    uint64_t activate_after_us;
} SafetyFaultInjection;

void safety_fault_injection_init(SafetyFaultInjection* injection,
                                 bool enabled,
                                 uint64_t activate_after_us);

void safety_fault_injection_start(SafetyFaultInjection* injection,
                                  uint64_t now_us);

/* Returns true once activation time has passed; callers must drop raw commands. */
bool safety_fault_injection_drop_raw_command(SafetyFaultInjection* injection,
                                             uint64_t now_us);

/* A moving vehicle with no raw command past timeout requires a safety action. */
bool safety_raw_command_timeout_expired(uint64_t now_us,
                                        uint64_t last_raw_command_us,
                                        bool vehicle_moving,
                                        uint64_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_FAULT_INJECTION_H */
