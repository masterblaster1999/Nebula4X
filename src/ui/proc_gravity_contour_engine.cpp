#include "ui/proc_gravity_contour_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "ui/map_render.h"

namespace nebula4x::ui {
namespace {

constexpr double kEarthsPerSolarMass = 332946.0487; // approx.

static inline bool finite_vec(const Vec2& v) {
  return std::isfinite(v.x) && std::isfinite(v.y);
}

static inline Vec2 screen_to_world(const ImVec2& p,
                                  const ImVec2& center,
                                  double scale_px_per_mkm,
                                  double zoom,
                                  const Vec2& pan_mkm) {
  const double denom = scale_px_per_mkm * zoom;
  if (denom <= 0.0) return Vec2{0.0, 0.0};
  const double inv = 1.0 / denom;
  return Vec2{(static_cast<double>(p.x) - static_cast<double>(center.x)) * inv - pan_mkm.x,
              (static_cast<double>(p.y) - static_cast<double>(center.y)) * inv - pan_mkm.y};
}

static inline ImVec2 world_to_screen(const Vec2& w,
                                    const ImVec2& center,
                                    double scale_px_per_mkm,
                                    double zoom,
                                    const Vec2& pan_mkm) {
  return ImVec2(center.x + static_cast<float>((w.x + pan_mkm.x) * scale_px_per_mkm * zoom),
                center.y + static_cast<float>((w.y + pan_mkm.y) * scale_px_per_mkm * zoom));
}

static inline std::uint32_t hash_u32(std::uint32_t x) {
  // A tiny 32-bit mix function.
  x ^= x >> 17;
  x *= 0xED5AD4BBu;
  x ^= x >> 11;
  x *= 0xAC4C1B51u;
  x ^= x >> 15;
  x *= 0x31848BABu;
  x ^= x >> 14;
  return x;
}

static inline std::uint64_t hash_combine_u64(std::uint64_t h, std::uint64_t v) {
  // 64-bit boost-ish combine.
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}

static inline double safe_log10(double x) {
  return std::log10(std::max(x, 1e-30));
}

static inline double clampd(double x, double lo, double hi) {
  return std::clamp(x, lo, hi);
}

static double effective_mass_earths(const Body& b) {
  if (b.mass_earths > 0.0) return b.mass_earths;
  if (b.mass_solar > 0.0) return b.mass_solar * kEarthsPerSolarMass;

  // Fallbacks so the overlay still works for incomplete data.
  switch (b.type) {
    case BodyType::Star:
      return 1.0 * kEarthsPerSolarMass;
    case BodyType::Planet:
      return 1.0;
    case BodyType::Moon:
      return 0.0123;
    case BodyType::Asteroid:
      return 1e-6;
    case BodyType::Comet:
      return 2e-7;
    default:
      return 0.0;
  }
}

struct MassBody {
  Vec2 pos_mkm{0.0, 0.0};
  double mass_earths{0.0};
  double soft_mkm{0.05};
};

static std::vector<MassBody> gather_mass_bodies(const Simulation& sim,
                                                Id system_id,
                                                const ProcGravityContourConfig& cfg,
                                                double* out_max_mass) {
  std::vector<MassBody> out;
  *out_max_mass = 0.0;

  const GameState& s = sim.state();
  const StarSystem* sys = find_ptr(s.systems, system_id);
  if (!sys) return out;

  out.reserve(sys->bodies.size());
  for (Id bid : sys->bodies) {
    const Body* b = find_ptr(s.bodies, bid);
    if (!b) continue;

    const double m = effective_mass_earths(*b);
    if (!(m > 0.0) || !std::isfinite(m)) continue;

    double soft = std::max(1e-6, static_cast<double>(cfg.softening_min_mkm));
    if (b->radius_km > 0.0 && std::isfinite(b->radius_km) && cfg.softening_radius_mult > 0.0f) {
      const double radius_mkm = b->radius_km / 1.0e6;
      soft = std::max(soft, radius_mkm * static_cast<double>(cfg.softening_radius_mult));
    }

    MassBody mb;
    mb.pos_mkm = b->position_mkm;
    mb.mass_earths = m;
    mb.soft_mkm = soft;
    out.push_back(mb);
    *out_max_mass = std::max(*out_max_mass, m);
  }

  return out;
}

static std::vector<double> compute_levels(double max_mass_earths,
                                          double tile_world_mkm,
                                          const ProcGravityContourConfig& cfg) {
  const int n = std::clamp(cfg.contour_levels, 1, 32);
  std::vector<double> levels;
  levels.resize(static_cast<std::size_t>(n));

  const double spacing = std::clamp(static_cast<double>(cfg.level_spacing_decades), 0.05, 3.0);
  const double offset = static_cast<double>(cfg.level_offset_decades);

  // Reference distance: tie it to the cached tile size so zoom buckets have
  // stable bands and adjacent tiles share levels.
  const double ref_r = std::max(1e-6, tile_world_mkm * 0.65);
  const double ref_phi = std::max(1e-30, max_mass_earths / (ref_r + 1e-6));
  const double base_log = safe_log10(ref_phi);

  const double mid = 0.5 * static_cast<double>(n - 1);
  for (int i = 0; i < n; ++i) {
    const double di = static_cast<double>(i) - mid;
    const double l = base_log + offset + di * spacing;
    levels[static_cast<std::size_t>(i)] = std::pow(10.0, l);
  }
  return levels;
}

static inline Vec2 interp_edge(const Vec2& a,
                              const Vec2& b,
                              double va,
                              double vb,
                              double level) {
  const double denom = vb - va;
  double t = 0.5;
  if (std::abs(denom) > 1e-18) {
    t = (level - va) / denom;
  }
  t = clampd(t, 0.0, 1.0);
  return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

} // namespace

std::size_t ProcGravityContourEngine::TileKeyHash::operator()(const TileKey& k) const noexcept {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.system_id));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.tx)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.ty)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.scale_bucket)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.tile_px)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.day_bucket));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.seed));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.style_hash));
  return static_cast<std::size_t>(h);
}

