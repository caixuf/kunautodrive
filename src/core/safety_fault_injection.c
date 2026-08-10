#include "safety_fault_injection.h"

#include <string.h>

void safety_fault_injection_init(SafetyFaultInjection* injection,
                                 bool enabled,
                                 uint64_t activate_after_us) {
    if (!injection) return;
    memset(injection, 0, sizeof(*injection));
    injection->enabled = enabled;
    injection->activate_after_us = activate_after_us;
}

void safety_fault_injection_start(SafetyFaultInjection* injection,
                                  uint64_t now_us) {
    if (!injection) return;
    injection->start_us = now_us;
}

bool safety_fault_injection_drop_raw_command(SafetyFaultInjection* injection,
                                             uint64_t now_us) {
    if (!injection || !injection->enabled || injection->start_us == 0) return false;
    if (now_us < injection->start_us ||
        now_us - injection->start_us < injection->activate_after_us) return false;
    injection->activated = true;
    return true;
}

bool safety_raw_command_timeout_expired(uint64_t now_us,
                                        uint64_t last_raw_command_us,
                                        bool vehicle_moving,
                                        uint64_t timeout_us) {
    return vehicle_moving && last_raw_command_us > 0 && now_us >= last_raw_command_us &&
           now_us - last_raw_command_us > timeout_us;
}
