#include "ui/proc_territory_field_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace nebula4x::ui {

namespace {

using Clock = std::chrono::high_resolution_clock;

static inline double ms_since(const Clock::time_point& start) {
  const auto end = Clock::now();
  const std::chrono::duration<double, std::milli> dt = end - start;
  return dt.count();
}

static inline std::uint32_t hash_u32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static inline std::uint64_t hash_combine_u64(std::uint64_t h, std::uint64_t v) {
  return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

static inline std::uint64_t quant_to_u64(double v, double scale) {
  const long long q = static_cast<long long>(std::llround(v * scale));
  return static_cast<std::uint64_t>(static_cast<std::uint64_t>(q) ^ 0xC0FFEEu);
}

static inline std::uint32_t hash_2d_i32(int x, int y, std::uint32_t seed) {
  std::uint32_t h = seed;
  h ^= hash_u32(static_cast<std::uint32_t>(x) * 0x9E3779B9u);
  h ^= hash_u32(static_cast<std::uint32_t>(y) * 0x85EBCA6Bu);
  return hash_u32(h);
}

static inline float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

struct RGB8 {
  std::uint8_t r{0}, g{0}, b{0};
};

static RGB8 faction_color(Id fid) {
  const float h = std::fmod(static_cast<float>(static_cast<std::uint32_t>(fid)) * 0.61803398875f, 1.0f);
  float r = 1.0f, g = 1.0f, b = 1.0f;
  ImGui::ColorConvertHSVtoRGB(h, 0.58f, 0.95f, r, g, b);
  auto to8 = [](float x) -> std::uint8_t {
    x = std::clamp(x, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(x * 255.0f));
  };
  return RGB8{to8(r), to8(g), to8(b)};
}

} // namespace

std::size_t ProcTerritoryFieldEngine::TileKeyHash::operator()(const TileKey& k) const noexcept {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.tx));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.ty));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.scale_bucket));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.tile_px));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.samples));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.seed));
  h = hash_combine_u64(h, k.sources_hash);
  h = hash_combine_u64(h, k.style_hash);
  return static_cast<std::size_t>(h);
}

void ProcTerritoryFieldEngine::begin_frame() {
  ++frame_index_;
  stats_.tiles_used_this_frame = 0;
  stats_.tiles_generated_this_frame = 0;
  stats_.cells_drawn = 0;
  stats_.gen_ms_this_frame = 0.0;
  stats_.cache_tiles = static_cast<int>(cache_.size());
}

void ProcTerritoryFieldEngine::clear() {
  cache_.clear();
  stats_.cache_tiles = 0;
}

std::uint64_t ProcTerritoryFieldEngine::compute_style_hash(const ProcTerritoryFieldConfig& cfg) {
  std::uint64_t h = 0;
  auto hb = [&](std::uint64_t v) { h = hash_combine_u64(h, v); };
  hb(static_cast<std::uint64_t>(cfg.draw_fill ? 1 : 0));
  hb(static_cast<std::uint64_t>(cfg.draw_boundaries ? 1 : 0));
  hb(static_cast<std::uint64_t>(cfg.contested_dither ? 1 : 0));
  hb(static_cast<std::uint64_t>(cfg.debug_tile_bounds ? 1 : 0));
  hb(quant_to_u64(cfg.fill_opacity, 10000.0));
  hb(quant_to_u64(cfg.boundary_opacity, 10000.0));
  hb(quant_to_u64(cfg.boundary_thickness_px, 1000.0));
  hb(quant_to_u64(cfg.influence_base_spacing_mult, 1000.0));
  hb(quant_to_u64(cfg.influence_pop_spacing_mult, 1000.0));
  hb(quant_to_u64(cfg.influence_pop_log_bias, 1000.0));
  hb(quant_to_u64(cfg.presence_falloff_spacing, 1000.0));
  hb(quant_to_u64(cfg.dominance_softness_spacing, 1000.0));
  hb(static_cast<std::uint64_t>(cfg.max_sources));
  hb(quant_to_u64(cfg.contested_threshold, 10000.0));
  hb(quant_to_u64(cfg.contested_dither_strength, 10000.0));
  return h;
}

