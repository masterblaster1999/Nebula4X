#pragma once

#include <algorithm>

#include <imgui.h>

#include "nebula4x/core/vec2.h"

namespace nebula4x::ui {

// Small, header-only helper for drawing an interactive distance ruler on maps.
//
// Notes:
// - This is UI-only state (does not touch the simulation).
// - The caller is responsible for converting between world and screen coordinates.

struct RulerState {
  bool has_start{false};
  bool dragging{false};
  Vec2 start{0.0, 0.0};
  Vec2 end{0.0, 0.0};

  [[nodiscard]] bool active() const { return has_start; }
  [[nodiscard]] double distance_world() const { return (end - start).length(); }
};

inline void clear_ruler(RulerState& r) { r = RulerState{}; }

inline void begin_ruler(RulerState& r, const Vec2& p) {
  r.has_start = true;
  r.dragging = true;
  r.start = p;
  r.end = p;
}

inline void update_ruler(RulerState& r, const Vec2& p) {
  if (!r.has_start) return;
  r.end = p;
}

inline void end_ruler(RulerState& r, const Vec2& p) {
  if (!r.has_start) return;
  r.end = p;
  r.dragging = false;
}

inline ImVec2 lerp(const ImVec2& a, const ImVec2& b, float t) {
  return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

inline void draw_ruler_line(ImDrawList* draw,
                           const ImVec2& a,
                           const ImVec2& b,
                           ImU32 col,
                           float thickness = 2.25f,
                           ImU32 shadow = IM_COL32(0, 0, 0, 200)) {
  if (!draw) return;
  draw->AddLine(a, b, shadow, thickness + 2.0f);
  draw->AddLine(a, b, col, thickness);

  // Endpoints.
  draw->AddCircleFilled(a, 4.0f, shadow, 0);
  draw->AddCircleFilled(b, 4.0f, shadow, 0);
  draw->AddCircleFilled(a, 3.0f, col, 0);
  draw->AddCircleFilled(b, 3.0f, col, 0);
}

inline void draw_ruler_label(ImDrawList* draw,
                            const ImVec2& pos,
                            const char* text,
                            ImU32 col_text = IM_COL32(240, 240, 240, 255),
                            ImU32 col_bg = IM_COL32(0, 0, 0, 160)) {
  if (!draw || !text || !*text) return;

  const ImVec2 ts = ImGui::CalcTextSize(text);
  const ImVec2 pad(6.0f, 4.0f);
  const ImVec2 a(pos.x, pos.y);
  const ImVec2 b(pos.x + ts.x + pad.x * 2.0f, pos.y + ts.y + pad.y * 2.0f);
  draw->AddRectFilled(a, b, col_bg, 4.0f);
  draw->AddRect(a, b, IM_COL32(255, 255, 255, 60), 4.0f);
  draw->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), col_text, text);
}

} // namespace nebula4x::ui
