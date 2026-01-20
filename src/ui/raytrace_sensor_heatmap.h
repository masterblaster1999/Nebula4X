#pragma once

#include <cstdint>
#include <vector>

#include <imgui.h>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x {
class Simulation;
}

namespace nebula4x::ui {

// A single sensor source used for ray-traced (LOS shaded) heatmap rendering.
//
// range_mkm is expected to already include any desired target signature
// multiplier, EMCON tweaks, etc. env_src_multiplier should reflect the
// *source-side* local sensor environment multiplier at pos_mkm.
struct RaytraceSensorSource {
  Vec2 pos_mkm{0.0, 0.0};
  double range_mkm{0.0};
  double env_src_multiplier{1.0};
  float weight{1.0f};
};

// Tunables for the LOS ray-traced sensor heatmap.
//
// This overlay is currently UI-only: it visualizes how in-system "terrain"
// (nebula microfields + storm cells) could attenuate sensor coverage along the
// line-of-sight path between a source and a point.
struct SensorRaytraceHeatmapSettings {
  // Adaptive quadtree max depth (0 = single quad, higher = finer detail).
  int max_depth{6};
  // Subdivide when max corner/center delta exceeds this threshold.
  float error_threshold{0.06f};
  // Stochastic samples per leaf quad (1 = deterministic center sample).
  int spp{1};

  // Number of stratified samples used to estimate LOS attenuation.
  // Higher = smoother, slower.
  int los_samples{8};
  // Strength in [0,1] for applying LOS attenuation.
  // 0 => legacy sensor heatmap behavior.
  // 1 => full LOS shading.
  float los_strength{0.85f};

  // Perceptual shaping of the heatmap field (like the fast grid heatmap).
  float gamma{0.75f};

  bool debug{false};
};

struct SensorRaytraceHeatmapStats {
  int quads_tested{0};
  int quads_leaf{0};
  int point_evals{0};
  int los_env_samples{0};
};

// Draw a sensor coverage heatmap using adaptive subdivision and stochastic
// sampling, with an optional line-of-sight environmental attenuation term.
//
// The heatmap returns values in [0,1] similar to the existing fast grid
// heatmap, but produces smoother results and reveals "clear lanes" / "dense
// curtains" created by nebula microfields.
void draw_raytraced_sensor_heatmap(ImDrawList* draw,
                                  const ImVec2& origin,
                                  const ImVec2& avail,
                                  const ImVec2& center_px,
                                  double scale_px_per_mkm,
                                  double zoom,
                                  const Vec2& pan_mkm,
                                  const Simulation& sim,
                                  Id system_id,
                                  const std::vector<RaytraceSensorSource>& sources,
                                  ImU32 base_col,
                                  float opacity,
                                  std::uint32_t seed,
                                  const SensorRaytraceHeatmapSettings& settings,
                                  SensorRaytraceHeatmapStats* out_stats);

} // namespace nebula4x::ui
