/**
 * lane_match_helpers.cpp — lane_match M3 helpers + D2-03 Prefer A Lanelet2 load
 */

#include "lane_match_helpers.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static constexpr double LM_CURVATURE_DEGEN_EPS = 1e-12;

double flowsim_lm_curvature_3pt(double x1, double y1,
                                double x2, double y2,
                                double x3, double y3) {
    const double ax = x2 - x1;
    const double ay = y2 - y1;
    const double bx = x3 - x1;
    const double by = y3 - y1;
    const double cx = x3 - x2;
    const double cy = y3 - y2;

    const double a_len = std::hypot(ax, ay);
    const double b_len = std::hypot(bx, by);
    const double c_len = std::hypot(cx, cy);

    if (a_len < 1e-9 || b_len < 1e-9 || c_len < 1e-9) return 0.0;

    const double denom = a_len * b_len * c_len;
    if (denom < LM_CURVATURE_DEGEN_EPS) return 0.0;

    const double cross = ax * by - ay * bx;
    const double k = (2.0 * cross) / denom;
    if (!std::isfinite(k)) return 0.0;
    return k;
}

uint64_t flowsim_lm_synthesize_lanelet_id(int road_id, int lane_id) {
    if (road_id < 0) return 0ULL;
    const long long lane_offset = static_cast<long long>(lane_id) + 500LL;
    if (lane_offset < 0) return 0ULL;
    const unsigned long long road_part =
        static_cast<unsigned long long>(road_id) * 1000ULL;
    return road_part + static_cast<unsigned long long>(lane_offset);
}

int flowsim_lm_pick_adjacent_lane_id(int current_lane,
                                     const int* candidate_ids, int n,
                                     int side) {
    if (candidate_ids == nullptr || n <= 0) return 0;
    if (side == 0) return 0;

    int best = 0;
    int best_dist = 0;
    for (int i = 0; i < n; ++i) {
        const int lid = candidate_ids[i];
        const int delta = lid - current_lane;
        if (delta == 0) continue;
        if (side > 0) {
            if (delta <= 0) continue;
            if (best == 0 || delta < best_dist) { best = lid; best_dist = delta; }
        } else {
            if (delta >= 0) continue;
            if (best == 0 || -delta < best_dist) { best = lid; best_dist = -delta; }
        }
    }
    if (best != 0 && std::abs(best_dist) > 1000000) return 0;
    return best;
}

static constexpr double LM_LANE_STICK_RATIO = 0.25;

int flowsim_lm_hold_lane_id(int candidate_lane, int held_lane,
                            double offset_in_lane, double lane_width) {
    if (held_lane == 0 || !(lane_width > 0.0)) return candidate_lane;
    if (candidate_lane == held_lane) return held_lane;

    const double band = LM_LANE_STICK_RATIO * lane_width;

    if (candidate_lane > held_lane) {
        return (offset_in_lane > band) ? candidate_lane : held_lane;
    }
    return (offset_in_lane < -band) ? candidate_lane : held_lane;
}

void flowsim_lm_zero_outputs_on_failure(
    uint64_t* out_llt_id, double* out_llt_s, double* out_llt_offset,
    double* out_heading_err, int* out_valid,
    double* out_curvature, double* out_lane_width,
    uint64_t* out_left_lanelet_id, uint64_t* out_right_lanelet_id,
    uint32_t* out_flags) {
    if (out_llt_id)            *out_llt_id = 0ULL;
    if (out_llt_s)             *out_llt_s = 0.0;
    if (out_llt_offset)        *out_llt_offset = 0.0;
    if (out_heading_err)       *out_heading_err = 0.0;
    if (out_valid)             *out_valid = 0;
    if (out_curvature)         *out_curvature = 0.0;
    if (out_lane_width)        *out_lane_width = 0.0;
    if (out_left_lanelet_id)   *out_left_lanelet_id = 0ULL;
    if (out_right_lanelet_id)  *out_right_lanelet_id = 0ULL;
    if (out_flags)             *out_flags = 0U;
}

/* ═══════════════════════════════════════════════════════════════════
 * D2-03 Prefer A — resolve / load / findNearest
 * ═══════════════════════════════════════════════════════════════════ */

#if defined(KUN_HAS_LANELET2) && KUN_HAS_LANELET2

#include <lanelet2_core/geometry/LaneletMap.h>
#include <lanelet2_core/primitives/GPSPoint.h>
#include <lanelet2_io/Io.h>

#include <lanelet2_io/Projection.h>

