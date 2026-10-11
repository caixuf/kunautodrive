/*
 * L3 EDR 分类单测（L3-P2 方向六）。
 *
 * 覆盖 include/l3_edr.h 的契约，与 tools/l3_edr_sim.py 的 --run-all 对应：
 *   1. TOR 状态字符串 → 稳定数值（未知必须返回 -1，不得静默当成 OFF）；
 *   2. 字段填充辅助清零全部 8 槽（未用槽显式为 0，避免"含义靠猜"）；
 *   3. 事件名统一 l3_ 前缀（便于 PEM dump 里 grep）；
 *   4. 碰撞窗口为正（法规取证要求前后可回溯）。
 */

#include "l3_edr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static void test_tor_state_codes(void) {
    CHECK(l3_edr_tor_state_code("OFF") == L3_EDR_TOR_OFF, "OFF maps to OFF code");
    CHECK(l3_edr_tor_state_code("ACTIVE") == L3_EDR_TOR_ACTIVE,
          "ACTIVE maps to ACTIVE code");
    CHECK(l3_edr_tor_state_code("TOR_REQUESTED") == L3_EDR_TOR_REQUESTED,
          "TOR_REQUESTED maps correctly");
    CHECK(l3_edr_tor_state_code("DRIVER_TAKEOVER") == L3_EDR_TOR_DRIVER_TAKEOVER,
          "DRIVER_TAKEOVER maps correctly");
    CHECK(l3_edr_tor_state_code("MRM") == L3_EDR_TOR_MRM, "MRM maps correctly");

    /* 未知/空一律 -1：不得静默当成 OFF（那会伪造一条"系统退出"记录）*/
    CHECK(l3_edr_tor_state_code("BOGUS") == -1, "unknown state must return -1");
    CHECK(l3_edr_tor_state_code("") == -1, "empty state must return -1");
    CHECK(l3_edr_tor_state_code(NULL) == -1, "NULL state must return -1");
    /* 大小写敏感：EDR 记录的是权威字符串，不做模糊匹配 */
    CHECK(l3_edr_tor_state_code("active") == -1, "state match must be case-sensitive");
}

static void test_values_init_zeroes_all_slots(void) {
    double v[8];
    for (int i = 0; i < 8; i++) v[i] = 123.0;
    l3_edr_values_init(v);
    for (int i = 0; i < 8; i++) {
        if (v[i] != 0.0) {
            CHECK(0, "l3_edr_values_init must zero every slot");
            return;
        }
    }
    CHECK(1, "all slots zeroed");
    /* NULL 必须安全（生产路径上字段可能未初始化） */
    l3_edr_values_init(NULL);
    CHECK(1, "NULL values array must not crash");
}

static void test_event_names_are_greppable(void) {
    const char* names[] = {
        L3_EDR_EV_ACTIVATION, L3_EDR_EV_TOR, L3_EDR_EV_MRM,
        L3_EDR_EV_ODD_EXIT, L3_EDR_EV_COLLISION,
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strncmp(names[i], "l3_", 3) != 0) {
            CHECK(0, "every L3 EDR event name must start with 'l3_'");
            return;
        }
    }
    CHECK(1, "all event names prefixed");
    /* 互不相同（否则 PEM 里无法区分）*/
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        for (size_t j = i + 1; j < sizeof(names) / sizeof(names[0]); j++)
            CHECK(strcmp(names[i], names[j]) != 0, "event names must be unique");
}

static void test_collision_windows_positive(void) {
    CHECK(L3_EDR_COLLISION_PRE_WINDOW_S > 0.0,
          "collision pre-window must be positive (forensic look-back)");
    CHECK(L3_EDR_COLLISION_POST_WINDOW_S > 0.0,
          "collision post-window must be positive (forensic look-ahead)");
}

int main(void) {
    test_tor_state_codes();
    test_values_init_zeroes_all_slots();
    test_event_names_are_greppable();
    test_collision_windows_positive();
    if (g_fail == 0) printf("l3_edr: all checks passed\n");
    return g_fail;
}
