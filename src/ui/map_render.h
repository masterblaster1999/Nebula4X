#pragma once

#include <cstdint>

#include <imgui.h>

#include "nebula4x/core/vec2.h"

namespace nebula4x::ui {

// Shared rendering helpers for the system/galaxy maps.

struct StarfieldStyle {
  bool enabled{true};
  // How much the starfield scrolls relative to the map pan (in pixels).
  // 0 = fixed to screen, 1 = moves with the map.
  float parallax{0.15f};
  // Density multiplier (roughly linear in number of stars drawn).
  float density{1.0f};
  // Global alpha multiplier (0..1).
  float alpha{1.0f};
};

struct GridStyle {
  bool enabled{false};
  // Desired spacing of minor grid lines in pixels.
  float desired_minor_px{90.0f};
  // Every N minor lines, draw a major line.
  int major_every{5};

  // Alpha multipliers for the grid layers (0..1).
  float minor_alpha{0.10f};
  float major_alpha{0.18f};
  float axis_alpha{0.25f};

  // Draw numeric labels on major grid lines.
  bool labels{true};
  float label_alpha{0.70f};
};

struct ScaleBarStyle {
  bool enabled{true};
  // Desired bar length in pixels.
  float desired_px{120.0f};
  float alpha{0.85f};
};

// Returns a "nice" number near v using the {1,2,5} * 10^n scheme.
double nice_number_125(double v);

ImU32 modulate_alpha(ImU32 col, float alpha_mul);

// Draw a deterministic tiled starfield pattern clipped to [origin, origin+size].
// offset_px_* should generally be map pan (in pixels) so the starfield scrolls.
void draw_starfield(ImDrawList* draw,
                    const ImVec2& origin,
                    const ImVec2& size,
                    const ImU32& tint,
                    float offset_px_x,
                    float offset_px_y,
                    std::uint32_t seed,
                    const StarfieldStyle& style);

// Draw a world-aligned grid.
// center_px is the map's screen-space center.
// scale_px_per_unit is the base fit scale (before zoom).
// pan_units is the map pan in world units.
void draw_grid(ImDrawList* draw,
               const ImVec2& origin,
               const ImVec2& size,
               const ImVec2& center_px,
               double scale_px_per_unit,
               double zoom,
               const Vec2& pan_units,
               const ImU32& color,
               const GridStyle& style,
               const char* unit_suffix);

// Draw a simple scale bar anchored in the bottom-left of the map.
void draw_scale_bar(ImDrawList* draw,
                    const ImVec2& origin,
                    const ImVec2& size,
                    double units_per_px,
                    const ImU32& color,
                    const ScaleBarStyle& style,
                    const char* unit_suffix);

} // namespace nebula4x::ui