int ProcTerritoryFieldEngine::quantize_scale_bucket(double units_per_px) {
  const double u = std::max(1e-12, units_per_px);
  const double x = std::log2(u);
  return static_cast<int>(std::llround(x * 8.0));
}

double ProcTerritoryFieldEngine::bucket_to_units_per_px(int bucket) {
  return std::pow(2.0, static_cast<double>(bucket) / 8.0);
}

std::uint64_t ProcTerritoryFieldEngine::compute_sources_hash(const std::vector<EvalSource>& eval_sources,
                                                             double spacing_units,
                                                             const ProcTerritoryFieldConfig& cfg) {
  std::vector<EvalSource> tmp = eval_sources;
  std::sort(tmp.begin(), tmp.end(), [](const EvalSource& a, const EvalSource& b) {
    if (a.faction_id != b.faction_id) return a.faction_id < b.faction_id;
    if (a.pos.x != b.pos.x) return a.pos.x < b.pos.x;
    if (a.pos.y != b.pos.y) return a.pos.y < b.pos.y;
    return a.radius < b.radius;
  });

  std::uint64_t h = 0;
  h = hash_combine_u64(h, quant_to_u64(spacing_units, 1024.0));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.max_sources));
  for (const auto& s : tmp) {
    h = hash_combine_u64(h, static_cast<std::uint64_t>(s.faction_id));
    h = hash_combine_u64(h, quant_to_u64(s.pos.x, 512.0));
    h = hash_combine_u64(h, quant_to_u64(s.pos.y, 512.0));
    h = hash_combine_u64(h, quant_to_u64(s.radius, 512.0));
  }
  return h;
}

void ProcTerritoryFieldEngine::trim_cache(int max_tiles) {
  if (max_tiles < 1) max_tiles = 1;
  if (static_cast<int>(cache_.size()) <= max_tiles) return;

  std::vector<std::pair<std::uint64_t, TileKey>> order;
  order.reserve(cache_.size());
  for (const auto& kv : cache_) {
    order.emplace_back(kv.second.last_used_frame, kv.first);
  }
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  const int to_evict = static_cast<int>(cache_.size()) - max_tiles;
  for (int i = 0; i < to_evict; ++i) {
    cache_.erase(order[i].second);
  }
}

