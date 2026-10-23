#include "hybrid_a_star.h"
#include <cmath>
#include <algorithm>

namespace flowengine {
namespace planning {

HybridAStar::HybridAStar(const VehicleParam& veh_param, const HybridAStarConfig& config)
    : veh_param_(veh_param), config_(config) {
    reeds_shepp_ = std::make_unique<ReedsShepp>(veh_param_.min_turn_radius(), veh_param_.max_steer);
    grid_search_ = std::make_unique<GridSearch>();
}

void HybridAStar::SetVehicleParam(const VehicleParam& veh_param) {
    veh_param_ = veh_param;
    reeds_shepp_ = std::make_unique<ReedsShepp>(veh_param_.min_turn_radius(), veh_param_.max_steer);
}

void HybridAStar::SetConfig(const HybridAStarConfig& config) {
    config_ = config;
}

double HybridAStar::Mod2Pi(double angle) {
    double v = std::fmod(angle, 2.0 * M_PI);
    if (v < -M_PI) v += 2.0 * M_PI;
    else if (v >= M_PI) v -= 2.0 * M_PI;
    return v;
}

double HybridAStar::NormalizeAngle2Pi(double angle) {
    double v = std::fmod(angle, 2.0 * M_PI);
    if (v < 0.0) v += 2.0 * M_PI;
    return v;
}

uint64_t HybridAStar::CalcIndexKey(int gx, int gy, int gphi) {
    uint64_t ux = static_cast<uint64_t>(gx) & 0x1FFFFFULL;
    uint64_t uy = static_cast<uint64_t>(gy) & 0x1FFFFFULL;
    uint64_t uphi = static_cast<uint64_t>(gphi) & 0x3FFULL;
    return ux | (uy << 21) | (uphi << 42);
}

Box2d HybridAStar::GetEgoVehicleBox(double x, double y, double phi, double safety_margin) const {
    double rear_to_center = veh_param_.rear_axle_to_center();
    double cx = x + rear_to_center * std::cos(phi);
    double cy = y + rear_to_center * std::sin(phi);
    double l = veh_param_.length + 2.0 * safety_margin;
    double w = veh_param_.width + 2.0 * safety_margin;
    return Box2d(Vec2d(cx, cy), phi, l, w);
}

bool HybridAStar::CheckVehicleCollision(double x, double y, double phi, const Environment& env) const {
    Box2d ego_box = GetEgoVehicleBox(x, y, phi, config_.vehicle_safety_margin);

    // 1. Boundary check: ensure vehicle box stays strictly inside environment boundaries
    for (const auto& corner : ego_box.GetAllCorners()) {
        if (corner.x < env.min_x || corner.x > env.max_x ||
            corner.y < env.min_y || corner.y > env.max_y) {
            return true;
        }
    }

    // 2. Obstacle boxes
    for (const auto& box : env.obstacle_boxes) {
        if (ego_box.HasOverlap(box)) {
            return true;
        }
    }

    // 3. Obstacle lines
    for (const auto& line : env.obstacle_lines) {
        if (ego_box.HasOverlap(line)) {
            return true;
        }
    }

    // 4. Obstacle points
    for (const auto& pt : env.obstacle_points) {
        if (ego_box.IsPointIn(pt)) {
            return true;
        }
    }

    return false;
}

double HybridAStar::CalculateEdgeCost(const SearchNode& parent, double step_len, int dir, double steer) const {
    double dist_cost = (dir > 0) ? (step_len * config_.weight_forward) : (step_len * config_.weight_reverse);
    double gear_cost = (dir != parent.direction) ? config_.weight_gear_switch : 0.0;
    double steer_cost = std::abs(steer) * config_.weight_steer;
    double steer_change_cost = std::abs(steer - parent.steer) * config_.weight_steer_change;
    return dist_cost + gear_cost + steer_cost + steer_change_cost;
}

double HybridAStar::CalculateHeuristic(double x, double y, double phi,
                                      double goal_x, double goal_y, double goal_phi) const {
    double rs_dist = reeds_shepp_->Distance(x, y, phi, goal_x, goal_y, goal_phi);
    double rs_cost = rs_dist * config_.weight_forward;
    double dp_cost = grid_search_->CheckDpMap(x, y);
    return std::max(rs_cost, dp_cost) * config_.weight_heuristic;
}

bool HybridAStar::TryAnalyticExpansion(const SearchNode& current,
                                      double goal_x, double goal_y, double goal_phi,
                                      const Environment& env,
                                      std::vector<PathPoint>* analytic_path) const {
    RSPath rs_path;
    if (!reeds_shepp_->ShortestPath(current.x, current.y, current.phi, goal_x, goal_y, goal_phi, &rs_path)) {
        return false;
    }

    std::vector<PathPoint> sampled_points;
    if (!reeds_shepp_->Interpolate(rs_path, current.x, current.y, current.phi, config_.check_step_size, &sampled_points)) {
        return false;
    }

    for (const auto& pt : sampled_points) {
        if (CheckVehicleCollision(pt.x, pt.y, pt.phi, env)) {
            return false;
        }
    }

    *analytic_path = std::move(sampled_points);
    return true;
}

bool HybridAStar::Plan(double start_x, double start_y, double start_phi,
                       double goal_x, double goal_y, double goal_phi,
                       const Environment& env,
                       HybridAStarResult* result) {
    if (!result) return false;
    result->success = false;
    result->path.clear();
    result->total_length = 0.0;
    result->gear_switches = 0;
    result->expanded_nodes = 0;
    result->search_time_ms = 0.0;

    auto start_time = std::chrono::high_resolution_clock::now();

    start_phi = Mod2Pi(start_phi);
    goal_phi = Mod2Pi(goal_phi);

    // Initial boundary & collision validity check
    if (CheckVehicleCollision(start_x, start_y, start_phi, env)) {
        return false;
    }
    if (CheckVehicleCollision(goal_x, goal_y, goal_phi, env)) {
        return false;
    }

    // Precompute 2D obstacle Dijkstra heuristic field from goal
    grid_search_->GenerateDpMap(goal_x, goal_y,
                                env.min_x, env.max_x,
                                env.min_y, env.max_y,
                                config_.xy_grid_resolution,
                                veh_param_.width * 0.5,
                                env.obstacle_boxes,
                                env.obstacle_lines,
                                env.obstacle_points);

    // Try direct analytic expansion from start immediately
    SearchNode root_node;
    root_node.x = start_x;
    root_node.y = start_y;
    root_node.phi = start_phi;
    root_node.direction = 1;
    root_node.steer = 0.0;
    root_node.g_cost = 0.0;

    std::vector<PathPoint> direct_analytic_path;
    if (TryAnalyticExpansion(root_node, goal_x, goal_y, goal_phi, env, &direct_analytic_path)) {
        result->success = true;
        result->path = std::move(direct_analytic_path);
        // Compute statistics
        double len = 0.0;
        int gears = 0;
        for (size_t i = 1; i < result->path.size(); ++i) {
            len += std::hypot(result->path[i].x - result->path[i-1].x, result->path[i].y - result->path[i-1].y);
            if (result->path[i].direction != result->path[i-1].direction) {
                gears++;
            }
        }
        result->total_length = len;
        result->gear_switches = gears;
        result->expanded_nodes = 1;
        auto end_time = std::chrono::high_resolution_clock::now();
        result->search_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        return true;
    }

    // Node memory pool and indexing
    std::vector<SearchNode> node_pool;
    node_pool.reserve(config_.max_iterations);

    std::unordered_map<uint64_t, int> grid_index_map;
    grid_index_map.reserve(config_.max_iterations);

    using NodePair = std::pair<double, int>; // (f_cost, node_index)
    std::priority_queue<NodePair, std::vector<NodePair>, std::greater<NodePair>> open_pq;

    root_node.h_cost = CalculateHeuristic(start_x, start_y, start_phi, goal_x, goal_y, goal_phi);
    root_node.f_cost = root_node.g_cost + root_node.h_cost;
    root_node.grid_x = static_cast<int>(std::floor((start_x - env.min_x) / config_.xy_grid_resolution));
    root_node.grid_y = static_cast<int>(std::floor((start_y - env.min_y) / config_.xy_grid_resolution));
    root_node.grid_phi = static_cast<int>(std::floor(NormalizeAngle2Pi(start_phi) / config_.phi_grid_resolution));
    root_node.index = 0;
    root_node.parent_index = -1;

    node_pool.push_back(root_node);
    open_pq.push({root_node.f_cost, 0});
    grid_index_map[CalcIndexKey(root_node.grid_x, root_node.grid_y, root_node.grid_phi)] = 0;

    // Discrete action space
    std::vector<double> candidate_steers;
    int slices = veh_param_.steer_slices;
    for (int s = -slices; s <= slices; ++s) {
        candidate_steers.push_back(veh_param_.max_steer * s / slices);
    }
    const int candidate_dirs[2] = {1, -1};

    int best_node_index = -1;
    std::vector<PathPoint> best_analytic_tail;

    while (!open_pq.empty() && node_pool.size() < config_.max_iterations) {
        auto [curr_f, curr_idx] = open_pq.top();
        open_pq.pop();

        SearchNode& curr = node_pool[curr_idx];
        if (curr.in_closed) continue;
        curr.in_closed = true;
        result->expanded_nodes++;

        // Periodic timeout check
        if ((result->expanded_nodes & 0x7F) == 0) {
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double, std::milli>(now - start_time).count();
            if (elapsed > config_.time_limit_ms) {
                break;
            }
        }

        // Analytic expansion towards goal
        double dist_to_goal = std::hypot(curr.x - goal_x, curr.y - goal_y);
        if (dist_to_goal <= config_.analytic_expansion_max_dist &&
            (result->expanded_nodes % config_.analytic_expansion_interval == 0 || dist_to_goal < 6.0)) {
            std::vector<PathPoint> analytic_tail;
            if (TryAnalyticExpansion(curr, goal_x, goal_y, goal_phi, env, &analytic_tail)) {
                best_node_index = curr_idx;
                best_analytic_tail = std::move(analytic_tail);
                break;
            }
        }

        // Direct proximity termination condition
        if (dist_to_goal < 0.25 && std::abs(Mod2Pi(curr.phi - goal_phi)) < 0.15) {
            best_node_index = curr_idx;
            break;
        }

        // Neighbor expansion
        for (int dir : candidate_dirs) {
            for (double steer : candidate_steers) {
                // Kinematic integration
                double step_len = veh_param_.step_size;
                int sub_steps = std::max(1, static_cast<int>(std::ceil(step_len / config_.check_step_size)));
                double ds = step_len / sub_steps;

                double nx = curr.x;
                double ny = curr.y;
                double nphi = curr.phi;
                bool collision = false;
                std::vector<PathPoint> inter_pts;
                inter_pts.reserve(sub_steps);

                double kappa = std::tan(steer) / veh_param_.wheelbase;

                for (int s = 0; s < sub_steps; ++s) {
                    nx += dir * ds * std::cos(nphi);
                    ny += dir * ds * std::sin(nphi);
                    nphi = Mod2Pi(nphi + dir * ds * kappa);

                    if (CheckVehicleCollision(nx, ny, nphi, env)) {
                        collision = true;
                        break;
                    }
                    inter_pts.push_back(PathPoint{nx, ny, nphi, steer, dir, 0.0, kappa});
                }

                if (collision) continue;

                int ngx = static_cast<int>(std::floor((nx - env.min_x) / config_.xy_grid_resolution));
                int ngy = static_cast<int>(std::floor((ny - env.min_y) / config_.xy_grid_resolution));
                int ngphi = static_cast<int>(std::floor(NormalizeAngle2Pi(nphi) / config_.phi_grid_resolution));
                uint64_t key = CalcIndexKey(ngx, ngy, ngphi);

                double edge_cost = CalculateEdgeCost(curr, step_len, dir, steer);
                double next_g = curr.g_cost + edge_cost;

                auto it = grid_index_map.find(key);
                if (it != grid_index_map.end()) {
                    int existing_idx = it->second;
                    if (node_pool[existing_idx].in_closed) continue;
                    if (node_pool[existing_idx].g_cost <= next_g) continue;
                }

                SearchNode next_node;
                next_node.x = nx;
                next_node.y = ny;
                next_node.phi = nphi;
                next_node.steer = steer;
                next_node.direction = dir;
                next_node.g_cost = next_g;
                next_node.h_cost = CalculateHeuristic(nx, ny, nphi, goal_x, goal_y, goal_phi);
                next_node.f_cost = next_node.g_cost + next_node.h_cost;
                next_node.grid_x = ngx;
                next_node.grid_y = ngy;
                next_node.grid_phi = ngphi;
                next_node.parent_index = curr_idx;
                next_node.index = static_cast<int>(node_pool.size());
                next_node.intermediate_points = std::move(inter_pts);

                node_pool.push_back(next_node);
                grid_index_map[key] = next_node.index;
                open_pq.push({next_node.f_cost, next_node.index});
            }
        }
    }

    if (best_node_index < 0) {
        auto end_time = std::chrono::high_resolution_clock::now();
        result->search_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        return false;
    }

    // Reconstruct trajectory path from goal to root
    std::vector<PathPoint> raw_path;
    int trace_idx = best_node_index;
    while (trace_idx >= 0) {
        const auto& node = node_pool[trace_idx];
        if (node.parent_index >= 0) {
            for (auto it = node.intermediate_points.rbegin(); it != node.intermediate_points.rend(); ++it) {
                raw_path.push_back(*it);
            }
        } else {
            raw_path.push_back(PathPoint{node.x, node.y, node.phi, node.steer, node.direction, 0.0, 0.0});
        }
        trace_idx = node.parent_index;
    }
    std::reverse(raw_path.begin(), raw_path.end());

    // Append analytic expansion tail if available
    if (!best_analytic_tail.empty()) {
        for (size_t i = 1; i < best_analytic_tail.size(); ++i) {
            raw_path.push_back(best_analytic_tail[i]);
        }
    }

    // Compute accumulated s and gear switches
    double total_s = 0.0;
    int gears = 0;
    if (!raw_path.empty()) {
        raw_path[0].s = 0.0;
        for (size_t i = 1; i < raw_path.size(); ++i) {
            double seg_len = std::hypot(raw_path[i].x - raw_path[i-1].x, raw_path[i].y - raw_path[i-1].y);
            total_s += seg_len;
            raw_path[i].s = total_s;
            if (raw_path[i].direction != raw_path[i-1].direction) {
                gears++;
            }
        }
    }

    result->success = true;
    result->path = std::move(raw_path);
    result->total_length = total_s;
    result->gear_switches = gears;

    auto end_time = std::chrono::high_resolution_clock::now();
    result->search_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    return true;
}

}  // namespace planning
}  // namespace flowengine
