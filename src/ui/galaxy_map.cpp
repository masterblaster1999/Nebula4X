#include "ui/galaxy_map.h"

#include "ui/map_render.h"

#include <imgui.h>

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

ImVec2 to_screen(const Vec2& world, const ImVec2& center_px, double scale_px_per_unit, double zoom, const Vec2& pan) {
  const double sx = (world.x + pan.x) * scale_px_per_unit * zoom;
  const double sy = (world.y + pan.y) * scale_px_per_unit * zoom;
  return ImVec2(static_cast<float>(center_px.x + sx), static_cast<float>(center_px.y + sy));
}

Vec2 to_world(const ImVec2& screen_px, const ImVec2& center_px, double scale_px_per_unit, double zoom, const Vec2& pan) {
  const double x = (screen_px.x - center_px.x) / (scale_px_per_unit * zoom) - pan.x;
  const double y = (screen_px.y - center_px.y) / (scale_px_per_unit * zoom) - pan.y;
  return Vec2{x, y};
}

void add_arrowhead(ImDrawList* draw, const ImVec2& from, const ImVec2& to, ImU32 col, float size_px) {
  const ImVec2 d{to.x - from.x, to.y - from.y};
  const float len2 = d.x * d.x + d.y * d.y;
  if (len2 < 1.0f) return;
  const float len = std::sqrt(len2);
  const ImVec2 dir{d.x / len, d.y / len};
  const ImVec2 perp{-dir.y, dir.x};
  const float s = std::max(3.0f, size_px);
  const ImVec2 p1 = to;
  const ImVec2 p2{to.x - dir.x * s + perp.x * (s * 0.5f), to.y - dir.y * s + perp.y * (s * 0.5f)};
  const ImVec2 p3{to.x - dir.x * s - perp.x * (s * 0.5f), to.y - dir.y * s - perp.y * (s * 0.5f)};
  draw->AddTriangleFilled(p1, p2, p3, col);
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

  // Precompute recent contact counts per-system for lightweight "intel alert" rings.
  std::unordered_map<Id, int> recent_contact_count;
  if (ui.show_galaxy_intel_alerts && viewer_faction_id != kInvalidId) {
    if (const auto* viewer = find_ptr(s.factions, viewer_faction_id)) {
      const int today = s.date.days_since_epoch();
      for (const auto& kv : viewer->ship_contacts) {
        const auto& c = kv.second;
        const int age = today - c.last_seen_day;
        if (age < 0) continue;
        if (age > ui.contact_max_age_days) continue;
        ++recent_contact_count[c.system_id];
      }
    }
  }


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
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool mouse_in_rect =
      (mouse.x >= origin.x && mouse.x <= origin.x + avail.x && mouse.y >= origin.y && mouse.y <= origin.y + avail.y);

  // Keyboard shortcuts.
  if (hovered && !ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
      zoom = 1.0;
      pan = Vec2{0.0, 0.0};
    }
  }

  if (hovered && mouse_in_rect) {
    // Zoom to cursor.
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      const Vec2 before = to_world(mouse, center_px, scale, zoom, pan);
      double new_zoom = zoom * std::pow(1.1, wheel);
      new_zoom = std::clamp(new_zoom, 0.2, 50.0);
      const Vec2 after = to_world(mouse, center_px, scale, new_zoom, pan);
      pan.x += (after.x - before.x);
      pan.y += (after.y - before.y);
      zoom = new_zoom;
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

  // Map chrome.
  {
    StarfieldStyle sf;
    sf.enabled = ui.galaxy_map_starfield;
    sf.density = ui.map_starfield_density;
    sf.parallax = ui.map_starfield_parallax;
    sf.alpha = 1.0f;
    const float pan_px_x = static_cast<float>(-pan.x * scale * zoom);
    const float pan_px_y = static_cast<float>(-pan.y * scale * zoom);
    draw_starfield(draw, origin, avail, bg, pan_px_x, pan_px_y,
                   static_cast<std::uint32_t>(viewer_faction_id == kInvalidId ? 0xC0FFEEu : viewer_faction_id), sf);

    GridStyle gs;
    gs.enabled = ui.galaxy_map_grid;
    gs.desired_minor_px = 95.0f;
    gs.major_every = 5;
    gs.minor_alpha = 0.10f * ui.map_grid_opacity;
    gs.major_alpha = 0.18f * ui.map_grid_opacity;
    gs.axis_alpha = 0.25f * ui.map_grid_opacity;
    gs.label_alpha = 0.70f * ui.map_grid_opacity;
    draw_grid(draw, origin, avail, center_px, scale, zoom, pan, IM_COL32(220, 220, 220, 255), gs, "u");

    ScaleBarStyle sb;
    sb.enabled = true;
    sb.desired_px = 120.0f;
    sb.alpha = 0.85f;
    draw_scale_bar(draw, origin, avail, 1.0 / (scale * zoom), IM_COL32(220, 220, 220, 255), sb, "u");
  }

  // Axes (when grid is disabled).
  if (!ui.galaxy_map_grid) {
    draw->AddLine(ImVec2(origin.x, center_px.y), ImVec2(origin.x + avail.x, center_px.y), IM_COL32(35, 35, 35, 255));
    draw->AddLine(ImVec2(center_px.x, origin.y), ImVec2(center_px.x, origin.y + avail.y), IM_COL32(35, 35, 35, 255));
  }

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

  // Selected ship/fleet travel route overlay (linked elements).
  if (ui.galaxy_map_selected_route) {
    Id route_ship_id = selected_ship;
    if (route_ship_id == kInvalidId && selected_fleet && selected_fleet->leader_ship_id != kInvalidId) {
      route_ship_id = selected_fleet->leader_ship_id;
    }

    const auto* sh = find_ptr(s.ships, route_ship_id);
    const auto* so = sh ? find_ptr(s.ship_orders, route_ship_id) : nullptr;
    if (sh && so) {
      const bool templ = so->queue.empty() && so->repeat && !so->repeat_template.empty();
      const auto& q = (templ ? so->repeat_template : so->queue);

      std::vector<Id> route_systems;
      route_systems.reserve(q.size() + 1);
      route_systems.push_back(sh->system_id);

      for (const auto& ord : q) {
        if (!std::holds_alternative<nebula4x::TravelViaJump>(ord)) continue;
        const auto& o = std::get<nebula4x::TravelViaJump>(ord);
        const auto* jp = find_ptr(s.jump_points, o.jump_point_id);
        if (!jp) continue;
        const auto* other = find_ptr(s.jump_points, jp->linked_jump_id);
        if (!other) continue;
        const Id dest_sys = other->system_id;

        // Don't leak undiscovered destinations under FoW.
        if (!can_show_system(viewer_faction_id, ui.fog_of_war, sim, dest_sys)) break;

        route_systems.push_back(dest_sys);
      }

      if (route_systems.size() >= 2) {
        const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
        const ImU32 base = templ ? IM_COL32(160, 160, 160, 255) : IM_COL32(255, 220, 80, 255);
        const ImU32 col = modulate_alpha(base, templ ? (0.55f * alpha) : alpha);
        const ImU32 shadow = modulate_alpha(IM_COL32(0, 0, 0, 200), templ ? (0.45f * alpha) : (0.8f * alpha));

        for (std::size_t i = 0; i + 1 < route_systems.size(); ++i) {
          const auto* a_sys = find_ptr(s.systems, route_systems[i]);
          const auto* b_sys = find_ptr(s.systems, route_systems[i + 1]);
          if (!a_sys || !b_sys) continue;

          // Respect visibility list to avoid drawing lines to hidden systems.
          if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
            if (!sim.is_system_discovered_by_faction(viewer_faction_id, a_sys->id)) continue;
            if (!sim.is_system_discovered_by_faction(viewer_faction_id, b_sys->id)) continue;
          }

          const Vec2 a = a_sys->galaxy_pos - world_center;
          const Vec2 b = b_sys->galaxy_pos - world_center;
          const ImVec2 pa = to_screen(a, center_px, scale, zoom, pan);
          const ImVec2 pb = to_screen(b, center_px, scale, zoom, pan);

          draw->AddLine(pa, pb, shadow, 4.0f);
          draw->AddLine(pa, pb, col, 2.25f);
          add_arrowhead(draw, pa, pb, col, 10.0f);
          draw->AddCircleFilled(pb, 4.0f, shadow, 0);
          draw->AddCircleFilled(pb, 3.0f, col, 0);

          char buf[16];
          std::snprintf(buf, sizeof(buf), "%zu", i + 1);
          const ImVec2 mid{(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f};
          draw->AddText(ImVec2(mid.x + 6.0f, mid.y + 4.0f), col, buf);
        }
      }
    }
  }

  // Nodes (systems) + hover selection.
  const float base_r = 7.0f;
  Id hovered_system = kInvalidId;
  float hovered_d2 = 1e30f;

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
  // Planning routes can be expensive, especially when called every frame while hovering.
  // Cache the preview route until the relevant inputs change.
  struct RoutePreviewCacheKey {
    Id hovered_system{kInvalidId};
    Id selected_ship{kInvalidId};
    Id selected_fleet{kInvalidId};
    bool fleet_mode{false};
    bool restrict_to_discovered{false};
    bool from_queue{false};
    std::int64_t sim_day{0};

    bool operator==(const RoutePreviewCacheKey& o) const {
      return hovered_system == o.hovered_system && selected_ship == o.selected_ship &&
             selected_fleet == o.selected_fleet && fleet_mode == o.fleet_mode &&
             restrict_to_discovered == o.restrict_to_discovered && from_queue == o.from_queue &&
             sim_day == o.sim_day;
    }
  };

  struct RoutePreviewCache {
    bool valid{false};
    RoutePreviewCacheKey key{};
    bool is_fleet{false};
    std::optional<JumpRoutePlan> route{};
  };

  static RoutePreviewCache route_cache;

  std::optional<JumpRoutePlan> preview_route;
  bool preview_is_fleet = false;
  bool preview_from_queue = false;
  if (hovered && hovered_system != kInvalidId) {
    const bool restrict = ui.fog_of_war;
    const bool from_queue = ImGui::GetIO().KeyShift;
    const bool fleet_mode = (ImGui::GetIO().KeyCtrl && selected_fleet != nullptr);
    const std::int64_t sim_day = s.date.days_since_epoch();

    RoutePreviewCacheKey key;
    key.hovered_system = hovered_system;
    key.selected_ship = fleet_mode ? kInvalidId : selected_ship;
    key.selected_fleet = fleet_mode ? selected_fleet->id : kInvalidId;
    key.fleet_mode = fleet_mode;
    key.restrict_to_discovered = restrict;
    key.from_queue = from_queue;
    key.sim_day = sim_day;

    if (!route_cache.valid || !(route_cache.key == key)) {
      route_cache.valid = true;
      route_cache.key = key;
      route_cache.is_fleet = fleet_mode;
      route_cache.route.reset();

      if (fleet_mode) {
        route_cache.route =
            sim.plan_jump_route_for_fleet(selected_fleet->id, hovered_system, restrict, from_queue);
      } else if (selected_ship != kInvalidId) {
        route_cache.route = sim.plan_jump_route_for_ship(selected_ship, hovered_system, restrict, from_queue);
      }
    }

    preview_route = route_cache.route;
    preview_is_fleet = route_cache.is_fleet;
    preview_from_queue = from_queue;
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

    const bool is_hovered = (hovered_system == n.id);

    const ImU32 fill = is_selected ? IM_COL32(0, 220, 140, 255) : IM_COL32(240, 240, 240, 255);
    const ImU32 outline = IM_COL32(20, 20, 20, 255);

    // Drop shadow + subtle glow for higher visual contrast.
    draw->AddCircleFilled(ImVec2(n.p.x + 1.5f, n.p.y + 1.5f), base_r + 0.5f, IM_COL32(0, 0, 0, 110), 0);
    const ImU32 glow_col = is_selected ? IM_COL32(0, 220, 140, 255) : IM_COL32(220, 220, 255, 255);
    draw->AddCircleFilled(n.p, base_r * 2.6f, modulate_alpha(glow_col, is_selected ? 0.12f : 0.08f), 0);
    draw->AddCircleFilled(n.p, base_r * 1.7f, modulate_alpha(glow_col, is_selected ? 0.22f : 0.14f), 0);

    draw->AddCircleFilled(n.p, base_r, fill);
    draw->AddCircle(n.p, base_r, outline, 0, 1.5f);

    if (is_hovered) {
      draw->AddCircle(n.p, base_r + 8.0f, IM_COL32(255, 255, 255, 140), 0, 2.0f);
    }

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

    // Intel-alert ring (recent hostile contacts in the system).
    if (ui.show_galaxy_intel_alerts) {
      auto it = recent_contact_count.find(n.id);
      if (it != recent_contact_count.end() && it->second > 0) {
        const float t = (float)ImGui::GetTime();
        const float pulse = 0.5f + 0.5f * std::sin(t * 2.25f + (float)((n.id & 0x3FF) * 0.01f));
        const float r = base_r + 7.0f + pulse * 2.5f;
        float a = 0.28f + 0.55f * pulse;
        // Scale visibility slightly with the number of contacts.
        a = std::min(1.0f, a + 0.07f * std::logf((float)it->second + 1.0f));
        const ImU32 col0 = modulate_alpha(IM_COL32(255, 90, 90, 255), a);
        const ImU32 col1 = modulate_alpha(IM_COL32(255, 180, 120, 255), a * 0.45f);
        draw->AddCircle(n.p, r, col0, 0, 2.0f);
        draw->AddCircle(n.p, r + 3.0f, col1, 0, 1.0f);
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
      if (ImGui::SmallButton("Select")) {
        s.selected_system = hovered_system;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("View system map")) {
        s.selected_system = hovered_system;
        ui.request_map_tab = MapTab::System;

        // If the current selected ship isn't in that system, deselect it.
        if (selected_ship != kInvalidId) {
          const auto* sh = find_ptr(s.ships, selected_ship);
          if (!sh || sh->system_id != hovered_system) selected_ship = kInvalidId;
        }
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Intel")) {
        s.selected_system = hovered_system;
        ui.show_intel_window = true;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Center")) {
        const Vec2 rel = sys->galaxy_pos - world_center;
        pan = Vec2{-rel.x, -rel.y};
      }
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
  ImGui::BeginChild("galaxy_legend", ImVec2(350, 320), true);
  ImGui::Text("Galaxy map");
  ImGui::BulletText("Wheel: zoom (to cursor)");
  ImGui::BulletText("Middle drag: pan");
  ImGui::BulletText("R: reset view");
  ImGui::BulletText("Left click: select system");
  ImGui::BulletText("Right click: route selected ship (Shift queues)");
  ImGui::BulletText("Ctrl+Right click: route selected fleet (Shift queues)");
  ImGui::BulletText("Hover: route preview (Shift=queued, Ctrl=fleet)");
  ImGui::SeparatorText("Overlays");
  ImGui::Checkbox("Starfield", &ui.galaxy_map_starfield);
  ImGui::SameLine();
  ImGui::Checkbox("Grid", &ui.galaxy_map_grid);
  ImGui::Checkbox("Selected travel route", &ui.galaxy_map_selected_route);
  ImGui::Checkbox("Fog of war", &ui.fog_of_war);
  ImGui::Checkbox("Labels", &ui.show_galaxy_labels);
  ImGui::Checkbox("Jump links", &ui.show_galaxy_jump_lines);
  ImGui::Checkbox("Unknown exits hint", &ui.show_galaxy_unknown_exits);
  ImGui::Checkbox("Intel alerts", &ui.show_galaxy_intel_alerts);

  if (ImGui::Button("Reset view (R)")) {
    zoom = 1.0;
    pan = Vec2{0.0, 0.0};
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Zoom: %.2fx", zoom);
  {
    const Vec2 rel = to_world(mouse, center_px, scale, zoom, pan);
    const Vec2 abs = rel + world_center;
    ImGui::TextDisabled("Cursor: %.2f, %.2f u", abs.x, abs.y);
  }

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