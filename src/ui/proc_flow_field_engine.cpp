#include "ui/proc_flow_field_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace nebula4x::ui {
namespace {

constexpr double kPi = 3.14159265358979323846;

static inline float clamp01(float x) {
  return std::clamp(x, 0.0f, 1.0f);
}

static inline ImU32 modulate_alpha(ImU32 col, float alpha_mul) {
  ImVec4 v = ImGui::ColorConvertU32ToFloat4(col);
  v.w = clamp01(v.w * alpha_mul);
  return ImGui::ColorConvertFloat4ToU32(v);
}

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

static inline std::uint32_t hash_2d_i32(int x, int y, std::uint32_t seed) {
  const std::uint32_t ux = static_cast<std::uint32_t>(x);
  const std::uint32_t uy = static_cast<std::uint32_t>(y);
  std::uint32_t h = seed;
  h ^= hash_u32(ux + 0x9E3779B9u);
  h ^= (hash_u32(uy + 0x7F4A7C15u) << 1);
  return hash_u32(h);
}

struct Rng {
  std::uint32_t s{1u};
  explicit Rng(std::uint32_t seed) : s(seed ? seed : 1u) {}

  std::uint32_t next_u32() {
    // xorshift32
    std::uint32_t x = s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s = x;
    return x;
  }

  float next_f01() {
    // 24-bit mantissa -> [0,1)
    return static_cast<float>((next_u32() >> 8) * (1.0 / 16777216.0));
  }
};

static inline float fade(float t) {
  // Perlin fade curve.
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static inline float lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

static float value_noise(float x, float y, std::uint32_t seed) {
  const int xi = static_cast<int>(std::floor(x));
  const int yi = static_cast<int>(std::floor(y));
  const float tx = x - static_cast<float>(xi);
  const float ty = y - static_cast<float>(yi);

  auto h01 = [&](int ix, int iy) {
    const std::uint32_t h = hash_2d_i32(ix, iy, seed);
    return static_cast<float>(h & 0x00FFFFFFu) * (1.0f / 16777216.0f); // [0,1)
  };

  const float v00 = h01(xi, yi);
  const float v10 = h01(xi + 1, yi);
  const float v01 = h01(xi, yi + 1);
  const float v11 = h01(xi + 1, yi + 1);

  const float sx = fade(tx);
  const float sy = fade(ty);
  const float a = lerp(v00, v10, sx);
  const float b = lerp(v01, v11, sx);
  return lerp(a, b, sy);
}

static float fbm(float x,
                 float y,
                 std::uint32_t seed,
                 int octaves,
                 float lacunarity,
                 float gain) {
  float amp = 0.5f;
  float sum = 0.0f;
  float fx = x;
  float fy = y;
  std::uint32_t s = seed;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * (value_noise(fx, fy, s) * 2.0f - 1.0f); // [-1,1]
    fx *= lacunarity;
    fy *= lacunarity;
    amp *= gain;
    s = hash_u32(s + 0xA511E9B3u);
  }
  return sum;
}

struct FlowSample {
  Vec2 dir{0.0, 0.0};
  float strength{0.0f};
};

static FlowSample curl_noise_dir(const Vec2& p_mkm, std::uint32_t seed, float field_scale_mkm) {
  FlowSample out{};

  const float scale = std::max(field_scale_mkm, 1.0f);
  const float freq = 1.0f / scale;

  const float x = static_cast<float>(p_mkm.x) * freq;
  const float y = static_cast<float>(p_mkm.y) * freq;

  // Finite difference epsilon in noise-space.
  const float eps = 0.85f;

  auto n = [&](float xx, float yy) {
    // Deterministic procedural scalar field.
    return fbm(xx, yy, seed, /*octaves=*/4, /*lacunarity=*/2.0f, /*gain=*/0.55f);
  };

  const float nx1 = n(x + eps, y);
  const float nx0 = n(x - eps, y);
  const float ny1 = n(x, y + eps);
  const float ny0 = n(x, y - eps);

  const float dx = (nx1 - nx0) / (2.0f * eps);
  const float dy = (ny1 - ny0) / (2.0f * eps);

  // Perpendicular gradient -> divergence-free 2D flow.
  const double vx = static_cast<double>(dy);
  const double vy = static_cast<double>(-dx);
  const double len = std::sqrt(vx * vx + vy * vy);
  if (len <= 1e-12) return out;

  const double inv = 1.0 / len;
  out.dir = Vec2{vx * inv, vy * inv};
  out.strength = static_cast<float>(std::clamp(len, 0.0, 2.0));
  return out;
}

static inline std::uint64_t hash_combine_u64(std::uint64_t h, std::uint64_t v) {
  // 64-bit boost-ish combine.
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}

} // namespace

