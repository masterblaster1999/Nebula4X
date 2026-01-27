#pragma once
#include <cmath>

namespace nebula4x {

// Simple 2D vector used for map + in-system coordinates.
// Units depend on context (we often use million-km in the sim).
struct Vec2 {
  double x{0.0};
  double y{0.0};

  Vec2() = default;
  Vec2(double x_, double y_) : x(x_), y(y_) {}

  Vec2 operator+(const Vec2& rhs) const { return {x + rhs.x, y + rhs.y}; }
  Vec2 operator-(const Vec2& rhs) const { return {x - rhs.x, y - rhs.y}; }
  Vec2 operator*(double s) const { return {x * s, y * s}; }

  // Exact equality. Use with care for computed floating-point values; it is
  // primarily intended for comparisons against stored/serialized coordinates.
  bool operator==(const Vec2& rhs) const { return x == rhs.x && y == rhs.y; }
  bool operator!=(const Vec2& rhs) const { return !(*this == rhs); }

  Vec2& operator+=(const Vec2& rhs) {
    x += rhs.x;
    y += rhs.y;
    return *this;
  }

  double length() const { return std::sqrt(x * x + y * y); }
  double length_squared() const { return x * x + y * y; }
  Vec2 normalized() const {
    const double len = length();
    if (len <= 1e-12) return {0.0, 0.0};
    return {x / len, y / len};
  }
};

} // namespace nebula4x
