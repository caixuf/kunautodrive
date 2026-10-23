#include "grid_search.h"
#include <algorithm>

namespace flowengine {
namespace planning {

bool GridSearch::GenerateDpMap(double goal_x, double goal_y,
                               double min_x, double max_x,
                               double min_y, double max_y,
                               double xy_resolution,
                               double inflation_radius,
                               const std::vector<Box2d>& obstacles,
                               const std::vector<LineSegment2d>& lines,
                               const std::vector<Vec2d>& points) {
    goal_x_ = goal_x;
    goal_y_ = goal_y;
    min_x_ = min_x;
    max_x_ = max_x;
    min_y_ = min_y;
    max_y_ = max_y;
    resolution_ = xy_resolution > 0.05 ? xy_resolution : 0.3;

    max_grid_x_ = static_cast<int>(std::ceil((max_x_ - min_x_) / resolution_));
    max_grid_y_ = static_cast<int>(std::ceil((max_y_ - min_y_) / resolution_));

    if (max_grid_x_ <= 0 || max_grid_y_ <= 0) return false;

    size_t total_cells = static_cast<size_t>(max_grid_x_ * max_grid_y_);
    dp_cost_.assign(total_cells, std::numeric_limits<double>::infinity());
    is_obstacle_.assign(total_cells, false);

    // 1. Mark obstacles on grid
    auto mark_obstacle_cell = [&](int gx, int gy) {
        if (gx >= 0 && gx < max_grid_x_ && gy >= 0 && gy < max_grid_y_) {
            is_obstacle_[static_cast<size_t>(gx + gy * max_grid_x_)] = true;
        }
    };

    // Obstacle boxes
    for (const auto& box : obstacles) {
        int gx_min = std::max(0, static_cast<int>(std::floor((box.min_x() - inflation_radius - min_x_) / resolution_)));
        int gx_max = std::min(max_grid_x_ - 1, static_cast<int>(std::ceil((box.max_x() + inflation_radius - min_x_) / resolution_)));
        int gy_min = std::max(0, static_cast<int>(std::floor((box.min_y() - inflation_radius - min_y_) / resolution_)));
        int gy_max = std::min(max_grid_y_ - 1, static_cast<int>(std::ceil((box.max_y() + inflation_radius - min_y_) / resolution_)));

        for (int gx = gx_min; gx <= gx_max; ++gx) {
            double cx = min_x_ + (gx + 0.5) * resolution_;
            for (int gy = gy_min; gy <= gy_max; ++gy) {
                double cy = min_y_ + (gy + 0.5) * resolution_;
                if (box.DistanceTo(Vec2d(cx, cy)) <= inflation_radius) {
                    mark_obstacle_cell(gx, gy);
                }
            }
        }
    }

    // Line segments
    for (const auto& line : lines) {
        double seg_min_x = std::min(line.start_.x, line.end_.x);
        double seg_max_x = std::max(line.start_.x, line.end_.x);
        double seg_min_y = std::min(line.start_.y, line.end_.y);
        double seg_max_y = std::max(line.start_.y, line.end_.y);

        int gx_min = std::max(0, static_cast<int>(std::floor((seg_min_x - inflation_radius - min_x_) / resolution_)));
        int gx_max = std::min(max_grid_x_ - 1, static_cast<int>(std::ceil((seg_max_x + inflation_radius - min_x_) / resolution_)));
        int gy_min = std::max(0, static_cast<int>(std::floor((seg_min_y - inflation_radius - min_y_) / resolution_)));
        int gy_max = std::min(max_grid_y_ - 1, static_cast<int>(std::ceil((seg_max_y + inflation_radius - min_y_) / resolution_)));

        for (int gx = gx_min; gx <= gx_max; ++gx) {
            double cx = min_x_ + (gx + 0.5) * resolution_;
            for (int gy = gy_min; gy <= gy_max; ++gy) {
                double cy = min_y_ + (gy + 0.5) * resolution_;
                if (line.DistanceTo(Vec2d(cx, cy)) <= inflation_radius) {
                    mark_obstacle_cell(gx, gy);
                }
            }
        }
    }

    // Points
    for (const auto& pt : points) {
        int gx_min = std::max(0, static_cast<int>(std::floor((pt.x - inflation_radius - min_x_) / resolution_)));
        int gx_max = std::min(max_grid_x_ - 1, static_cast<int>(std::ceil((pt.x + inflation_radius - min_x_) / resolution_)));
        int gy_min = std::max(0, static_cast<int>(std::floor((pt.y - inflation_radius - min_y_) / resolution_)));
        int gy_max = std::min(max_grid_y_ - 1, static_cast<int>(std::ceil((pt.y + inflation_radius - min_y_) / resolution_)));

        for (int gx = gx_min; gx <= gx_max; ++gx) {
            double cx = min_x_ + (gx + 0.5) * resolution_;
            for (int gy = gy_min; gy <= gy_max; ++gy) {
                double cy = min_y_ + (gy + 0.5) * resolution_;
                if (pt.DistanceTo(Vec2d(cx, cy)) <= inflation_radius) {
                    mark_obstacle_cell(gx, gy);
                }
            }
        }
    }

    // 2. Goal cell setup
    int goal_gx = std::clamp(static_cast<int>((goal_x_ - min_x_) / resolution_), 0, max_grid_x_ - 1);
    int goal_gy = std::clamp(static_cast<int>((goal_y_ - min_y_) / resolution_), 0, max_grid_y_ - 1);
    int goal_idx = goal_gx + goal_gy * max_grid_x_;

    // Ensure goal cell itself is free to allow search initialization
    is_obstacle_[static_cast<size_t>(goal_idx)] = false;

    // 3. Dijkstra algorithm from goal
    using NodePair = std::pair<double, int>;
    std::priority_queue<NodePair, std::vector<NodePair>, std::greater<NodePair>> pq;

    dp_cost_[static_cast<size_t>(goal_idx)] = 0.0;
    pq.push({0.0, goal_idx});

    const int dx[8] = { 1, -1,  0,  0,  1,  1, -1, -1 };
    const int dy[8] = { 0,  0,  1, -1,  1, -1,  1, -1 };
    const double step_costs[8] = {
        resolution_, resolution_, resolution_, resolution_,
        resolution_ * 1.41421356, resolution_ * 1.41421356,
        resolution_ * 1.41421356, resolution_ * 1.41421356
    };

    while (!pq.empty()) {
        auto [c, curr_idx] = pq.top();
        pq.pop();

        if (c > dp_cost_[static_cast<size_t>(curr_idx)]) continue;

        int cx = curr_idx % max_grid_x_;
        int cy = curr_idx / max_grid_x_;

        for (int i = 0; i < 8; ++i) {
            int nx = cx + dx[i];
            int ny = cy + dy[i];
            if (nx < 0 || nx >= max_grid_x_ || ny < 0 || ny >= max_grid_y_) continue;

            int next_idx = nx + ny * max_grid_x_;
            if (is_obstacle_[static_cast<size_t>(next_idx)]) continue;

            double next_cost = c + step_costs[i];
            if (next_cost < dp_cost_[static_cast<size_t>(next_idx)]) {
                dp_cost_[static_cast<size_t>(next_idx)] = next_cost;
                pq.push({next_cost, next_idx});
            }
        }
    }

    return true;
}

double GridSearch::CheckDpMap(double x, double y) const {
    if (dp_cost_.empty()) return std::hypot(x - goal_x_, y - goal_y_);

    int gx = static_cast<int>((x - min_x_) / resolution_);
    int gy = static_cast<int>((y - min_y_) / resolution_);

    if (gx >= 0 && gx < max_grid_x_ && gy >= 0 && gy < max_grid_y_) {
        double c = dp_cost_[static_cast<size_t>(gx + gy * max_grid_x_)];
        if (c < std::numeric_limits<double>::infinity()) {
            return c;
        }
    }
    return std::hypot(x - goal_x_, y - goal_y_);
}

}  // namespace planning
}  // namespace flowengine
