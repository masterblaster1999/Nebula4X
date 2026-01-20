#include "ui/procgen_graphics.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace nebula4x::ui::procgen_gfx {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct StampGridCacheEntry {
  std::uint64_t stamp_hash{0};
  SurfaceStampGrid grid;
};

static std::uint64_t fnv1a64(std::string_view s) {
  // Fowler–Noll–Vo 1a 64-bit.
  std::uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : s) {
    h ^= static_cast<std::uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

static std::unordered_map<Id, StampGridCacheEntry> g_stamp_grid_cache;


static std::uint64_t splitmix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

static float fract01(std::uint32_t u) { return static_cast<float>(u) / 4294967295.0f; }

static ImU32 hsv(float h, float s, float v, float a) {
  const ImVec4 c = ImColor::HSV(std::fmod(h, 1.0f), std::clamp(s, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f),
                                std::clamp(a, 0.0f, 1.0f));
  return ImGui::ColorConvertFloat4ToU32(c);
}

static ImU32 modulate_alpha(ImU32 col, float a_mul) {
  a_mul = std::clamp(a_mul, 0.0f, 1.0f);
  ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
  c.w *= a_mul;
  return ImGui::ColorConvertFloat4ToU32(c);
}

static ImU32 scale_rgb(ImU32 col, float mul, float a_mul) {
  mul = std::max(0.0f, mul);
  ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
  c.x = std::clamp(c.x * mul, 0.0f, 1.0f);
  c.y = std::clamp(c.y * mul, 0.0f, 1.0f);
  c.z = std::clamp(c.z * mul, 0.0f, 1.0f);
  c.w = std::clamp(c.w * a_mul, 0.0f, 1.0f);
  return ImGui::ColorConvertFloat4ToU32(c);
}



static std::uint32_t hash_u32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static float rand01(std::uint32_t x) { return fract01(hash_u32(x)); }

static std::uint32_t hash2_u32(std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
  // A simple 2D hash using a couple rounds of mixing.
  std::uint32_t h = hash_u32(x + 0x9e3779b9u);
  h ^= hash_u32(y + 0x85ebca6bu);
  h ^= hash_u32(seed + 0xc2b2ae35u);
  return hash_u32(h);
}

static ImU32 lerp_col(ImU32 a, ImU32 b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a);
  const ImVec4 cb = ImGui::ColorConvertU32ToFloat4(b);
  const ImVec4 c(ca.x + (cb.x - ca.x) * t,
                ca.y + (cb.y - ca.y) * t,
                ca.z + (cb.z - ca.z) * t,
                ca.w + (cb.w - ca.w) * t);
  return ImGui::ColorConvertFloat4ToU32(c);
}

static void draw_ellipse_arc(ImDrawList* dl, ImVec2 c, float rx, float ry, float a0, float a1,
                            ImU32 col, float thickness, int segs) {
  if (!dl) return;
  if (rx <= 0.5f || ry <= 0.5f) return;
  segs = std::clamp(segs, 6, 64);

  ImVec2 pts[65];
  for (int i = 0; i <= segs; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(segs);
    const float a = a0 + (a1 - a0) * t;
    pts[i] = ImVec2(c.x + std::cos(a) * rx, c.y + std::sin(a) * ry);
  }
  dl->AddPolyline(pts, segs + 1, col, false, thickness);
}
static ImU32 color_for_cell(char c, const SurfacePalette& pal) {
  switch (c) {
    // Terrestrial.
    case '~':
      return pal.ocean;
    case '.':
      return pal.land;
    case ',':
      return pal.hills;
    case '^':
      return pal.mountain;
    case '*':
      return pal.ice;
    case ':':
      return pal.desert;
    case ';':
      return pal.hills;

    // Gas giants.
    case '=':
      return pal.bright;
    case '-':
      return pal.mid;
    case '_':
      return pal.dark;
    case 'O':
      return pal.storm;

    // Minor bodies.
    case '#':
      return pal.rock;
    case 'o':
      return pal.crater;

    // Stars / general.
    case ' ':
      return pal.empty;
    case '!':
      return scale_rgb(pal.star_mid, 1.2f, 1.0f);

    default:
      // Star brightness ramp: " .:-=+*#%@"
      if (c == ' ') return pal.empty;
      if (c == '.') return pal.land;
      break;
  }

  static constexpr std::string_view ramp = " .:-=+*#%@";
  const std::size_t idx = ramp.find(c);
  if (idx != std::string_view::npos && ramp.size() > 1) {
    const float t = static_cast<float>(idx) / static_cast<float>(ramp.size() - 1);
    // Dark edge -> bright core.
    return scale_rgb(pal.star_mid, 0.55f + 0.9f * t, 1.0f);
  }

  // Fallback.
  return pal.land ? pal.land : IM_COL32(200, 200, 200, 255);
}

}  // namespace

