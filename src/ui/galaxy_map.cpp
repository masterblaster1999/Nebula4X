#include "ui/galaxy_map.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace nebula4x::ui {
namespace {

ImVec2 to_screen(const Vec2& world, const ImVec2& center_px, double scale_px_per_unit, double zoom, const Vec2& pan) {
  const double sx = (world.x + pan.x) * scale_px_per_unit * zoom;
  const double sy = (world.y + pan.y) * scale_px_per_unit * zoom;
  return ImVec2(static_cast<float>(center_px.x + sx), static_cast<float>(center_px.y + sy));
}
// Helper: are we allowed to show the destination system name/links?
bool can_show_system(Id viewer_faction_id, bool fog_of_war, const Simulation& sim, Id system_id) {
  if (!fog_of_war) return true;
  if (viewer_faction_id == kInvalidId) return false;
  return sim.is_system_discovered_by_faction(viewer_faction_id, system_id);
}

} // namespace

void draw_galaxy_map(Simulation& sim, UIState& ui, Id& selected_ship, double& zoom, Vec2& pan) {
  auto& s = sim.state();

  const Ship* viewer_ship = (selected_ship != kInvalidId) ? find_ptr(s.ships, selected_ship) : nullptr;
  const Id viewer_faction_id = viewer_ship ? viewer_ship->faction_id : ui.viewer_faction_id;

  if (ui.fog_of_war && viewer_faction_id == kInvalidId) {
    ImGui::TextDisabled("Fog of war requires a viewer faction.");
    ImGui::TextDisabled("Select a faction in the Research tab, or select a ship.");
    return;
  }

  // Visible systems (respect discovery under FoW).
  struct SysView {
    Id id{kInvalidId};
    const StarSystem* sys{nullptr};
  };

  std::vector<SysView> visible;
  visible.reserve(s.systems.size());

  for (const auto& [id, sys] : s.systems) {
    if (ui.fog_of_war) {
      if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, id)) continue;
    }
    visible.push_back(SysView{id, &sys});
  }

  if (visible.empty()) {
    ImGui::TextDisabled("No systems to display");
    return;
  }

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 center_px = ImVec2(origin.x + avail.x * 0.5f, origin.y + avail.y * 0.5f);

  // Compute bounds (in galaxy units).
  double min_x = visible.front().sys->galaxy_pos.x;
  double max_x = visible.front().sys->galaxy_pos.x;
  double min_y = visible.front().sys->galaxy_pos.y;
  double max_y = visible.front().sys->galaxy_pos.y;

  for (const auto& v : visible) {
    min_x = std::min(min_x, v.sys->galaxy_pos.x);
    max_x = std::max(max_x, v.sys->galaxy_pos.x);
    min_y = std::min(min_y, v.sys->galaxy_pos.y);
    max_y = std::max(max_y, v.sys->galaxy_pos.y);
  }

  const Vec2 world_center = Vec2{(min_x + max_x) * 0.5, (min_y + max_y) * 0.5};
  const double span_x = std::max(1e-6, max_x - min_x);
  const double span_y = std::max(1e-6, max_y - min_y);
  const double max_half_span = std::max(span_x, span_y) * 0.5;

  // Fit the farthest system into the available area.
  const double fit = std::min(avail.x, avail.y) * 0.45;
  const double scale = fit / std::max(1.0, max_half_span);

  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

  if (hovered) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      zoom *= std::pow(1.1, wheel);
      zoom = std::clamp(zoom, 0.2, 50.0);
    }

    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      pan.x += d.x / (scale * zoom);
      pan.y += d.y / (scale * zoom);
    }
  }

  auto* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(12, 14, 18, 255));
  draw->AddRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(60, 60, 60, 255));

  // Axes
  draw->AddLine(ImVec2(origin.x, center_px.y), ImVec2(origin.x + avail.x, center_px.y), IM_COL32(35, 35, 35, 255));
  draw->AddLine(ImVec2(center_px.x, origin.y), ImVec2(center_px.x, origin.y + avail.y), IM_COL32(35, 35, 35, 255));

  // Unknown exits count (per visible system).
  std::unordered_map<Id, int> unknown_exits;
  if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
    for (const auto& v : visible) {
      int u = 0;
      for (Id jid : v.sys->jump_points) {
        const auto* jp = find_ptr(s.jump_points, jid);
        if (!jp) continue;
        const auto* dest_jp = find_ptr(s.jump_points, jp->linked_jump_id);
        if (!dest_jp) continue;
        if (!sim.is_system_discovered_by_faction(viewer_faction_id, dest_jp->system_id)) {
          u++;
        }
      }
      unknown_exits[v.id] = u;
    }
  }

  // Jump links (only between visible systems under FoW).
  if (ui.show_galaxy_jump_lines) {
    for (const auto& v : visible) {
      for (Id jid : v.sys->jump_points) {
        const auto* jp = find_ptr(s.jump_points, jid);
        if (!jp) continue;
        const auto* dest_jp = find_ptr(s.jump_points, jp->linked_jump_id);
        if (!dest_jp) continue;

        const auto* dest_sys = find_ptr(s.systems, dest_jp->system_id);
        if (!dest_sys) continue;

        if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
          if (!sim.is_system_discovered_by_faction(viewer_faction_id, dest_sys->id)) continue;
        }

        // Only draw once per pair.
        if (v.id > dest_sys->id) continue;

        const Vec2 a = v.sys->galaxy_pos - world_center;
        const Vec2 b = dest_sys->galaxy_pos - world_center;
        const ImVec2 pa = to_screen(a, center_px, scale, zoom, pan);
        const ImVec2 pb = to_screen(b, center_px, scale, zoom, pan);
        draw->AddLine(pa, pb, IM_COL32(120, 120, 160, 200), 2.0f);
      }
    }
  }

  // Nodes (systems)
  const float base_r = 7.0f;
  Id hovered_system = kInvalidId;
  float hovered_d2 = 1e30f;

  const ImVec2 mouse = ImGui::GetIO().MousePos;

  for (const auto& v : visible) {
    const Vec2 rel = v.sys->galaxy_pos - world_center;
    const ImVec2 p = to_screen(rel, center_px, scale, zoom, pan);

    const bool is_selected = (s.selected_system == v.id);

    const ImU32 fill = is_selected ? IM_COL32(0, 220, 140, 255) : IM_COL32(240, 240, 240, 255);
    const ImU32 outline = IM_COL32(20, 20, 20, 255);

    draw->AddCircleFilled(p, base_r, fill);
    draw->AddCircle(p, base_r, outline, 0, 1.5f);

    // Unknown-exit hint ring.
    if (ui.show_galaxy_unknown_exits) {
      auto it = unknown_exits.find(v.id);
      if (it != unknown_exits.end() && it->second > 0) {
        draw->AddCircle(p, base_r + 4.0f, IM_COL32(255, 180, 0, 200), 0, 2.0f);
      }
    }

    if (ui.show_galaxy_labels) {
      draw->AddText(ImVec2(p.x + base_r + 4.0f, p.y - base_r), IM_COL32(220, 220, 220, 255),
                    v.sys->name.c_str());
    }

    // Hover detection.
    const float dx = mouse.x - p.x;
    const float dy = mouse.y - p.y;
    const float d2 = dx * dx + dy * dy;
    if (d2 < (base_r + 6.0f) * (base_r + 6.0f) && d2 < hovered_d2) {
      hovered_d2 = d2;
      hovered_system = v.id;
    }
  }

  // Click interaction: select system.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (mouse.x >= origin.x && mouse.x <= origin.x + avail.x && mouse.y >= origin.y && mouse.y <= origin.y + avail.y) {
      if (hovered_system != kInvalidId) {
        s.selected_system = hovered_system;

        // If we have a selected ship that isn't in this system, deselect it.
        if (selected_ship != kInvalidId) {
          const auto* sh = find_ptr(s.ships, selected_ship);
          if (!sh || sh->system_id != hovered_system) selected_ship = kInvalidId;
        }
      }
    }
  }

  // Tooltip for hovered system.
  if (hovered_system != kInvalidId && hovered) {
    const auto* sys = find_ptr(s.systems, hovered_system);
    if (sys) {
      int friendly = 0;
      int total_ships = static_cast<int>(sys->ships.size());
      if (viewer_faction_id != kInvalidId) {
        for (Id sid : sys->ships) {
          const auto* sh = find_ptr(s.ships, sid);
          if (sh && sh->faction_id == viewer_faction_id) friendly++;
        }
      }

      int detected_hostiles = 0;
      int contact_count = 0;
      if (viewer_faction_id != kInvalidId) {
        detected_hostiles = (int)sim.detected_hostile_ships_in_system(viewer_faction_id, sys->id).size();
        contact_count = (int)sim.recent_contacts_in_system(viewer_faction_id, sys->id, ui.contact_max_age_days).size();
      }

      const int unknown = unknown_exits.count(sys->id) ? unknown_exits[sys->id] : 0;

      ImGui::BeginTooltip();
      ImGui::Text("%s", sys->name.c_str());
      ImGui::Separator();
      ImGui::Text("Pos: (%.2f, %.2f)", sys->galaxy_pos.x, sys->galaxy_pos.y);
      ImGui::Text("Ships: %d", total_ships);
      if (viewer_faction_id != kInvalidId) {
        ImGui::Text("Friendly ships: %d", friendly);
        if (ui.fog_of_war) {
          ImGui::Text("Detected hostiles: %d", detected_hostiles);
          ImGui::Text("Recent contacts: %d", contact_count);
          ImGui::Text("Unknown jump exits: %d", unknown);
        }
      }
      ImGui::EndTooltip();
    }
  }

  // Legend / help
  ImGui::SetCursorScreenPos(ImVec2(origin.x + 10, origin.y + 10));
  ImGui::BeginChild("galaxy_legend", ImVec2(320, 210), true);
  ImGui::Text("Galaxy map");
  ImGui::BulletText("Wheel: zoom");
  ImGui::BulletText("Middle drag: pan");
  ImGui::BulletText("Left click: select system");
  ImGui::Separator();
  ImGui::Checkbox("Fog of war", &ui.fog_of_war);
  ImGui::Checkbox("Labels", &ui.show_galaxy_labels);
  ImGui::Checkbox("Jump links", &ui.show_galaxy_jump_lines);
  ImGui::Checkbox("Unknown exits hint", &ui.show_galaxy_unknown_exits);

  if (ui.fog_of_war) {
    if (viewer_faction_id == kInvalidId) {
      ImGui::TextDisabled("Select a ship/faction to define view");
    } else {
      ImGui::TextDisabled("Viewer faction: %llu", (unsigned long long)viewer_faction_id);
      ImGui::TextDisabled("Visible systems: %d", (int)visible.size());
    }
  }

  ImGui::EndChild();
}

} // namespace nebula4x::ui