std::size_t ProcFlowFieldEngine::TileKeyHash::operator()(const TileKey& k) const noexcept {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.system_id));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.tx)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.ty)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.scale_bucket)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.tile_px)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.seed));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.style_hash));
  return static_cast<std::size_t>(h);
}

std::uint64_t ProcFlowFieldEngine::compute_style_hash(const ProcFlowFieldConfig& cfg) {
  std::uint64_t h = 0xBADC0FFEE0DDF00Dull;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.tile_px));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.lines_per_tile));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.steps_per_line));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(std::llround(cfg.step_px * 1000.0f)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(std::llround(cfg.field_scale_mkm * 1000.0f)));
  return h;
}

int ProcFlowFieldEngine::quantize_scale_bucket(double units_per_px_mkm) {
  // Log2 bucketed, with 1/8th octave steps to avoid thrashing while zooming.
  units_per_px_mkm = std::max(units_per_px_mkm, 1e-12);
  const double l2 = std::log2(units_per_px_mkm);
  return static_cast<int>(std::llround(l2 * 8.0));
}

double ProcFlowFieldEngine::bucket_to_units_per_px_mkm(int bucket) {
  return std::pow(2.0, static_cast<double>(bucket) / 8.0);
}

void ProcFlowFieldEngine::begin_frame(double sim_time_days) {
  ++frame_index_;

  double t = std::fmod(sim_time_days, 100000.0);
  if (!std::isfinite(t)) t = 0.0;
  time_days_ = t;

  stats_ = ProcFlowFieldStats{};
  stats_.cache_tiles = static_cast<int>(cache_.size());
}

void ProcFlowFieldEngine::clear() {
  cache_.clear();
  stats_ = ProcFlowFieldStats{};
  last_style_hash_valid_ = false;
  last_style_hash_ = 0;
}

ProcFlowFieldEngine::TileEntry& ProcFlowFieldEngine::get_or_create_tile(const TileKey& key,
                                                                        double tile_world_mkm,
                                                                        double step_world_mkm,
                                                                        const ProcFlowFieldConfig& cfg) {
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    it->second.last_used_frame = frame_index_;
    return it->second;
  }

  TileEntry entry;
  entry.last_used_frame = frame_index_;

  const int n_lines = std::clamp(cfg.lines_per_tile, 1, 128);
  const int n_steps = std::clamp(cfg.steps_per_line, 2, 512);

  const double ox = static_cast<double>(key.tx) * tile_world_mkm;
  const double oy = static_cast<double>(key.ty) * tile_world_mkm;

  // Deterministic RNG per tile.
  const std::uint32_t base = hash_u32(static_cast<std::uint32_t>(key.system_id) ^ key.seed);
  std::uint32_t tile_seed = hash_2d_i32(key.tx, key.ty, base ^ static_cast<std::uint32_t>(key.scale_bucket));
  tile_seed ^= static_cast<std::uint32_t>(key.style_hash);
  Rng rng(hash_u32(tile_seed));

  const int grid = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n_lines)))));
  const double margin = tile_world_mkm * 0.25;

  entry.lines.reserve(static_cast<std::size_t>(n_lines));

  for (int i = 0; i < n_lines; ++i) {
    const int gx = i % grid;
    const int gy = i / grid;

    const double u = (static_cast<double>(gx) + rng.next_f01()) / static_cast<double>(grid);
    const double v = (static_cast<double>(gy) + rng.next_f01()) / static_cast<double>(grid);

    Vec2 p{ox + u * tile_world_mkm, oy + v * tile_world_mkm};

    Streamline line;
    line.pts_mkm.reserve(static_cast<std::size_t>(n_steps + 1));
    line.s_mkm.reserve(static_cast<std::size_t>(n_steps + 1));

    line.pts_mkm.push_back(p);
    line.s_mkm.push_back(0.0f);

    double acc = 0.0;

    for (int s = 0; s < n_steps; ++s) {
      const FlowSample a = curl_noise_dir(p, key.seed, cfg.field_scale_mkm);
      if (a.strength <= 1e-6f) break;

      // RK2/midpoint integration for smoother curves.
      const Vec2 mid = p + a.dir * (0.5 * step_world_mkm);
      const FlowSample b = curl_noise_dir(mid, key.seed, cfg.field_scale_mkm);
      const Vec2 dir = (b.strength > 1e-6f) ? b.dir : a.dir;

      const Vec2 p2 = p + dir * step_world_mkm;
      if (!finite_vec(p2)) break;

      acc += step_world_mkm;
      line.pts_mkm.push_back(p2);
      line.s_mkm.push_back(static_cast<float>(acc));
      p = p2;

      // Keep lines mostly local so tile caches stay coherent.
      if (p.x < (ox - margin) || p.x > (ox + tile_world_mkm + margin) ||
          p.y < (oy - margin) || p.y > (oy + tile_world_mkm + margin)) {
        break;
      }
    }

    if (line.pts_mkm.size() >= 2) {
      entry.lines.push_back(std::move(line));
    }
  }

  auto [ins_it, _] = cache_.emplace(key, std::move(entry));
  stats_.tiles_generated_this_frame += 1;
  return ins_it->second;
}