SurfaceStampGrid parse_surface_stamp_grid(const std::string& stamp) {
  SurfaceStampGrid out;

  std::vector<std::string_view> lines;
  lines.reserve(32);

  std::size_t start = 0;
  while (start < stamp.size()) {
    std::size_t end = stamp.find('\n', start);
    if (end == std::string::npos) end = stamp.size();
    lines.push_back(std::string_view(stamp).substr(start, end - start));
    start = end + 1;
  }

  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  if (lines.size() < 3) return out;

  int top = -1;
  int bottom = -1;
  for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
    const auto ln = lines[static_cast<std::size_t>(i)];
    if (ln.size() >= 3 && ln.front() == '+' && ln.back() == '+') {
      top = i;
      break;
    }
  }
  for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
    const auto ln = lines[static_cast<std::size_t>(i)];
    if (ln.size() >= 3 && ln.front() == '+' && ln.back() == '+') {
      bottom = i;
      break;
    }
  }

  if (top < 0 || bottom <= top + 1) return out;

  std::vector<std::string_view> rows;
  rows.reserve(static_cast<std::size_t>(bottom - top));
  int w = 1 << 30;

  for (int i = top + 1; i < bottom; ++i) {
    const auto ln = lines[static_cast<std::size_t>(i)];
    if (ln.size() < 2 || ln.front() != '|' || ln.back() != '|') continue;
    const auto inner = ln.substr(1, ln.size() - 2);
    if (inner.empty()) continue;
    w = std::min<int>(w, static_cast<int>(inner.size()));
    rows.push_back(inner);
  }

  if (rows.empty() || w <= 0) return out;

  out.w = w;
  out.h = static_cast<int>(rows.size());
  out.cells.resize(static_cast<std::size_t>(out.w * out.h), ' ');
  for (int y = 0; y < out.h; ++y) {
    const auto row = rows[static_cast<std::size_t>(y)];
    for (int x = 0; x < out.w; ++x) {
      out.cells[static_cast<std::size_t>(y * out.w + x)] = row[static_cast<std::size_t>(x)];
    }
  }

  return out;
}

const SurfaceStampGrid& cached_surface_stamp_grid(Id stable_id, const std::string& stamp) {
  // The stamp strings are small (a few hundred bytes) but parsing them every frame
  // in immediate-mode UI can add up when browsing large systems.
  const std::uint64_t h = fnv1a64(stamp);

  // If we don't have a stable id, fall back to a single static buffer.
  if (stable_id == kInvalidId) {
    static StampGridCacheEntry tmp;
    if (tmp.stamp_hash != h) {
      tmp.stamp_hash = h;
      tmp.grid = parse_surface_stamp_grid(stamp);
    }
    return tmp.grid;
  }

  auto it = g_stamp_grid_cache.find(stable_id);
  if (it != g_stamp_grid_cache.end() && it->second.stamp_hash == h && it->second.grid.valid()) {
    return it->second.grid;
  }

  StampGridCacheEntry ent;
  ent.stamp_hash = h;
  ent.grid = parse_surface_stamp_grid(stamp);

  // Keep the cache bounded. If something goes wrong (e.g. many scenarios in one session),
  // a full clear is acceptable: recomputation is cheap and deterministic.
  constexpr std::size_t kMaxCacheEntries = 2048;
  if (g_stamp_grid_cache.size() > kMaxCacheEntries) {
    g_stamp_grid_cache.clear();
  }

  auto [it2, inserted] = g_stamp_grid_cache.insert({stable_id, std::move(ent)});
  if (!inserted) it2->second = std::move(ent);
  return it2->second.grid;
}

