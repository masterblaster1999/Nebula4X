#pragma once

// Centralized ImGui includes for Nebula4X UI code.
//
// Keeping this in a single header avoids "include roulette" across windows
// (some need ImVec2/ImDrawList, others need std::string InputText helpers).

#include <imgui.h>

// Optional helpers: std::string overloads for InputText/Combo, etc.
#include <misc/cpp/imgui_stdlib.h>

// ---- Nebula4X ImGui extensions ----
//
// Keep small compatibility helpers here to avoid scattering version-dependent
// ImGui calls across the codebase.

namespace ImGui {

// Dear ImGui doesn't provide a SliderDouble() helper, but Nebula4X frequently
// tweaks values stored as doubles.
//
// Implementation uses SliderScalar with ImGuiDataType_Double.
//
// Compatibility: older ImGui versions used a `power` parameter instead of
// ImGuiSliderFlags (the change landed around v1.78).
#if defined(IMGUI_VERSION_NUM) && (IMGUI_VERSION_NUM >= 17800)
inline bool SliderDouble(const char* label, double* v, double v_min, double v_max, const char* format = "%.3f",
                         ImGuiSliderFlags flags = 0) {
  return SliderScalar(label, ImGuiDataType_Double, v, &v_min, &v_max, format, flags);
}
#else
inline bool SliderDouble(const char* label, double* v, double v_min, double v_max, const char* format = "%.3f",
                         float power = 1.0f) {
  return SliderScalar(label, ImGuiDataType_Double, v, &v_min, &v_max, format, power);
}
#endif

}  // namespace ImGui
