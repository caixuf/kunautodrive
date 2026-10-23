#include "box2d.h"

namespace flowengine {
namespace planning {

Box2d::Box2d(const Vec2d& center, double heading, double length, double width)
    : center_(center),
      heading_(heading),
      length_(length),
      width_(width),
      half_length_(length * 0.5),
      half_width_(width * 0.5),
      cos_heading_(std::cos(heading)),
      sin_heading_(std::sin(heading)) {
    InitCornersAndAABB();
}

void Box2d::InitCornersAndAABB() {
    double dx1 = cos_heading_ * half_length_;
    double dy1 = sin_heading_ * half_length_;
    double dx2 = -sin_heading_ * half_width_;
    double dy2 = cos_heading_ * half_width_;

    corners_.resize(4);
    corners_[0] = Vec2d(center_.x + dx1 + dx2, center_.y + dy1 + dy2);
    corners_[1] = Vec2d(center_.x - dx1 + dx2, center_.y - dy1 + dy2);
    corners_[2] = Vec2d(center_.x - dx1 - dx2, center_.y - dy1 - dy2);
    corners_[3] = Vec2d(center_.x + dx1 - dx2, center_.y + dy1 - dy2);

    min_x_ = std::min({corners_[0].x, corners_[1].x, corners_[2].x, corners_[3].x});
    max_x_ = std::max({corners_[0].x, corners_[1].x, corners_[2].x, corners_[3].x});
    min_y_ = std::min({corners_[0].y, corners_[1].y, corners_[2].y, corners_[3].y});
    max_y_ = std::max({corners_[0].y, corners_[1].y, corners_[2].y, corners_[3].y});
}

bool Box2d::HasOverlap(const Box2d& other) const {
    // 1. Broad-phase AABB test
    if (max_x_ < other.min_x_ || min_x_ > other.max_x_ ||
        max_y_ < other.min_y_ || min_y_ > other.max_y_) {
        return false;
    }

    // 2. Narrow-phase SAT test on 4 axes
    Vec2d diff = other.center_ - center_;

    // Axes
    Vec2d axes[4] = {
        Vec2d(cos_heading_, sin_heading_),
        Vec2d(-sin_heading_, cos_heading_),
        Vec2d(other.cos_heading_, other.sin_heading_),
        Vec2d(-other.sin_heading_, other.cos_heading_)
    };

    for (int i = 0; i < 4; ++i) {
        const Vec2d& axis = axes[i];
        double d = std::abs(diff.InnerProd(axis));
        double r1 = half_length_ * std::abs(axes[0].InnerProd(axis)) +
                    half_width_ * std::abs(axes[1].InnerProd(axis));
        double r2 = other.half_length_ * std::abs(axes[2].InnerProd(axis)) +
                    other.half_width_ * std::abs(axes[3].InnerProd(axis));
        if (d > r1 + r2) {
            return false;  // Separating axis found
        }
    }
    return true;
}

bool Box2d::HasOverlap(const LineSegment2d& segment) const {
    // Transform segment points to local Box frame
    Vec2d p1 = segment.start_ - center_;
    Vec2d p2 = segment.end_ - center_;

    double x1 = p1.x * cos_heading_ + p1.y * sin_heading_;
    double y1 = -p1.x * sin_heading_ + p1.y * cos_heading_;

    double x2 = p2.x * cos_heading_ + p2.y * sin_heading_;
    double y2 = -p2.x * sin_heading_ + p2.y * cos_heading_;

    // Endpoint inside check
    if (std::abs(x1) <= half_length_ && std::abs(y1) <= half_width_) return true;
    if (std::abs(x2) <= half_length_ && std::abs(y2) <= half_width_) return true;

    // Both outside same plane
    if ((x1 < -half_length_ && x2 < -half_length_) || (x1 > half_length_ && x2 > half_length_)) return false;
    if ((y1 < -half_width_ && y2 < -half_width_) || (y1 > half_width_ && y2 > half_width_)) return false;

    // Liang-Barsky clipping test
    double dx = x2 - x1;
    double dy = y2 - y1;
    double t0 = 0.0, t1 = 1.0;

    auto clip = [&](double p, double q) -> bool {
        if (std::abs(p) < 1e-12) {
            if (q < 0.0) return false;
            return true;
        }
        double r = q / p;
        if (p < 0.0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
        return true;
    };

    if (!clip(-dx, x1 - (-half_length_))) return false;
    if (!clip(dx, half_length_ - x1)) return false;
    if (!clip(-dy, y1 - (-half_width_))) return false;
    if (!clip(dy, half_width_ - y1)) return false;

    return t0 <= t1;
}

bool Box2d::IsPointIn(const Vec2d& point) const {
    Vec2d p = point - center_;
    double lx = p.x * cos_heading_ + p.y * sin_heading_;
    double ly = -p.x * sin_heading_ + p.y * cos_heading_;
    return std::abs(lx) <= half_length_ && std::abs(ly) <= half_width_;
}

double Box2d::DistanceTo(const Vec2d& point) const {
    Vec2d p = point - center_;
    double lx = p.x * cos_heading_ + p.y * sin_heading_;
    double ly = -p.x * sin_heading_ + p.y * cos_heading_;

    double dx = std::max(0.0, std::abs(lx) - half_length_);
    double dy = std::max(0.0, std::abs(ly) - half_width_);
    return std::hypot(dx, dy);
}

}  // namespace planning
}  // namespace flowengine
