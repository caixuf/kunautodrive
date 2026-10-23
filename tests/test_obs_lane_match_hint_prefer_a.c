/**
 * test_obs_lane_match_hint_prefer_a.c — D2-07 Prefer A A3/A4 fixtures
 *
 * A3: valid==1 → all obstacles in list get hint==1
 * A4: never/valid==0 → hint==0; publish path still stamps list
 */
#include "adas_msgs_gen.h"
#include "obs_lane_match_hint.h"

#include <stdio.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(cond, name) do { \
    if (cond) { g_passed++; printf("  PASS: %s\n", name); } \
    else      { g_failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

static ObstacleList make_list(uint32_t n) {
    ObstacleList list;
    memset(&list, 0, sizeof(list));
    list.count = n;
    for (uint32_t i = 0; i < n; i++) {
        list.obstacles[i].id = i + 1;
        list.obstacles[i].lane_id = (int8_t)(i % 3);
        list.obstacles[i].obs_lane_match_hint = (int8_t)99; /* poison */
    }
    return list;
}

static void test_a3_valid_one_whole_publish(void) {
    ObstacleList list = make_list(3);
    int8_t hint = obs_lane_match_hint_from_valid(1);
    TEST(hint == 1, "A3: from_valid(1) == 1");
    obs_lane_match_hint_apply(&list, hint);
    TEST(list.count == 3, "A3: count unchanged");
    TEST(list.obstacles[0].obs_lane_match_hint == 1, "A3: obs[0] hint==1");
    TEST(list.obstacles[1].obs_lane_match_hint == 1, "A3: obs[1] hint==1");
    TEST(list.obstacles[2].obs_lane_match_hint == 1, "A3: obs[2] hint==1");
}

static void test_a4_missing_or_invalid(void) {
    ObstacleList list = make_list(2);
    /* never-received / valid==0 / parse fail all map to from_valid(0) */
    int8_t hint = obs_lane_match_hint_from_valid(0);
    TEST(hint == 0, "A4: from_valid(0) == 0");
    obs_lane_match_hint_apply(&list, hint);
    TEST(list.obstacles[0].obs_lane_match_hint == 0, "A4: obs[0] hint==0");
    TEST(list.obstacles[1].obs_lane_match_hint == 0, "A4: obs[1] hint==0");

    /* non-1 valid values also → 0 */
    TEST(obs_lane_match_hint_from_valid(2) == 0, "A4: from_valid(2) == 0");
    TEST(obs_lane_match_hint_from_valid(-1) == 0, "A4: from_valid(-1) == 0");

    /* apply still OK on empty list (publish OK path) */
    ObstacleList empty;
    memset(&empty, 0, sizeof(empty));
    obs_lane_match_hint_apply(&empty, 0);
    TEST(empty.count == 0, "A4: empty list apply OK");
}

static void test_same_hint_not_per_obs(void) {
    /* Prefer A: far-lane obs still get hint==1 when valid==1 (not per-obs). */
    ObstacleList list = make_list(2);
    list.obstacles[0].y = 0.0f;
    list.obstacles[1].y = 20.0f; /* far lateral — still same hint */
    obs_lane_match_hint_apply(&list, obs_lane_match_hint_from_valid(1));
    TEST(list.obstacles[0].obs_lane_match_hint == 1, "whole-publish near hint==1");
    TEST(list.obstacles[1].obs_lane_match_hint == 1, "whole-publish far hint==1");
}

int main(void) {
    printf("Running D2-07 Prefer A obs_lane_match_hint fixtures...\n");
    test_a3_valid_one_whole_publish();
    test_a4_missing_or_invalid();
    test_same_hint_not_per_obs();
    printf("Results: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
