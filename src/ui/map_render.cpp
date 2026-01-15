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

float lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

float smoothstep01(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float hash_to_f01(std::uint32_t h) {
  // 24 bits for stable float conversion.
  return static_cast<float>((h >> 8) & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

float value_noise_2d(float x, float y, std::uint32_t seed) {
  // Hash-based value noise on an integer lattice with smooth interpolation.
  const int ix0 = static_cast<int>(std::floor(x));
  const int iy0 = static_cast<int>(std::floor(y));
  const int ix1 = ix0 + 1;
  const int iy1 = iy0 + 1;

  const float fx = x - static_cast<float>(ix0);
  const float fy = y - static_cast<float>(iy0);

  const auto lattice = [&](int ix, int iy) -> float {
    const std::uint32_t ux = static_cast<std::uint32_t>(ix);
    const std::uint32_t uy = static_cast<std::uint32_t>(iy);
    const std::uint32_t h = hash_u32(seed ^ hash_u32(ux * 374761393u) ^ hash_u32(uy * 668265263u));
    return hash_to_f01(h);
  };

  const float v00 = lattice(ix0, iy0);
  const float v10 = lattice(ix1, iy0);
  const float v01 = lattice(ix0, iy1);
  const float v11 = lattice(ix1, iy1);

  const float sx = smoothstep01(fx);
  const float sy = smoothstep01(fy);

  const float vx0 = lerp(v00, v10, sx);
  const float vx1 = lerp(v01, v11, sx);
  return lerp(vx0, vx1, sy);
}

float fbm_2d(float x, float y, std::uint32_t seed, int octaves = 4) {
  // Simple fractal Brownian motion using value noise.
  float sum = 0.0f;
  float amp = 0.55f;
  float freq = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * value_noise_2d(x * freq, y * freq, seed ^ (0x9E3779B9u * static_cast<std::uint32_t>(i + 1)));
    norm += amp;
    freq *= 2.0f;
    amp *= 0.5f;
  }
  if (norm <= 1e-6f) return 0.0f;
  return sum / norm;
}

struct ScrollTiles {
  int tile_x0{0};
  int tile_y0{0};
  float frac_x{0.0f};
  float frac_y{0.0f};
};

ScrollTiles compute_scroll_tiles(float offset_px_x, float offset_px_y, float parallax, float tile_px) {
  // Convert a scrolling offset into an integer tile coordinate + fractional offset.
  // This yields an infinite, non-repeating tiled pattern (unlike wrap_mod).
  const double sx = static_cast<double>(offset_px_x) * static_cast<double>(parallax);
  const double sy = static_cast<double>(offset_px_y) * static_cast<double>(parallax);

  const double tx0d = std::floor(sx / static_cast<double>(tile_px));
  const double ty0d = std::floor(sy / static_cast<double>(tile_px));

  ScrollTiles st;
  st.tile_x0 = static_cast<int>(tx0d);
  st.tile_y0 = static_cast<int>(ty0d);

  st.frac_x = static_cast<float>(sx - tx0d * static_cast<double>(tile_px));
  st.frac_y = static_cast<float>(sy - ty0d * static_cast<double>(tile_px));

  // frac is in [0, tile_px).
  return st;
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

  // The tint is used as a very subtle colorization to help the starfield
  // harmonize with the user's chosen map background.
  const ImVec4 tint4 = ImGui::ColorConvertU32ToFloat4(rgba_u32_from_u32(tint));

  const int tiles_x = static_cast<int>(std::ceil(size.x / tile)) + 2;
  const int tiles_y = static_cast<int>(std::ceil(size.y / tile)) + 2;

  const float density = std::max(0.0f, style.density);
  const float base_parallax = std::clamp(style.parallax, 0.0f, 1.0f);
  const float base_alpha = std::clamp(style.alpha, 0.0f, 1.0f);

  // Subtle twinkle (kept extremely low amplitude so it's never distracting).
  const float time_s = static_cast<float>(ImGui::GetTime());

  // Clip to the map rect.
  draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

  // --- Procedural nebula haze (drawn behind stars) ---
  // This is intentionally conservative: it should add depth without becoming
  // a dominant overlay that competes with map data.
  if (density > 1e-3f && base_alpha > 1e-3f) {
    const float neb_parallax = std::clamp(base_parallax * 0.08f, 0.0f, 1.0f);
    const ScrollTiles sc = compute_scroll_tiles(offset_px_x, offset_px_y, neb_parallax, tile);

    for (int ty = -1; ty <= tiles_y; ++ty) {
      for (int tx = -1; tx <= tiles_x; ++tx) {
        const int gx = tx + sc.tile_x0;
        const int gy = ty + sc.tile_y0;

        const float base_x = origin.x + static_cast<float>(tx) * tile - sc.frac_x;
        const float base_y = origin.y + static_cast<float>(ty) * tile - sc.frac_y;

        const std::uint32_t hx = static_cast<std::uint32_t>(gx) * 73856093u;
        const std::uint32_t hy = static_cast<std::uint32_t>(gy) * 19349663u;
        const std::uint32_t tile_seed = hash_u32(seed ^ 0xA8F1D3B9u ^ hash_u32(hx ^ hy));
        Rng rng(tile_seed);

        const int blob_count = std::clamp(static_cast<int>(1.0f + 2.6f * density), 0, 6);
        for (int i = 0; i < blob_count; ++i) {
          const float lx = rng.next_f01() * tile;
          const float ly = rng.next_f01() * tile;

          // Global, infinite starfield-plane coordinates for coherent noise.
          const float gx_px = static_cast<float>(gx) * tile + lx;
          const float gy_px = static_cast<float>(gy) * tile + ly;

          // Low-frequency domain warp to avoid obvious grid alignment.
          const float warp = fbm_2d(gx_px * 0.00065f, gy_px * 0.00065f, seed ^ 0x3C6EF372u, 3);
          const float nx = (gx_px + 420.0f * warp) * 0.00115f;
          const float ny = (gy_px - 380.0f * warp) * 0.00115f;

          const float n = fbm_2d(nx, ny, seed ^ 0x1B873593u, 4);

          // Emphasize high values to create sparse clouds.
          float intensity = (n - 0.56f) * 2.6f;
          intensity = std::clamp(intensity, 0.0f, 1.0f);
          if (intensity <= 0.001f) continue;

          // Slight color variation (cool -> warm) driven by a separate noise sample.
          const float cnoise = fbm_2d(nx + 37.0f, ny - 19.0f, seed ^ 0x85EBCA6Bu, 3);
          const float hue = 0.60f + 0.12f * cnoise; // ~blue/purple band
          const float sat = 0.25f + 0.20f * rng.next_f01();
          const float val = 0.55f + 0.25f * intensity;

          float r = 1.0f, g = 1.0f, b = 1.0f;
          ImGui::ColorConvertHSVtoRGB(hue, sat, val, r, g, b);

          // Apply background tint (subtle).
          r = std::clamp(r * (0.75f + 0.25f * tint4.x), 0.0f, 1.0f);
          g = std::clamp(g * (0.75f + 0.25f * tint4.y), 0.0f, 1.0f);
          b = std::clamp(b * (0.75f + 0.25f * tint4.z), 0.0f, 1.0f);

          const float cx = base_x + lx;
          const float cy = base_y + ly;
          const float rad = (90.0f + 210.0f * rng.next_f01()) * (0.75f + 0.70f * intensity);

          // Alpha is intentionally low; this is "depth haze", not a gameplay overlay.
          const float aa = (0.010f + 0.055f * intensity) * base_alpha;

          const ImVec4 c0(r, g, b, aa);
          const ImVec4 c1(r, g, b, 0.0f);
          // Fake radial gradient: draw two circles.
          draw->AddCircleFilled(ImVec2(cx, cy), rad, ImGui::ColorConvertFloat4ToU32(c0), 0);
          draw->AddCircleFilled(ImVec2(cx, cy), rad * 0.55f, ImGui::ColorConvertFloat4ToU32(c1), 0);
        }
      }
    }
  }

  // --- Star layers ---
  auto draw_star_layer = [&](float parallax_mul, float density_mul, float alpha_mul, float size_mul,
                             std::uint32_t layer_tag, bool twinkle) {
    if (density <= 1e-3f || base_alpha <= 1e-3f) return;

    const float par = std::clamp(base_parallax * parallax_mul, 0.0f, 1.0f);
    const ScrollTiles sc = compute_scroll_tiles(offset_px_x, offset_px_y, par, tile);

    for (int ty = -1; ty <= tiles_y; ++ty) {
      for (int tx = -1; tx <= tiles_x; ++tx) {
        const int gx = tx + sc.tile_x0;
        const int gy = ty + sc.tile_y0;

        const float base_x = origin.x + static_cast<float>(tx) * tile - sc.frac_x;
        const float base_y = origin.y + static_cast<float>(ty) * tile - sc.frac_y;

        const std::uint32_t hx = static_cast<std::uint32_t>(gx) * 73856093u;
        const std::uint32_t hy = static_cast<std::uint32_t>(gy) * 19349663u;
        const std::uint32_t tile_seed = hash_u32(seed ^ layer_tag ^ hash_u32(hx ^ hy));
        Rng rng(tile_seed);

        const int star_count_raw = static_cast<int>(64.0f * density * density_mul);
        const int star_count = std::clamp(star_count_raw, 0, 280);
        if (star_count <= 0) continue;

        for (int i = 0; i < star_count; ++i) {
          const float lx = rng.next_f01() * tile;
          const float ly = rng.next_f01() * tile;
          const float x = base_x + lx;
          const float y = base_y + ly;

          // Size distribution: many tiny, few medium.
          const float r0 = rng.next_f01();
          float radius = 0.7f;
          if (r0 > 0.985f) radius = 2.2f;
          else if (r0 > 0.95f) radius = 1.6f;
          else if (r0 > 0.80f) radius = 1.05f;
          radius *= size_mul;

          // Brightness distribution.
          const float b0 = 0.45f + 0.55f * rng.next_f01();
          float a = (0.20f + 0.70f * b0) * base_alpha * alpha_mul;

          if (twinkle) {
            // Stable per-star phase/frequency based on hashed star coords.
            const std::uint32_t ph = hash_u32(tile_seed ^ hash_u32(static_cast<std::uint32_t>(i) * 2654435761u));
            const float phase = 6.2831853f * hash_to_f01(ph);
            const float freq = 0.55f + 1.85f * hash_to_f01(ph ^ 0x6D2B79F5u);
            const float w = 0.5f + 0.5f * std::sin(time_s * freq + phase);
            // Extremely subtle amplitude (≈ +/-3%).
            a *= (0.97f + 0.03f * w);
          }

          // Occasional colored star (very subtle).
          const float hue_r = rng.next_f01();
          float r = 1.0f;
          float g = 1.0f;
          float bl = 1.0f;
          if (hue_r < 0.06f) {
            // Cool/blue
            r = 0.85f;
            g = 0.92f;
            bl = 1.0f;
          } else if (hue_r > 0.96f) {
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
      }
    }
  };

  // Far layer: smaller, denser stars.
  draw_star_layer(0.50f, 0.55f, 0.60f, 0.82f, 0xC0FFEE11u, false);
  // Near layer: slightly larger stars with gentle twinkle.
  draw_star_layer(1.00f, 0.70f, 1.00f, 1.05f, 0xC0FFEE22u, true);

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
        // Label near the left.
        const float la = std::clamp(style.label_alpha, 0.0f, 1.0f);
        const ImU32 lcol = modulate_alpha(color, la);
        char buf[64];
        if (unit_suffix && unit_suffix[0] != '\0') {
          std::snprintf(buf, sizeof(buf), "%.0f%s", y, unit_suffix);
        } else {
          std::snprintf(buf, sizeof(buf), "%.0f", y);
        }
        draw->AddText(ImVec2(origin.x + 4.0f, a.y + 3.0f), lcol, buf);
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
  if (size.x <= 2.0f || size.y <= 2.0f) return;
  if (!(units_per_px > 0.0)) return;

  const float alpha = std::clamp(style.alpha, 0.0f, 1.0f);
  if (alpha <= 1e-4f) return;

  const double desired_units = static_cast<double>(std::max(10.0f, style.desired_px)) * units_per_px;
  const double nice_units = nice_number_125(desired_units);
  const double bar_px = nice_units / units_per_px;

  const float x0 = origin.x + 14.0f;
  const float y0 = origin.y + size.y - 18.0f;
  const float x1 = x0 + static_cast<float>(bar_px);

  const ImU32 col = modulate_alpha(color, alpha);
  draw->AddLine(ImVec2(x0, y0), ImVec2(x1, y0), col, 2.0f);
  draw->AddLine(ImVec2(x0, y0 - 4.0f), ImVec2(x0, y0 + 4.0f), col, 2.0f);
  draw->AddLine(ImVec2(x1, y0 - 4.0f), ImVec2(x1, y0 + 4.0f), col, 2.0f);

  char buf[64];
  if (unit_suffix && unit_suffix[0] != '\0') {
    std::snprintf(buf, sizeof(buf), "%.0f%s", nice_units, unit_suffix);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f", nice_units);
  }
  draw->AddText(ImVec2(x0, y0 - 16.0f), col, buf);
}

} // namespace nebula4x::ui
