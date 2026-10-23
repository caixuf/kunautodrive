/**
 * obs_lane_match_hint.h — D2-07 Prefer A whole-publish hint helper
 *
 * Invariant (rails):
 *   on each perception/obstacles publish:
 *     obs_lane_match_hint := 1  iff  latest localization/lane_match.valid == 1
 *                          := 0  otherwise (missing / never / valid==0 / parse fail)
 *   Same hint value for ALL obstacles in that publish.
 *
 * Soft metadata only. Does not hard-filter obstacles.
 * Distinct from fusion_lane_hint.h (per-obs same/adj-lane heuristic used by
 * perception_fusion_node — OOS for Prefer A default perception_node path).
 */
#ifndef OBS_LANE_MATCH_HINT_H
#define OBS_LANE_MATCH_HINT_H

#include "adas_msgs_gen.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Map lane_match.valid (0/1) to Obstacle.obs_lane_match_hint (int8 0/1). */
static inline int8_t obs_lane_match_hint_from_valid(int valid) {
    return (valid == 1) ? (int8_t)1 : (int8_t)0;
}

/** Stamp every obstacle in the list with the same Prefer A hint. */
static inline void obs_lane_match_hint_apply(ObstacleList* list, int8_t hint) {
    if (!list) return;
    const int8_t h = (hint == 1) ? (int8_t)1 : (int8_t)0;
    for (uint32_t i = 0; i < list->count; i++) {
        list->obstacles[i].obs_lane_match_hint = h;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* OBS_LANE_MATCH_HINT_H */
