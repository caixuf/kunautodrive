/*
 * L3 EDR 分类实现 — 见 include/l3_edr.h。
 * 与 tools/l3_edr_sim.py 的 EDR_TAXONOMY 逐条对应。
 */

#include "l3_edr.h"

#include <string.h>

int l3_edr_tor_state_code(const char* state_name) {
    if (!state_name) return -1;
    if (strcmp(state_name, "OFF") == 0)             return L3_EDR_TOR_OFF;
    if (strcmp(state_name, "ACTIVE") == 0)          return L3_EDR_TOR_ACTIVE;
    if (strcmp(state_name, "TOR_REQUESTED") == 0)   return L3_EDR_TOR_REQUESTED;
    if (strcmp(state_name, "DRIVER_TAKEOVER") == 0) return L3_EDR_TOR_DRIVER_TAKEOVER;
    if (strcmp(state_name, "MRM") == 0)             return L3_EDR_TOR_MRM;
    return -1;
}

void l3_edr_values_init(double values[8]) {
    if (!values) return;
    memset(values, 0, sizeof(double) * 8u);
}
