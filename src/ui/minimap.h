#pragma once

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "nebula4x/core/vec2.h"

namespace nebula4x::ui {

// Small, header-only helper for drawing interactive minimaps.
//
// The maps in Nebula4X use a simple camera model:
//   screen = (world + pan) * scale * zoom + center
// where pan is in world-units. Re-centering on a world point W is done by
// setting pan = -W.

inline bool point_in_rect(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
  return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y;
}

// Expand world bounds so they match the target aspect ratio.
// This keeps the world fully visible in the minimap without stretching.
inline void expand_bounds_to_aspect(Vec2& world_min, Vec2& world_max, float target_aspect) {
  const double eps = 1e-9;
  double w = world_max.x - world_min.x;
  double h = world_max.y - world_min.y;

  if (w < eps) {
    const double cx = (world_min.x + world_max.x) * 0.5;
    world_min.x = cx - 0.5;
    world_max.x = cx + 0.5;
    w = world_max.x - world_min.x;
  }
  if (h < eps) {
    const double cy = (world_min.y + world_max.y) * 0.5;
    world_min.y = cy - 0.5;
    world_max.y = cy + 0.5;
    h = world_max.y - world_min.y;
  }

  const double aspect = w / h;
  if (target_aspect <= 1e-6f) return;

  if (aspect > static_cast<double>(target_aspect)) {
    // World is wider than minimap: expand Y.
    const double new_h = w / static_cast<double>(target_aspect);
    const double pad = (new_h - h) * 0.5;
    world_min.y -= pad;
    world_max.y += pad;
  } else {
    // World is taller than minimap: expand X.
    const double new_w = h * static_cast<double>(target_aspect);
    const double pad = (new_w - w) * 0.5;
    world_min.x -= pad;
    world_max.x += pad;
  }
}

struct MinimapTransform {
  ImVec2 p0{0.0f, 0.0f};
  ImVec2 p1{0.0f, 0.0f};
  Vec2 world_min{0.0, 0.0};
  Vec2 world_max{1.0, 1.0};

  [[nodiscard]] float width_px() const { return std::max(1.0f, p1.x - p0.x); }
  [[nodiscard]] float height_px() const { return std::max(1.0f, p1.y - p0.y); }
  [[nodiscard]] double width_world() const { return std::max(1e-9, world_max.x - world_min.x); }
  [[nodiscard]] double height_world() const { return std::max(1e-9, world_max.y - world_min.y); }
  [[nodiscard]] float aspect_px() const { return width_px() / height_px(); }
};

inline MinimapTransform make_minimap_transform(ImVec2 p0, ImVec2 p1, Vec2 world_min, Vec2 world_max,
                                              bool keep_aspect = true) {
  MinimapTransform t;
  t.p0 = p0;
  t.p1 = p1;
  t.world_min = world_min;
  t.world_max = world_max;
  if (keep_aspect) {
    expand_bounds_to_aspect(t.world_min, t.world_max, t.aspect_px());
  }
  return t;
}

inline ImVec2 world_to_minimap_px(const MinimapTransform& t, const Vec2& w) {
  const double ux = (w.x - t.world_min.x) / t.width_world();
  const double uy = (w.y - t.world_min.y) / t.height_world();
  const float x = t.p0.x + static_cast<float>(ux) * (t.p1.x - t.p0.x);
  const float y = t.p0.y + static_cast<float>(uy) * (t.p1.y - t.p0.y);
  return ImVec2(x, y);
}

inline Vec2 minimap_px_to_world(const MinimapTransform& t, const ImVec2& p) {
  const float wpx = std::max(1.0f, t.p1.x - t.p0.x);
  const float hpx = std::max(1.0f, t.p1.y - t.p0.y);
  const double ux = std::clamp<double>((p.x - t.p0.x) / wpx, 0.0, 1.0);
  const double uy = std::clamp<double>((p.y - t.p0.y) / hpx, 0.0, 1.0);
  return Vec2{t.world_min.x + ux * t.width_world(), t.world_min.y + uy * t.height_world()};
}

inline ImVec2 clamp_to_rect(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
  return ImVec2(std::clamp(p.x, a.x, b.x), std::clamp(p.y, a.y, b.y));
}

} // namespace nebula4x::ui