ProcTerritoryFieldEngine::Tile& ProcTerritoryFieldEngine::get_or_build_tile(const TileKey& key,
                                                                            double tile_world_units,
                                                                            const std::vector<EvalSource>& eval_sources,
                                                                            int faction_count,
                                                                            double spacing_units,
                                                                            const ProcTerritoryFieldConfig& cfg) {
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    it->second.last_used_frame = frame_index_;
    return it->second;
  }

  Tile t;
  t.last_used_frame = frame_index_;
  t.grid = std::max(4, key.samples);
  t.cells.resize(static_cast<std::size_t>(t.grid) * static_cast<std::size_t>(t.grid));

  const auto t0 = Clock::now();

  const int grid = t.grid;
  const double cell_world = tile_world_units / static_cast<double>(grid);
  const double tile_min_x = static_cast<double>(key.tx) * tile_world_units;
  const double tile_min_y = static_cast<double>(key.ty) * tile_world_units;

  const float presence_falloff = std::max(1e-6f, cfg.presence_falloff_spacing * static_cast<float>(spacing_units));
  const float softness = std::max(1e-6f, cfg.dominance_softness_spacing * static_cast<float>(spacing_units));

  std::vector<float> best;
  best.resize(static_cast<std::size_t>(std::max(1, faction_count)));

  auto cell_idx = [grid](int x, int y) -> std::size_t { return static_cast<std::size_t>(y) * static_cast<std::size_t>(grid) + static_cast<std::size_t>(x); };

  for (int y = 0; y < grid; ++y) {
    for (int x = 0; x < grid; ++x) {
      const double wx = tile_min_x + (static_cast<double>(x) + 0.5) * cell_world;
      const double wy = tile_min_y + (static_cast<double>(y) + 0.5) * cell_world;

      std::fill(best.begin(), best.end(), std::numeric_limits<float>::infinity());

      for (const auto& src : eval_sources) {
        const double dx = wx - src.pos.x;
        const double dy = wy - src.pos.y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        const float d = static_cast<float>(dist) - src.radius;
        const int fi = src.faction_index;
        if (fi >= 0 && fi < faction_count && d < best[static_cast<std::size_t>(fi)]) {
          best[static_cast<std::size_t>(fi)] = d;
        }
      }

      int best_i = -1;
      float best_d = std::numeric_limits<float>::infinity();
      int second_i = -1;
      float second_d = std::numeric_limits<float>::infinity();
      for (int fi = 0; fi < faction_count; ++fi) {
        const float d = best[static_cast<std::size_t>(fi)];
        if (!std::isfinite(d)) continue;
        if (d < best_d) {
          second_d = best_d;
          second_i = best_i;
          best_d = d;
          best_i = fi;
        } else if (d < second_d) {
          second_d = d;
          second_i = fi;
        }
      }

      Cell c;
      c.faction_index = -1;
      c.alpha = 0.0f;
      c.dominance = 0.0f;
      c.boundary = false;

      if (best_i >= 0) {
        float presence = 1.0f;
        if (best_d > 0.0f) {
          presence = 1.0f / (1.0f + (best_d / presence_falloff));
        }

        // Drop very weak coverage so we don't paint the whole galaxy.
        if (presence >= 0.08f) {
          float diff = std::isfinite(second_d) ? (second_d - best_d) : softness * 4.0f;
          if (diff < 0.0f) diff = 0.0f;
          const float dom = diff / (diff + softness);

          float a = cfg.fill_opacity * presence * dom;
          a = clamp01(a);

          if (cfg.contested_dither && dom < cfg.contested_threshold) {
            const std::uint32_t h = hash_2d_i32(key.tx * 4096 + x, key.ty * 4096 + y, key.seed ^ 0xA771BEEFu);
            const bool on = (h & 1u) != 0u;
            if (on) {
              a *= (1.0f - clamp01(cfg.contested_dither_strength));
            }
          }

          c.faction_index = best_i;
          c.alpha = a;
          c.dominance = dom;
        }
      }

      t.cells[cell_idx(x, y)] = c;
    }
  }

  // Boundary detection (4-neighborhood) and dilation.
  if (cfg.draw_boundaries) {
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(grid) * static_cast<std::size_t>(grid), 0);
    for (int y = 0; y < grid; ++y) {
      for (int x = 0; x < grid; ++x) {
        const Cell& c = t.cells[cell_idx(x, y)];
        const int f = c.faction_index;
        if (f < 0) continue;
        bool b = false;
        auto check = [&](int nx, int ny) {
          if (nx < 0 || ny < 0 || nx >= grid || ny >= grid) return;
          const int of = t.cells[cell_idx(nx, ny)].faction_index;
          if (of >= 0 && of != f) b = true;
        };
        check(x + 1, y);
        check(x - 1, y);
        check(x, y + 1);
        check(x, y - 1);
        if (b) mask[cell_idx(x, y)] = 1;
      }
    }

    int thick = 1;
    if (cfg.boundary_thickness_px > 0.0f && key.tile_px > 0) {
      const float cells_per_px = static_cast<float>(grid) / static_cast<float>(key.tile_px);
      thick = std::max(1, static_cast<int>(std::lround(cfg.boundary_thickness_px * cells_per_px)));
    }

    if (thick > 1) {
      std::vector<std::uint8_t> out = mask;
      for (int y = 0; y < grid; ++y) {
        for (int x = 0; x < grid; ++x) {
          if (!mask[cell_idx(x, y)]) continue;
          for (int dy = -thick; dy <= thick; ++dy) {
            for (int dx = -thick; dx <= thick; ++dx) {
              const int nx = x + dx;
              const int ny = y + dy;
              if (nx < 0 || ny < 0 || nx >= grid || ny >= grid) continue;
              out[cell_idx(nx, ny)] = 1;
            }
          }
        }
      }
      mask.swap(out);
    }

    for (int y = 0; y < grid; ++y) {
      for (int x = 0; x < grid; ++x) {
        t.cells[cell_idx(x, y)].boundary = (mask[cell_idx(x, y)] != 0);
      }
    }
  }

  const double gen_ms = ms_since(t0);
  stats_.tiles_generated_this_frame += 1;
  stats_.gen_ms_this_frame += gen_ms;

  auto [it2, inserted] = cache_.emplace(key, std::move(t));
  (void)inserted;
  it2->second.last_used_frame = frame_index_;
  stats_.cache_tiles = static_cast<int>(cache_.size());
  return it2->second;
}

