#include "ui/map_render.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdint>

namespace nebula4x::ui {
namespace {

// Tiny deterministic RNG (xorshift32).
struct Rng {
  std::uint32_t s{1u};
  explicit Rng(std::uint32_t seed) : s(seed ? seed : 1u) {}

  std::uint32_t next_u32() {
    std::uint32_t x = s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s = x;
    return x;
  }

  float next_f01() {
    // 24 bits of mantissa.
    return static_cast<float>((next_u32() >> 8) & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
  }
};

std::uint32_t hash_u32(std::uint32_t x) {
  // A simple integer hash (Avalanche-ish).
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

float wrap_mod(float v, float m) {
  if (m <= 0.0f) return 0.0f;
  float r = std::fmod(v, m);
  if (r < 0.0f) r += m;
  return r;
}

ImVec2 to_screen(const Vec2& world, const ImVec2& center_px, double scale_px_per_unit, double zoom, const Vec2& pan) {
  const double sx = (world.x + pan.x) * scale_px_per_unit * zoom;
  const double sy = (world.y + pan.y) * scale_px_per_unit * zoom;
  return ImVec2(static_cast<float>(center_px.x + sx), static_cast<float>(center_px.y + sy));
}

Vec2 to_world(const ImVec2& screen_px, const ImVec2& center_px, double scale_px_per_unit, double zoom, const Vec2& pan) {
  const double x = (screen_px.x - center_px.x) / (scale_px_per_unit * zoom) - pan.x;
  const double y = (screen_px.y - center_px.y) / (scale_px_per_unit * zoom) - pan.y;
  return Vec2{x, y};
}

ImU32 rgba_u32_from_u32(ImU32 col) {
  return col;
}

} // namespace

double nice_number_125(double v) {
  if (!std::isfinite(v) || v <= 0.0) return 1.0;
  const double exp10 = std::floor(std::log10(v));
  const double base = std::pow(10.0, exp10);
  const double f = v / base;

  double n = 1.0;
  if (f < 1.5) {
    n = 1.0;
  } else if (f < 3.5) {
    n = 2.0;
  } else if (f < 7.5) {
    n = 5.0;
  } else {
    n = 10.0;
  }
  return n * base;
}

ImU32 modulate_alpha(ImU32 col, float alpha_mul) {
  alpha_mul = std::clamp(alpha_mul, 0.0f, 1.0f);
  const std::uint32_t a = (col >> 24) & 0xFFu;
  const std::uint32_t rgb = col & 0x00FFFFFFu;
  const std::uint32_t na = static_cast<std::uint32_t>(std::round(static_cast<float>(a) * alpha_mul));
  return rgb | (na << 24);
}

void draw_starfield(ImDrawList* draw,
                    const ImVec2& origin,
                    const ImVec2& size,
                    const ImU32& tint,
                    float offset_px_x,
                    float offset_px_y,
                    std::uint32_t seed,
                    const StarfieldStyle& style) {
  if (!draw || !style.enabled) return;
  if (size.x <= 2.0f || size.y <= 2.0f) return;

  const float tile = 520.0f;
  const float ox = wrap_mod(offset_px_x * style.parallax, tile);
  const float oy = wrap_mod(offset_px_y * style.parallax, tile);

  // The tint is used as a very subtle colorization to help the starfield
  // harmonize with the user's chosen map background.
  const ImVec4 tint4 = ImGui::ColorConvertU32ToFloat4(rgba_u32_from_u32(tint));

  const int tiles_x = static_cast<int>(std::ceil(size.x / tile)) + 2;
  const int tiles_y = static_cast<int>(std::ceil(size.y / tile)) + 2;

  // Clip to the map rect.
  draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

  for (int ty = -1; ty <= tiles_y; ++ty) {
    for (int tx = -1; tx <= tiles_x; ++tx) {
      const float base_x = origin.x + static_cast<float>(tx) * tile - ox;
      const float base_y = origin.y + static_cast<float>(ty) * tile - oy;

      // Unique deterministic seed per tile.
      const std::uint32_t tile_seed = hash_u32(seed ^ hash_u32(static_cast<std::uint32_t>(tx * 73856093) ^
                                                              static_cast<std::uint32_t>(ty * 19349663)));
      Rng rng(tile_seed);

      const int star_count = std::clamp(static_cast<int>(64.0f * style.density), 16, 220);
      for (int i = 0; i < star_count; ++i) {
        const float x = base_x + rng.next_f01() * tile;
        const float y = base_y + rng.next_f01() * tile;

        // Size distribution: many tiny, few medium.
        const float r0 = rng.next_f01();
        float radius = 0.7f;
        if (r0 > 0.985f) radius = 2.2f;
        else if (r0 > 0.95f) radius = 1.6f;
        else if (r0 > 0.80f) radius = 1.05f;

        // Brightness distribution.
        const float b = 0.45f + 0.55f * rng.next_f01();
        float a = (0.20f + 0.70f * b) * style.alpha;

        // Occasional colored star (very subtle).
        const float hue = rng.next_f01();
        float r = 1.0f;
        float g = 1.0f;
        float bl = 1.0f;
        if (hue < 0.06f) {
          // Cool/blue
          r = 0.85f;
          g = 0.92f;
          bl = 1.0f;
        } else if (hue > 0.96f) {
          // Warm/yellow
          r = 1.0f;
          g = 0.95f;
          bl = 0.82f;
        }

        // Apply background tint (subtle).
        r = std::clamp(r * (0.85f + 0.15f * tint4.x), 0.0f, 1.0f);
        g = std::clamp(g * (0.85f + 0.15f * tint4.y), 0.0f, 1.0f);
        bl = std::clamp(bl * (0.85f + 0.15f * tint4.z), 0.0f, 1.0f);

        const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, bl, a));
        draw->AddCircleFilled(ImVec2(x, y), radius, col, 0);
      }

      // Occasional soft "nebula" blotches to reduce flatness.
      // We keep this sparse to avoid an obviously tiled appearance.
      if (rng.next_f01() < 0.14f * style.density) {
        const float cx = base_x + rng.next_f01() * tile;
        const float cy = base_y + rng.next_f01() * tile;
        const float rad = 70.0f + rng.next_f01() * 140.0f;
        const float rr = 0.35f + 0.40f * rng.next_f01();
        const float gg = 0.35f + 0.40f * rng.next_f01();
        const float bb = 0.55f + 0.35f * rng.next_f01();
        const float aa = 0.06f * style.alpha;

        const ImVec4 c0(rr * tint4.z + (1.0f - tint4.z) * rr, gg, bb, aa);
        const ImVec4 c1(rr, gg, bb, 0.0f);
        // Fake radial gradient: draw two circles.
        draw->AddCircleFilled(ImVec2(cx, cy), rad, ImGui::ColorConvertFloat4ToU32(c0), 0);
        draw->AddCircleFilled(ImVec2(cx, cy), rad * 0.55f, ImGui::ColorConvertFloat4ToU32(c1), 0);
      }
    }
  }

