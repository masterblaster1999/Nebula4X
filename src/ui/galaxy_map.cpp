#include "ui/galaxy_map.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/util/log.h"

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


  // Selected fleet (for routing/highlighting).
  const Fleet* selected_fleet = (ui.selected_fleet_id != kInvalidId) ? find_ptr(s.fleets, ui.selected_fleet_id) : nullptr;
  Id selected_fleet_system = kInvalidId;
  if (selected_fleet && selected_fleet->leader_ship_id != kInvalidId) {
    const auto* leader = find_ptr(s.ships, selected_fleet->leader_ship_id);
    if (leader) selected_fleet_system = leader->system_id;
  }

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
  const ImU32 bg = ImGui::ColorConvertFloat4ToU32(
      ImVec4(ui.galaxy_map_bg[0], ui.galaxy_map_bg[1], ui.galaxy_map_bg[2], ui.galaxy_map_bg[3]));
  draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), bg);
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

  // Nodes (systems) + hover selection.
  const float base_r = 7.0f;
  Id hovered_system = kInvalidId;
  float hovered_d2 = 1e30f;

  const ImVec2 mouse = ImGui::GetIO().MousePos;

  struct NodeDrawInfo {
    Id id{kInvalidId};
    const StarSystem* sys{nullptr};
    ImVec2 p{0.0f, 0.0f};
  };

  std::vector<NodeDrawInfo> nodes;
  nodes.reserve(visible.size());
  std::unordered_map<Id, ImVec2> pos_px;
  pos_px.reserve(visible.size() * 2);

  for (const auto& v : visible) {
    const Vec2 rel = v.sys->galaxy_pos - world_center;
    const ImVec2 p = to_screen(rel, center_px, scale, zoom, pan);
    nodes.push_back(NodeDrawInfo{v.id, v.sys, p});
    pos_px[v.id] = p;

    // Hover detection.
    const float dx = mouse.x - p.x;
    const float dy = mouse.y - p.y;
    const float d2 = dx * dx + dy * dy;
    if (d2 < (base_r + 6.0f) * (base_r + 6.0f) && d2 < hovered_d2) {
      hovered_d2 = d2;
      hovered_system = v.id;
    }
  }

  // --- Route preview (hover target) ---
  std::optional<JumpRoutePlan> preview_route;
  bool preview_is_fleet = false;
  bool preview_from_queue = false;
  if (hovered && hovered_system != kInvalidId) {
    const bool restrict = ui.fog_of_war;
    preview_from_queue = ImGui::GetIO().KeyShift;
    const bool fleet_mode = (ImGui::GetIO().KeyCtrl && selected_fleet != nullptr);
    if (fleet_mode) {
      preview_is_fleet = true;
      preview_route = sim.plan_jump_route_for_fleet(selected_fleet->id, hovered_system, restrict, preview_from_queue);
    } else if (selected_ship != kInvalidId) {
      preview_route = sim.plan_jump_route_for_ship(selected_ship, hovered_system, restrict, preview_from_queue);
    }
  }

  if (preview_route && preview_route->systems.size() >= 2) {
    for (std::size_t i = 0; i + 1 < preview_route->systems.size(); ++i) {
      const Id a = preview_route->systems[i];
      const Id b = preview_route->systems[i + 1];
      auto ita = pos_px.find(a);
      auto itb = pos_px.find(b);
      if (ita == pos_px.end() || itb == pos_px.end()) continue;
      draw->AddLine(ita->second, itb->second, IM_COL32(255, 235, 80, 200), 3.0f);
    }
  }

  // Draw nodes.
  for (const auto& n : nodes) {
    const bool is_selected = (s.selected_system == n.id);

    const ImU32 fill = is_selected ? IM_COL32(0, 220, 140, 255) : IM_COL32(240, 240, 240, 255);
    const ImU32 outline = IM_COL32(20, 20, 20, 255);

    draw->AddCircleFilled(n.p, base_r, fill);
    draw->AddCircle(n.p, base_r, outline, 0, 1.5f);

    // Highlight the selected fleet's leader system.
    if (selected_fleet_system != kInvalidId && n.id == selected_fleet_system) {
      draw->AddCircle(n.p, base_r + 6.0f, IM_COL32(0, 160, 255, 200), 0, 2.0f);
    }

    // Unknown-exit hint ring.
    if (ui.show_galaxy_unknown_exits) {
      auto it = unknown_exits.find(n.id);
      if (it != unknown_exits.end() && it->second > 0) {
        draw->AddCircle(n.p, base_r + 4.0f, IM_COL32(255, 180, 0, 200), 0, 2.0f);
      }
    }

    if (ui.show_galaxy_labels && n.sys) {
      draw->AddText(ImVec2(n.p.x + base_r + 4.0f, n.p.y - base_r), IM_COL32(220, 220, 220, 255),
                    n.sys->name.c_str());
    }
  }

  // Click interaction:
  // - Left click selects a system.
  // - Right click routes selected ship to the target system (Shift queues).
  // - Ctrl + right click routes selected fleet to the target system (Shift queues).
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

  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    if (mouse.x >= origin.x && mouse.x <= origin.x + avail.x && mouse.y >= origin.y && mouse.y <= origin.y + avail.y) {
      if (hovered_system != kInvalidId) {
        // Ctrl + right click: route selected fleet.
        const bool fleet_mode = ImGui::GetIO().KeyCtrl && selected_fleet != nullptr;

        if (fleet_mode) {
          const bool queue_orders = ImGui::GetIO().KeyShift;
          if (!queue_orders) sim.clear_fleet_orders(selected_fleet->id);

          // In fog-of-war mode, only allow routing through systems the faction already knows.
          const bool restrict = ui.fog_of_war;
          if (!sim.issue_fleet_travel_to_system(selected_fleet->id, hovered_system, restrict)) {
            nebula4x::log::warn("No known jump route to that system.");
          }
        } else if (selected_ship != kInvalidId) {
          // Route the selected ship to the target system.
          const bool queue_orders = ImGui::GetIO().KeyShift;
          if (!queue_orders) sim.clear_orders(selected_ship);

          // In fog-of-war mode, only allow routing through systems the faction already knows.
          const bool restrict = ui.fog_of_war;
          if (!sim.issue_travel_to_system(selected_ship, hovered_system, restrict)) {
            nebula4x::log::warn("No known jump route to that system.");
          }
        } else {
          // No ship selected: treat right-click as a select.
          s.selected_system = hovered_system;
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

      // Route preview details (when a ship/fleet is selected).
      if (preview_route && !preview_route->systems.empty() && preview_route->systems.back() == sys->id) {
        ImGui::Separator();
        ImGui::Text("%s route preview%s:", preview_is_fleet ? "Fleet" : "Ship",
                    preview_from_queue ? " (queued)" : "");
        ImGui::Text("Jumps: %d", (int)preview_route->jump_ids.size());
        ImGui::Text("Distance: %.1f mkm", preview_route->distance_mkm);
        if (std::isfinite(preview_route->eta_days)) {
          ImGui::Text("ETA: %.1f days", preview_route->eta_days);
        } else {
          ImGui::TextDisabled("ETA: n/a");
        }

        std::string route;
        route.reserve(preview_route->systems.size() * 24);
        for (std::size_t i = 0; i < preview_route->systems.size(); ++i) {
          const Id sid = preview_route->systems[i];
          const auto* rsys = find_ptr(s.systems, sid);
          const char* name = rsys ? rsys->name.c_str() : "?";
          if (i > 0) route += " -> ";
          route += name;
        }
        ImGui::TextWrapped("%s", route.c_str());
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
  ImGui::BulletText("Right click: route selected ship (Shift queues)");
  ImGui::BulletText("Ctrl+Right click: route selected fleet (Shift queues)");
  ImGui::BulletText("Hover: route preview (Shift=queued, Ctrl=fleet)");
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