/**
 * lane_match_helpers.h — `localization/lane_match` M3 字段纯算法 helper
 *
 * D2-03 Prefer A: dual-mode real Lanelet2 IDs when sibling lanelet.osm loads;
 * M2 synthesize kept for no-osm road_network scenes (D2-10).
 */

#ifndef FLOWSIM_LANE_MATCH_HELPERS_H
#define FLOWSIM_LANE_MATCH_HELPERS_H

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

double flowsim_lm_curvature_3pt(double x1, double y1,
                                double x2, double y2,
                                double x3, double y3);

/**
 * M2 / no-osm path: synthesize llt_id = road_id*1000 + (lane_id+500).
 * Retired on the loaded Lanelet2 path (D2-03 Prefer A).
 */
uint64_t flowsim_lm_synthesize_lanelet_id(int road_id, int lane_id);

int flowsim_lm_pick_adjacent_lane_id(int current_lane,
                                     const int* candidate_ids, int n,
                                     int side);

int flowsim_lm_hold_lane_id(int candidate_lane, int held_lane,
                            double offset_in_lane, double lane_width);

void flowsim_lm_zero_outputs_on_failure(
    uint64_t* out_llt_id, double* out_llt_s, double* out_llt_offset,
    double* out_heading_err, int* out_valid,
    double* out_curvature, double* out_lane_width,
    uint64_t* out_left_lanelet_id, uint64_t* out_right_lanelet_id,
    uint32_t* out_flags);

/* ── D2-03 Prefer A: opaque LaneletMap handle (C ABI for unit tests) ── */

typedef struct FlowsimLaneletMap FlowsimLaneletMap;

/**
 * Resolve sibling lanelet.osm for a scenario JSON path.
 * @param declared_out 1 if scenario has map_file or map_id (osm expected)
 * @return 0 and writes absolute-ish path to out on success; -1 if not declared
 *         or path could not be composed. Existence is NOT checked here.
 */
int flowsim_lm_resolve_lanelet_osm(const char* scenario_file,
                                   char* out, size_t out_sz,
                                   int* declared_out);

/**
 * Load lanelet.osm (M1 N#/W#/R# id rewrite + pass-through lon→x,lat→y).
 * @return handle or NULL; on failure writes message to err (may be NULL).
 */
FlowsimLaneletMap* flowsim_lm_map_load(const char* osm_path,
                                       char* err, size_t err_sz);

void flowsim_lm_map_free(FlowsimLaneletMap* map);

/** findNearest → lanelet::Id; 0 if empty / null map. */
uint64_t flowsim_lm_map_id_at(const FlowsimLaneletMap* map, double x, double y);

/** 1 if id ∈ laneletLayer (and id != 0). */
int flowsim_lm_map_contains(const FlowsimLaneletMap* map, uint64_t id);

/** laneletLayer size (0 if null). */
size_t flowsim_lm_map_size(const FlowsimLaneletMap* map);

#ifdef __cplusplus
}  /* extern "C" */

#if defined(KUN_HAS_LANELET2) && KUN_HAS_LANELET2
#include <lanelet2_core/LaneletMap.h>
#include <memory>
#include <string>

namespace flowsim_lm {

/** Pass-through projector: M1 OSM stores map (x,y) as (lon,lat). */
class PassThroughProjector;

std::string resolve_lanelet_osm_path(const std::string& scenario_file,
                                     bool* declared);

/**
 * Load with N/W/R prefix strip + PassThrough. Throws std::runtime_error on
 * IO/parse failure (caller maps to startup FAIL).
 */
lanelet::LaneletMapPtr load_lanelet_map(const std::string& osm_path);

uint64_t real_lanelet_id_at(const lanelet::LaneletMap& map, double x, double y);
bool lanelet_exists(const lanelet::LaneletMap& map, uint64_t id);

}  /* namespace flowsim_lm */
#endif  /* KUN_HAS_LANELET2 */

#endif  /* __cplusplus */

#endif  /* FLOWSIM_LANE_MATCH_HELPERS_H */
