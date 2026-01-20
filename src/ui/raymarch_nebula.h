#pragma once

#include <cstdint>

#include <imgui.h>

namespace nebula4x::ui {

// A lightweight signed-distance-field (SDF) raymarcher used for subtle
// background chrome on the maps.
//
// Design goals:
//  - Fast enough for immediate-mode UI (adaptive subdivision + early-outs)
//  - Deterministic per seed + camera pan (no flicker)
//  - Tunable quality/performance knobs exposed to the player.

struct RaymarchNebulaStyle {
  bool enabled{false};

  // Global alpha multiplier (0..1). Keep subtle: 0.05..0.35 recommended.
  float alpha{0.18f};

  // How much the field scrolls relative to the map pan (in pixels).
  // 0 = fixed to screen, 1 = moves with the map.
  float parallax{0.06f};

  // Adaptive subdivision depth (higher = sharper, slower).
  int max_depth{6};

  // Error threshold for subdivision. Lower = more detail, slower.
  float error_threshold{0.05f};

  // Stochastic samples per node evaluation (anti-aliasing / noise reduction).
  int spp{1};

  // Ray-march steps (higher = fewer artifacts, slower).
  int max_steps{48};

  // Animate the field slowly (purely cosmetic).
  bool animate{true};
  float time_scale{0.20f};

  // Debug overlay (draws stats text at the top-left of the rect).
  bool debug_overlay{false};
};

struct RaymarchNebulaStats {
  int quads_drawn{0};
  int nodes_split{0};
  int shade_calls{0};
  int rays_cast{0};
  int steps_total{0};
  int max_depth_reached{0};
};

void draw_raymarched_nebula(ImDrawList* draw,
                            const ImVec2& origin,
                            const ImVec2& size,
                            ImU32 bg_tint,
                            float offset_px_x,
                            float offset_px_y,
                            std::uint32_t seed,
                            const RaymarchNebulaStyle& style,
                            RaymarchNebulaStats* out_stats = nullptr);

}  // namespace nebula4x::ui
