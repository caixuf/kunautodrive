#include "reeds_shepp_path.h"
#include <cmath>
#include <algorithm>

namespace flowengine {
namespace planning {

ReedsShepp::ReedsShepp(double min_radius, double max_steer)
    : min_radius_(min_radius), max_steer_(max_steer) {}

double ReedsShepp::Mod2Pi(double x) {
    double v = std::fmod(x, 2.0 * M_PI);
    if (v < -M_PI) v += 2.0 * M_PI;
    else if (v >= M_PI) v -= 2.0 * M_PI;
    return v;
}

void ReedsShepp::Polar(double x, double y, double& r, double& theta) {
    r = std::hypot(x, y);
    theta = std::atan2(y, x);
}

bool ReedsShepp::LeftStraightLeft(double x, double y, double phi,
                                  std::vector<double>& lengths, std::vector<char>& types) {
    double u, t;
    Polar(x - std::sin(phi), y - 1.0 + std::cos(phi), u, t);
    if (t >= 0.0) {
        double v = Mod2Pi(phi - t);
        if (v >= 0.0) {
            lengths = {t, u, v};
            types = {'L', 'S', 'L'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftStraightRight(double x, double y, double phi,
                                   std::vector<double>& lengths, std::vector<char>& types) {
    double u1, t1;
    Polar(x + std::sin(phi), y - 1.0 - std::cos(phi), u1, t1);
    double u1_sq = u1 * u1;
    if (u1_sq >= 4.0) {
        double u = std::sqrt(u1_sq - 4.0);
        double theta = std::atan2(2.0, u);
        double t = Mod2Pi(t1 + theta);
        double v = Mod2Pi(t - phi);
        if (t >= 0.0 && v >= 0.0) {
            lengths = {t, u, v};
            types = {'L', 'S', 'R'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftXRightXLeft(double x, double y, double phi,
                                std::vector<double>& lengths, std::vector<char>& types) {
    double zeta = x - std::sin(phi);
    double eeta = y - 1.0 + std::cos(phi);
    double u1, theta;
    Polar(zeta, eeta, u1, theta);
    if (u1 <= 4.0) {
        double A = std::acos(std::clamp(0.25 * u1, -1.0, 1.0));
        double t = Mod2Pi(A + theta + M_PI * 0.5);
        double u = Mod2Pi(M_PI - 2.0 * A);
        double v = Mod2Pi(phi - t - u);
        if (t >= 0.0 && u >= 0.0 && v >= 0.0) {
            lengths = {t, -u, v};
            types = {'L', 'R', 'L'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftXRightLeft(double x, double y, double phi,
                               std::vector<double>& lengths, std::vector<char>& types) {
    double zeta = x - std::sin(phi);
    double eeta = y - 1.0 + std::cos(phi);
    double u1, theta;
    Polar(zeta, eeta, u1, theta);
    if (u1 <= 4.0) {
        double A = std::acos(std::clamp(0.25 * u1, -1.0, 1.0));
        double t = Mod2Pi(A + theta + M_PI * 0.5);
        double u = Mod2Pi(M_PI - 2.0 * A);
        double v = Mod2Pi(-phi + t + u);
        if (t >= 0.0 && u >= 0.0 && v >= 0.0) {
            lengths = {t, -u, -v};
            types = {'L', 'R', 'L'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftRightXLeft(double x, double y, double phi,
                               std::vector<double>& lengths, std::vector<char>& types) {
    double zeta = x - std::sin(phi);
    double eeta = y - 1.0 + std::cos(phi);
    double u1, theta;
    Polar(zeta, eeta, u1, theta);
    if (u1 <= 4.0) {
        double u = std::acos(std::clamp(1.0 - u1 * u1 * 0.125, -1.0, 1.0));
        double A = std::asin(std::clamp(2.0 * std::sin(u) / (u1 > 1e-6 ? u1 : 1e-6), -1.0, 1.0));
        double t = Mod2Pi(-A + theta + M_PI * 0.5);
        double v = Mod2Pi(t - u - phi);
        if (t >= 0.0 && u >= 0.0 && v >= 0.0) {
            lengths = {t, u, -v};
            types = {'L', 'R', 'L'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftRightXLeftRight(double x, double y, double phi,
                                    std::vector<double>& lengths, std::vector<char>& types) {
    double zeta = x + std::sin(phi);
    double eeta = y - 1.0 - std::cos(phi);
    double u1, theta;
    Polar(zeta, eeta, u1, theta);
    if (u1 <= 2.0) {
        double A = std::acos(std::clamp((u1 + 2.0) * 0.25, -1.0, 1.0));
        double t = Mod2Pi(theta + A + M_PI * 0.5);
        double u = Mod2Pi(A);
        double v = Mod2Pi(phi - t + 2.0 * u);
        if (t >= 0.0 && u >= 0.0 && v >= 0.0) {
            lengths = {t, u, -u, -v};
            types = {'L', 'R', 'L', 'R'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftXRightLeftXRight(double x, double y, double phi,
                                     std::vector<double>& lengths, std::vector<char>& types) {
    double zeta = x + std::sin(phi);
    double eeta = y - 1.0 - std::cos(phi);
    double u1, theta;
    Polar(zeta, eeta, u1, theta);
    double u2 = (20.0 - u1 * u1) / 16.0;
    if (u2 >= 0.0 && u2 <= 1.0) {
        double u = std::acos(u2);
        double A = std::asin(std::clamp(2.0 * std::sin(u) / (u1 > 1e-6 ? u1 : 1e-6), -1.0, 1.0));
        double t = Mod2Pi(theta + A + M_PI * 0.5);
        double v = Mod2Pi(t - phi);
        if (t >= 0.0 && v >= 0.0) {
            lengths = {t, -u, -u, v};
            types = {'L', 'R', 'L', 'R'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftXRight90StraightLeft(double x, double y, double phi,
                                         std::vector<double>& lengths, std::vector<char>& types) {
    double zeta = x - std::sin(phi);
    double eeta = y - 1.0 + std::cos(phi);
    double u1, theta;
    Polar(zeta, eeta, u1, theta);
    if (u1 >= 2.0) {
        double u = std::sqrt(u1 * u1 - 4.0) - 2.0;
        double A = std::atan2(2.0, std::sqrt(u1 * u1 - 4.0));
        double t = Mod2Pi(theta + A + M_PI * 0.5);
        double v = Mod2Pi(t - phi + M_PI * 0.5);
        if (t >= 0.0 && u >= 0.0 && v >= 0.0) {
            lengths = {t, -M_PI * 0.5, -u, -v};
            types = {'L', 'R', 'S', 'L'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftXRight90StraightRight(double x, double y, double phi,
                                          std::vector<double>& lengths, std::vector<char>& types) {
    double zeta = x + std::sin(phi);
    double eeta = y - 1.0 - std::cos(phi);
    double u1, theta;
    Polar(zeta, eeta, u1, theta);
    if (u1 >= 2.0) {
        double t = Mod2Pi(theta + M_PI * 0.5);
        double u = u1 - 2.0;
        double v = Mod2Pi(phi - t - M_PI * 0.5);
        if (t >= 0.0 && u >= 0.0 && v >= 0.0) {
            lengths = {t, -M_PI * 0.5, -u, -v};
            types = {'L', 'R', 'S', 'R'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftStraightRight90XLeft(double x, double y, double phi,
                                         std::vector<double>& lengths, std::vector<char>& types) {
    double zeta = x - std::sin(phi);
    double eeta = y - 1.0 + std::cos(phi);
    double u1, theta;
    Polar(zeta, eeta, u1, theta);
    if (u1 >= 2.0) {
        double u = std::sqrt(u1 * u1 - 4.0) - 2.0;
        double A = std::atan2(std::sqrt(u1 * u1 - 4.0), 2.0);
        double t = Mod2Pi(theta - A + M_PI * 0.5);
        double v = Mod2Pi(t - phi - M_PI * 0.5);
        if (t >= 0.0 && u >= 0.0 && v >= 0.0) {
            lengths = {t, u, M_PI * 0.5, -v};
            types = {'L', 'S', 'R', 'L'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftStraightLeft90XRight(double x, double y, double phi,
                                         std::vector<double>& lengths, std::vector<char>& types) {
    double zeta = x + std::sin(phi);
    double eeta = y - 1.0 - std::cos(phi);
    double u1, theta;
    Polar(zeta, eeta, u1, theta);
    if (u1 >= 2.0) {
        double t = Mod2Pi(theta);
        double u = u1 - 2.0;
        double v = Mod2Pi(phi - t - M_PI * 0.5);
        if (t >= 0.0 && u >= 0.0 && v >= 0.0) {
            lengths = {t, u, M_PI * 0.5, -v};
            types = {'L', 'S', 'L', 'R'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::LeftXRight90StraightLeft90XRight(double x, double y, double phi,
                                                 std::vector<double>& lengths, std::vector<char>& types) {
    double zeta = x + std::sin(phi);
    double eeta = y - 1.0 - std::cos(phi);
    double u1, theta;
    Polar(zeta, eeta, u1, theta);
    if (u1 >= 4.0) {
        double u = std::sqrt(u1 * u1 - 4.0) - 4.0;
        double A = std::atan2(2.0, std::sqrt(u1 * u1 - 4.0));
        double t = Mod2Pi(theta + A + M_PI * 0.5);
        double v = Mod2Pi(t - phi);
        if (t >= 0.0 && u >= 0.0 && v >= 0.0) {
            lengths = {t, -M_PI * 0.5, -u, -M_PI * 0.5, v};
            types = {'L', 'R', 'S', 'L', 'R'};
            return true;
        }
    }
    return false;
}

bool ReedsShepp::GenerateAllPaths(double x, double y, double phi, std::vector<RSPath>& all_paths) const {
    using GeneratorFunc = bool (*)(double, double, double, std::vector<double>&, std::vector<char>&);
    static const GeneratorFunc generators[] = {
        LeftStraightLeft,
        LeftStraightRight,
        LeftXRightXLeft,
        LeftXRightLeft,
        LeftRightXLeft,
        LeftRightXLeftRight,
        LeftXRightLeftXRight,
        LeftXRight90StraightLeft,
        LeftXRight90StraightRight,
        LeftStraightRight90XLeft,
        LeftStraightLeft90XRight,
        LeftXRight90StraightLeft90XRight
    };

    all_paths.clear();
    all_paths.reserve(48);

    std::vector<double> lengths;
    std::vector<char> types;

    for (auto fn : generators) {
        struct Config {
            double gx, gy, gphi;
            bool timeflip;
            bool reflect;
        };

        const Config configs[4] = {
            { x,  y,  phi, false, false},
            {-x,  y, -phi, true,  false},
            { x, -y, -phi, false, true },
            {-x, -y,  phi, true,  true }
        };

        for (const auto& cfg : configs) {
            if (fn(cfg.gx, cfg.gy, cfg.gphi, lengths, types)) {
                RSPath p;
                p.raw_lengths = lengths;
                p.raw_types = types;
                if (cfg.timeflip) {
                    for (double& l : p.raw_lengths) l = -l;
                }
                if (cfg.reflect) {
                    for (char& t : p.raw_types) {
                        if (t == 'L') t = 'R';
                        else if (t == 'R') t = 'L';
                    }
                }
                double total = 0.0;
                for (double l : p.raw_lengths) total += std::abs(l);
                p.total_length = total * min_radius_;

                // Fill structured segments
                p.segments.resize(p.raw_lengths.size());
                for (size_t i = 0; i < p.raw_lengths.size(); ++i) {
                    p.segments[i].length = std::abs(p.raw_lengths[i]) * min_radius_;
                    p.segments[i].type = p.raw_types[i];
                    p.segments[i].direction = (p.raw_lengths[i] > 0.0) ? 1 : -1;
                    if (p.raw_types[i] == 'L') p.segments[i].steer = max_steer_;
                    else if (p.raw_types[i] == 'R') p.segments[i].steer = -max_steer_;
                    else p.segments[i].steer = 0.0;
                }

                all_paths.push_back(std::move(p));
            }
        }
    }
    return !all_paths.empty();
}

bool ReedsShepp::ShortestPath(double start_x, double start_y, double start_phi,
                              double goal_x, double goal_y, double goal_phi,
                              RSPath* best_path) const {
    if (!best_path) return false;

    double dx = goal_x - start_x;
    double dy = goal_y - start_y;
    double c = std::cos(start_phi);
    double s = std::sin(start_phi);

    double x = (c * dx + s * dy) / min_radius_;
    double y = (-s * dx + c * dy) / min_radius_;
    double phi = Mod2Pi(goal_phi - start_phi);

    std::vector<RSPath> paths;
    if (!GenerateAllPaths(x, y, phi, paths)) {
        return false;
    }

    double min_len = std::numeric_limits<double>::infinity();
    size_t best_idx = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (paths[i].total_length < min_len) {
            min_len = paths[i].total_length;
            best_idx = i;
        }
    }

    *best_path = std::move(paths[best_idx]);
    return true;
}

double ReedsShepp::Distance(double start_x, double start_y, double start_phi,
                            double goal_x, double goal_y, double goal_phi) const {
    RSPath p;
    if (ShortestPath(start_x, start_y, start_phi, goal_x, goal_y, goal_phi, &p)) {
        return p.total_length;
    }
    return std::hypot(goal_x - start_x, goal_y - start_y);
}

bool ReedsShepp::Interpolate(const RSPath& path,
                             double start_x, double start_y, double start_phi,
                             double step_size,
                             std::vector<PathPoint>* sampled_points) const {
    if (!sampled_points) return false;
    sampled_points->clear();

    double x = start_x;
    double y = start_y;
    double phi = start_phi;
    double accum_s = 0.0;

    sampled_points->push_back(PathPoint{x, y, phi, 0.0, 1, accum_s, 0.0});

    for (const auto& seg : path.segments) {
        if (seg.length <= 1e-6) continue;
        int n_steps = std::max(1, static_cast<int>(std::ceil(seg.length / step_size)));
        double ds = seg.length / n_steps;
        double dir = static_cast<double>(seg.direction);
        double kappa = 0.0;
        if (seg.type == 'L') kappa = 1.0 / min_radius_;
        else if (seg.type == 'R') kappa = -1.0 / min_radius_;

        for (int i = 0; i < n_steps; ++i) {
            if (seg.type == 'S') {
                x += dir * ds * std::cos(phi);
                y += dir * ds * std::sin(phi);
            } else if (seg.type == 'L') {
                double dphi = dir * ds / min_radius_;
                double cx = x - min_radius_ * std::sin(phi);
                double cy = y + min_radius_ * std::cos(phi);
                phi = Mod2Pi(phi + dphi);
                x = cx + min_radius_ * std::sin(phi);
                y = cy - min_radius_ * std::cos(phi);
            } else if (seg.type == 'R') {
                double dphi = -dir * ds / min_radius_;
                double cx = x + min_radius_ * std::sin(phi);
                double cy = y - min_radius_ * std::cos(phi);
                phi = Mod2Pi(phi + dphi);
                x = cx - min_radius_ * std::sin(phi);
                y = cy + min_radius_ * std::cos(phi);
            }
            accum_s += ds;
            sampled_points->push_back(PathPoint{x, y, phi, seg.steer, seg.direction, accum_s, kappa});
        }
    }
    return true;
}

}  // namespace planning
}  // namespace flowengine
