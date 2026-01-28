#include "ui/proc_particle_field_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nebula4x::ui {

namespace {

constexpr float kTwoPi = 6.2831853071795864769f;

static inline float clampf(float x, float a, float b) {
  return std::max(a, std::min(b, x));
}

static inline unsigned hash_i64(std::int64_t v) {
  const std::uint64_t u = static_cast<std::uint64_t>(v);
  const unsigned lo = static_cast<unsigned>(u & 0xFFFFFFFFull);
  const unsigned hi = static_cast<unsigned>((u >> 32) & 0xFFFFFFFFull);
  // Mix hi and lo with independent hashing.
  unsigned x = lo ^ (hi * 0x9E3779B9u);
  // SplitMix32-style avalanche.
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

} // namespace

void ProcParticleFieldEngine::begin_frame(double sim_time_days, double realtime_seconds) {
  sim_time_days_ = sim_time_days;
  realtime_seconds_ = realtime_seconds;
  frame_index_++;
  stats_ = ProcParticleFieldStats{};
}

unsigned ProcParticleFieldEngine::hash_u32(unsigned x) {
  // SplitMix32 / Murmur-ish avalanche (fast, deterministic).
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

unsigned ProcParticleFieldEngine::permute(unsigned i, unsigned l, unsigned p) {
  // Implementation based on the permute() from Kensler 2013 (CMJ sampling).
  // Produces a permutation of [0..l) without storing a table.
  unsigned w = l - 1;
  w |= w >> 1;
  w |= w >> 2;
  w |= w >> 4;
  w |= w >> 8;
  w |= w >> 16;

  do {
    i ^= p;
    i *= 0xe170893du;
    i ^= p >> 16;
    i ^= (i & w) >> 4;
    i ^= p >> 8;
    i *= 0x0929eb3fu;
    i ^= p >> 23;
    i ^= (i & w) >> 1;
    i *= 1u | (p >> 27);
    i *= 0x6935fa69u;
    i ^= (i & w) >> 11;
    i *= 0x74dcb303u;
    i ^= (i & w) >> 2;
    i *= 0x9e501cc3u;
    i ^= (i & w) >> 2;
    i *= 0xc860a3dfu;
    i &= w;
    i ^= i >> 5;
  } while (i >= l);

  return (i + p) % l;
}

float ProcParticleFieldEngine::randfloat(unsigned i, unsigned p) {
  // Deterministic float in [0, 1).
  i ^= p;
  i ^= i >> 17;
  i ^= i >> 10;
  i *= 0xb36534e5u;
  i ^= i >> 12;
  i ^= i >> 21;
  i *= 0x93fc4795u;
  i ^= 0xdf6e307fu;
  i ^= i >> 17;
  i *= 1u | (p >> 18);
  // 4294967808 = 2^32 + 512, as in the paper.
  return static_cast<float>(i) * (1.0f / 4294967808.0f);
}

ImVec2 ProcParticleFieldEngine::cmj_sample(int s, int m, int n, unsigned p) {
  // Correlated multi-jittered sample in [0,1)^2.
  // N = m*n; s in [0, N).
  const int sx = static_cast<int>(permute(static_cast<unsigned>(s % m), static_cast<unsigned>(m), p * 0xa511e9b3u));
  const int sy = static_cast<int>(permute(static_cast<unsigned>(s / m), static_cast<unsigned>(n), p * 0x63d83595u));
  const float jx = randfloat(static_cast<unsigned>(s), p * 0xa399d265u);
  const float jy = randfloat(static_cast<unsigned>(s), p * 0x711ad6a5u);

  const float x = (static_cast<float>(s % m) + (static_cast<float>(sy) + jx) / static_cast<float>(n)) /
                  static_cast<float>(m);
  const float y = (static_cast<float>(s / m) + (static_cast<float>(sx) + jy) / static_cast<float>(m)) /
                  static_cast<float>(n);
  return ImVec2(x, y);
}

void ProcParticleFieldEngine::draw_particles(ImDrawList* draw,
                                            const ImVec2& origin,
                                            const ImVec2& size,
                                            ImU32 tint_color,
                                            float pan_px_x,
                                            float pan_px_y,
                                            std::uint32_t seed,
                                            const ProcParticleFieldConfig& cfg) {
  if (!draw) return;
  if (!cfg.enabled) return;
  if (cfg.opacity <= 0.0f) return;
  if (cfg.tile_px <= 8) return;
  if (cfg.particles_per_tile <= 0) return;

  const float viewport_x0 = origin.x;
  const float viewport_y0 = origin.y;
  const float viewport_x1 = origin.x + size.x;
  const float viewport_y1 = origin.y + size.y;

  const int tile_px = std::clamp(cfg.tile_px, 32, 2048);
  const int target_particles = std::clamp(cfg.particles_per_tile, 1, 4096);

  // CMJ uses N=m*n samples. We choose m=n=ceil(sqrt(target)).
  const int m = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(target_particles)))));
  const int n = m;

  // How many tiles cover the viewport (with a 1-tile margin for panning).
  const int tiles_x = static_cast<int>(std::ceil(size.x / static_cast<float>(tile_px))) + 2;
  const int tiles_y = static_cast<int>(std::ceil(size.y / static_cast<float>(tile_px))) + 2;

  const int layers = std::clamp(cfg.layers, 1, 3);

  // Precompute RGB and base alpha.
  const ImU32 rgb = tint_color & 0x00FFFFFFu;
  const int base_a = static_cast<int>((tint_color >> 24) & 0xFFu);

  const float t_real = static_cast<float>(realtime_seconds_ * static_cast<double>(cfg.twinkle_speed));
  const float t_days = static_cast<float>(sim_time_days_);

  // Avoid allocating a clip rect in ImDrawList for every call; just manual cull.
  const float pad = 2.0f;

  for (int layer = 0; layer < layers; ++layer) {
    float parallax = cfg.layer0_parallax;
    if (layer == 1) parallax = cfg.layer1_parallax;
    if (layer == 2) parallax = cfg.layer2_parallax;
    parallax = clampf(parallax, 0.0f, 1.0f);

    // Style scales by depth.
    const float layer_t = layers <= 1 ? 0.0f : (static_cast<float>(layer) / static_cast<float>(layers - 1));
    const float layer_alpha_scale = 0.65f + 0.35f * layer_t;
    const float layer_radius_scale = 0.80f + 0.45f * layer_t;
    const float layer_sparkle_scale = 0.75f + 0.45f * layer_t;

    // Derive a per-layer seed.
    const unsigned layer_seed = hash_u32(static_cast<unsigned>(seed) ^ (0x9E3779B9u + static_cast<unsigned>(layer) * 0x85ebca6bu));

    // Slow drift direction (deterministic per layer).
    float drift_x = 0.0f;
    float drift_y = 0.0f;
    if (cfg.animate_drift && cfg.drift_px_per_day != 0.0f) {
      float dx = randfloat(layer_seed ^ 0xA3C59AC3u, layer_seed * 0x63d83595u) * 2.0f - 1.0f;
      float dy = randfloat(layer_seed ^ 0x711ad6a5u, layer_seed * 0xa511e9b3u) * 2.0f - 1.0f;
      const float len = std::sqrt(dx * dx + dy * dy) + 1e-6f;
      dx /= len;
      dy /= len;
      const float d = cfg.drift_px_per_day * t_days;
      drift_x = dx * d;
      drift_y = dy * d;
    }

    const float off_x = pan_px_x * parallax + drift_x;
    const float off_y = pan_px_y * parallax + drift_y;

    const double tile_x0_d = std::floor(static_cast<double>(off_x) / static_cast<double>(tile_px));
    const double tile_y0_d = std::floor(static_cast<double>(off_y) / static_cast<double>(tile_px));
    const std::int64_t tile_x0 = static_cast<std::int64_t>(tile_x0_d);
    const std::int64_t tile_y0 = static_cast<std::int64_t>(tile_y0_d);

    const float shift_x = off_x - static_cast<float>(tile_x0_d * static_cast<double>(tile_px));
    const float shift_y = off_y - static_cast<float>(tile_y0_d * static_cast<double>(tile_px));

    const ImVec2 base(origin.x - shift_x, origin.y - shift_y);

    stats_.layers_drawn++;

    for (int iy = 0; iy < tiles_y; ++iy) {
      const std::int64_t ty = tile_y0 + static_cast<std::int64_t>(iy);
      const float tile_y = base.y + static_cast<float>(iy * tile_px);
      if (tile_y > viewport_y1 + tile_px) continue;
      if (tile_y + tile_px < viewport_y0 - tile_px) continue;

      for (int ix = 0; ix < tiles_x; ++ix) {
        const std::int64_t tx = tile_x0 + static_cast<std::int64_t>(ix);
        const float tile_x = base.x + static_cast<float>(ix * tile_px);
        if (tile_x > viewport_x1 + tile_px) continue;
        if (tile_x + tile_px < viewport_x0 - tile_px) continue;

        stats_.tiles_drawn++;

        if (cfg.debug_tile_bounds) {
          draw->AddRect(ImVec2(tile_x, tile_y), ImVec2(tile_x + tile_px, tile_y + tile_px), IM_COL32(255, 0, 255, 60));
        }

        // Tile seed mixes layer seed and tile coords.
        const unsigned tile_seed = hash_u32(layer_seed ^ hash_i64(tx) ^ (hash_i64(ty) * 0x85ebca6bu));

        for (int s = 0; s < target_particles; ++s) {
          const ImVec2 u = cmj_sample(s, m, n, tile_seed);
          const float px = tile_x + u.x * static_cast<float>(tile_px);
          const float py = tile_y + u.y * static_cast<float>(tile_px);

          if (px < viewport_x0 - pad || px > viewport_x1 + pad || py < viewport_y0 - pad || py > viewport_y1 + pad) {
            continue;
          }

          // Per-particle randomness.
          const unsigned ps = hash_u32(tile_seed ^ (static_cast<unsigned>(s) * 0x9E3779B9u));
          const float r0 = randfloat(ps, ps * 0xa399d265u);
          const float r1 = randfloat(ps, ps * 0x711ad6a5u);
          const float r2 = randfloat(ps, ps * 0x63d83595u);

          const float radius = (cfg.base_radius_px + cfg.radius_jitter_px * r0) * layer_radius_scale;

          // Twinkle (smooth sinusoid).
          float tw = 1.0f;
          if (cfg.twinkle_strength > 0.001f) {
            const float phase = r2 * kTwoPi;
            const float freq = 0.6f + 1.8f * r1;
            const float s0 = std::sin(t_real * freq + phase);
            const float osc = 0.5f + 0.5f * s0;
            tw = (1.0f - cfg.twinkle_strength) + cfg.twinkle_strength * osc;
          }

          float a = cfg.opacity * layer_alpha_scale * tw;
          a = clampf(a, 0.0f, 1.0f);
          if (a <= 0.001f) continue;

          const int aa = static_cast<int>(std::round(static_cast<float>(base_a) * a));
          if (aa <= 0) continue;
          const ImU32 col = (static_cast<ImU32>(aa) << 24) | rgb;

          // Dust particle: small quad.
          draw->AddRectFilled(ImVec2(px - radius, py - radius), ImVec2(px + radius, py + radius), col);
          stats_.particles_drawn++;

          // Optional sparkle cross.
          if (cfg.sparkles && r0 < cfg.sparkle_chance * layer_sparkle_scale) {
            const float len = (cfg.sparkle_length_px * (0.75f + 0.85f * r1)) * (0.75f + 0.65f * layer_t);
            const float a2 = clampf(a * 1.8f, 0.0f, 1.0f);
            const int aa2 = static_cast<int>(std::round(static_cast<float>(base_a) * a2));
            const ImU32 col2 = (static_cast<ImU32>(aa2) << 24) | rgb;

            // Horizontal + vertical.
            draw->AddLine(ImVec2(px - len, py), ImVec2(px + len, py), col2, 1.0f);
            draw->AddLine(ImVec2(px, py - len), ImVec2(px, py + len), col2, 1.0f);

            // Subtle diagonal for a more star-like feel.
            draw->AddLine(ImVec2(px - len * 0.55f, py - len * 0.55f), ImVec2(px + len * 0.55f, py + len * 0.55f), col2, 1.0f);
          }
        }
      }
    }
  }
}

} // namespace nebula4x::ui
