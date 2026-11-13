#ifndef FLOWSIM_BUILDING_FILTER_H
#define FLOWSIM_BUILDING_FILTER_H

#include "building.h"
#include "road_network.h"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace flowsim {

inline bool point_in_poly_2d(double px, double py,
                             const std::vector<std::pair<double, double>>& poly) {
    bool inside = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        double xi = poly[i].first,  yi = poly[i].second;
        double xj = poly[j].first,  yj = poly[j].second;
        if ((yi > py) != (yj > py)) {
            double xint = (xj - xi) * (py - yi) / (yj - yi) + xi;
            if (px < xint) inside = !inside;
        }
    }
    return inside;
}

inline double point_seg_dist_2d(double px, double py,
                                double ax, double ay, double bx, double by) {
    double abx = bx - ax, aby = by - ay;
    double L2 = abx * abx + aby * aby;
    double t = (L2 > 1e-12) ? ((px - ax) * abx + (py - ay) * aby) / L2 : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    double cx = ax + abx * t, cy = ay + aby * t;
    return std::hypot(px - cx, py - cy);
}

/**
 * Filter buildings whose 2D footprint overlaps any drivable road surface
 * (e.g., tunnel-overpass roofs or OSM artifacts) out of the collision list.
 * Returns number of dropped buildings.
 */
inline int filter_buildings_overlapping_roads(FlowRoadNetwork& roads,
                                            std::vector<BuildingOBB>& buildings) {
    if (!roads.loaded() || buildings.empty()) return 0;
    const size_t nb = buildings.size();
    std::vector<double> bx0(nb), bx1(nb), by0(nb), by1(nb);
    for (size_t bi = 0; bi < nb; ++bi) {
        const auto& poly = buildings[bi].poly;
        if (poly.empty()) {
            bx0[bi] = 1e18; bx1[bi] = -1e18;
            by0[bi] = 1e18; by1[bi] = -1e18;
            continue;
        }
        double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
        for (const auto& p : poly) {
            if (p.first  < x0) x0 = p.first;
            if (p.first  > x1) x1 = p.first;
            if (p.second < y0) y0 = p.second;
            if (p.second > y1) y1 = p.second;
        }
        bx0[bi] = x0; bx1[bi] = x1; by0[bi] = y0; by1[bi] = y1;
    }

    std::vector<char> drop(nb, 0);
    const int nroad = roads.road_count();
    for (int ri = 0; ri < nroad; ++ri) {
        RoadInfo info;
        if (!roads.road_info(ri, info) || info.length <= 0.0) continue;
        for (double s = 0.0; s <= info.length + 1e-6; s += 5.0) {
            WorldPos wp;
            if (!roads.frenet_to_world((int)info.id, 0, s, 0.0, wp)) continue;
            double lw = roads.lane_width((int)info.id, 0, s);
            if (lw < 1.0 || lw > 10.0) lw = 3.5;
            int lanes = roads.drivable_lane_count((int)info.id, s);
            if (lanes < 1) lanes = 1;
            double half_w = lanes * 0.5 * lw + 1.0;
            for (size_t bi = 0; bi < nb; ++bi) {
                if (drop[bi]) continue;
                const auto& poly = buildings[bi].poly;
                if (poly.size() < 3) continue;
                if (wp.x < bx0[bi] - half_w || wp.x > bx1[bi] + half_w ||
                    wp.y < by0[bi] - half_w || wp.y > by1[bi] + half_w) continue;
                bool hit = point_in_poly_2d(wp.x, wp.y, poly);
                if (!hit) {
                    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                        if (point_seg_dist_2d(wp.x, wp.y,
                                              poly[j].first, poly[j].second,
                                              poly[i].first, poly[i].second) < half_w) {
                            hit = true;
                            break;
                        }
                    }
                }
                if (hit) drop[bi] = 1;
            }
        }
    }

    size_t out = 0;
    int ndrop = 0;
    for (size_t bi = 0; bi < nb; ++bi) {
        if (drop[bi]) { ++ndrop; continue; }
        buildings[out++] = buildings[bi];
    }
    buildings.resize(out);
    return ndrop;
}

}  // namespace flowsim

#endif  // FLOWSIM_BUILDING_FILTER_H