std::uint64_t ProcGravityContourEngine::compute_style_hash(const ProcGravityContourConfig& cfg) {
  std::uint64_t h = 0xC0FFEEBADC0FFEEFull;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.tile_px));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.samples_per_tile));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.contour_levels));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(std::llround(cfg.level_spacing_decades * 1000.0f)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(std::llround(cfg.level_offset_decades * 1000.0f)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(std::llround(cfg.softening_min_mkm * 1000.0f)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(std::llround(cfg.softening_radius_mult * 1000.0f)));
  return h;
}

int ProcGravityContourEngine::quantize_scale_bucket(double units_per_px_mkm) {
  // Log2 bucketed, with 1/8th octave steps to avoid thrashing while zooming.
  units_per_px_mkm = std::max(units_per_px_mkm, 1e-12);
  const double l2 = std::log2(units_per_px_mkm);
  return static_cast<int>(std::llround(l2 * 8.0));
}

double ProcGravityContourEngine::bucket_to_units_per_px_mkm(int bucket) {
  return std::pow(2.0, static_cast<double>(bucket) / 8.0);
}

void ProcGravityContourEngine::begin_frame(double sim_time_days) {
  ++frame_index_;

  double t = std::fmod(sim_time_days, 1000000000.0);
  if (!std::isfinite(t)) t = 0.0;
  time_days_ = t;

  stats_ = ProcGravityContourStats{};
  stats_.cache_tiles = static_cast<int>(cache_.size());
}

void ProcGravityContourEngine::clear() {
  cache_.clear();
  stats_ = ProcGravityContourStats{};
}

ProcGravityContourEngine::Tile& ProcGravityContourEngine::get_or_build_tile(const TileKey& key,
                                                                            double tile_world_mkm,
                                                                            const Simulation& sim,
                                                                            Id system_id,
                                                                            double max_mass_earths,
                                                                            const ProcGravityContourConfig& cfg) {
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    it->second.last_used_frame = frame_index_;
    return it->second;
  }

  Tile tile;
  tile.last_used_frame = frame_index_;

  // Gather bodies (positions are taken from current state; cache is day-bucketed).
  double dummy = 0.0;
  std::vector<MassBody> bodies = gather_mass_bodies(sim, system_id, cfg, &dummy);
  if (bodies.empty() || !(max_mass_earths > 0.0)) {
    auto [ins_it, _] = cache_.emplace(key, std::move(tile));
    stats_.tiles_generated_this_frame += 1;
    return ins_it->second;
  }

  const int N = std::clamp(cfg.samples_per_tile, 8, 96);
  const double step = tile_world_mkm / static_cast<double>(N);
  const double ox = static_cast<double>(key.tx) * tile_world_mkm;
  const double oy = static_cast<double>(key.ty) * tile_world_mkm;

  // Field samples at grid points (N+1)^2.
  std::vector<double> field;
  field.resize(static_cast<std::size_t>((N + 1) * (N + 1)), 0.0);

  for (int j = 0; j <= N; ++j) {
    const double y = oy + static_cast<double>(j) * step;
    for (int i = 0; i <= N; ++i) {
      const double x = ox + static_cast<double>(i) * step;
      double v = 0.0;
      for (const MassBody& mb : bodies) {
        const double dx = x - mb.pos_mkm.x;
        const double dy = y - mb.pos_mkm.y;
        const double r = std::sqrt(dx * dx + dy * dy);
        v += mb.mass_earths / (r + mb.soft_mkm);
      }
      if (!std::isfinite(v)) v = 0.0;
      field[static_cast<std::size_t>(j * (N + 1) + i)] = v;
    }
  }

  const std::vector<double> levels = compute_levels(max_mass_earths, tile_world_mkm, cfg);
  tile.segments.reserve(static_cast<std::size_t>(levels.size() * N * 2));

  const double min_seg = step * 0.08;
  const double min_seg2 = min_seg * min_seg;

  for (std::size_t li = 0; li < levels.size(); ++li) {
    const double level = levels[li];
    const std::uint16_t level_idx = static_cast<std::uint16_t>(std::min<std::size_t>(li, 65535));

    for (int j = 0; j < N; ++j) {
      const double y0 = oy + static_cast<double>(j) * step;
      const double y1 = y0 + step;
      for (int i = 0; i < N; ++i) {
        const double x0 = ox + static_cast<double>(i) * step;
        const double x1 = x0 + step;

        const double v00 = field[static_cast<std::size_t>(j * (N + 1) + i)];
        const double v10 = field[static_cast<std::size_t>(j * (N + 1) + (i + 1))];
        const double v01 = field[static_cast<std::size_t>((j + 1) * (N + 1) + i)];
        const double v11 = field[static_cast<std::size_t>((j + 1) * (N + 1) + (i + 1))];

        int c = 0;
        if (v00 > level) c |= 1;
        if (v10 > level) c |= 2;
        if (v11 > level) c |= 4;
        if (v01 > level) c |= 8;
        if (c == 0 || c == 15) continue;

        const Vec2 p00{x0, y0};
        const Vec2 p10{x1, y0};
        const Vec2 p01{x0, y1};
        const Vec2 p11{x1, y1};

        auto edge_pt = [&](int edge) -> Vec2 {
          switch (edge) {
            case 0:
              return interp_edge(p00, p10, v00, v10, level);
            case 1:
              return interp_edge(p10, p11, v10, v11, level);
            case 2:
              return interp_edge(p01, p11, v01, v11, level);
            default:
              return interp_edge(p00, p01, v00, v01, level);
          }
        };

        auto emit = [&](int ea, int eb) {
          const Vec2 a = edge_pt(ea);
          const Vec2 b = edge_pt(eb);
          if (!finite_vec(a) || !finite_vec(b)) return;
          const double dx = a.x - b.x;
          const double dy = a.y - b.y;
          if ((dx * dx + dy * dy) < min_seg2) return;
          Segment s;
          s.a_mkm = a;
          s.b_mkm = b;
          s.level_idx = level_idx;
          tile.segments.push_back(s);
        };

        // Standard marching-squares cases (with a simple asymptotic decider for
        // the two ambiguous saddle cases).
        switch (c) {
          case 1:
            emit(3, 0);
            break;
          case 2:
            emit(0, 1);
            break;
          case 3:
            emit(3, 1);
            break;
          case 4:
            emit(1, 2);
            break;
          case 5: {
            const double center = 0.25 * (v00 + v10 + v01 + v11);
            if (center > level) {
              emit(0, 1);
              emit(2, 3);
            } else {
              emit(3, 0);
              emit(1, 2);
            }
            break;
          }
          case 6:
            emit(0, 2);
            break;
          case 7:
            emit(3, 2);
            break;
          case 8:
            emit(2, 3);
            break;
          case 9:
            emit(0, 2);
            break;
          case 10: {
            const double center = 0.25 * (v00 + v10 + v01 + v11);
            if (center > level) {
              emit(3, 0);
              emit(1, 2);
            } else {
              emit(0, 1);
              emit(2, 3);
            }
            break;
          }
          case 11:
            emit(1, 2);
            break;
          case 12:
            emit(3, 1);
            break;
          case 13:
            emit(0, 1);
            break;
          case 14:
            emit(3, 0);
            break;
          default:
            break;
        }
      }
    }
  }

  auto [ins_it, _] = cache_.emplace(key, std::move(tile));
  stats_.tiles_generated_this_frame += 1;
  return ins_it->second;
}