void ProcTerritoryFieldEngine::draw_territories(ImDrawList* draw,
                                                const ImVec2& origin,
                                                const ImVec2& size,
                                                const ImVec2& center_px,
                                                double scale_px_per_unit,
                                                double zoom,
                                                const Vec2& pan,
                                                const std::vector<ProcTerritorySource>& sources,
                                                double system_spacing_units,
                                                std::uint32_t seed,
                                                const ProcTerritoryFieldConfig& cfg) {
  if (!draw || !cfg.enabled) return;
  if (size.x <= 2.0f || size.y <= 2.0f) return;
  if (sources.empty()) return;

  const double denom = std::max(1e-12, scale_px_per_unit * zoom);
  const double units_per_px = 1.0 / denom;

  const int bucket = quantize_scale_bucket(units_per_px);
  const double q_units_per_px = bucket_to_units_per_px(bucket);
  const int tile_px = std::clamp(cfg.tile_px, 96, 2048);
  const int samples = std::clamp(cfg.samples_per_tile, 4, 256);
  const double tile_world = q_units_per_px * static_cast<double>(tile_px);

  const double spacing = std::max(1e-6, system_spacing_units);

  // --- Gather unique factions + build evaluation sources.
  std::vector<ProcTerritorySource> src = sources;
  const int max_src = std::max(1, cfg.max_sources);
  std::sort(src.begin(), src.end(), [](const ProcTerritorySource& a, const ProcTerritorySource& b) {
    if (a.population_millions != b.population_millions) return a.population_millions > b.population_millions;
    if (a.faction_id != b.faction_id) return a.faction_id < b.faction_id;
    if (a.pos.x != b.pos.x) return a.pos.x < b.pos.x;
    return a.pos.y < b.pos.y;
  });
  if (static_cast<int>(src.size()) > max_src) src.resize(static_cast<std::size_t>(max_src));

  std::vector<Id> factions;
  factions.reserve(src.size());
  for (const auto& s : src) {
    if (s.faction_id == kInvalidId) continue;
    factions.push_back(s.faction_id);
  }
  std::sort(factions.begin(), factions.end());
  factions.erase(std::unique(factions.begin(), factions.end()), factions.end());
  const int faction_count = static_cast<int>(factions.size());
  if (faction_count <= 0) return;

  std::vector<RGB8> colors;
  colors.reserve(factions.size());
  for (Id fid : factions) colors.push_back(faction_color(fid));

  auto faction_index = [&](Id fid) -> int {
    auto it = std::lower_bound(factions.begin(), factions.end(), fid);
    if (it == factions.end() || *it != fid) return -1;
    return static_cast<int>(std::distance(factions.begin(), it));
  };

  std::vector<EvalSource> eval;
  eval.reserve(src.size());
  for (const auto& s : src) {
    const int fi = faction_index(s.faction_id);
    if (fi < 0) continue;
    const float p = std::max(0.0f, s.population_millions);
    const float bias = std::max(0.01f, cfg.influence_pop_log_bias);
    const float logp = std::log1p(p / bias);
    const float radius = static_cast<float>(spacing) *
                         (cfg.influence_base_spacing_mult + cfg.influence_pop_spacing_mult * logp);
    EvalSource es;
    es.pos = s.pos;
    es.radius = std::max(0.0f, radius);
    es.faction_index = fi;
    es.faction_id = s.faction_id;
    eval.push_back(es);
  }
  if (eval.empty()) return;

  const std::uint64_t style_hash = compute_style_hash(cfg);
  const std::uint64_t sources_hash = compute_sources_hash(eval, spacing, cfg);

  // World bounds of the current viewport.
  auto to_world = [&](const ImVec2& p) -> Vec2 {
    return Vec2{(p.x - center_px.x) / denom - pan.x, (p.y - center_px.y) / denom - pan.y};
  };

  const Vec2 w0 = to_world(origin);
  const Vec2 w1 = to_world(ImVec2(origin.x + size.x, origin.y + size.y));

  const double min_x = std::min(w0.x, w1.x);
  const double max_x = std::max(w0.x, w1.x);
  const double min_y = std::min(w0.y, w1.y);
  const double max_y = std::max(w0.y, w1.y);

  const int tx0 = static_cast<int>(std::floor(min_x / tile_world)) - 1;
  const int ty0 = static_cast<int>(std::floor(min_y / tile_world)) - 1;
  const int tx1 = static_cast<int>(std::floor(max_x / tile_world)) + 1;
  const int ty1 = static_cast<int>(std::floor(max_y / tile_world)) + 1;

  auto to_screen = [&](const Vec2& w) -> ImVec2 {
    const float x = static_cast<float>(center_px.x + (w.x + pan.x) * denom);
    const float y = static_cast<float>(center_px.y + (w.y + pan.y) * denom);
    return ImVec2(x, y);
  };

  stats_.tiles_used_this_frame = 0;

  // Draw behind other overlays.
  ImGui::PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      TileKey key;
      key.tx = tx;
      key.ty = ty;
      key.scale_bucket = bucket;
      key.tile_px = tile_px;
      key.samples = samples;
      key.seed = seed;
      key.sources_hash = sources_hash;
      key.style_hash = style_hash;

      Tile& tile = get_or_build_tile(key, tile_world, eval, faction_count, spacing, cfg);
      tile.last_used_frame = frame_index_;

      stats_.tiles_used_this_frame += 1;

      const Vec2 tile_w0{static_cast<double>(tx) * tile_world, static_cast<double>(ty) * tile_world};
      const Vec2 tile_w1{static_cast<double>(tx + 1) * tile_world, static_cast<double>(ty + 1) * tile_world};

      const ImVec2 p0 = to_screen(tile_w0);
      const ImVec2 p1 = to_screen(tile_w1);

      const int grid = tile.grid;
      if (grid <= 0) continue;

      const float dx = (p1.x - p0.x) / static_cast<float>(grid);
      const float dy = (p1.y - p0.y) / static_cast<float>(grid);

      auto idx = [grid](int x, int y) -> std::size_t {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(grid) + static_cast<std::size_t>(x);
      };

      // Fill.
      if (cfg.draw_fill) {
        for (int y = 0; y < grid; ++y) {
          const float y0 = p0.y + static_cast<float>(y) * dy;
          const float y1p = y0 + dy;
          for (int x = 0; x < grid; ++x) {
            const Cell& c = tile.cells[idx(x, y)];
            if (c.faction_index < 0) continue;
            if (c.alpha <= 0.001f) continue;

            const RGB8 rgb = colors[static_cast<std::size_t>(c.faction_index)];
            const int a8 = std::clamp(static_cast<int>(std::lround(c.alpha * 255.0f)), 0, 255);
            if (a8 <= 0) continue;
            const ImU32 col = IM_COL32(rgb.r, rgb.g, rgb.b, a8);
            const float x0 = p0.x + static_cast<float>(x) * dx;
            const float x1p = x0 + dx;
            draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1p, y1p), col);
            stats_.cells_drawn += 1;
          }
        }
      }

      // Boundary overlay.
      if (cfg.draw_boundaries && cfg.boundary_opacity > 0.0f) {
        const int a8 = std::clamp(static_cast<int>(std::lround(cfg.boundary_opacity * 255.0f)), 0, 255);
        if (a8 > 0) {
          const ImU32 bcol = IM_COL32(255, 255, 255, a8);
          for (int y = 0; y < grid; ++y) {
            const float y0 = p0.y + static_cast<float>(y) * dy;
            const float y1p = y0 + dy;
            for (int x = 0; x < grid; ++x) {
              const Cell& c = tile.cells[idx(x, y)];
              if (!c.boundary) continue;
              if (c.faction_index < 0 && c.alpha <= 0.001f) continue;
              const float x0 = p0.x + static_cast<float>(x) * dx;
              const float x1p = x0 + dx;
              draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1p, y1p), bcol);
            }
          }
        }
      }

      if (cfg.debug_tile_bounds) {
        draw->AddRect(p0, p1, IM_COL32(255, 80, 80, 200), 0.0f, 0, 1.0f);
      }
    }
  }

  ImGui::PopClipRect();

  trim_cache(std::clamp(cfg.max_cached_tiles, 1, 20000));
  stats_.cache_tiles = static_cast<int>(cache_.size());
}

} // namespace nebula4x::ui
