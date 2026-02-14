#include "ui/galaxy_map.h"

#include "ui/map_render.h"
#include "ui/map_label_placer.h"
#include "ui/galaxy_constellations.h"
#include "ui/minimap.h"
#include "ui/procgen_metrics.h"
#include "ui/raymarch_nebula.h"
#include "ui/ruler.h"

#include "ui/proc_render_engine.h"
#include "ui/proc_particle_field_engine.h"
#include "ui/proc_territory_field_engine.h"

#include "ui/imgui_includes.h"

#include <array>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "nebula4x/util/log.h"
#include "nebula4x/util/time.h"

#include "nebula4x/core/trade_network.h"

namespace nebula4x::ui {
namespace {

ImVec2 to_screen(const Vec2& world, const ImVec2& center_px, double scale_px_per_unit, double zoom, const Vec2& pan) {
  const double sx = (world.x + pan.x) * scale_px_per_unit * zoom;
  const double sy = (world.y + pan.y) * scale_px_per_unit * zoom;
  return ImVec2(static_cast<float>(center_px.x + sx), static_cast<float>(center_px.y + sy));
}

Vec2 to_world(const ImVec2& screen_px, const ImVec2& center_px, double scale_px_per_unit, double zoom, const Vec2& pan) {
  const double denom = std::max(1e-12, scale_px_per_unit * zoom);
  const double x = (screen_px.x - center_px.x) / denom - pan.x;
  const double y = (screen_px.y - center_px.y) / denom - pan.y;
  return Vec2{x, y};
}

void add_arrowhead(ImDrawList* draw, const ImVec2& from, const ImVec2& to, ImU32 col, float size_px) {
  const ImVec2 d{to.x - from.x, to.y - from.y};
  const float len2 = d.x * d.x + d.y * d.y;
  if (len2 < 1.0f) return;
  const float len = std::sqrt(len2);
  const ImVec2 dir{d.x / len, d.y / len};
  const ImVec2 perp{-dir.y, dir.x};
  const float s = std::max(3.0f, size_px);
  const ImVec2 p1 = to;
  const ImVec2 p2{to.x - dir.x * s + perp.x * (s * 0.5f), to.y - dir.y * s + perp.y * (s * 0.5f)};
  const ImVec2 p3{to.x - dir.x * s - perp.x * (s * 0.5f), to.y - dir.y * s - perp.y * (s * 0.5f)};
  draw->AddTriangleFilled(p1, p2, p3, col);
}

// Convenience overloads.
// Some call sites only care about geometry; default to a visible arrow.
void add_arrowhead(ImDrawList* draw, const ImVec2& from, const ImVec2& to, ImU32 col) {
  add_arrowhead(draw, from, to, col, 10.0f);
}

void add_arrowhead(ImDrawList* draw, const ImVec2& from, const ImVec2& to) {
  add_arrowhead(draw, from, to, IM_COL32(255, 255, 255, 255), 10.0f);
}
// Helper: are we allowed to show the destination system name/links?
bool can_show_system(Id viewer_faction_id, bool fog_of_war, const Simulation& sim, Id system_id) {
  if (!fog_of_war) return true;
  if (viewer_faction_id == kInvalidId) return false;
  return sim.is_system_discovered_by_faction(viewer_faction_id, system_id);
}

float dist2_point_segment(const ImVec2& p, const ImVec2& a, const ImVec2& b, float* t_out = nullptr) {
  const float abx = b.x - a.x;
  const float aby = b.y - a.y;
  const float apx = p.x - a.x;
  const float apy = p.y - a.y;
  const float ab_len2 = abx * abx + aby * aby;
  if (ab_len2 <= 1.0e-6f) {
    if (t_out) *t_out = 0.0f;
    return apx * apx + apy * apy;
  }
  float t = (apx * abx + apy * aby) / ab_len2;
  t = std::clamp(t, 0.0f, 1.0f);
  if (t_out) *t_out = t;
  const float cx = a.x + abx * t;
  const float cy = a.y + aby * t;
  const float dx = p.x - cx;
  const float dy = p.y - cy;
  return dx * dx + dy * dy;
}

ImU32 risk_gradient_color(float t01, float alpha) {
  t01 = std::clamp(t01, 0.0f, 1.0f);
  alpha = std::clamp(alpha, 0.0f, 1.0f);
  // Hue: ~0.33 = green, 0.0 = red.
  const float h = (1.0f - t01) * 0.33f;
  const ImVec4 c = ImColor::HSV(h, 0.80f, 0.95f, alpha);
  return ImGui::ColorConvertFloat4ToU32(c);
}

bool ascii_istarts_with(const std::string& s, const std::string& prefix) {
  if (prefix.empty()) return true;
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    const unsigned char a = static_cast<unsigned char>(s[i]);
    const unsigned char b = static_cast<unsigned char>(prefix[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

bool ascii_icontains(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  const auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char ch1, char ch2) {
        const unsigned char a = static_cast<unsigned char>(ch1);
        const unsigned char b = static_cast<unsigned char>(ch2);
        return std::tolower(a) == std::tolower(b);
      });
  return it != haystack.end();
}

bool ascii_iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const unsigned char ca = static_cast<unsigned char>(a[i]);
    const unsigned char cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return true;
}

std::string ascii_trim(std::string s) {
  auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

std::string normalize_tag(std::string t) {
  t = ascii_trim(std::move(t));
  if (!t.empty() && t.front() == '#') t.erase(t.begin());
  // Normalize to lower-case ASCII (good enough for common tags).
  for (char& ch : t) {
    const unsigned char c = static_cast<unsigned char>(ch);
    ch = static_cast<char>(std::tolower(c));
  }
  // Replace internal whitespace with '_' to make tags single-token.
  for (char& ch : t) {
    if (ch == ' ' || ch == '\t') ch = '_';
  }
  // Hard size limit (UI + save friendliness).
  if (t.size() > 24) t.resize(24);
  // Strip trailing underscores.
  while (!t.empty() && t.back() == '_') t.pop_back();
  return t;
}

bool note_has_tag(const SystemIntelNote& n, const std::string& tag_norm) {
  if (tag_norm.empty()) return false;
  for (const std::string& t : n.tags) {
    if (ascii_iequals(t, tag_norm)) return true;
  }
  return false;
}

// Effective piracy risk for a system used by trade overlays.
//
// If the system is assigned to a region, use Region::pirate_risk dampened by
// Region::pirate_suppression. Otherwise fall back to SimConfig's default risk.
double system_piracy_risk_effective(const Simulation& sim, const GameState& s, Id system_id) {
  const auto* sys = find_ptr(s.systems, system_id);
  if (!sys) return 0.0;

  if (sys->region_id != kInvalidId) {
    if (const auto* reg = find_ptr(s.regions, sys->region_id)) {
      const double base = std::clamp(reg->pirate_risk, 0.0, 1.0);
      const double sup = std::clamp(reg->pirate_suppression, 0.0, 1.0);
      return std::clamp(base * (1.0 - sup), 0.0, 1.0);
    }
  }

  return std::clamp(sim.cfg().pirate_raid_default_system_risk, 0.0, 1.0);
}

struct TradeOverlayCache {
  // The trade network is expensive enough that we cache it between frames.
  //
  // It depends on the evolving simulation state (e.g. colony output), so we
  // also include a time key (day/hour) similar to other simulation caches.
  std::int64_t day{-1};
  int hour{-1};

  std::uint64_t state_generation{0};
  std::uint64_t content_generation{0};

  TradeNetworkOptions opts{};
  bool has_opts{false};
  std::optional<TradeNetwork> net;
};

TradeOverlayCache& trade_overlay_cache() {
  static TradeOverlayCache cache;
  return cache;
}

bool trade_opts_equal(const TradeNetworkOptions& a, const TradeNetworkOptions& b) {
  if (a.max_lanes != b.max_lanes) return false;
  if (a.max_goods_per_lane != b.max_goods_per_lane) return false;
  if (a.include_uncolonized_markets != b.include_uncolonized_markets) return false;
  if (a.include_colony_contributions != b.include_colony_contributions) return false;

  auto eqd = [](double x, double y) {
    return std::abs(x - y) <= 1.0e-9 * std::max(1.0, std::max(std::abs(x), std::abs(y)));
  };
  if (!eqd(a.distance_exponent, b.distance_exponent)) return false;
  if (!eqd(a.colony_tons_per_unit, b.colony_tons_per_unit)) return false;
  return true;
}

ImU32 region_col(Id rid, float alpha) {
  if (rid == kInvalidId) return 0;
  const float h = std::fmod(static_cast<float>((static_cast<std::uint32_t>(rid) * 0.61803398875f)), 1.0f);
  const ImVec4 c = ImColor::HSV(h, 0.55f, 0.95f, alpha);
  return ImGui::ColorConvertFloat4ToU32(c);
}

double cross(const Vec2& o, const Vec2& a, const Vec2& b) {
  return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

// Convex hull (monotonic chain). Returns points in CCW order.
std::vector<Vec2> convex_hull(std::vector<Vec2> pts) {
  if (pts.size() <= 2) return pts;

  // Sort by x then y.
  std::sort(pts.begin(), pts.end(), [](const Vec2& a, const Vec2& b) {
    if (a.x < b.x) return true;
    if (a.x > b.x) return false;
    return a.y < b.y;
  });

  // Deduplicate exact duplicates.
  pts.erase(std::unique(pts.begin(), pts.end(), [](const Vec2& a, const Vec2& b) {
    return a.x == b.x && a.y == b.y;
  }), pts.end());
  if (pts.size() <= 2) return pts;

  std::vector<Vec2> h;
  h.reserve(pts.size() * 2);

  // Lower hull.
  for (const Vec2& p : pts) {
    while (h.size() >= 2 && cross(h[h.size() - 2], h[h.size() - 1], p) <= 0.0) {
      h.pop_back();
    }
    h.push_back(p);
  }

  // Upper hull.
  const std::size_t lower_size = h.size();
  for (std::size_t i = pts.size(); i-- > 0;) {
    const Vec2& p = pts[i];
    while (h.size() > lower_size && cross(h[h.size() - 2], h[h.size() - 1], p) <= 0.0) {
      h.pop_back();
    }
    h.push_back(p);
  }

  if (!h.empty()) h.pop_back();
  return h;
}

// Small deterministic mixing helper for UI-only caches.
inline std::uint64_t mix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

float hash01_from_u64(std::uint64_t x) {
  const std::uint64_t m = mix64(x);
  return static_cast<float>(m & 0xFFFFu) / 65535.0f;
}

// Draw moving "traffic packets" on a jump link.
//
// This is UI-only and deterministic per-link so the motion remains readable
// while still feeling alive.
void draw_jump_flow_packets(ImDrawList* draw,
                            const ImVec2& a,
                            const ImVec2& b,
                            ImU32 col,
                            float base_alpha,
                            std::uint64_t seed,
                            float zoom) {
  if (!draw) return;
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float len2 = dx * dx + dy * dy;
  if (len2 < 18.0f * 18.0f) return;

  const float len = std::sqrt(len2);
  const float inv = 1.0f / std::max(1.0e-6f, len);
  const float ux = dx * inv;
  const float uy = dy * inv;
  const float px = -uy;
  const float py = ux;

  const float phase = hash01_from_u64(seed ^ 0x2E5AA7B3ULL);
  const float speed_cps = 0.05f + 0.18f * hash01_from_u64(seed ^ 0x9C03D451ULL); // cycles/sec
  const float time_s = static_cast<float>(ImGui::GetTime());

  int packets = static_cast<int>(len / 135.0f);
  packets = std::clamp(packets, 1, 4);
  const float zoom_gate = std::clamp((zoom - 0.45f) / 0.85f, 0.0f, 1.0f);
  const float alpha = std::clamp(base_alpha * (0.22f + 0.48f * zoom_gate), 0.0f, 1.0f);
  if (alpha <= 0.001f) return;

  for (int i = 0; i < packets; ++i) {
    const float slot = static_cast<float>(i) / static_cast<float>(packets);
    float t = phase + slot + time_s * speed_cps;
    t -= std::floor(t);

    const float center_boost = 1.0f - std::abs(t * 2.0f - 1.0f);
    const float wobble =
        std::sin(time_s * (3.0f + 0.6f * static_cast<float>(i)) + phase * 6.2831853f + 1.8f * static_cast<float>(i));
    const float lateral = (0.8f + 1.6f * center_boost) * wobble;

    const ImVec2 p{
        a.x + dx * t + px * lateral,
        a.y + dy * t + py * lateral,
    };

    const float r = 1.3f + 1.4f * center_boost;
    draw->AddCircleFilled(p, r + 0.9f, modulate_alpha(IM_COL32(0, 0, 0, 220), alpha * 0.65f), 0);
    draw->AddCircleFilled(p, r, modulate_alpha(col, alpha * (0.45f + 0.55f * center_boost)), 0);
  }
}

// --- Procedural lens field rendering ---
//
// A lightweight raster heatmap drawn behind the galaxy map to visualize
// ProcGenLensMode metrics as a continuous scalar field.
//
// Implementation notes:
//  - Values are interpolated from visible systems using inverse-distance
//    weighting (IDW) over the N nearest sources.
//  - We apply a distance-based fade so the field doesn't smear across the
//    entire map when only a handful of systems are visible (especially under
//    fog-of-war).
struct LensFieldSource {
  Vec2 p;      // relative to world_center
  double value{0.0}; // already transformed (e.g. log-scaled) to match lens_min/max
};

struct IdwSample {
  bool ok{false};
  double value{0.0};
  double min_d2{0.0};
};

// Sample a scalar field at world coordinate w by inverse-distance weighting (IDW)
// using the k-nearest visible sources. Returns the interpolated value (in the
// same transformed space as the input sources) and the squared distance to the
// closest source (for distance-based fading).
inline IdwSample sample_lens_field_idw(const Vec2& w,
                                      const std::vector<LensFieldSource>& sources,
                                      double soft2) {
  if (sources.empty()) return IdwSample{};

  constexpr int kN = 8;
  std::array<double, kN> best_d2;
  std::array<double, kN> best_v;
  best_d2.fill(std::numeric_limits<double>::infinity());
  best_v.fill(0.0);

  double min_d2 = std::numeric_limits<double>::infinity();

  for (const auto& src : sources) {
    const double dx = w.x - src.p.x;
    const double dy = w.y - src.p.y;
    const double d2 = dx * dx + dy * dy;
    if (d2 < min_d2) min_d2 = d2;

    // Replace the current worst entry if this is closer.
    int worst_i = 0;
    double worst_d2 = best_d2[0];
    for (int i = 1; i < kN; ++i) {
      if (best_d2[i] > worst_d2) {
        worst_d2 = best_d2[i];
        worst_i = i;
      }
    }
    if (d2 < worst_d2) {
      best_d2[worst_i] = d2;
      best_v[worst_i] = src.value;
    }
  }

  if (!std::isfinite(min_d2)) return IdwSample{};

  // If we're very close to a source, snap to it to avoid numerical noise.
  double val = 0.0;
  if (min_d2 < 1e-10) {
    int best_i = 0;
    double best_dist = best_d2[0];
    for (int i = 1; i < kN; ++i) {
      if (best_d2[i] < best_dist) {
        best_dist = best_d2[i];
        best_i = i;
      }
    }
    val = best_v[best_i];
  } else {
    double w_sum = 0.0;
    double v_sum = 0.0;
    for (int i = 0; i < kN; ++i) {
      const double d2 = best_d2[i];
      if (!std::isfinite(d2)) continue;
      const double wgt = 1.0 / (d2 + soft2);
      w_sum += wgt;
      v_sum += wgt * best_v[i];
    }
    if (w_sum <= 0.0) return IdwSample{};
    val = v_sum / w_sum;
  }

  IdwSample out;
  out.ok = true;
  out.value = val;
  out.min_d2 = min_d2;
  return out;
}

void draw_procgen_lens_field(ImDrawList* draw,
                            const ImVec2& origin,
                            const ImVec2& avail,
                            const ImVec2& center_px,
                            double scale_px_per_unit,
                            double zoom,
                            const Vec2& pan,
                            const std::vector<LensFieldSource>& sources,
                            double lens_min,
                            double lens_max,
                            float alpha,
                            int cell_px,
                            double typical_spacing_u) {
  if (!draw) return;
  if (sources.empty()) return;

  alpha = std::clamp(alpha, 0.0f, 1.0f);
  cell_px = std::clamp(cell_px, 4, 128);
  if (alpha <= 0.001f) return;

  const int cells_x = std::clamp(static_cast<int>(avail.x / static_cast<float>(cell_px)), 8, 240);
  const int cells_y = std::clamp(static_cast<int>(avail.y / static_cast<float>(cell_px)), 6, 200);
  const float cw = avail.x / static_cast<float>(cells_x);
  const float ch = avail.y / static_cast<float>(cells_y);

  // Fade out where there are no nearby sources.
  const double spacing = std::max(1e-6, typical_spacing_u);
  const double max_dist = spacing * 2.35;
  const double max_dist2 = max_dist * max_dist;
  // Small softening term so IDW doesn't blow up at tiny distances.
  const double soft2 = (spacing * 0.15) * (spacing * 0.15) + 1e-9;

  ImGui::PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
  for (int y = 0; y < cells_y; ++y) {
    const float y0 = origin.y + y * ch;
    const float y1 = origin.y + (y + 1) * ch;
    for (int x = 0; x < cells_x; ++x) {
      const float x0 = origin.x + x * cw;
      const float x1 = origin.x + (x + 1) * cw;

      const ImVec2 sp(x0 + cw * 0.5f, y0 + ch * 0.5f);
      const Vec2 w = to_world(sp, center_px, scale_px_per_unit, zoom, pan);

      const IdwSample samp = sample_lens_field_idw(w, sources, soft2);
      if (!samp.ok || !std::isfinite(samp.min_d2) || samp.min_d2 > max_dist2) {
        continue;
      }

      const double val = samp.value;
      const double min_d2 = samp.min_d2;

      float t = 0.5f;
      if (std::isfinite(val) && lens_max > lens_min + 1e-12) {
        t = static_cast<float>((val - lens_min) / (lens_max - lens_min));
      }

      const float dist = static_cast<float>(std::sqrt(std::max(0.0, min_d2)));
      float fade = 1.0f - std::clamp(dist / static_cast<float>(max_dist), 0.0f, 1.0f);
      // Keep a faint baseline to avoid a harsh edge.
      fade = 0.10f + 0.90f * fade;

      const float a = std::clamp(alpha * fade, 0.0f, 1.0f);
      if (a <= 0.001f) continue;

      const ImU32 col = procgen_lens_gradient_color(t, a);
      draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
    }
  }
  ImGui::PopClipRect();
}

// Draw contour lines (isolines) for the current ProcGen lens field.
//
// Uses a Marching Squares lookup with linear interpolation along each cell edge.
// Ambiguous saddle cases are resolved using a simple center-value decider.
void draw_procgen_lens_contours(ImDrawList* draw,
                               const ImVec2& origin,
                               const ImVec2& avail,
                               const ImVec2& center_px,
                               double scale_px_per_unit,
                               double zoom,
                               const Vec2& pan,
                               const std::vector<LensFieldSource>& sources,
                               double lens_min,
                               double lens_max,
                               float alpha,
                               int cell_px,
                               int levels,
                               float thickness_px,
                               double typical_spacing_u) {
  if (!draw) return;
  if (sources.empty()) return;
  if (!(lens_max > lens_min + 1e-12)) return;

  alpha = std::clamp(alpha, 0.0f, 1.0f);
  if (alpha <= 0.001f) return;

  cell_px = std::clamp(cell_px, 6, 128);
  levels = std::clamp(levels, 2, 16);
  thickness_px = std::clamp(thickness_px, 0.5f, 6.0f);

  // Keep contours a little coarser than the heatmap by default.
  const int cells_x = std::clamp(static_cast<int>(avail.x / static_cast<float>(cell_px)), 10, 220);
  const int cells_y = std::clamp(static_cast<int>(avail.y / static_cast<float>(cell_px)), 8, 180);
  const float cw = avail.x / static_cast<float>(cells_x);
  const float ch = avail.y / static_cast<float>(cells_y);

  // Fade out where there are no nearby sources (matches the heatmap field).
  const double spacing = std::max(1e-6, typical_spacing_u);
  const double max_dist = spacing * 2.35;
  const double max_dist2 = max_dist * max_dist;
  const double soft2 = (spacing * 0.15) * (spacing * 0.15) + 1e-9;

  const int nodes_x = cells_x + 1;
  const int nodes_y = cells_y + 1;

  std::vector<float> node_t(static_cast<std::size_t>(nodes_x) * static_cast<std::size_t>(nodes_y), std::numeric_limits<float>::quiet_NaN());
  std::vector<float> node_fade(static_cast<std::size_t>(nodes_x) * static_cast<std::size_t>(nodes_y), 0.0f);

  auto idx = [&](int x, int y) -> std::size_t {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(nodes_x) + static_cast<std::size_t>(x);
  };

  // Sample the field at grid nodes.
  for (int y = 0; y < nodes_y; ++y) {
    const float sy = origin.y + static_cast<float>(y) * ch;
    for (int x = 0; x < nodes_x; ++x) {
      const float sx = origin.x + static_cast<float>(x) * cw;
      const Vec2 w = to_world(ImVec2(sx, sy), center_px, scale_px_per_unit, zoom, pan);

      const IdwSample samp = sample_lens_field_idw(w, sources, soft2);
      if (!samp.ok || !std::isfinite(samp.min_d2) || samp.min_d2 > max_dist2) {
        continue;
      }

      const double val = samp.value;
      if (!std::isfinite(val)) continue;

      float t = 0.5f;
      if (lens_max > lens_min + 1e-12) {
        t = static_cast<float>((val - lens_min) / (lens_max - lens_min));
      }

      const float dist = static_cast<float>(std::sqrt(std::max(0.0, samp.min_d2)));
      float fade = 1.0f - std::clamp(dist / static_cast<float>(max_dist), 0.0f, 1.0f);
      fade = 0.10f + 0.90f * fade;

      node_t[idx(x, y)] = t;
      node_fade[idx(x, y)] = fade;
    }
  }

  auto lerp_pt = [&](const ImVec2& a, const ImVec2& b, float va, float vb, float thr) -> ImVec2 {
    float t = 0.5f;
    const float denom = (vb - va);
    if (std::abs(denom) > 1e-6f) {
      t = (thr - va) / denom;
    }
    t = std::clamp(t, 0.0f, 1.0f);
    return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
  };

  ImGui::PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);

  for (int li = 1; li <= levels; ++li) {
    const float thr = static_cast<float>(li) / static_cast<float>(levels + 1);

    for (int y = 0; y < cells_y; ++y) {
      const float y0 = origin.y + static_cast<float>(y) * ch;
      const float y1 = origin.y + static_cast<float>(y + 1) * ch;
      for (int x = 0; x < cells_x; ++x) {
        const float x0 = origin.x + static_cast<float>(x) * cw;
        const float x1 = origin.x + static_cast<float>(x + 1) * cw;

        const float t00 = node_t[idx(x, y)];
        const float t10 = node_t[idx(x + 1, y)];
        const float t11 = node_t[idx(x + 1, y + 1)];
        const float t01 = node_t[idx(x, y + 1)];
        if (!std::isfinite(t00) || !std::isfinite(t10) || !std::isfinite(t11) || !std::isfinite(t01)) {
          continue;
        }

        const float f00 = node_fade[idx(x, y)];
        const float f10 = node_fade[idx(x + 1, y)];
        const float f11 = node_fade[idx(x + 1, y + 1)];
        const float f01 = node_fade[idx(x, y + 1)];
        const float fade = std::min(std::min(f00, f10), std::min(f11, f01));

        const float a = std::clamp(alpha * fade, 0.0f, 1.0f);
        if (a <= 0.01f) continue;

        const int b0 = (t00 > thr) ? 1 : 0;
        const int b1 = (t10 > thr) ? 1 : 0;
        const int b2 = (t11 > thr) ? 1 : 0;
        const int b3 = (t01 > thr) ? 1 : 0;
        const int code = b0 | (b1 << 1) | (b2 << 2) | (b3 << 3);
        if (code == 0 || code == 15) continue;

        const ImVec2 p00{x0, y0};
        const ImVec2 p10{x1, y0};
        const ImVec2 p11{x1, y1};
        const ImVec2 p01{x0, y1};

        auto edge_pt = [&](int edge) -> ImVec2 {
          switch (edge) {
            case 0: return lerp_pt(p00, p10, t00, t10, thr); // bottom
            case 1: return lerp_pt(p10, p11, t10, t11, thr); // right
            case 2: return lerp_pt(p11, p01, t11, t01, thr); // top
            case 3: return lerp_pt(p01, p00, t01, t00, thr); // left
            default: return p00;
          }
        };

        auto emit = [&](int e0, int e1) {
          const ImVec2 a0 = edge_pt(e0);
          const ImVec2 a1 = edge_pt(e1);
          const ImU32 col = procgen_lens_gradient_color(thr, a);
          draw->AddLine(a0, a1, col, thickness_px);
        };

        switch (code) {
          case 1:  emit(3, 0); break;
          case 2:  emit(0, 1); break;
          case 3:  emit(3, 1); break;
          case 4:  emit(1, 2); break;
          case 5: {
            const float center = 0.25f * (t00 + t10 + t11 + t01);
            if (center > thr) {
              emit(0, 1);
              emit(2, 3);
            } else {
              emit(3, 0);
              emit(1, 2);
            }
            break;
          }
          case 6:  emit(0, 2); break;
          case 7:  emit(3, 2); break;
          case 8:  emit(2, 3); break;
          case 9:  emit(0, 2); break;
          case 10: {
            const float center = 0.25f * (t00 + t10 + t11 + t01);
            if (center > thr) {
              emit(3, 0);
              emit(1, 2);
            } else {
              emit(0, 1);
              emit(2, 3);
            }
            break;
          }
          case 11: emit(1, 2); break;
          case 12: emit(1, 3); break;
          case 13: emit(0, 1); break;
          case 14: emit(3, 0); break;
          default: break;
        }
      }
    }
  }

  ImGui::PopClipRect();
}

// Draw a gradient vector field for the current ProcGen lens.

// Each arrow indicates the direction of increasing lens value (the local
// gradient) for the interpolated lens field.
void draw_procgen_lens_vectors(ImDrawList* draw,
                              const ImVec2& origin,
                              const ImVec2& avail,
                              const ImVec2& center_px,
                              double scale_px_per_unit,
                              double zoom,
                              const Vec2& pan,
                              const std::vector<LensFieldSource>& sources,
                              double lens_min,
                              double lens_max,
                              float alpha,
                              int cell_px,
                              float arrow_scale_px,
                              float min_mag,
                              double typical_spacing_u) {
  if (!draw) return;
  if (sources.empty()) return;
  if (!(lens_max > lens_min + 1e-12)) return;

  alpha = std::clamp(alpha, 0.0f, 1.0f);
  cell_px = std::clamp(cell_px, 8, 140);
  arrow_scale_px = std::clamp(arrow_scale_px, 1.0f, 600.0f);
  min_mag = std::clamp(min_mag, 0.0f, 1.0f);
  if (alpha <= 0.001f) return;

  const int cells_x = std::clamp(static_cast<int>(avail.x / static_cast<float>(cell_px)), 10, 180);
  const int cells_y = std::clamp(static_cast<int>(avail.y / static_cast<float>(cell_px)), 8, 150);
  const float cw = avail.x / static_cast<float>(cells_x);
  const float ch = avail.y / static_cast<float>(cells_y);

  // Fade out where there are no nearby sources (matches the heatmap field).
  const double spacing = std::max(1e-6, typical_spacing_u);
  const double max_dist = spacing * 2.35;
  const double max_dist2 = max_dist * max_dist;
  const double soft2 = (spacing * 0.15) * (spacing * 0.15) + 1e-9;

  const int nodes_x = cells_x + 1;
  const int nodes_y = cells_y + 1;
  std::vector<float> node_t(static_cast<std::size_t>(nodes_x) * static_cast<std::size_t>(nodes_y),
                            std::numeric_limits<float>::quiet_NaN());
  std::vector<float> node_fade(static_cast<std::size_t>(nodes_x) * static_cast<std::size_t>(nodes_y), 0.0f);

  auto nidx = [&](int x, int y) -> std::size_t {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(nodes_x) + static_cast<std::size_t>(x);
  };

  // Sample the field at grid nodes.
  for (int y = 0; y < nodes_y; ++y) {
    const float sy = origin.y + static_cast<float>(y) * ch;
    for (int x = 0; x < nodes_x; ++x) {
      const float sx = origin.x + static_cast<float>(x) * cw;
      const Vec2 w = to_world(ImVec2(sx, sy), center_px, scale_px_per_unit, zoom, pan);

      const IdwSample samp = sample_lens_field_idw(w, sources, soft2);
      if (!samp.ok || !std::isfinite(samp.min_d2) || samp.min_d2 > max_dist2) {
        continue;
      }

      const double val = samp.value;
      if (!std::isfinite(val)) continue;

      float t = static_cast<float>((val - lens_min) / (lens_max - lens_min));

      const float dist = static_cast<float>(std::sqrt(std::max(0.0, samp.min_d2)));
      float fade = 1.0f - std::clamp(dist / static_cast<float>(max_dist), 0.0f, 1.0f);
      fade = 0.10f + 0.90f * fade;

      node_t[nidx(x, y)] = t;
      node_fade[nidx(x, y)] = fade;
    }
  }

  // Decimate if the map is very large to avoid overdraw.
  int stride = 1;
  const int total_cells = cells_x * cells_y;
  if (total_cells > 18000) stride = 2;
  if (total_cells > 42000) stride = 3;

  const float thickness = 1.35f;

  ImGui::PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
  for (int y = 0; y < cells_y; y += stride) {
    const float y0 = origin.y + static_cast<float>(y) * ch;
    for (int x = 0; x < cells_x; x += stride) {
      const float x0 = origin.x + static_cast<float>(x) * cw;

      const float t00 = node_t[nidx(x, y)];
      const float t10 = node_t[nidx(x + 1, y)];
      const float t11 = node_t[nidx(x + 1, y + 1)];
      const float t01 = node_t[nidx(x, y + 1)];
      if (!std::isfinite(t00) || !std::isfinite(t10) || !std::isfinite(t11) || !std::isfinite(t01)) {
        continue;
      }

      const float f00 = node_fade[nidx(x, y)];
      const float f10 = node_fade[nidx(x + 1, y)];
      const float f11 = node_fade[nidx(x + 1, y + 1)];
      const float f01 = node_fade[nidx(x, y + 1)];
      const float fade = std::min(std::min(f00, f10), std::min(f11, f01));

      const float a = std::clamp(alpha * fade, 0.0f, 1.0f);
      if (a <= 0.01f) continue;

      // Gradient estimate at the cell center using mid-edge differences.
      const float dx = 0.5f * ((t10 + t11) - (t00 + t01));
      const float dy = 0.5f * ((t01 + t11) - (t00 + t10));
      const float mag = std::sqrt(dx * dx + dy * dy);
      if (mag < min_mag) continue;

      // Convert to a screen-space direction (account for aspect).
      const float vx0 = dx / std::max(1e-6f, cw);
      const float vy0 = dy / std::max(1e-6f, ch);
      const float vlen = std::sqrt(vx0 * vx0 + vy0 * vy0);
      if (vlen <= 1e-6f) continue;
      const float vx = vx0 / vlen;
      const float vy = vy0 / vlen;

      float len = 6.0f + arrow_scale_px * mag;
      len = std::clamp(len, 6.0f, 34.0f);

      const ImVec2 c(x0 + cw * 0.5f, y0 + ch * 0.5f);
      const ImVec2 p0(c.x - vx * len * 0.45f, c.y - vy * len * 0.45f);
      const ImVec2 p1(c.x + vx * len * 0.55f, c.y + vy * len * 0.55f);

      const float tavg = 0.25f * (t00 + t10 + t11 + t01);
      const ImU32 col = procgen_lens_gradient_color(tavg, a);
      draw->AddLine(p0, p1, col, thickness);
      add_arrowhead(draw, p0, p1, col, 6.0f);
    }
  }
  ImGui::PopClipRect();
}

} // namespace

void draw_galaxy_map(Simulation& sim,
                     UIState& ui,
                     Id& selected_ship,
                     double& zoom,
                     Vec2& pan,
                     ProcRenderEngine* proc_engine,
                     ProcParticleFieldEngine* particle_engine,
                     ProcTerritoryFieldEngine* territory_engine) {
  auto& s = sim.state();

  const ImGuiIO& io = ImGui::GetIO();

  const Ship* viewer_ship = (selected_ship != kInvalidId) ? find_ptr(s.ships, selected_ship) : nullptr;
  const Id viewer_faction_id = viewer_ship ? viewer_ship->faction_id : ui.viewer_faction_id;
  Faction* viewer_faction = (viewer_faction_id != kInvalidId) ? find_ptr(s.factions, viewer_faction_id) : nullptr;

  // Precompute recent contact counts per-system for lightweight "intel alert" rings.
  std::unordered_map<Id, int> recent_contact_count;
  if (ui.show_galaxy_intel_alerts && viewer_faction) {
    const std::int64_t today = s.date.days_since_epoch();
    for (const auto& kv : viewer_faction->ship_contacts) {
      const auto& c = kv.second;
      const std::int64_t age = today - static_cast<std::int64_t>(c.last_seen_day);
      if (age < 0) continue;
      if (age > ui.contact_max_age_days) continue;
      ++recent_contact_count[c.system_id];
    }
  }


  // Selected fleet (for routing/highlighting).
  const Fleet* selected_fleet = (ui.selected_fleet_id != kInvalidId) ? find_ptr(s.fleets, ui.selected_fleet_id) : nullptr;
  Id selected_fleet_system = kInvalidId;
  if (selected_fleet && selected_fleet->leader_ship_id != kInvalidId) {
    const auto* leader = find_ptr(s.ships, selected_fleet->leader_ship_id);
    if (leader) selected_fleet_system = leader->system_id;
  }

  if (ui.fog_of_war && viewer_faction_id == kInvalidId) {
    ImGui::TextDisabled("Fog of war requires a viewer faction.");
    ImGui::TextDisabled("Select a faction in the Research tab, or select a ship.");
    return;
  }

  // Visible systems (respect discovery under FoW).
  struct SysView {
    Id id{kInvalidId};
    const StarSystem* sys{nullptr};
  };

  std::vector<SysView> visible;
  visible.reserve(s.systems.size());

  // Procedural trade overlay state (computed lazily below when enabled).
  const TradeNetwork* trade_net = nullptr;

  // Lanes considered for analysis/tooltips this frame (after fog-of-war + filters).
  //
  // Note: this list is populated even when the lane rendering toggle is off
  // (e.g. the player wants the security analysis panel but not the visuals).
  std::vector<const TradeLane*> trade_lanes_visible;
  trade_lanes_visible.reserve(256);

  struct TradeLaneHoverInfo {
    const TradeLane* lane{nullptr};
    float d2{1e30f};
    ImVec2 a{0.0f, 0.0f};
    ImVec2 b{0.0f, 0.0f};
    float thickness{1.0f};
    double risk_avg{0.0};
  } trade_lane_hover;

  for (const auto& [id, sys] : s.systems) {
    if (ui.fog_of_war) {
      if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, id)) continue;
    }
    visible.push_back(SysView{id, &sys});
  }

