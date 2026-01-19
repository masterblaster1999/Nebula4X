#include "ui/procedural_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nebula4x::ui {

namespace {

float clamp01(float v) {
  return std::max(0.0f, std::min(1.0f, v));
}

float wrap_deg(float d) {
  // Keep in [0, 360).
  d = std::fmod(d, 360.0f);
  if (d < 0.0f) d += 360.0f;
  return d;
}

std::uint32_t mix_u32(std::uint32_t x) {
  // A tiny integer hash (splitmix32-ish) to turn a seed into reasonably
  // distributed bits.
  x += 0x9e3779b9u;
  x = (x ^ (x >> 16)) * 0x85ebca6bu;
  x = (x ^ (x >> 13)) * 0xc2b2ae35u;
  x = x ^ (x >> 16);
  return x;
}

float seed_to_hue_deg(std::int32_t seed) {
  const std::uint32_t h = mix_u32(static_cast<std::uint32_t>(seed));
  // Map to [0, 360).
  return (static_cast<float>(h) / 4294967296.0f) * 360.0f;
}

ImVec4 hsv_deg(float deg, float s, float v, float a) {
  const float h = wrap_deg(deg) / 360.0f;
  const float ss = clamp01(s);
  const float vv = clamp01(v);
  const float aa = clamp01(a);
  const ImColor c = ImColor::HSV(h, ss, vv, aa);
  return ImVec4(c.Value.x, c.Value.y, c.Value.z, c.Value.w);
}

ImVec4 with_alpha(ImVec4 c, float a) {
  c.w = clamp01(a);
  return c;
}

} // namespace

ProceduralThemePalette compute_procedural_theme_palette(const ProceduralThemeParams& p, float time_sec) {
  ProceduralThemePalette pal;

  const float sat = clamp01(p.saturation);
  const float val = clamp01(p.value);
  const float bg_v = clamp01(p.bg_value);

  float h1 = p.use_seed_hue ? seed_to_hue_deg(p.seed) : p.hue_deg;
  if (p.animate_hue) {
    h1 += time_sec * p.animate_speed_deg_per_sec;
  }
  h1 = wrap_deg(h1);

  float h2 = h1;
  float h3 = h1;

  switch (p.variant) {
    case 0: // Analogous
      h2 = wrap_deg(h1 + 30.0f);
      h3 = wrap_deg(h1 - 30.0f);
      break;
    case 1: // Complementary
      h2 = wrap_deg(h1 + 180.0f);
      h3 = wrap_deg(h1 + 150.0f);
      break;
    case 2: // Triad
      h2 = wrap_deg(h1 + 120.0f);
      h3 = wrap_deg(h1 + 240.0f);
      break;
    case 3: // Monochrome
    default:
      h2 = h1;
      h3 = h1;
      break;
  }

  pal.accent_primary = hsv_deg(h1, sat, val, 1.0f);
  pal.accent_secondary = hsv_deg(h2, clamp01(sat * 0.92f), clamp01(val * 0.92f), 1.0f);
  pal.accent_tertiary = hsv_deg(h3, clamp01(sat * 0.75f), clamp01(val * 0.85f), 1.0f);

  // Backgrounds: low saturation tint so it feels cohesive but remains readable.
  const float bg_sat = clamp01(std::min(0.18f, sat * 0.12f));
  pal.bg_window = hsv_deg(h1, bg_sat, bg_v, 0.94f);
  pal.bg_child = hsv_deg(h1, clamp01(bg_sat * 0.85f), clamp01(bg_v * 0.92f), 0.94f);
  pal.bg_popup = hsv_deg(h1, clamp01(bg_sat * 0.95f), clamp01(bg_v * 1.06f), 0.96f);

  // Suggested non-ImGui colors.
  pal.clear_color = hsv_deg(h1, clamp01(bg_sat * 0.55f), clamp01(bg_v * 0.25f), 1.0f);
  pal.system_map_bg = hsv_deg(h1, clamp01(bg_sat * 0.70f), clamp01(bg_v * 1.05f), 1.0f);
  pal.galaxy_map_bg = hsv_deg(h2, clamp01(bg_sat * 0.65f), clamp01(bg_v * 0.95f), 1.0f);

  return pal;
}