  draw->PopClipRect();
}

void draw_grid(ImDrawList* draw,
               const ImVec2& origin,
               const ImVec2& size,
               const ImVec2& center_px,
               double scale_px_per_unit,
               double zoom,
               const Vec2& pan_units,
               const ImU32& color,
               const GridStyle& style,
               const char* unit_suffix) {
  if (!draw || !style.enabled) return;
  if (size.x <= 2.0f || size.y <= 2.0f) return;
  if (!(scale_px_per_unit > 0.0) || !(zoom > 0.0)) return;

  const double units_per_px = 1.0 / (scale_px_per_unit * zoom);
  const double raw_step = units_per_px * static_cast<double>(std::max(10.0f, style.desired_minor_px));
  const double step = nice_number_125(raw_step);
  const double major_step = step * static_cast<double>(std::max(1, style.major_every));

  const Vec2 w0 = to_world(origin, center_px, scale_px_per_unit, zoom, pan_units);
  const Vec2 w1 = to_world(ImVec2(origin.x + size.x, origin.y + size.y), center_px, scale_px_per_unit, zoom, pan_units);

  const double min_x = std::min(w0.x, w1.x);
  const double max_x = std::max(w0.x, w1.x);
  const double min_y = std::min(w0.y, w1.y);
  const double max_y = std::max(w0.y, w1.y);

  auto is_major = [&](double v) {
    if (major_step <= 0.0) return false;
    const double k = std::round(v / major_step);
    return std::abs(v - k * major_step) <= (step * 1e-4);
  };

  const ImU32 minor_col = modulate_alpha(color, style.minor_alpha);
  const ImU32 major_col = modulate_alpha(color, style.major_alpha);
  const ImU32 axis_col = modulate_alpha(color, style.axis_alpha);

  draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

  // Vertical lines.
  {
    const double start = std::floor(min_x / step) * step;
    for (double x = start; x <= max_x + step * 0.5; x += step) {
      const bool major = is_major(x);
      const ImU32 col = (std::abs(x) <= step * 1e-4) ? axis_col : (major ? major_col : minor_col);
      const float thick = (major ? 1.25f : 1.0f);
      const ImVec2 a = to_screen(Vec2{x, min_y}, center_px, scale_px_per_unit, zoom, pan_units);
      const ImVec2 b = to_screen(Vec2{x, max_y}, center_px, scale_px_per_unit, zoom, pan_units);
      draw->AddLine(a, b, col, thick);

      if (style.labels && major) {
        // Label near the top.
        const float la = std::clamp(style.label_alpha, 0.0f, 1.0f);
        const ImU32 lcol = modulate_alpha(color, la);
        char buf[64];
        if (unit_suffix && unit_suffix[0] != '\0') {
          std::snprintf(buf, sizeof(buf), "%.0f%s", x, unit_suffix);
        } else {
          std::snprintf(buf, sizeof(buf), "%.0f", x);
        }
        draw->AddText(ImVec2(a.x + 3.0f, origin.y + 4.0f), lcol, buf);
      }
    }
  }

  // Horizontal lines.
  {
    const double start = std::floor(min_y / step) * step;
    for (double y = start; y <= max_y + step * 0.5; y += step) {
      const bool major = is_major(y);
      const ImU32 col = (std::abs(y) <= step * 1e-4) ? axis_col : (major ? major_col : minor_col);
      const float thick = (major ? 1.25f : 1.0f);
      const ImVec2 a = to_screen(Vec2{min_x, y}, center_px, scale_px_per_unit, zoom, pan_units);
      const ImVec2 b = to_screen(Vec2{max_x, y}, center_px, scale_px_per_unit, zoom, pan_units);
      draw->AddLine(a, b, col, thick);

      if (style.labels && major) {
        const float la = std::clamp(style.label_alpha, 0.0f, 1.0f);
        const ImU32 lcol = modulate_alpha(color, la);
        char buf[64];
        if (unit_suffix && unit_suffix[0] != '\0') {
          std::snprintf(buf, sizeof(buf), "%.0f%s", y, unit_suffix);
        } else {
          std::snprintf(buf, sizeof(buf), "%.0f", y);
        }
        draw->AddText(ImVec2(origin.x + 4.0f, a.y + 2.0f), lcol, buf);
      }
    }
  }

  draw->PopClipRect();
}

void draw_scale_bar(ImDrawList* draw,
                    const ImVec2& origin,
                    const ImVec2& size,
                    double units_per_px,
                    const ImU32& color,
                    const ScaleBarStyle& style,
                    const char* unit_suffix) {
  if (!draw || !style.enabled) return;
  if (size.x <= 40.0f || size.y <= 20.0f) return;
  if (!(units_per_px > 0.0) || !std::isfinite(units_per_px)) return;

  const float desired_px = std::clamp(style.desired_px, 40.0f, 260.0f);
  const double raw_units = static_cast<double>(desired_px) * units_per_px;
  const double nice_units = nice_number_125(raw_units);
  const float bar_px = static_cast<float>(nice_units / units_per_px);

  const float pad = 10.0f;
  const ImVec2 p0(origin.x + pad, origin.y + size.y - pad);
  const ImVec2 p1(p0.x + bar_px, p0.y);

  const ImU32 col = modulate_alpha(color, style.alpha);
  draw->AddLine(p0, p1, col, 2.0f);
  draw->AddLine(ImVec2(p0.x, p0.y - 4.0f), ImVec2(p0.x, p0.y + 4.0f), col, 2.0f);
  draw->AddLine(ImVec2(p1.x, p1.y - 4.0f), ImVec2(p1.x, p1.y + 4.0f), col, 2.0f);

  char buf[64];
  if (unit_suffix && unit_suffix[0] != '\0') {
    std::snprintf(buf, sizeof(buf), "%.0f%s", nice_units, unit_suffix);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f", nice_units);
  }
  draw->AddText(ImVec2(p0.x, p0.y - 18.0f), col, buf);
}

} // namespace nebula4x::ui
