#include "ui/system_map.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace nebula4x::ui {
namespace {

ImU32 color_body(BodyType t) {
  switch (t) {
    case BodyType::Star: return IM_COL32(255, 230, 120, 255);
    case BodyType::GasGiant: return IM_COL32(180, 160, 255, 255);
    case BodyType::Asteroid: return IM_COL32(170, 170, 170, 255);
    case BodyType::Moon: return IM_COL32(210, 210, 210, 255);
    default: return IM_COL32(120, 200, 255, 255);
  }
}

ImU32 color_ship() { return IM_COL32(255, 255, 255, 255); }
ImU32 color_jump() { return IM_COL32(200, 120, 255, 255); }

ImVec2 to_screen(const Vec2& world_mkm, const ImVec2& center_px, double scale_px_per_mkm, double zoom,
                 const Vec2& pan_mkm) {
  const double sx = (world_mkm.x + pan_mkm.x) * scale_px_per_mkm * zoom;
  const double sy = (world_mkm.y + pan_mkm.y) * scale_px_per_mkm * zoom;
  return ImVec2(static_cast<float>(center_px.x + sx), static_cast<float>(center_px.y + sy));
}

Vec2 to_world(const ImVec2& screen_px, const ImVec2& center_px, double scale_px_per_mkm, double zoom,
              const Vec2& pan_mkm) {
  const double x = (screen_px.x - center_px.x) / (scale_px_per_mkm * zoom) - pan_mkm.x;
  const double y = (screen_px.y - center_px.y) / (scale_px_per_mkm * zoom) - pan_mkm.y;
  return Vec2{x, y};
}

} // namespace

void draw_system_map(Simulation& sim, Id& selected_ship, double& zoom, Vec2& pan) {
  const auto& s = sim.state();
  const auto* sys = find_ptr(s.systems, s.selected_system);
  if (!sys) {
    ImGui::TextDisabled("No system selected");
    return;
  }

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 center = ImVec2(origin.x + avail.x * 0.5f, origin.y + avail.y * 0.5f);

  // Determine scaling from max orbit radius.
  double max_r = 1.0;
  for (Id bid : sys->bodies) {
    const auto* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    max_r = std::max(max_r, b->orbit_radius_mkm);
  }
  // Make sure jump points beyond the outermost orbit are still visible.
  for (Id jid : sys->jump_points) {
    const auto* jp = find_ptr(s.jump_points, jid);
    if (!jp) continue;
    max_r = std::max(max_r, jp->position_mkm.length());
  }

  const double fit = std::min(avail.x, avail.y) * 0.45;
  const double scale = fit / max_r;

  // Input handling (hovered?)
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

  // Zoom via wheel.
  if (hovered) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      zoom *= std::pow(1.1, wheel);
      zoom = std::clamp(zoom, 0.2, 20.0);
    }

    // Pan with middle mouse drag.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      pan.x += d.x / (scale * zoom);
      pan.y += d.y / (scale * zoom);
    }
  }

  auto* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(15, 18, 22, 255));
  draw->AddRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(60, 60, 60, 255));

  // Axes
  draw->AddLine(ImVec2(origin.x, center.y), ImVec2(origin.x + avail.x, center.y), IM_COL32(40, 40, 40, 255));
  draw->AddLine(ImVec2(center.x, origin.y), ImVec2(center.x, origin.y + avail.y), IM_COL32(40, 40, 40, 255));

  // Orbits + bodies
  for (Id bid : sys->bodies) {
    const auto* b = find_ptr(s.bodies, bid);
    if (!b) continue;

    if (b->orbit_radius_mkm > 1e-6) {
      draw->AddCircle(center, static_cast<float>(b->orbit_radius_mkm * scale * zoom), IM_COL32(35, 35, 35, 255), 0,
                      1.0f);
    }

    const ImVec2 p = to_screen(b->position_mkm, center, scale, zoom, pan);
    const float r = (b->type == BodyType::Star) ? 8.0f : 5.0f;
    draw->AddCircleFilled(p, r, color_body(b->type));
    draw->AddText(ImVec2(p.x + 6, p.y + 6), IM_COL32(200, 200, 200, 255), b->name.c_str());
  }

  // Jump points
  for (Id jid : sys->jump_points) {
    const auto* jp = find_ptr(s.jump_points, jid);
    if (!jp) continue;

    const ImVec2 p = to_screen(jp->position_mkm, center, scale, zoom, pan);
    const float r = 6.0f;
    draw->AddCircle(p, r, color_jump(), 0, 2.0f);
    draw->AddText(ImVec2(p.x + 6, p.y - 6), IM_COL32(200, 200, 200, 255), jp->name.c_str());
  }

  // Ships
  for (Id sid : sys->ships) {
    const auto* sh = find_ptr(s.ships, sid);
    if (!sh) continue;

    const ImVec2 p = to_screen(sh->position_mkm, center, scale, zoom, pan);
    const float r = (selected_ship == sid) ? 5.0f : 4.0f;
    draw->AddCircleFilled(p, r, color_ship());
    if (selected_ship == sid) {
      draw->AddCircle(p, 10.0f, IM_COL32(0, 255, 140, 255), 0, 1.5f);
    }
  }

  // Interaction: left click sets a move-to-point order for the selected ship.
  if (hovered && selected_ship != kInvalidId && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    const ImVec2 mp = ImGui::GetIO().MousePos;
    // Avoid clicking in the menu bar area by requiring click inside our rect.
    if (mp.x >= origin.x && mp.x <= origin.x + avail.x && mp.y >= origin.y && mp.y <= origin.y + avail.y) {
      const Vec2 world = to_world(mp, center, scale, zoom, pan);
      sim.issue_move_to_point(selected_ship, world);
    }
  }

  // Legend / help
  ImGui::SetCursorScreenPos(ImVec2(origin.x + 10, origin.y + 10));
  ImGui::BeginChild("legend", ImVec2(260, 115), true);
  ImGui::Text("Controls");
  ImGui::BulletText("Mouse wheel: zoom");
  ImGui::BulletText("Middle drag: pan");
  ImGui::BulletText("Left click: move order");
  ImGui::BulletText("Jump points are purple rings");
  ImGui::EndChild();
}

} // namespace nebula4x::ui
