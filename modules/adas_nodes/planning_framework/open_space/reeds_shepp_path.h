#pragma once

#include <vector>
#include <cmath>
#include <string>
#include <limits>
#include "box2d.h"

namespace flowengine {
namespace planning {

struct PathPoint {
    double x{0.0};
    double y{0.0};
    double phi{0.0};
    double steer{0.0};
    int direction{1};       // 1 = forward, -1 = reverse
    double s{0.0};          // accumulated s along path
    double kappa{0.0};      // curvature (1/R)
};

struct RSPathSegment {
    double length{0.0};     // absolute arc/linear distance (m)
    char type{'S'};         // 'L', 'R', 'S'
    int direction{1};       // +1 = forward, -1 = reverse
    double steer{0.0};      // steering angle (rad)
};

struct RSPath {
    std::vector<RSPathSegment> segments;
    double total_length{std::numeric_limits<double>::infinity()};
    std::vector<double> raw_lengths; // normalized signed lengths
    std::vector<char> raw_types;     // 'L', 'R', 'S'
};

/**
 * @brief Reeds-Shepp curve generator supporting all 48 canonical forms.
 */
class ReedsShepp {
public:
    explicit ReedsShepp(double min_radius, double max_steer = 0.6);

    double min_radius() const { return min_radius_; }
    double max_steer() const { return max_steer_; }

    /**
     * @brief Find the shortest Reeds-Shepp path between start and goal configurations.
     */
    bool ShortestPath(double start_x, double start_y, double start_phi,
                      double goal_x, double goal_y, double goal_phi,
                      RSPath* best_path) const;

    /**
     * @brief Fast analytical calculation of the shortest Reeds-Shepp distance (for heuristic).
     */
    double Distance(double start_x, double start_y, double start_phi,
                    double goal_x, double goal_y, double goal_phi) const;

    /**
     * @brief Interpolate along the Reeds-Shepp path with a discretization step size.
     */
    bool Interpolate(const RSPath& path,
                     double start_x, double start_y, double start_phi,
                     double step_size,
                     std::vector<PathPoint>* sampled_points) const;

private:
    double min_radius_{5.0};
    double max_steer_{0.6};

    static double Mod2Pi(double x);
    static void Polar(double x, double y, double& r, double& theta);

    // 12 base generator routines
    static bool LeftStraightLeft(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftStraightRight(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftXRightXLeft(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftXRightLeft(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftRightXLeft(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftRightXLeftRight(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftXRightLeftXRight(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftXRight90StraightLeft(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftXRight90StraightRight(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftStraightRight90XLeft(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftStraightLeft90XRight(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);
    static bool LeftXRight90StraightLeft90XRight(double x, double y, double phi, std::vector<double>& lengths, std::vector<char>& types);

    bool GenerateAllPaths(double x, double y, double phi, std::vector<RSPath>& all_paths) const;
};

}  // namespace planning
}  // namespace flowengine