  if (visible.empty()) {
    ImGui::TextDisabled("No systems to display");
    return;
  }

  // Fast lookup for the visible system set (respects fog-of-war).
  std::unordered_map<Id, const StarSystem*> visible_sys_by_id;
  visible_sys_by_id.reserve(visible.size() * 2);
  for (const auto& v : visible) {
    if (v.sys) visible_sys_by_id.emplace(v.id, v.sys);
  }

  // --- Procedural generation lens normalization (for node coloring). ---
  //
  // The lens uses the same visible-system set as the map (respecting FoW) so
  // it doesn't leak information.
  const ProcGenLensMode lens_mode = ui.galaxy_procgen_lens_mode;
  const bool lens_active = (lens_mode != ProcGenLensMode::Off);
  bool lens_bounds_valid = false;
  double lens_raw_min = 0.0;
  double lens_raw_max = 0.0;
  double lens_min = 0.0; // transformed
  double lens_max = 0.0; // transformed
  if (lens_active) {
    for (const auto& v : visible) {
      const double raw = procgen_lens_value(s, *v.sys, lens_mode);
      if (!std::isfinite(raw)) continue;
      double x = raw;
      if (ui.galaxy_procgen_lens_log_scale && (lens_mode == ProcGenLensMode::MineralWealth)) {
        // Wide-range values benefit from log scaling.
        x = std::log10(std::max(0.0, raw) + 1.0);
      }
      if (!std::isfinite(x)) continue;
      if (!lens_bounds_valid) {
        lens_bounds_valid = true;
        lens_raw_min = lens_raw_max = raw;
        lens_min = lens_max = x;
      } else {
        lens_raw_min = std::min(lens_raw_min, raw);
        lens_raw_max = std::max(lens_raw_max, raw);
        lens_min = std::min(lens_min, x);
        lens_max = std::max(lens_max, x);
      }
    }
  }

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 center_px = ImVec2(origin.x + avail.x * 0.5f, origin.y + avail.y * 0.5f);

  // Compute bounds (in galaxy units).
  double min_x = visible.front().sys->galaxy_pos.x;
  double max_x = visible.front().sys->galaxy_pos.x;
  double min_y = visible.front().sys->galaxy_pos.y;
  double max_y = visible.front().sys->galaxy_pos.y;

  for (const auto& v : visible) {
    min_x = std::min(min_x, v.sys->galaxy_pos.x);
    max_x = std::max(max_x, v.sys->galaxy_pos.x);
    min_y = std::min(min_y, v.sys->galaxy_pos.y);
    max_y = std::max(max_y, v.sys->galaxy_pos.y);
  }

  const Vec2 world_center = Vec2{(min_x + max_x) * 0.5, (min_y + max_y) * 0.5};
  const double span_x = std::max(1e-6, max_x - min_x);
  const double span_y = std::max(1e-6, max_y - min_y);
  const double max_half_span = std::max(span_x, span_y) * 0.5;

  // Fit the farthest system into the available area.
  const double fit = std::min(avail.x, avail.y) * 0.45;
  const double scale = std::max(1e-9, fit / std::max(1.0, max_half_span));

  // ProcGen lens sources for continuous overlays (field, contours, probe).
  std::vector<LensFieldSource> lens_sources;
  double lens_typical_spacing = 1.0;
  const bool want_lens_sources = lens_active && lens_bounds_valid &&
                               (ui.galaxy_procgen_field || ui.galaxy_procgen_contours || ui.galaxy_procgen_vectors || ui.galaxy_procgen_probe);
  if (want_lens_sources) {
    lens_sources.reserve(visible.size());
    for (const auto& v : visible) {
      const double raw = procgen_lens_value(s, *v.sys, lens_mode);
      if (!std::isfinite(raw)) continue;
      double x = raw;
      if (ui.galaxy_procgen_lens_log_scale && (lens_mode == ProcGenLensMode::MineralWealth)) {
        x = std::log10(std::max(0.0, raw) + 1.0);
      }
      if (!std::isfinite(x)) continue;
      lens_sources.push_back(LensFieldSource{v.sys->galaxy_pos - world_center, x});
    }

    // Approximate typical spacing to avoid smearing overlays across the entire map when
    // the visible set is sparse.
    const double area = std::max(1e-9, span_x * span_y);
    lens_typical_spacing = std::sqrt(area / std::max(1.0, static_cast<double>(visible.size())));
  }

  // One-frame request (from other windows) to center/fit the galaxy map.
  if (ui.request_galaxy_map_center) {
    const Vec2 abs{ui.request_galaxy_map_center_x, ui.request_galaxy_map_center_y};
    const Vec2 rel = abs - world_center;
    pan = Vec2{-rel.x, -rel.y};

    if (ui.request_galaxy_map_center_zoom > 0.0) {
      zoom = std::clamp(ui.request_galaxy_map_center_zoom, 0.2, 50.0);
    } else if (ui.request_galaxy_map_fit_half_span > 1e-9) {
      const double target_half = std::max(1e-9, ui.request_galaxy_map_fit_half_span);
      // zoom=1 fits max_half_span; scale accordingly to fit a smaller half-span.
      const double z = (max_half_span / target_half) * 0.85; // a little padding
      zoom = std::clamp(z, 0.2, 50.0);
    }

    ui.request_galaxy_map_center = false;
    ui.request_galaxy_map_center_zoom = 0.0;
    ui.request_galaxy_map_fit_half_span = 0.0;
  }

  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool mouse_in_rect =
      (mouse.x >= origin.x && mouse.x <= origin.x + avail.x && mouse.y >= origin.y && mouse.y <= origin.y + avail.y);

  // Map viewport rectangle (content area).
  const ImVec2 view_p0 = origin;
  const ImVec2 view_p1(origin.x + avail.x, origin.y + avail.y);

  // Legend overlay rectangle (top-left). The legend is drawn later in the frame using
  // SetCursorScreenPos/BeginChild, so we must treat this area as UI now or
  // clicks/scrolls will 'fall through' to the map interactions.
  const float legend_margin = 10.0f;
  const ImVec2 legend_desired(350.0f, 320.0f);
  const float legend_max_w = std::max(0.0f, avail.x - legend_margin * 2.0f);
  const float legend_max_h = std::max(0.0f, avail.y - legend_margin * 2.0f);
  ImVec2 legend_size(std::min(legend_desired.x, legend_max_w), std::min(legend_desired.y, legend_max_h));
  // Avoid (0,0) which in BeginChild means "use remaining region".
  legend_size.x = std::max(1.0f, legend_size.x);
  legend_size.y = std::max(1.0f, legend_size.y);
  const ImVec2 legend_p0(origin.x + legend_margin, origin.y + legend_margin);
  const ImVec2 legend_p1(legend_p0.x + legend_size.x, legend_p0.y + legend_size.y);

  // Clamp hit-test to visible viewport. This avoids blocking map interactions
  // in cases where the window is smaller than the overlay and the child window
  // is clipped by ImGui.
  const ImVec2 legend_hit_p0(std::max(legend_p0.x, view_p0.x), std::max(legend_p0.y, view_p0.y));
  const ImVec2 legend_hit_p1(std::min(legend_p1.x, view_p1.x), std::min(legend_p1.y, view_p1.y));
  const bool legend_hit_valid = legend_hit_p1.x > legend_hit_p0.x && legend_hit_p1.y > legend_hit_p0.y;
  const bool over_legend = legend_hit_valid && mouse_in_rect && point_in_rect(mouse, legend_hit_p0, legend_hit_p1);

  // Minimap rectangle (bottom-right overlay).
  const float mm_margin = 10.0f;
  float mm_w = std::clamp(avail.x * 0.28f, 150.0f, 300.0f);
  float mm_h = std::clamp(avail.y * 0.23f, 105.0f, 240.0f);
  const float mm_max_w = std::max(0.0f, avail.x - mm_margin * 2.0f);
  const float mm_max_h = std::max(0.0f, avail.y - mm_margin * 2.0f);
  mm_w = std::min(mm_w, mm_max_w);
  mm_h = std::min(mm_h, mm_max_h);
  const bool minimap_has_room = (mm_w >= 10.0f && mm_h >= 10.0f);
  const ImVec2 mm_p1(view_p1.x - mm_margin, view_p1.y - mm_margin);
  const ImVec2 mm_p0(mm_p1.x - mm_w, mm_p1.y - mm_h);
  bool minimap_enabled = ui.galaxy_map_show_minimap && minimap_has_room;