void clear_surface_stamp_cache() { g_stamp_grid_cache.clear(); }


SurfacePalette palette_for_body(const Body& b) {
  SurfacePalette p;

  // Use the body id as the stable base for palette variation.
  const std::uint64_t seed64 = splitmix64(0xC0FFEEULL ^ (static_cast<std::uint64_t>(b.id) << 1) ^
                                          (static_cast<std::uint64_t>(b.system_id) << 17) ^
                                          (static_cast<std::uint64_t>(static_cast<int>(b.type)) << 49));
  const float h0 = std::fmod(fract01(static_cast<std::uint32_t>(seed64 & 0xffffffffu)) * 0.85f + 0.08f, 1.0f);

  // Terrestrial base palette.
  p.ocean = hsv(h0 + 0.58f, 0.60f, 0.88f, 1.0f);
  p.land = hsv(h0 + 0.28f, 0.55f, 0.80f, 1.0f);
  p.hills = hsv(h0 + 0.28f, 0.60f, 0.64f, 1.0f);
  p.mountain = hsv(h0 + 0.25f, 0.18f, 0.92f, 1.0f);
  p.ice = hsv(h0 + 0.60f, 0.10f, 0.97f, 1.0f);
  p.desert = hsv(h0 + 0.12f, 0.40f, 0.92f, 1.0f);

  // Gas giant bands.
  p.bright = hsv(h0 + 0.08f, 0.30f, 0.94f, 1.0f);
  p.mid = hsv(h0 + 0.10f, 0.26f, 0.80f, 1.0f);
  p.dark = hsv(h0 + 0.12f, 0.35f, 0.62f, 1.0f);
  p.storm = hsv(h0 + 0.02f, 0.55f, 0.76f, 1.0f);

  // Minor bodies.
  p.rock = hsv(h0 + 0.07f, 0.14f, 0.75f, 1.0f);
  p.regolith = hsv(h0 + 0.07f, 0.10f, 0.62f, 1.0f);
  p.crater = hsv(h0 + 0.07f, 0.18f, 0.50f, 1.0f);

  // Star palette: temperature -> hue.
  double temp = b.surface_temp_k;
  if (!std::isfinite(temp) || temp <= 0.0) temp = 5800.0;
  temp = std::clamp(temp, 2500.0, 30000.0);
  const float t01 = static_cast<float>((temp - 2500.0) / (30000.0 - 2500.0));

  // Cool stars lean orange/red, hot stars lean blue.
  const float h_star = 0.02f + 0.58f * t01;
  const float s_star = 0.10f + 0.35f * std::fabs(t01 - 0.55f);
  p.star_mid = hsv(h_star, s_star, 0.95f, 1.0f);
  p.star_cool = hsv(h_star * 0.85f, s_star + 0.05f, 0.90f, 1.0f);
  p.star_hot = hsv(std::fmod(h_star + 0.04f, 1.0f), s_star + 0.08f, 1.0f, 1.0f);

  // Transparent.
  p.empty = IM_COL32(0, 0, 0, 0);

  // Type-specific tweaks.
  if (b.type == BodyType::Star) {
    // Make "land" colors irrelevant.
    p.land = p.star_mid;
    p.hills = scale_rgb(p.star_mid, 0.85f, 1.0f);
    p.mountain = p.star_hot;
    p.ocean = scale_rgb(p.star_mid, 0.65f, 1.0f);
  } else if (b.type == BodyType::GasGiant) {
    // Slightly more contrast for band readability.
    p.bright = scale_rgb(p.bright, 1.05f, 1.0f);
    p.dark = scale_rgb(p.dark, 0.92f, 1.0f);
  }

  return p;
}