void ProcGravityContourEngine::trim_cache(int max_tiles) {
  max_tiles = std::clamp(max_tiles, 0, 4096);
  if (max_tiles == 0) {
    cache_.clear();
    return;
  }

  const int sz = static_cast<int>(cache_.size());
  if (sz <= max_tiles) return;

  std::vector<std::pair<TileKey, std::uint64_t>> lru;
  lru.reserve(cache_.size());
  for (const auto& kv : cache_) {
    lru.emplace_back(kv.first, kv.second.last_used_frame);
  }

  std::sort(lru.begin(), lru.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

  const int remove_n = static_cast<int>(lru.size()) - max_tiles;
  for (int i = 0; i < remove_n; ++i) {
    cache_.erase(lru[static_cast<std::size_t>(i)].first);
  }
}

void ProcGravityContourEngine::draw_contours(ImDrawList* draw,
                                            const ImVec2& origin,
                                            const ImVec2& size,
                                            const ImVec2& center,
                                            double scale_px_per_mkm,
                                            double zoom,
                                            const Vec2& pan_mkm,
                                            const Simulation& sim,
                                            Id system_id,
                                            std::uint32_t seed,
                                            const ProcGravityContourConfig& cfg,
                                            ImU32 base_color) {
  if (!draw) return;
  if (!cfg.enabled) return;
  if (system_id == kInvalidId) return;
  if (size.x <= 2.0f || size.y <= 2.0f) return;
  if (!(cfg.opacity > 0.0f) || !(cfg.thickness_px > 0.0f)) return;

  const double denom = scale_px_per_mkm * zoom;
  if (denom <= 0.0) return;
  const double units_per_px_mkm = 1.0 / denom;

  const int scale_bucket = quantize_scale_bucket(units_per_px_mkm);
  const double bucket_units_per_px_mkm = bucket_to_units_per_px_mkm(scale_bucket);
  const int tile_px = std::clamp(cfg.tile_px, 64, 2048);
  const double tile_world_mkm = bucket_units_per_px_mkm * static_cast<double>(tile_px);
  if (!(tile_world_mkm > 1e-9) || !std::isfinite(tile_world_mkm)) return;

  const std::int64_t day_bucket = static_cast<std::int64_t>(std::floor(time_days_ + 1e-9));
  const std::uint64_t style_hash = compute_style_hash(cfg);

  // Gather bodies once to determine a stable contour range.
  double max_mass = 0.0;
  (void)gather_mass_bodies(sim, system_id, cfg, &max_mass);
  if (!(max_mass > 0.0)) return;

  const Vec2 w0 = screen_to_world(origin, center, scale_px_per_mkm, zoom, pan_mkm);
  const Vec2 w1 = screen_to_world(ImVec2(origin.x + size.x, origin.y + size.y), center, scale_px_per_mkm, zoom, pan_mkm);

  const double minx = std::min(w0.x, w1.x);
  const double maxx = std::max(w0.x, w1.x);
  const double miny = std::min(w0.y, w1.y);
  const double maxy = std::max(w0.y, w1.y);

  const int tx0 = static_cast<int>(std::floor(minx / tile_world_mkm));
  const int tx1 = static_cast<int>(std::floor(maxx / tile_world_mkm));
  const int ty0 = static_cast<int>(std::floor(miny / tile_world_mkm));
  const int ty1 = static_cast<int>(std::floor(maxy / tile_world_mkm));

  draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

  const int max_levels = std::clamp(cfg.contour_levels, 1, 32);

  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      TileKey key;
      key.system_id = system_id;
      key.tx = tx;
      key.ty = ty;
      key.scale_bucket = scale_bucket;
      key.tile_px = tile_px;
      key.day_bucket = day_bucket;
      key.seed = hash_u32(seed ^ hash_u32(static_cast<std::uint32_t>(tx * 73856093) ^ static_cast<std::uint32_t>(ty * 19349663)));
      key.style_hash = style_hash;

      Tile& tile = get_or_build_tile(key, tile_world_mkm, sim, system_id, max_mass, cfg);
      tile.last_used_frame = frame_index_;
      stats_.tiles_used_this_frame += 1;

      if (cfg.debug_tile_bounds) {
        const Vec2 a{static_cast<double>(tx) * tile_world_mkm, static_cast<double>(ty) * tile_world_mkm};
        const Vec2 b{a.x + tile_world_mkm, a.y + tile_world_mkm};
        const ImVec2 pa = world_to_screen(a, center, scale_px_per_mkm, zoom, pan_mkm);
        const ImVec2 pb = world_to_screen(b, center, scale_px_per_mkm, zoom, pan_mkm);
        draw->AddRect(pa, pb, IM_COL32(255, 0, 255, 85), 0.0f, 0, 1.0f);
      }

      for (const Segment& seg : tile.segments) {
        const ImVec2 a = world_to_screen(seg.a_mkm, center, scale_px_per_mkm, zoom, pan_mkm);
        const ImVec2 b = world_to_screen(seg.b_mkm, center, scale_px_per_mkm, zoom, pan_mkm);

        float t = 1.0f;
        if (max_levels > 1) {
          t = static_cast<float>(seg.level_idx) / static_cast<float>(max_levels - 1);
        }
        const float alpha = cfg.opacity * (0.35f + 0.65f * t);
        if (alpha <= 1e-4f) continue;
        draw->AddLine(a, b, modulate_alpha(base_color, alpha), cfg.thickness_px);
        stats_.segments_drawn += 1;
      }
    }
  }

  draw->PopClipRect();

  trim_cache(cfg.max_cached_tiles);
  stats_.cache_tiles = static_cast<int>(cache_.size());
}

} // namespace nebula4x::ui
