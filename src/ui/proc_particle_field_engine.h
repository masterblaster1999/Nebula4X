#pragma once

#include <cstdint>

#include <imgui.h>

namespace nebula4x::ui {

// A tiny, deterministic, screen-space particle renderer intended for map chrome.
//
// Goals
//  - Add depth / motion (dust, sparkles) to galaxy & system maps.
//  - No textures, no GL calls: only ImDrawList primitives.
//  - Deterministic distribution: a given (seed, camera pan) produces a stable
//    pattern.
//  - "Blue-noise-ish" distribution using correlated multi-jittered sampling.
//
// NOTE: This engine intentionally operates in *screen space* (pixels). The
// caller supplies the camera pan in pixels; a per-layer parallax factor is
// applied so the field scrolls at different rates, creating depth.
struct ProcParticleFieldConfig {
  bool enabled{false};

  // Tiling size in pixels. The field is evaluated on a repeating grid of tiles.
  int tile_px{256};

  // Number of particles generated per tile.
  int particles_per_tile{64};

  // Number of parallax layers (1-3 recommended).
  int layers{2};

  // Per-layer parallax (layer 0 is furthest / slowest).
  float layer0_parallax{0.10f};
  float layer1_parallax{0.28f};
  float layer2_parallax{0.55f};

  // Visual style.
  float opacity{0.22f};
  float base_radius_px{0.85f};
  float radius_jitter_px{1.15f};

  // Twinkle animation (uses realtime seconds).
  float twinkle_strength{0.55f}; // 0..1
  float twinkle_speed{1.00f};    // multiplier

  // Slow drift (uses simulation time in days so fast-forward feels alive).
  bool animate_drift{true};
  float drift_px_per_day{4.0f};

  // Occasional sparkles (small cross/star).
  bool sparkles{true};
  float sparkle_chance{0.06f};
  float sparkle_length_px{5.0f};

  // Debug: draw tile bounds.
  bool debug_tile_bounds{false};
};

struct ProcParticleFieldStats {
  int layers_drawn{0};
  int tiles_drawn{0};
  int particles_drawn{0};
};

class ProcParticleFieldEngine {
 public:
  void begin_frame(double sim_time_days, double realtime_seconds);

  ProcParticleFieldStats stats() const { return stats_; }

  // Draw a dust/sparkle field covering the rectangle [origin, origin+size).
  //
  // pan_px_x/y are camera pan in pixels (matching the starfield / procedural
  // background conventions).
  void draw_particles(ImDrawList* draw,
                      const ImVec2& origin,
                      const ImVec2& size,
                      ImU32 tint_color,
                      float pan_px_x,
                      float pan_px_y,
                      std::uint32_t seed,
                      const ProcParticleFieldConfig& cfg);

 private:
  // Correlated multi-jittered sampling helpers (Kensler 2013).
  static unsigned permute(unsigned i, unsigned l, unsigned p);
  static float randfloat(unsigned i, unsigned p);
  static ImVec2 cmj_sample(int s, int m, int n, unsigned p);

  static unsigned hash_u32(unsigned x);

  double sim_time_days_{0.0};
  double realtime_seconds_{0.0};
  std::uint64_t frame_index_{0};

  ProcParticleFieldStats stats_{};
};

} // namespace nebula4x::ui