#include <stdexcept>
#include <unistd.h>

namespace flowsim_lm {
namespace {

bool file_readable(const std::string& path) {
    std::ifstream in(path);
    return in.good();
}

std::string dirname_of(const std::string& path) {
    const auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::string read_text_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/** Strip M1 N#/W#/R# prefixes so pugixml as_llong can parse ids/refs. */
std::string rewrite_m1_osm_ids(const std::string& src) {
    static const std::regex id_re(R"re((id|ref)="([NWR])(\d+)")re");
    return std::regex_replace(src, id_re, R"re($1="$3")re");
}

std::string write_temp_osm(const std::string& content) {
    /* mkstemps needs XXXXXX before suffix */
    char tmpl2[] = "/tmp/kun_lanelet_XXXXXX";
    int fd = mkstemp(tmpl2);
    if (fd < 0) {
        throw std::runtime_error("mkstemp failed for lanelet rewrite temp");
    }
    std::string path = std::string(tmpl2) + ".osm";
    close(fd);
    unlink(tmpl2);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write temp osm: " + path);
    }
    out << content;
    out.close();
    return path;
}

/** Extract "map_file" / "map_id" string from scenario JSON without full cJSON
 *  dependency in this TU — tiny key scanner (values are simple strings). */
bool json_string_field(const std::string& json, const char* key,
                       std::string* value) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return false;
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return false;
    *value = json.substr(pos + 1, end - pos - 1);
    return true;
}

}  /* namespace */

class PassThroughProjector : public lanelet::Projector {
public:
    explicit PassThroughProjector(lanelet::Origin origin = lanelet::Origin({0.0, 0.0}))
        : Projector(std::move(origin)) {}

    lanelet::BasicPoint3d forward(const lanelet::GPSPoint& gps) const override {
        /* M1 contract: lat=y, lon=x (fake WGS84 carrying map metres). */
        return {gps.lon, gps.lat, gps.ele};
    }

    lanelet::GPSPoint reverse(const lanelet::BasicPoint3d& p) const override {
        return {p.y(), p.x(), p.z()};
    }
};

std::string resolve_lanelet_osm_path(const std::string& scenario_file,
                                     bool* declared) {
    if (declared) *declared = false;
    if (scenario_file.empty()) return {};

    const std::string json = read_text_file(scenario_file);
    if (json.empty()) return {};

    std::string map_file, map_id;
    const bool has_mf = json_string_field(json, "map_file", &map_file);
    const bool has_id = json_string_field(json, "map_id", &map_id);
    if (!has_mf && !has_id) {
        return {};
    }
    if (declared) *declared = true;

    const std::string base = dirname_of(scenario_file);
    std::string map_json_path;

    if (has_mf) {
        const std::string cands[] = {
            base + "/" + map_file,
            base + "/../" + map_file,
            map_file,
        };
        for (const auto& c : cands) {
            if (file_readable(c)) {
                map_json_path = c;
                break;
            }
        }
        if (map_json_path.empty()) {
            /* Declared but map.json missing — still compose osm beside the
             * first candidate so startup FAIL sees a concrete missing path. */
            map_json_path = cands[0];
        }
    } else {
        map_json_path = base + "/../maps/" + map_id + "/map.json";
        if (!file_readable(map_json_path)) {
            map_json_path = std::string("maps/") + map_id + "/map.json";
        }
    }

    return dirname_of(map_json_path) + "/lanelet.osm";
}

lanelet::LaneletMapPtr load_lanelet_map(const std::string& osm_path) {
    if (!file_readable(osm_path)) {
        throw std::runtime_error("lanelet.osm not readable: " + osm_path);
    }
    const std::string raw = read_text_file(osm_path);
    if (raw.empty()) {
        throw std::runtime_error("lanelet.osm empty: " + osm_path);
    }
    const std::string rewritten = rewrite_m1_osm_ids(raw);
    const std::string tmp = write_temp_osm(rewritten);
    lanelet::ErrorMessages errs;
    lanelet::LaneletMapPtr map;
    try {
        PassThroughProjector projector;
        map = lanelet::load(tmp, projector, &errs);
    } catch (...) {
        unlink(tmp.c_str());
        throw;
    }
    unlink(tmp.c_str());
    if (!map) {
        std::string detail;
        for (const auto& e : errs) {
            if (!detail.empty()) detail += "; ";
            detail += e;
        }
        throw std::runtime_error(
            std::string("lanelet::load failed for ") + osm_path +
            (detail.empty() ? "" : (": " + detail)));
    }
    if (!errs.empty()) {
        /* Non-fatal parse warnings — still accept map if layer non-empty. */
    }
    if (map->laneletLayer.empty()) {
        throw std::runtime_error(
            "lanelet::load produced empty laneletLayer: " + osm_path);
    }
    return map;
}

