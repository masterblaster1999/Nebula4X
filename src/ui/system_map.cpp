#include "ui/system_map.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>

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

void draw_system_map(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body,
                     double& zoom, Vec2& pan) {
  const auto& s = sim.state();
  const auto* sys = find_ptr(s.systems, s.selected_system);
  if (!sys) {
    ImGui::TextDisabled("No system selected");
    return;
  }

  const Ship* viewer_ship = (selected_ship != kInvalidId) ? find_ptr(s.ships, selected_ship) : nullptr;
  const Id viewer_faction_id = viewer_ship ? viewer_ship->faction_id : ui.viewer_faction_id;

  if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
    if (!sim.is_system_discovered_by_faction(viewer_faction_id, sys->id)) {
      ImGui::TextDisabled("System not discovered by viewer faction");
      ImGui::TextDisabled("(Select a ship or faction in the Research tab to change view)");
      return;
    }
  }

  std::vector<Id> detected_hostiles;
  if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
    detected_hostiles = sim.detected_hostile_ships_in_system(viewer_faction_id, sys->id);
  }


  // Selected fleet member cache (for highlighting / fleet orders).
  std::unordered_set<Id> selected_fleet_members;
  const Fleet* selected_fleet = nullptr;
  if (ui.selected_fleet_id != kInvalidId) {
    selected_fleet = find_ptr(s.fleets, ui.selected_fleet_id);
    if (selected_fleet) {
      selected_fleet_members.reserve(selected_fleet->ship_ids.size() * 2);
      for (Id sid : selected_fleet->ship_ids) selected_fleet_members.insert(sid);
    }
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
  const ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(ui.system_map_bg[0], ui.system_map_bg[1], ui.system_map_bg[2],
                                                         ui.system_map_bg[3]));
  draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), bg);
  draw->AddRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(60, 60, 60, 255));

  // Axes
  draw->AddLine(ImVec2(origin.x, center.y), ImVec2(origin.x + avail.x, center.y), IM_COL32(40, 40, 40, 255));
  draw->AddLine(ImVec2(center.x, origin.y), ImVec2(center.x, origin.y + avail.y), IM_COL32(40, 40, 40, 255));

  // Cache: colonized bodies (for highlight rings).
  std::unordered_set<Id> colonized_bodies;
  colonized_bodies.reserve(s.colonies.size() * 2);
  for (const auto& [cid, c] : s.colonies) {
    (void)cid;
    if (c.body_id != kInvalidId) colonized_bodies.insert(c.body_id);
  }

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

    // Highlight colonized bodies.
    if (colonized_bodies.count(bid)) {
      draw->AddCircle(p, r + 4.0f, IM_COL32(0, 255, 140, 180), 0, 1.5f);
    }

    // Highlight selected body.
    if (selected_body == bid) {
      draw->AddCircle(p, r + 7.0f, IM_COL32(255, 220, 80, 220), 0, 2.0f);
    }
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

    // Fog-of-war: show friendly ships and detected hostiles (view faction is the selected ship's faction).
    if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
      if (sh->faction_id != viewer_faction_id) {
        if (std::find(detected_hostiles.begin(), detected_hostiles.end(), sid) == detected_hostiles.end()) continue;
      }
    }

    const ImVec2 p = to_screen(sh->position_mkm, center, scale, zoom, pan);

    // Selected ship sensor range overlay
    if (ui.show_selected_sensor_range && selected_ship == sid) {
      const auto* d = sim.find_design(sh->design_id);
      if (d && d->sensor_range_mkm > 0.0) {
        draw->AddCircle(p, static_cast<float>(d->sensor_range_mkm * scale * zoom), IM_COL32(0, 170, 255, 80), 0, 1.0f);
      }
    }

    const float r = (selected_ship == sid) ? 5.0f : 4.0f;
    draw->AddCircleFilled(p, r, color_ship());
    if (selected_ship == sid) {
      draw->AddCircle(p, 10.0f, IM_COL32(0, 255, 140, 255), 0, 1.5f);
    }


    // Highlight selected fleet members.
    if (!selected_fleet_members.empty() && selected_fleet_members.count(sid)) {
      draw->AddCircle(p, 13.0f, IM_COL32(0, 160, 255, 200), 0, 1.5f);
    }
  }

  // Contact markers (last known positions)
  if (ui.fog_of_war && ui.show_contact_markers && viewer_faction_id != kInvalidId) {
    const auto contacts = sim.recent_contacts_in_system(viewer_faction_id, sys->id, ui.contact_max_age_days);
    const int now = static_cast<int>(s.date.days_since_epoch());

    for (const auto& c : contacts) {
      // Don't draw a contact marker if the ship is currently detected (we already draw the ship itself).
      if (c.ship_id != kInvalidId && sim.is_ship_detected_by_faction(viewer_faction_id, c.ship_id)) continue;

      const int age = std::max(0, now - c.last_seen_day);
      const ImVec2 p = to_screen(c.last_seen_position_mkm, center, scale, zoom, pan);
      const ImU32 col = IM_COL32(255, 180, 0, 200);

      draw->AddCircle(p, 6.0f, col, 0, 2.0f);
      draw->AddLine(ImVec2(p.x - 5, p.y - 5), ImVec2(p.x + 5, p.y + 5), col, 2.0f);
      draw->AddLine(ImVec2(p.x - 5, p.y + 5), ImVec2(p.x + 5, p.y - 5), col, 2.0f);

      if (ui.show_contact_labels) {
        std::string lbl = c.last_seen_name.empty() ? std::string("Unknown") : c.last_seen_name;
        lbl += "  (" + std::to_string(age) + "d)";
        draw->AddText(ImVec2(p.x + 8, p.y + 8), IM_COL32(240, 220, 180, 220), lbl.c_str());
      }
    }
  }

  // Interaction:
  // - Left click issues an order for the selected ship.
  //   (Also selects the clicked body for convenience)
  // - Right click selects the closest ship/body (no orders).
  // Ctrl + left click issues an order for the selected fleet (if any).
  // - Click near a body: MoveToBody
  //   - Alt + click near a body: ColonizeBody
  // - Click near a jump point: TravelViaJump
  // - Otherwise: MoveToPoint
  // Holding Shift will *queue* the order; otherwise it replaces the current queue.
  const bool fleet_mode = ImGui::GetIO().KeyCtrl && selected_fleet != nullptr;
  const bool can_issue_orders = fleet_mode || (selected_ship != kInvalidId);
  if (hovered && can_issue_orders && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    // Don't issue orders when clicking UI controls (legend, etc.).
    if (!ImGui::IsAnyItemHovered()) {
      const ImVec2 mp = ImGui::GetIO().MousePos;

      // Require click inside our rect.
      if (mp.x >= origin.x && mp.x <= origin.x + avail.x && mp.y >= origin.y && mp.y <= origin.y + avail.y) {
        const bool queue = ImGui::GetIO().KeyShift;
        if (!queue) {
          if (fleet_mode) {
            sim.clear_fleet_orders(selected_fleet->id);
          } else {
            sim.clear_orders(selected_ship);
          }
        }

        constexpr float kPickRadiusPx = 12.0f;
        const float pick_d2 = kPickRadiusPx * kPickRadiusPx;

        // Find the closest clickable target.
        Id picked_jump = kInvalidId;
        float best_jump_d2 = pick_d2;
        for (Id jid : sys->jump_points) {
          const auto* jp = find_ptr(s.jump_points, jid);
          if (!jp) continue;
          const ImVec2 p = to_screen(jp->position_mkm, center, scale, zoom, pan);
          const float dx = mp.x - p.x;
          const float dy = mp.y - p.y;
          const float d2 = dx * dx + dy * dy;
          if (d2 <= best_jump_d2) {
            best_jump_d2 = d2;
            picked_jump = jid;
          }
        }

        Id picked_body = kInvalidId;
        float best_body_d2 = pick_d2;
        for (Id bid : sys->bodies) {
          const auto* b = find_ptr(s.bodies, bid);
          if (!b) continue;
          const ImVec2 p = to_screen(b->position_mkm, center, scale, zoom, pan);
          const float dx = mp.x - p.x;
          const float dy = mp.y - p.y;
          const float d2 = dx * dx + dy * dy;
          if (d2 <= best_body_d2) {
            best_body_d2 = d2;
            picked_body = bid;
          }
        }

        // Prefer the closest of (jump, body) if both were in range.
        if (picked_jump != kInvalidId && best_jump_d2 <= best_body_d2) {
          if (fleet_mode) {
            sim.issue_fleet_travel_via_jump(selected_fleet->id, picked_jump);
          } else {
            sim.issue_travel_via_jump(selected_ship, picked_jump);
          }
        } else if (picked_body != kInvalidId) {
          // Always select the clicked body (even when ordering).
          selected_body = picked_body;

          // If this body has a colony, select it too.
          for (const auto& [cid, c] : s.colonies) {
            if (c.body_id == picked_body) {
              selected_colony = cid;
              break;
            }
          }

          if (fleet_mode) {
            sim.issue_fleet_move_to_body(selected_fleet->id, picked_body, ui.fog_of_war);
          } else {
            if (ImGui::GetIO().KeyAlt) {
              sim.issue_colonize_body(selected_ship, picked_body, "", ui.fog_of_war);
            } else {
              sim.issue_move_to_body(selected_ship, picked_body, ui.fog_of_war);
            }
          }
        } else {
          const Vec2 world = to_world(mp, center, scale, zoom, pan);
          if (fleet_mode) {
            sim.issue_fleet_move_to_point(selected_fleet->id, world);
          } else {
            sim.issue_move_to_point(selected_ship, world);
          }
        }
      }
    }
  }

  // Right click selection (no orders). Prefer ships, then bodies.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    if (!ImGui::IsAnyItemHovered()) {
      const ImVec2 mp = ImGui::GetIO().MousePos;
      if (mp.x >= origin.x && mp.x <= origin.x + avail.x && mp.y >= origin.y && mp.y <= origin.y + avail.y) {
        constexpr float kPickRadiusPx = 14.0f;
        const float pick_d2 = kPickRadiusPx * kPickRadiusPx;

        Id picked_ship = kInvalidId;
        float best_ship_d2 = pick_d2;
        for (Id sid : sys->ships) {
          const auto* sh = find_ptr(s.ships, sid);
          if (!sh) continue;

          // Respect fog-of-war visibility for picking.
          if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
            if (sh->faction_id != viewer_faction_id) {
              if (std::find(detected_hostiles.begin(), detected_hostiles.end(), sid) == detected_hostiles.end()) {
                continue;
              }
            }
          }

          const ImVec2 p = to_screen(sh->position_mkm, center, scale, zoom, pan);
          const float dx = mp.x - p.x;
          const float dy = mp.y - p.y;
          const float d2 = dx * dx + dy * dy;
          if (d2 <= best_ship_d2) {
            best_ship_d2 = d2;
            picked_ship = sid;
          }
        }

        Id picked_body = kInvalidId;
        float best_body_d2 = pick_d2;
        for (Id bid : sys->bodies) {
          const auto* b = find_ptr(s.bodies, bid);
          if (!b) continue;
          const ImVec2 p = to_screen(b->position_mkm, center, scale, zoom, pan);
          const float dx = mp.x - p.x;
          const float dy = mp.y - p.y;
          const float d2 = dx * dx + dy * dy;
          if (d2 <= best_body_d2) {
            best_body_d2 = d2;
            picked_body = bid;
          }
        }

        if (picked_ship != kInvalidId && best_ship_d2 <= best_body_d2) {
          selected_ship = picked_ship;
          ui.selected_fleet_id = sim.fleet_for_ship(picked_ship);
        } else if (picked_body != kInvalidId) {
          selected_body = picked_body;
          // Select colony on that body if present.
          for (const auto& [cid, c] : s.colonies) {
            if (c.body_id == picked_body) {
              selected_colony = cid;
              break;
            }
          }
        }
      }
    }
  }

  // Legend / help
  ImGui::SetCursorScreenPos(ImVec2(origin.x + 10, origin.y + 10));
  ImGui::BeginChild("legend", ImVec2(280, 190), true);
  ImGui::Text("Controls");
  ImGui::BulletText("Mouse wheel: zoom");
  ImGui::BulletText("Middle drag: pan");
  ImGui::BulletText("Left click: issue order to ship (Shift queues)");
  ImGui::BulletText("Right click: select ship/body (no orders)");
  ImGui::BulletText("Alt+Left click body: colonize (colony ship required)");
  ImGui::BulletText("Ctrl+Left click: issue order to fleet");
  ImGui::BulletText("Click body: move-to-body");
  ImGui::BulletText("Click jump point: travel via jump");
  ImGui::BulletText("Jump points are purple rings");
  ImGui::Separator();
  ImGui::Checkbox("Fog of war", &ui.fog_of_war);
  ImGui::Checkbox("Show sensor range", &ui.show_selected_sensor_range);
  ImGui::Checkbox("Show contacts", &ui.show_contact_markers);
  ImGui::SameLine();
  ImGui::Checkbox("Labels", &ui.show_contact_labels);

  if (ui.fog_of_war) {
    if (viewer_faction_id == kInvalidId) {
      ImGui::TextDisabled("Select a ship to define view faction");
    } else {
      ImGui::TextDisabled("Detected hostiles: %d", (int)detected_hostiles.size());
      ImGui::TextDisabled("Contacts shown (<= %dd): %d", ui.contact_max_age_days,
                          (int)sim.recent_contacts_in_system(viewer_faction_id, sys->id, ui.contact_max_age_days).size());
    }
  }
  ImGui::EndChild();
}

} // namespace nebula4x::ui
