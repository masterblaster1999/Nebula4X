#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nebula4x/core/vec2.h"

namespace nebula4x {

// Undirected edge between input points (indices into the point array).
struct DelaunayEdge {
  int a{-1};
  int b{-1};
};

namespace delaunay_detail {

inline std::uint64_t edge_key_i32(int a, int b) {
  const auto lo = static_cast<std::uint32_t>(std::min(a, b));
  const auto hi = static_cast<std::uint32_t>(std::max(a, b));
  return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
}

inline double orient2d(const Vec2& a, const Vec2& b, const Vec2& c) {
  // Cross((b-a),(c-a)).
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Test whether p lies inside the circumcircle of triangle (a,b,c).
//
// The sign depends on the orientation of the triangle; we normalize to CCW.
inline bool in_circumcircle_ccw(Vec2 a, Vec2 b, Vec2 c, const Vec2& p) {
  // Ensure triangle is CCW.
  if (orient2d(a, b, c) < 0.0) std::swap(b, c);

  const double ax = a.x - p.x;
  const double ay = a.y - p.y;
  const double bx = b.x - p.x;
  const double by = b.y - p.y;
  const double cx = c.x - p.x;
  const double cy = c.y - p.y;

  const double a2 = ax * ax + ay * ay;
  const double b2 = bx * bx + by * by;
  const double c2 = cx * cx + cy * cy;

  // Determinant of the 3x3 matrix.
  const double det = a2 * (bx * cy - cx * by) - b2 * (ax * cy - cx * ay) + c2 * (ax * by - bx * ay);

  // A small epsilon avoids unstable flips when p lies extremely close to the boundary.
  return det > 1e-12;
}

struct Tri {
  int a{-1};
  int b{-1};
  int c{-1};
};

} // namespace delaunay_detail

// Compute the (undirected) Delaunay triangulation edges for a set of 2D points.
//
// Notes:
// - The output is deterministic for a fixed point array order.
// - For degenerate inputs (e.g., all points collinear), this returns a simple
//   nearest-neighbor chain rather than failing.
inline std::vector<DelaunayEdge> delaunay_edges(const std::vector<Vec2>& points) {
  using namespace delaunay_detail;

  const int n = static_cast<int>(points.size());
  std::vector<DelaunayEdge> out;
  if (n < 2) return out;
  if (n == 2) {
    out.push_back({0, 1});
    return out;
  }

  // Compute bounding box.
  double min_x = points[0].x;
  double max_x = points[0].x;
  double min_y = points[0].y;
  double max_y = points[0].y;
  for (int i = 1; i < n; ++i) {
    min_x = std::min(min_x, points[i].x);
    max_x = std::max(max_x, points[i].x);
    min_y = std::min(min_y, points[i].y);
    max_y = std::max(max_y, points[i].y);
  }

  const double dx = max_x - min_x;
  const double dy = max_y - min_y;
  double dmax = std::max(dx, dy);
  if (!(dmax > 0.0)) dmax = 1.0;

  const double midx = (min_x + max_x) * 0.5;
  const double midy = (min_y + max_y) * 0.5;

  // Supertriangle (large enough to contain all points).
  const Vec2 p0{midx - 20.0 * dmax, midy - 1.0 * dmax};
  const Vec2 p1{midx, midy + 20.0 * dmax};
  const Vec2 p2{midx + 20.0 * dmax, midy - 1.0 * dmax};

  std::vector<Vec2> pts = points;
  pts.push_back(p0);
  pts.push_back(p1);
  pts.push_back(p2);

  const int st0 = n;
  const int st1 = n + 1;
  const int st2 = n + 2;

  // Initial triangulation: just the supertriangle.
  std::vector<Tri> tris;
  tris.reserve(static_cast<std::size_t>(n) * 3);
  // Ensure CCW.
  if (orient2d(pts[st0], pts[st1], pts[st2]) < 0.0) {
    tris.push_back({st0, st2, st1});
  } else {
    tris.push_back({st0, st1, st2});
  }

  // Incremental insertion.
  for (int pi = 0; pi < n; ++pi) {
    const Vec2 p = pts[pi];

    // Triangles whose circumcircle contains p.
    std::vector<char> bad(tris.size(), 0);
    std::size_t bad_count = 0;
    for (std::size_t ti = 0; ti < tris.size(); ++ti) {
      const Tri& t = tris[ti];
      if (in_circumcircle_ccw(pts[t.a], pts[t.b], pts[t.c], p)) {
        bad[ti] = 1;
        ++bad_count;
      }
    }

    if (bad_count == 0) {
      // Point lies outside all circumcircles; no change (rare but possible for
      // some insertion orders). Continue.
      continue;
    }

    // Boundary of the polygonal hole: edges that appear exactly once among bad triangles.
    struct EdgeRec {
      int a{-1};
      int b{-1};
      int count{0};
    };

    std::unordered_map<std::uint64_t, EdgeRec> edge_map;
    edge_map.reserve(bad_count * 3);

    auto add_edge = [&](int a, int b) {
      const int lo = std::min(a, b);
      const int hi = std::max(a, b);
      const std::uint64_t key = edge_key_i32(lo, hi);
      auto it = edge_map.find(key);
      if (it == edge_map.end()) {
        edge_map.emplace(key, EdgeRec{lo, hi, 1});
      } else {
        it->second.count++;
      }
    };

    for (std::size_t ti = 0; ti < tris.size(); ++ti) {
      if (!bad[ti]) continue;
      const Tri& t = tris[ti];
      add_edge(t.a, t.b);
      add_edge(t.b, t.c);
      add_edge(t.c, t.a);
    }

    std::vector<DelaunayEdge> boundary;
    boundary.reserve(edge_map.size());
    for (const auto& kv : edge_map) {
      const EdgeRec& e = kv.second;
      if (e.count == 1) {
        boundary.push_back({e.a, e.b});
      }
    }

    // Remove bad triangles.
    std::vector<Tri> keep;
    keep.reserve(tris.size() - bad_count + boundary.size());
    for (std::size_t ti = 0; ti < tris.size(); ++ti) {
      if (!bad[ti]) keep.push_back(tris[ti]);
    }
    tris.swap(keep);

    // Re-triangulate the hole with point pi.
    for (const auto& e : boundary) {
      int a = e.a;
      int b = e.b;
      int c = pi;

      const double o = orient2d(pts[a], pts[b], pts[c]);
      if (std::fabs(o) < 1e-14) {
        // Degenerate (collinear) triangle; skip.
        continue;
      }
      if (o < 0.0) std::swap(a, b);
      tris.push_back({a, b, c});
    }
  }

  // Collect edges from triangles that don't involve the supertriangle vertices.
  std::unordered_set<std::uint64_t> edges;
  edges.reserve(tris.size() * 3);

  for (const Tri& t : tris) {
    if (t.a >= n || t.b >= n || t.c >= n) continue;
    edges.insert(edge_key_i32(t.a, t.b));
    edges.insert(edge_key_i32(t.b, t.c));
    edges.insert(edge_key_i32(t.c, t.a));
  }

  // Degenerate fallback: if no edges, connect points in a deterministic chain.
  if (edges.empty()) {
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
      if (points[a].x == points[b].x) return points[a].y < points[b].y;
      return points[a].x < points[b].x;
    });
    for (int i = 0; i + 1 < n; ++i) out.push_back({order[i], order[i + 1]});
    return out;
  }

  out.reserve(edges.size());
  for (const auto k : edges) {
    const int a = static_cast<int>(k >> 32);
    const int b = static_cast<int>(k & 0xFFFFFFFFu);
    if (a == b) continue;
    if (a < 0 || b < 0 || a >= n || b >= n) continue;
    out.push_back({a, b});
  }

  // Sort for stable iteration (useful for deterministic downstream selection).
  std::sort(out.begin(), out.end(), [](const DelaunayEdge& x, const DelaunayEdge& y) {
    const int xa = std::min(x.a, x.b);
    const int xb = std::max(x.a, x.b);
    const int ya = std::min(y.a, y.b);
    const int yb = std::max(y.a, y.b);
    if (xa != ya) return xa < ya;
    return xb < yb;
  });

  // Remove duplicates (paranoia).
  out.erase(std::unique(out.begin(), out.end(), [](const DelaunayEdge& x, const DelaunayEdge& y) {
    return edge_key_i32(x.a, x.b) == edge_key_i32(y.a, y.b);
  }), out.end());

  return out;
}

} // namespace nebula4x
