#pragma once

#include <imgui.h>

#include <cstdint>

namespace nebula4x::ui {

// Parameters controlling the procedural UI theme.
//
// The goal is to let players create/share a cohesive UI skin using a compact
// "theme DNA" (seed + a few knobs) rather than editing dozens of colors.
struct ProceduralThemeParams {
  std::int32_t seed{1337};

  // If true, hue is derived from the seed; otherwise hue_deg is used directly.
  bool use_seed_hue{true};
  float hue_deg{190.0f}; // 0..360, used when !use_seed_hue

  // 0=Analogous, 1=Complementary, 2=Triad, 3=Monochrome
  int variant{0};

  // Accent HSV knobs.
  float saturation{0.72f}; // 0..1
  float value{0.90f};      // 0..1

  // Background brightness ("value" in HSV). 0..0.25 is typical for dark themes.
  float bg_value{0.11f}; // 0..1

  // Strength of accent overlays used for hover/active/focus highlights.
  float accent_strength{0.28f}; // 0..1

  // Optional hue animation (purely aesthetic).
  bool animate_hue{false};
  float animate_speed_deg_per_sec{6.0f};

  // When enabled, the palette is also exported for SDL clear + map backgrounds.
  bool sync_backgrounds{false};
};

// A fully derived palette.
struct ProceduralThemePalette {
  ImVec4 accent_primary{0, 0, 0, 1};
  ImVec4 accent_secondary{0, 0, 0, 1};
  ImVec4 accent_tertiary{0, 0, 0, 1};

  ImVec4 bg_window{0, 0, 0, 1};
  ImVec4 bg_child{0, 0, 0, 1};
  ImVec4 bg_popup{0, 0, 0, 1};

  // Suggested non-ImGui colors.
  ImVec4 clear_color{0, 0, 0, 1};
  ImVec4 system_map_bg{0, 0, 0, 1};
  ImVec4 galaxy_map_bg{0, 0, 0, 1};
};

// Compute a palette given params + current time.
ProceduralThemePalette compute_procedural_theme_palette(const ProceduralThemeParams& p, float time_sec);

// Apply a procedural theme palette to an ImGuiStyle.
//
// Caller is expected to start from an existing base style (e.g. StyleColorsDark)
// and then call this to apply accent/background overrides.
void apply_procedural_theme(ImGuiStyle& style, const ProceduralThemeParams& p, float time_sec);

// Helper: export a color into float[4] arrays.
void palette_to_float4(const ImVec4& c, float out[4]);

} // namespace nebula4x::ui