void draw_surface_stamp_pixels(ImDrawList* dl, ImVec2 p0, ImVec2 size, const SurfaceStampGrid& g,
                               const SurfacePalette& pal, float alpha, bool draw_border) {
  if (!dl || !g.valid()) return;

  const float cell = std::floor(std::min(size.x / std::max(1, g.w), size.y / std::max(1, g.h)));
  if (cell < 1.0f) return;

  const ImVec2 total(cell * g.w, cell * g.h);
  const ImVec2 o(p0.x + (size.x - total.x) * 0.5f, p0.y + (size.y - total.y) * 0.5f);

  if (draw_border) {
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImVec2 pad(3.0f, 3.0f);
    const ImVec2 a(o.x - pad.x, o.y - pad.y);
    const ImVec2 b(o.x + total.x + pad.x, o.y + total.y + pad.y);
    dl->AddRectFilled(a, b, modulate_alpha(bg, 0.75f * alpha), 4.0f);
    dl->AddRect(a, b, modulate_alpha(border, 0.9f * alpha), 4.0f, 0, 1.0f);
  }

  for (int y = 0; y < g.h; ++y) {
    for (int x = 0; x < g.w; ++x) {
      const char c = g.at(x, y);
      ImU32 col = color_for_cell(c, pal);
      if ((col & IM_COL32_A_MASK) == 0) continue;
      col = modulate_alpha(col, alpha);

      const ImVec2 a(o.x + cell * x, o.y + cell * y);
      const ImVec2 b(a.x + cell, a.y + cell);
      dl->AddRectFilled(a, b, col);
    }
  }
}

void draw_star_glyph(ImDrawList* dl, ImVec2 center, float r, std::uint32_t seed, ImU32 col, float alpha) {
  if (!dl || r <= 0.5f) return;

  const int spikes = 4 + static_cast<int>(seed % 4u);
  const float rot = fract01(seed * 2654435761u) * 2.0f * kPi;
  const float r0 = r * 0.58f;
  const float r1 = r * (1.10f + 0.12f * fract01(seed ^ 0xA5A5A5A5u));

  ImVec2 pts[32];
  const int n = std::min(2 * spikes, 32);
  for (int i = 0; i < n; ++i) {
    const float a = rot + (static_cast<float>(i) / static_cast<float>(n)) * 2.0f * kPi;
    const float rr = (i % 2 == 0) ? r1 : r0;
    pts[i] = ImVec2(center.x + std::cos(a) * rr, center.y + std::sin(a) * rr);
  }

  const ImU32 glow = modulate_alpha(col, 0.14f * alpha);
  dl->AddCircleFilled(center, r1 * 1.25f, glow, 0);
  dl->AddConvexPolyFilled(pts, n, modulate_alpha(col, 1.0f * alpha));
  dl->AddCircleFilled(center, r * 0.62f, modulate_alpha(IM_COL32(255, 255, 255, 255), 0.25f * alpha), 0);
}