uint64_t real_lanelet_id_at(const lanelet::LaneletMap& map, double x, double y) {
    const lanelet::BasicPoint2d pt{x, y};
    const auto nearest = lanelet::geometry::findNearest(map.laneletLayer, pt, 1);
    if (nearest.empty()) return 0ULL;
    const lanelet::Id id = nearest.front().second.id();
    if (id <= 0) return 0ULL;
    return static_cast<uint64_t>(id);
}

bool lanelet_exists(const lanelet::LaneletMap& map, uint64_t id) {
    if (id == 0) return false;
    try {
        const auto lit = map.laneletLayer.find(static_cast<lanelet::Id>(id));
        return lit != map.laneletLayer.end();
    } catch (...) {
        return false;
    }
}

}  /* namespace flowsim_lm */

struct FlowsimLaneletMap {
    lanelet::LaneletMapPtr ptr;
};

int flowsim_lm_resolve_lanelet_osm(const char* scenario_file,
                                   char* out, size_t out_sz,
                                   int* declared_out) {
    if (declared_out) *declared_out = 0;
    if (!scenario_file || !out || out_sz == 0) return -1;
    bool declared = false;
    const std::string path =
        flowsim_lm::resolve_lanelet_osm_path(scenario_file, &declared);
    if (declared_out) *declared_out = declared ? 1 : 0;
    if (!declared || path.empty()) return -1;
    if (path.size() >= out_sz) return -1;
    std::memcpy(out, path.c_str(), path.size() + 1);
    return 0;
}

FlowsimLaneletMap* flowsim_lm_map_load(const char* osm_path,
                                       char* err, size_t err_sz) {
    if (err && err_sz) err[0] = '\0';
    if (!osm_path) {
        if (err && err_sz) std::snprintf(err, err_sz, "null osm_path");
        return nullptr;
    }
    try {
        auto* h = new FlowsimLaneletMap;
        h->ptr = flowsim_lm::load_lanelet_map(osm_path);
        return h;
    } catch (const std::exception& ex) {
        if (err && err_sz) std::snprintf(err, err_sz, "%s", ex.what());
        return nullptr;
    } catch (...) {
        if (err && err_sz) std::snprintf(err, err_sz, "unknown load error");
        return nullptr;
    }
}

void flowsim_lm_map_free(FlowsimLaneletMap* map) {
    delete map;
}

uint64_t flowsim_lm_map_id_at(const FlowsimLaneletMap* map, double x, double y) {
    if (!map || !map->ptr) return 0ULL;
    return flowsim_lm::real_lanelet_id_at(*map->ptr, x, y);
}

int flowsim_lm_map_contains(const FlowsimLaneletMap* map, uint64_t id) {
    if (!map || !map->ptr) return 0;
    return flowsim_lm::lanelet_exists(*map->ptr, id) ? 1 : 0;
}

size_t flowsim_lm_map_size(const FlowsimLaneletMap* map) {
    if (!map || !map->ptr) return 0;
    return map->ptr->laneletLayer.size();
}

#else  /* !KUN_HAS_LANELET2 */

int flowsim_lm_resolve_lanelet_osm(const char* /*scenario_file*/,
                                   char* /*out*/, size_t /*out_sz*/,
                                   int* declared_out) {
    if (declared_out) *declared_out = 0;
    return -1;
}

FlowsimLaneletMap* flowsim_lm_map_load(const char* /*osm_path*/,
                                       char* err, size_t err_sz) {
    if (err && err_sz)
        std::snprintf(err, err_sz, "KUN_HAS_LANELET2 not enabled at build");
    return nullptr;
}

void flowsim_lm_map_free(FlowsimLaneletMap* /*map*/) {}

uint64_t flowsim_lm_map_id_at(const FlowsimLaneletMap* /*map*/,
                              double /*x*/, double /*y*/) {
    return 0ULL;
}

int flowsim_lm_map_contains(const FlowsimLaneletMap* /*map*/, uint64_t /*id*/) {
    return 0;
}

size_t flowsim_lm_map_size(const FlowsimLaneletMap* /*map*/) { return 0; }

#endif  /* KUN_HAS_LANELET2 */
