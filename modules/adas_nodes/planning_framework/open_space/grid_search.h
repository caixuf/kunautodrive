#pragma once

#include <vector>
#include <cmath>
#include <limits>
#include <queue>
#include <memory>
#include "box2d.h"

namespace flowengine {
namespace planning {

class GridSearch {
public:
    GridSearch() = default;

    /**
     * @brief Generate 2D DP distance field from goal to all free cells.
     */
    bool GenerateDpMap(double goal_x, double goal_y,
                       double min_x, double max_x,
                       double min_y, double max_y,
                       double xy_resolution,
                       double inflation_radius,
                       const std::vector<Box2d>& obstacles,
                       const std::vector<LineSegment2d>& lines,
                       const std::vector<Vec2d>& points);

    /**
     * @brief Query the 2D obstacle heuristic distance from (x, y) to the goal.
     */
    double CheckDpMap(double x, double y) const;

private:
    double goal_x_{0.0};
    double goal_y_{0.0};
    double min_x_{0.0};
    double max_x_{0.0};
    double min_y_{0.0};
    double max_y_{0.0};
    double resolution_{0.3};

    int max_grid_x_{0};
    int max_grid_y_{0};

    std::vector<double> dp_cost_;
    std::vector<bool> is_obstacle_;
};

}  // namespace planning
}  // namespace flowengine