void draw_body_glyph(ImDrawList* dl, ImVec2 center, float r, const Body& b, float alpha, bool selected) {
  if (!dl || r <= 0.8f) return;
  alpha = std::clamp(alpha, 0.0f, 1.0f);

  // For extremely small glyphs just draw a dot.
  if (r < 2.2f) {
    const SurfacePalette pal = palette_for_body(b);
    ImU32 col = pal.land ? pal.land : IM_COL32(200, 200, 200, 255);
    if (b.type == BodyType::Star) col = pal.star_mid;
    dl->AddCircleFilled(center, r, modulate_alpha(col, alpha), 0);
    if (selected) {
      dl->AddCircle(center, r + 1.5f, modulate_alpha(IM_COL32(255, 220, 80, 255), 0.85f * alpha), 0, 1.5f);
    }
    return;
  }

  const SurfacePalette pal = palette_for_body(b);
  const std::uint32_t seed = hash_u32(static_cast<std::uint32_t>(b.id) ^ (static_cast<std::uint32_t>(b.system_id) << 11) ^
                                      (static_cast<std::uint32_t>(static_cast<int>(b.type)) * 0x9e3779b9u));

  // Subtle shadow to pop on dark backgrounds.
  dl->AddCircleFilled(ImVec2(center.x + 1.0f, center.y + 1.0f), r + 0.35f, modulate_alpha(IM_COL32(0, 0, 0, 255), 0.25f * alpha), 0);

  if (b.type == BodyType::Star) {
    const ImU32 star = pal.star_mid;
    // Soft glow.
    dl->AddCircleFilled(center, r * 1.9f, modulate_alpha(star, 0.08f * alpha), 0);
    dl->AddCircleFilled(center, r * 1.35f, modulate_alpha(star, 0.16f * alpha), 0);
    // Core.
    draw_star_glyph(dl, center, r * 0.95f, seed, star, alpha);
    dl->AddCircle(center, r, modulate_alpha(IM_COL32(255, 240, 190, 255), 0.35f * alpha), 0, 1.25f);
  } else if (b.type == BodyType::GasGiant) {
    // Base fill.
    dl->AddCircleFilled(center, r, modulate_alpha(pal.mid, alpha), 0);

    // Banding via scanlines (clipped to the circle).
    const float step = std::clamp(r * 0.18f, 1.0f, 2.5f);
    const int bands = 4 + static_cast<int>(seed % 5u);
    for (float yy = -r; yy <= r; yy += step) {
      const float y0 = center.y + yy;
      const float y1 = center.y + std::min(yy + step, r);
      const float yr0 = y0 - center.y;
      const float yr1 = y1 - center.y;
      const float x0 = std::sqrt(std::max(0.0f, r * r - yr0 * yr0));
      const float x1 = std::sqrt(std::max(0.0f, r * r - yr1 * yr1));
      const float x = std::min(x0, x1);
      if (x <= 0.0f) continue;

      const float lat01 = (yy + r) / (2.0f * r);
      const int bi = static_cast<int>(std::floor(lat01 * bands));
      const float n = rand01(seed ^ static_cast<std::uint32_t>(bi * 0x27d4eb2d));
      const float phase = (std::sin((lat01 + n * 0.15f) * 6.2831853f * (1.15f + 0.35f * n)) + 1.0f) * 0.5f;

      ImU32 c0 = pal.bright;
      ImU32 c1 = pal.dark;
      // Slight per-body hue variance.
      if ((bi + static_cast<int>(seed & 3u)) % 3 == 0) {
        c0 = pal.mid;
        c1 = pal.dark;
      }
      ImU32 band_col = lerp_col(c1, c0, phase);

      // Edge darkening.
      const float edge = 1.0f - 0.28f * std::pow(std::abs(lat01 - 0.5f) * 2.0f, 1.35f);
      band_col = scale_rgb(band_col, edge, alpha);

      dl->AddRectFilled(ImVec2(center.x - x, y0), ImVec2(center.x + x, y1 + 0.5f), band_col);
    }

    // Storm spot.
    if (r >= 4.0f) {
      const float sx = (rand01(seed ^ 0xA1B2C3D4u) - 0.5f) * r * 0.9f;
      const float sy = (rand01(seed ^ 0xC0FFEEu) - 0.25f) * r * 0.55f;
      const float sr = r * (0.15f + 0.10f * rand01(seed ^ 0xF00DF00Du));
      dl->AddCircleFilled(ImVec2(center.x + sx, center.y + sy), sr, modulate_alpha(pal.storm, 0.55f * alpha), 0);
      dl->AddCircle(ImVec2(center.x + sx, center.y + sy), sr, modulate_alpha(IM_COL32(255, 255, 255, 255), 0.15f * alpha), 0, 1.0f);
    }

    // Optional rings (back arc, planet, front arc).
    if (r >= 5.5f && ((seed >> 3) % 5u == 0u)) {
      const float rx = r * (1.55f + 0.20f * rand01(seed ^ 0x11111111u));
      const float tilt = 0.18f + 0.38f * rand01(seed ^ 0x22222222u);
      const float ry = rx * tilt;
      const int seg = 24;
      const ImU32 ring_back = modulate_alpha(IM_COL32(210, 200, 255, 255), 0.18f * alpha);
      const ImU32 ring_front = modulate_alpha(IM_COL32(220, 210, 255, 255), 0.45f * alpha);
      // Back (upper) half first.
      draw_ellipse_arc(dl, center, rx, ry, kPi, 2.0f * kPi, ring_back, 2.0f, seg);
      // Front (lower) half.
      draw_ellipse_arc(dl, center, rx, ry, 0.0f, kPi, ring_front, 2.0f, seg);
    }

    dl->AddCircle(center, r, modulate_alpha(IM_COL32(0, 0, 0, 255), 0.25f * alpha), 0, 1.0f);
  } else {
    // Terrestrial / minor bodies.
    const bool is_minor = (b.type == BodyType::Asteroid || b.type == BodyType::Comet);

    // Base fill.
    ImU32 base = is_minor ? pal.regolith : pal.ocean;

    // Temperature-driven bias (if temperature is unknown, assume temperate).
    double temp = b.surface_temp_k;
    if (!std::isfinite(temp) || temp <= 1.0) temp = 288.0;

    const bool cold = (temp < 255.0);
    const bool hot = (temp > 340.0);

    if (is_minor) {
      base = lerp_col(pal.rock, pal.regolith, 0.55f);
    } else if (cold) {
      base = lerp_col(pal.ocean, pal.ice, 0.45f);
    } else if (hot) {
      base = lerp_col(pal.land, pal.desert, 0.55f);
    }

    dl->AddCircleFilled(center, r, modulate_alpha(base, alpha), 0);

    // Surface variation (scanline segmentation, clipped to the circle).
    const float step = std::clamp(r * 0.22f, 1.0f, 3.0f);
    const int segs = std::clamp(static_cast<int>(2.0f + r * 0.60f), 2, 8);
    for (float yy = -r; yy <= r; yy += step) {
      const float y0 = center.y + yy;
      const float y1 = center.y + std::min(yy + step, r);
      const float yr0 = y0 - center.y;
      const float yr1 = y1 - center.y;
      const float x0 = std::sqrt(std::max(0.0f, r * r - yr0 * yr0));
      const float x1 = std::sqrt(std::max(0.0f, r * r - yr1 * yr1));
      const float x = std::min(x0, x1);
      if (x <= 0.0f) continue;

      const float span = 2.0f * x;
      const float seg_w = span / static_cast<float>(segs);

      for (int sx = 0; sx < segs; ++sx) {
        const float seg_x0 = -x + seg_w * static_cast<float>(sx);
        const float seg_x1 = -x + seg_w * static_cast<float>(sx + 1);
        const float cx = (seg_x0 + seg_x1) * 0.5f;

        // Stable 2D-ish noise from (segment, latitude).
        const std::uint32_t h = hash2_u32(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>((yy + r) / step), seed);
        const float n = rand01(h);

        ImU32 c = base;
        if (is_minor) {
          c = lerp_col(pal.regolith, pal.crater, 0.35f + 0.55f * n);
        } else {
          // Mix between ocean/land/hills/ice/desert based on temp + noise.
          if (cold && n > 0.35f) c = lerp_col(pal.ice, pal.land, 0.25f + 0.35f * n);
          else if (hot && n > 0.55f) c = lerp_col(pal.desert, pal.land, 0.15f + 0.30f * n);
          else if (n > 0.72f) c = pal.mountain;
          else if (n > 0.58f) c = pal.hills;
          else if (n > 0.40f) c = pal.land;
          else c = pal.ocean;
        }

        // Gentle lighting falloff toward the rim.
        const float rr = std::sqrt((cx * cx) + (yy * yy));
        const float rim = 1.0f - 0.35f * std::pow(std::clamp(rr / r, 0.0f, 1.0f), 1.65f);
        c = scale_rgb(c, rim, alpha);

        dl->AddRectFilled(ImVec2(center.x + seg_x0, y0), ImVec2(center.x + seg_x1, y1 + 0.5f), c);
      }
    }

    // Atmosphere halo for thicker atmospheres.
    if (!is_minor) {
      const double atm = std::clamp(b.atmosphere_atm, 0.0, 10.0);
      if (atm > 0.12 && r >= 3.0f) {
        const float a = std::clamp(static_cast<float>(0.08 + 0.10 * std::log10(atm + 1.0)), 0.06f, 0.22f);
        const ImU32 halo = modulate_alpha(IM_COL32(140, 210, 255, 255), a * alpha);
        dl->AddCircle(center, r + 1.0f, halo, 0, 2.0f);
      }
    }

    // Terminator shadow + highlight (simple fake lighting).
    const float lx = (rand01(seed ^ 0xFACEB00Cu) - 0.2f);
    const float ly = (rand01(seed ^ 0x00C0FFEEu) - 0.5f);
    dl->AddCircleFilled(ImVec2(center.x + lx * r * 0.55f, center.y + ly * r * 0.35f), r * 0.95f,
                       modulate_alpha(IM_COL32(0, 0, 0, 255), 0.12f * alpha), 0);
    dl->AddCircleFilled(ImVec2(center.x - lx * r * 0.40f, center.y - ly * r * 0.25f), r * 0.55f,
                       modulate_alpha(IM_COL32(255, 255, 255, 255), 0.05f * alpha), 0);

    // A few craters for minor bodies.
    if (is_minor && r >= 3.0f) {
      const int n = 1 + static_cast<int>((seed >> 5) % 3u);
      for (int i = 0; i < n; ++i) {
        const float ax = (rand01(seed ^ (0x11110000u + i * 1337u)) - 0.5f) * r * 0.9f;
        const float ay = (rand01(seed ^ (0x22220000u + i * 7331u)) - 0.5f) * r * 0.7f;
        const float cr = r * (0.12f + 0.10f * rand01(seed ^ (0x33330000u + i * 9001u)));
        dl->AddCircleFilled(ImVec2(center.x + ax, center.y + ay), cr, modulate_alpha(pal.crater, 0.55f * alpha), 0);
      }
    }

    dl->AddCircle(center, r, modulate_alpha(IM_COL32(0, 0, 0, 255), 0.25f * alpha), 0, 1.0f);
  }

  if (selected) {
    dl->AddCircle(center, r + 2.0f, modulate_alpha(IM_COL32(255, 220, 80, 255), 0.90f * alpha), 0, 2.0f);
  }
}