  // Keyboard shortcuts.
  if (hovered && !ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
      zoom = 1.0;
      pan = Vec2{0.0, 0.0};
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M)) {
      ui.galaxy_map_show_minimap = !ui.galaxy_map_show_minimap;
      minimap_enabled = ui.galaxy_map_show_minimap && minimap_has_room;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
      ui.galaxy_map_fuel_range = !ui.galaxy_map_fuel_range;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_B)) {
      ui.galaxy_map_particle_field = !ui.galaxy_map_particle_field;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
      ui.galaxy_map_territory_overlay = !ui.galaxy_map_territory_overlay;
    }
  }

  const ImVec2 mm_hit_p0(std::max(mm_p0.x, view_p0.x), std::max(mm_p0.y, view_p0.y));
  const ImVec2 mm_hit_p1(std::min(mm_p1.x, view_p1.x), std::min(mm_p1.y, view_p1.y));
  const bool mm_hit_valid = minimap_enabled && mm_hit_p1.x > mm_hit_p0.x && mm_hit_p1.y > mm_hit_p0.y;
  const bool over_minimap = mm_hit_valid && mouse_in_rect && point_in_rect(mouse, mm_hit_p0, mm_hit_p1);

  MinimapTransform mm;
  if (minimap_enabled) {
    const Vec2 rel_min{min_x - world_center.x, min_y - world_center.y};
    const Vec2 rel_max{max_x - world_center.x, max_y - world_center.y};
    mm = make_minimap_transform(mm_p0, mm_p1, rel_min, rel_max);
  }

  if (hovered && mouse_in_rect && !over_minimap && !over_legend) {
    // Zoom to cursor.
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      const Vec2 before = to_world(mouse, center_px, scale, zoom, pan);
      double new_zoom = zoom * std::pow(1.1, wheel);
      new_zoom = std::clamp(new_zoom, 0.2, 50.0);
      const Vec2 after = to_world(mouse, center_px, scale, new_zoom, pan);
      pan.x += (after.x - before.x);
      pan.y += (after.y - before.y);
      zoom = new_zoom;
    }

    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      const double denom = std::max(1e-12, scale * zoom);
      pan.x += d.x / denom;
      pan.y += d.y / denom;
    }
  }

  // Minimap pan/teleport: click+drag to set the view center.
  if (hovered && mouse_in_rect && over_minimap && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const Vec2 target = minimap_px_to_world(mm, mouse);
    pan = Vec2{-target.x, -target.y};
  }

  // --- Route ruler (hold D) ---
  // A lightweight planning helper to measure a jump-route between two systems.
  // State is cached UI-only state and is cleared on new game / state reload.
  struct RouteRulerState {
    Id a{kInvalidId};
    Id b{kInvalidId};
  };
  static RouteRulerState route_ruler;
  static std::uint64_t route_ruler_state_gen = 0;
  if (route_ruler_state_gen != sim.state_generation()) {
    route_ruler = RouteRulerState{};
    route_ruler_state_gen = sim.state_generation();
  }

  bool ruler_consumed_left = false;
  bool ruler_consumed_right = false;

  bool trade_consumed_left = false;
  bool trade_consumed_right = false;

  auto* draw = ImGui::GetWindowDrawList();
  const ImU32 bg = ImGui::ColorConvertFloat4ToU32(
      ImVec4(ui.galaxy_map_bg[0], ui.galaxy_map_bg[1], ui.galaxy_map_bg[2], ui.galaxy_map_bg[3]));
  draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), bg);
  draw->AddRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(60, 60, 60, 255));

  // Map chrome.
  {
    const float pan_px_x = static_cast<float>(-pan.x * scale * zoom);
    const float pan_px_y = static_cast<float>(-pan.y * scale * zoom);
    const std::uint32_t chrome_seed = static_cast<std::uint32_t>(viewer_faction_id == kInvalidId ? 0xC0FFEEu : viewer_faction_id);

    // Experimental: ray-marched SDF nebula background.
    if (ui.map_raymarch_nebula && ui.map_raymarch_nebula_alpha > 0.0f) {
      RaymarchNebulaStyle rs;
      rs.enabled = true;
      rs.alpha = ui.map_raymarch_nebula_alpha;
      rs.parallax = ui.map_raymarch_nebula_parallax;
      rs.max_depth = ui.map_raymarch_nebula_max_depth;
      rs.error_threshold = ui.map_raymarch_nebula_error_threshold;
      rs.spp = ui.map_raymarch_nebula_spp;
      rs.max_steps = ui.map_raymarch_nebula_max_steps;
      rs.animate = ui.map_raymarch_nebula_animate;
      rs.time_scale = ui.map_raymarch_nebula_time_scale;
      rs.debug_overlay = ui.map_raymarch_nebula_debug;

      RaymarchNebulaStats stats;
      draw_raymarched_nebula(draw, origin, avail, bg, pan_px_x, pan_px_y, chrome_seed ^ 0x8F00DBA5u, rs,
                             rs.debug_overlay ? &stats : nullptr);
    }

    // Starfield / procedural background.
    //
    // The legacy path draws stars directly into the ImDrawList every frame.
    // The procedural engine path rasterizes tiles on-demand, uploads textures,
    // then just draws textured quads while panning.
    if (ui.map_proc_render_engine && proc_engine && proc_engine->ready()) {
      if (ui.map_proc_render_clear_cache_requested) {
        proc_engine->clear();
        ui.map_proc_render_clear_cache_requested = false;
      }

      ProcRenderConfig pcfg;
      pcfg.tile_px = ui.map_proc_render_tile_px;
      pcfg.max_cached_tiles = ui.map_proc_render_cache_tiles;
      pcfg.star_density = ui.galaxy_map_starfield ? ui.map_starfield_density : 0.0f;
      pcfg.parallax = ui.map_starfield_parallax;
      pcfg.nebula_enable = ui.map_proc_render_nebula_enable;
      pcfg.nebula_strength = ui.map_proc_render_nebula_strength;
      pcfg.nebula_scale = ui.map_proc_render_nebula_scale;
      pcfg.nebula_warp = ui.map_proc_render_nebula_warp;
      pcfg.debug_show_tile_bounds = ui.map_proc_render_debug_tiles;

      // Subtle tinting so the procedural textures respect user theme colors.
      const ImVec4 bg4 = ImGui::ColorConvertU32ToFloat4(bg);
      const ImVec4 tint4(0.75f + 0.25f * bg4.x, 0.75f + 0.25f * bg4.y, 0.75f + 0.25f * bg4.z, 1.0f);
      const ImU32 tint_u32 = ImGui::ColorConvertFloat4ToU32(tint4);

      proc_engine->draw_background(draw, origin, avail, tint_u32, pan_px_x, pan_px_y, chrome_seed, pcfg);
    } else {
      StarfieldStyle sf;
      sf.enabled = ui.galaxy_map_starfield;
      sf.density = ui.map_starfield_density;
      sf.parallax = ui.map_starfield_parallax;
      sf.alpha = 1.0f;
      draw_starfield(draw, origin, avail, bg, pan_px_x, pan_px_y, chrome_seed, sf);
    }

    // Procedural particle field (dust): deterministic screen-space points with parallax.
    if (ui.galaxy_map_particle_field && particle_engine && ui.map_particle_opacity > 0.0f) {
      ProcParticleFieldConfig pcfg;
      pcfg.enabled = true;
      pcfg.tile_px = ui.map_particle_tile_px;
      pcfg.particles_per_tile = ui.map_particle_particles_per_tile;
      pcfg.layers = ui.map_particle_layers;
      pcfg.layer0_parallax = ui.map_particle_layer0_parallax;
      pcfg.layer1_parallax = ui.map_particle_layer1_parallax;
      pcfg.layer2_parallax = ui.map_particle_layer2_parallax;
      pcfg.opacity = ui.map_particle_opacity;
      pcfg.base_radius_px = ui.map_particle_base_radius_px;
      pcfg.radius_jitter_px = ui.map_particle_radius_jitter_px;
      pcfg.twinkle_strength = ui.map_particle_twinkle_strength;
      pcfg.twinkle_speed = ui.map_particle_twinkle_speed;
      pcfg.animate_drift = ui.map_particle_drift;
      pcfg.drift_px_per_day = ui.map_particle_drift_px_per_day;
      pcfg.sparkles = ui.map_particle_sparkles;
      pcfg.sparkle_chance = ui.map_particle_sparkle_chance;
      pcfg.sparkle_length_px = ui.map_particle_sparkle_length_px;
      pcfg.debug_tile_bounds = ui.map_particle_debug_tiles;

      const ImVec4 bg4 = ImGui::ColorConvertU32ToFloat4(bg);
      const ImVec4 tint4(0.85f + 0.15f * bg4.x, 0.85f + 0.15f * bg4.y, 0.90f + 0.10f * bg4.z, 1.0f);
      const ImU32 tint_u32 = ImGui::ColorConvertFloat4ToU32(tint4);

      particle_engine->draw_particles(draw, origin, avail, tint_u32, pan_px_x, pan_px_y,
                                    chrome_seed ^ 0x51A7EEDu, pcfg);

      const auto st = particle_engine->stats();
      ui.map_particle_last_frame_layers_drawn = st.layers_drawn;
      ui.map_particle_last_frame_tiles_drawn = st.tiles_drawn;
      ui.map_particle_last_frame_particles_drawn = st.particles_drawn;
    }

    // Procedural lens *field* overlay (optional).
    // Draw behind the grid/region boundaries so the map stays readable.
    if (ui.galaxy_procgen_field && lens_active && lens_bounds_valid && !lens_sources.empty()) {
      draw_procgen_lens_field(draw, origin, avail, center_px, scale, zoom, pan,
                              lens_sources, lens_min, lens_max,
                              ui.galaxy_procgen_field_alpha,
                              ui.galaxy_procgen_field_cell_px,
                              lens_typical_spacing);
    }



    // Procedural territory overlay (political map).
    // Draw above the procgen lens (if any) but behind the grid/regions/nodes.
    if (ui.galaxy_map_territory_overlay && territory_engine) {
      std::vector<ProcTerritorySource> terr;
      terr.reserve(s.colonies.size());

      for (const auto& kv : s.colonies) {
        const Colony& c = kv.second;
        if (c.faction_id == kInvalidId) continue;
        const double population_millions = std::max(0.0, c.population_millions);
        if (!std::isfinite(population_millions) || population_millions <= 0.0) continue;
        const Body* b = find_ptr(s.bodies, c.body_id);
        if (!b) continue;
        auto it = visible_sys_by_id.find(b->system_id);
        if (it == visible_sys_by_id.end()) continue;
        const StarSystem* sys = it->second;

        ProcTerritorySource src;
        src.pos = sys->galaxy_pos - world_center;
        src.faction_id = c.faction_id;
        src.population_millions =
            static_cast<float>(std::min(population_millions, static_cast<double>(std::numeric_limits<float>::max())));
        terr.push_back(src);
      }

      if (!terr.empty()) {
        ProcTerritoryFieldConfig tcfg;
        tcfg.enabled = true;
        tcfg.tile_px = ui.galaxy_map_territory_tile_px;
        tcfg.max_cached_tiles = ui.galaxy_map_territory_cache_tiles;
        tcfg.samples_per_tile = ui.galaxy_map_territory_samples_per_tile;
        tcfg.draw_fill = ui.galaxy_map_territory_fill;
        tcfg.draw_boundaries = ui.galaxy_map_territory_boundaries;
        tcfg.fill_opacity = ui.galaxy_map_territory_fill_opacity;
        tcfg.boundary_opacity = ui.galaxy_map_territory_boundary_opacity;
        tcfg.boundary_thickness_px = ui.galaxy_map_territory_boundary_thickness_px;
        tcfg.influence_base_spacing_mult = ui.galaxy_map_territory_influence_base_spacing_mult;
        tcfg.influence_pop_spacing_mult = ui.galaxy_map_territory_influence_pop_spacing_mult;
        tcfg.influence_pop_log_bias = ui.galaxy_map_territory_influence_pop_log_bias;
        tcfg.presence_falloff_spacing = ui.galaxy_map_territory_presence_falloff_spacing;
        tcfg.dominance_softness_spacing = ui.galaxy_map_territory_dominance_softness_spacing;
        tcfg.contested_dither = ui.galaxy_map_territory_contested_dither;
        tcfg.contested_threshold = ui.galaxy_map_territory_contested_threshold;
        tcfg.contested_dither_strength = ui.galaxy_map_territory_contested_dither_strength;
        tcfg.debug_tile_bounds = ui.galaxy_map_territory_debug_tiles;

        const double spacing = (lens_typical_spacing > 0.0) ? lens_typical_spacing : 1.0;
        territory_engine->draw_territories(draw, origin, avail, center_px, scale, zoom, pan,
                                          terr, spacing, chrome_seed ^ 0xA31C9B7u, tcfg);
      }
    }
    GridStyle gs;
    gs.enabled = ui.galaxy_map_grid;
    gs.desired_minor_px = 95.0f;
    gs.major_every = 5;
    gs.minor_alpha = 0.10f * ui.map_grid_opacity;
    gs.major_alpha = 0.18f * ui.map_grid_opacity;
    gs.axis_alpha = 0.25f * ui.map_grid_opacity;
    gs.label_alpha = 0.70f * ui.map_grid_opacity;
    draw_grid(draw, origin, avail, center_px, scale, zoom, pan, IM_COL32(220, 220, 220, 255), gs, "u");

    // ProcGen contour lines (optional).
    // Draw above the grid but behind region boundaries / links / nodes.
    if (ui.galaxy_procgen_contours && lens_active && lens_bounds_valid) {
      draw_procgen_lens_contours(draw, origin, avail, center_px, scale, zoom, pan,
                                lens_sources, lens_min, lens_max,
                                ui.galaxy_procgen_contour_alpha,
                                ui.galaxy_procgen_contour_cell_px,
                                ui.galaxy_procgen_contour_levels,
                                ui.galaxy_procgen_contour_thickness,
                                lens_typical_spacing);
    }

    // ProcGen gradient vectors (optional).
    // Draw above contours but behind region boundaries / links / nodes.
    if (ui.galaxy_procgen_vectors && lens_active && lens_bounds_valid && !lens_sources.empty()) {
      draw_procgen_lens_vectors(draw, origin, avail, center_px, scale, zoom, pan,
                               lens_sources, lens_min, lens_max,
                               ui.galaxy_procgen_vector_alpha,
                               ui.galaxy_procgen_vector_cell_px,
                               ui.galaxy_procgen_vector_scale,
                               ui.galaxy_procgen_vector_min_mag,
                               lens_typical_spacing);
    }


    // Region boundaries (procedural sector overlays).
    // Draw behind jump links / nodes but above the grid so the map stays readable.
    const bool want_region_overlay = (ui.show_galaxy_region_boundaries || ui.show_galaxy_region_centers);
    std::unordered_map<Id, std::vector<Vec2>> reg_pts;
    if (want_region_overlay) {
      reg_pts.reserve(s.regions.size() * 2);
      for (const auto& v : visible) {
        const Id rid = v.sys ? v.sys->region_id : kInvalidId;
        if (rid == kInvalidId) continue;
        reg_pts[rid].push_back(v.sys->galaxy_pos - world_center);
      }
    }

    // Region boundaries: convex hull (fast) or clipped Voronoi partition (accurate).
    if (ui.show_galaxy_region_boundaries && !reg_pts.empty()) {
      if (ui.galaxy_region_boundary_voronoi) {
        // Build clipped Voronoi cells based on Region::center (seed points).
        //
        // Under fog-of-war, we restrict the *site set* to regions that have at least
        // one visible system to avoid leaking undiscovered regions. This still
        // provides intuitive partitions for the explored space.
        struct VoronoiCache {
          std::uint64_t key{0};
          std::unordered_map<Id, std::vector<Vec2>> cells; // region_id -> convex polygon (relative coords)
        };
        static VoronoiCache vcache;
        static std::uint64_t vcache_state_gen = 0;
        if (vcache_state_gen != sim.state_generation()) {
          vcache = VoronoiCache{};
          vcache_state_gen = sim.state_generation();
        }

        auto dot2 = [](const Vec2& a, const Vec2& b) -> double { return a.x * b.x + a.y * b.y; };

        auto clip_halfplane = [&](const std::vector<Vec2>& poly, const Vec2& n, double c) -> std::vector<Vec2> {
          std::vector<Vec2> out;
          if (poly.empty()) return out;
          out.reserve(poly.size() + 4);
          constexpr double eps = 1.0e-9;

          auto f = [&](const Vec2& p) -> double { return dot2(p, n) - c; };

          Vec2 prev = poly.back();
          double f_prev = f(prev);
          bool prev_in = (f_prev <= eps);

          for (const Vec2& curr : poly) {
            const double f_curr = f(curr);
            const bool curr_in = (f_curr <= eps);

            if (prev_in && curr_in) {
              out.push_back(curr);
            } else if (prev_in && !curr_in) {
              // Leaving: emit intersection.
              const double denom = (f_prev - f_curr);
              double t = 0.0;
              if (std::abs(denom) > 1.0e-12) t = f_prev / denom;
              t = std::clamp(t, 0.0, 1.0);
              out.push_back(prev + (curr - prev) * t);
            } else if (!prev_in && curr_in) {
              // Entering: emit intersection then curr.
              const double denom = (f_prev - f_curr);
              double t = 0.0;
              if (std::abs(denom) > 1.0e-12) t = f_prev / denom;
              t = std::clamp(t, 0.0, 1.0);
              out.push_back(prev + (curr - prev) * t);
              out.push_back(curr);
            }

            prev = curr;
            f_prev = f_curr;
            prev_in = curr_in;
          }

          // Deduplicate consecutive points (and collapse closing duplicate).
          if (out.size() >= 2) {
            auto is_near_pt = [](const Vec2& a, const Vec2& b) -> bool {
              const double dx = a.x - b.x;
              const double dy = a.y - b.y;
              return (dx * dx + dy * dy) <= 1.0e-16;
            };
            std::vector<Vec2> dedup;
            dedup.reserve(out.size());
            for (const Vec2& p : out) {
              if (dedup.empty() || !is_near_pt(dedup.back(), p)) dedup.push_back(p);
            }
            if (dedup.size() >= 2 && is_near_pt(dedup.front(), dedup.back())) dedup.pop_back();
            out.swap(dedup);
          }

          return out;
        };

        auto simplify_convex = [&](std::vector<Vec2>& poly) {
          if (poly.size() < 3) return;
          // Remove near-collinear vertices (small visual cleanup).
          constexpr double eps = 1.0e-10;
          bool changed = true;
          for (int iter = 0; changed && iter < 3; ++iter) {
            changed = false;
            if (poly.size() < 3) break;
            std::vector<Vec2> out;
            out.reserve(poly.size());
            for (std::size_t i = 0; i < poly.size(); ++i) {
              const Vec2& a = poly[(i + poly.size() - 1) % poly.size()];
              const Vec2& b = poly[i];
              const Vec2& cpt = poly[(i + 1) % poly.size()];
              const Vec2 ab = b - a;
              const Vec2 bc = cpt - b;
              const double cr = ab.x * bc.y - ab.y * bc.x;
              const double lab = std::max(1.0e-12, ab.length());
              const double lbc = std::max(1.0e-12, bc.length());
              if (std::abs(cr) <= eps * (lab + lbc)) {
                changed = true;
                continue;
              }
              out.push_back(b);
            }
            poly.swap(out);
          }
        };

        auto build_voronoi = [&]() {
          vcache.cells.clear();
          if (reg_pts.empty()) return;

          // Determine a clipping rectangle slightly larger than the visible bounds.
          const Vec2 rel_min{min_x - world_center.x, min_y - world_center.y};
          const Vec2 rel_max{max_x - world_center.x, max_y - world_center.y};
          const double margin = std::max(1.0, max_half_span * 0.18);
          const Vec2 clip_min{rel_min.x - margin, rel_min.y - margin};
          const Vec2 clip_max{rel_max.x + margin, rel_max.y + margin};

          const std::vector<Vec2> bbox{
            Vec2{clip_min.x, clip_min.y},
            Vec2{clip_max.x, clip_min.y},
            Vec2{clip_max.x, clip_max.y},
            Vec2{clip_min.x, clip_max.y},
          };

          struct Site {
            Id rid{kInvalidId};
            Vec2 p{0.0, 0.0}; // relative coords
          };
          std::vector<Site> sites;
          sites.reserve(reg_pts.size());

          for (const auto& kv : reg_pts) {
            const Id rid = kv.first;
            Vec2 p{0.0, 0.0};

            // Prefer the procedural seed point; fallback to centroid of visible systems.
            if (const auto* reg = find_ptr(s.regions, rid)) {
              p = reg->center - world_center;
            } else {
              const auto& pts = kv.second;
              if (!pts.empty()) {
                Vec2 sum{0.0, 0.0};
                for (const Vec2& q : pts) sum = sum + q;
                p = sum * (1.0 / static_cast<double>(pts.size()));
              }
            }

            // Deterministic micro-jitter to avoid degenerate equal-site cases.
            const std::uint64_t h = mix64(static_cast<std::uint64_t>(rid) * 0xA24BAED4963EE407ULL);
            const double ang = (double)((h & 0xFFFFu) / 65535.0) * 6.283185307179586;
            const double rad = std::max(1.0e-6, max_half_span * 1.0e-3);
            p.x += std::cos(ang) * rad;
            p.y += std::sin(ang) * rad;

            sites.push_back(Site{rid, p});
          }

          // Build cells by half-plane intersection.
          for (const Site& si : sites) {
            std::vector<Vec2> poly = bbox;

            for (const Site& sj : sites) {
              if (si.rid == sj.rid) continue;

              const Vec2 n = sj.p - si.p;
              const double nlen2 = n.x * n.x + n.y * n.y;
              if (nlen2 < 1.0e-18) continue;

              // Keep the half-plane closer to si than sj:
              //   dot(x, n) <= dot(mid, n), where mid=(si+sj)/2 and n=(sj-si).
              const Vec2 mid = (si.p + sj.p) * 0.5;
              const double c = dot2(mid, n);

              poly = clip_halfplane(poly, n, c);
              if (poly.size() < 3) {
                poly.clear();
                break;
              }
            }

            if (poly.size() < 3) continue;
            simplify_convex(poly);
            if (poly.size() < 3) continue;

            vcache.cells[si.rid] = std::move(poly);
          }
        };

        // Cache key: visible region ids + FoW flag + viewer faction.
        std::uint64_t acc = 0;
        for (const auto& kv : reg_pts) {
          acc ^= mix64(static_cast<std::uint64_t>(kv.first) * 0x9E3779B97F4A7C15ULL);
        }

        std::uint64_t key = 0xD0C0BEEF0DDF00DULL;
        key ^= mix64(static_cast<std::uint64_t>(ui.fog_of_war) << 1);
        key ^= mix64(static_cast<std::uint64_t>(viewer_faction_id) << 2);
        key ^= mix64(acc);
        key = mix64(key);

        if (key != vcache.key) {
          vcache.key = key;
          build_voronoi();
        }

        for (const auto& kv : vcache.cells) {
          const Id rid = kv.first;
          const auto& cell = kv.second;
          if (cell.size() < 3) continue;

          // Colors/alpha (highlight selected region and optionally dim others).
          float fill_a = 0.05f;
          float line_a = 0.25f;
          if (ui.selected_region_id != kInvalidId) {
            if (rid == ui.selected_region_id) {
              fill_a = 0.09f;
              line_a = 0.55f;
            } else if (ui.galaxy_region_dim_nonselected) {
              fill_a *= 0.25f;
              line_a *= 0.35f;
            }
          }

          const ImU32 fill = region_col(rid, fill_a);
          const ImU32 line = region_col(rid, line_a);

          std::vector<ImVec2> poly;
          poly.reserve(cell.size());
          for (const Vec2& p : cell) poly.push_back(to_screen(p, center_px, scale, zoom, pan));
          draw->AddConvexPolyFilled(poly.data(), static_cast<int>(poly.size()), fill);
          draw->AddPolyline(poly.data(), static_cast<int>(poly.size()), line, true, 2.0f);
        }
      } else {
        // Hull boundaries mode (original).
        for (const auto& kv : reg_pts) {
          const Id rid = kv.first;
          std::vector<Vec2> hull = convex_hull(kv.second);
          if (hull.empty()) continue;

          // Colors/alpha (highlight selected region and optionally dim others).
          float fill_a = 0.05f;
          float line_a = 0.25f;
          if (ui.selected_region_id != kInvalidId) {
            if (rid == ui.selected_region_id) {
              fill_a = 0.09f;
              line_a = 0.55f;
            } else if (ui.galaxy_region_dim_nonselected) {
              fill_a *= 0.25f;
              line_a *= 0.35f;
            }
          }

          const ImU32 fill = region_col(rid, fill_a);
          const ImU32 line = region_col(rid, line_a);

          if (hull.size() == 1) {
            const ImVec2 p = to_screen(hull[0], center_px, scale, zoom, pan);
            draw->AddCircleFilled(p, 18.0f, fill);
            draw->AddCircle(p, 18.0f, line, 0, 2.0f);
            continue;
          }

          if (hull.size() == 2) {
            const ImVec2 p0 = to_screen(hull[0], center_px, scale, zoom, pan);
            const ImVec2 p1 = to_screen(hull[1], center_px, scale, zoom, pan);
            draw->AddLine(p0, p1, fill, 12.0f);
            draw->AddLine(p0, p1, line, 2.0f);
            draw->AddCircleFilled(p0, 6.0f, fill);
            draw->AddCircleFilled(p1, 6.0f, fill);
            draw->AddCircle(p0, 6.0f, line, 0, 2.0f);
            draw->AddCircle(p1, 6.0f, line, 0, 2.0f);
            continue;
          }

          std::vector<ImVec2> poly;
          poly.reserve(hull.size());
          for (const Vec2& p : hull) poly.push_back(to_screen(p, center_px, scale, zoom, pan));
          draw->AddConvexPolyFilled(poly.data(), static_cast<int>(poly.size()), fill);
          draw->AddPolyline(poly.data(), static_cast<int>(poly.size()), line, true, 2.0f);
        }
      }
    }

    // Optional region seed/center points (debug overlay).
    if (ui.show_galaxy_region_centers && !reg_pts.empty()) {
      for (const auto& kv : reg_pts) {
        const Id rid = kv.first;

        // Use the procedural seed point when present; fallback to centroid of visible systems.
        Vec2 p{0.0, 0.0};
        if (const auto* reg = find_ptr(s.regions, rid)) {
          p = reg->center - world_center;
        } else if (!kv.second.empty()) {
          Vec2 sum{0.0, 0.0};
          for (const Vec2& q : kv.second) sum = sum + q;
          p = sum * (1.0 / static_cast<double>(kv.second.size()));
        }

        float a = 0.55f;
        if (ui.selected_region_id != kInvalidId) {
          if (rid == ui.selected_region_id) a = 0.85f;
          else if (ui.galaxy_region_dim_nonselected) a *= 0.35f;
        }

        const ImVec2 sp = to_screen(p, center_px, scale, zoom, pan);
        const ImU32 col = region_col(rid, a);
        const ImU32 sh = modulate_alpha(IM_COL32(0, 0, 0, 220), a);

        const float r = 6.0f;
        draw->AddLine(ImVec2(sp.x - r, sp.y), ImVec2(sp.x + r, sp.y), sh, 3.0f);
        draw->AddLine(ImVec2(sp.x, sp.y - r), ImVec2(sp.x, sp.y + r), sh, 3.0f);
        draw->AddLine(ImVec2(sp.x - r, sp.y), ImVec2(sp.x + r, sp.y), col, 1.5f);
        draw->AddLine(ImVec2(sp.x, sp.y - r), ImVec2(sp.x, sp.y + r), col, 1.5f);
      }
    }
    ScaleBarStyle sb;
    sb.enabled = true;
    sb.desired_px = 120.0f;
    sb.alpha = 0.85f;
    const double units_per_px = 1.0 / std::max(1e-12, scale * zoom);
    draw_scale_bar(draw, origin, avail, units_per_px, IM_COL32(220, 220, 220, 255), sb, "u");
  }

  // Axes (when grid is disabled).
  if (!ui.galaxy_map_grid) {
    draw->AddLine(ImVec2(origin.x, center_px.y), ImVec2(origin.x + avail.x, center_px.y), IM_COL32(35, 35, 35, 255));
    draw->AddLine(ImVec2(center_px.x, origin.y), ImVec2(center_px.x, origin.y + avail.y), IM_COL32(35, 35, 35, 255));
  }

  // Unknown exits count (per visible system).
  std::unordered_map<Id, int> unknown_exits;
  if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
    for (const auto& v : visible) {
      int u = 0;
      for (Id jid : v.sys->jump_points) {
        const auto* jp = find_ptr(s.jump_points, jid);
        if (!jp) continue;

        // If the exit hasn't been surveyed, it counts as an unknown exit.
        if (!sim.is_jump_point_surveyed_by_faction(viewer_faction_id, jid)) {
          u++;
          continue;
        }

        const auto* dest_jp = find_ptr(s.jump_points, jp->linked_jump_id);
        if (!dest_jp) continue;
        if (!sim.is_system_discovered_by_faction(viewer_faction_id, dest_jp->system_id)) {
          u++;
        }
      }
      unknown_exits[v.id] = u;
    }
  }

  // Chokepoint (articulation) systems in the *visible* jump graph.
  //
  // This is a "data lens" for reading the strategic shape of the map. Under
  // fog-of-war, we only use surveyed links + discovered destinations to avoid
  // leaking info.
  std::unordered_set<Id> chokepoints;
  if (ui.show_galaxy_chokepoints) {
    std::vector<Id> vids;
    vids.reserve(visible.size());
    for (const auto& v : visible) vids.push_back(v.id);

    std::unordered_map<Id, int> vidx;
    vidx.reserve(vids.size() * 2);
    for (int i = 0; i < static_cast<int>(vids.size()); ++i) vidx[vids[static_cast<std::size_t>(i)]] = i;

    std::vector<std::vector<int>> adj(vids.size());
    std::unordered_set<std::uint64_t> ekeys;
    ekeys.reserve(s.jump_points.size());

    auto add_edge = [&](Id a_id, Id b_id) {
      const auto ita = vidx.find(a_id);
      const auto itb = vidx.find(b_id);
      if (ita == vidx.end() || itb == vidx.end()) return;
      const int a = ita->second;
      const int b = itb->second;
      if (a == b) return;
      const auto lo = static_cast<std::uint32_t>(std::min(a, b));
      const auto hi = static_cast<std::uint32_t>(std::max(a, b));
      const std::uint64_t k = (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
      if (!ekeys.insert(k).second) return;
      adj[static_cast<std::size_t>(a)].push_back(b);
      adj[static_cast<std::size_t>(b)].push_back(a);
    };

    for (const auto& v : visible) {
      if (!v.sys) continue;
      for (Id jid : v.sys->jump_points) {
        const auto* jp = find_ptr(s.jump_points, jid);
        if (!jp) continue;
        const auto* other = find_ptr(s.jump_points, jp->linked_jump_id);
        if (!other) continue;
        const Id dest_sys_id = other->system_id;
        if (dest_sys_id == kInvalidId) continue;

        if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
          if (!sim.is_jump_point_surveyed_by_faction(viewer_faction_id, jid)) continue;
          if (!sim.is_system_discovered_by_faction(viewer_faction_id, dest_sys_id)) continue;
        }

        add_edge(v.id, dest_sys_id);
      }
    }

    const int n = static_cast<int>(vids.size());
    std::vector<int> disc(static_cast<std::size_t>(n), -1);
    std::vector<int> low(static_cast<std::size_t>(n), -1);
    std::vector<int> parent(static_cast<std::size_t>(n), -1);
    std::vector<char> ap(static_cast<std::size_t>(n), 0);
    int t = 0;

    auto dfs = [&](auto&& self, int u) -> void {
      disc[static_cast<std::size_t>(u)] = low[static_cast<std::size_t>(u)] = t++;
      int children = 0;
      for (int v : adj[static_cast<std::size_t>(u)]) {
        if (disc[static_cast<std::size_t>(v)] == -1) {
          parent[static_cast<std::size_t>(v)] = u;
          ++children;
          self(self, v);
          low[static_cast<std::size_t>(u)] = std::min(low[static_cast<std::size_t>(u)], low[static_cast<std::size_t>(v)]);

          if (parent[static_cast<std::size_t>(u)] == -1 && children > 1) ap[static_cast<std::size_t>(u)] = 1;
          if (parent[static_cast<std::size_t>(u)] != -1 && low[static_cast<std::size_t>(v)] >= disc[static_cast<std::size_t>(u)]) {
            ap[static_cast<std::size_t>(u)] = 1;
          }
        } else if (v != parent[static_cast<std::size_t>(u)]) {
          low[static_cast<std::size_t>(u)] = std::min(low[static_cast<std::size_t>(u)], disc[static_cast<std::size_t>(v)]);
        }
      }
    };

    for (int i = 0; i < n; ++i) {
      if (disc[static_cast<std::size_t>(i)] == -1) dfs(dfs, i);
    }

    for (int i = 0; i < n; ++i) {
      if (ap[static_cast<std::size_t>(i)]) chokepoints.insert(vids[static_cast<std::size_t>(i)]);
    }
  }

  // Star Atlas (procedural constellations): faint connective skeleton that
  // groups visible systems into deterministic clusters.
  if (ui.galaxy_star_atlas_constellations && zoom >= ui.galaxy_star_atlas_min_zoom) {
    std::vector<Id> vids;
    vids.reserve(visible.size());
    for (const auto& v : visible) vids.push_back(v.id);

    GalaxyConstellationParams params;
    params.target_cluster_size = ui.galaxy_star_atlas_target_cluster_size;
    params.max_constellations = ui.galaxy_star_atlas_max_constellations;

    struct Cache {
      std::uint64_t key{0};
      std::vector<GalaxyConstellation> constellations;
    };
    static Cache cache;
    static std::uint64_t cache_state_gen = 0;
    if (cache_state_gen != sim.state_generation()) {
      cache = Cache{};
      cache_state_gen = sim.state_generation();
    }

    // Order-independent key: include params, fog flag, and the set of visible system ids.
    std::uint64_t acc = 0;
    for (Id id : vids) acc ^= mix64(static_cast<std::uint64_t>(id) * 0xA24BAED4963EE407ULL);
    std::uint64_t key = 0xC0FFEE00BADC0DE1ULL;
    key ^= mix64(static_cast<std::uint64_t>(params.target_cluster_size));
    key ^= mix64(static_cast<std::uint64_t>(params.max_constellations) << 1);
    key ^= mix64(static_cast<std::uint64_t>(ui.fog_of_war) << 8);
    key ^= mix64(acc);
    key = mix64(key);

    if (key != cache.key) {
      cache.key = key;
      cache.constellations = build_galaxy_constellations(s, vids, params);
    }

    const float zfade = std::clamp(static_cast<float>((zoom - ui.galaxy_star_atlas_min_zoom) / std::max(0.10f, ui.galaxy_star_atlas_min_zoom)), 0.0f, 1.0f);
    const float a_line = std::clamp(ui.galaxy_star_atlas_alpha * zfade, 0.0f, 1.0f);
    const float a_label = std::clamp(ui.galaxy_star_atlas_label_alpha * zfade, 0.0f, 1.0f);

    for (const auto& c : cache.constellations) {
      const ImU32 col = region_col(c.region_id, a_line);
      for (const auto& e : c.edges) {
        const auto* a_sys = find_ptr(s.systems, e.a);
        const auto* b_sys = find_ptr(s.systems, e.b);
        if (!a_sys || !b_sys) continue;
        const Vec2 a = a_sys->galaxy_pos - world_center;
        const Vec2 b = b_sys->galaxy_pos - world_center;
        const ImVec2 pa = to_screen(a, center_px, scale, zoom, pan);
        const ImVec2 pb = to_screen(b, center_px, scale, zoom, pan);
        draw->AddLine(pa, pb, col, 1.0f);
      }

      if (ui.galaxy_star_atlas_labels && a_label > 0.001f) {
        const ImU32 tcol = region_col(c.region_id, a_label);
        const Vec2 rel = c.centroid - world_center;
        const ImVec2 pc = to_screen(rel, center_px, scale, zoom, pan);
        draw->AddText(ImVec2(pc.x + 4.0f, pc.y + 4.0f), tcol, c.name.c_str());
      }
    }
  }

  // Jump links (only between visible systems under FoW).
  if (ui.show_galaxy_jump_lines) {
    const float jump_flow_alpha = std::clamp(ui.map_route_opacity, 0.15f, 1.0f);
    for (const auto& v : visible) {
      for (Id jid : v.sys->jump_points) {
        const auto* jp = find_ptr(s.jump_points, jid);
        if (!jp) continue;
        const auto* dest_jp = find_ptr(s.jump_points, jp->linked_jump_id);
        if (!dest_jp) continue;

        const auto* dest_sys = find_ptr(s.systems, dest_jp->system_id);
        if (!dest_sys) continue;

        if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
          if (!sim.is_jump_point_surveyed_by_faction(viewer_faction_id, jid)) continue;
          if (!sim.is_system_discovered_by_faction(viewer_faction_id, dest_sys->id)) continue;
        }

        // Only draw once per pair.
        if (v.id > dest_sys->id) continue;

        const Vec2 a = v.sys->galaxy_pos - world_center;
        const Vec2 b = dest_sys->galaxy_pos - world_center;
        const ImVec2 pa = to_screen(a, center_px, scale, zoom, pan);
        const ImVec2 pb = to_screen(b, center_px, scale, zoom, pan);
        ImU32 col = IM_COL32(120, 120, 160, 200);
        float thick = 2.0f;

        // Optional: highlight links that cross region borders (debugging / strategic overlay).
        if (ui.show_galaxy_region_border_links && v.sys) {
          const Id ra = v.sys->region_id;
          const Id rb = dest_sys->region_id;
          if (ra != kInvalidId && rb != kInvalidId && ra != rb) {
            float alpha = 0.55f;
            thick = 3.25f;

            // If a region is selected, emphasize only links on its border.
            if (ui.selected_region_id != kInvalidId) {
              if (ra == ui.selected_region_id || rb == ui.selected_region_id) {
                alpha = 0.85f;
                thick = 4.25f;
              } else if (ui.galaxy_region_dim_nonselected) {
                alpha *= 0.30f;
                thick = 2.0f;
              }
            }

            col = modulate_alpha(IM_COL32(255, 210, 120, 255), alpha);
            draw->AddLine(pa, pb, modulate_alpha(IM_COL32(0, 0, 0, 220), alpha), thick + 1.5f);
          }
        }

        draw->AddLine(pa, pb, col, thick);

        // Procedural transit cue: moving packets suggest route "flow" and make
        // long links easier to scan at a glance.
        if (zoom >= 0.45f) {
          const std::uint64_t seed = mix64(static_cast<std::uint64_t>(v.id) * 0x9E3779B97F4A7C15ULL) ^
                                     mix64(static_cast<std::uint64_t>(dest_sys->id) * 0xA24BAED4963EE407ULL);
          draw_jump_flow_packets(draw, pa, pb, IM_COL32(220, 230, 255, 255), jump_flow_alpha, seed,
                                 static_cast<float>(zoom));
        }
      }
    }
  }



  // Auto-freight lane overlay: draws directional cargo routes inferred from ship orders.
  // This is a strategic debugging/operational lens for seeing where your logistics ships are flowing.
  if (ui.show_galaxy_freight_lanes) {
    struct LaneKey {
      Id a{kInvalidId};
      Id b{kInvalidId};
    };
    struct LaneAgg {
      int ships{0};
      double tons{0.0};
    };
    struct LaneKeyHash {
      std::size_t operator()(const LaneKey& k) const noexcept {
        // Stable-ish mix for Id pairs.
        const std::size_t h1 = std::hash<Id>{}(k.a);
        const std::size_t h2 = std::hash<Id>{}(k.b);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
      }
    };
    struct LaneKeyEq {
      bool operator()(const LaneKey& x, const LaneKey& y) const noexcept { return x.a == y.a && x.b == y.b; }
    };

    auto colony_system_id = [&](Id colony_id) -> Id {
      const auto* c = find_ptr(s.colonies, colony_id);
      if (!c) return kInvalidId;
      const auto* b = find_ptr(s.bodies, c->body_id);
      if (!b) return kInvalidId;
      return b->system_id;
    };

    auto ship_cargo_used = [&](const Ship& sh) {
      double used = 0.0;
      for (const auto& kv : sh.cargo) used += std::max(0.0, kv.second);
      return used;
    };

    // Aggregate routes to reduce clutter.
    std::unordered_map<LaneKey, LaneAgg, LaneKeyHash, LaneKeyEq> lanes;
    lanes.reserve(128);

    // Default: show viewer faction's logistics when defined; otherwise show everything.
    const Id lane_faction = viewer_faction_id;

    for (const auto& kv : s.ships) {
      const Ship& sh = kv.second;
      if (!sh.auto_freight) continue;
      if (lane_faction != kInvalidId && sh.faction_id != lane_faction) continue;
      if (sh.system_id == kInvalidId) continue;

      const auto* so = find_ptr(s.ship_orders, sh.id);
      if (!so) continue;
      const bool templ = so->queue.empty() && so->repeat && !so->repeat_template.empty() && so->repeat_count_remaining != 0;
      const auto& q = templ ? so->repeat_template : so->queue;
      if (q.empty()) continue;

      Id src_colony = kInvalidId;
      Id dst_colony = kInvalidId;
      double load_tons = 0.0;
      double unload_tons = 0.0;

      for (const auto& ord : q) {
        if (const auto* lm = std::get_if<nebula4x::LoadMineral>(&ord)) {
          if (src_colony == kInvalidId) src_colony = lm->colony_id;
          if (lm->colony_id == src_colony && lm->tons > 0.0) load_tons += lm->tons;
        }
        if (const auto* um = std::get_if<nebula4x::UnloadMineral>(&ord)) {
          if (dst_colony == kInvalidId) dst_colony = um->colony_id;
          if (um->colony_id == dst_colony && um->tons > 0.0) unload_tons += um->tons;
        }
      }

      if (dst_colony == kInvalidId) continue;

      Id src_sys = (src_colony != kInvalidId) ? colony_system_id(src_colony) : sh.system_id;
      Id dst_sys = colony_system_id(dst_colony);
      if (src_sys == kInvalidId || dst_sys == kInvalidId) continue;
      if (src_sys == dst_sys) continue;

      // Respect fog-of-war: only draw lanes where both endpoints are visible.
      if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, src_sys)) continue;
      if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, dst_sys)) continue;

      double tons = (unload_tons > 1e-6) ? unload_tons : load_tons;
      if (tons <= 1e-6) tons = ship_cargo_used(sh);

      LaneKey key{src_sys, dst_sys};
      LaneAgg& agg = lanes[key];
      agg.ships++;
      agg.tons += std::max(0.0, tons);
    }

    if (!lanes.empty()) {
      // Sort by volume so the busiest lanes get drawn first (and kept when we hit caps).
      struct LaneItem { LaneKey key; LaneAgg agg; };
      std::vector<LaneItem> items;
      items.reserve(lanes.size());
      for (const auto& kv : lanes) items.push_back(LaneItem{kv.first, kv.second});

      std::sort(items.begin(), items.end(), [](const LaneItem& a, const LaneItem& b) {
        if (a.agg.tons > b.agg.tons + 1e-6) return true;
        if (b.agg.tons > a.agg.tons + 1e-6) return false;
        return a.agg.ships > b.agg.ships;
      });

      const std::size_t kMaxLanes = 256;
      if (items.size() > kMaxLanes) items.resize(kMaxLanes);

      const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
      const ImU32 base = IM_COL32(120, 220, 140, 255);
      const ImU32 col = modulate_alpha(base, 0.55f * alpha);
      const ImU32 shadow = modulate_alpha(IM_COL32(0, 0, 0, 200), 0.55f * alpha);

      for (const auto& it : items) {
        const auto* a_sys = find_ptr(s.systems, it.key.a);
        const auto* b_sys = find_ptr(s.systems, it.key.b);
        if (!a_sys || !b_sys) continue;

        const Vec2 a = a_sys->galaxy_pos - world_center;
        const Vec2 b = b_sys->galaxy_pos - world_center;
        const ImVec2 pa = to_screen(a, center_px, scale, zoom, pan);
        const ImVec2 pb = to_screen(b, center_px, scale, zoom, pan);

        // Thickness scales gently with total planned tonnage.
        const double t = std::max(0.0, it.agg.tons);
        const float thick = 1.0f + static_cast<float>(std::clamp(std::log10(t + 1.0) * 0.75, 0.0, 4.0));

        draw->AddLine(pa, pb, shadow, thick + 2.0f);
        draw->AddLine(pa, pb, col, thick);
        add_arrowhead(draw, pa, pb, col, 8.0f + thick * 2.0f);
        draw->AddCircleFilled(pb, 3.0f + thick, shadow, 0);
        draw->AddCircleFilled(pb, 2.25f + thick * 0.75f, col, 0);

        if (zoom >= 0.75f && it.agg.ships > 1) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%dx", it.agg.ships);
          const ImVec2 mid{(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f};
          draw->AddText(ImVec2(mid.x + 6.0f, mid.y + 4.0f), shadow, buf);
          draw->AddText(ImVec2(mid.x + 5.0f, mid.y + 3.0f), col, buf);
        }
      }
    }
  }

  // Procedural interstellar trade overlay.
  //
  // This draws a *civilian* trade network inferred from system resources,
  // region themes, jump-network topology, and (optionally) colony industry.
  // The intent is to give players strategic context and to provide a future hook
  // for piracy/blockade mechanics.
  if (ui.show_galaxy_trade_lanes || ui.show_galaxy_trade_hubs) {
    TradeNetworkOptions topt;
    topt.max_lanes = 220;
    topt.max_goods_per_lane = 3;
    topt.distance_exponent = 1.35;
    topt.include_uncolonized_markets = true;
    topt.include_colony_contributions = true;
    topt.colony_tons_per_unit = 100.0;

    // Cache the procedural trade network across frames.
    //
    // Without this, the map would recompute an O(N^2) style network every frame
    // while the overlay is enabled, which is unnecessary when the simulation
    // hasn't advanced.
    const std::int64_t day = s.date.days_since_epoch();
    const int hour = std::clamp(s.hour_of_day, 0, 23);
    const std::uint64_t gen = sim.state_generation();
    const std::uint64_t cgen = sim.content_generation();

    auto& cache = trade_overlay_cache();
    if (!cache.net || cache.day != day || cache.hour != hour ||
        cache.state_generation != gen || cache.content_generation != cgen ||
        !trade_opts_equal(cache.opts, topt)) {
      cache.net = compute_trade_network(sim, topt);
      cache.day = day;
      cache.hour = hour;
      cache.state_generation = gen;
      cache.content_generation = cgen;
      cache.opts = topt;
      cache.has_opts = true;
    }
    trade_net = cache.net ? &*cache.net : nullptr;

    auto kind_col = [&](TradeGoodKind k, float alpha) {
      alpha = std::clamp(alpha, 0.0f, 1.0f);
      ImU32 c = IM_COL32(220, 200, 120, 255);
      switch (k) {
        case TradeGoodKind::RawMetals: c = IM_COL32(110, 210, 255, 255); break;
        case TradeGoodKind::RawMinerals: c = IM_COL32(120, 255, 150, 255); break;
        case TradeGoodKind::Volatiles: c = IM_COL32(255, 200, 100, 255); break;
        case TradeGoodKind::Exotics: c = IM_COL32(220, 130, 255, 255); break;
        case TradeGoodKind::ProcessedMetals: c = IM_COL32(210, 210, 210, 255); break;
        case TradeGoodKind::ProcessedMinerals: c = IM_COL32(180, 220, 190, 255); break;
        case TradeGoodKind::Fuel: c = IM_COL32(255, 230, 80, 255); break;
        case TradeGoodKind::Munitions: c = IM_COL32(255, 120, 120, 255); break;
        default: break;
      }
      return modulate_alpha(c, alpha);
    };

    const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
    const ImU32 shadow = modulate_alpha(IM_COL32(0, 0, 0, 200), 0.55f * alpha);

    // Clear per-frame list.
    trade_lanes_visible.clear();
    trade_lanes_visible.reserve(256);

    // Trade lane filtering.
    const int good_filter = ui.galaxy_trade_good_filter;
    const float min_volume = std::max(0.0f, ui.galaxy_trade_min_lane_volume);
    const bool include_secondary = ui.galaxy_trade_filter_include_secondary;

    auto lane_passes_filters = [&](const TradeLane& lane) -> bool {
      const double v = std::max(0.0, lane.total_volume);
      if (min_volume > 0.0f && v < static_cast<double>(min_volume)) return false;

      if (good_filter < 0) return true;
      const TradeGoodKind want = static_cast<TradeGoodKind>(good_filter);
      if (lane.top_flows.empty()) return false;
      if (lane.top_flows.front().good == want) return true;
      if (!include_secondary) return false;
      for (const auto& f : lane.top_flows) {
        if (f.good == want && f.volume > 1e-9) return true;
      }
      return false;
    };

    // If a pinned lane no longer exists (trade network changed), clear it.
    if (trade_net && ui.galaxy_trade_pinned_from != kInvalidId && ui.galaxy_trade_pinned_to != kInvalidId) {
      bool found = false;
      for (const auto& lane : trade_net->lanes) {
        if (lane.from_system_id == ui.galaxy_trade_pinned_from && lane.to_system_id == ui.galaxy_trade_pinned_to) {
          found = true;
          break;
        }
      }
      if (!found) {
        ui.galaxy_trade_pinned_from = kInvalidId;
        ui.galaxy_trade_pinned_to = kInvalidId;
      }
    }

    if (trade_net) {
      for (const auto& lane : trade_net->lanes) {
        const auto itA = visible_sys_by_id.find(lane.from_system_id);
        const auto itB = visible_sys_by_id.find(lane.to_system_id);
        if (itA == visible_sys_by_id.end() || itB == visible_sys_by_id.end()) {
          continue;
        }

        const StarSystem* sysA = itA->second;
        const StarSystem* sysB = itB->second;
        if (!sysA || !sysB) {
          continue;
        }

        const bool pinned_match =
            (lane.from_system_id == ui.galaxy_trade_pinned_from && lane.to_system_id == ui.galaxy_trade_pinned_to);

        const bool draw_this = lane_passes_filters(lane) || pinned_match;
        if (!draw_this) {
          continue;
        }

        // Always keep the lane for analysis/tooltip purposes (even if the player disables lane rendering).
        trade_lanes_visible.push_back(&lane);

        if (!ui.show_galaxy_trade_lanes) {
          continue;
        }

        const Vec2 a = sysA->galaxy_pos - world_center;
        const Vec2 b = sysB->galaxy_pos - world_center;
        const ImVec2 pa = to_screen(a, center_px, scale, zoom, pan);
        const ImVec2 pb = to_screen(b, center_px, scale, zoom, pan);

        const double vol = std::max(0.0, lane.total_volume);
        float thick = 1.0f + 3.0f * std::clamp(static_cast<float>(std::log10(vol + 1.0) / 3.0), 0.0f, 1.0f);
        if (pinned_match) {
          thick += 1.5f;
        }

        const TradeGoodKind dom_good = lane.top_flows.empty() ? TradeGoodKind::RawMetals : lane.top_flows.front().good;
        const ImU32 lane_col = kind_col(dom_good, (pinned_match ? 0.65f : 0.40f) * alpha);

        draw->AddLine(pa, pb, shadow, thick + 1.5f);
        draw->AddLine(pa, pb, lane_col, thick);

        // Optional risk overlay (green=safe, red=dangerous).
        double ravg = 0.0;
        if (ui.galaxy_trade_risk_overlay) {
          const double r0 = system_piracy_risk_effective(sim, s, lane.from_system_id);
          const double r1 = system_piracy_risk_effective(sim, s, lane.to_system_id);
          ravg = 0.5 * (r0 + r1);
          const ImU32 risk_col = risk_gradient_color(static_cast<float>(ravg), (pinned_match ? 0.55f : 0.35f) * alpha);
          draw->AddLine(pa, pb, risk_col, thick + (pinned_match ? 1.5f : 0.75f));
        }

        // Hover pick: closest lane within a small pixel threshold.
        if (hovered && mouse_in_rect && !over_minimap && !over_legend && !io.WantTextInput && !ImGui::IsAnyItemHovered()) {
          const ImVec2 mp = io.MousePos;
          const float d2 = dist2_point_segment(mp, pa, pb);
          const float thresh = 8.0f + thick * 1.5f;
          if (d2 < thresh * thresh && d2 < trade_lane_hover.d2) {
            trade_lane_hover.lane = &lane;
            trade_lane_hover.d2 = d2;
            trade_lane_hover.a = pa;
            trade_lane_hover.b = pb;
            trade_lane_hover.thickness = thick;
            trade_lane_hover.risk_avg = ravg;
          }
        }
      }

      // Render hub markers.
      if (ui.show_galaxy_trade_hubs && zoom >= 0.50f) {
        for (const auto& node : trade_net->nodes) {
          if (node.system_id == kInvalidId) continue;
          if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, node.system_id)) continue;
          const auto* sys = find_ptr(s.systems, node.system_id);
          if (!sys) continue;

          // If a commodity filter is active, dim hubs that don't match.
          bool hub_ok = true;
          if (good_filter >= 0) {
            const TradeGoodKind want = static_cast<TradeGoodKind>(good_filter);
            hub_ok = (node.primary_export == want);
          }

          const Vec2 p = sys->galaxy_pos - world_center;
          const ImVec2 pp = to_screen(p, center_px, scale, zoom, pan);

          const float r = 4.0f + static_cast<float>(std::clamp(node.market_size, 0.0, 2.5)) * 4.0f;
          float a = 0.20f + 0.45f * static_cast<float>(std::clamp(node.hub_score, 0.0, 1.0));
          if (!hub_ok) a *= 0.35f;
          const ImU32 col = kind_col(node.primary_export, a * alpha);
          draw->AddCircle(pp, r, shadow, 0, 3.0f);
          draw->AddCircle(pp, r, col, 0, 1.5f);
        }
      }
    }
  }
  // Selected ship/fleet travel route overlay (linked elements).
  if (ui.galaxy_map_selected_route) {
    Id route_ship_id = selected_ship;
    if (route_ship_id == kInvalidId && selected_fleet && selected_fleet->leader_ship_id != kInvalidId) {
      route_ship_id = selected_fleet->leader_ship_id;
    }

    const auto* sh = find_ptr(s.ships, route_ship_id);
    const auto* so = sh ? find_ptr(s.ship_orders, route_ship_id) : nullptr;
    if (sh && so) {
      const bool templ = so->queue.empty() && so->repeat && !so->repeat_template.empty() &&
                         so->repeat_count_remaining != 0;
      const auto& q = (templ ? so->repeat_template : so->queue);

      std::vector<Id> route_systems;
      route_systems.reserve(q.size() + 1);
      route_systems.push_back(sh->system_id);

      for (const auto& ord : q) {
        if (!std::holds_alternative<nebula4x::TravelViaJump>(ord)) continue;
        const auto& o = std::get<nebula4x::TravelViaJump>(ord);

        // Don't leak an unsurveyed link under FoW.
        if (ui.fog_of_war && viewer_faction_id != kInvalidId &&
            !sim.is_jump_point_surveyed_by_faction(viewer_faction_id, o.jump_point_id)) {
          break;
        }

        const auto* jp = find_ptr(s.jump_points, o.jump_point_id);
        if (!jp) continue;
        const auto* other = find_ptr(s.jump_points, jp->linked_jump_id);
        if (!other) continue;
        const Id dest_sys = other->system_id;

        // Don't leak undiscovered destinations under FoW.
        if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, dest_sys)) break;

        route_systems.push_back(dest_sys);
      }

      if (route_systems.size() >= 2) {
        const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
        const ImU32 base = templ ? IM_COL32(160, 160, 160, 255) : IM_COL32(255, 220, 80, 255);
        const ImU32 col = modulate_alpha(base, templ ? (0.55f * alpha) : alpha);
        const ImU32 shadow = modulate_alpha(IM_COL32(0, 0, 0, 200), templ ? (0.45f * alpha) : (0.8f * alpha));

        for (std::size_t i = 0; i + 1 < route_systems.size(); ++i) {
          const auto* a_sys = find_ptr(s.systems, route_systems[i]);
          const auto* b_sys = find_ptr(s.systems, route_systems[i + 1]);
          if (!a_sys || !b_sys) continue;

          // Respect visibility list to avoid drawing lines to hidden systems.
          if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
            if (!sim.is_system_discovered_by_faction(viewer_faction_id, a_sys->id)) continue;
            if (!sim.is_system_discovered_by_faction(viewer_faction_id, b_sys->id)) continue;
          }

          const Vec2 a = a_sys->galaxy_pos - world_center;
          const Vec2 b = b_sys->galaxy_pos - world_center;
          const ImVec2 pa = to_screen(a, center_px, scale, zoom, pan);
          const ImVec2 pb = to_screen(b, center_px, scale, zoom, pan);

          draw->AddLine(pa, pb, shadow, 4.0f);
          draw->AddLine(pa, pb, col, 2.25f);
          add_arrowhead(draw, pa, pb, col, 10.0f);
          draw->AddCircleFilled(pb, 4.0f, shadow, 0);
          draw->AddCircleFilled(pb, 3.0f, col, 0);

          char buf[16];
          std::snprintf(buf, sizeof(buf), "%zu", i + 1);
          const ImVec2 mid{(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f};
          draw->AddText(ImVec2(mid.x + 6.0f, mid.y + 4.0f), col, buf);
        }
      }
    }
  }

  // Route ruler overlay (hold D and click two systems).
  //
  // Unlike the selected-ship route overlay (which shows the current order queue),
  // the route ruler is a planning utility that lets the player compare routes
  // without disturbing selection.
  std::optional<JumpRoutePlan> ruler_route;
  double ruler_speed_km_s = 0.0;
  const char* ruler_speed_basis = nullptr;
  if (route_ruler.a != kInvalidId && route_ruler.b != kInvalidId && route_ruler.a != route_ruler.b) {
    Vec2 start_pos_mkm{0.0, 0.0};

    // Prefer selected ship speed, otherwise use fleet slowest speed (ETA only).
    if (const auto* sh = (selected_ship != kInvalidId) ? find_ptr(s.ships, selected_ship) : nullptr) {
      ruler_speed_km_s = sh->speed_km_s;
      ruler_speed_basis = "Ship";
      if (sh->system_id == route_ruler.a) start_pos_mkm = sh->position_mkm;
    } else if (selected_fleet && !selected_fleet->ship_ids.empty()) {
      double slowest = std::numeric_limits<double>::infinity();
      for (Id sid : selected_fleet->ship_ids) {
        if (const auto* mem = find_ptr(s.ships, sid)) slowest = std::min(slowest, mem->speed_km_s);
      }
      if (std::isfinite(slowest)) {
        ruler_speed_km_s = slowest;
        ruler_speed_basis = "Fleet";
      }
      if (const auto* lead = (selected_fleet->leader_ship_id != kInvalidId)
                                 ? find_ptr(s.ships, selected_fleet->leader_ship_id)
                                 : nullptr) {
        if (lead->system_id == route_ruler.a) start_pos_mkm = lead->position_mkm;
      }
    }

    const bool restrict = ui.fog_of_war;
    ruler_route = sim.plan_jump_route_from_pos(route_ruler.a, start_pos_mkm, viewer_faction_id, ruler_speed_km_s,
                                               route_ruler.b, restrict);
  }

  if (route_ruler.a != kInvalidId) {
    const auto* a_sys = find_ptr(s.systems, route_ruler.a);
    if (a_sys) {
      const Vec2 a = a_sys->galaxy_pos - world_center;
      const ImVec2 pa = to_screen(a, center_px, scale, zoom, pan);
      const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
      const ImU32 col = modulate_alpha(IM_COL32(80, 220, 255, 255), 0.85f * alpha);
      const ImU32 shadow = modulate_alpha(IM_COL32(0, 0, 0, 200), 0.75f * alpha);
      const float t = static_cast<float>(ImGui::GetTime());
      const float pulse = 0.55f + 0.45f * std::sin(t * 3.2f);
      draw->AddCircle(pa, 12.0f + 3.0f * pulse, shadow, 0, 3.0f);
      draw->AddCircle(pa, 12.0f + 3.0f * pulse, col, 0, 1.75f);
    }
  }

  if (route_ruler.b != kInvalidId) {
    const auto* b_sys = find_ptr(s.systems, route_ruler.b);
    if (b_sys) {
      const Vec2 b = b_sys->galaxy_pos - world_center;
      const ImVec2 pb = to_screen(b, center_px, scale, zoom, pan);
      const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
      const ImU32 col = modulate_alpha(IM_COL32(80, 220, 255, 255), 0.85f * alpha);
      const ImU32 shadow = modulate_alpha(IM_COL32(0, 0, 0, 200), 0.75f * alpha);
      const float t = static_cast<float>(ImGui::GetTime());
      const float pulse = 0.55f + 0.45f * std::sin(t * 3.2f + 1.2f);
      draw->AddCircle(pb, 12.0f + 3.0f * pulse, shadow, 0, 3.0f);
      draw->AddCircle(pb, 12.0f + 3.0f * pulse, col, 0, 1.75f);
    }
  }

  if (route_ruler.a != kInvalidId && route_ruler.b != kInvalidId) {
    const auto* a_sys = find_ptr(s.systems, route_ruler.a);
    const auto* b_sys = find_ptr(s.systems, route_ruler.b);
    if (a_sys && b_sys) {
      const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
      const ImU32 col = modulate_alpha(IM_COL32(80, 220, 255, 255), alpha);
      const ImU32 shadow = modulate_alpha(IM_COL32(0, 0, 0, 200), 0.8f * alpha);

      bool drew_path = false;
      if (ruler_route && ruler_route->systems.size() >= 2) {
        for (std::size_t i = 0; i + 1 < ruler_route->systems.size(); ++i) {
          const auto* a_hop = find_ptr(s.systems, ruler_route->systems[i]);
          const auto* b_hop = find_ptr(s.systems, ruler_route->systems[i + 1]);
          if (!a_hop || !b_hop) continue;

          // Under FoW, avoid drawing any hop that includes an undiscovered system.
          if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
            if (!sim.is_system_discovered_by_faction(viewer_faction_id, a_hop->id)) continue;
            if (!sim.is_system_discovered_by_faction(viewer_faction_id, b_hop->id)) continue;
          }

          const Vec2 a = a_hop->galaxy_pos - world_center;
          const Vec2 b = b_hop->galaxy_pos - world_center;
          const ImVec2 pa = to_screen(a, center_px, scale, zoom, pan);
          const ImVec2 pb = to_screen(b, center_px, scale, zoom, pan);

          draw->AddLine(pa, pb, shadow, 4.0f);
          draw->AddLine(pa, pb, col, 2.25f);
          draw->AddCircleFilled(pb, 4.0f, shadow, 0);
          draw->AddCircleFilled(pb, 3.0f, col, 0);

          // Hop index label.
          char hopbuf[16];
          std::snprintf(hopbuf, sizeof(hopbuf), "%zu", i + 1);
          const ImVec2 mid{(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f};
          draw->AddText(ImVec2(mid.x + 6.0f, mid.y + 4.0f), col, hopbuf);
        }
        drew_path = true;
      }

      if (!drew_path) {
        const Vec2 a = a_sys->galaxy_pos - world_center;
        const Vec2 b = b_sys->galaxy_pos - world_center;
        const ImVec2 pa = to_screen(a, center_px, scale, zoom, pan);
        const ImVec2 pb = to_screen(b, center_px, scale, zoom, pan);
        draw_ruler_line(draw, pa, pb, modulate_alpha(IM_COL32(80, 220, 255, 255), 0.7f * alpha));
      }

      // Compact on-map label near the midpoint (keeps the big details in the legend).
      const Vec2 ra = a_sys->galaxy_pos - world_center;
      const Vec2 rb = b_sys->galaxy_pos - world_center;
      const ImVec2 pa = to_screen(ra, center_px, scale, zoom, pan);
      const ImVec2 pb = to_screen(rb, center_px, scale, zoom, pan);
      const ImVec2 mid{(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f};

      char lbl[192];
      if (ruler_route && ruler_route->systems.size() >= 2) {
        const int jumps = static_cast<int>(ruler_route->systems.size() - 1);
        if (std::isfinite(ruler_route->eta_days) && ruler_speed_km_s > 1e-9) {
          std::snprintf(lbl, sizeof(lbl), "Route ruler: %d jumps  ETA %s", jumps,
                        format_duration_days(ruler_route->eta_days).c_str());
        } else {
          std::snprintf(lbl, sizeof(lbl), "Route ruler: %d jumps", jumps);
        }
      } else {
        std::snprintf(lbl, sizeof(lbl), "Route ruler: no known route");
      }
      draw_ruler_label(draw, ImVec2(mid.x + 8.0f, mid.y + 8.0f), lbl);
    }
  }

  // Nodes (systems) + hover selection.
  const float base_r = 7.0f;
  Id hovered_system = kInvalidId;
  float hovered_d2 = 1e30f;

  struct NodeDrawInfo {
    Id id{kInvalidId};
    const StarSystem* sys{nullptr};
    ImVec2 p{0.0f, 0.0f};
  };

  std::vector<NodeDrawInfo> nodes;
  nodes.reserve(visible.size());

  // Screen-space position lookup for systems by ID.
  // (Used by route previews, mission overlays, and UI picking helpers.)
  std::unordered_map<Id, ImVec2> pos_by_id;
  pos_by_id.reserve(visible.size() * 2);

  for (const auto& v : visible) {
    const Vec2 rel = v.sys->galaxy_pos - world_center;
    const ImVec2 p = to_screen(rel, center_px, scale, zoom, pan);
    nodes.push_back(NodeDrawInfo{v.id, v.sys, p});
    pos_by_id[v.id] = p;

    // Hover detection (disabled when the mouse is over the minimap overlay).
    if (!over_minimap && !over_legend) {
      const float dx = mouse.x - p.x;
      const float dy = mouse.y - p.y;
      const float d2 = dx * dx + dy * dy;
      if (d2 < (base_r + 6.0f) * (base_r + 6.0f) && d2 < hovered_d2) {
        hovered_d2 = d2;
        hovered_system = v.id;
      }
    }
  }

  const bool trade_pick_mode = hovered && mouse_in_rect && !over_minimap && !over_legend && !io.WantTextInput &&
                               (ui.show_galaxy_trade_lanes || ui.show_galaxy_trade_hubs) &&
                               ImGui::IsKeyDown(ImGuiKey_T) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
  const bool route_ruler_mode = hovered && mouse_in_rect && !over_minimap && !over_legend && !io.WantTextInput &&
                               ImGui::IsKeyDown(ImGuiKey_D) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;

  // Quick pin toggle: hover a system and press P.
  // Stored per-faction and persisted in saves.
  if (viewer_faction && hovered_system != kInvalidId && hovered && !over_minimap && !over_legend && !io.WantTextInput &&
      !trade_pick_mode && !route_ruler_mode && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_P)) {
    auto itn = viewer_faction->system_notes.find(hovered_system);
    if (itn == viewer_faction->system_notes.end()) {
      viewer_faction->system_notes[hovered_system].pinned = true;
    } else {
      itn->second.pinned = !itn->second.pinned;
      if (!itn->second.pinned && itn->second.text.empty() && itn->second.tags.empty()) {
        viewer_faction->system_notes.erase(itn);
      }
    }
  }

  // --- Route preview (hover target) ---
  // Planning routes can be expensive, especially when called every frame while hovering.
  // Cache the preview route until the relevant inputs change.
  struct RoutePreviewCacheKey {
    Id hovered_system{kInvalidId};
    Id selected_ship{kInvalidId};
    Id selected_fleet{kInvalidId};
    bool fleet_mode{false};
    bool restrict_to_discovered{false};
    bool from_queue{false};
    std::int64_t sim_day{0};
    std::uint64_t event_seq{0};

    bool operator==(const RoutePreviewCacheKey& o) const {
      return hovered_system == o.hovered_system && selected_ship == o.selected_ship &&
             selected_fleet == o.selected_fleet && fleet_mode == o.fleet_mode &&
             restrict_to_discovered == o.restrict_to_discovered && from_queue == o.from_queue &&
             sim_day == o.sim_day && event_seq == o.event_seq;
    }
  };

  struct RoutePreviewCache {
    bool valid{false};
    RoutePreviewCacheKey key{};
    bool is_fleet{false};
    std::optional<JumpRoutePlan> route{};
  };

  static RoutePreviewCache route_cache;

  std::optional<JumpRoutePlan> preview_route;
  bool preview_is_fleet = false;
  bool preview_from_queue = false;
  if (hovered && hovered_system != kInvalidId && !route_ruler_mode) {
    const bool restrict = ui.fog_of_war;
    const bool from_queue = ImGui::GetIO().KeyShift;
    const bool fleet_mode = (ImGui::GetIO().KeyCtrl && selected_fleet != nullptr);
    const std::int64_t sim_day = s.date.days_since_epoch();

    RoutePreviewCacheKey key;
    key.hovered_system = hovered_system;
    key.selected_ship = fleet_mode ? kInvalidId : selected_ship;
    key.selected_fleet = fleet_mode ? selected_fleet->id : kInvalidId;
    key.fleet_mode = fleet_mode;
    key.restrict_to_discovered = restrict;
    key.from_queue = from_queue;
    key.sim_day = sim_day;
    key.event_seq = s.next_event_seq;

    if (!route_cache.valid || !(route_cache.key == key)) {
      route_cache.valid = true;
      route_cache.key = key;
      route_cache.is_fleet = fleet_mode;
      route_cache.route.reset();

      if (fleet_mode) {
        route_cache.route =
            sim.plan_jump_route_for_fleet(selected_fleet->id, hovered_system, restrict, from_queue);
      } else if (selected_ship != kInvalidId) {
        route_cache.route = sim.plan_jump_route_for_ship(selected_ship, hovered_system, restrict, from_queue);
      }
    }

    preview_route = route_cache.route;
    preview_is_fleet = route_cache.is_fleet;
    preview_from_queue = from_queue;
  }

  // --- Fleet mission overlay (strategic planning geometry) ---
  // Draws patrol routes/circuits and other mission geometry for the viewer faction.
  if (ui.show_galaxy_fleet_missions && !pos_by_id.empty()) {
    auto fleet_base_col = [&](Id fid) -> ImU32 {
      const float h = std::fmod((static_cast<float>(static_cast<std::uint32_t>(fid)) * 0.61803398875f), 1.0f);
      const ImVec4 c = ImColor::HSV(h, 0.55f, 0.95f, 1.0f);
      return ImGui::ColorConvertFloat4ToU32(c);
    };

    for (const auto& kv : s.fleets) {
      const Id fid = kv.first;
      const Fleet& fl = kv.second;

      // Avoid FoW leaks: only show missions for the viewer faction.
      if (viewer_faction_id != kInvalidId && fl.faction_id != viewer_faction_id) continue;

      if (fl.mission.type == FleetMissionType::None) continue;

      const bool is_sel = (selected_fleet != nullptr && fid == selected_fleet->id);
      float a = ui.galaxy_fleet_mission_alpha * (is_sel ? 1.0f : 0.55f);
      a = std::clamp(a, 0.0f, 1.0f);
      const ImU32 base = fleet_base_col(fid);
      const ImU32 line_col = modulate_alpha(base, a);
      const ImU32 ring_col = modulate_alpha(base, std::min(1.0f, a * 1.35f));

      // PatrolRoute: draw A <-> B segment + direction hint from patrol_leg_index.
      if (fl.mission.type == FleetMissionType::PatrolRoute) {
        const Id a_sys = fl.mission.patrol_route_a_system_id;
        const Id b_sys = fl.mission.patrol_route_b_system_id;
        if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, a_sys)) continue;
        if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, b_sys)) continue;

        auto ita = pos_by_id.find(a_sys);
        auto itb = pos_by_id.find(b_sys);
        if (ita == pos_by_id.end() || itb == pos_by_id.end()) continue;

        draw->AddLine(ita->second, itb->second, line_col, is_sel ? 2.8f : 2.0f);

        // Direction hint: even index -> toward B, odd -> toward A.
        const bool to_b = (fl.mission.patrol_leg_index % 2) == 0;
        const ImVec2 from = to_b ? ita->second : itb->second;
        const ImVec2 to = to_b ? itb->second : ita->second;
        add_arrowhead(draw, from, to, line_col, 10.0f);

        // Endpoint rings.
        draw->AddCircle(ita->second, base_r + 10.0f, ring_col, 0, 2.0f);
        draw->AddCircle(itb->second, base_r + 10.0f, ring_col, 0, 2.0f);
      }

      // PatrolCircuit: draw a closed polyline through waypoints.
      if (fl.mission.type == FleetMissionType::PatrolCircuit) {
        const auto& wps = fl.mission.patrol_circuit_system_ids;
        const int n = static_cast<int>(wps.size());

        // Waypoint rings.
        for (int i = 0; i < n; ++i) {
          const Id sid = wps[i];
          if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, sid)) continue;
          auto itp = pos_by_id.find(sid);
          if (itp == pos_by_id.end()) continue;
          draw->AddCircle(itp->second, base_r + 9.0f, ring_col, 0, 2.0f);
        }

        if (n >= 2) {
          for (int i = 0; i < n; ++i) {
            const Id a_sys = wps[i];
            const Id b_sys = wps[(i + 1) % n];
            if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, a_sys)) continue;
            if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, b_sys)) continue;
            auto ita = pos_by_id.find(a_sys);
            auto itb = pos_by_id.find(b_sys);
            if (ita == pos_by_id.end() || itb == pos_by_id.end()) continue;
            draw->AddLine(ita->second, itb->second, line_col, is_sel ? 2.8f : 2.0f);
          }

          // Current-leg direction hint.
          const int idx = (n > 0) ? (std::max(0, fl.mission.patrol_leg_index) % n) : 0;
          const Id cur = wps[idx];
          const Id nxt = wps[(idx + 1) % n];
          if (can_show_system(viewer_faction_id, ui.fog_of_war, sim, cur) &&
              can_show_system(viewer_faction_id, ui.fog_of_war, sim, nxt)) {
            auto itc = pos_by_id.find(cur);
            auto itn = pos_by_id.find(nxt);
            if (itc != pos_by_id.end() && itn != pos_by_id.end()) {
              add_arrowhead(draw, itc->second, itn->second, line_col, 11.0f);
            }
          }
        }
      }

      // GuardJumpPoint: ring the system containing the guarded JP.
      if (fl.mission.type == FleetMissionType::GuardJumpPoint) {
        const auto* jp = find_ptr(s.jump_points, fl.mission.guard_jump_point_id);
        if (!jp) continue;
        const Id sys_id = jp->system_id;
        if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, sys_id)) continue;
        auto itp = pos_by_id.find(sys_id);
        if (itp == pos_by_id.end()) continue;
        draw->AddCircle(itp->second, base_r + 12.0f, ring_col, 0, 2.4f);
      }
    }
  }

  // --- Route preview (hover target) ---
  // Draw after mission geometry so it stays visually on-top.
  if (preview_route && preview_route->systems.size() >= 2) {
    for (std::size_t i = 0; i + 1 < preview_route->systems.size(); ++i) {
      const Id a = preview_route->systems[i];
      const Id b = preview_route->systems[i + 1];
      auto ita = pos_by_id.find(a);
      auto itb = pos_by_id.find(b);
      if (ita == pos_by_id.end() || itb == pos_by_id.end()) continue;
      draw->AddLine(ita->second, itb->second, IM_COL32(255, 235, 80, 200), 3.0f);
    }
  }

  // Region label anchors (computed in screen space from visible systems).
  std::unordered_map<Id, ImVec2> reg_label_sum;
  std::unordered_map<Id, int> reg_label_count;
  if (ui.show_galaxy_regions && ui.show_galaxy_region_labels && zoom >= 0.55f) {
    reg_label_sum.reserve(s.regions.size() * 2);
    reg_label_count.reserve(s.regions.size() * 2);
    for (const auto& n : nodes) {
      if (!n.sys) continue;
      if (n.sys->region_id == kInvalidId) continue;
      reg_label_sum[n.sys->region_id].x += n.p.x;
      reg_label_sum[n.sys->region_id].y += n.p.y;
      ++reg_label_count[n.sys->region_id];
    }
  }


  // Fuel-range overlay for selected ship/fleet (optional).
  struct FuelRangeOverlay {
    bool enabled{false};
    bool fleet_mode{false};
    Id ship_id{kInvalidId};
    Id fleet_id{kInvalidId};
    double range_now_mkm{0.0};
    double range_full_mkm{0.0};
  };

  FuelRangeOverlay fuel_overlay;
  if (ui.galaxy_map_fuel_range && (selected_ship != kInvalidId || selected_fleet != nullptr)) {
    fuel_overlay.enabled = true;
    fuel_overlay.fleet_mode = (selected_fleet != nullptr) && (selected_ship == kInvalidId || ImGui::GetIO().KeyCtrl);

    auto ship_ranges = [&](Id ship_id) -> std::optional<std::pair<double, double>> {
      const auto* sh = find_ptr(s.ships, ship_id);
      if (!sh) {
        return std::nullopt;
      }
      const auto* d = sim.find_design(sh->design_id);
      if (!d) {
        return std::nullopt;
      }

      const double cap = std::max(0.0, d->fuel_capacity_tons);
      const double burn = std::max(0.0, d->fuel_use_per_mkm);
      if (burn <= 0.0) {
        // No fuel burn model => treat as infinite range (still constrained by jump-network visibility).
        const double inf = std::numeric_limits<double>::infinity();
        return std::pair<double, double>{inf, inf};
      }
      if (cap <= 0.0) {
        return std::pair<double, double>{0.0, 0.0};
      }

      double fuel = sh->fuel_tons;
      if (!std::isfinite(fuel) || fuel < 0.0) {
        fuel = cap;
      }
      fuel = std::clamp(fuel, 0.0, cap);
      return std::pair<double, double>{fuel / burn, cap / burn};
    };

    if (fuel_overlay.fleet_mode) {
      fuel_overlay.fleet_id = selected_fleet->id;

      double min_now = std::numeric_limits<double>::infinity();
      double min_full = std::numeric_limits<double>::infinity();
      bool any = false;
      for (Id ship_id : selected_fleet->ship_ids) {
        auto r = ship_ranges(ship_id);
        if (!r) {
          // Missing ship or design; be conservative.
          min_now = 0.0;
          min_full = 0.0;
          any = true;
          continue;
        }
        any = true;
        min_now = std::min(min_now, r->first);
        min_full = std::min(min_full, r->second);
      }
      if (!any) {
        fuel_overlay.enabled = false;
      } else {
        fuel_overlay.range_now_mkm = min_now;
        fuel_overlay.range_full_mkm = min_full;
      }
    } else {
      fuel_overlay.ship_id = selected_ship;
      auto r = ship_ranges(selected_ship);
      if (!r) {
        fuel_overlay.enabled = false;
      } else {
        fuel_overlay.range_now_mkm = r->first;
        fuel_overlay.range_full_mkm = r->second;
      }
    }
  }

  // Precompute colonized systems for label priority.
  std::unordered_set<Id> colonized_systems;
  colonized_systems.reserve(s.colonies.size() * 2 + 4);
  for (const auto& kv : s.colonies) {
    const Colony& c = kv.second;
    const auto* b = find_ptr(s.bodies, c.body_id);
    if (!b) continue;
    if (b->system_id == kInvalidId) continue;
    colonized_systems.insert(b->system_id);
  }

  struct SystemLabelCandidate {
    Id id{kInvalidId};
    ImVec2 anchor{0.0f, 0.0f};
    float dx{0.0f};
    float dy{0.0f};
    const char* text{nullptr};
    ImU32 col{0};
    float priority{0.0f};
  };

  std::vector<SystemLabelCandidate> label_cands;
  label_cands.reserve(nodes.size());
  const bool declutter_labels = !ImGui::GetIO().KeyAlt; // Alt = show all labels.

  // Draw nodes.
  for (const auto& n : nodes) {
    const bool is_selected = (s.selected_system == n.id);

    const bool is_hovered = (hovered_system == n.id);

    const SystemIntelNote* sys_note = nullptr;
    bool is_pinned = false;
    if (viewer_faction) {
      const auto itn = viewer_faction->system_notes.find(n.id);
      if (itn != viewer_faction->system_notes.end()) {
        sys_note = &itn->second;
        is_pinned = sys_note->pinned;
      }
    }

    ImU32 fill = is_selected ? IM_COL32(0, 220, 140, 255) : IM_COL32(240, 240, 240, 255);
    ImU32 lens_col = 0; // full-alpha lens color (for glow/legend).
    if (!is_selected && lens_active && lens_bounds_valid && n.sys) {
      const double raw = procgen_lens_value(s, *n.sys, lens_mode);
      double x = raw;
      if (ui.galaxy_procgen_lens_log_scale && (lens_mode == ProcGenLensMode::MineralWealth)) {
        x = std::log10(std::max(0.0, raw) + 1.0);
      }
      float t = 0.5f;
      if (std::isfinite(x) && lens_max > lens_min + 1e-12) {
        t = static_cast<float>((x - lens_min) / (lens_max - lens_min));
      }
      lens_col = procgen_lens_gradient_color(t, 1.0f);
      fill = modulate_alpha(lens_col, ui.galaxy_procgen_lens_alpha);
    }
    const ImU32 outline = IM_COL32(20, 20, 20, 255);

    // Nebula haze (system-level environmental effect).
    if (n.sys) {
      // Region halo (optional overlay).
      if (ui.show_galaxy_regions && n.sys->region_id != kInvalidId) {
        float a = 0.10f;
        float r = base_r + 14.0f;
        if (ui.selected_region_id != kInvalidId) {
          if (n.sys->region_id == ui.selected_region_id) {
            a = 0.16f;
            r = base_r + 16.0f;
          } else if (ui.galaxy_region_dim_nonselected) {
            a *= 0.35f;
          }
        }
        draw->AddCircleFilled(n.p, r, region_col(n.sys->region_id, a), 0);
      }

      const float neb = (float)std::clamp(n.sys->nebula_density, 0.0, 1.0);
      if (neb > 0.01f) {
        const float r = base_r + 10.0f + neb * 14.0f;
        const float a = 0.06f + 0.22f * neb;
        draw->AddCircleFilled(n.p, r, modulate_alpha(IM_COL32(120, 170, 255, 255), a), 0);
      }
    }

    // Drop shadow + subtle glow for higher visual contrast.
    draw->AddCircleFilled(ImVec2(n.p.x + 1.5f, n.p.y + 1.5f), base_r + 0.5f, IM_COL32(0, 0, 0, 110), 0);
    ImU32 glow_col = is_selected ? IM_COL32(0, 220, 140, 255) : IM_COL32(220, 220, 255, 255);
    if (!is_selected && lens_col != 0) {
      glow_col = lens_col;
    }
    const float t_now = static_cast<float>(ImGui::GetTime());
    const float twinkle_phase =
        hash01_from_u64(static_cast<std::uint64_t>(n.id) * 0x9E3779B97F4A7C15ULL) * 6.2831853f;
    const float twinkle = 0.78f + 0.22f * std::sin(t_now * 1.75f + twinkle_phase);
    const float neb_boost = n.sys ? static_cast<float>(std::clamp(n.sys->nebula_density, 0.0, 1.0)) * 0.20f : 0.0f;
    const float glow_outer_alpha = (is_selected ? 0.12f : 0.08f) * std::clamp(twinkle + neb_boost, 0.0f, 1.6f);
    const float glow_inner_alpha =
        (is_selected ? 0.22f : 0.14f) * std::clamp(0.9f + 0.35f * twinkle + neb_boost, 0.0f, 1.8f);
    const float glow_outer_r = base_r * (2.35f + 0.35f * twinkle);
    const float glow_inner_r = base_r * (1.55f + 0.22f * twinkle);
    draw->AddCircleFilled(n.p, glow_outer_r, modulate_alpha(glow_col, glow_outer_alpha), 0);
    draw->AddCircleFilled(n.p, glow_inner_r, modulate_alpha(glow_col, glow_inner_alpha), 0);

    draw->AddCircleFilled(n.p, base_r, fill);
    draw->AddCircle(n.p, base_r, outline, 0, 1.5f);

    if (is_hovered) {
      draw->AddCircle(n.p, base_r + 8.0f, IM_COL32(255, 255, 255, 140), 0, 2.0f);
    }

    // Pinned system marker (player-authored intel note).
    if (ui.show_galaxy_pins && is_pinned) {
      const float off = base_r + 8.0f;
      const float d = 4.25f;
      const ImVec2 c(n.p.x, n.p.y - off);
      ImVec2 pts[4] = {ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y)};
      ImVec2 sh[4] = {ImVec2(pts[0].x + 1.0f, pts[0].y + 1.0f), ImVec2(pts[1].x + 1.0f, pts[1].y + 1.0f),
                     ImVec2(pts[2].x + 1.0f, pts[2].y + 1.0f), ImVec2(pts[3].x + 1.0f, pts[3].y + 1.0f)};
      const float a = is_selected ? 0.95f : 0.75f;
      const ImU32 col = modulate_alpha(IM_COL32(255, 225, 140, 255), a);
      draw->AddConvexPolyFilled(sh, 4, modulate_alpha(IM_COL32(0, 0, 0, 200), a));
      draw->AddConvexPolyFilled(pts, 4, col);
      draw->AddPolyline(pts, 4, modulate_alpha(IM_COL32(0, 0, 0, 220), a), true, 1.0f);
    }

    // Highlight the selected fleet's leader system.
    if (selected_fleet_system != kInvalidId && n.id == selected_fleet_system) {
      draw->AddCircle(n.p, base_r + 6.0f, IM_COL32(0, 160, 255, 200), 0, 2.0f);
    }

    // Unknown-exit hint ring.
    if (ui.show_galaxy_unknown_exits) {
      auto it = unknown_exits.find(n.id);
      if (it != unknown_exits.end() && it->second > 0) {
        draw->AddCircle(n.p, base_r + 4.0f, IM_COL32(255, 180, 0, 200), 0, 2.0f);
      }
    }

    // Chokepoint ring (articulation point in the visible jump graph).
    if (ui.show_galaxy_chokepoints && !chokepoints.empty() && zoom >= 0.45) {
      if (chokepoints.find(n.id) != chokepoints.end()) {
        draw->AddCircle(n.p, base_r + 10.0f, IM_COL32(190, 120, 255, 200), 0, 2.0f);
      }
    }

    // Intel-alert ring (recent hostile contacts in the system).
    if (ui.show_galaxy_intel_alerts) {
      auto it = recent_contact_count.find(n.id);
      if (it != recent_contact_count.end() && it->second > 0) {
        const float t = (float)ImGui::GetTime();
        const float pulse = 0.5f + 0.5f * std::sin(t * 2.25f + (float)((n.id & 0x3FF) * 0.01f));
        const float r = base_r + 7.0f + pulse * 2.5f;
        float a = 0.28f + 0.55f * pulse;
        // Scale visibility slightly with the number of contacts.
        a = std::min(1.0f, a + 0.07f * std::logf((float)it->second + 1.0f));
        const ImU32 col0 = modulate_alpha(IM_COL32(255, 90, 90, 255), a);
        const ImU32 col1 = modulate_alpha(IM_COL32(255, 180, 120, 255), a * 0.45f);
        draw->AddCircle(n.p, r, col0, 0, 2.0f);
        draw->AddCircle(n.p, r + 3.0f, col1, 0, 1.0f);
      }
    }

    // Fuel range reachability overlay ring (selected ship/fleet).
    if (fuel_overlay.enabled) {
      const bool restrict = ui.fog_of_war;
      std::optional<JumpRoutePlan> plan;
      if (fuel_overlay.fleet_mode) {
        plan = sim.plan_jump_route_for_fleet(fuel_overlay.fleet_id, n.id, restrict, false);
      } else {
        plan = sim.plan_jump_route_for_ship(fuel_overlay.ship_id, n.id, restrict, false);
      }
      if (plan) {
        const double d_mkm = plan->distance_mkm;
        const float r = base_r + 12.0f;
        if (d_mkm <= fuel_overlay.range_now_mkm + 1e-6) {
          draw->AddCircle(n.p, r, IM_COL32(90, 255, 150, 200), 0, 2.0f);
        } else if (d_mkm <= fuel_overlay.range_full_mkm + 1e-6) {
          draw->AddCircle(n.p, r, IM_COL32(255, 220, 90, 200), 0, 2.0f);
        }
      }
    }

    if (ui.show_galaxy_labels && n.sys) {
      float pr = 10.0f;
      if (is_selected) pr += 1000.0f;
      if (is_hovered) pr += 900.0f;
      if (selected_fleet_system != kInvalidId && n.id == selected_fleet_system) pr += 800.0f;
      if (ui.show_galaxy_pins && is_pinned) pr += 550.0f;
      if (colonized_systems.find(n.id) != colonized_systems.end()) pr += 300.0f;
      if (ui.show_galaxy_chokepoints && !chokepoints.empty() && chokepoints.find(n.id) != chokepoints.end()) pr += 120.0f;
      auto itc = recent_contact_count.find(n.id);
      if (ui.show_galaxy_intel_alerts && itc != recent_contact_count.end() && itc->second > 0) pr += 240.0f;

      // When very zoomed-out, keep only high-signal labels unless the user holds Alt.
      if (declutter_labels && zoom < 0.45f && pr < 260.0f) {
        // skip
      } else {
        ImU32 col = IM_COL32(220, 220, 220, 255);
        if (is_selected) col = IM_COL32(190, 255, 220, 255);
        else if (is_hovered) col = IM_COL32(255, 255, 255, 255);
        else if (ui.show_galaxy_pins && is_pinned) col = IM_COL32(255, 245, 210, 255);
        else if (colonized_systems.find(n.id) != colonized_systems.end()) col = IM_COL32(225, 235, 255, 255);

        SystemLabelCandidate c;
        c.id = n.id;
        c.anchor = n.p;
        c.dx = base_r + 4.0f;
        c.dy = base_r + 4.0f;
        c.text = n.sys->name.c_str();
        c.col = col;
        c.priority = pr;
        label_cands.push_back(c);
      }
    }
  }

  // Draw labels after nodes so they remain legible over overlays.
  if (ui.show_galaxy_labels && !label_cands.empty()) {
    const ImVec2 vmin = origin;
    const ImVec2 vmax = ImVec2(origin.x + avail.x, origin.y + avail.y);

    // Label budget scales with viewport area and zoom (prevents visual overload).
    const float area = std::max(1.0f, avail.x * avail.y);
    const float z = std::clamp(static_cast<float>(zoom), 0.35f, 2.5f);
    const int budget = static_cast<int>(std::clamp((area / (160.0f * 40.0f)) * (0.70f + 0.90f * z), 20.0f, 900.0f));

    std::stable_sort(label_cands.begin(), label_cands.end(), [](const SystemLabelCandidate& a, const SystemLabelCandidate& b) {
      if (a.priority != b.priority) return a.priority > b.priority;
      return a.id < b.id;
    });

    if (!declutter_labels) {
      for (const auto& c : label_cands) {
        const ImVec2 sz = ImGui::CalcTextSize(c.text);
        const ImVec2 pos(c.anchor.x + c.dx, c.anchor.y - c.dy);
        // Cheap viewport rejection (still allow slightly outside to avoid popping).
        if (pos.x > vmax.x || pos.y > vmax.y) continue;
        if (pos.x + sz.x < vmin.x || pos.y + sz.y < vmin.y) continue;
        draw->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 170), c.text);
        draw->AddText(pos, c.col, c.text);
      }
    } else {
      LabelPlacer placer(vmin, vmax, 96.0f);
      int placed = 0;
      for (const auto& c : label_cands) {
        if (placed >= budget && c.priority < 850.0f) break;
        const ImVec2 sz = ImGui::CalcTextSize(c.text);
        ImVec2 pos;
        if (!placer.place_near(c.anchor, c.dx, c.dy, sz, 2.0f, /*preferred_quadrant=*/0, &pos)) continue;
        draw->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 170), c.text);
        draw->AddText(pos, c.col, c.text);
        ++placed;
      }
    }
  }

  // Click interaction:
  // - Left click selects a system.
  // - Right click routes selected ship to the target system (Shift queues).
  // - Alt + Right click smart-routes selected ship (adds refuel stops; Shift queues).
  // - Ctrl + right click routes selected fleet to the target system (Shift queues).
  // - Ctrl + Shift + Alt + right click smart-routes selected fleet (adds refuel stops).
  // - Ctrl + Alt + right click toggles the hovered system as a waypoint in the
  //   selected fleet's patrol circuit.
  // Route ruler: hold D and click two systems to keep a persistent planning overlay.
  // This consumes the click so we don't disturb normal selection (especially selected_ship).

  // Trade lane pick/inspect: hold T and click a lane to pin it (right click clears).
  //
  // The lane tooltip also exposes a pin button; the hotkey is just a fast path.
  if (trade_pick_mode) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered()) {
      ui.galaxy_trade_pinned_from = kInvalidId;
      ui.galaxy_trade_pinned_to = kInvalidId;
      trade_consumed_right = true;
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && trade_lane_hover.lane && !ImGui::IsAnyItemHovered()) {
      const Id from = trade_lane_hover.lane->from_system_id;
      const Id to = trade_lane_hover.lane->to_system_id;
      if (ui.galaxy_trade_pinned_from == from && ui.galaxy_trade_pinned_to == to) {
        ui.galaxy_trade_pinned_from = kInvalidId;
        ui.galaxy_trade_pinned_to = kInvalidId;
      } else {
        ui.galaxy_trade_pinned_from = from;
        ui.galaxy_trade_pinned_to = to;
      }
      trade_consumed_left = true;
    }
  }

  if (route_ruler_mode) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered()) {
      route_ruler = RouteRulerState{};
      ruler_consumed_right = true;
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_system != kInvalidId && !ImGui::IsAnyItemHovered()) {
      if (route_ruler.a == kInvalidId || route_ruler.b != kInvalidId) {
        route_ruler.a = hovered_system;
        route_ruler.b = kInvalidId;
      } else {
        route_ruler.b = hovered_system;
        if (route_ruler.b == route_ruler.a) route_ruler.b = kInvalidId;
      }
      ruler_consumed_left = true;
    }
  }

  if (hovered && !over_minimap && !over_legend && !ruler_consumed_left && !trade_consumed_left &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (mouse.x >= origin.x && mouse.x <= origin.x + avail.x && mouse.y >= origin.y && mouse.y <= origin.y + avail.y) {
      if (hovered_system != kInvalidId) {
        s.selected_system = hovered_system;

        // If we have a selected ship that isn't in this system, deselect it.
        if (selected_ship != kInvalidId) {
          const auto* sh = find_ptr(s.ships, selected_ship);
          if (!sh || sh->system_id != hovered_system) selected_ship = kInvalidId;
        }
      }
    }
  }

  if (hovered && !over_minimap && !over_legend && !ruler_consumed_right && !trade_consumed_right &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    if (mouse.x >= origin.x && mouse.x <= origin.x + avail.x && mouse.y >= origin.y && mouse.y <= origin.y + avail.y) {
      if (hovered_system != kInvalidId) {
        // Ctrl + right click: route selected fleet.
        const bool fleet_mode = ImGui::GetIO().KeyCtrl && selected_fleet != nullptr;

        if (fleet_mode) {
          // Ctrl + Shift + Alt + right click: smart-route selected fleet (fuel-aware, adds refuel stops).
          // Ctrl + Alt + right click (without Shift): edit selected fleet's patrol circuit waypoints.
          const bool smart_fleet_travel = ImGui::GetIO().KeyAlt && ImGui::GetIO().KeyShift;
          if (smart_fleet_travel) {
            // NOTE: Shift is part of the chord here, so we treat smart fleet travel as non-queued.
            sim.clear_fleet_orders(selected_fleet->id);

            // In fog-of-war mode, only allow routing through systems the faction already knows.
            const bool restrict = ui.fog_of_war;
            if (!sim.issue_fleet_travel_to_system_smart(selected_fleet->id, hovered_system, restrict)) {
              nebula4x::log::warn("No known jump route to that system.");
            }
          } else if (ImGui::GetIO().KeyAlt) {
            Fleet* fl_mut = find_ptr(s.fleets, selected_fleet->id);
            if (fl_mut && (viewer_faction_id == kInvalidId || fl_mut->faction_id == viewer_faction_id)) {
              // Avoid FoW leaks: only allow adding known systems.
              if (ui.fog_of_war && viewer_faction_id != kInvalidId &&
                  !sim.is_system_discovered_by_faction(viewer_faction_id, hovered_system)) {
                nebula4x::log::warn("That system has not been discovered.");
              } else {
                if (fl_mut->mission.type != FleetMissionType::PatrolCircuit) {
                  fl_mut->mission.type = FleetMissionType::PatrolCircuit;
                  fl_mut->mission.patrol_circuit_system_ids.clear();
                  if (selected_fleet_system != kInvalidId && selected_fleet_system != hovered_system &&
                      (!ui.fog_of_war ||
                       sim.is_system_discovered_by_faction(fl_mut->faction_id, selected_fleet_system))) {
                    fl_mut->mission.patrol_circuit_system_ids.push_back(selected_fleet_system);
                  }
                  if (fl_mut->mission.patrol_dwell_days <= 0) fl_mut->mission.patrol_dwell_days = 3;
                }

                auto& wps = fl_mut->mission.patrol_circuit_system_ids;
                auto it = std::find(wps.begin(), wps.end(), hovered_system);
                if (it != wps.end()) {
                  wps.erase(it);
                  nebula4x::log::info("Patrol circuit: removed waypoint.");
                } else {
                  wps.push_back(hovered_system);
                  nebula4x::log::info("Patrol circuit: added waypoint.");
                }
                fl_mut->mission.patrol_leg_index = 0;
              }
            }

            // Still treat the click as a selection.
            s.selected_system = hovered_system;
          } else {
            const bool queue_orders = ImGui::GetIO().KeyShift;
            if (!queue_orders) sim.clear_fleet_orders(selected_fleet->id);

            // In fog-of-war mode, only allow routing through systems the faction already knows.
            const bool restrict = ui.fog_of_war;
            if (!sim.issue_fleet_travel_to_system(selected_fleet->id, hovered_system, restrict)) {
              nebula4x::log::warn("No known jump route to that system.");
            }
          }
        } else if (selected_ship != kInvalidId) {
          // Route the selected ship to the target system.
          const bool queue_orders = ImGui::GetIO().KeyShift;
          if (!queue_orders) sim.clear_orders(selected_ship);

          // In fog-of-war mode, only allow routing through systems the faction already knows.
          const bool restrict = ui.fog_of_war;
          const bool smart_ship_travel = ImGui::GetIO().KeyAlt;
          const bool ok = smart_ship_travel
                            ? sim.issue_travel_to_system_smart(selected_ship, hovered_system, restrict)
                            : sim.issue_travel_to_system(selected_ship, hovered_system, restrict);
          if (!ok) {
            nebula4x::log::warn("No known jump route to that system.");
          }
        } else {
          // No ship selected: treat right-click as a select.
          s.selected_system = hovered_system;
        }
      }
    }
  }

  // Region labels.
  if (ui.show_galaxy_regions && ui.show_galaxy_region_labels && zoom >= 0.55f && !reg_label_sum.empty()) {
    for (const auto& [rid, sum] : reg_label_sum) {
      if (ui.selected_region_id != kInvalidId && ui.galaxy_region_dim_nonselected && rid != ui.selected_region_id) {
        continue;
      }
      const int cnt = reg_label_count[rid];
      if (cnt < 2) continue;
      const auto* reg = find_ptr(s.regions, rid);
      if (!reg) continue;

      ImVec2 p{sum.x / (float)cnt, sum.y / (float)cnt};
      const ImVec2 ts = ImGui::CalcTextSize(reg->name.c_str());
      p.x -= ts.x * 0.5f;
      p.y -= ts.y * 0.5f;

      const float a = (ui.selected_region_id != kInvalidId && rid == ui.selected_region_id) ? 0.80f : 0.55f;
      draw->AddText(p, region_col(rid, a), reg->name.c_str());
    }
  }

  // Tooltip for hovered system.
  if (hovered_system != kInvalidId && hovered) {
    const auto* sys = find_ptr(s.systems, hovered_system);
    if (sys) {
      int friendly = 0;
      int total_ships = static_cast<int>(sys->ships.size());
      if (viewer_faction_id != kInvalidId) {
        for (Id sid : sys->ships) {
          const auto* sh = find_ptr(s.ships, sid);
          if (sh && sh->faction_id == viewer_faction_id) friendly++;
        }
      }

      int detected_hostiles = 0;
      int contact_count = 0;
      if (viewer_faction_id != kInvalidId) {
        detected_hostiles = (int)sim.detected_hostile_ships_in_system(viewer_faction_id, sys->id).size();
        contact_count = (int)sim.recent_contacts_in_system(viewer_faction_id, sys->id, ui.contact_max_age_days).size();
      }

      const int unknown_exits_count = unknown_exits.count(sys->id) ? unknown_exits[sys->id] : 0;
      int jump_degree_total = 0;
      int jump_degree_known = 0;
      int region_border_links = 0;
      std::unordered_set<Id> degree_seen;
      degree_seen.reserve(sys->jump_points.size() * 2 + 4);

      for (Id jid : sys->jump_points) {
        const auto* jp = find_ptr(s.jump_points, jid);
        if (!jp) continue;
        const auto* dest_jp = find_ptr(s.jump_points, jp->linked_jump_id);
        if (!dest_jp) continue;
        const auto* dest_sys = find_ptr(s.systems, dest_jp->system_id);
        if (!dest_sys) continue;
        if (!degree_seen.insert(dest_sys->id).second) continue;

        ++jump_degree_total;

        if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
          if (sim.is_jump_point_surveyed_by_faction(viewer_faction_id, jid) &&
              sim.is_system_discovered_by_faction(viewer_faction_id, dest_sys->id)) {
            ++jump_degree_known;
          }
        } else {
          ++jump_degree_known;
        }

        if (sys->region_id != kInvalidId && dest_sys->region_id != kInvalidId && sys->region_id != dest_sys->region_id) {
          ++region_border_links;
        }
      }

      const bool is_strategic_chokepoint =
          (ui.show_galaxy_chokepoints && !chokepoints.empty() && chokepoints.find(sys->id) != chokepoints.end());

      ImGui::BeginTooltip();
      ImGui::Text("%s", sys->name.c_str());
      ImGui::Separator();

      // Current ProcGen lens value for this system (if enabled).
      if (lens_active && lens_bounds_valid && lens_mode != ProcGenLensMode::Off) {
        const double raw_lens = procgen_lens_value(s, *sys, lens_mode);
        if (std::isfinite(raw_lens)) {
          const char* unit = procgen_lens_value_unit(lens_mode);
          const double sc = procgen_lens_display_scale(lens_mode);
          const int dec = procgen_lens_display_decimals(lens_mode);
          const double disp = raw_lens * sc;
          if (dec <= 0) {
            ImGui::TextDisabled("%s: %.0f %s", procgen_lens_mode_label(lens_mode), disp, unit);
          } else if (dec == 1) {
            ImGui::TextDisabled("%s: %.1f %s", procgen_lens_mode_label(lens_mode), disp, unit);
          } else if (dec == 2) {
            ImGui::TextDisabled("%s: %.2f %s", procgen_lens_mode_label(lens_mode), disp, unit);
          } else {
            ImGui::TextDisabled("%s: %.3f %s", procgen_lens_mode_label(lens_mode), disp, unit);
          }
        }
      }

      const double neb = std::clamp(sys->nebula_density, 0.0, 1.0);
      if (neb > 0.01) {
        ImGui::Text("Nebula density: %.0f%%", neb * 100.0);
      } else {
        ImGui::TextDisabled("Nebula density: none");
      }

      const double env = sim.system_sensor_environment_multiplier(sys->id);
      if (env < 0.999) ImGui::TextDisabled("Sensor env: x%.2f", env);

      const double speed_env = sim.system_movement_speed_multiplier(sys->id);
      if (speed_env < 0.999) ImGui::TextDisabled("Speed env: x%.2f", speed_env);

      if (sim.system_has_storm(sys->id)) {
        const double cur = sim.system_storm_intensity(sys->id);
        const double peak = std::clamp(sys->storm_peak_intensity, 0.0, 1.0);
        const std::int64_t now = s.date.days_since_epoch();
        const int days_left = (sys->storm_end_day > now) ? static_cast<int>(sys->storm_end_day - now) : 0;
        ImGui::Text("Nebula storm: %.0f%% (peak %.0f%%)", cur * 100.0, peak * 100.0);
        ImGui::TextDisabled("Ends in %d days", days_left);
      }

      if (sys->region_id != kInvalidId) {
        if (const auto* reg = find_ptr(s.regions, sys->region_id)) {
          ImGui::TextDisabled("Region: %s", reg->name.c_str());
          if (!reg->theme.empty()) ImGui::TextDisabled("Theme: %s", reg->theme.c_str());
        }
      }

      if (trade_net && (ui.show_galaxy_trade_lanes || ui.show_galaxy_trade_hubs)) {
        const TradeNode* tnode = nullptr;
        for (const auto& n : trade_net->nodes) {
          if (n.system_id == sys->id) {
            tnode = &n;
            break;
          }
        }
        if (tnode) {
          auto top_balance = [&](bool want_export) {
            std::array<int, kTradeGoodKindCount> idxs{};
            for (int i = 0; i < kTradeGoodKindCount; ++i) idxs[i] = i;
            std::sort(idxs.begin(), idxs.end(), [&](int a, int b) {
              const double va = tnode->balance[a];
              const double vb = tnode->balance[b];
              if (want_export) {
                if (va > vb + 1e-12) return true;
                if (vb > va + 1e-12) return false;
              } else {
                if (-va > -vb + 1e-12) return true;
                if (-vb > -va + 1e-12) return false;
              }
              return a < b;
            });
            std::string out;
            int shown = 0;
            for (int k : idxs) {
              const double v = tnode->balance[k];
              if (want_export && v <= 1e-6) continue;
              if (!want_export && v >= -1e-6) continue;
              if (shown > 0) out += ", ";
              out += trade_good_kind_label(static_cast<TradeGoodKind>(k));
              ++shown;
              if (shown >= 2) break;
            }
            if (out.empty()) out = want_export ? "None" : "None";
            return out;
          };

          ImGui::Separator();
          ImGui::Text("Trade market");
          ImGui::TextDisabled("Market size: %.2f  |  Hub: %.2f", tnode->market_size, tnode->hub_score);

          // Additional context: trade exposure + effective piracy risk at this node.
          const double eff_risk = system_piracy_risk_effective(sim, s, sys->id);
          double exposure = 0.0;
          for (const auto* ln : trade_lanes_visible) {
            if (!ln) continue;
            if (ln->from_system_id == sys->id || ln->to_system_id == sys->id) {
              exposure += std::max(0.0, ln->total_volume);
            }
          }
          if (exposure > 1e-6) {
            ImGui::TextDisabled("Exposure: %.2f  |  Piracy risk (eff): %.0f%%", exposure, eff_risk * 100.0);
          } else {
            ImGui::TextDisabled("Piracy risk (eff): %.0f%%", eff_risk * 100.0);
          }

          ImGui::Text("Exports: %s", top_balance(true).c_str());
          ImGui::Text("Imports: %s", top_balance(false).c_str());
        }
      }

      if (ImGui::SmallButton("Select")) {
        s.selected_system = hovered_system;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("View system map")) {
        s.selected_system = hovered_system;
        ui.request_map_tab = MapTab::System;

        // If the current selected ship isn't in that system, deselect it.
        if (selected_ship != kInvalidId) {
          const auto* sh = find_ptr(s.ships, selected_ship);
          if (!sh || sh->system_id != hovered_system) selected_ship = kInvalidId;
        }
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Intel")) {
        s.selected_system = hovered_system;
        ui.show_intel_window = true;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Center")) {
        const Vec2 rel = sys->galaxy_pos - world_center;
        pan = Vec2{-rel.x, -rel.y};
      }
      ImGui::Separator();
      ImGui::Text("Pos: (%.2f, %.2f)", sys->galaxy_pos.x, sys->galaxy_pos.y);
      if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
        ImGui::Text("Jump links: %d known / %d total", jump_degree_known, jump_degree_total);
      } else {
        ImGui::Text("Jump links: %d", jump_degree_total);
      }
      if (region_border_links > 0) {
        ImGui::TextDisabled("Region-border links: %d", region_border_links);
      }
      if (is_strategic_chokepoint) {
        ImGui::TextColored(ImVec4(0.86f, 0.62f, 1.00f, 1.0f), "Strategic chokepoint");
      }
      ImGui::Text("Ships: %d", total_ships);
      if (viewer_faction_id != kInvalidId) {
        ImGui::Text("Friendly ships: %d", friendly);
        if (ui.fog_of_war) {
          ImGui::Text("Detected hostiles: %d", detected_hostiles);
          ImGui::Text("Recent contacts: %d", contact_count);
          ImGui::Text("Unknown exits (unsurveyed / undiscovered): %d", unknown_exits_count);
        }
      }

      // Route preview details (when a ship/fleet is selected).
      if (preview_route && !preview_route->systems.empty() && preview_route->systems.back() == sys->id) {
        ImGui::Separator();
        ImGui::Text("%s route preview%s:", preview_is_fleet ? "Fleet" : "Ship",
                    preview_from_queue ? " (queued)" : "");
        ImGui::Text("Jumps: %d", (int)preview_route->jump_ids.size());
        ImGui::Text("Distance: %.1f mkm", preview_route->distance_mkm);
        if (std::isfinite(preview_route->eta_days)) {
          ImGui::Text("ETA: %.1f days", preview_route->eta_days);
        } else {
          ImGui::TextDisabled("ETA: n/a");
        }

        // Fuel estimate (simple: fuel burn is proportional to jump-route distance).
        // Note: the Shift "queued" preview starts after the current queued jump chain;
        // we do not currently project fuel burn for the queued segment.
        const double preview_dist_mkm = preview_route->distance_mkm;
        if (preview_dist_mkm > 0.0) {
          if (!preview_is_fleet && selected_ship != kInvalidId) {
            if (const auto* sh = find_ptr(s.ships, selected_ship)) {
              if (const auto* d = sim.find_design(sh->design_id)) {
                const double cap = std::max(0.0, d->fuel_capacity_tons);
                const double burn = std::max(0.0, d->fuel_use_per_mkm);
                if (cap > 0.0 && burn > 0.0) {
                  double fuel = sh->fuel_tons;
                  if (!std::isfinite(fuel) || fuel < 0.0) {
                    fuel = cap;
                  }
                  fuel = std::clamp(fuel, 0.0, cap);

                  const double need = preview_dist_mkm * burn;
                  ImGui::Text("Fuel: %.1f / %.1f t", fuel, cap);
                  ImGui::Text("Fuel needed: %.1f t", need);
                  if (!preview_from_queue) {
                    const double rem = fuel - need;
                    if (rem >= -1e-6) {
                      ImGui::TextDisabled("On arrival: %.1f t", rem);
                    } else {
                      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                         "INSUFFICIENT FUEL (short %.1f t)", -rem);
                    }
                  } else {
                    ImGui::TextDisabled("Fuel check doesn't project queued fuel burn.");
                  }
                }
              }
            }
          } else if (preview_is_fleet && selected_fleet != nullptr) {
            int total = (int)selected_fleet->ship_ids.size();
            int ok = 0;
            int unknown_ships = 0;
            double worst_short = 0.0;
            double min_remaining = std::numeric_limits<double>::infinity();

            for (Id ship_id : selected_fleet->ship_ids) {
              const auto* sh = find_ptr(s.ships, ship_id);
              if (!sh) {
                unknown_ships++;
                continue;
              }
              const auto* d = sim.find_design(sh->design_id);
              if (!d) {
                unknown_ships++;
                continue;
              }
              const double cap = std::max(0.0, d->fuel_capacity_tons);
              const double burn = std::max(0.0, d->fuel_use_per_mkm);
              if (cap <= 0.0 || burn <= 0.0) {
                ok++;
                continue;
              }

              double fuel = sh->fuel_tons;
              if (!std::isfinite(fuel) || fuel < 0.0) {
                fuel = cap;
              }
              fuel = std::clamp(fuel, 0.0, cap);

              const double need = preview_dist_mkm * burn;
              const double rem = fuel - need;
              min_remaining = std::min(min_remaining, rem);
              if (rem >= -1e-6) {
                ok++;
              } else {
                worst_short = std::max(worst_short, -rem);
              }
            }

            ImGui::Text("Fleet fuel: %d/%d ships OK", ok, total);
            if (unknown_ships > 0) {
              ImGui::TextDisabled("%d ships: unknown fuel model", unknown_ships);
            }

            if (!preview_from_queue && unknown_ships == 0) {
              if (ok == total) {
                if (std::isfinite(min_remaining)) {
                  ImGui::TextDisabled("Worst arrival fuel: %.1f t", min_remaining);
                }
              } else {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                   "%d ships will run out of fuel (worst short %.1f t)", total - ok, worst_short);
              }
            } else if (preview_from_queue) {
              ImGui::TextDisabled("Fuel check doesn't project queued fuel burn.");
            }
          }
        }

        std::string route;
        route.reserve(preview_route->systems.size() * 24);
        for (std::size_t i = 0; i < preview_route->systems.size(); ++i) {
          const Id sid = preview_route->systems[i];
          const auto* rsys = find_ptr(s.systems, sid);
          const char* name = rsys ? rsys->name.c_str() : "?";
          if (i > 0) route += " -> ";
          route += name;
        }
        ImGui::TextWrapped("%s", route.c_str());
      }
      ImGui::EndTooltip();
    }
  }


  // Tooltip for hovered trade lane (procedural economy overlay).
  //
  // Only shows when not hovering a system to avoid fighting with the system tooltip.
  if (hovered && hovered_system == kInvalidId && trade_lane_hover.lane &&
      (ui.show_galaxy_trade_lanes || ui.show_galaxy_trade_hubs) && !ImGui::IsAnyItemHovered()) {
    const TradeLane& lane = *trade_lane_hover.lane;

    const auto* a = find_ptr(s.systems, lane.from_system_id);
    const auto* b = find_ptr(s.systems, lane.to_system_id);
    const char* an = a ? a->name.c_str() : "?";
    const char* bn = b ? b->name.c_str() : "?";

    const TradeGoodKind dom = lane.top_flows.empty() ? TradeGoodKind::RawMetals : lane.top_flows.front().good;
    const double vol = std::max(0.0, lane.total_volume);
    const double r0 = system_piracy_risk_effective(sim, s, lane.from_system_id);
    const double r1 = system_piracy_risk_effective(sim, s, lane.to_system_id);
    const double ravg = 0.5 * (r0 + r1);

    const double risk_w = std::max(0.0, sim.cfg().ai_trade_security_patrol_risk_weight);
    const double base_need = 0.20;
    const double sec_score = vol * (base_need + risk_w * ravg);

    const bool pinned = (ui.galaxy_trade_pinned_from == lane.from_system_id &&
                         ui.galaxy_trade_pinned_to == lane.to_system_id);

    ImGui::BeginTooltip();
    ImGui::Text("Trade lane");
    ImGui::Separator();
    ImGui::Text("%s  ->  %s", an, bn);
    ImGui::TextDisabled("Volume: %.2f", vol);
    ImGui::TextDisabled("Dominant commodity: %s", trade_good_kind_label(dom));

    if (!lane.top_flows.empty() && vol > 1e-9) {
      ImGui::Separator();
      ImGui::Text("Top flows");
      for (const auto& f : lane.top_flows) {
        if (f.volume <= 1e-9) continue;
        const double pct = std::clamp(f.volume / vol, 0.0, 1.0) * 100.0;
        ImGui::Text("%s: %.2f (%.0f%%)", trade_good_kind_label(f.good), f.volume, pct);
      }
    }

    ImGui::Separator();
    ImGui::Text("Security (piracy)");
    ImGui::TextDisabled("Endpoint risk (eff): %.0f%% / %.0f%%", r0 * 100.0, r1 * 100.0);
    ImGui::TextDisabled("Lane avg risk: %.0f%%", ravg * 100.0);
    ImGui::TextDisabled("Score: %.2f  (vol * (0.20 + %.2f*risk))", sec_score, risk_w);

    ImGui::Separator();
    ImGui::PushID("trade_lane_tt");
    if (ImGui::SmallButton(pinned ? "Unpin" : "Pin")) {
      if (pinned) {
        ui.galaxy_trade_pinned_from = kInvalidId;
        ui.galaxy_trade_pinned_to = kInvalidId;
      } else {
        ui.galaxy_trade_pinned_from = lane.from_system_id;
        ui.galaxy_trade_pinned_to = lane.to_system_id;
      }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Filter commodity")) {
      ui.galaxy_trade_good_filter = static_cast<int>(dom);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear filter")) {
      ui.galaxy_trade_good_filter = -1;
    }

    if (ImGui::SmallButton("Select A")) {
      if (lane.from_system_id != kInvalidId) s.selected_system = lane.from_system_id;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Select B")) {
      if (lane.to_system_id != kInvalidId) s.selected_system = lane.to_system_id;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Center")) {
      Vec2 mid{0.0, 0.0};
      int cnt = 0;
      if (a) {
        const Vec2 rel = a->galaxy_pos - world_center;
        mid.x += rel.x;
        mid.y += rel.y;
        cnt++;
      }
      if (b) {
        const Vec2 rel = b->galaxy_pos - world_center;
        mid.x += rel.x;
        mid.y += rel.y;
        cnt++;
      }
      if (cnt > 0) {
        mid.x /= (double)cnt;
        mid.y /= (double)cnt;
        pan = Vec2{-mid.x, -mid.y};
      }
    }

    ImGui::TextDisabled("Tip: hold T + click a lane to pin it.");
    ImGui::PopID();
    ImGui::EndTooltip();
  }


  // ProcGen lens probe (hold Alt) for continuous field visualization.
  // Only shows when not hovering a system (to avoid fighting with the system tooltip).
  if (ui.galaxy_procgen_probe && lens_active && lens_bounds_valid && lens_mode != ProcGenLensMode::Off &&
      ImGui::GetIO().KeyAlt && hovered && mouse_in_rect && !over_minimap && !over_legend &&
      hovered_system == kInvalidId && !lens_sources.empty() && !ImGui::IsAnyItemHovered()) {

    const double spacing = std::max(1e-6, lens_typical_spacing);
    const double max_dist = spacing * 2.35;
    const double max_dist2 = max_dist * max_dist;
    const double soft2 = (spacing * 0.15) * (spacing * 0.15) + 1e-9;

    const Vec2 w = to_world(mouse, center_px, scale, zoom, pan);
    const IdwSample samp = sample_lens_field_idw(w, lens_sources, soft2);

    ImGui::BeginTooltip();
    ImGui::Text("ProcGen probe: %s", procgen_lens_mode_label(lens_mode));
    ImGui::Separator();

    if (!samp.ok || !std::isfinite(samp.min_d2) || samp.min_d2 > max_dist2 || !std::isfinite(samp.value)) {
      ImGui::TextDisabled("No nearby data (out of range)");
    } else {
      // Convert back to raw units for display (inverse of log scaling).
      double raw_est = samp.value;
      if (ui.galaxy_procgen_lens_log_scale && (lens_mode == ProcGenLensMode::MineralWealth)) {
        raw_est = std::pow(10.0, std::max(0.0, samp.value)) - 1.0;
      }

      const char* unit = procgen_lens_value_unit(lens_mode);
      const double sc = procgen_lens_display_scale(lens_mode);
      const int dec = procgen_lens_display_decimals(lens_mode);
      const double disp = raw_est * sc;

      if (dec <= 0) {
        ImGui::Text("Value: %.0f %s", disp, unit);
      } else if (dec == 1) {
        ImGui::Text("Value: %.1f %s", disp, unit);
      } else if (dec == 2) {
        ImGui::Text("Value: %.2f %s", disp, unit);
      } else {
        ImGui::Text("Value: %.3f %s", disp, unit);
      }

      // Nearest visible system (context).
      Id nearest_id = kInvalidId;
      double nearest_d2 = std::numeric_limits<double>::infinity();
      for (const auto& v : visible) {
        const Vec2 rel = v.sys->galaxy_pos - world_center;
        const double dx = w.x - rel.x;
        const double dy = w.y - rel.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 < nearest_d2) {
          nearest_d2 = d2;
          nearest_id = v.id;
        }
      }
      if (nearest_id != kInvalidId) {
        const auto* ns = find_ptr(s.systems, nearest_id);
        const double d = std::sqrt(std::max(0.0, nearest_d2));
        if (ns) {
          ImGui::TextDisabled("Nearest system: %s (%.1f u)", ns->name.c_str(), d);
        } else {
          ImGui::TextDisabled("Nearest system: %.1f u", d);
        }
      }

      const double d_near = std::sqrt(std::max(0.0, samp.min_d2));
      ImGui::TextDisabled("Nearest sample: %.1f u (fade radius %.1f u)", d_near, max_dist);
    }

    ImGui::EndTooltip();
  }

  // Minimap overlay (bottom-right).
  if (minimap_enabled) {
    const ImU32 mm_bg = IM_COL32(8, 8, 10, 200);
    const ImU32 mm_border = over_minimap ? IM_COL32(235, 235, 235, 220) : IM_COL32(90, 90, 95, 220);
    draw->AddRectFilled(mm_p0, mm_p1, mm_bg, 4.0f);
    draw->AddRect(mm_p0, mm_p1, mm_border, 4.0f);

    // Systems.
    for (const auto& v : visible) {
      const Vec2 rel = v.sys->galaxy_pos - world_center;
      ImVec2 p = world_to_minimap_px(mm, rel);
      p = clamp_to_rect(p, mm_p0, mm_p1);
      const bool is_selected = (s.selected_system == v.id);
      const bool is_fleet = (selected_fleet_system != kInvalidId && v.id == selected_fleet_system);
      const float r = is_selected ? 3.3f : (is_fleet ? 2.8f : 2.0f);
      ImU32 col = IM_COL32(200, 200, 210, 200);
      if (!is_selected && lens_active && lens_bounds_valid && lens_mode != ProcGenLensMode::Off) {
        const double raw = procgen_lens_value(s, *v.sys, lens_mode);
        double x = raw;
        if (ui.galaxy_procgen_lens_log_scale && (lens_mode == ProcGenLensMode::MineralWealth)) {
          x = std::log10(std::max(0.0, raw) + 1.0);
        }
        float t = 0.5f;
        if (std::isfinite(x) && lens_max > lens_min + 1e-12) {
          t = static_cast<float>((x - lens_min) / (lens_max - lens_min));
        }
        col = procgen_lens_gradient_color(t, 0.85f);
      }
      if (is_fleet) col = IM_COL32(0, 160, 255, 220);
      if (is_selected) col = IM_COL32(245, 245, 245, 255);
      draw->AddCircleFilled(p, r, col, 0);
    }

    // Viewport rectangle.
    const Vec2 view_tl = to_world(origin, center_px, scale, zoom, pan);
    const Vec2 view_br = to_world(ImVec2(origin.x + avail.x, origin.y + avail.y), center_px, scale, zoom, pan);
    const ImVec2 pv0 = world_to_minimap_px(mm, view_tl);
    const ImVec2 pv1 = world_to_minimap_px(mm, view_br);
    ImVec2 a(std::min(pv0.x, pv1.x), std::min(pv0.y, pv1.y));
    ImVec2 b(std::max(pv0.x, pv1.x), std::max(pv0.y, pv1.y));
    a = clamp_to_rect(a, mm_p0, mm_p1);
    b = clamp_to_rect(b, mm_p0, mm_p1);
    draw->AddRectFilled(a, b, IM_COL32(255, 255, 255, 22));
    draw->AddRect(a, b, IM_COL32(255, 255, 255, 170), 0.0f, 0, 1.5f);

    // Label.
    draw->AddText(ImVec2(mm_p0.x + 6.0f, mm_p0.y + 4.0f), IM_COL32(210, 210, 210, 200), "Minimap (M)");
  }

  // Legend / help
  ImGui::SetCursorScreenPos(legend_p0);
  ImGui::BeginChild("galaxy_legend", legend_size, true);
  ImGui::Text("Galaxy map");
  ImGui::BulletText("Wheel: zoom (to cursor)");
  ImGui::BulletText("Middle drag: pan");
  ImGui::BulletText("Minimap (M): click/drag to pan");
  // Avoid \x escape parsing pitfalls on MSVC (\x consumes all following hex digits).
  ImGui::BulletText("Hold D + click: route ruler (plan A->B)");
  ImGui::BulletText("Hold T + click: pin trade lane (T+right click clears)");
  ImGui::BulletText("R: reset view");
  ImGui::BulletText("B: dust particles");
  ImGui::BulletText("Left click: select system");
  ImGui::BulletText("Right click: route selected ship (Shift queues)");
  ImGui::BulletText("Alt+Right click: smart-route selected ship (adds refuel stops; Shift queues)");
  ImGui::BulletText("Ctrl+Right click: route selected fleet (Shift queues)");
  ImGui::BulletText("Ctrl+Shift+Alt+Right click: smart-route selected fleet (adds refuel stops)");
  ImGui::BulletText("Ctrl+Alt+Right click: edit selected fleet patrol circuit waypoints");
  ImGui::BulletText("Hover: route preview (Shift=queued, Ctrl=fleet, Alt=smart)");

  ImGui::SeparatorText("Visuals");
  ImGui::Checkbox("Starfield", &ui.galaxy_map_starfield);
  ImGui::SameLine();
  ImGui::Checkbox("Dust particles (B)", &ui.galaxy_map_particle_field);
  if (ui.galaxy_map_particle_field) {
    ImGui::TextDisabled("Particles: L%d | Tiles %d | Points %d", ui.map_particle_last_frame_layers_drawn, ui.map_particle_last_frame_tiles_drawn, ui.map_particle_last_frame_particles_drawn);
  }

  // --- System finder, pins & notes ---
  // Keeps everything within the *visible* system set to avoid leaking information under FoW.
  {
    ImGui::SeparatorText("Find");
    static char find_query[96] = "";
    static bool pinned_only = false;
    // Fallback when no viewer faction is available.
    static std::vector<Id> local_favorites;

    // Ctrl+F focuses the search bar.
    static bool focus_find = false;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
      focus_find = true;
    }

    if (focus_find) {
      ImGui::SetKeyboardFocusHere();
      focus_find = false;
    }

    const char* hint = viewer_faction ? "Find: name tokens + #tags (Enter to jump)" : "Find system (name) - Enter to jump";
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter_pressed = ImGui::InputTextWithHint("##galaxy_find", hint, find_query, sizeof(find_query),
                                                       ImGuiInputTextFlags_EnterReturnsTrue);

    // Escape clears.
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      find_query[0] = '\0';
    }

    // Parse query into name terms and tag terms.
    std::vector<std::string> name_terms;
    std::vector<std::string> tag_terms;
    bool wants_pinned = pinned_only;

    std::string q = ascii_trim(std::string(find_query));
    if (!q.empty()) {
      size_t pos = 0;
      while (pos < q.size()) {
        while (pos < q.size() && std::isspace(static_cast<unsigned char>(q[pos]))) ++pos;
        const size_t start = pos;
        while (pos < q.size() && !std::isspace(static_cast<unsigned char>(q[pos]))) ++pos;
        if (start >= pos) break;
        std::string tok = q.substr(start, pos - start);
        if (tok == "pin" || tok == "pinned") {
          wants_pinned = true;
          continue;
        }
        if (!tok.empty() && tok.front() == '#') {
          tok = normalize_tag(std::move(tok));
          if (!tok.empty()) tag_terms.push_back(std::move(tok));
        } else {
          name_terms.push_back(std::move(tok));
        }
      }
    }

    if (viewer_faction) {
      ImGui::Checkbox("Pinned only", &pinned_only);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Filter results to systems you pinned (P) or pinned via Notes.");
      }
    } else {
      pinned_only = false;
    }

    auto is_pinned_id = [&](Id id) -> bool {
      if (!viewer_faction) return false;
      const auto itn = viewer_faction->system_notes.find(id);
      return itn != viewer_faction->system_notes.end() && itn->second.pinned;
    };

    auto note_for_id = [&](Id id) -> const SystemIntelNote* {
      if (!viewer_faction) return nullptr;
      const auto itn = viewer_faction->system_notes.find(id);
      if (itn == viewer_faction->system_notes.end()) return nullptr;
      return &itn->second;
    };

    // Collect matches.
    std::vector<const SysView*> matches;
    matches.reserve(24);
    const bool have_filter = !name_terms.empty() || !tag_terms.empty() || wants_pinned;
    if (have_filter) {
      for (const auto& v : visible) {
        if (!v.sys) continue;

        bool ok = true;
        for (const std::string& term : name_terms) {
          if (!ascii_icontains(v.sys->name, term)) {
            ok = false;
            break;
          }
        }
        if (!ok) continue;

        if (wants_pinned && !is_pinned_id(v.id)) continue;

        if (!tag_terms.empty()) {
          const SystemIntelNote* n = note_for_id(v.id);
          if (!n) continue;
          for (const std::string& tag : tag_terms) {
            if (!note_has_tag(*n, tag)) {
              ok = false;
              break;
            }
          }
          if (!ok) continue;
        }

        matches.push_back(&v);
        if (matches.size() >= 32) break;
      }

      const std::string first_term = name_terms.empty() ? std::string() : name_terms.front();
      std::stable_sort(matches.begin(), matches.end(), [&](const SysView* a, const SysView* b) {
        const bool ap = !first_term.empty() ? ascii_istarts_with(a->sys->name, first_term) : false;
        const bool bp = !first_term.empty() ? ascii_istarts_with(b->sys->name, first_term) : false;
        if (ap != bp) return ap; // Prefix matches first.
        const bool api = is_pinned_id(a->id);
        const bool bpi = is_pinned_id(b->id);
        if (api != bpi) return api; // Pinned next.
        return a->sys->name < b->sys->name;
      });
    }

    auto jump_to_system = [&](Id id, const StarSystem* sys) {
      if (!sys || id == kInvalidId) return;
      s.selected_system = id;
      ui.request_galaxy_map_center = true;
      ui.request_galaxy_map_center_x = sys->galaxy_pos.x;
      ui.request_galaxy_map_center_y = sys->galaxy_pos.y;
    };

    if (enter_pressed && !matches.empty()) {
      jump_to_system(matches.front()->id, matches.front()->sys);
    }

    if (have_filter) {
      if (matches.empty()) {
        ImGui::TextDisabled("No matches");
      } else {
        ImGui::BeginChild("##galaxy_find_results", ImVec2(0.0f, 110.0f), true);
        const int max_show = 12;
        for (int i = 0; i < static_cast<int>(matches.size()) && i < max_show; ++i) {
          const SysView* v = matches[i];
          const bool is_sel = (s.selected_system == v->id);

          std::string label;
          if (viewer_faction && is_pinned_id(v->id)) {
            label += "* ";
          }
          label += v->sys->name;
          label += "##find_" + std::to_string(static_cast<std::uint64_t>(v->id));

          if (ImGui::Selectable(label.c_str(), is_sel)) {
            jump_to_system(v->id, v->sys);
          }
          if (ImGui::IsItemHovered() && viewer_faction) {
            if (const SystemIntelNote* n = note_for_id(v->id); n && (!n->tags.empty() || !n->text.empty())) {
              ImGui::BeginTooltip();
              if (!n->tags.empty()) {
                ImGui::TextDisabled("Tags:");
                ImGui::SameLine();
                for (size_t ti = 0; ti < n->tags.size(); ++ti) {
                  ImGui::TextUnformatted(n->tags[ti].c_str());
                  if (ti + 1 < n->tags.size()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                  }
                }
              }
              if (!n->text.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", n->text.c_str());
              }
              ImGui::EndTooltip();
            }
          }
        }
        ImGui::EndChild();
      }
    }

    // Pins (persisted per-faction) or local favorites fallback.
    if (viewer_faction) {
      ImGui::SeparatorText("Pins");

      // Drop invalid IDs (e.g. after loading another save).
      {
        std::vector<Id> to_erase;
        to_erase.reserve(8);
        for (const auto& kv : viewer_faction->system_notes) {
          if (kv.first == kInvalidId || find_ptr(s.systems, kv.first) == nullptr) {
            to_erase.push_back(kv.first);
          }
        }
        for (Id id : to_erase) {
          viewer_faction->system_notes.erase(id);
        }
      }

      const Id sel = s.selected_system;
      const bool sel_pinned = (sel != kInvalidId) ? is_pinned_id(sel) : false;

      ImGui::BeginDisabled(sel == kInvalidId);
      if (ImGui::SmallButton(sel_pinned ? "Unpin selected" : "Pin selected")) {
        if (sel != kInvalidId) {
          auto& n = viewer_faction->system_notes[sel];
          n.pinned = !sel_pinned;
          if (!n.pinned && n.text.empty() && n.tags.empty()) {
            viewer_faction->system_notes.erase(sel);
          }
        }
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear pins")) {
        std::vector<Id> to_erase;
        to_erase.reserve(viewer_faction->system_notes.size());
        for (auto& kv : viewer_faction->system_notes) {
          kv.second.pinned = false;
          if (kv.second.text.empty() && kv.second.tags.empty()) {
            to_erase.push_back(kv.first);
          }
        }
        for (Id id : to_erase) {
          viewer_faction->system_notes.erase(id);
        }
      }

      // Collect pinned systems.
      std::vector<Id> pins;
      pins.reserve(16);
      for (const auto& kv : viewer_faction->system_notes) {
        if (kv.second.pinned) pins.push_back(kv.first);
      }
      std::stable_sort(pins.begin(), pins.end(), [&](Id a, Id b) {
        const StarSystem* sa = find_ptr(s.systems, a);
        const StarSystem* sb = find_ptr(s.systems, b);
        const std::string an = sa ? sa->name : std::string();
        const std::string bn = sb ? sb->name : std::string();
        if (an != bn) return an < bn;
        return a < b;
      });

      if (pins.empty()) {
        ImGui::TextDisabled("(none)");
      } else {
        ImGui::BeginChild("##galaxy_pins", ImVec2(0.0f, 110.0f), true);
        int remove_idx = -1;
        for (int i = 0; i < static_cast<int>(pins.size()); ++i) {
          const Id id = pins[i];
          const StarSystem* sys = find_ptr(s.systems, id);
          if (!sys) continue;
          const bool can_show = can_show_system(viewer_faction_id, ui.fog_of_war, sim, id);
          const bool is_sel = (s.selected_system == id);
          std::string label = can_show ? sys->name : std::string("(undiscovered)");
          label += "##pin_" + std::to_string(static_cast<std::uint64_t>(id));
          ImGui::BeginDisabled(!can_show);
          if (ImGui::Selectable(label.c_str(), is_sel)) {
            jump_to_system(id, sys);
          }
          ImGui::EndDisabled();

          if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Jump")) {
              if (can_show) jump_to_system(id, sys);
              ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Unpin")) {
              remove_idx = i;
              ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Delete note")) {
              viewer_faction->system_notes.erase(id);
              remove_idx = -2; // sentinel: already handled.
              ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
          }
        }

        if (remove_idx >= 0 && remove_idx < static_cast<int>(pins.size())) {
          const Id id = pins[remove_idx];
          auto itn = viewer_faction->system_notes.find(id);
          if (itn != viewer_faction->system_notes.end()) {
            itn->second.pinned = false;
            if (itn->second.text.empty() && itn->second.tags.empty()) {
              viewer_faction->system_notes.erase(itn);
            }
          }
        }

        ImGui::EndChild();
      }
    } else {
      // Local-only favorites (non-persisted) when no viewer faction is available.
      ImGui::SeparatorText("Favorites (local)");

      // Drop any invalid IDs (e.g. after loading another save).
      local_favorites.erase(std::remove_if(local_favorites.begin(), local_favorites.end(), [&](Id id) {
                            return find_ptr(s.systems, id) == nullptr;
                          }),
                          local_favorites.end());

      ImGui::BeginDisabled(s.selected_system == kInvalidId);
      if (ImGui::SmallButton("Add selected")) {
        const Id id = s.selected_system;
        if (id != kInvalidId && std::find(local_favorites.begin(), local_favorites.end(), id) == local_favorites.end()) {
          local_favorites.push_back(id);
        }
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) {
        local_favorites.clear();
      }

      if (local_favorites.empty()) {
        ImGui::TextDisabled("(none)");
      } else {
        ImGui::BeginChild("##galaxy_favorites", ImVec2(0.0f, 110.0f), true);
        int remove_idx = -1;
        for (int i = 0; i < static_cast<int>(local_favorites.size()); ++i) {
          const Id id = local_favorites[i];
          const StarSystem* sys = find_ptr(s.systems, id);
          if (!sys) continue;
          const bool is_sel = (s.selected_system == id);
          std::string label = sys->name;
          label += "##fav_" + std::to_string(static_cast<std::uint64_t>(id));
          if (ImGui::Selectable(label.c_str(), is_sel)) {
            jump_to_system(id, sys);
          }

          if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove")) {
              remove_idx = i;
              ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
          }
        }
        if (remove_idx >= 0 && remove_idx < static_cast<int>(local_favorites.size())) {
          local_favorites.erase(local_favorites.begin() + remove_idx);
        }
        ImGui::EndChild();
      }
    }

    // Notes for the selected system (persisted per-faction).
    ImGui::SeparatorText("Notes");
    if (!viewer_faction) {
      ImGui::TextDisabled("Notes require a viewer faction (select a ship or set a viewer faction).");
    } else if (s.selected_system == kInvalidId) {
      ImGui::TextDisabled("(select a system)");
    } else {
      const Id sys_id = s.selected_system;
      const StarSystem* sys = find_ptr(s.systems, sys_id);
      if (!sys) {
        ImGui::TextDisabled("(selected system missing)");
      } else {
        SystemIntelNote* note = nullptr;
        if (auto itn = viewer_faction->system_notes.find(sys_id); itn != viewer_faction->system_notes.end()) {
          note = &itn->second;
        }

        // Pinned toggle.
        bool pinned = note ? note->pinned : false;
        if (ImGui::Checkbox("Pinned##sys_pin", &pinned)) {
          auto& n = viewer_faction->system_notes[sys_id];
          n.pinned = pinned;
          note = &n;
        }

        // Tags.
        if (note && !note->tags.empty()) {
          ImGui::TextDisabled("Tags:");
          ImGui::SameLine();
          int remove_tag = -1;
          for (int i = 0; i < static_cast<int>(note->tags.size()); ++i) {
            const std::string& t = note->tags[i];
            ImGui::PushID(i);
            if (ImGui::SmallButton(("#" + t).c_str())) {
              // Quick jump back into search for this tag.
              std::string qq = "#" + t;
              std::snprintf(find_query, sizeof(find_query), "%s", qq.c_str());
              pinned_only = false;
            }
            if (ImGui::BeginPopupContextItem()) {
              if (ImGui::MenuItem("Remove tag")) {
                remove_tag = i;
                ImGui::CloseCurrentPopup();
              }
              ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImGui::PopID();
          }
          ImGui::NewLine();

          if (remove_tag >= 0 && remove_tag < static_cast<int>(note->tags.size())) {
            note->tags.erase(note->tags.begin() + remove_tag);
          }
        }

        static char new_tag[32] = "";
        ImGui::SetNextItemWidth(-1.0f);
        const bool add_enter = ImGui::InputTextWithHint("##note_tag", "Add tag (e.g. home, ruins)", new_tag, sizeof(new_tag),
                                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Tags are case-insensitive. Use #tag in Find.");
        }
        const bool add_clicked = ImGui::SmallButton("Add tag");
        if (add_enter || add_clicked) {
          const std::string tag = normalize_tag(std::string(new_tag));
          if (!tag.empty()) {
            auto& n = viewer_faction->system_notes[sys_id];
            bool exists = false;
            for (const std::string& t : n.tags) {
              if (ascii_iequals(t, tag)) {
                exists = true;
                break;
              }
            }
            if (!exists) {
              n.tags.push_back(tag);
            }
            note = &n;
          }
          new_tag[0] = '\0';
        }

        // Note text editor (stable buffer per selected system).
        static Id note_edit_id = kInvalidId;
        static std::string note_edit_text;
        if (note_edit_id != sys_id) {
          note_edit_id = sys_id;
          note_edit_text = note ? note->text : std::string();
        }

        ImGui::TextDisabled("Text:");
        ImGui::SetNextItemWidth(-1.0f);
        const bool edited = ImGui::InputTextMultiline("##note_text", &note_edit_text, ImVec2(0.0f, 90.0f),
                                                     ImGuiInputTextFlags_AllowTabInput);
        if (edited) {
          auto& n = viewer_faction->system_notes[sys_id];
          n.text = note_edit_text;
          note = &n;
        }

        if (ImGui::SmallButton("Clear text")) {
          note_edit_text.clear();
          if (note) note->text.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete note")) {
          viewer_faction->system_notes.erase(sys_id);
          note = nullptr;
          note_edit_text.clear();
        }

        // Prune empty entries to keep saves tidy.
        if (auto itn = viewer_faction->system_notes.find(sys_id); itn != viewer_faction->system_notes.end()) {
          if (!itn->second.pinned && itn->second.text.empty() && itn->second.tags.empty()) {
            viewer_faction->system_notes.erase(itn);
          }
        }
      }
    }
  }

  ImGui::SeparatorText("Overlays");
  ImGui::TextDisabled("Visual presets");
  if (ImGui::SmallButton("Strategic##gal_preset")) {
    ui.show_galaxy_labels = true;
    ui.show_galaxy_jump_lines = true;
    ui.show_galaxy_chokepoints = true;
    ui.show_galaxy_unknown_exits = true;
    ui.show_galaxy_intel_alerts = true;
    ui.show_galaxy_regions = true;
    ui.show_galaxy_region_boundaries = true;
    ui.galaxy_map_starfield = true;
    ui.galaxy_map_grid = false;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Intel##gal_preset")) {
    ui.fog_of_war = true;
    ui.show_galaxy_labels = true;
    ui.show_galaxy_pins = true;
    ui.show_galaxy_intel_alerts = true;
    ui.show_galaxy_unknown_exits = true;
    ui.show_galaxy_trade_lanes = true;
    ui.galaxy_trade_risk_overlay = true;
    ui.show_galaxy_chokepoints = true;
    ui.show_galaxy_regions = true;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Cinematic##gal_preset")) {
    ui.galaxy_map_starfield = true;
    ui.map_proc_render_engine = true;
    ui.galaxy_map_particle_field = true;
    ui.galaxy_star_atlas_constellations = true;
    ui.galaxy_map_territory_overlay = true;
    ui.show_galaxy_jump_lines = true;
    ui.show_galaxy_labels = false;
    ui.galaxy_map_grid = false;
  }

  ImGui::Checkbox("Starfield", &ui.galaxy_map_starfield);
  ImGui::SameLine();
  ImGui::Checkbox("Grid", &ui.galaxy_map_grid);

  // Custom tile-based procedural background engine (stars + optional haze).
  ImGui::Checkbox("Procedural background (tiles)", &ui.map_proc_render_engine);
  if (ui.map_proc_render_engine) {
    ImGui::Indent();
    ui.map_proc_render_tile_px = std::clamp(ui.map_proc_render_tile_px, 64, 512);
    ui.map_proc_render_cache_tiles = std::clamp(ui.map_proc_render_cache_tiles, 16, 512);

    ImGui::SliderInt("Tile px", &ui.map_proc_render_tile_px, 64, 512);
    ImGui::SliderInt("Cache tiles", &ui.map_proc_render_cache_tiles, 16, 512);
    ImGui::Checkbox("Nebula haze", &ui.map_proc_render_nebula_enable);
    if (ui.map_proc_render_nebula_enable) {
      ImGui::SliderFloat("Nebula strength", &ui.map_proc_render_nebula_strength, 0.0f, 1.0f, "%.2f");
      ImGui::SliderFloat("Nebula scale", &ui.map_proc_render_nebula_scale, 0.25f, 4.0f, "%.2f");
      ImGui::SliderFloat("Nebula warp", &ui.map_proc_render_nebula_warp, 0.0f, 2.0f, "%.2f");
    }
    ImGui::Checkbox("Debug tile bounds", &ui.map_proc_render_debug_tiles);

    const ProcRenderStats* pst = nullptr;
    ProcRenderStats st_local;
    if (proc_engine && proc_engine->ready()) {
      st_local = proc_engine->stats();
      pst = &st_local;
    }

    if (ImGui::SmallButton("Clear cache##proc_bg")) {
      if (proc_engine && proc_engine->ready()) {
        proc_engine->clear();
      } else {
        ui.map_proc_render_clear_cache_requested = true;
      }
    }
    ImGui::SameLine();
    const int cached = pst ? pst->cache_tiles : ui.map_proc_render_stats_cache_tiles;
    const int gen = pst ? pst->generated_this_frame : ui.map_proc_render_stats_generated_this_frame;
    const float gen_ms = pst ? static_cast<float>(pst->gen_ms_this_frame) : ui.map_proc_render_stats_gen_ms_this_frame;
    const float up_ms = pst ? static_cast<float>(pst->upload_ms_this_frame) : ui.map_proc_render_stats_upload_ms_this_frame;
    ImGui::TextDisabled("Cached %d | +%d | Gen %.2fms | Upload %.2fms", cached, gen, gen_ms, up_ms);

    ImGui::Unindent();
  }
  ImGui::Checkbox("Minimap (M)", &ui.galaxy_map_show_minimap);
  ImGui::Checkbox("Selected travel route", &ui.galaxy_map_selected_route);
  {
    const bool have_nav_ref = (selected_ship != kInvalidId) || (selected_fleet != nullptr);
    ImGui::BeginDisabled(!have_nav_ref);
    ImGui::Checkbox("Fuel range (F)", &ui.galaxy_map_fuel_range);
    ImGui::EndDisabled();
    if (ui.galaxy_map_fuel_range) {
      ImGui::TextDisabled("Green: reachable now. Yellow: reachable on full tanks.");
    }
  }
  ImGui::Checkbox("Fog of war", &ui.fog_of_war);
  ImGui::Checkbox("Labels", &ui.show_galaxy_labels);
  ImGui::Checkbox("Pins", &ui.show_galaxy_pins);
  ImGui::Checkbox("Jump links", &ui.show_galaxy_jump_lines);
  ImGui::Checkbox("Chokepoints (articulation)", &ui.show_galaxy_chokepoints);


  ImGui::Checkbox("Territory overlay (Y)", &ui.galaxy_map_territory_overlay);
  if (ui.galaxy_map_territory_overlay) {
    ImGui::Indent();
    ImGui::Checkbox("Fill", &ui.galaxy_map_territory_fill);
    ImGui::SameLine();
    ImGui::Checkbox("Boundaries", &ui.galaxy_map_territory_boundaries);
    ImGui::SliderFloat("Fill opacity", &ui.galaxy_map_territory_fill_opacity, 0.0f, 0.55f, "%.2f");
    ImGui::SliderFloat("Boundary opacity", &ui.galaxy_map_territory_boundary_opacity, 0.0f, 0.85f, "%.2f");
    ImGui::SliderFloat("Boundary thickness px", &ui.galaxy_map_territory_boundary_thickness_px, 0.25f, 8.0f, "%.2f");

    ui.galaxy_map_territory_tile_px = std::clamp(ui.galaxy_map_territory_tile_px, 96, 1024);
    ui.galaxy_map_territory_cache_tiles = std::clamp(ui.galaxy_map_territory_cache_tiles, 8, 20000);
    ui.galaxy_map_territory_samples_per_tile = std::clamp(ui.galaxy_map_territory_samples_per_tile, 8, 128);

    ImGui::SliderInt("Tile px", &ui.galaxy_map_territory_tile_px, 96, 1024);
    ImGui::SliderInt("Cache tiles", &ui.galaxy_map_territory_cache_tiles, 8, 2000);
    ImGui::SliderInt("Samples / tile", &ui.galaxy_map_territory_samples_per_tile, 8, 128);

    ImGui::SeparatorText("Influence model");
    ImGui::SliderFloat("Base radius (x spacing)", &ui.galaxy_map_territory_influence_base_spacing_mult, 0.0f, 6.0f, "%.2f");
    ImGui::SliderFloat("Pop radius (x spacing)", &ui.galaxy_map_territory_influence_pop_spacing_mult, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Pop log bias", &ui.galaxy_map_territory_influence_pop_log_bias, 0.5f, 64.0f, "%.2f");
    ImGui::SliderFloat("Presence falloff (x spacing)", &ui.galaxy_map_territory_presence_falloff_spacing, 0.25f, 10.0f, "%.2f");
    ImGui::SliderFloat("Edge softness (x spacing)", &ui.galaxy_map_territory_dominance_softness_spacing, 0.05f, 8.0f, "%.2f");

    ImGui::SeparatorText("Contested zones");
    ImGui::Checkbox("Stipple contested frontiers", &ui.galaxy_map_territory_contested_dither);
    if (ui.galaxy_map_territory_contested_dither) {
      ImGui::SliderFloat("Contested threshold", &ui.galaxy_map_territory_contested_threshold, 0.0f, 0.75f, "%.2f");
      ImGui::SliderFloat("Stipple strength", &ui.galaxy_map_territory_contested_dither_strength, 0.0f, 1.0f, "%.2f");
    }

    ImGui::Checkbox("Debug tile bounds", &ui.galaxy_map_territory_debug_tiles);

    if (ImGui::SmallButton("Clear cache##territory")) {
      if (territory_engine) {
        territory_engine->clear();
      } else {
        ui.galaxy_map_territory_clear_cache_requested = true;
      }
    }

    const int cached = ui.galaxy_map_territory_stats_cache_tiles;
    const int used = ui.galaxy_map_territory_stats_tiles_used_this_frame;
    const int gen = ui.galaxy_map_territory_stats_tiles_generated_this_frame;
    const int cells = ui.galaxy_map_territory_stats_cells_drawn;
    const float ms = ui.galaxy_map_territory_stats_gen_ms_this_frame;
    ImGui::SameLine();
    ImGui::TextDisabled("Cached %d | Used %d | +%d | Cells %d | Gen %.2fms", cached, used, gen, cells, ms);

    ImGui::Unindent();
  }
  {
    ImGui::Checkbox("Star Atlas constellations", &ui.galaxy_star_atlas_constellations);
    ImGui::SameLine();
    if (ImGui::SmallButton("Open Star Atlas")) ui.show_star_atlas_window = true;
    if (ui.galaxy_star_atlas_constellations) {
      ImGui::Indent();
      ImGui::Checkbox("Constellation labels", &ui.galaxy_star_atlas_labels);
      ImGui::SliderFloat("Line alpha##atlas", &ui.galaxy_star_atlas_alpha, 0.05f, 0.60f, "%.2f");
      ImGui::SliderFloat("Label alpha##atlas", &ui.galaxy_star_atlas_label_alpha, 0.05f, 0.70f, "%.2f");
      ImGui::SliderInt("Cluster size##atlas", &ui.galaxy_star_atlas_target_cluster_size, 4, 18);
      ImGui::SliderInt("Max##atlas", &ui.galaxy_star_atlas_max_constellations, 0, 512);
      ImGui::SliderFloat("Min zoom##atlas", &ui.galaxy_star_atlas_min_zoom, 0.05f, 2.0f, "%.2f");
      ImGui::Unindent();
    }
  }
  ImGui::Checkbox("Unknown exits hint (unsurveyed / undiscovered)", &ui.show_galaxy_unknown_exits);
  ImGui::Checkbox("Intel alerts", &ui.show_galaxy_intel_alerts);
  ImGui::Checkbox("Freight lanes", &ui.show_galaxy_freight_lanes);
  ImGui::TextDisabled("Shows current auto-freight cargo routes (viewer faction).");
  ImGui::Checkbox("Trade lanes", &ui.show_galaxy_trade_lanes);
  ImGui::SameLine();
  ImGui::Checkbox("Hubs##trade", &ui.show_galaxy_trade_hubs);
  ImGui::TextDisabled("Procedural civilian trade lanes. Color indicates the dominant commodity.");

  if (ui.show_galaxy_trade_lanes || ui.show_galaxy_trade_hubs) {
    ImGui::Indent();

    // Commodity filter (dominant good by default; optional inclusion of secondary goods).
    const char* filter_preview = "All";
    if (ui.galaxy_trade_good_filter >= 0 && ui.galaxy_trade_good_filter < kTradeGoodKindCount) {
      filter_preview = trade_good_kind_label(static_cast<TradeGoodKind>(ui.galaxy_trade_good_filter));
    }
    if (ImGui::BeginCombo("Commodity filter##trade", filter_preview)) {
      if (ImGui::Selectable("All", ui.galaxy_trade_good_filter < 0)) {
        ui.galaxy_trade_good_filter = -1;
      }
      ImGui::Separator();
      for (int k = 0; k < kTradeGoodKindCount; ++k) {
        const bool sel = ui.galaxy_trade_good_filter == k;
        if (ImGui::Selectable(trade_good_kind_label(static_cast<TradeGoodKind>(k)), sel)) {
          ui.galaxy_trade_good_filter = k;
        }
      }
      ImGui::EndCombo();
    }

    ImGui::Checkbox("Include secondary goods##trade", &ui.galaxy_trade_filter_include_secondary);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("When filtering by commodity, keep lanes where the commodity appears as a secondary flow too.");
    }

    ImGui::DragFloat("Min lane volume##trade", &ui.galaxy_trade_min_lane_volume, 5.0f, 0.0f, 1.0e6f, "%.1f");
    ui.galaxy_trade_min_lane_volume = std::max(0.0f, ui.galaxy_trade_min_lane_volume);

    ImGui::Checkbox("Risk overlay##trade", &ui.galaxy_trade_risk_overlay);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Draw an additional overlay on lanes based on effective piracy risk (green=safe, red=dangerous).");
    }

    ImGui::Checkbox("Security panel##trade", &ui.galaxy_trade_security_panel);
    if (ui.galaxy_trade_security_panel) {
      ImGui::SliderInt("Top N##trade", &ui.galaxy_trade_security_top_n, 5, 25);
      ui.galaxy_trade_security_top_n = std::clamp(ui.galaxy_trade_security_top_n, 3, 64);
    }

    // Pinned lane summary (if any).
    if (ui.galaxy_trade_pinned_from != kInvalidId && ui.galaxy_trade_pinned_to != kInvalidId) {
      const auto* a = find_ptr(s.systems, ui.galaxy_trade_pinned_from);
      const auto* b = find_ptr(s.systems, ui.galaxy_trade_pinned_to);
      if (a && b) {
        ImGui::Text("Pinned: %s -> %s", a->name.c_str(), b->name.c_str());
      } else {
        ImGui::TextDisabled("Pinned: (invalid)");
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear##trade_pin")) {
        ui.galaxy_trade_pinned_from = kInvalidId;
        ui.galaxy_trade_pinned_to = kInvalidId;
      }
    } else {
      ImGui::TextDisabled("Tip: hold T + click a lane to pin it.");
    }

    // Trade security panel: surfaces the most valuable *and* dangerous corridors/markets.
    if (ui.galaxy_trade_security_panel && trade_net && !trade_lanes_visible.empty()) {
      if (ImGui::TreeNodeEx("Trade security", ImGuiTreeNodeFlags_DefaultOpen)) {
        const double risk_w = std::max(0.0, sim.cfg().ai_trade_security_patrol_risk_weight);
        constexpr double kBaseNeed = 0.20;

        struct LaneRow {
          const TradeLane* lane{nullptr};
          double score{0.0};
          double vol{0.0};
          double risk{0.0};
        };
        std::vector<LaneRow> lane_rows;
        lane_rows.reserve(trade_lanes_visible.size());

        std::unordered_map<Id, double> exposure;
        exposure.reserve(trade_lanes_visible.size() * 2);

        for (const auto* lane : trade_lanes_visible) {
          if (!lane) continue;
          const double vol = std::max(0.0, lane->total_volume);
          const double r0 = system_piracy_risk_effective(sim, s, lane->from_system_id);
          const double r1 = system_piracy_risk_effective(sim, s, lane->to_system_id);
          const double ravg = 0.5 * (r0 + r1);
          const double score = vol * (kBaseNeed + risk_w * ravg);
          lane_rows.push_back(LaneRow{lane, score, vol, ravg});
          exposure[lane->from_system_id] += vol;
          exposure[lane->to_system_id] += vol;
        }

        std::sort(lane_rows.begin(), lane_rows.end(), [](const LaneRow& a, const LaneRow& b) {
          if (a.score != b.score) return a.score > b.score;
          return a.vol > b.vol;
        });

        struct SysRow {
          Id system_id{kInvalidId};
          double score{0.0};
          double vol{0.0};
          double risk{0.0};
        };
        std::vector<SysRow> sys_rows;
        sys_rows.reserve(exposure.size());
        for (const auto& [sid, vol] : exposure) {
          const double r = system_piracy_risk_effective(sim, s, sid);
          const double score = vol * (kBaseNeed + risk_w * r);
          sys_rows.push_back(SysRow{sid, score, vol, r});
        }
        std::sort(sys_rows.begin(), sys_rows.end(), [](const SysRow& a, const SysRow& b) {
          if (a.score != b.score) return a.score > b.score;
          return a.vol > b.vol;
        });

        const int top_n = std::clamp(ui.galaxy_trade_security_top_n, 3, 64);

        ImGui::TextDisabled("Score ~ volume * (0.20 + risk_w * risk). risk_w=%.2f", risk_w);

        if (ImGui::BeginTable("trade_sec_lanes", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 150.0f))) {
          ImGui::TableSetupColumn("Lane", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed);
          ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed);
          ImGui::TableSetupColumn("Risk", ImGuiTableColumnFlags_WidthFixed);
          ImGui::TableHeadersRow();

          for (int i = 0; i < (int)lane_rows.size() && i < top_n; ++i) {
            const auto& row = lane_rows[i];
            const auto* lane = row.lane;
            const auto* a = lane ? find_ptr(s.systems, lane->from_system_id) : nullptr;
            const auto* b = lane ? find_ptr(s.systems, lane->to_system_id) : nullptr;
            const bool pinned = lane && ui.galaxy_trade_pinned_from == lane->from_system_id &&
                                ui.galaxy_trade_pinned_to == lane->to_system_id;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            if (lane) {
              if (ImGui::SmallButton(pinned ? "Unpin" : "Pin")) {
                if (pinned) {
                  ui.galaxy_trade_pinned_from = kInvalidId;
                  ui.galaxy_trade_pinned_to = kInvalidId;
                } else {
                  ui.galaxy_trade_pinned_from = lane->from_system_id;
                  ui.galaxy_trade_pinned_to = lane->to_system_id;
                }
              }
              ImGui::SameLine();
            }
            ImGui::Text("%s -> %s", a ? a->name.c_str() : "?", b ? b->name.c_str() : "?");
            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.1f", row.score);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", row.vol);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.0f%%", row.risk * 100.0);
          }
          ImGui::EndTable();
        }

        if (ImGui::BeginTable("trade_sec_systems", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 150.0f))) {
          ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed);
          ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed);
          ImGui::TableSetupColumn("Risk", ImGuiTableColumnFlags_WidthFixed);
          ImGui::TableHeadersRow();

          for (int i = 0; i < (int)sys_rows.size() && i < top_n; ++i) {
            const auto& row = sys_rows[i];
            const auto* sys = find_ptr(s.systems, row.system_id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            if (ImGui::SmallButton("Go")) {
              if (sys) {
                s.selected_system = sys->id;
                const Vec2 rel = sys->galaxy_pos - world_center;
                pan = Vec2{-rel.x, -rel.y};
              }
            }
            ImGui::SameLine();
            ImGui::Text("%s", sys ? sys->name.c_str() : "?");
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.1f", row.score);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", row.vol);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.0f%%", row.risk * 100.0);
          }
          ImGui::EndTable();
        }

        ImGui::TreePop();
      }
    }

    ImGui::Unindent();
  }

  ImGui::Checkbox("Fleet missions", &ui.show_galaxy_fleet_missions);
  if (ui.show_galaxy_fleet_missions) {
    ImGui::Indent();
    ImGui::SliderFloat("Opacity##galaxy_fleet_mission_alpha", &ui.galaxy_fleet_mission_alpha, 0.05f, 1.00f,
                       "%.2f");
    ui.galaxy_fleet_mission_alpha = std::clamp(ui.galaxy_fleet_mission_alpha, 0.0f, 1.0f);
    ImGui::TextDisabled("Draws patrol routes/circuits and jump point guards for the viewer faction.");
    ImGui::Unindent();
  }

  ImGui::SeparatorText("ProcGen lens");
  {
    int mode = static_cast<int>(ui.galaxy_procgen_lens_mode);
    ImGui::Combo("Metric", &mode, procgen_lens_mode_combo_items());
    ui.galaxy_procgen_lens_mode = static_cast<ProcGenLensMode>(mode);
    ImGui::SliderFloat("Intensity", &ui.galaxy_procgen_lens_alpha, 0.10f, 1.00f, "%.2f");

    // Continuous field (heatmap) behind the galaxy map.
    {
      ImGui::BeginDisabled(ui.galaxy_procgen_lens_mode == ProcGenLensMode::Off);
      ImGui::Checkbox("Field overlay", &ui.galaxy_procgen_field);
      ImGui::EndDisabled();
      if (ui.galaxy_procgen_field && ui.galaxy_procgen_lens_mode != ProcGenLensMode::Off) {
        ImGui::SliderFloat("Field alpha", &ui.galaxy_procgen_field_alpha, 0.05f, 0.60f, "%.2f");
        ui.galaxy_procgen_field_alpha = std::clamp(ui.galaxy_procgen_field_alpha, 0.0f, 1.0f);
        ImGui::SliderInt("Cell px", &ui.galaxy_procgen_field_cell_px, 6, 48);
        ui.galaxy_procgen_field_cell_px = std::clamp(ui.galaxy_procgen_field_cell_px, 4, 128);
        ImGui::TextDisabled("Lower cell px = higher resolution (more draw calls)");
      }
    }

    // Contour lines (isolines) over the interpolated lens field.
    {
      ImGui::BeginDisabled(ui.galaxy_procgen_lens_mode == ProcGenLensMode::Off);
      ImGui::Checkbox("Contours", &ui.galaxy_procgen_contours);
      ImGui::EndDisabled();
      if (ui.galaxy_procgen_contours && ui.galaxy_procgen_lens_mode != ProcGenLensMode::Off) {
        ImGui::SliderFloat("Contour alpha", &ui.galaxy_procgen_contour_alpha, 0.05f, 0.55f, "%.2f");
        ui.galaxy_procgen_contour_alpha = std::clamp(ui.galaxy_procgen_contour_alpha, 0.0f, 1.0f);
        ImGui::SliderInt("Contour cell px", &ui.galaxy_procgen_contour_cell_px, 10, 72);
        ui.galaxy_procgen_contour_cell_px = std::clamp(ui.galaxy_procgen_contour_cell_px, 4, 128);
        ImGui::SliderInt("Levels", &ui.galaxy_procgen_contour_levels, 2, 16);
        ui.galaxy_procgen_contour_levels = std::clamp(ui.galaxy_procgen_contour_levels, 2, 16);
        ImGui::SliderFloat("Thickness", &ui.galaxy_procgen_contour_thickness, 0.8f, 3.0f, "%.1f");
        ui.galaxy_procgen_contour_thickness = std::clamp(ui.galaxy_procgen_contour_thickness, 0.5f, 6.0f);
      }
    }

    // Gradient vectors (optional) for the interpolated lens field.
    {
      ImGui::BeginDisabled(ui.galaxy_procgen_lens_mode == ProcGenLensMode::Off);
      ImGui::Checkbox("Vectors", &ui.galaxy_procgen_vectors);
      ImGui::EndDisabled();
      if (ui.galaxy_procgen_vectors && ui.galaxy_procgen_lens_mode != ProcGenLensMode::Off) {
        ImGui::SliderFloat("Vector alpha", &ui.galaxy_procgen_vector_alpha, 0.05f, 0.60f, "%.2f");
        ui.galaxy_procgen_vector_alpha = std::clamp(ui.galaxy_procgen_vector_alpha, 0.0f, 1.0f);
        ImGui::SliderInt("Vector cell px", &ui.galaxy_procgen_vector_cell_px, 10, 120);
        ui.galaxy_procgen_vector_cell_px = std::clamp(ui.galaxy_procgen_vector_cell_px, 6, 192);
        ImGui::SliderFloat("Vector scale", &ui.galaxy_procgen_vector_scale, 10.0f, 300.0f, "%.0f");
        ui.galaxy_procgen_vector_scale = std::clamp(ui.galaxy_procgen_vector_scale, 1.0f, 600.0f);
        ImGui::SliderFloat("Min gradient", &ui.galaxy_procgen_vector_min_mag, 0.000f, 0.200f, "%.3f");
        ui.galaxy_procgen_vector_min_mag = std::clamp(ui.galaxy_procgen_vector_min_mag, 0.0f, 1.0f);
        ImGui::TextDisabled("Arrows show direction of increasing value");
      }
    }

    // Probe tool: hold Alt over the map to sample the interpolated lens value.
    {
      ImGui::BeginDisabled(ui.galaxy_procgen_lens_mode == ProcGenLensMode::Off);
      ImGui::Checkbox("Alt probe", &ui.galaxy_procgen_probe);
      ImGui::EndDisabled();
      if (ui.galaxy_procgen_probe) {
        ImGui::TextDisabled("Hold Alt over the map to sample the lens field");
      }
    }

    if (ui.galaxy_procgen_lens_mode == ProcGenLensMode::MineralWealth) {
      ImGui::Checkbox("Log scale", &ui.galaxy_procgen_lens_log_scale);
    } else {
      bool dummy = ui.galaxy_procgen_lens_log_scale;
      ImGui::BeginDisabled(true);
      ImGui::Checkbox("Log scale", &dummy);
      ImGui::EndDisabled();
    }
    ImGui::Checkbox("Legend", &ui.galaxy_procgen_lens_show_legend);

    if (ui.galaxy_procgen_lens_mode != ProcGenLensMode::Off && lens_bounds_valid && ui.galaxy_procgen_lens_show_legend) {
      const float w = 220.0f;
      const float h = 12.0f;
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      const ImVec2 p1 = ImVec2(p0.x + w, p0.y + h);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilledMultiColor(p0, p1,
                                 procgen_lens_gradient_color(0.0f, 1.0f),
                                 procgen_lens_gradient_color(1.0f, 1.0f),
                                 procgen_lens_gradient_color(1.0f, 1.0f),
                                 procgen_lens_gradient_color(0.0f, 1.0f));
      dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 200));
      ImGui::Dummy(ImVec2(w, h + 4.0f));

      // Distribution histogram (visible systems) to help tune lenses/thresholds.
      {
        constexpr int kBins = 24;
        std::array<int, kBins> bins{};
        bins.fill(0);
        int total = 0;
        if (lens_max > lens_min + 1e-12) {
          for (const auto& v : visible) {
            const double raw = procgen_lens_value(s, *v.sys, lens_mode);
            if (!std::isfinite(raw)) continue;
            double x = raw;
            if (ui.galaxy_procgen_lens_log_scale && (lens_mode == ProcGenLensMode::MineralWealth)) {
              x = std::log10(std::max(0.0, raw) + 1.0);
            }
            if (!std::isfinite(x)) continue;
            float t = static_cast<float>((x - lens_min) / (lens_max - lens_min));
            t = std::clamp(t, 0.0f, 1.0f);
            int b = static_cast<int>(t * static_cast<float>(kBins));
            if (b >= kBins) b = kBins - 1;
            if (b < 0) b = 0;
            bins[b] += 1;
            total += 1;
          }
        }

        int max_bin = 0;
        for (int i = 0; i < kBins; ++i) max_bin = std::max(max_bin, bins[i]);

        const float hist_h = 32.0f;
        const ImVec2 hp0 = ImGui::GetCursorScreenPos();
        const ImVec2 hp1 = ImVec2(hp0.x + w, hp0.y + hist_h);

        if (max_bin > 0) {
          for (int i = 0; i < kBins; ++i) {
            const float x0 = hp0.x + (w * (static_cast<float>(i) / static_cast<float>(kBins)));
            const float x1 = hp0.x + (w * (static_cast<float>(i + 1) / static_cast<float>(kBins)));
            const float frac = static_cast<float>(bins[i]) / static_cast<float>(max_bin);
            const float y0 = hp1.y - hist_h * frac;
            const float tcol = (static_cast<float>(i) + 0.5f) / static_cast<float>(kBins);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, hp1.y), procgen_lens_gradient_color(tcol, 0.70f));
          }
        }
        dl->AddRect(hp0, hp1, IM_COL32(0, 0, 0, 200));
        ImGui::Dummy(ImVec2(w, hist_h + 4.0f));
        ImGui::TextDisabled("Distribution (visible systems): %d", total);
      }

      const char* unit = procgen_lens_value_unit(ui.galaxy_procgen_lens_mode);
      const double disp_scale = procgen_lens_display_scale(ui.galaxy_procgen_lens_mode);
      const int dec = procgen_lens_display_decimals(ui.galaxy_procgen_lens_mode);
      const double disp_min = lens_raw_min * disp_scale;
      const double disp_max = lens_raw_max * disp_scale;
      if (ui.galaxy_procgen_lens_mode == ProcGenLensMode::MineralWealth && ui.galaxy_procgen_lens_log_scale) {
        // Mineral wealth is wide-range; keep a compact integer display.
        ImGui::TextDisabled("Min %.0f %s, Max %.0f %s (log)", disp_min, unit, disp_max, unit);
      } else {
        if (dec <= 0) {
          ImGui::TextDisabled("Min %.0f %s, Max %.0f %s", disp_min, unit, disp_max, unit);
        } else if (dec == 1) {
          ImGui::TextDisabled("Min %.1f %s, Max %.1f %s", disp_min, unit, disp_max, unit);
        } else if (dec == 2) {
          ImGui::TextDisabled("Min %.2f %s, Max %.2f %s", disp_min, unit, disp_max, unit);
        } else {
          ImGui::TextDisabled("Min %.3f %s, Max %.3f %s", disp_min, unit, disp_max, unit);
        }
      }
    }
  }
  ImGui::SeparatorText("Route ruler (hold D)");
  ImGui::TextDisabled("Hold D and left click two systems. D+right click clears.");
  {
    const auto* a_sys = (route_ruler.a != kInvalidId) ? find_ptr(s.systems, route_ruler.a) : nullptr;
    const auto* b_sys = (route_ruler.b != kInvalidId) ? find_ptr(s.systems, route_ruler.b) : nullptr;

    if (a_sys) {
      ImGui::Text("A: %s", a_sys->name.c_str());
    } else {
      ImGui::TextDisabled("A: (not set)");
    }
    if (b_sys) {
      ImGui::Text("B: %s", b_sys->name.c_str());
    } else {
      ImGui::TextDisabled("B: (not set)");
    }

    if (a_sys && b_sys) {
      const double dist_u = (b_sys->galaxy_pos - a_sys->galaxy_pos).length();
      ImGui::TextDisabled("Direct: %.2f u", dist_u);

      if (ruler_route && ruler_route->systems.size() >= 2) {
        const int jumps = static_cast<int>(ruler_route->systems.size() - 1);
        ImGui::Text("Route: %d jumps", jumps);
        ImGui::TextDisabled("Route distance: %.0f mkm", ruler_route->distance_mkm);

        if (std::isfinite(ruler_route->eta_days) && ruler_speed_km_s > 1e-9) {
          ImGui::Text("ETA: %s", format_duration_days(ruler_route->eta_days).c_str());
          ImGui::SameLine();
          ImGui::TextDisabled("(%s @ %.0f km/s)", ruler_speed_basis ? ruler_speed_basis : "Speed", ruler_speed_km_s);
        } else {
          ImGui::TextDisabled("ETA: (select a ship/fleet for speed)");
        }
      } else {
        ImGui::TextDisabled("No known route (respecting fog of war).\nSurvey more jump points.");
      }

      if (ImGui::Button("Swap A<->B")) {
        std::swap(route_ruler.a, route_ruler.b);
        // Force recompute of the cached route next frame.
        ruler_route.reset();
      }
      ImGui::SameLine();
      if (ImGui::Button("Clear ruler")) {
        route_ruler = RouteRulerState{};
        ruler_route.reset();
      }
    } else {
      if (route_ruler.a != kInvalidId || route_ruler.b != kInvalidId) {
        if (ImGui::Button("Clear ruler")) {
          route_ruler = RouteRulerState{};
        }
      }
    }
  }

  ImGui::Checkbox("Region halos", &ui.show_galaxy_regions);
  ImGui::SameLine();
  ImGui::Checkbox("Boundaries", &ui.show_galaxy_region_boundaries);
  ImGui::BeginDisabled(!ui.show_galaxy_regions);
  ImGui::SameLine();
  ImGui::Checkbox("Labels", &ui.show_galaxy_region_labels);
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!(ui.show_galaxy_regions || ui.show_galaxy_region_boundaries) || ui.selected_region_id == kInvalidId);
  ImGui::SameLine();
  ImGui::Checkbox("Dim others", &ui.galaxy_region_dim_nonselected);
  ImGui::EndDisabled();

  const bool any_region_overlay = (ui.show_galaxy_regions || ui.show_galaxy_region_boundaries);
  ImGui::BeginDisabled(!any_region_overlay);
  ImGui::Checkbox("Centers##region_centers", &ui.show_galaxy_region_centers);
  ImGui::SameLine();
  ImGui::Checkbox("Border links##region_border_links", &ui.show_galaxy_region_border_links);
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!ui.show_galaxy_region_boundaries);
  ImGui::Checkbox("Voronoi geometry##region_voronoi_geom", &ui.galaxy_region_boundary_voronoi);
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted("Hull: convex hull of visible systems.\nVoronoi: clipped partition from region centers.");
    ImGui::EndTooltip();
  }
  ImGui::EndDisabled();


  if (ui.selected_region_id != kInvalidId) {
    const auto* reg = find_ptr(s.regions, ui.selected_region_id);
    if (reg) {
      ImGui::TextDisabled("Selected region: %s", reg->name.c_str());
    } else {
      ImGui::TextDisabled("Selected region: %llu", (unsigned long long)ui.selected_region_id);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##selected_region")) {
      ui.selected_region_id = kInvalidId;
    }
  }

  if (ImGui::Button("Reset view (R)")) {
    zoom = 1.0;
    pan = Vec2{0.0, 0.0};
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Zoom: %.2fx", zoom);
  {
    const Vec2 rel = to_world(mouse, center_px, scale, zoom, pan);
    const Vec2 abs = rel + world_center;
    ImGui::TextDisabled("Cursor: %.2f, %.2f u", abs.x, abs.y);
  }

  if (ui.fog_of_war) {
    if (viewer_faction_id == kInvalidId) {
      ImGui::TextDisabled("Select a ship/faction to define view");
    } else {
      ImGui::TextDisabled("Viewer faction: %llu", (unsigned long long)viewer_faction_id);
      ImGui::TextDisabled("Visible systems: %d", (int)visible.size());
    }
  }

  ImGui::EndChild();
}

} // namespace nebula4x::ui
