#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>

namespace flowengine {
namespace planning {

/**
 * @brief 2D Vector and Point representation
 */
class Vec2d {
public:
    double x{0.0};
    double y{0.0};

    constexpr Vec2d() = default;
    constexpr Vec2d(double x_val, double y_val) : x(x_val), y(y_val) {}

    double Length() const { return std::hypot(x, y); }
    double LengthSq() const { return x * x + y * y; }
    double DistanceTo(const Vec2d& other) const { return std::hypot(x - other.x, y - other.y); }
    double DistanceSqTo(const Vec2d& other) const {
        double dx = x - other.x;
        double dy = y - other.y;
        return dx * dx + dy * dy;
    }

    double InnerProd(const Vec2d& other) const { return x * other.x + y * other.y; }
    double CrossProd(const Vec2d& other) const { return x * other.y - y * other.x; }

    Vec2d Rotate(double angle) const {
        double c = std::cos(angle);
        double s = std::sin(angle);
        return Vec2d(x * c - y * s, x * s + y * c);
    }

    Vec2d operator+(const Vec2d& other) const { return Vec2d(x + other.x, y + other.y); }
    Vec2d operator-(const Vec2d& other) const { return Vec2d(x - other.x, y - other.y); }
    Vec2d operator*(double scalar) const { return Vec2d(x * scalar, y * scalar); }
    Vec2d operator/(double scalar) const { return Vec2d(x / scalar, y / scalar); }
    Vec2d& operator+=(const Vec2d& other) { x += other.x; y += other.y; return *this; }
    Vec2d& operator-=(const Vec2d& other) { x -= other.x; y -= other.y; return *this; }
};

/**
 * @brief 2D Line Segment
 */
class LineSegment2d {
public:
    Vec2d start_;
    Vec2d end_;

    LineSegment2d() = default;
    LineSegment2d(const Vec2d& start, const Vec2d& end) : start_(start), end_(end) {}

    double Length() const { return start_.DistanceTo(end_); }
    double LengthSq() const { return start_.DistanceSqTo(end_); }

    double DistanceTo(const Vec2d& point) const {
        if (LengthSq() < 1e-12) return point.DistanceTo(start_);
        Vec2d v = end_ - start_;
        Vec2d w = point - start_;
        double c1 = w.InnerProd(v);
        if (c1 <= 0.0) return point.DistanceTo(start_);
        double c2 = v.InnerProd(v);
        if (c2 <= c1) return point.DistanceTo(end_);
        double b = c1 / c2;
        Vec2d pb = start_ + v * b;
        return point.DistanceTo(pb);
    }
};

/**
 * @brief 2D Oriented Bounding Box (OBB)
 */
class Box2d {
public:
    Box2d() = default;
    Box2d(const Vec2d& center, double heading, double length, double width);

    const Vec2d& center() const { return center_; }
    double heading() const { return heading_; }
    double length() const { return length_; }
    double width() const { return width_; }
    double half_length() const { return half_length_; }
    double half_width() const { return half_width_; }
    double cos_heading() const { return cos_heading_; }
    double sin_heading() const { return sin_heading_; }

    double min_x() const { return min_x_; }
    double max_x() const { return max_x_; }
    double min_y() const { return min_y_; }
    double max_y() const { return max_y_; }

    const std::vector<Vec2d>& GetAllCorners() const { return corners_; }

    /**
     * @brief OBB vs OBB intersection using Separating Axis Theorem (SAT)
     */
    bool HasOverlap(const Box2d& other) const;

    /**
     * @brief OBB vs Line Segment intersection
     */
    bool HasOverlap(const LineSegment2d& segment) const;

    /**
     * @brief Check whether a 2D point is inside the OBB
     */
    bool IsPointIn(const Vec2d& point) const;

    /**
     * @brief Calculate minimum distance from a point to the OBB
     */
    double DistanceTo(const Vec2d& point) const;

private:
    void InitCornersAndAABB();

    Vec2d center_{0.0, 0.0};
    double heading_{0.0};
    double length_{0.0};
    double width_{0.0};
    double half_length_{0.0};
    double half_width_{0.0};
    double cos_heading_{1.0};
    double sin_heading_{0.0};

    std::vector<Vec2d> corners_;
    double min_x_{0.0};
    double max_x_{0.0};
    double min_y_{0.0};
    double max_y_{0.0};
};

}  // namespace planning
}  // namespace flowengine