void draw_jump_glyph(ImDrawList* dl, ImVec2 center, float r, std::uint32_t seed, ImU32 col, float alpha, bool surveyed) {
  if (!dl || r <= 0.8f) return;
  alpha = std::clamp(alpha, 0.0f, 1.0f);

  // Base color can be dimmed for unsurveyed points.
  const float dim = surveyed ? 1.0f : 0.55f;
  col = scale_rgb(col, dim, alpha);

  // Outer glow.
  dl->AddCircleFilled(center, r * 1.35f, modulate_alpha(col, 0.08f * alpha), 0);
  dl->AddCircleFilled(center, r * 1.05f, modulate_alpha(col, 0.12f * alpha), 0);

  // Portal rings.
  const int rings = 3 + static_cast<int>(seed % 3u);
  for (int i = 0; i < rings; ++i) {
    const float rr = r * (0.40f + 0.18f * static_cast<float>(i));
    const float a = 0.35f - 0.08f * static_cast<float>(i);
    dl->AddCircle(center, rr, modulate_alpha(col, a * alpha), 0, 1.5f);
  }

  // Swirl segments.
  const int seg = 20;
  const float rot = rand01(seed ^ 0xDEADBEEFu) * 2.0f * kPi;
  for (int i = 0; i < seg; ++i) {
    const float t0 = static_cast<float>(i) / static_cast<float>(seg);
    const float t1 = static_cast<float>(i + 1) / static_cast<float>(seg);
    const float a0 = rot + t0 * 2.0f * kPi;
    const float a1 = rot + t1 * 2.0f * kPi;

    const float rr0 = r * (0.25f + 0.55f * t0);
    const float rr1 = r * (0.25f + 0.55f * t1);

    const ImVec2 p0(center.x + std::cos(a0) * rr0, center.y + std::sin(a0) * rr0);
    const ImVec2 p1(center.x + std::cos(a1) * rr1, center.y + std::sin(a1) * rr1);

    const float a = 0.20f + 0.35f * (1.0f - t0);
    dl->AddLine(p0, p1, modulate_alpha(col, a * alpha), 2.0f);
  }

  // Core.
  dl->AddCircleFilled(center, r * 0.28f, modulate_alpha(IM_COL32(255, 255, 255, 255), 0.22f * alpha), 0);
  dl->AddCircle(center, r, modulate_alpha(IM_COL32(0, 0, 0, 255), 0.25f * alpha), 0, 1.0f);
}

