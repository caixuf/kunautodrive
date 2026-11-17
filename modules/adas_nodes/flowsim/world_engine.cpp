#include "flowsim/world_engine.h"
#include "road_speed_policy.h"

#include <cjson/cJSON.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace flowsim {
namespace {

constexpr double kPi = 3.14159265358979323846;

/* ── Deterministic FNV-1a + SplitMix64 helpers ─────────────────── */

inline uint64_t fnv1a_step(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
    return h;
}

inline uint64_t fnv1a_bytes(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

inline double hash_unit(uint32_t seed, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t x = 1469598103934665603ULL;
    x = fnv1a_step(x, seed);
    x = fnv1a_step(x, a);
    x = fnv1a_step(x, b);
    x = fnv1a_step(x, c);
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return static_cast<double>(x & 0xFFFFFFFFULL) / 4294967296.0;
}

inline double clamp_d(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}

inline bool is_finite_pos(double x, double y, double z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

inline uint32_t parse_kind_mask(cJSON* jkinds) {
    if (!jkinds || !cJSON_IsArray(jkinds) || cJSON_GetArraySize(jkinds) == 0) {
        return WORLD_MASK_ALL;
    }
    uint32_t mask = WORLD_MASK_NONE;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, jkinds) {
        if (!cJSON_IsString(item) || !item->valuestring) continue;
        const char* k = item->valuestring;
        if (strcmp(k, "all") == 0) return WORLD_MASK_ALL;
        if (strcmp(k, "pole") == 0 || strcmp(k, "poles") == 0) mask |= WORLD_MASK_POLE;
        else if (strcmp(k, "tree") == 0 || strcmp(k, "trees") == 0) mask |= WORLD_MASK_TREE;
        else if (strcmp(k, "barrier") == 0 || strcmp(k, "barriers") == 0) mask |= WORLD_MASK_BARRIER;
        else if (strcmp(k, "pier") == 0 || strcmp(k, "piers") == 0) mask |= WORLD_MASK_PIER;
        else if (strcmp(k, "sign") == 0 || strcmp(k, "signs") == 0) mask |= WORLD_MASK_SIGN;
        else if (strcmp(k, "furniture") == 0) mask |= WORLD_MASK_FURNITURE;
        else if (strcmp(k, "building") == 0 || strcmp(k, "buildings") == 0) mask |= WORLD_MASK_BUILDING;
        else if (strcmp(k, "prop") == 0 || strcmp(k, "props") == 0) mask |= WORLD_MASK_PROP;
    }
    return mask == WORLD_MASK_NONE ? WORLD_MASK_ALL : mask;
}

void apply_preset_defaults(WorldGenConfig& cfg) {
    if (cfg.preset == "modern_cbd") {
        cfg.pole_spacing_m = 36.0;
        cfg.tree_spacing_m = 26.0;
        cfg.barrier_spacing_m = 4.0;
        cfg.pier_spacing_m = 22.0;
        cfg.auto_buildings = true;
        cfg.auto_signs = true;
        cfg.auto_furniture = true;
        cfg.tree_species = "broadleaf";
    } else if (cfg.preset == "suburban_parkway") {
        cfg.pole_spacing_m = 48.0;
        cfg.tree_spacing_m = 20.0;
        cfg.barrier_spacing_m = 4.0;
        cfg.pier_spacing_m = 24.0;
        cfg.auto_buildings = true;
        cfg.auto_signs = true;
        cfg.auto_furniture = true;
        cfg.tree_species = "parkway";
    } else if (cfg.preset == "highway_corridor") {
        cfg.pole_spacing_m = 50.0;
        cfg.tree_spacing_m = 42.0;
        cfg.barrier_spacing_m = 4.0;
        cfg.pier_spacing_m = 22.0;
        cfg.auto_trees = false;
        cfg.auto_furniture = false;
        cfg.auto_signs = true;
        cfg.auto_barriers = true;
        cfg.auto_buildings = false;
        cfg.tree_species = "pine";
    } else {
        cfg.preset = "urban_arterial";
        cfg.pole_spacing_m = 40.0;
        cfg.tree_spacing_m = 30.0;
        cfg.barrier_spacing_m = 4.0;
        cfg.pier_spacing_m = 22.0;
    }
}

/* ── Compiled Edge Representation ────────────────────────────── */

struct Pt3 {
    double x{0.0}, y{0.0}, z{0.0};
};

struct CompiledEdge {
    int id{0};
    std::string id_str;
    std::string name;
    std::string type;
    int lanes{2};
    double lane_width{3.5};
    double half_w{3.5};
    double speed_limit_mps{13.89};
    bool is_oneway{false};
    bool is_bridge{false};
    bool is_tunnel{false};
    bool is_subsurface{false}; /* garage_hall or open_ramp */
    bool is_viaduct_special{false}; /* viaduct_highway name/type */
    std::vector<Pt3> raw_pts;
    std::vector<Pt3> pts;      /* Resampled center-shifted spline points */
    std::vector<double> cum;   /* Cumulative 2D arc length */
    double total_len{0.0};
    double max_z{0.0};
    double min_z{0.0};
    /* Effective edge flags after edge_overrides */
    bool gen_poles{true};
    bool gen_trees{true};
    bool gen_barriers{false};
    bool gen_signs{true};
    bool gen_furniture{true};
    double pole_spacing_m{40.0};
    double tree_spacing_m{30.0};

    void sample_at_s(double s_req, double& ox, double& oy, double& oz, double& ohdg) const {
        if (pts.size() < 2) {
            ox = oy = oz = ohdg = 0.0;
            return;
        }
        double sc = clamp_d(s_req, 0.0, total_len);
        size_t seg_i = 0;
        for (size_t i = 1; i < cum.size(); ++i) {
            seg_i = i - 1;
            if (cum[i] >= sc) break;
        }
        const Pt3& a = pts[seg_i];
        const Pt3& b = pts[seg_i + 1];
        double seg_len = cum[seg_i + 1] - cum[seg_i];
        double u = (seg_len > 1e-6) ? clamp_d((sc - cum[seg_i]) / seg_len, 0.0, 1.0) : 0.0;
        ox = a.x + (b.x - a.x) * u;
        oy = a.y + (b.y - a.y) * u;
        oz = a.z + (b.z - a.z) * u;
        ohdg = std::atan2(b.y - a.y, b.x - a.x);
    }
};

std::vector<Pt3> resample_spine(const std::vector<Pt3>& in_raw) {
    if (in_raw.size() < 2) return in_raw;
    std::vector<Pt3> clean;
    clean.reserve(in_raw.size());
    clean.push_back(in_raw.front());
    for (size_t i = 1; i + 1 < in_raw.size(); ++i) {
        const auto& prev = clean.back();
        if (std::hypot(in_raw[i].x - prev.x, in_raw[i].y - prev.y) >= 0.25) {
            clean.push_back(in_raw[i]);
        }
    }
    const auto& last_raw = in_raw.back();
    if (clean.size() == 1 ||
        std::hypot(last_raw.x - clean.back().x, last_raw.y - clean.back().y) >= 1e-4) {
        clean.push_back(last_raw);
    }
    if (clean.size() == 1) {
        clean.push_back({last_raw.x + 0.01, last_raw.y, last_raw.z});
    }
    if (clean.size() == 2) return clean;

    bool sharp_or_uneven = false;
    for (size_t i = 1; i + 1 < clean.size(); ++i) {
        double v1x = clean[i].x - clean[i - 1].x;
        double v1y = clean[i].y - clean[i - 1].y;
        double v2x = clean[i + 1].x - clean[i].x;
        double v2y = clean[i + 1].y - clean[i].y;
        double l1 = std::hypot(v1x, v1y);
        double l2 = std::hypot(v2x, v2y);
        if (l1 < 1e-4 || l2 < 1e-4) {
            sharp_or_uneven = true;
            break;
        }
        double ratio = (l1 > l2) ? (l1 / l2) : (l2 / l1);
        double dot = clamp_d((v1x * v2x + v1y * v2y) / (l1 * l2), -1.0, 1.0);
        double ang = std::acos(dot);
        if (ang > 0.55 || ratio > 3.2) {
            sharp_or_uneven = true;
            break;
        }
    }

    if (sharp_or_uneven) {
        std::vector<Pt3> poly = clean;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<Pt3> next;
            next.reserve(poly.size() * 2);
            next.push_back(poly.front());
            for (size_t i = 1; i + 1 < poly.size(); ++i) {
                const auto& a = poly[i - 1];
                const auto& m = poly[i];
                const auto& b = poly[i + 1];
                double l1 = std::hypot(m.x - a.x, m.y - a.y);
                double l2 = std::hypot(b.x - m.x, b.y - m.y);
                double t1 = l1 > 1e-6 ? std::min(0.25, 3.5 / l1) : 0.25;
                double t2 = l2 > 1e-6 ? std::min(0.25, 3.5 / l2) : 0.25;
                next.push_back({m.x + (a.x - m.x) * t1, m.y + (a.y - m.y) * t1, m.z + (a.z - m.z) * t1});
                next.push_back({m.x + (b.x - m.x) * t2, m.y + (b.y - m.y) * t2, m.z + (b.z - m.z) * t2});
            }
            next.push_back(poly.back());
            poly = std::move(next);
        }
        return poly;
    }

    /* Smooth Catmull-Rom 6 subdivisions matching flowsim/Curve.js */
    const int sub = 6;
    std::vector<Pt3> pts;
    pts.reserve((clean.size() - 1) * sub + 1);
    auto cr = [](double p0, double p1, double p2, double p3, double t) {
        double t2 = t * t, t3 = t2 * t;
        return 0.5 * ((2.0 * p1) + (-p0 + p2) * t +
                      (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                      (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
    };
    for (size_t i = 0; i + 1 < clean.size(); ++i) {
        const Pt3& p0 = clean[i > 0 ? i - 1 : 0];
        const Pt3& p1 = clean[i];
        const Pt3& p2 = clean[i + 1];
        const Pt3& p3 = clean[i + 2 < clean.size() ? i + 2 : clean.size() - 1];
        for (int st = 0; st < sub; ++st) {
            double t = static_cast<double>(st) / sub;
            pts.push_back({cr(p0.x, p1.x, p2.x, p3.x, t),
                           cr(p0.y, p1.y, p2.y, p3.y, t),
                           cr(p0.z, p1.z, p2.z, p3.z, t)});
        }
    }
    pts.push_back(clean.back());
    return pts;
}

void shift_oneway_center(std::vector<Pt3>& pts, double half_w) {
    if (pts.size() < 2 || std::fabs(half_w) < 1e-6) return;
    std::vector<Pt3> shifted = pts;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Pt3& a = pts[i > 0 ? i - 1 : 0];
        const Pt3& b = pts[i + 1 < pts.size() ? i + 1 : pts.size() - 1];
        double dx = b.x - a.x, dy = b.y - a.y;
        double slen = std::hypot(dx, dy);
        if (slen > 1e-6) {
            /* OpenDRIVE oneway left-edge -> center: ENU right normal (dy/L, -dx/L) */
            shifted[i].x += (dy / slen) * half_w;
            shifted[i].y -= (dx / slen) * half_w;
        }
    }
    pts = std::move(shifted);
}

bool contains_tunnel_keyword(const std::string& s) {
    if (s.find("tunnel") != std::string::npos ||
        s.find("TUNNEL") != std::string::npos ||
        s.find("\xe9\x9a\xa7\xe9\x81\x93") != std::string::npos || /* 隧道 */
        s.find("\xe5\x9c\xb0\xe9\x81\x93") != std::string::npos) { /* 地道 */
        return true;
    }
    return false;
}

/* ── Spatial Corridor & Elevation Field ──────────────────────── */

struct RoadBandSeg {
    double ax, ay, az;
    double bx, by, bz;
    double hw;
    double road_hw;
    bool is_subsurface;
    bool is_bridge;
    int edge_idx;
};

struct JunctionCluster {
    double x{0.0}, y{0.0}, z{0.0};
    double max_hw{3.5};
    int count{0};
};

class SpatialField {
public:
    static constexpr double kCellM = 12.0;

    void build(const std::vector<CompiledEdge>& edges,
               const WorldEngineInput& input,
               const WorldGenConfig& cfg) {
        cfg_ = &cfg;
        input_ = &input;

        for (size_t ei = 0; ei < edges.size(); ++ei) {
            const auto& e = edges[ei];
            if (e.pts.size() < 2) continue;

            if (!e.is_subsurface && !e.is_viaduct_special) {
                for (size_t ep : {size_t{0}, e.pts.size() - 1}) {
                    const auto& p = e.pts[ep];
                    bool merged = false;
                    for (auto& jc : junctions_) {
                        if (std::hypot(p.x - jc.x, p.y - jc.y) <= 12.0 &&
                            std::fabs(p.z - jc.z) <= 3.0) {
                            jc.x = (jc.x * jc.count + p.x) / (jc.count + 1);
                            jc.y = (jc.y * jc.count + p.y) / (jc.count + 1);
                            jc.z = (jc.z * jc.count + p.z) / (jc.count + 1);
                            jc.max_hw = std::max(jc.max_hw, e.half_w);
                            jc.count++;
                            merged = true;
                            break;
                        }
                    }
                    if (!merged) {
                        junctions_.push_back({p.x, p.y, p.z, e.half_w, 1});
                    }
                }
            }

            double pad = e.is_subsurface ? 2.5 : 0.55;
            if (e.type == "garage_hall") pad = std::max(pad, 12.2 - e.half_w);
            double band_hw = e.half_w + pad;
            for (size_t k = 1; k < e.pts.size(); ++k) {
                RoadBandSeg s{
                    e.pts[k - 1].x, e.pts[k - 1].y, e.pts[k - 1].z,
                    e.pts[k].x, e.pts[k].y, e.pts[k].z,
                    band_hw, e.half_w, e.is_subsurface, e.is_bridge, static_cast<int>(ei)
                };
                int seg_id = static_cast<int>(segs_.size());
                segs_.push_back(s);
                double pad_box = band_hw + 18.0;
                int c0 = cell_coord(std::min(s.ax, s.bx) - pad_box);
                int c1 = cell_coord(std::max(s.ax, s.bx) + pad_box);
                int r0 = cell_coord(std::min(s.ay, s.by) - pad_box);
                int r1 = cell_coord(std::max(s.ay, s.by) + pad_box);
                for (int c = c0; c <= c1; ++c) {
                    for (int r = r0; r <= r1; ++r) {
                        grid_[pack_cell(c, r)].push_back(seg_id);
                    }
                }
            }
        }
    }

    const std::vector<JunctionCluster>& junctions() const { return junctions_; }

    bool in_junction(double x, double y, double extra_margin, int min_arms = 3) const {
        for (const auto& jc : junctions_) {
            if (jc.count >= min_arms) {
                double clip_r = jc.max_hw + 4.1 + extra_margin;
                if (std::hypot(x - jc.x, y - jc.y) <= clip_r) return true;
            }
        }
        return false;
    }

    bool near_traffic_light(double x, double y, double z, double min_dist = 7.5) const {
        if (!input_) return false;
        for (const auto& tl : input_->traffic_lights) {
            if (std::fabs(z - tl.z) <= 3.0 && std::hypot(x - tl.x, y - tl.y) < min_dist) {
                return true;
            }
        }
        return false;
    }

    bool near_parking_cutout(double x, double y, double z, double extra_margin = 0.0) const {
        if (!input_) return false;
        for (const auto& ps : input_->parking_slots) {
            if (std::fabs(z - ps.z) <= 2.5 &&
                std::hypot(x - ps.x, y - ps.y) < (12.0 + extra_margin)) {
                return true;
            }
        }
        if (input_->has_parking_goal) {
            const auto& pg = input_->parking_goal;
            if (std::fabs(z - pg.z) <= 2.5 &&
                std::hypot(x - pg.x, y - pg.y) < (12.0 + extra_margin)) {
                return true;
            }
        }
        if (input_->park_out.enabled) {
            const auto& po = input_->park_out;
            if (std::fabs(z - po.slot_z) <= 2.5 &&
                (std::hypot(x - po.slot_x, y - po.slot_y) < 12.0 ||
                 std::hypot(x - po.exit_x, y - po.exit_y) < 10.0)) {
                return true;
            }
        }
        return false;
    }

    bool is_excluded(double x, double y, double z, uint32_t mask) const {
        if (!cfg_) return false;
        for (const auto& ez : cfg_->exclude_zones) {
            if ((ez.kind_mask & mask) == 0u) continue;
            if (ez.check_z && std::fabs(z - ez.z) > 3.5) continue;
            if (ez.is_box) {
                if (x >= ez.min_x && x <= ez.max_x && y >= ez.min_y && y <= ez.max_y) return true;
            } else {
                if (std::hypot(x - ez.x, y - ez.y) <= ez.radius) return true;
            }
        }
        return false;
    }

    bool inside_road_band(double x, double y, double z, double extra_margin,
                          bool use_tight_asphalt = false) const {
        int c0 = cell_coord(x), r0 = cell_coord(y);
        for (int dc = -1; dc <= 1; ++dc) {
            for (int dr = -1; dr <= 1; ++dr) {
                auto it = grid_.find(pack_cell(c0 + dc, r0 + dr));
                if (it == grid_.end()) continue;
                for (int sid : it->second) {
                    const auto& s = segs_[sid];
                    double dx = s.bx - s.ax, dy = s.by - s.ay;
                    double l2 = dx * dx + dy * dy;
                    if (l2 < 1e-6) continue;
                    double t = clamp_d(((x - s.ax) * dx + (y - s.ay) * dy) / l2, 0.0, 1.0);
                    double px = s.ax + t * dx, py = s.ay + t * dy, pz = s.az + t * (s.bz - s.az);
                    double clear_hw = (use_tight_asphalt ? s.road_hw : s.hw) + extra_margin;
                    bool same_level = (std::fabs(z - pz) <= 2.5) ||
                                      (s.is_subsurface && z <= 1.0 && pz <= 0.5);
                    if (same_level && std::hypot(x - px, y - py) < clear_hw) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    double min_road_dist_2d(double x, double y) const {
        int c0 = cell_coord(x), r0 = cell_coord(y);
        double best = 1e9;
        for (int dc = -2; dc <= 2; ++dc) {
            for (int dr = -2; dr <= 2; ++dr) {
                auto it = grid_.find(pack_cell(c0 + dc, r0 + dr));
                if (it == grid_.end()) continue;
                for (int sid : it->second) {
                    const auto& s = segs_[sid];
                    if (s.is_bridge && std::min(s.az, s.bz) >= 2.8) continue;
                    double dx = s.bx - s.ax, dy = s.by - s.ay;
                    double l2 = dx * dx + dy * dy;
                    if (l2 < 1e-6) continue;
                    double t = clamp_d(((x - s.ax) * dx + (y - s.ay) * dy) / l2, 0.0, 1.0);
                    double px = s.ax + t * dx, py = s.ay + t * dy;
                    best = std::min(best, std::hypot(x - px, y - py));
                }
            }
        }
        return best;
    }

    bool building_clears_roads(double cx, double cy, double width, double depth,
                               double heading, double height,
                               double sidewalk_margin = 3.2) const {
        const double podium_mul = (height > 28.0) ? 1.35 : 1.0;
        const double hw_along = 0.5 * width * podium_mul;
        const double hd_across = 0.5 * depth * podium_mul;
        const double ch = std::cos(heading), sh = std::sin(heading);
        const double lx[9] = {0.0,  hw_along,  hw_along, -hw_along, -hw_along,
                              hw_along, -hw_along, 0.0, 0.0};
        const double ly[9] = {0.0,  hd_across, -hd_across, hd_across, -hd_across,
                              0.0, 0.0, hd_across, -hd_across};
        for (int k = 0; k < 9; ++k) {
            double px = cx + lx[k] * ch - ly[k] * sh;
            double py = cy + lx[k] * sh + ly[k] * ch;
            int c0 = cell_coord(px), r0 = cell_coord(py);
            for (int dc = -2; dc <= 2; ++dc) {
                for (int dr = -2; dr <= 2; ++dr) {
                    auto it = grid_.find(pack_cell(c0 + dc, r0 + dr));
                    if (it == grid_.end()) continue;
                    for (int sid : it->second) {
                        const auto& s = segs_[sid];
                        if (s.is_bridge && std::min(s.az, s.bz) >= 2.8) continue;
                        double dx = s.bx - s.ax, dy = s.by - s.ay;
                        double l2 = dx * dx + dy * dy;
                        if (l2 < 1e-6) continue;
                        double t = clamp_d(((px - s.ax) * dx + (py - s.ay) * dy) / l2, 0.0, 1.0);
                        double qx = s.ax + t * dx, qy = s.ay + t * dy;
                        double req_hw = (s.is_subsurface ? std::max(s.road_hw, s.hw) : s.road_hw) + sidewalk_margin;
                        if (std::hypot(px - qx, py - qy) < req_hw) {
                            return false;
                        }
                    }
                }
            }
        }
        return true;
    }

    bool hits_lower_asphalt(double x, double y, double z_top, double margin = 1.2) const {
        int c0 = cell_coord(x), r0 = cell_coord(y);
        for (int dc = -1; dc <= 1; ++dc) {
            for (int dr = -1; dr <= 1; ++dr) {
                auto it = grid_.find(pack_cell(c0 + dc, r0 + dr));
                if (it == grid_.end()) continue;
                for (int sid : it->second) {
                    const auto& s = segs_[sid];
                    double dx = s.bx - s.ax, dy = s.by - s.ay;
                    double l2 = dx * dx + dy * dy;
                    if (l2 < 1e-6) continue;
                    double t = clamp_d(((x - s.ax) * dx + (y - s.ay) * dy) / l2, 0.0, 1.0);
                    double px = s.ax + t * dx, py = s.ay + t * dy, pz = s.az + t * (s.bz - s.az);
                    if (pz < z_top - 1.5 && pz >= -1.0 &&
                        std::hypot(x - px, y - py) < s.road_hw + margin) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    double resolve_surface_z(double x, double y, double z_hint) const {
        int c0 = cell_coord(x), r0 = cell_coord(y);
        double best_dist = 1e9;
        double best_z = 0.0;
        for (int dc = -2; dc <= 2; ++dc) {
            for (int dr = -2; dr <= 2; ++dr) {
                auto it = grid_.find(pack_cell(c0 + dc, r0 + dr));
                if (it == grid_.end()) continue;
                for (int sid : it->second) {
                    const auto& s = segs_[sid];
                    double dx = s.bx - s.ax, dy = s.by - s.ay;
                    double l2 = dx * dx + dy * dy;
                    if (l2 < 1e-6) continue;
                    double t = clamp_d(((x - s.ax) * dx + (y - s.ay) * dy) / l2, 0.0, 1.0);
                    double px = s.ax + t * dx, py = s.ay + t * dy, pz = s.az + t * (s.bz - s.az);
                    double d = std::hypot(x - px, y - py);
                    if (d < 32.0) {
                        double score = d + std::fabs(pz - z_hint) * 0.25;
                        if (score < best_dist) {
                            best_dist = score;
                            best_z = pz;
                        }
                    }
                }
            }
        }
        return best_dist < 1e8 ? best_z : 0.0;
    }

private:
    static int cell_coord(double v) {
        return static_cast<int>(std::floor(v / kCellM));
    }
    static int64_t pack_cell(int cx, int cy) {
        return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cy);
    }

    const WorldGenConfig* cfg_{nullptr};
    const WorldEngineInput* input_{nullptr};
    std::vector<RoadBandSeg> segs_;
    std::vector<JunctionCluster> junctions_;
    std::unordered_map<int64_t, std::vector<int>> grid_;
};

/* ── Edge & JSON Parsing Helpers ─────────────────────────────── */

std::string read_text_file(const char* path) {
    if (!path || !path[0]) return {};
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

WorldGenConfig parse_world_gen_obj(cJSON* jwg, uint32_t default_seed) {
    WorldGenConfig cfg;
    cfg.seed = default_seed;
    if (!jwg || !cJSON_IsObject(jwg)) {
        apply_preset_defaults(cfg);
        return cfg;
    }
    if (cJSON* jp = cJSON_GetObjectItemCaseSensitive(jwg, "preset");
        cJSON_IsString(jp) && jp->valuestring) {
        cfg.preset = jp->valuestring;
    }
    apply_preset_defaults(cfg);

    if (cJSON* js = cJSON_GetObjectItemCaseSensitive(jwg, "seed"); cJSON_IsNumber(js)) {
        cfg.seed = static_cast<uint32_t>(js->valuedouble);
    }
    auto get_b = [&](const char* k, bool& dst) {
        cJSON* j = cJSON_GetObjectItemCaseSensitive(jwg, k);
        if (cJSON_IsBool(j)) dst = cJSON_IsTrue(j);
    };
    auto get_d = [&](const char* k, double& dst) {
        cJSON* j = cJSON_GetObjectItemCaseSensitive(jwg, k);
        if (cJSON_IsNumber(j) && j->valuedouble > 0.0) dst = j->valuedouble;
    };
    get_b("auto_poles", cfg.auto_poles);
    get_b("auto_trees", cfg.auto_trees);
    get_b("auto_barriers", cfg.auto_barriers);
    get_b("auto_piers", cfg.auto_piers);
    get_b("auto_signs", cfg.auto_signs);
    get_b("auto_furniture", cfg.auto_furniture);
    get_b("auto_buildings", cfg.auto_buildings);
    get_d("pole_spacing_m", cfg.pole_spacing_m);
    get_d("tree_spacing_m", cfg.tree_spacing_m);
    get_d("barrier_spacing_m", cfg.barrier_spacing_m);
    get_d("pier_spacing_m", cfg.pier_spacing_m);

    if (cJSON* jb = cJSON_GetObjectItemCaseSensitive(jwg, "biome"); cJSON_IsObject(jb)) {
        if (cJSON* jt = cJSON_GetObjectItemCaseSensitive(jb, "treeline"); cJSON_IsBool(jt)) {
            if (!cJSON_IsTrue(jt)) cfg.auto_trees = false;
        }
        if (cJSON* jd = cJSON_GetObjectItemCaseSensitive(jb, "density");
            cJSON_IsNumber(jd) && jd->valuedouble > 0.05) {
            cfg.tree_density = clamp_d(jd->valuedouble, 0.25, 2.5);
            cfg.tree_spacing_m = std::max(10.0, cfg.tree_spacing_m / cfg.tree_density);
        }
        if (cJSON* jsp = cJSON_GetObjectItemCaseSensitive(jb, "species");
            cJSON_IsString(jsp) && jsp->valuestring) {
            cfg.tree_species = jsp->valuestring;
        }
    }

    if (cJSON* jez = cJSON_GetObjectItemCaseSensitive(jwg, "exclude_zones"); cJSON_IsArray(jez)) {
        cJSON* z = nullptr;
        cJSON_ArrayForEach(z, jez) {
            if (!cJSON_IsObject(z)) continue;
            WorldExclusionZone ez;
            cJSON* jx = cJSON_GetObjectItemCaseSensitive(z, "x");
            cJSON* jy = cJSON_GetObjectItemCaseSensitive(z, "y");
            cJSON* jz = cJSON_GetObjectItemCaseSensitive(z, "z");
            cJSON* jr = cJSON_GetObjectItemCaseSensitive(z, "radius");
            cJSON* jminx = cJSON_GetObjectItemCaseSensitive(z, "min_x");
            cJSON* jmaxx = cJSON_GetObjectItemCaseSensitive(z, "max_x");
            cJSON* jminy = cJSON_GetObjectItemCaseSensitive(z, "min_y");
            cJSON* jmaxy = cJSON_GetObjectItemCaseSensitive(z, "max_y");
            if (cJSON_IsNumber(jminx) && cJSON_IsNumber(jmaxx) &&
                cJSON_IsNumber(jminy) && cJSON_IsNumber(jmaxy)) {
                ez.is_box = true;
                ez.min_x = std::min(jminx->valuedouble, jmaxx->valuedouble);
                ez.max_x = std::max(jminx->valuedouble, jmaxx->valuedouble);
                ez.min_y = std::min(jminy->valuedouble, jmaxy->valuedouble);
                ez.max_y = std::max(jminy->valuedouble, jmaxy->valuedouble);
            } else if (cJSON_IsNumber(jx) && cJSON_IsNumber(jy) && cJSON_IsNumber(jr)) {
                ez.x = jx->valuedouble;
                ez.y = jy->valuedouble;
                ez.radius = std::max(0.5, jr->valuedouble);
            } else {
                continue;
            }
            if (cJSON_IsNumber(jz)) {
                ez.z = jz->valuedouble;
                ez.check_z = true;
            }
            ez.kind_mask = parse_kind_mask(cJSON_GetObjectItemCaseSensitive(z, "kinds"));
            cfg.exclude_zones.push_back(std::move(ez));
        }
    }

    if (cJSON* jeo = cJSON_GetObjectItemCaseSensitive(jwg, "edge_overrides"); cJSON_IsArray(jeo)) {
        cJSON* o = nullptr;
        cJSON_ArrayForEach(o, jeo) {
            if (!cJSON_IsObject(o)) continue;
            cJSON* jm = cJSON_GetObjectItemCaseSensitive(o, "edge");
            if (!jm) jm = cJSON_GetObjectItemCaseSensitive(o, "road");
            if (!jm) continue;
            WorldEdgeOverride ov;
            if (cJSON_IsString(jm) && jm->valuestring) ov.match = jm->valuestring;
            else if (cJSON_IsNumber(jm)) ov.match = std::to_string(static_cast<int>(jm->valuedouble));
            if (ov.match.empty()) continue;

            if (cJSON* j = cJSON_GetObjectItemCaseSensitive(o, "poles"); cJSON_IsBool(j)) {
                ov.has_poles = true;
                ov.poles = cJSON_IsTrue(j);
            }
            if (cJSON* j = cJSON_GetObjectItemCaseSensitive(o, "trees"); cJSON_IsBool(j)) {
                ov.has_trees = true;
                ov.trees = cJSON_IsTrue(j);
            }
            if (cJSON* j = cJSON_GetObjectItemCaseSensitive(o, "barriers"); cJSON_IsBool(j)) {
                ov.has_barriers = true;
                ov.barriers = cJSON_IsTrue(j);
            }
            if (cJSON* j = cJSON_GetObjectItemCaseSensitive(o, "signs"); cJSON_IsBool(j)) {
                ov.has_signs = true;
                ov.signs = cJSON_IsTrue(j);
            }
            if (cJSON* j = cJSON_GetObjectItemCaseSensitive(o, "furniture"); cJSON_IsBool(j)) {
                ov.has_furniture = true;
                ov.furniture = cJSON_IsTrue(j);
            }
            if (cJSON* j = cJSON_GetObjectItemCaseSensitive(o, "pole_spacing_m");
                cJSON_IsNumber(j) && j->valuedouble > 2.0) {
                ov.pole_spacing_m = j->valuedouble;
            }
            if (cJSON* j = cJSON_GetObjectItemCaseSensitive(o, "tree_spacing_m");
                cJSON_IsNumber(j) && j->valuedouble > 2.0) {
                ov.tree_spacing_m = j->valuedouble;
            }
            if (cJSON* j = cJSON_GetObjectItemCaseSensitive(o, "speed_limit_kph");
                cJSON_IsNumber(j) && j->valuedouble > 5.0) {
                ov.speed_limit_kph = j->valuedouble;
            }
            cfg.edge_overrides.push_back(std::move(ov));
        }
    }

    if (cJSON* jcp = cJSON_GetObjectItemCaseSensitive(jwg, "custom_props"); cJSON_IsArray(jcp)) {
        cJSON* p = nullptr;
        int idx = 0;
        cJSON_ArrayForEach(p, jcp) {
            if (!cJSON_IsObject(p)) continue;
            WorldCustomPropSpec sp;
            cJSON* jid = cJSON_GetObjectItemCaseSensitive(p, "id");
            sp.id = (cJSON_IsString(jid) && jid->valuestring)
                ? jid->valuestring
                : ("prop_" + std::to_string(idx));
            cJSON* jkind = cJSON_GetObjectItemCaseSensitive(p, "kind");
            if (!cJSON_IsString(jkind) || !jkind->valuestring) continue;
            sp.kind = jkind->valuestring;
            if (cJSON* jl = cJSON_GetObjectItemCaseSensitive(p, "label");
                cJSON_IsString(jl) && jl->valuestring) {
                sp.label = jl->valuestring;
            }
            if (cJSON* jpl = cJSON_GetObjectItemCaseSensitive(p, "placement");
                cJSON_IsString(jpl) && jpl->valuestring) {
                sp.placement = jpl->valuestring;
            }
            if (cJSON* jrd = cJSON_GetObjectItemCaseSensitive(p, "road")) {
                if (cJSON_IsString(jrd) && jrd->valuestring) sp.road = jrd->valuestring;
                else if (cJSON_IsNumber(jrd)) sp.road = std::to_string(static_cast<int>(jrd->valuedouble));
                if (sp.placement.empty()) sp.placement = "road_s";
            }
            if (sp.placement.empty()) sp.placement = "world";
            auto get_prop_d = [&](const char* k, double& dst) {
                cJSON* j = cJSON_GetObjectItemCaseSensitive(p, k);
                if (cJSON_IsNumber(j)) dst = j->valuedouble;
            };
            get_prop_d("s", sp.s);
            get_prop_d("d", sp.d);
            get_prop_d("x", sp.x);
            get_prop_d("y", sp.y);
            get_prop_d("z", sp.z);
            get_prop_d("z_offset", sp.z_offset);
            get_prop_d("heading", sp.heading);
            get_prop_d("heading_offset", sp.heading_offset);
            get_prop_d("length", sp.length);
            get_prop_d("width", sp.width);
            get_prop_d("height", sp.height);
            if (cJSON* jaz = cJSON_GetObjectItemCaseSensitive(p, "auto_z"); cJSON_IsBool(jaz)) {
                sp.auto_z = cJSON_IsTrue(jaz);
            }
            if (cJSON* jcol = cJSON_GetObjectItemCaseSensitive(p, "collider"); cJSON_IsBool(jcol)) {
                sp.has_collider = cJSON_IsTrue(jcol);
                sp.collider_explicit = true;
            } else {
                const bool is_overhead =
                    (sp.kind == "overhead_gantry" || sp.kind == "arch_landmark" ||
                     sp.kind == "gantry" || sp.kind == "arch" ||
                     sp.kind == "banner" || sp.kind == "decal");
                sp.has_collider = !is_overhead;
            }
            cfg.custom_props.push_back(std::move(sp));
            idx++;
        }
    }

    return cfg;
}

std::vector<CompiledEdge> build_compiled_edges(cJSON* rn_root, const WorldGenConfig& cfg) {
    std::vector<CompiledEdge> edges;
    if (!rn_root) return edges;
    cJSON* earr = cJSON_GetObjectItemCaseSensitive(rn_root, "edges");
    if (!cJSON_IsArray(earr)) earr = cJSON_GetObjectItemCaseSensitive(rn_root, "roads");
    if (!cJSON_IsArray(earr)) return edges;

    cJSON* lane_data_obj = cJSON_GetObjectItemCaseSensitive(rn_root, "lane_data");

    cJSON* edge = nullptr;
    int auto_idx = 0;
    cJSON_ArrayForEach(edge, earr) {
        if (!cJSON_IsObject(edge)) continue;
        cJSON* jnodes = cJSON_GetObjectItemCaseSensitive(edge, "nodes");
        if (!cJSON_IsArray(jnodes)) jnodes = cJSON_GetObjectItemCaseSensitive(edge, "centerline");
        if (!cJSON_IsArray(jnodes) || cJSON_GetArraySize(jnodes) < 2) continue;

        CompiledEdge ce;
        cJSON* jid = cJSON_GetObjectItemCaseSensitive(edge, "id");
        ce.id = cJSON_IsNumber(jid) ? static_cast<int>(jid->valuedouble) : auto_idx;
        ce.id_str = (cJSON_IsString(jid) && jid->valuestring)
            ? jid->valuestring
            : std::to_string(ce.id);
        cJSON* jname = cJSON_GetObjectItemCaseSensitive(edge, "name");
        ce.name = (cJSON_IsString(jname) && jname->valuestring) ? jname->valuestring : "";
        cJSON* jtype = cJSON_GetObjectItemCaseSensitive(edge, "type");
        ce.type = (cJSON_IsString(jtype) && jtype->valuestring) ? jtype->valuestring : "urban";

        cJSON* jlanes = cJSON_GetObjectItemCaseSensitive(edge, "lanes");
        if (cJSON_IsNumber(jlanes)) ce.lanes = std::max(1, static_cast<int>(jlanes->valuedouble));
        else if (cJSON_IsArray(jlanes) && cJSON_GetArraySize(jlanes) > 0) ce.lanes = cJSON_GetArraySize(jlanes);
        cJSON* jlw = cJSON_GetObjectItemCaseSensitive(edge, "lane_width");
        ce.lane_width = (cJSON_IsNumber(jlw) && jlw->valuedouble > 0.5) ? jlw->valuedouble : 3.5;
        ce.half_w = ce.lanes * ce.lane_width * 0.5;

        cJSON* jsl = cJSON_GetObjectItemCaseSensitive(edge, "speed_limit");
        if (cJSON_IsNumber(jsl) && jsl->valuedouble > 0.5) {
            ce.speed_limit_mps = jsl->valuedouble;
        }
        /* 类型表盖过场景里写平的限速。edge_overrides.speed_limit_kph 在后面仍可再盖。 */
        ce.speed_limit_mps = flowengine::posted_speed_mps(ce.type.c_str(), ce.speed_limit_mps);
        cJSON* jow = cJSON_GetObjectItemCaseSensitive(edge, "oneway");
        ce.is_oneway = cJSON_IsTrue(jow);

        cJSON* jbridge = cJSON_GetObjectItemCaseSensitive(edge, "bridge");
        cJSON* jtunnel = cJSON_GetObjectItemCaseSensitive(edge, "tunnel");
        ce.is_subsurface = (ce.type == "garage_hall" || ce.type == "open_ramp");
        ce.is_viaduct_special = (ce.type == "viaduct_highway" || ce.name == "viaduct_highway");
        ce.is_bridge = cJSON_IsTrue(jbridge) || ce.is_viaduct_special || (ce.type == "ramp_curve");
        ce.is_tunnel = cJSON_IsTrue(jtunnel) || contains_tunnel_keyword(ce.name) || contains_tunnel_keyword(ce.id_str);

        int npts = cJSON_GetArraySize(jnodes);
        ce.raw_pts.reserve(static_cast<size_t>(npts));
        for (int k = 0; k < npts; ++k) {
            cJSON* pk = cJSON_GetArrayItem(jnodes, k);
            if (!cJSON_IsArray(pk) || cJSON_GetArraySize(pk) < 2) continue;
            double x = cJSON_GetArrayItem(pk, 0)->valuedouble;
            double y = cJSON_GetArrayItem(pk, 1)->valuedouble;
            double z = cJSON_GetArraySize(pk) > 2 ? cJSON_GetArrayItem(pk, 2)->valuedouble : 0.0;
            if (!is_finite_pos(x, y, z)) continue;
            ce.raw_pts.push_back({x, y, z});
        }
        if (ce.raw_pts.size() < 2) continue;

        bool has_explicit_lane_data = false;
        if (cJSON_IsObject(lane_data_obj) && !ce.name.empty()) {
            if (cJSON_GetObjectItemCaseSensitive(lane_data_obj, ce.name.c_str()) != nullptr) {
                has_explicit_lane_data = true;
            }
        }

        ce.pts = resample_spine(ce.raw_pts);
        if (ce.is_oneway && !ce.is_subsurface && !has_explicit_lane_data) {
            shift_oneway_center(ce.pts, ce.half_w);
        }

        ce.cum.assign(ce.pts.size(), 0.0);
        ce.max_z = ce.pts[0].z;
        ce.min_z = ce.pts[0].z;
        for (size_t k = 1; k < ce.pts.size(); ++k) {
            ce.cum[k] = ce.cum[k - 1] + std::hypot(ce.pts[k].x - ce.pts[k - 1].x,
                                                     ce.pts[k].y - ce.pts[k - 1].y);
            ce.max_z = std::max(ce.max_z, ce.pts[k].z);
            ce.min_z = std::min(ce.min_z, ce.pts[k].z);
        }
        ce.total_len = ce.cum.back();
        if (ce.max_z >= 2.2) ce.is_bridge = true;

        /* Apply global world_gen defaults + per-edge overrides */
        ce.gen_poles = cfg.auto_poles;
        ce.gen_trees = cfg.auto_trees;
        ce.gen_barriers = cfg.auto_barriers &&
                          (ce.is_bridge || ce.type == "highway" || ce.type == "ramp_curve" ||
                           cfg.preset == "highway_corridor");
        ce.gen_signs = cfg.auto_signs;
        ce.gen_furniture = cfg.auto_furniture;
        ce.pole_spacing_m = cfg.pole_spacing_m;
        ce.tree_spacing_m = cfg.tree_spacing_m;

        for (const auto& ov : cfg.edge_overrides) {
            if (ov.match == ce.name || ov.match == ce.id_str) {
                if (ov.has_poles) ce.gen_poles = ov.poles;
                if (ov.has_trees) ce.gen_trees = ov.trees;
                if (ov.has_barriers) ce.gen_barriers = ov.barriers;
                if (ov.has_signs) ce.gen_signs = ov.signs;
                if (ov.has_furniture) ce.gen_furniture = ov.furniture;
                if (ov.pole_spacing_m > 0.0) ce.pole_spacing_m = ov.pole_spacing_m;
                if (ov.tree_spacing_m > 0.0) ce.tree_spacing_m = ov.tree_spacing_m;
                if (ov.speed_limit_kph > 0.0) ce.speed_limit_mps = ov.speed_limit_kph / 3.6;
            }
        }

        edges.push_back(std::move(ce));
        auto_idx++;
    }
    return edges;
}

/* ── Procedural Passes ───────────────────────────────────────── */

void pass_subsurface_and_parking(const std::vector<CompiledEdge>& edges,
                                 const WorldEngineInput& input,
                                 WorldLayout& layout) {
    for (const auto& ce : edges) {
        if (ce.type == "garage_hall" && ce.raw_pts.size() >= 2) {
            const auto& p0 = ce.raw_pts.front();
            const auto& p1 = ce.raw_pts.back();
            double len = std::hypot(p1.x - p0.x, p1.y - p0.y);
            double hdg = std::atan2(p1.y - p0.y, p1.x - p0.x);
            double ch = std::cos(hdg), sh = std::sin(hdg);
            constexpr double kHallHalfW = 11.6;
            const double pillar_s[] = {2.0, 19.1, 31.5, 48.0, 64.0, 82.0, 96.0, 110.0, 124.0};
            for (double ps : pillar_s) {
                if (ps >= len - 2.0) continue;
                for (double side : {-1.0, 1.0}) {
                    double lat = side * (kHallHalfW - 0.85);
                    layout.furniture.push_back({
                        p0.x + ch * ps - sh * lat,
                        p0.y + sh * ps + ch * lat,
                        p0.z, hdg, 0.60, 0.60, 3.2, "pillar", true, true
                    });
                }
            }
            for (double s = 0.0; s < len; s += 10.0) {
                double seg_len = std::min(10.0, len - s);
                double sm = s + seg_len * 0.5;
                for (double side : {-1.0, 1.0}) {
                    double lat = side * (kHallHalfW + 0.15);
                    layout.furniture.push_back({
                        p0.x + ch * sm - sh * lat,
                        p0.y + sh * sm + ch * lat,
                        p0.z, hdg, seg_len, 0.40, 3.2, "wall", true, true
                    });
                }
            }
            for (double lat = -8.0; lat <= 8.0; lat += 8.0) {
                layout.furniture.push_back({
                    p0.x - sh * lat, p0.y + ch * lat,
                    p0.z, hdg, 0.40, 8.0, 3.2, "wall", true, true
                });
            }
        } else if (ce.type == "open_ramp" && ce.raw_pts.size() >= 2) {
            double lat_w = ce.lanes * ce.lane_width * 0.5 + 0.45;
            for (size_t k = 1; k < ce.raw_pts.size(); ++k) {
                const auto& pa = ce.raw_pts[k - 1];
                const auto& pb = ce.raw_pts[k];
                double slen = std::hypot(pb.x - pa.x, pb.y - pa.y);
                if (slen < 1.0) continue;
                double shdg = std::atan2(pb.y - pa.y, pb.x - pa.x);
                double sc = std::cos(shdg), ss = std::sin(shdg);
                for (double s = 0.0; s < slen; s += 10.0) {
                    double seg = std::min(10.0, slen - s);
                    double sm = s + seg * 0.5;
                    double t = sm / slen;
                    double mz = pa.z + (pb.z - pa.z) * t;
                    for (double side : {-1.0, 1.0}) {
                        double lat = side * lat_w;
                        layout.furniture.push_back({
                            pa.x + sc * sm - ss * lat,
                            pa.y + ss * sm + sc * lat,
                            mz, shdg, seg, 0.35, 1.5, "wall", true, true
                        });
                    }
                }
            }
        }
    }

    for (const auto& eg : input.etc_gates) {
        double cg = std::cos(eg.heading), sg = std::sin(eg.heading);
        double lat = -2.25;
        layout.furniture.push_back({
            eg.x - sg * lat, eg.y + cg * lat, eg.z,
            eg.heading, 1.6, 0.7, 1.5, "gate", true, true
        });
    }

    if (!input.parking_slots.empty()) {
        double min_sx = 1e9, max_sx = -1e9;
        bool has_surface = false;
        double surf_y = 0.0, surf_z = 0.0, surf_h = 1.5708;
        double slot_len = 5.6, slot_wid = 2.7;
        for (const auto& ps : input.parking_slots) {
            if (std::fabs(ps.z) < 1.2) {
                min_sx = std::min(min_sx, ps.x);
                max_sx = std::max(max_sx, ps.x);
                if (!has_surface) {
                    surf_y = ps.y;
                    surf_z = ps.z;
                    surf_h = (ps.heading != 0.0) ? ps.heading : 1.5708;
                    if (ps.length > 0.0) slot_len = ps.length;
                    if (ps.width > 0.0) slot_wid = ps.width;
                    has_surface = true;
                }
            }
        }
        if (has_surface && min_sx < max_sx) {
            double west_x = (min_sx - 4.0) - 5.65;
            double east_x = (max_sx + 4.0) + 8.5;
            layout.furniture.push_back({west_x, -6.5, 0.0, 0.0, 4.6, 5.1, 0.70, "planter", true, true});
            layout.furniture.push_back({east_x, -6.5, 0.0, 0.0, 4.6, 5.1, 0.70, "planter", true, true});

            std::vector<double> bay_xs = {
                min_sx - 4.0, min_sx, (min_sx + max_sx) * 0.5, max_sx, max_sx + 4.0
            };
            std::vector<double> uniq;
            auto add_u = [&](double bx) {
                for (double u : uniq) if (std::fabs(u - bx) < 1.2) return;
                uniq.push_back(bx);
            };
            for (double bx : bay_xs) add_u(bx);
            for (const auto& ps : input.parking_slots) {
                if (std::fabs(ps.z) < 1.2) add_u(ps.x);
            }
            std::sort(uniq.begin(), uniq.end());
            if (!uniq.empty()) {
                double cb = std::cos(surf_h), sb = std::sin(surf_h);
                double stop_off = -slot_len * 0.5 + 0.65;
                for (double bx : uniq) {
                    layout.furniture.push_back({
                        bx + cb * stop_off, surf_y + sb * stop_off, surf_z,
                        surf_h, 0.16, std::max(0.8, slot_wid * 0.72), 0.18, "wheel_stop", true, true
                    });
                    layout.furniture.push_back({
                        bx - 1.95, surf_y - 3.05, surf_z,
                        surf_h, 0.18, 0.18, 0.78, "bollard", true, true
                    });
                }
                layout.furniture.push_back({
                    uniq.back() + 1.95, surf_y - 3.05, surf_z,
                    surf_h, 0.18, 0.18, 0.78, "bollard", true, true
                });
                size_t ch_start = uniq.size() > 3 ? uniq.size() - 3 : 0;
                for (size_t ci = ch_start; ci < uniq.size(); ++ci) {
                    layout.furniture.push_back({
                        uniq[ci], surf_y - 3.55, surf_z,
                        surf_h, 0.42, 0.34, 1.55, "charger", true, true
                    });
                }
                double west_planter_x = uniq.front() - 5.65;
                double pad_min_x = west_planter_x - 3.6;
                double gate_island_x = std::min(3198.0, pad_min_x - 6.0);
                layout.furniture.push_back({
                    gate_island_x, -4.5, 0.0, 0.0, 6.2, 1.25, 1.50, "barrier", true, true
                });
            }
        }
        for (const auto& ps : input.parking_slots) {
            if (std::fabs(ps.z) < 1.2) continue;
            double sh = (ps.heading != 0.0) ? ps.heading : 1.5708;
            double len = (ps.length > 0.0) ? ps.length : 5.6;
            double wid = (ps.width > 0.0) ? ps.width : 2.7;
            double stop_off = -len * 0.5 + 0.65;
            layout.furniture.push_back({
                ps.x + std::cos(sh) * stop_off, ps.y + std::sin(sh) * stop_off, ps.z,
                sh, 0.16, std::max(0.8, wid * 0.72), 0.18, "wheel_stop", true, true
            });
        }
    }

    if (input.has_parking_goal) {
        const auto& pg = input.parking_goal;
        bool covered = false;
        for (const auto& ps : input.parking_slots) {
            if (std::hypot(ps.x - pg.x, ps.y - pg.y) < 1.5 && std::fabs(ps.z - pg.z) < 1.5) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            double gh = pg.heading;
            double glen = pg.length > 0.0 ? pg.length : 6.0;
            double gwid = pg.width > 0.0 ? pg.width : 2.8;
            double cg = std::cos(gh), sg = std::sin(gh);
            double stop_off = -glen * 0.5 + 0.65;
            layout.furniture.push_back({
                pg.x + cg * stop_off, pg.y + sg * stop_off, pg.z,
                gh, 0.16, std::max(0.8, gwid * 0.72), 0.18, "wheel_stop", true, true
            });
            if (std::fabs(pg.z) < 1.2) {
                double ch_off = -glen * 0.5 - 0.55;
                layout.furniture.push_back({
                    pg.x + cg * ch_off, pg.y + sg * ch_off, pg.z,
                    gh, 0.42, 0.34, 1.55, "charger", true, true
                });
                double bol_back = -glen * 0.5 - 0.05;
                double bol_lat = gwid * 0.5 + 0.30;
                for (double side : {-1.0, 1.0}) {
                    layout.furniture.push_back({
                        pg.x + cg * bol_back - sg * (side * bol_lat),
                        pg.y + sg * bol_back + cg * (side * bol_lat),
                        pg.z, gh, 0.18, 0.18, 0.78, "bollard", true, true
                    });
                }
            }
        }
    }
}

bool is_surface_corridor_edge(const CompiledEdge& ce) {
    if (ce.is_subsurface || ce.is_viaduct_special) return false;
    return true;
}

void pass_poles(const std::vector<CompiledEdge>& edges,
                const SpatialField& field,
                WorldLayout& layout) {
    for (size_t ei = 0; ei < edges.size(); ++ei) {
        const auto& ce = edges[ei];
        if (!is_surface_corridor_edge(ce) || !ce.gen_poles || ce.total_len < 28.0) continue;
        double spacing = std::max(8.0, ce.pole_spacing_m);
        double lat_p = ce.half_w + 1.5;
        int pole_count = static_cast<int>(std::floor(ce.total_len / spacing));
        for (int pi = 0; pi < pole_count; ++pi) {
            double s = (pi + 0.5) * spacing;
            double cx, cy, cz, hdg;
            ce.sample_at_s(s, cx, cy, cz, hdg);
            double sc = std::cos(hdg), ss = std::sin(hdg);

            double side = ce.is_oneway ? 1.0 : ((pi % 2 == 0) ? -1.0 : 1.0);
            auto try_place_pole = [&](double cand_side, WorldPole& out_pole) -> bool {
                double lat = cand_side * lat_p;
                double px = cx - ss * lat;
                double py = cy + sc * lat;
                if (field.in_junction(px, py, 3.0)) {
                    layout.sanity.pruned_junction++;
                    return false;
                }
                if (field.inside_road_band(px, py, cz, -0.55)) {
                    layout.sanity.pruned_asphalt++;
                    return false;
                }
                if (field.near_parking_cutout(px, py, cz)) {
                    layout.sanity.pruned_cutout++;
                    return false;
                }
                if (field.is_excluded(px, py, cz, WORLD_MASK_POLE)) {
                    layout.sanity.pruned_excluded++;
                    return false;
                }
                out_pole.x = px;
                out_pole.y = py;
                out_pole.z = cz;
                out_pole.nx = -ss * cand_side;
                out_pole.ny = sc * cand_side;
                out_pole.heading = hdg;
                out_pole.side = cand_side > 0.0 ? 1 : -1;
                out_pole.is_double_arm = false;
                out_pole.edge_id = ce.id_str;
                return true;
            };

            WorldPole wp;
            if (try_place_pole(side, wp)) {
                layout.poles.push_back(std::move(wp));
            } else if (!ce.is_oneway && try_place_pole(-side, wp)) {
                layout.poles.push_back(std::move(wp));
            }
        }
    }
}

void pass_trees(const std::vector<CompiledEdge>& edges,
                const SpatialField& field,
                const WorldGenConfig& cfg,
                WorldLayout& layout) {
    for (size_t ei = 0; ei < edges.size(); ++ei) {
        const auto& ce = edges[ei];
        if (!is_surface_corridor_edge(ce) || !ce.gen_trees || ce.is_bridge || ce.is_tunnel) continue;
        if (ce.total_len < 10.0) continue;
        double spacing = std::max(8.0, ce.tree_spacing_m);
        double lat_t = ce.half_w + 3.0;
        int tree_count = static_cast<int>(std::floor((ce.total_len - 5.0) / spacing)) + 1;
        for (int ti = 0; ti < tree_count; ++ti) {
            double s = 5.0 + ti * spacing;
            if (s > ce.total_len) break;
            double cx, cy, cz, hdg;
            ce.sample_at_s(s, cx, cy, cz, hdg);
            if (std::fabs(cz) >= 0.8) continue;
            if (ce.name == "surface_lot_avenue" && cx >= 3200.0 && cx <= 3250.0) continue;

            double sc = std::cos(hdg), ss = std::sin(hdg);
            for (double side : {-1.0, 1.0}) {
                double lat = side * lat_t;
                double tx = cx - ss * lat;
                double ty = cy + sc * lat;
                if (field.in_junction(tx, ty, 2.0)) {
                    layout.sanity.pruned_junction++;
                    continue;
                }
                if (field.near_parking_cutout(tx, ty, cz)) {
                    layout.sanity.pruned_cutout++;
                    continue;
                }
                if (field.inside_road_band(tx, ty, cz, 1.4 - 0.55)) {
                    layout.sanity.pruned_asphalt++;
                    continue;
                }
                if (field.is_excluded(tx, ty, cz, WORLD_MASK_TREE)) {
                    layout.sanity.pruned_excluded++;
                    continue;
                }
                int side_tag = (side > 0.0) ? 7 : 3;
                double variant = static_cast<double>((ti * 17 + side_tag) % 11) / 10.0;
                if (hash_unit(cfg.seed, ei, static_cast<uint64_t>(ti), static_cast<uint64_t>(side_tag)) > 0.999) {
                    variant = 0.5;
                }
                layout.trees.push_back({
                    tx, ty, cz,
                    variant * 2.0 * kPi,
                    variant,
                    0.88 + variant * 0.24,
                    0.90 + variant * 0.18,
                    side > 0.0 ? 1 : -1,
                    cfg.tree_species
                });
            }
        }
    }
}

void pass_barriers(const std::vector<CompiledEdge>& edges,
                   const SpatialField& field,
                   const WorldGenConfig& cfg,
                   WorldLayout& layout) {
    const double post_spacing = std::max(2.0, cfg.barrier_spacing_m);
    const double post_offset = 0.4;

    for (size_t ei = 0; ei < edges.size(); ++ei) {
        const auto& ce = edges[ei];
        if (ce.is_subsurface || ce.is_viaduct_special || !ce.gen_barriers || ce.total_len < 12.0) continue;

        int post_count = static_cast<int>(std::floor(ce.total_len / post_spacing));
        if (post_count <= 0) continue;

        for (double side : {1.0, -1.0}) {
            double side_off = (ce.half_w + post_offset) * side;
            bool has_prev = false;
            Pt3 prev_pt{};
            for (int i = 0; i <= post_count; ++i) {
                double s = i * post_spacing;
                double cx, cy, cz, hdg;
                ce.sample_at_s(s, cx, cy, cz, hdg);
                double sc = std::cos(hdg), ss = std::sin(hdg);
                double px = cx - ss * side_off;
                double py = cy + sc * side_off;

                if (field.in_junction(cx, cy, 0.0) || field.in_junction(px, py, 0.5)) {
                    has_prev = false;
                    layout.sanity.pruned_junction++;
                    continue;
                }
                if (field.is_excluded(px, py, cz, WORLD_MASK_BARRIER)) {
                    has_prev = false;
                    layout.sanity.pruned_excluded++;
                    continue;
                }
                if (has_prev) {
                    double chord = std::hypot(px - prev_pt.x, py - prev_pt.y);
                    if (chord < post_spacing * 0.35) {
                        has_prev = false;
                        continue;
                    }
                }
                layout.barrier_posts.push_back({px, py, cz, hdg});
                if (has_prev) {
                    double dx = px - prev_pt.x, dy = py - prev_pt.y;
                    double slen = std::hypot(dx, dy);
                    if (slen > 0.01) {
                        layout.barrier_spans.push_back({
                            prev_pt.x, prev_pt.y, prev_pt.z,
                            px, py, cz,
                            0.5 * (prev_pt.x + px),
                            0.5 * (prev_pt.y + py),
                            0.5 * (prev_pt.z + cz),
                            slen,
                            std::atan2(dy, dx)
                        });
                    }
                }
                prev_pt = {px, py, cz};
                has_prev = true;
            }
        }
    }
}

void pass_piers(const std::vector<CompiledEdge>& edges,
                const SpatialField& field,
                const WorldGenConfig& cfg,
                WorldLayout& layout) {
    if (!cfg.auto_piers) return;
    double step = std::max(12.0, cfg.pier_spacing_m);
    for (const auto& ce : edges) {
        if ((!ce.is_bridge && ce.max_z < 1.5) || ce.is_subsurface || ce.is_viaduct_special) continue;
        if (ce.total_len < 12.0) continue;

        double pier_hw = ce.half_w + 0.6;
        double last_s = -1e9;
        for (double s = 12.0; s < ce.total_len; s += step) {
            double cx, cy, cz, hdg;
            ce.sample_at_s(s, cx, cy, cz, hdg);
            if (cz < 2.2 || (s - last_s) < 19.5) continue;
            if (field.is_excluded(cx, cy, cz, WORLD_MASK_PIER)) {
                layout.sanity.pruned_excluded++;
                continue;
            }

            double nx = -std::sin(hdg), ny = std::cos(hdg);
            if (pier_hw < 6.0) {
                if (field.hits_lower_asphalt(cx, cy, cz, 1.0)) {
                    /* Avoid placing single central pier inside lower road deck: straddle with 2 columns */
                    double col_off = std::max(pier_hw + 1.4, 5.2);
                    double c1x = cx - nx * col_off, c1y = cy - ny * col_off;
                    double c2x = cx + nx * col_off, c2y = cy + ny * col_off;
                    if (!field.hits_lower_asphalt(c1x, c1y, cz, 0.8) &&
                        !field.hits_lower_asphalt(c2x, c2y, cz, 0.8)) {
                        layout.piers.push_back({
                            cx, cy, 0.0, cz - 0.45, hdg, pier_hw, 2, col_off, 0.72,
                            col_off * 2.2, 1.7, 0.75
                        });
                    }
                } else {
                    layout.piers.push_back({
                        cx, cy, 0.0, cz - 0.45, hdg, pier_hw, 1, 0.0, 0.75,
                        std::max(1.8, pier_hw * 1.4), 1.5, 0.65
                    });
                }
            } else {
                double col_off = pier_hw * 0.48;
                double c1x = cx - nx * col_off, c1y = cy - ny * col_off;
                double c2x = cx + nx * col_off, c2y = cy + ny * col_off;
                if (field.hits_lower_asphalt(c1x, c1y, cz, 0.8) ||
                    field.hits_lower_asphalt(c2x, c2y, cz, 0.8)) {
                    col_off = pier_hw + 1.6;
                    c1x = cx - nx * col_off; c1y = cy - ny * col_off;
                    c2x = cx + nx * col_off; c2y = cy + ny * col_off;
                    if (field.hits_lower_asphalt(c1x, c1y, cz, 0.8) ||
                        field.hits_lower_asphalt(c2x, c2y, cz, 0.8)) {
                        continue;
                    }
                }
                layout.piers.push_back({
                    cx, cy, 0.0, cz - 0.45, hdg, pier_hw, 2, col_off, 0.75,
                    std::max(pier_hw * 1.95, col_off * 2.15), 1.9, 0.85
                });
            }
            last_s = s;
        }
    }
}

bool spot_clear_for_roadside_prop(const SpatialField& field, double x, double y, double z,
                                  double asphalt_margin, uint32_t kind_mask,
                                  bool use_tight_asphalt = false,
                                  bool allow_near_parking = false) {
    if (!is_finite_pos(x, y, z)) return false;
    if (field.in_junction(x, y, 3.0)) return false;
    if (field.inside_road_band(x, y, z, asphalt_margin, use_tight_asphalt)) return false;
    if (!allow_near_parking && field.near_parking_cutout(x, y, z, 1.0)) return false;
    if (field.near_traffic_light(x, y, z, 7.5)) return false;
    if (field.is_excluded(x, y, z, kind_mask)) return false;
    return true;
}

void pass_signs(const std::vector<CompiledEdge>& edges,
                const SpatialField& field,
                WorldLayout& layout) {
    for (size_t ei = 0; ei < edges.size(); ++ei) {
        const auto& ce = edges[ei];
        if (!ce.gen_signs || ce.total_len < 24.0) continue;

        /* Subsurface garage hall & open ramp signage (B1 speed limit, exit, slope warning) */
        if (ce.type == "garage_hall") {
            const double lat = -(ce.half_w + 0.85);
            const double s_spd = std::min(16.0, ce.total_len * 0.20);
            {
                double cx, cy, cz, hdg;
                ce.sample_at_s(s_spd, cx, cy, cz, hdg);
                double sc = std::cos(hdg), ss = std::sin(hdg);
                double sx = cx - ss * lat, sy = cy + sc * lat;
                if (spot_clear_for_roadside_prop(field, sx, sy, cz, 0.55, WORLD_MASK_SIGN, true, true)) {
                    layout.signs.push_back({sx, sy, cz, hdg + kPi, "speed_limit", "15", 15});
                }
            }
            if (ce.total_len >= 48.0) {
                double s_exit = std::max(s_spd + 20.0, ce.total_len - 20.0);
                double cx, cy, cz, hdg;
                ce.sample_at_s(s_exit, cx, cy, cz, hdg);
                double sc = std::cos(hdg), ss = std::sin(hdg);
                double sx = cx - ss * lat, sy = cy + sc * lat;
                if (spot_clear_for_roadside_prop(field, sx, sy, cz, 0.55, WORLD_MASK_SIGN, true, true)) {
                    layout.signs.push_back({sx, sy, cz, hdg + kPi, "exit_sign", "EXIT", 0});
                }
            }
            continue;
        }
        if (ce.type == "open_ramp") {
            const double lat = -(ce.half_w + 1.10);
            const double s_spd = std::min(18.0, ce.total_len * 0.24);
            {
                double cx, cy, cz, hdg;
                ce.sample_at_s(s_spd, cx, cy, cz, hdg);
                double sc = std::cos(hdg), ss = std::sin(hdg);
                double sx = cx - ss * lat, sy = cy + sc * lat;
                if (spot_clear_for_roadside_prop(field, sx, sy, cz, 0.70, WORLD_MASK_SIGN, true, true)) {
                    layout.signs.push_back({sx, sy, cz, hdg + kPi, "speed_limit", "25", 25});
                }
            }
            if (ce.total_len >= 50.0) {
                double s_warn = std::min(ce.total_len * 0.60, 55.0);
                double cx, cy, cz, hdg;
                ce.sample_at_s(s_warn, cx, cy, cz, hdg);
                double sc = std::cos(hdg), ss = std::sin(hdg);
                double sx = cx - ss * lat, sy = cy + sc * lat;
                if (spot_clear_for_roadside_prop(field, sx, sy, cz, 0.70, WORLD_MASK_SIGN, true, true)) {
                    layout.signs.push_back({sx, sy, cz, hdg + kPi, "slope_warn", "SLOPE", 0});
                }
            }
            continue;
        }

        if (!is_surface_corridor_edge(ce) || ce.is_tunnel) continue;

        /* 1) Speed limit signs: entry sign + periodic repeaters every 120m on long corridors */
        int raw_kph = static_cast<int>(std::llround(ce.speed_limit_mps * 3.6 / 5.0)) * 5;
        int kph = std::max(20, std::min(120, raw_kph > 0 ? raw_kph : 60));
        double s_spd = std::min(30.0, ce.total_len * 0.22);
        for (double s_cur = s_spd; s_cur <= ce.total_len - 18.0; s_cur += 120.0) {
            double cx, cy, cz, hdg;
            ce.sample_at_s(s_cur, cx, cy, cz, hdg);
            double sc = std::cos(hdg), ss = std::sin(hdg);
            double lat = -(ce.half_w + 1.55); /* ENU right side for forward traffic */
            double sx = cx - ss * lat;
            double sy = cy + sc * lat;
            if (spot_clear_for_roadside_prop(field, sx, sy, cz, 0.8, WORLD_MASK_SIGN)) {
                layout.signs.push_back({
                    sx, sy, cz, hdg + kPi, "speed_limit", std::to_string(kph), kph
                });
            }
            if (ce.total_len < 150.0) break;
        }

        /* 2) Crosswalk / Junction warning or STOP sign before downstream 3+ arm intersection */
        if (ce.total_len > 36.0) {
            double s_end = std::max(12.0, ce.total_len - 22.0);
            double cx, cy, cz, hdg;
            ce.sample_at_s(s_end, cx, cy, cz, hdg);
            const auto& end_pt = ce.pts.back();
            if (field.in_junction(end_pt.x, end_pt.y, 1.0)) {
                double sc = std::cos(hdg), ss = std::sin(hdg);
                double lat = -(ce.half_w + 1.55);
                double sx = cx - ss * lat;
                double sy = cy + sc * lat;
                if (spot_clear_for_roadside_prop(field, sx, sy, cz, 0.8, WORLD_MASK_SIGN)) {
                    std::string kind = (ei % 2 == 0) ? "stop_sign" : "crosswalk_warn";
                    layout.signs.push_back({sx, sy, cz, hdg + kPi, kind, "", 0});
                }
            }
        }

        /* 3) Curve warning sign ahead of sharp curve vertex */
        if (ce.raw_pts.size() >= 3 && ce.total_len > 45.0) {
            double h0 = std::atan2(ce.raw_pts[1].y - ce.raw_pts[0].y, ce.raw_pts[1].x - ce.raw_pts[0].x);
            double h1 = std::atan2(ce.raw_pts.back().y - ce.raw_pts[ce.raw_pts.size() - 2].y,
                                   ce.raw_pts.back().x - ce.raw_pts[ce.raw_pts.size() - 2].x);
            double diff = std::fabs(std::atan2(std::sin(h1 - h0), std::cos(h1 - h0)));
            if (diff > 0.38) {
                double s_warn = std::min(ce.total_len * 0.45, 25.0);
                double cx, cy, cz, hdg;
                ce.sample_at_s(s_warn, cx, cy, cz, hdg);
                double sc = std::cos(hdg), ss = std::sin(hdg);
                double lat = -(ce.half_w + 1.65);
                double sx = cx - ss * lat;
                double sy = cy + sc * lat;
                if (spot_clear_for_roadside_prop(field, sx, sy, cz, 0.8, WORLD_MASK_SIGN)) {
                    layout.signs.push_back({sx, sy, cz, hdg + kPi, "curve_warn", "", 0});
                }
            }
        }
    }
}

void pass_furniture(const std::vector<CompiledEdge>& edges,
                    const SpatialField& field,
                    const WorldGenConfig& cfg,
                    WorldLayout& layout) {
    for (size_t ei = 0; ei < edges.size(); ++ei) {
        const auto& ce = edges[ei];
        if (!is_surface_corridor_edge(ce) || !ce.gen_furniture || ce.is_bridge || ce.is_tunnel) continue;
        if (ce.total_len < 45.0) continue;

        /* Fire hydrants spaced deterministically on wide shoulders */
        double s_hyd = ce.total_len * 0.35;
        {
            double cx, cy, cz, hdg;
            ce.sample_at_s(s_hyd, cx, cy, cz, hdg);
            if (std::fabs(cz) < 0.8) {
                double sc = std::cos(hdg), ss = std::sin(hdg);
                double lat = -(ce.half_w + 1.85);
                double fx = cx - ss * lat;
                double fy = cy + sc * lat;
                if (spot_clear_for_roadside_prop(field, fx, fy, cz, 1.1, WORLD_MASK_FURNITURE)) {
                    layout.furniture.push_back({
                        fx, fy, cz, hdg, 0.45, 0.45, 0.95, "hydrant", true, true
                    });
                }
            }
        }

        /* Bus shelters along longer surface avenues (>= 95m) */
        if (ce.total_len >= 95.0 && (ei % 2 == 0 || cfg.preset == "modern_cbd")) {
            double s_bus = clamp_d(ce.total_len * 0.58, 35.0, ce.total_len - 35.0);
            double cx, cy, cz, hdg;
            ce.sample_at_s(s_bus, cx, cy, cz, hdg);
            if (std::fabs(cz) < 0.8) {
                double sc = std::cos(hdg), ss = std::sin(hdg);
                double lat = -(ce.half_w + 2.85);
                double bx = cx - ss * lat;
                double by = cy + sc * lat;
                if (spot_clear_for_roadside_prop(field, bx, by, cz, 2.0, WORLD_MASK_FURNITURE)) {
                    layout.furniture.push_back({
                        bx, by, cz, hdg, 4.2, 1.35, 2.5, "bus_shelter", true, true
                    });
                }
            }
        }
    }
}

void pass_buildings(const std::vector<CompiledEdge>& edges,
                    const SpatialField& field,
                    const WorldEngineInput& input,
                    const WorldGenConfig& cfg,
                    WorldLayout& layout) {
    if (!input.manual_buildings.empty()) {
        layout.buildings_source = "map_buildings";
        return;
    }
    if (!cfg.auto_buildings) {
        layout.buildings_source = "none";
        return;
    }
    layout.buildings_source = "world_engine";

    constexpr double kPlaceCellM = 18.0;
    std::unordered_map<int64_t, bool> placed_grid;
    auto try_place_cell = [&](double ex, double ey, double half_diag) -> bool {
        int c0 = static_cast<int>(std::floor((ex - half_diag) / kPlaceCellM));
        int c1 = static_cast<int>(std::ceil((ex + half_diag) / kPlaceCellM));
        int r0 = static_cast<int>(std::floor((ey - half_diag) / kPlaceCellM));
        int r1 = static_cast<int>(std::ceil((ey + half_diag) / kPlaceCellM));
        for (int c = c0; c <= c1; ++c) {
            for (int r = r0; r <= r1; ++r) {
                int64_t key = (static_cast<int64_t>(c) << 32) ^ static_cast<uint32_t>(r);
                if (placed_grid.find(key) != placed_grid.end()) return false;
            }
        }
        for (int c = c0; c <= c1; ++c) {
            for (int r = r0; r <= r1; ++r) {
                int64_t key = (static_cast<int64_t>(c) << 32) ^ static_cast<uint32_t>(r);
                placed_grid[key] = true;
            }
        }
        return true;
    };

    struct LayerSpec { double offset_mul, spacing_mul, h_min, h_max; };
    const bool is_suburb = (cfg.preset == "suburban_parkway");
    const bool is_cbd = (cfg.preset == "modern_cbd");
    const LayerSpec layers[3] = {
        {1.0,  1.0,  is_suburb ? 10.0 : (is_cbd ? 22.0 : 14.0), is_suburb ? 22.0 : (is_cbd ? 58.0 : 42.0)},
        {2.65, 1.35, is_suburb ? 14.0 : (is_cbd ? 34.0 : 22.0), is_suburb ? 28.0 : (is_cbd ? 82.0 : 62.0)},
        {4.6,  1.8,  is_suburb ? 18.0 : (is_cbd ? 48.0 : 32.0), is_suburb ? 34.0 : (is_cbd ? 115.0 : 90.0)},
    };
    const double base_spacing = is_suburb ? 44.0 : (is_cbd ? 32.0 : 36.0);
    constexpr double kSetback = 18.0;
    constexpr size_t kMaxProceduralBuildings = 900;

    for (size_t ei = 0; ei < edges.size(); ++ei) {
        const auto& ce = edges[ei];
        if (!is_surface_corridor_edge(ce) || ce.is_bridge || ce.total_len < 20.0) continue;
        double base_offset = ce.half_w + kSetback;

        for (int li = 0; li < 3; ++li) {
            if (layout.buildings.size() >= kMaxProceduralBuildings) return;
            const auto& L = layers[li];
            double offset = base_offset * L.offset_mul;
            double lspace = base_spacing * L.spacing_mul;
            for (double s = lspace * 0.6; s <= ce.total_len - 10.0; s += lspace) {
                if (layout.buildings.size() >= kMaxProceduralBuildings) return;
                double cx, cy, cz, hdg;
                ce.sample_at_s(s, cx, cy, cz, hdg);
                if (std::fabs(cz) >= 1.5) continue;
                double sc = std::cos(hdg), ss = std::sin(hdg);
                double seed_u = hash_unit(cfg.seed, ei, static_cast<uint64_t>(li),
                                          static_cast<uint64_t>(std::llround(s * 10.0)));
                double h = L.h_min + seed_u * (L.h_max - L.h_min);
                double w = (12.0 + seed_u * 8.0) * (li == 0 ? 1.0 : 0.85);
                double d = (10.0 + (1.0 - seed_u) * 8.0) * (li == 0 ? 1.0 : 0.85);
                double extent_r = 0.5 * std::max(w, d) * (h > 28.0 ? 1.35 : 1.0);

                for (double side : {-1.0, 1.0}) {
                    double bx = cx - ss * (side * offset);
                    double by = cy + sc * (side * offset);
                    if (!field.building_clears_roads(bx, by, w, d, hdg, h, 3.2)) continue;
                    if (field.in_junction(bx, by, extent_r + 2.0, 3)) {
                        layout.sanity.pruned_junction++;
                        continue;
                    }
                    if (field.near_parking_cutout(bx, by, 0.0, extent_r + 2.0)) {
                        layout.sanity.pruned_cutout++;
                        continue;
                    }
                    if (field.is_excluded(bx, by, 0.0, WORLD_MASK_BUILDING)) {
                        layout.sanity.pruned_excluded++;
                        continue;
                    }
                    if (!try_place_cell(bx, by, std::hypot(w, d) * 0.5)) continue;
                    layout.buildings.push_back({bx, by, 0.0, w, d, h, hdg, li, seed_u});
                }
            }
        }
    }
}

void pass_custom_props(const std::vector<CompiledEdge>& edges,
                       const SpatialField& field,
                       const WorldGenConfig& cfg,
                       WorldLayout& layout) {
    for (const auto& sp : cfg.custom_props) {
        WorldProp p;
        p.id = sp.id;
        p.kind = sp.kind;
        p.label = sp.label;
        p.length = sp.length;
        p.width = sp.width;
        p.height = sp.height;
        p.has_collider = sp.has_collider;

        if (sp.placement == "road_s" && !sp.road.empty()) {
            const CompiledEdge* found = nullptr;
            for (const auto& ce : edges) {
                if (ce.name == sp.road || ce.id_str == sp.road) {
                    found = &ce;
                    break;
                }
            }
            if (!found) continue;
            double cx, cy, cz, hdg;
            found->sample_at_s(sp.s, cx, cy, cz, hdg);
            double sc = std::cos(hdg), ss = std::sin(hdg);
            p.x = cx - ss * sp.d;
            p.y = cy + sc * sp.d;
            p.z = cz + sp.z_offset;
            p.heading = hdg + sp.heading_offset;
        } else {
            p.x = sp.x;
            p.y = sp.y;
            p.z = sp.auto_z ? (field.resolve_surface_z(sp.x, sp.y, sp.z) + sp.z_offset)
                            : (sp.z + sp.z_offset);
            p.heading = sp.heading + sp.heading_offset;
        }

        if (!is_finite_pos(p.x, p.y, p.z)) continue;
        layout.props.push_back(std::move(p));
    }
}

void prune_and_audit(const SpatialField& field, WorldLayout& layout) {
    auto prune_list = [&](auto& vec, auto get_z, double extra_margin, double junct_margin,
                          bool use_tight = false) {
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const auto& item) {
            if (field.in_junction(item.x, item.y, junct_margin)) {
                layout.sanity.pruned_junction++;
                return true;
            }
            if (field.inside_road_band(item.x, item.y, get_z(item), extra_margin, use_tight)) {
                layout.sanity.pruned_asphalt++;
                return true;
            }
            return false;
        }), vec.end());
    };

    prune_list(layout.poles, [](const WorldPole& p) { return p.z; }, -0.55, 3.0);
    prune_list(layout.trees, [](const WorldTree& t) { return t.z; }, 0.85, 2.0);
    prune_list(layout.signs, [](const WorldSign& s) { return s.z; }, 0.35, 3.0, true);

    int intrusions = 0;
    for (const auto& p : layout.poles) {
        if (field.inside_road_band(p.x, p.y, p.z, -0.05, true)) intrusions++;
    }
    for (const auto& t : layout.trees) {
        if (field.inside_road_band(t.x, t.y, t.z, 0.25, true)) intrusions++;
    }
    for (const auto& s : layout.signs) {
        if (field.inside_road_band(s.x, s.y, s.z, 0.25, true)) intrusions++;
    }
    for (const auto& f : layout.furniture) {
        if (f.kind == "bus_shelter" || f.kind == "hydrant") {
            if (field.inside_road_band(f.x, f.y, f.z, 0.25, true)) intrusions++;
        }
    }

    layout.sanity.poles = static_cast<int>(layout.poles.size());
    layout.sanity.trees = static_cast<int>(layout.trees.size());
    layout.sanity.barrier_posts = static_cast<int>(layout.barrier_posts.size());
    layout.sanity.barrier_spans = static_cast<int>(layout.barrier_spans.size());
    layout.sanity.piers = static_cast<int>(layout.piers.size());
    layout.sanity.signs = static_cast<int>(layout.signs.size());
    layout.sanity.furniture = static_cast<int>(layout.furniture.size());
    layout.sanity.buildings = static_cast<int>(layout.buildings.size());
    layout.sanity.props = static_cast<int>(layout.props.size());
    layout.sanity.remaining_asphalt_intrusions = intrusions;
    layout.sanity.floating_count = 0;
}

void build_colliders_from_layout(WorldLayout& layout) {
    layout.colliders.clear();
    layout.colliders.reserve(
        layout.furniture.size() + layout.poles.size() + layout.trees.size() +
        layout.signs.size() + layout.piers.size() + layout.barrier_spans.size() +
        layout.props.size() + layout.buildings.size());

    auto classify_furniture_role = [](const std::string& kind) -> ColliderRole {
        if (kind == "wall" || kind == "pillar" || kind == "bollard") {
            return ColliderRole::StructuralObstacle;
        }
        if (kind == "barrier") {
            return ColliderRole::RoadsideBarrier;
        }
        return ColliderRole::OffroadProp;
    };
    auto classify_prop_role = [](const std::string& kind) -> ColliderRole {
        if (kind == "bollard" || kind == "wall" || kind == "pillar" ||
            kind == "barrier_block" || kind == "jersey_barrier") {
            return ColliderRole::StructuralObstacle;
        }
        return ColliderRole::OffroadProp;
    };

    for (const auto& f : layout.furniture) {
        if (!f.has_collider) continue;
        bool cyl = (f.kind == "hydrant" || f.kind == "bollard");
        layout.colliders.push_back({
            f.kind, f.x, f.y, f.z, f.length, f.width, f.heading, f.height, cyl, f.sensor_visible,
            {}, classify_furniture_role(f.kind)
        });
    }
    for (const auto& p : layout.poles) {
        if (!p.has_collider) continue;
        layout.colliders.push_back({
            "pole", p.x, p.y, p.z, 0.45, 0.45, p.heading, 6.5, true, p.sensor_visible,
            {}, ColliderRole::OffroadProp
        });
    }
    for (const auto& t : layout.trees) {
        if (!t.has_collider) continue;
        layout.colliders.push_back({
            "tree", t.x, t.y, t.z, 0.90, 0.90, t.heading, 4.6 * t.height_scale, true, t.sensor_visible,
            {}, ColliderRole::OffroadProp
        });
    }
    for (const auto& s : layout.signs) {
        if (!s.has_collider) continue;
        layout.colliders.push_back({
            "sign", s.x, s.y, s.z,
            std::max(0.40, s.length), std::max(0.50, s.width), s.heading,
            std::max(1.80, s.height), false, s.sensor_visible, s.kind, ColliderRole::OffroadProp
        });
    }
    for (const auto& b : layout.barrier_spans) {
        if (!b.has_collider) continue;
        layout.colliders.push_back({
            "barrier", b.mid_x, b.mid_y, b.mid_z, b.len, 0.32, b.heading, 0.85, false, b.sensor_visible,
            {}, ColliderRole::RoadsideBarrier
        });
    }
    for (const auto& pr : layout.piers) {
        if (!pr.has_collider) continue;
        double h = std::max(0.5, pr.z_top - pr.z_ground);
        double dia = pr.radius * 2.0;
        if (pr.columns <= 1) {
            layout.colliders.push_back({
                "pier", pr.x, pr.y, pr.z_ground, dia, dia, pr.heading, h, true, pr.sensor_visible,
                {}, ColliderRole::StructuralObstacle
            });
        } else {
            double nx = -std::sin(pr.heading), ny = std::cos(pr.heading);
            layout.colliders.push_back({
                "pier", pr.x - nx * pr.col_offset, pr.y - ny * pr.col_offset, pr.z_ground,
                dia, dia, pr.heading, h, true, pr.sensor_visible,
                {}, ColliderRole::StructuralObstacle
            });
            layout.colliders.push_back({
                "pier", pr.x + nx * pr.col_offset, pr.y + ny * pr.col_offset, pr.z_ground,
                dia, dia, pr.heading, h, true, pr.sensor_visible,
                {}, ColliderRole::StructuralObstacle
            });
        }
    }
    for (const auto& cp : layout.props) {
        if (!cp.has_collider) continue;
        layout.colliders.push_back({
            cp.kind, cp.x, cp.y, cp.z,
            std::max(0.4, cp.length), std::max(0.4, cp.width),
            cp.heading, std::max(0.5, cp.height), false, cp.sensor_visible, cp.label,
            classify_prop_role(cp.kind)
        });
    }
    /* Procedural skyline buildings carry has_collider/sensor_visible flags
     * just like furniture, but used to be serialized + rendered without ever
     * reaching Jolt/legacy colliders. When buildings_source=="world_engine"
     * and the map has no building list, vehicles could drive through them. */
    for (const auto& b : layout.buildings) {
        if (!b.has_collider) continue;
        layout.colliders.push_back({
            "building", b.x, b.y, b.z,
            std::max(1.0, b.width), std::max(1.0, b.depth),
            b.heading, std::max(1.0, b.height), false, b.sensor_visible,
            {}, ColliderRole::StructuralObstacle
        });
    }

    layout.sanity.colliders = static_cast<int>(layout.colliders.size());
}

/* ── Fast String JSON Builders ───────────────────────────────── */

inline void append_str_escaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 0x20) out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
}

inline void append_f(std::string& out, double v) {
    char buf[48];
    if (!std::isfinite(v)) v = 0.0;
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    /* Trim trailing fractional zeros for compact payload */
    char* dot = std::strchr(buf, '.');
    if (dot) {
        char* end = buf + std::strlen(buf) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (end == dot) *end = '\0';
        if (std::strcmp(buf, "-0") == 0) std::strcpy(buf, "0");
    }
    out += buf;
}

inline void append_i(std::string& out, int v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", v);
    out += buf;
}

uint64_t compute_layout_fnv(const WorldLayout& layout) {
    uint64_t h = 1469598103934665603ULL;
    auto q = [](double v) { return static_cast<uint64_t>(std::llround(v * 100.0)); };
    h = fnv1a_bytes(h, layout.preset.data(), layout.preset.size());
    h = fnv1a_step(h, layout.config.seed);
    h = fnv1a_step(h, layout.poles.size());
    h = fnv1a_step(h, layout.trees.size());
    h = fnv1a_step(h, layout.barrier_posts.size());
    h = fnv1a_step(h, layout.piers.size());
    h = fnv1a_step(h, layout.signs.size());
    h = fnv1a_step(h, layout.furniture.size());
    h = fnv1a_step(h, layout.buildings.size());
    h = fnv1a_step(h, layout.props.size());
    for (const auto& p : layout.poles) { h = fnv1a_step(h, q(p.x)); h = fnv1a_step(h, q(p.y)); }
    for (const auto& t : layout.trees) { h = fnv1a_step(h, q(t.x)); h = fnv1a_step(h, q(t.y)); }
    for (const auto& cp : layout.props) {
        h = fnv1a_bytes(h, cp.id.data(), cp.id.size());
        h = fnv1a_step(h, q(cp.x));
        h = fnv1a_step(h, q(cp.y));
        h = fnv1a_step(h, q(cp.z));
    }
    return h;
}

}  // namespace

WorldGenConfig parse_world_gen_config(const char* scenario_or_wg_json, uint32_t default_seed) {
    WorldGenConfig cfg;
    cfg.seed = default_seed;
    apply_preset_defaults(cfg);
    if (!scenario_or_wg_json || !scenario_or_wg_json[0]) return cfg;
    cJSON* root = cJSON_Parse(scenario_or_wg_json);
    if (!root) return cfg;
    cJSON* jwg = cJSON_GetObjectItemCaseSensitive(root, "world_gen");
    if (cJSON_IsObject(jwg)) {
        uint32_t sc_seed = default_seed;
        if (cJSON* js = cJSON_GetObjectItemCaseSensitive(root, "random_seed"); cJSON_IsNumber(js)) {
            sc_seed = static_cast<uint32_t>(js->valuedouble);
        }
        cfg = parse_world_gen_obj(jwg, sc_seed);
    } else if (cJSON_GetObjectItemCaseSensitive(root, "preset") ||
               cJSON_GetObjectItemCaseSensitive(root, "custom_props") ||
               cJSON_GetObjectItemCaseSensitive(root, "exclude_zones") ||
               cJSON_GetObjectItemCaseSensitive(root, "edge_overrides")) {
        cfg = parse_world_gen_obj(root, default_seed);
    } else if (cJSON* js = cJSON_GetObjectItemCaseSensitive(root, "random_seed"); cJSON_IsNumber(js)) {
        cfg.seed = static_cast<uint32_t>(js->valuedouble);
    }
    cJSON_Delete(root);
    return cfg;
}

WorldLayout compile_world(const WorldEngineInput& input) {
    const auto t0 = std::chrono::steady_clock::now(); /* wall-clock-prof */

    std::string raw_wg;
    if (input.world_gen_json && input.world_gen_json[0]) {
        raw_wg = input.world_gen_json;
    } else if (input.scenario_file_path && input.scenario_file_path[0]) {
        raw_wg = read_text_file(input.scenario_file_path);
    }

    WorldLayout layout;
    layout.config = parse_world_gen_config(raw_wg.empty() ? nullptr : raw_wg.c_str(), input.random_seed);
    layout.preset = layout.config.preset;

    cJSON* rn_root = (input.road_network_json && input.road_network_json[0])
        ? cJSON_Parse(input.road_network_json)
        : nullptr;
    std::vector<CompiledEdge> edges = build_compiled_edges(rn_root, layout.config);
    if (rn_root) cJSON_Delete(rn_root);

    SpatialField field;
    field.build(edges, input, layout.config);

    pass_subsurface_and_parking(edges, input, layout);
    pass_poles(edges, field, layout);
    pass_trees(edges, field, layout.config, layout);
    pass_barriers(edges, field, layout.config, layout);
    pass_piers(edges, field, layout.config, layout);
    pass_signs(edges, field, layout);
    pass_furniture(edges, field, layout.config, layout);
    pass_buildings(edges, field, input, layout.config, layout);
    pass_custom_props(edges, field, layout.config, layout);

    prune_and_audit(field, layout);
    build_colliders_from_layout(layout);

    const auto t1 = std::chrono::steady_clock::now(); /* wall-clock-prof */
    layout.sanity.compile_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    char hash_buf[32];
    std::snprintf(hash_buf, sizeof(hash_buf), "%016llx",
                  static_cast<unsigned long long>(compute_layout_fnv(layout)));
    layout.world_hash = hash_buf;
    return layout;
}

std::string serialize_world_layout_json(const WorldLayout& layout) {
    std::string out;
    out.reserve(32768);
    out += "{\"schema\":1,\"authoritative\":\"world_engine\",";
    out += "\"preset\":";
    append_str_escaped(out, layout.preset);
    out += ",\"world_hash\":";
    append_str_escaped(out, layout.world_hash);
    out += ",\"buildings_source\":";
    append_str_escaped(out, layout.buildings_source);

    out += ",\"sanity\":{\"poles\":";
    append_i(out, layout.sanity.poles);
    out += ",\"trees\":";
    append_i(out, layout.sanity.trees);
    out += ",\"barrier_posts\":";
    append_i(out, layout.sanity.barrier_posts);
    out += ",\"barrier_spans\":";
    append_i(out, layout.sanity.barrier_spans);
    out += ",\"piers\":";
    append_i(out, layout.sanity.piers);
    out += ",\"signs\":";
    append_i(out, layout.sanity.signs);
    out += ",\"furniture\":";
    append_i(out, layout.sanity.furniture);
    out += ",\"buildings\":";
    append_i(out, layout.sanity.buildings);
    out += ",\"props\":";
    append_i(out, layout.sanity.props);
    out += ",\"colliders\":";
    append_i(out, layout.sanity.colliders);
    out += ",\"pruned_asphalt\":";
    append_i(out, layout.sanity.pruned_asphalt);
    out += ",\"pruned_junction\":";
    append_i(out, layout.sanity.pruned_junction);
    out += ",\"pruned_cutout\":";
    append_i(out, layout.sanity.pruned_cutout);
    out += ",\"pruned_excluded\":";
    append_i(out, layout.sanity.pruned_excluded);
    out += ",\"remaining_asphalt_intrusions\":";
    append_i(out, layout.sanity.remaining_asphalt_intrusions);
    out += ",\"floating_count\":";
    append_i(out, layout.sanity.floating_count);
    out += "}";

    out += ",\"poles\":[";
    for (size_t i = 0; i < layout.poles.size(); ++i) {
        const auto& p = layout.poles[i];
        if (i) out.push_back(',');
        out += "{\"x\":"; append_f(out, p.x);
        out += ",\"y\":"; append_f(out, p.y);
        out += ",\"z\":"; append_f(out, p.z);
        out += ",\"nx\":"; append_f(out, p.nx);
        out += ",\"ny\":"; append_f(out, p.ny);
        out += ",\"heading\":"; append_f(out, p.heading);
        out += ",\"side\":"; append_i(out, p.side);
        out += "}";
    }
    out += "]";

    out += ",\"trees\":[";
    for (size_t i = 0; i < layout.trees.size(); ++i) {
        const auto& t = layout.trees[i];
        if (i) out.push_back(',');
        out += "{\"x\":"; append_f(out, t.x);
        out += ",\"y\":"; append_f(out, t.y);
        out += ",\"z\":"; append_f(out, t.z);
        out += ",\"variant\":"; append_f(out, t.variant);
        out += ",\"height_scale\":"; append_f(out, t.height_scale);
        out += ",\"crown_scale\":"; append_f(out, t.crown_scale);
        out += ",\"species\":"; append_str_escaped(out, t.species);
        out += "}";
    }
    out += "]";

    out += ",\"barriers\":{\"posts\":[";
    for (size_t i = 0; i < layout.barrier_posts.size(); ++i) {
        const auto& p = layout.barrier_posts[i];
        if (i) out.push_back(',');
        out += "{\"x\":"; append_f(out, p.x);
        out += ",\"y\":"; append_f(out, p.y);
        out += ",\"z\":"; append_f(out, p.z);
        out += ",\"heading\":"; append_f(out, p.heading);
        out += "}";
    }
    out += "],\"spans\":[";
    for (size_t i = 0; i < layout.barrier_spans.size(); ++i) {
        const auto& b = layout.barrier_spans[i];
        if (i) out.push_back(',');
        out += "{\"x\":"; append_f(out, b.mid_x);
        out += ",\"y\":"; append_f(out, b.mid_y);
        out += ",\"z\":"; append_f(out, b.mid_z);
        out += ",\"len\":"; append_f(out, b.len);
        out += ",\"heading\":"; append_f(out, b.heading);
        out += "}";
    }
    out += "]}";

    out += ",\"piers\":[";
    for (size_t i = 0; i < layout.piers.size(); ++i) {
        const auto& pr = layout.piers[i];
        if (i) out.push_back(',');
        out += "{\"x\":"; append_f(out, pr.x);
        out += ",\"y\":"; append_f(out, pr.y);
        out += ",\"z_ground\":"; append_f(out, pr.z_ground);
        out += ",\"z_top\":"; append_f(out, pr.z_top);
        out += ",\"heading\":"; append_f(out, pr.heading);
        out += ",\"columns\":"; append_i(out, pr.columns);
        out += ",\"col_offset\":"; append_f(out, pr.col_offset);
        out += ",\"radius\":"; append_f(out, pr.radius);
        out += ",\"cap_len\":"; append_f(out, pr.cap_len);
        out += ",\"cap_w\":"; append_f(out, pr.cap_w);
        out += ",\"cap_h\":"; append_f(out, pr.cap_h);
        out += "}";
    }
    out += "]";

    out += ",\"signs\":[";
    for (size_t i = 0; i < layout.signs.size(); ++i) {
        const auto& s = layout.signs[i];
        if (i) out.push_back(',');
        out += "{\"x\":"; append_f(out, s.x);
        out += ",\"y\":"; append_f(out, s.y);
        out += ",\"z\":"; append_f(out, s.z);
        out += ",\"heading\":"; append_f(out, s.heading);
        out += ",\"length\":"; append_f(out, s.length);
        out += ",\"width\":"; append_f(out, s.width);
        out += ",\"height\":"; append_f(out, s.height);
        out += ",\"kind\":"; append_str_escaped(out, s.kind);
        out += ",\"label\":"; append_str_escaped(out, s.label);
        out += ",\"speed_kph\":"; append_i(out, s.speed_kph);
        out += ",\"collider\":"; out += (s.has_collider ? "true" : "false");
        out += ",\"sensor_visible\":"; out += (s.sensor_visible ? "true" : "false");
        out += "}";
    }
    out += "]";

    out += ",\"furniture\":[";
    for (size_t i = 0; i < layout.furniture.size(); ++i) {
        const auto& f = layout.furniture[i];
        if (i) out.push_back(',');
        out += "{\"x\":"; append_f(out, f.x);
        out += ",\"y\":"; append_f(out, f.y);
        out += ",\"z\":"; append_f(out, f.z);
        out += ",\"heading\":"; append_f(out, f.heading);
        out += ",\"length\":"; append_f(out, f.length);
        out += ",\"width\":"; append_f(out, f.width);
        out += ",\"height\":"; append_f(out, f.height);
        out += ",\"kind\":"; append_str_escaped(out, f.kind);
        out += "}";
    }
    out += "]";

    out += ",\"buildings\":[";
    for (size_t i = 0; i < layout.buildings.size(); ++i) {
        const auto& b = layout.buildings[i];
        if (i) out.push_back(',');
        out += "{\"x\":"; append_f(out, b.x);
        out += ",\"y\":"; append_f(out, b.y);
        out += ",\"z\":"; append_f(out, b.z);
        out += ",\"width\":"; append_f(out, b.width);
        out += ",\"depth\":"; append_f(out, b.depth);
        out += ",\"height\":"; append_f(out, b.height);
        out += ",\"heading\":"; append_f(out, b.heading);
        out += ",\"layer\":"; append_i(out, b.layer);
        out += ",\"seed\":"; append_f(out, b.seed);
        out += "}";
    }
    out += "]";

    out += ",\"props\":[";
    for (size_t i = 0; i < layout.props.size(); ++i) {
        const auto& cp = layout.props[i];
        if (i) out.push_back(',');
        out += "{\"id\":"; append_str_escaped(out, cp.id);
        out += ",\"kind\":"; append_str_escaped(out, cp.kind);
        out += ",\"label\":"; append_str_escaped(out, cp.label);
        out += ",\"x\":"; append_f(out, cp.x);
        out += ",\"y\":"; append_f(out, cp.y);
        out += ",\"z\":"; append_f(out, cp.z);
        out += ",\"heading\":"; append_f(out, cp.heading);
        out += ",\"length\":"; append_f(out, cp.length);
        out += ",\"width\":"; append_f(out, cp.width);
        out += ",\"height\":"; append_f(out, cp.height);
        out += ",\"collider\":"; out += (cp.has_collider ? "true" : "false");
        out += "}";
    }
    out += "]}";
    return out;
}

bool write_world_layout_json(const WorldLayout& layout, const char* path) {
    if (!path || !path[0]) return false;
    std::string json = serialize_world_layout_json(layout);
    std::string tmp = std::string(path) + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs.write(json.data(), static_cast<std::streamsize>(json.size()));
        ofs.flush();
        if (!ofs.good()) return false;
    }
    if (std::rename(tmp.c_str(), path) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace flowsim