void apply_procedural_theme(ImGuiStyle& style, const ProceduralThemeParams& p, float time_sec) {
  const ProceduralThemePalette pal = compute_procedural_theme_palette(p, time_sec);

  // Chrome/rounding. Keep it sci-fi friendly but readable.
  style.WindowRounding = 6.0f;
  style.ChildRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.PopupRounding = 6.0f;
  style.ScrollbarRounding = 6.0f;
  style.GrabRounding = 4.0f;
  style.TabRounding = 4.0f;

  if (p.variant == 3) {
    // Monochrome reads more "industrial".
    style.WindowRounding = 3.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 2.0f;
    style.TabRounding = 2.0f;
  }

  ImVec4* c = style.Colors;
  const ImVec4 acc = pal.accent_primary;
  const ImVec4 acc2 = pal.accent_secondary;
  const ImVec4 acc3 = pal.accent_tertiary;

  // Backgrounds.
  c[ImGuiCol_WindowBg] = pal.bg_window;
  c[ImGuiCol_ChildBg] = pal.bg_child;
  c[ImGuiCol_PopupBg] = pal.bg_popup;

  // Accent-driven interactions.
  const float s = clamp01(p.accent_strength);
  const float hover_a = std::clamp(0.12f + s * 0.30f, 0.08f, 0.55f);
  const float active_a = std::clamp(0.22f + s * 0.40f, 0.14f, 0.75f);

  c[ImGuiCol_CheckMark] = acc;
  c[ImGuiCol_SliderGrab] = with_alpha(acc, 0.70f);
  c[ImGuiCol_SliderGrabActive] = acc;

  c[ImGuiCol_FrameBgHovered] = with_alpha(acc, hover_a);
  c[ImGuiCol_FrameBgActive] = with_alpha(acc, active_a);

  c[ImGuiCol_ButtonHovered] = with_alpha(acc, hover_a);
  c[ImGuiCol_ButtonActive] = with_alpha(acc, active_a);

  c[ImGuiCol_Header] = with_alpha(acc, std::clamp(0.08f + s * 0.20f, 0.06f, 0.35f));
  c[ImGuiCol_HeaderHovered] = with_alpha(acc, hover_a);
  c[ImGuiCol_HeaderActive] = with_alpha(acc, active_a);

  c[ImGuiCol_SeparatorHovered] = with_alpha(acc, std::clamp(0.25f + s * 0.35f, 0.18f, 0.80f));
  c[ImGuiCol_SeparatorActive] = with_alpha(acc, std::clamp(0.45f + s * 0.35f, 0.25f, 1.0f));

  c[ImGuiCol_ResizeGripHovered] = with_alpha(acc, std::clamp(0.20f + s * 0.35f, 0.12f, 0.80f));
  c[ImGuiCol_ResizeGripActive] = with_alpha(acc, std::clamp(0.35f + s * 0.45f, 0.18f, 1.0f));

  c[ImGuiCol_TabHovered] = with_alpha(acc, std::clamp(0.10f + s * 0.22f, 0.08f, 0.45f));
  c[ImGuiCol_TabActive] = with_alpha(acc, std::clamp(0.18f + s * 0.26f, 0.12f, 0.60f));
  c[ImGuiCol_TabUnfocusedActive] = with_alpha(acc, std::clamp(0.08f + s * 0.12f, 0.05f, 0.35f));

  c[ImGuiCol_NavHighlight] = with_alpha(acc, std::clamp(0.55f + s * 0.30f, 0.45f, 1.0f));
  c[ImGuiCol_TextSelectedBg] = with_alpha(acc, std::clamp(0.18f + s * 0.26f, 0.12f, 0.60f));
  c[ImGuiCol_DockingPreview] = with_alpha(acc, std::clamp(0.35f + s * 0.25f, 0.25f, 0.75f));

  // Plots.
  c[ImGuiCol_PlotLines] = acc2;
  c[ImGuiCol_PlotLinesHovered] = acc3;
  c[ImGuiCol_PlotHistogram] = acc2;
  c[ImGuiCol_PlotHistogramHovered] = acc3;

  // Tables: slightly tinted headers so large tables scan better.
  c[ImGuiCol_TableHeaderBg] = with_alpha(acc, std::clamp(0.05f + s * 0.18f, 0.03f, 0.35f));
  c[ImGuiCol_TableBorderStrong] = with_alpha(acc2, std::clamp(0.18f + s * 0.30f, 0.12f, 0.70f));
  c[ImGuiCol_TableBorderLight] = with_alpha(acc2, std::clamp(0.10f + s * 0.20f, 0.06f, 0.50f));
}

void palette_to_float4(const ImVec4& c, float out[4]) {
  if (!out) return;
  out[0] = clamp01(c.x);
  out[1] = clamp01(c.y);
  out[2] = clamp01(c.z);
  out[3] = clamp01(c.w);
}

} // namespace nebula4x::ui
