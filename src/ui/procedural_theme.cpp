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

ImVec4 lerp_color(const ImVec4& a, const ImVec4& b, float t) {
  const float tt = clamp01(t);
  return ImVec4(
      a.x + (b.x - a.x) * tt,
      a.y + (b.y - a.y) * tt,
      a.z + (b.z - a.z) * tt,
      a.w + (b.w - a.w) * tt);
}

float srgb_to_linear(float v) {
  const float c = clamp01(v);
  if (c <= 0.04045f) return c / 12.92f;
  return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float relative_luminance(const ImVec4& c) {
  // WCAG relative luminance (alpha is ignored).
  const float r = srgb_to_linear(c.x);
  const float g = srgb_to_linear(c.y);
  const float b = srgb_to_linear(c.z);
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

float contrast_ratio(const ImVec4& a, const ImVec4& b) {
  float la = relative_luminance(a);
  float lb = relative_luminance(b);
  if (la < lb) std::swap(la, lb);
  return (la + 0.05f) / (lb + 0.05f);
}

ImVec4 ensure_min_contrast(ImVec4 fg, const ImVec4& bg, float min_ratio) {
  float best_ratio = contrast_ratio(fg, bg);
  if (best_ratio >= min_ratio) return fg;

  ImVec4 light = ImVec4(0.96f, 0.97f, 0.99f, fg.w);
  ImVec4 dark = ImVec4(0.08f, 0.10f, 0.12f, fg.w);
  const float light_ratio = contrast_ratio(light, bg);
  const float dark_ratio = contrast_ratio(dark, bg);
  const ImVec4 target = (light_ratio >= dark_ratio) ? light : dark;

  ImVec4 best = fg;
  for (int i = 1; i <= 8; ++i) {
    const float t = static_cast<float>(i) / 8.0f;
    const ImVec4 cand = lerp_color(fg, target, t);
    const float cr = contrast_ratio(cand, bg);
    if (cr > best_ratio) {
      best_ratio = cr;
      best = cand;
    }
    if (cr >= min_ratio) return cand;
  }
  return best;
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
  const float s = clamp01(p.accent_strength);

  // Backgrounds.
  c[ImGuiCol_WindowBg] = pal.bg_window;
  c[ImGuiCol_ChildBg] = pal.bg_child;
  c[ImGuiCol_PopupBg] = pal.bg_popup;

  // Contrast-aware text colors so generated themes remain readable.
  ImVec4 text_seed = lerp_color(ImVec4(0.90f, 0.93f, 0.98f, 1.0f), acc3, 0.08f);
  c[ImGuiCol_Text] = ensure_min_contrast(text_seed, pal.bg_window, 6.2f);
  c[ImGuiCol_TextDisabled] = ensure_min_contrast(lerp_color(c[ImGuiCol_Text], pal.bg_window, 0.52f), pal.bg_window, 3.0f);

  // Cohesive "base" chrome so accent overlays don't feel disconnected.
  c[ImGuiCol_Border] = with_alpha(lerp_color(acc2, c[ImGuiCol_TextDisabled], 0.50f), std::clamp(0.22f + s * 0.22f, 0.18f, 0.70f));
  c[ImGuiCol_Separator] = with_alpha(lerp_color(acc2, c[ImGuiCol_TextDisabled], 0.35f), std::clamp(0.20f + s * 0.20f, 0.16f, 0.68f));
  c[ImGuiCol_FrameBg] = with_alpha(lerp_color(pal.bg_child, acc2, 0.10f), 0.76f);
  c[ImGuiCol_Button] = with_alpha(lerp_color(c[ImGuiCol_FrameBg], acc, 0.18f), std::clamp(0.55f + s * 0.15f, 0.48f, 0.90f));
  c[ImGuiCol_Header] = with_alpha(lerp_color(c[ImGuiCol_FrameBg], acc, 0.26f), std::clamp(0.52f + s * 0.18f, 0.44f, 0.92f));
  c[ImGuiCol_Tab] = with_alpha(lerp_color(pal.bg_window, acc2, 0.16f), 0.90f);
  c[ImGuiCol_TabUnfocused] = with_alpha(lerp_color(c[ImGuiCol_Tab], pal.bg_window, 0.38f), 0.84f);
  c[ImGuiCol_TitleBg] = with_alpha(lerp_color(pal.bg_window, acc3, 0.10f), 0.98f);
  c[ImGuiCol_TitleBgActive] = with_alpha(lerp_color(c[ImGuiCol_TitleBg], acc, 0.16f), 1.0f);
  c[ImGuiCol_MenuBarBg] = with_alpha(lerp_color(c[ImGuiCol_TitleBg], pal.bg_window, 0.30f), 0.96f);
  c[ImGuiCol_ScrollbarBg] = with_alpha(pal.bg_child, 0.90f);
  c[ImGuiCol_ScrollbarGrab] = with_alpha(lerp_color(acc2, c[ImGuiCol_TextDisabled], 0.40f), std::clamp(0.34f + s * 0.20f, 0.26f, 0.75f));
  c[ImGuiCol_ScrollbarGrabHovered] = with_alpha(acc, std::clamp(0.40f + s * 0.20f, 0.30f, 0.82f));
  c[ImGuiCol_ScrollbarGrabActive] = with_alpha(acc, std::clamp(0.55f + s * 0.25f, 0.40f, 0.95f));
  c[ImGuiCol_TableRowBgAlt] = with_alpha(lerp_color(pal.bg_child, acc2, 0.12f), 0.22f);
  c[ImGuiCol_ModalWindowDimBg] = with_alpha(ImVec4(0.03f, 0.04f, 0.06f, 1.0f), 0.56f);

  // Accent-driven interactions.
  const float hover_a = std::clamp(0.12f + s * 0.30f, 0.08f, 0.55f);
  const float active_a = std::clamp(0.22f + s * 0.40f, 0.14f, 0.75f);

  c[ImGuiCol_CheckMark] = acc;
  c[ImGuiCol_SliderGrab] = with_alpha(acc, 0.70f);
  c[ImGuiCol_SliderGrabActive] = acc;

  c[ImGuiCol_FrameBgHovered] = with_alpha(acc, hover_a);
  c[ImGuiCol_FrameBgActive] = with_alpha(acc, active_a);

  c[ImGuiCol_ButtonHovered] = with_alpha(acc, hover_a);
  c[ImGuiCol_ButtonActive] = with_alpha(acc, active_a);

  c[ImGuiCol_Header] = with_alpha(lerp_color(c[ImGuiCol_Header], acc, 0.35f), std::clamp(0.16f + s * 0.20f, 0.10f, 0.55f));
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
  c[ImGuiCol_DragDropTarget] = with_alpha(acc, std::clamp(0.62f + s * 0.25f, 0.50f, 1.0f));

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