void ProcFlowFieldEngine::trim_cache(int max_tiles) {
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

void ProcFlowFieldEngine::draw_streamlines(ImDrawList* draw,
                                          const ImVec2& origin,
                                          const ImVec2& size,
                                          const ImVec2& center,
                                          double scale_px_per_mkm,
                                          double zoom,
                                          const Vec2& pan_mkm,
                                          const Simulation& sim,
                                          Id system_id,
                                          std::uint32_t seed,
                                          const ProcFlowFieldConfig& cfg_in,
                                          ImU32 base_color) {
  if (!draw) return;

  ProcFlowFieldConfig cfg = cfg_in;
  cfg.tile_px = std::clamp(cfg.tile_px, 64, 1024);
  cfg.max_cached_tiles = std::clamp(cfg.max_cached_tiles, 0, 4096);
  cfg.lines_per_tile = std::clamp(cfg.lines_per_tile, 1, 128);
  cfg.steps_per_line = std::clamp(cfg.steps_per_line, 2, 512);
  cfg.step_px = std::clamp(cfg.step_px, 1.0f, 64.0f);
  cfg.thickness_px = std::clamp(cfg.thickness_px, 0.5f, 10.0f);
  cfg.opacity = clamp01(cfg.opacity);
  cfg.animate_speed_cycles_per_day = std::clamp(cfg.animate_speed_cycles_per_day, 0.0f, 10.0f);
  cfg.highlight_wavelength_px = std::clamp(cfg.highlight_wavelength_px, 20.0f, 2000.0f);
  cfg.nebula_threshold = clamp01(cfg.nebula_threshold);
  cfg.storm_threshold = clamp01(cfg.storm_threshold);
  cfg.field_scale_mkm = std::clamp(cfg.field_scale_mkm, 250.0f, 250000.0f);

  if (!cfg.enabled || cfg.opacity <= 1e-5f) return;
  if (scale_px_per_mkm <= 0.0 || zoom <= 0.0) return;

  const std::uint64_t style_hash = compute_style_hash(cfg);
  if (last_style_hash_valid_ && style_hash != last_style_hash_) {
    cache_.clear();
  }
  last_style_hash_ = style_hash;
  last_style_hash_valid_ = true;

  const double units_per_px_mkm = 1.0 / (scale_px_per_mkm * zoom);
  const int bucket = quantize_scale_bucket(units_per_px_mkm);
  const double bucket_units = bucket_to_units_per_px_mkm(bucket);

  const double tile_world_mkm = std::max(1e-9, static_cast<double>(cfg.tile_px) * bucket_units);
  const double step_world_mkm = std::max(1e-9, static_cast<double>(cfg.step_px) * bucket_units);
  const double wave_world_mkm = std::max(1e-6, static_cast<double>(cfg.highlight_wavelength_px) * bucket_units);

  const Vec2 w0 = screen_to_world(origin, center, scale_px_per_mkm, zoom, pan_mkm);
  const Vec2 w1 = screen_to_world(ImVec2(origin.x + size.x, origin.y + size.y), center, scale_px_per_mkm, zoom, pan_mkm);

  const double min_x = std::min(w0.x, w1.x);
  const double max_x = std::max(w0.x, w1.x);
  const double min_y = std::min(w0.y, w1.y);
  const double max_y = std::max(w0.y, w1.y);

  auto tile_of = [&](double v) -> int {
    return static_cast<int>(std::floor(v / tile_world_mkm));
  };

  int tx0 = tile_of(min_x) - 1;
  int tx1 = tile_of(max_x) + 1;
  int ty0 = tile_of(min_y) - 1;
  int ty1 = tile_of(max_y) + 1;

  // Avoid pathological zoom states.
  tx0 = std::clamp(tx0, -1000000, 1000000);
  tx1 = std::clamp(tx1, -1000000, 1000000);
  ty0 = std::clamp(ty0, -1000000, 1000000);
  ty1 = std::clamp(ty1, -1000000, 1000000);

  if (tx1 < tx0 || ty1 < ty0) return;

  draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

  const bool storms_enabled = sim.cfg().enable_nebula_storms;

  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      TileKey key;
      key.system_id = system_id;
      key.tx = tx;
      key.ty = ty;
      key.scale_bucket = bucket;
      key.tile_px = cfg.tile_px;
      key.seed = seed;
      key.style_hash = style_hash;

      TileEntry& tile = get_or_create_tile(key, tile_world_mkm, step_world_mkm, cfg);
      tile.last_used_frame = frame_index_;
      stats_.tiles_used_this_frame += 1;

      if (cfg.debug_tile_bounds) {
        const double ox = static_cast<double>(tx) * tile_world_mkm;
        const double oy = static_cast<double>(ty) * tile_world_mkm;
        const Vec2 a{ox, oy};
        const Vec2 b{ox + tile_world_mkm, oy + tile_world_mkm};
        ImVec2 p0 = world_to_screen(a, center, scale_px_per_mkm, zoom, pan_mkm);
        ImVec2 p1 = world_to_screen(b, center, scale_px_per_mkm, zoom, pan_mkm);
        const ImVec2 r0(std::min(p0.x, p1.x), std::min(p0.y, p1.y));
        const ImVec2 r1(std::max(p0.x, p1.x), std::max(p0.y, p1.y));
        draw->AddRect(r0, r1, IM_COL32(255, 255, 0, 40));
      }

      for (const Streamline& line : tile.lines) {
        if (line.pts_mkm.size() < 2 || line.s_mkm.size() != line.pts_mkm.size()) continue;

        bool any_segment = false;

        for (std::size_t i = 0; i + 1 < line.pts_mkm.size(); ++i) {
          const Vec2& a = line.pts_mkm[i];
          const Vec2& b = line.pts_mkm[i + 1];

          const Vec2 mid{0.5 * (a.x + b.x), 0.5 * (a.y + b.y)};

          float mask = 1.0f;
          if (cfg.mask_by_nebula) {
            const float d = static_cast<float>(sim.system_nebula_density_at(system_id, mid));
            if (d <= cfg.nebula_threshold) continue;
            const float t = (d - cfg.nebula_threshold) / std::max(1e-6f, 1.0f - cfg.nebula_threshold);
            mask *= clamp01(t);
          }
          if (cfg.mask_by_storms && storms_enabled) {
            const float st = static_cast<float>(sim.system_storm_intensity_at(system_id, mid));
            if (st <= cfg.storm_threshold) continue;
            const float t = (st - cfg.storm_threshold) / std::max(1e-6f, 1.0f - cfg.storm_threshold);
            mask *= clamp01(t);
          }
          if (mask <= 1e-4f) continue;

          float anim = 1.0f;
          if (cfg.animate && cfg.animate_speed_cycles_per_day > 1e-6f) {
            const double s_mid = 0.5 * (static_cast<double>(line.s_mkm[i]) + static_cast<double>(line.s_mkm[i + 1]));
            double phase = (s_mid / wave_world_mkm) + (time_days_ * static_cast<double>(cfg.animate_speed_cycles_per_day));
            phase -= std::floor(phase);
            const double w = 0.5 + 0.5 * std::sin(phase * 2.0 * kPi);
            // Sharpen the highlight without hard dashes.
            anim = static_cast<float>(0.30 + 0.70 * std::pow(w, 1.65));
          }

          const float alpha = cfg.opacity * mask * anim;
          if (alpha <= 1e-4f) continue;

          const ImVec2 pa = world_to_screen(a, center, scale_px_per_mkm, zoom, pan_mkm);
          const ImVec2 pb = world_to_screen(b, center, scale_px_per_mkm, zoom, pan_mkm);

          draw->AddLine(pa, pb, modulate_alpha(base_color, alpha), cfg.thickness_px);
          any_segment = true;
          stats_.segments_drawn += 1;
        }

        if (any_segment) stats_.lines_drawn += 1;
      }
    }
  }

  draw->PopClipRect();

  trim_cache(cfg.max_cached_tiles);
  stats_.cache_tiles = static_cast<int>(cache_.size());
}

} // namespace nebula4x::ui