void draw_system_badge(ImDrawList* dl, ImVec2 p0, float sz, std::uint32_t seed, int jump_degree,
                       double nebula_density, int habitable, float minerals01, bool chokepoint, bool selected) {
  if (!dl || sz < 6.0f) return;

  const ImVec2 p1(p0.x + sz, p0.y + sz);
  const float r = sz * 0.5f;
  const ImVec2 c(p0.x + r, p0.y + r);

  const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
  dl->AddRectFilled(p0, p1, modulate_alpha(bg, 0.55f), 3.0f);

  // Color theme from seed.
  const float h = std::fmod(fract01(seed * 2246822519u) * 0.85f + 0.08f, 1.0f);
  const ImU32 star_col = hsv(h + 0.08f, 0.22f, 0.98f, 1.0f);

  const float neb = static_cast<float>(std::clamp(nebula_density, 0.0, 1.0));
  if (neb > 0.01f) {
    const ImU32 haze = hsv(h + 0.62f, 0.40f, 0.95f, 0.12f + 0.22f * neb);
    dl->AddCircleFilled(c, r * (1.25f + neb * 0.35f), haze, 0);
  }

  // Star glyph.
  draw_star_glyph(dl, c, r * 0.62f, seed, star_col, 1.0f);

  // Jump degree: little satellites on the rim.
  const int dots = std::clamp(jump_degree, 0, 8);
  if (dots > 0) {
    for (int i = 0; i < dots; ++i) {
      const float a = (static_cast<float>(i) / static_cast<float>(dots)) * 2.0f * kPi;
      const ImVec2 d(c.x + std::cos(a) * r * 0.95f, c.y + std::sin(a) * r * 0.95f);
      dl->AddCircleFilled(d, std::max(1.0f, sz * 0.06f), modulate_alpha(IM_COL32(255, 255, 255, 255), 0.65f), 0);
    }
  }

  // Habitable hint: green pip bottom-left.
  if (habitable > 0) {
    dl->AddCircleFilled(ImVec2(p0.x + sz * 0.26f, p0.y + sz * 0.78f), std::max(1.5f, sz * 0.10f),
                        IM_COL32(120, 255, 170, 220), 0);
  }

  // Mineral hint: warm pip bottom-right.
  minerals01 = std::clamp(minerals01, 0.0f, 1.0f);
  if (minerals01 > 0.01f) {
    const float a = 0.18f + 0.62f * minerals01;
    dl->AddCircleFilled(ImVec2(p0.x + sz * 0.76f, p0.y + sz * 0.78f), std::max(1.5f, sz * 0.10f),
                        modulate_alpha(IM_COL32(255, 210, 110, 255), a), 0);
  }

  if (chokepoint) {
    const ImVec2 a(p0.x + 1.0f, p0.y + 1.0f);
    const ImVec2 b(p1.x - 1.0f, p1.y - 1.0f);
    dl->AddRect(a, b, IM_COL32(190, 120, 255, 220), 3.0f, 0, 1.5f);
  }

  const ImU32 outline = selected ? IM_COL32(255, 255, 255, 200) : IM_COL32(0, 0, 0, 90);
  dl->AddRect(p0, p1, outline, 3.0f, 0, selected ? 2.0f : 1.0f);
}

}  // namespace nebula4x::ui::procgen_gfx