#include "ui/system_map.h"

#include "ui/map_render.h"

#include "nebula4x/core/fleet_formation.h"
#include "nebula4x/core/enum_strings.h"
#include "nebula4x/core/power.h"
#include "nebula4x/util/time.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <variant>

namespace nebula4x::ui {
namespace {

constexpr double kTwoPi = 6.283185307179586;

ImU32 color_body(BodyType t) {
  switch (t) {
    case BodyType::Star: return IM_COL32(255, 230, 120, 255);
    case BodyType::GasGiant: return IM_COL32(180, 160, 255, 255);
    case BodyType::Comet: return IM_COL32(120, 255, 210, 255);
    case BodyType::Asteroid: return IM_COL32(170, 170, 170, 255);
    case BodyType::Moon: return IM_COL32(210, 210, 210, 255);
    default: return IM_COL32(120, 200, 255, 255);
  }
}

ImU32 color_ship() { return IM_COL32(255, 255, 255, 255); }
ImU32 color_jump() { return IM_COL32(200, 120, 255, 255); }

std::uint32_t hash_u32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

ImU32 color_faction(Id faction_id) {
  if (faction_id == kInvalidId) return IM_COL32(220, 220, 220, 255);
  const std::uint32_t h = hash_u32(static_cast<std::uint32_t>(faction_id));
  const float hue = static_cast<float>(h % 360u) / 360.0f;
  float r = 1.0f, g = 1.0f, b = 1.0f;
  ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.95f, r, g, b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

void add_arrowhead(ImDrawList* draw, const ImVec2& from, const ImVec2& to, ImU32 col, float size_px) {
  if (!draw) return;
  const float dx = to.x - from.x;
  const float dy = to.y - from.y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-3f) return;
  const float ux = dx / len;
  const float uy = dy / len;
  const float px = -uy;
  const float py = ux;
  const float back = size_px;
  const float half = size_px * 0.55f;
  const ImVec2 p1 = to;
  const ImVec2 p2(to.x - ux * back + px * half, to.y - uy * back + py * half);
  const ImVec2 p3(to.x - ux * back - px * half, to.y - uy * back - py * half);
  draw->AddTriangleFilled(p1, p2, p3, col);
}

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

  // Cache recent contacts for this system (used for markers + picking).
  std::vector<Contact> recent_contacts;
  if (ui.fog_of_war && ui.show_contact_markers && viewer_faction_id != kInvalidId) {
    recent_contacts = sim.recent_contacts_in_system(viewer_faction_id, sys->id, ui.contact_max_age_days);
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

    // Ensure the view fits both the body's orbit circle and its current absolute position.
    // For moons (or other child bodies), the orbit is centered on the parent body.
    Vec2 orbit_center_mkm{0.0, 0.0};
    if (b->parent_body_id != kInvalidId) {
      if (const auto* parent = find_ptr(s.bodies, b->parent_body_id)) {
        orbit_center_mkm = parent->position_mkm;
      }
    }

    const bool is_minor = (b->type == BodyType::Asteroid || b->type == BodyType::Comet);
    if (!ui.show_minor_bodies && is_minor && selected_body != bid) continue;

    const double e = std::clamp(std::abs(b->orbit_eccentricity), 0.0, 0.999999);
    const double orbit_extent = b->orbit_radius_mkm * (1.0 + e);
    const double extent = orbit_center_mkm.length() + orbit_extent;
    max_r = std::max(max_r, extent);
  }
  // Make sure jump points beyond the outermost orbit are still visible.
  for (Id jid : sys->jump_points) {
    const auto* jp = find_ptr(s.jump_points, jid);
    if (!jp) continue;
    max_r = std::max(max_r, jp->position_mkm.length());
  }

  const double fit = std::min(avail.x, avail.y) * 0.45;
  const double scale = fit / max_r;

  // Input handling.
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool mouse_in_rect =
      (mouse.x >= origin.x && mouse.x <= origin.x + avail.x && mouse.y >= origin.y && mouse.y <= origin.y + avail.y);

  // Keyboard shortcuts (only when the map window is hovered and the user isn't typing).
  if (hovered && !ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
      zoom = 1.0;
      pan = Vec2{0.0, 0.0};
      ui.system_map_follow_selected = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
      ui.system_map_follow_selected = !ui.system_map_follow_selected;
    }
  }

  // Zoom via wheel (zoom to cursor).
  if (hovered && mouse_in_rect) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      const Vec2 before = to_world(mouse, center, scale, zoom, pan);
      double new_zoom = zoom * std::pow(1.1, wheel);
      new_zoom = std::clamp(new_zoom, 0.2, 20.0);
      const Vec2 after = to_world(mouse, center, scale, new_zoom, pan);
      pan.x += (after.x - before.x);
      pan.y += (after.y - before.y);
      zoom = new_zoom;
    }

    // Pan with middle mouse drag ("grab" style). Manual panning disables follow mode.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      if (std::abs(d.x) > 0.0f || std::abs(d.y) > 0.0f) {
        ui.system_map_follow_selected = false;
      }
      pan.x += d.x / (scale * zoom);
      pan.y += d.y / (scale * zoom);
    }
  }

  // External request: one-shot center (used by Intel window, etc.).
  if (ui.request_system_map_center &&
      (ui.request_system_map_center_system_id == kInvalidId || ui.request_system_map_center_system_id == sys->id)) {
    pan = Vec2{-ui.request_system_map_center_x_mkm, -ui.request_system_map_center_y_mkm};
    if (ui.request_system_map_center_zoom > 0.0) {
      zoom = ui.request_system_map_center_zoom;
    }
    // Manual reposition implies we should stop following.
    ui.system_map_follow_selected = false;
    ui.request_system_map_center = false;
    ui.request_system_map_center_system_id = kInvalidId;
    ui.request_system_map_center_zoom = 0.0;
  }

  // Optional: follow the selected ship (or fleet leader) by keeping it centered.
  if (ui.system_map_follow_selected) {
    Id follow_ship_id = selected_ship;
    if (follow_ship_id == kInvalidId && selected_fleet && selected_fleet->leader_ship_id != kInvalidId) {
      follow_ship_id = selected_fleet->leader_ship_id;
    }

    if (const auto* sh = find_ptr(s.ships, follow_ship_id); sh && sh->system_id == sys->id) {
      const Vec2 target{-sh->position_mkm.x, -sh->position_mkm.y};
      const double t = 0.18; // smoothing
      pan.x = pan.x + (target.x - pan.x) * t;
      pan.y = pan.y + (target.y - pan.y) * t;
    }
  }

  auto* draw = ImGui::GetWindowDrawList();
  const ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(ui.system_map_bg[0], ui.system_map_bg[1], ui.system_map_bg[2],
                                                         ui.system_map_bg[3]));
  draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), bg);
  draw->AddRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(60, 60, 60, 255));

  // Map chrome.
  {
    StarfieldStyle sf;
    sf.enabled = ui.system_map_starfield;
    sf.density = ui.map_starfield_density;
    sf.parallax = ui.map_starfield_parallax;
    sf.alpha = 1.0f;
    const float pan_px_x = static_cast<float>(-pan.x * scale * zoom);
    const float pan_px_y = static_cast<float>(-pan.y * scale * zoom);
    draw_starfield(draw, origin, avail, bg, pan_px_x, pan_px_y,
                   hash_u32(static_cast<std::uint32_t>(sys->id) ^ 0xA3C59AC3u), sf);

    GridStyle gs;
    gs.enabled = ui.system_map_grid;
    gs.desired_minor_px = 90.0f;
    gs.major_every = 5;
    gs.minor_alpha = 0.10f * ui.map_grid_opacity;
    gs.major_alpha = 0.18f * ui.map_grid_opacity;
    gs.axis_alpha = 0.25f * ui.map_grid_opacity;
    gs.label_alpha = 0.70f * ui.map_grid_opacity;
    draw_grid(draw, origin, avail, center, scale, zoom, pan, IM_COL32(220, 220, 220, 255), gs, "mkm");

    ScaleBarStyle sb;
    sb.enabled = true;
    sb.desired_px = 120.0f;
    sb.alpha = 0.85f;
    draw_scale_bar(draw, origin, avail, 1.0 / (scale * zoom), IM_COL32(220, 220, 220, 255), sb, "mkm");
  }

  // Axes (when grid is disabled).
  if (!ui.system_map_grid) {
    draw->AddLine(ImVec2(origin.x, center.y), ImVec2(origin.x + avail.x, center.y), IM_COL32(40, 40, 40, 255));
    draw->AddLine(ImVec2(center.x, origin.y), ImVec2(center.x, origin.y + avail.y), IM_COL32(40, 40, 40, 255));
  }

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

    const bool is_minor = (b->type == BodyType::Asteroid || b->type == BodyType::Comet);
    if (!ui.show_minor_bodies && is_minor && selected_body != bid) continue;

    // Orbit path (centered on system origin for planets, or on the parent body for moons/binaries).
    if (b->orbit_radius_mkm > 1e-6) {
      Vec2 orbit_center_mkm{0.0, 0.0};
      if (b->parent_body_id != kInvalidId) {
        if (const auto* parent = find_ptr(s.bodies, b->parent_body_id)) {
          orbit_center_mkm = parent->position_mkm;
        }
      }

      const double a = b->orbit_radius_mkm;
      const double e = std::clamp(std::abs(b->orbit_eccentricity), 0.0, 0.999999);

      if (e < 1e-4) {
        const ImVec2 orbit_center_px = to_screen(orbit_center_mkm, center, scale, zoom, pan);
        draw->AddCircle(orbit_center_px, static_cast<float>(a * scale * zoom), IM_COL32(35, 35, 35, 255), 0, 1.0f);
      } else {
        // Ellipse sampled in eccentric anomaly (focus at orbit_center_mkm).
        const double bsemi = a * std::sqrt(std::max(0.0, 1.0 - e * e));
        const double w = b->orbit_arg_periapsis_radians;
        const double cw = std::cos(w);
        const double sw = std::sin(w);

        const int kSegments = std::clamp(static_cast<int>(96.0 * std::sqrt(std::max(1.0, zoom))), 64, 320);
        ImVec2 first{};
        ImVec2 prev{};
        for (int i = 0; i <= kSegments; ++i) {
          const double E = (kTwoPi * static_cast<double>(i)) / static_cast<double>(kSegments);
          const double cE = std::cos(E);
          const double sE = std::sin(E);

          const double x = a * (cE - e);
          const double y = bsemi * sE;
          const double rx = x * cw - y * sw;
          const double ry = x * sw + y * cw;

          const Vec2 world = orbit_center_mkm + Vec2{rx, ry};
          const ImVec2 pt = to_screen(world, center, scale, zoom, pan);

          if (i == 0) {
            first = pt;
            prev = pt;
          } else {
            draw->AddLine(prev, pt, IM_COL32(35, 35, 35, 255), 1.0f);
            prev = pt;
          }
        }
        draw->AddLine(prev, first, IM_COL32(35, 35, 35, 255), 1.0f);
      }
    }

    const ImVec2 p = to_screen(b->position_mkm, center, scale, zoom, pan);

    float r = 5.0f;
    switch (b->type) {
      case BodyType::Star: r = 8.0f; break;
      case BodyType::GasGiant: r = 6.0f; break;
      case BodyType::Moon: r = 4.0f; break;
      case BodyType::Asteroid: r = 2.5f; break;
      case BodyType::Comet: r = 3.0f; break;
      default: r = 5.0f; break;
    }

    // Simple glow / style hints (purely visual).
    if (b->type == BodyType::Star) {
      // Soft glow to make the star feel less "flat".
      draw->AddCircleFilled(p, r * 4.0f, IM_COL32(255, 230, 120, 18), 0);
      draw->AddCircleFilled(p, r * 2.6f, IM_COL32(255, 230, 120, 36), 0);
      draw->AddCircleFilled(p, r * 1.6f, IM_COL32(255, 230, 120, 70), 0);
    }

    // Comet tail (visual hint): points away from the system origin.
    if (b->type == BodyType::Comet) {
      const Vec2 dir = b->position_mkm.normalized();
      const ImVec2 tail = ImVec2(p.x + static_cast<float>(dir.x * 16.0), p.y + static_cast<float>(dir.y * 16.0));
      draw->AddLine(p, tail, IM_COL32(120, 255, 210, 170), 2.0f);
    }

    // Body marker.
    draw->AddCircleFilled(p, r, color_body(b->type), 0);

    // Additional styling.
    if (b->type == BodyType::GasGiant) {
      draw->AddCircle(p, r + 2.0f, IM_COL32(200, 190, 255, 120), 0, 1.5f);
    } else if (b->type == BodyType::Star) {
      draw->AddCircle(p, r + 1.0f, IM_COL32(255, 240, 180, 160), 0, 1.25f);
    }

    // Highlight colonized bodies.
    if (colonized_bodies.count(bid)) {
      draw->AddCircle(p, r + 4.0f, IM_COL32(0, 255, 140, 180), 0, 1.5f);
    }

    // Highlight selected body.
    if (selected_body == bid) {
      draw->AddCircle(p, r + 7.0f, IM_COL32(255, 220, 80, 220), 0, 2.0f);
    }

    const bool show_label =
        (!is_minor) || (selected_body == bid) || (ui.show_minor_body_labels && zoom >= 2.0);
    if (show_label) {
      draw->AddText(ImVec2(p.x + 6, p.y + 6), IM_COL32(200, 200, 200, 255), b->name.c_str());
    }
  }

  // Jump points
  for (Id jid : sys->jump_points) {
    const auto* jp = find_ptr(s.jump_points, jid);
    if (!jp) continue;

    const ImVec2 p = to_screen(jp->position_mkm, center, scale, zoom, pan);
    const float r = 6.0f;
    const bool surveyed = (!ui.fog_of_war) || sim.is_jump_point_surveyed_by_faction(viewer_faction_id, jid);
    const ImU32 col = (surveyed ? color_jump() : IM_COL32(90, 90, 100, 255));
    const ImU32 text_col = (surveyed ? IM_COL32(200, 200, 200, 255) : IM_COL32(140, 140, 150, 255));
    draw->AddCircle(p, r, col, 0, 2.0f);
    draw->AddText(ImVec2(p.x + 6, p.y - 6), text_col, jp->name.c_str());
  }

  // Selected ship order path (linked elements).
  if (ui.system_map_order_paths) {
    Id route_ship_id = selected_ship;
    if (route_ship_id == kInvalidId && selected_fleet && selected_fleet->leader_ship_id != kInvalidId) {
      route_ship_id = selected_fleet->leader_ship_id;
    }

    const auto* sh = find_ptr(s.ships, route_ship_id);
    const auto* so = sh ? find_ptr(s.ship_orders, route_ship_id) : nullptr;

    if (sh && sh->system_id == sys->id && so) {
      const bool templ = so->queue.empty() && so->repeat && !so->repeat_template.empty() &&
                         so->repeat_count_remaining != 0;
      const auto& q = (templ ? so->repeat_template : so->queue);

      auto resolve_target = [&](const nebula4x::Order& ord) -> std::optional<Vec2> {
        return std::visit(
            [&](auto&& o) -> std::optional<Vec2> {
              using T = std::decay_t<decltype(o)>;
              if constexpr (std::is_same_v<T, nebula4x::MoveToPoint>) {
                return o.target_mkm;
              } else if constexpr (std::is_same_v<T, nebula4x::MoveToBody> || std::is_same_v<T, nebula4x::ColonizeBody>
                                   || std::is_same_v<T, nebula4x::OrbitBody>) {
                const auto* b = find_ptr(s.bodies, o.body_id);
                if (!b || b->system_id != sys->id) return std::nullopt;
                return b->position_mkm;
              } else if constexpr (std::is_same_v<T, nebula4x::TravelViaJump>) {
                const auto* jp = find_ptr(s.jump_points, o.jump_point_id);
                if (!jp || jp->system_id != sys->id) return std::nullopt;
                return jp->position_mkm;
              } else if constexpr (std::is_same_v<T, nebula4x::AttackShip> ||
                                   std::is_same_v<T, nebula4x::EscortShip> ||
                                   std::is_same_v<T, nebula4x::TransferCargoToShip> ||
                                   std::is_same_v<T, nebula4x::TransferFuelToShip> ||
                                   std::is_same_v<T, nebula4x::TransferTroopsToShip>) {
                if (const auto* tgt = find_ptr(s.ships, o.target_ship_id); tgt && tgt->system_id == sys->id) {
                  return tgt->position_mkm;
                }
                if constexpr (std::is_same_v<T, nebula4x::AttackShip>) {
                  if (o.has_last_known) return o.last_known_position_mkm;
                }
                return std::nullopt;
              } else if constexpr (std::is_same_v<T, nebula4x::SalvageWreck>) {
                const auto* w = find_ptr(s.wrecks, o.wreck_id);
                if (!w || w->system_id != sys->id) return std::nullopt;
                return w->position_mkm;
              } else if constexpr (std::is_same_v<T, nebula4x::LoadMineral> || std::is_same_v<T, nebula4x::UnloadMineral>
                                   || std::is_same_v<T, nebula4x::LoadTroops> || std::is_same_v<T, nebula4x::UnloadTroops>
                                   || std::is_same_v<T, nebula4x::LoadColonists> || std::is_same_v<T, nebula4x::UnloadColonists>
                                   || std::is_same_v<T, nebula4x::InvadeColony> || std::is_same_v<T, nebula4x::BombardColony>
                                   || std::is_same_v<T, nebula4x::ScrapShip>) {
                const auto* c = find_ptr(s.colonies, o.colony_id);
                if (!c) return std::nullopt;
                const auto* b = find_ptr(s.bodies, c->body_id);
                if (!b || b->system_id != sys->id) return std::nullopt;
                return b->position_mkm;
              } else {
                // WaitDays and any other no-target order.
                return std::nullopt;
              }
            },
            ord);
      };

      if (!q.empty()) {
        const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
        const ImU32 base = templ ? IM_COL32(160, 160, 160, 255) : IM_COL32(255, 220, 80, 255);
        const ImU32 col = modulate_alpha(base, templ ? (0.55f * alpha) : alpha);
        const ImU32 col_pt = modulate_alpha(IM_COL32(10, 10, 10, 255), templ ? (0.55f * alpha) : alpha);

        Vec2 prev_w = sh->position_mkm;
        ImVec2 prev = to_screen(prev_w, center, scale, zoom, pan);
        int idx = 1;

        for (const auto& ord : q) {
          const auto tgt = resolve_target(ord);
          if (!tgt) continue;

          const ImVec2 next = to_screen(*tgt, center, scale, zoom, pan);
          draw->AddLine(prev, next, col, 2.0f);
          add_arrowhead(draw, prev, next, col, 8.0f);

          // Waypoint marker.
          draw->AddCircleFilled(next, 6.0f, col_pt, 0);
          draw->AddCircle(next, 6.0f, col, 0, 2.0f);
          char buf[8];
          std::snprintf(buf, sizeof(buf), "%d", idx++);
          draw->AddText(ImVec2(next.x - 3.0f, next.y - 6.0f), col, buf);

          prev = next;
        }
      }
    }
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

    const auto* d = sim.find_design(sh->design_id);

    DiplomacyStatus ds = DiplomacyStatus::Neutral;
    if (viewer_faction_id != kInvalidId) {
      ds = sim.diplomatic_status(viewer_faction_id, sh->faction_id);
    }

    const bool is_selected = (selected_ship == sid);
    const bool is_fleet_member = (!selected_fleet_members.empty() && selected_fleet_members.count(sid));
    const bool is_hostile = (viewer_faction_id != kInvalidId && ds == DiplomacyStatus::Hostile);

    // Weapon range rings (optional tactical overlay).
    if (d && d->weapon_range_mkm > 0.0) {
      const float rpx = static_cast<float>(d->weapon_range_mkm * scale * zoom);
      const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);

      if (ui.show_hostile_weapon_ranges && is_hostile) {
        draw->AddCircle(p, rpx, modulate_alpha(IM_COL32(255, 90, 90, 255), 0.18f * alpha), 0, 1.0f);
      }
      if (ui.show_fleet_weapon_ranges && is_fleet_member) {
        draw->AddCircle(p, rpx, modulate_alpha(IM_COL32(255, 170, 90, 255), 0.22f * alpha), 0, 1.0f);
      }
      if (ui.show_selected_weapon_range && is_selected) {
        draw->AddCircle(p, rpx, modulate_alpha(IM_COL32(255, 200, 120, 255), 0.32f * alpha), 0, 1.25f);
      }
    }

    // Selected ship sensor range overlay
    if (ui.show_selected_sensor_range && is_selected) {
      if (d && d->sensor_range_mkm > 0.0) {
        // Match actual detection: if sensors are disabled or powered down, indicate it.
        const auto pa = compute_power_allocation(d->power_generation, d->power_use_engines, d->power_use_shields,
                                                 d->power_use_weapons, d->power_use_sensors, sh->power_policy);
        const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
        const ImU32 col = pa.sensors_online
                              ? modulate_alpha(IM_COL32(0, 170, 255, 255), 0.31f * alpha)
                              : modulate_alpha(IM_COL32(255, 90, 90, 255), 0.22f * alpha);
        double mult = 1.0;
        if (sh->sensor_mode == SensorMode::Passive) mult = sim.cfg().sensor_mode_passive_range_multiplier;
        else if (sh->sensor_mode == SensorMode::Active) mult = sim.cfg().sensor_mode_active_range_multiplier;
        if (!std::isfinite(mult) || mult < 0.0) mult = 0.0;
        const double r_mkm = std::max(0.0, d->sensor_range_mkm) * mult;
        draw->AddCircle(p, static_cast<float>(r_mkm * scale * zoom), col, 0, 1.0f);
      }
    }

    // Ship marker color.
    ImU32 ship_col = color_faction(sh->faction_id);
    if (viewer_faction_id != kInvalidId) {
      if (ds == DiplomacyStatus::Friendly) {
        ship_col = IM_COL32(120, 255, 180, 255);
      } else if (ds == DiplomacyStatus::Hostile) {
        ship_col = IM_COL32(255, 120, 90, 255);
      }
    }

    const float r = (selected_ship == sid) ? 5.0f : 4.0f;
    // Subtle drop shadow to make markers pop over the background.
    draw->AddCircleFilled(ImVec2(p.x + 1.0f, p.y + 1.0f), r, IM_COL32(0, 0, 0, 140));
    draw->AddCircleFilled(p, r, ship_col);
    if (selected_ship == sid) {
      draw->AddCircle(p, 10.0f, IM_COL32(0, 255, 140, 255), 0, 1.5f);
    }


    // Highlight selected fleet members.
    if (!selected_fleet_members.empty() && selected_fleet_members.count(sid)) {
      draw->AddCircle(p, 13.0f, IM_COL32(0, 160, 255, 200), 0, 1.5f);
    }
  }

  // Missile salvos (in flight). These are visual only (damage is resolved in the simulation tick).
  if (ui.system_map_missile_salvos) {
    for (const auto& [mid, ms] : s.missile_salvos) {
      if (ms.system_id != sys->id) continue;

      // Fog-of-war: show salvos if the viewer is involved (attacker/target), or if the viewer
      // has detected either the attacker or the target ship.
      if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
        if (ms.attacker_faction_id != viewer_faction_id && ms.target_faction_id != viewer_faction_id) {
          const bool sees_attacker = sim.is_ship_detected_by_faction(viewer_faction_id, ms.attacker_ship_id);
          const bool sees_target = sim.is_ship_detected_by_faction(viewer_faction_id, ms.target_ship_id);
          if (!sees_attacker && !sees_target) continue;
        }
      }

      const double total = std::max(1e-6, ms.eta_days_total);
      const double rem = std::max(0.0, ms.eta_days_remaining);
      const double frac = std::clamp(1.0 - rem / total, 0.0, 1.0);
      const Vec2 pos_mkm = ms.launch_pos_mkm + (ms.target_pos_mkm - ms.launch_pos_mkm) * frac;

      Vec2 target_pos_mkm = ms.target_pos_mkm;
      if (const auto* tgt = find_ptr(s.ships, ms.target_ship_id); tgt && tgt->system_id == sys->id) {
        target_pos_mkm = tgt->position_mkm;
      }

      const ImVec2 p = to_screen(pos_mkm, center, scale, zoom, pan);
      const ImVec2 t = to_screen(target_pos_mkm, center, scale, zoom, pan);

      const ImU32 base = modulate_alpha(color_faction(ms.attacker_faction_id), 0.85f);
      const ImU32 trail = modulate_alpha(base, 0.22f);

      // Trail to show direction.
      draw->AddLine(p, t, trail, 1.0f);

      // Marker.
      draw->AddCircleFilled(ImVec2(p.x + 1.0f, p.y + 1.0f), 2.7f, IM_COL32(0, 0, 0, 140));
      draw->AddCircleFilled(p, 2.7f, base);
    }
  }

  // Wreck markers (salvageable debris)
  for (const auto& [wid, w] : s.wrecks) {
    if (w.system_id != sys->id) continue;
    const ImVec2 p = to_screen(w.position_mkm, center, scale, zoom, pan);
    const float r = 5.0f;
    const ImU32 c = IM_COL32(160, 160, 160, 200);
    draw->AddLine(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), c, 2.0f);
    draw->AddLine(ImVec2(p.x - r, p.y + r), ImVec2(p.x + r, p.y - r), c, 2.0f);
  }

  // Fleet formation preview: when enabled, visualize the *per-ship* target points
  // that will be produced by the formation solver (raw target + offset).
  if (ui.system_map_fleet_formation_preview && selected_fleet != nullptr &&
      selected_fleet->formation != FleetFormation::None &&
      selected_fleet->formation_spacing_mkm > 0.0) {
    struct Cohort {
      enum class Kind { MovePoint, Attack } kind{Kind::MovePoint};
      std::uint64_t x_bits{0};
      std::uint64_t y_bits{0};
      Id target_id{kInvalidId};
      std::vector<Id> members;
    };

    auto double_bits = [](double v) -> std::uint64_t {
      std::uint64_t out = 0;
      std::memcpy(&out, &v, sizeof(out));
      return out;
    };
    auto bits_to_double = [](std::uint64_t bits) -> double {
      double out = 0.0;
      std::memcpy(&out, &bits, sizeof(out));
      return out;
    };

    auto current_order_ptr = [&](Id ship_id) -> const Order* {
      const auto it = s.ship_orders.find(ship_id);
      if (it == s.ship_orders.end()) return nullptr;
      const ShipOrders& so = it->second;
      if (!so.queue.empty()) return &so.queue.front();
      if (so.repeat && !so.repeat_template.empty() && so.repeat_count_remaining != 0) {
        return &so.repeat_template.front();
      }
      return nullptr;
    };

    std::vector<Cohort> cohorts;
    cohorts.reserve(4);

    // Build cohorts from the selected fleet's ships in this system.
    for (Id sid : selected_fleet->ship_ids) {
      const auto* sh = find_ptr(s.ships, sid);
      if (!sh) continue;
      if (sh->system_id != sys->id) continue;

      const Order* ord_ptr = current_order_ptr(sid);
      if (!ord_ptr) continue;
      const Order& ord = *ord_ptr;

      Cohort cand;
      bool ok = false;
      if (std::holds_alternative<MoveToPoint>(ord)) {
        cand.kind = Cohort::Kind::MovePoint;
        const auto& mo = std::get<MoveToPoint>(ord);
        cand.x_bits = double_bits(mo.target_mkm.x);
        cand.y_bits = double_bits(mo.target_mkm.y);
        ok = true;
      } else if (std::holds_alternative<AttackShip>(ord)) {
        cand.kind = Cohort::Kind::Attack;
        const auto& ao = std::get<AttackShip>(ord);
        cand.target_id = ao.target_ship_id;
        ok = true;
      }
      if (!ok) continue;

      bool merged = false;
      for (auto& c : cohorts) {
        if (c.kind != cand.kind) continue;
        if (c.kind == Cohort::Kind::MovePoint) {
          if (c.x_bits != cand.x_bits || c.y_bits != cand.y_bits) continue;
        } else {
          if (c.target_id != cand.target_id) continue;
        }
        c.members.push_back(sid);
        merged = true;
        break;
      }
      if (!merged) {
        cand.members.push_back(sid);
        cohorts.push_back(std::move(cand));
      }
    }

    const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
    const ImU32 col_edge = modulate_alpha(IM_COL32(0, 180, 255, 255), 0.80f * alpha);
    const ImU32 col_fill = modulate_alpha(IM_COL32(0, 180, 255, 255), 0.18f * alpha);
    const ImU32 col_line = modulate_alpha(IM_COL32(0, 180, 255, 255), 0.35f * alpha);

    for (auto& c : cohorts) {
      if (c.members.size() < 2) continue;
      std::sort(c.members.begin(), c.members.end());
      c.members.erase(std::unique(c.members.begin(), c.members.end()), c.members.end());
      if (c.members.size() < 2) continue;

      // Leader selection mirrors simulation tick behavior.
      Id leader_id = selected_fleet->leader_ship_id;
      if (leader_id == kInvalidId ||
          std::find(c.members.begin(), c.members.end(), leader_id) == c.members.end()) {
        leader_id = c.members.front();
      }

      const auto* leader = find_ptr(s.ships, leader_id);
      if (!leader) continue;
      const Vec2 leader_pos = leader->position_mkm;

      Vec2 raw_target = leader_pos + Vec2{1.0, 0.0};
      if (c.kind == Cohort::Kind::MovePoint) {
        raw_target = Vec2{bits_to_double(c.x_bits), bits_to_double(c.y_bits)};
      } else {
        const Id target_ship_id = c.target_id;
        const bool detected = sim.is_ship_detected_by_faction(leader->faction_id, target_ship_id);
        if (detected) {
          if (const auto* tgt = find_ptr(s.ships, target_ship_id)) raw_target = tgt->position_mkm;
        } else {
          const Order* lord_ptr = current_order_ptr(leader_id);
          if (lord_ptr && std::holds_alternative<AttackShip>(*lord_ptr)) {
            const auto& ao = std::get<AttackShip>(*lord_ptr);
            if (ao.has_last_known) raw_target = ao.last_known_position_mkm;
          }
        }
      }

      const auto offsets = compute_fleet_formation_offsets(selected_fleet->formation,
                                                           selected_fleet->formation_spacing_mkm, leader_id,
                                                           leader_pos, raw_target, c.members);
      if (offsets.empty()) continue;

      // Raw target marker.
      const ImVec2 p_raw = to_screen(raw_target, center, scale, zoom, pan);
      draw->AddCircleFilled(p_raw, 7.5f, modulate_alpha(IM_COL32(0, 0, 0, 255), 0.25f * alpha));
      draw->AddCircle(p_raw, 7.5f, col_edge, 0, 2.0f);
      draw->AddLine(ImVec2(p_raw.x - 6.0f, p_raw.y), ImVec2(p_raw.x + 6.0f, p_raw.y), col_edge, 1.5f);
      draw->AddLine(ImVec2(p_raw.x, p_raw.y - 6.0f), ImVec2(p_raw.x, p_raw.y + 6.0f), col_edge, 1.5f);

      // Per-ship target markers.
      int slot = 1;
      for (Id sid : c.members) {
        const auto* sh = find_ptr(s.ships, sid);
        if (!sh) continue;
        const auto it = offsets.find(sid);
        if (it == offsets.end()) continue;

        const Vec2 tgt_w = raw_target + it->second;
        const ImVec2 p_tgt = to_screen(tgt_w, center, scale, zoom, pan);
        const ImVec2 p_src = to_screen(sh->position_mkm, center, scale, zoom, pan);

        draw->AddLine(p_src, p_tgt, col_line, 1.0f);
        draw->AddCircleFilled(p_tgt, 4.0f, col_fill);
        draw->AddCircle(p_tgt, 4.0f, col_edge, 0, 1.0f);

        if (zoom >= 2.0f) {
          char buf[8];
          if (sid == leader_id) {
            std::snprintf(buf, sizeof(buf), "L");
          } else {
            std::snprintf(buf, sizeof(buf), "%d", slot++);
          }
          draw->AddText(ImVec2(p_tgt.x + 6.0f, p_tgt.y - 6.0f), col_edge, buf);
        }
      }
    }
  }

  // Contact markers (last known positions)
  if (!recent_contacts.empty() && viewer_faction_id != kInvalidId) {
    const int now = static_cast<int>(s.date.days_since_epoch());

    for (const auto& c : recent_contacts) {
      // Don't draw a contact marker if the ship is currently detected (we already draw the ship itself).
      if (c.ship_id != kInvalidId && sim.is_ship_detected_by_faction(viewer_faction_id, c.ship_id)) continue;

      const int age = std::max(0, now - c.last_seen_day);
      const ImVec2 p = to_screen(c.last_seen_position_mkm, center, scale, zoom, pan);
      const float t = 1.0f - (ui.contact_max_age_days > 0 ? (static_cast<float>(age) / static_cast<float>(ui.contact_max_age_days)) : 1.0f);
      const int a = std::clamp(static_cast<int>(60 + 140 * std::clamp(t, 0.0f, 1.0f)), 40, 220);
      const ImU32 col = IM_COL32(255, 180, 0, a);

      draw->AddCircle(p, 6.0f, col, 0, 2.0f);
      draw->AddLine(ImVec2(p.x - 5, p.y - 5), ImVec2(p.x + 5, p.y + 5), col, 2.0f);
      draw->AddLine(ImVec2(p.x - 5, p.y + 5), ImVec2(p.x + 5, p.y - 5), col, 2.0f);

      // Highlight the actively selected contact (from Intel window / previous clicks).
      if (ui.selected_contact_ship_id != kInvalidId && c.ship_id == ui.selected_contact_ship_id) {
        const float t_p = (float)ImGui::GetTime();
        const float pulse = 0.5f + 0.5f * std::sin(t_p * 4.0f);
        const float r = 10.0f + pulse * 4.0f;
        draw->AddCircle(p, r, IM_COL32(255, 230, 140, 190), 0, 2.5f);
      }

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
          const bool is_minor = (b->type == BodyType::Asteroid || b->type == BodyType::Comet);
          if (!ui.show_minor_bodies && is_minor) continue;
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
          const bool is_minor = (b->type == BodyType::Asteroid || b->type == BodyType::Comet);
          if (!ui.show_minor_bodies && is_minor) continue;
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
        } else if (!recent_contacts.empty()) {
          // Treat contact markers as a distinct selectable entity in fog-of-war mode.
          Id picked_contact = kInvalidId;
          float best_contact_d2 = pick_d2;
          for (const auto& c : recent_contacts) {
            if (c.ship_id == kInvalidId) continue;
            // Skip contacts that are currently detected (the real ship marker is pickable).
            if (viewer_faction_id != kInvalidId && sim.is_ship_detected_by_faction(viewer_faction_id, c.ship_id)) continue;
            const ImVec2 p = to_screen(c.last_seen_position_mkm, center, scale, zoom, pan);
            const float dx = mp.x - p.x;
            const float dy = mp.y - p.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 <= best_contact_d2) {
              best_contact_d2 = d2;
              picked_contact = c.ship_id;
            }
          }

          if (picked_contact != kInvalidId && best_contact_d2 <= best_body_d2) {
            ui.selected_contact_ship_id = picked_contact;
            ui.show_intel_window = true;
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

  // Hover tooltip (clickable links).
  if (hovered && mouse_in_rect && !ImGui::IsAnyItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
    const ImVec2 mp = mouse;
    constexpr float kHoverRadiusPx = 18.0f;
    const float hover_d2 = kHoverRadiusPx * kHoverRadiusPx;

    enum class HoverKind { None, Ship, Missile, Wreck, Body, Jump };
    HoverKind kind = HoverKind::None;
    Id hovered_id = kInvalidId;
    float best_d2 = hover_d2;

    // Prefer ships first (more common interaction target), then bodies, then jumps.
    for (Id sid : sys->ships) {
      const auto* sh = find_ptr(s.ships, sid);
      if (!sh) continue;

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
      if (d2 <= best_d2) {
        best_d2 = d2;
        kind = HoverKind::Ship;
        hovered_id = sid;
      }
    }

    // Missile salvos (optional overlay).
    if (kind == HoverKind::None && ui.system_map_missile_salvos) {
      for (const auto& [mid, ms] : s.missile_salvos) {
        if (ms.system_id != sys->id) continue;

        if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
          if (ms.attacker_faction_id != viewer_faction_id && ms.target_faction_id != viewer_faction_id) {
            const bool sees_attacker = sim.is_ship_detected_by_faction(viewer_faction_id, ms.attacker_ship_id);
            const bool sees_target = sim.is_ship_detected_by_faction(viewer_faction_id, ms.target_ship_id);
            if (!sees_attacker && !sees_target) continue;
          }
        }

        const double total = std::max(1e-6, ms.eta_days_total);
        const double rem = std::max(0.0, ms.eta_days_remaining);
        const double frac = std::clamp(1.0 - rem / total, 0.0, 1.0);
        const Vec2 pos_mkm = ms.launch_pos_mkm + (ms.target_pos_mkm - ms.launch_pos_mkm) * frac;

        const ImVec2 p = to_screen(pos_mkm, center, scale, zoom, pan);
        const float dx = mp.x - p.x;
        const float dy = mp.y - p.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= best_d2) {
          best_d2 = d2;
          kind = HoverKind::Missile;
          hovered_id = mid;
        }
      }
    }

    if (kind == HoverKind::None) {
      for (const auto& [wid, w] : s.wrecks) {
        if (w.system_id != sys->id) continue;
        const ImVec2 p = to_screen(w.position_mkm, center, scale, zoom, pan);
        const float dx = mp.x - p.x;
        const float dy = mp.y - p.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= best_d2) {
          best_d2 = d2;
          kind = HoverKind::Wreck;
          hovered_id = wid;
        }
      }
    }

    if (kind == HoverKind::None) {
      for (Id bid : sys->bodies) {
        const auto* b = find_ptr(s.bodies, bid);
        if (!b) continue;
        const bool is_minor = (b->type == BodyType::Asteroid || b->type == BodyType::Comet);
        if (!ui.show_minor_bodies && is_minor) continue;

        const ImVec2 p = to_screen(b->position_mkm, center, scale, zoom, pan);
        const float dx = mp.x - p.x;
        const float dy = mp.y - p.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= best_d2) {
          best_d2 = d2;
          kind = HoverKind::Body;
          hovered_id = bid;
        }
      }
    }

    if (kind == HoverKind::None) {
      for (Id jid : sys->jump_points) {
        const auto* jp = find_ptr(s.jump_points, jid);
        if (!jp) continue;
        const ImVec2 p = to_screen(jp->position_mkm, center, scale, zoom, pan);
        const float dx = mp.x - p.x;
        const float dy = mp.y - p.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= best_d2) {
          best_d2 = d2;
          kind = HoverKind::Jump;
          hovered_id = jid;
        }
      }
    }

    if (kind != HoverKind::None && hovered_id != kInvalidId) {
      ImGui::BeginTooltip();
      if (kind == HoverKind::Ship) {
        const auto* sh = find_ptr(s.ships, hovered_id);
        if (sh) {
          ImGui::Text("%s", sh->name.c_str());
          if (const auto* f = find_ptr(s.factions, sh->faction_id)) {
            ImGui::TextDisabled("Faction: %s", f->name.c_str());
          }
          ImGui::TextDisabled("Design: %s", sh->design_id.c_str());

          if (auto it = s.ship_orders.find(hovered_id); it != s.ship_orders.end()) {
            ImGui::TextDisabled("Orders: %d", static_cast<int>(it->second.queue.size()));
          }

          if (ImGui::SmallButton("Select")) {
            selected_ship = hovered_id;
            ui.selected_fleet_id = sim.fleet_for_ship(hovered_id);
            ui.request_details_tab = DetailsTab::Ship;
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Follow")) {
            selected_ship = hovered_id;
            ui.selected_fleet_id = sim.fleet_for_ship(hovered_id);
            ui.system_map_follow_selected = true;
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Center")) {
            pan = Vec2{-sh->position_mkm.x, -sh->position_mkm.y};
            ui.system_map_follow_selected = false;
          }
        }
      } else if (kind == HoverKind::Missile) {
        const auto* ms = find_ptr(s.missile_salvos, hovered_id);
        if (ms) {
          ImGui::TextUnformatted("Missile salvo");
          ImGui::Separator();

          const Ship* attacker = find_ptr(s.ships, ms->attacker_ship_id);
          const Ship* target = find_ptr(s.ships, ms->target_ship_id);

          if (attacker) {
            ImGui::TextDisabled("Attacker: %s", attacker->name.c_str());
          } else {
            ImGui::TextDisabled("Attacker: Ship %llu", static_cast<unsigned long long>(ms->attacker_ship_id));
          }
          if (target) {
            ImGui::TextDisabled("Target: %s", target->name.c_str());
          } else {
            ImGui::TextDisabled("Target: Ship %llu", static_cast<unsigned long long>(ms->target_ship_id));
          }

          ImGui::TextDisabled("ETA: %s", format_duration_days(std::max(0.0, ms->eta_days_remaining)).c_str());
          const double payload = (ms->damage_initial > 1e-12) ? ms->damage_initial : ms->damage;
          ImGui::TextDisabled("Payload: %.1f (remaining %.1f)", payload, std::max(0.0, ms->damage));

          if (target && ImGui::SmallButton("Select target")) {
            selected_ship = target->id;
            ui.selected_fleet_id = sim.fleet_for_ship(target->id);
            ui.request_details_tab = DetailsTab::Ship;
          }
          if (attacker) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Select attacker")) {
              selected_ship = attacker->id;
              ui.selected_fleet_id = sim.fleet_for_ship(attacker->id);
              ui.request_details_tab = DetailsTab::Ship;
            }
          }
          if (target) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Center")) {
              pan = Vec2{-target->position_mkm.x, -target->position_mkm.y};
              ui.system_map_follow_selected = false;
            }
          }
        }
      } else if (kind == HoverKind::Wreck) {
        const auto* w = find_ptr(s.wrecks, hovered_id);
        if (w) {
          ImGui::Text("%s", w->name.c_str());
          if (const auto* sys2 = find_ptr(s.systems, w->system_id)) {
            ImGui::TextDisabled("System: %s", sys2->name.c_str());
          }
          double total = 0.0;
          for (const auto& [_, t] : w->minerals) total += t;
          ImGui::TextDisabled("Salvage: %.1f tons", total);
          // Show up to 6 minerals (largest first).
          std::vector<std::pair<std::string, double>> items;
          items.reserve(w->minerals.size());
          for (const auto& [m, t] : w->minerals) items.emplace_back(m, t);
          std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
          int shown = 0;
          for (const auto& [m, t] : items) {
            if (shown++ >= 6) break;
            ImGui::BulletText("%s: %.1f", m.c_str(), t);
          }
        }
      } else if (kind == HoverKind::Body) {
        const auto* b = find_ptr(s.bodies, hovered_id);
        if (b) {
          ImGui::Text("%s", b->name.c_str());
          ImGui::TextDisabled("Type: %s", body_type_to_string(b->type).c_str());
          ImGui::TextDisabled("Orbit: %.1f mkm", b->orbit_radius_mkm);

          // Colony summary (if any).
          for (const auto& [cid, c] : s.colonies) {
            if (c.body_id != hovered_id) continue;
            if (const auto* f = find_ptr(s.factions, c.faction_id)) {
              ImGui::TextDisabled("Colony: %s (%s)", c.name.c_str(), f->name.c_str());
            } else {
              ImGui::TextDisabled("Colony: %s", c.name.c_str());
            }
            ImGui::TextDisabled("Population: %.3f B", c.population_millions / 1000.0);
            break;
          }

          if (ImGui::SmallButton("Select")) {
            selected_body = hovered_id;
            // Select colony on that body if present.
            for (const auto& [cid, c] : s.colonies) {
              if (c.body_id == hovered_id) {
                selected_colony = cid;
                ui.request_details_tab = DetailsTab::Colony;
                break;
              }
            }
            if (selected_colony == kInvalidId) {
              ui.request_details_tab = DetailsTab::Body;
            }
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Center")) {
            pan = Vec2{-b->position_mkm.x, -b->position_mkm.y};
            ui.system_map_follow_selected = false;
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Details")) {
            selected_body = hovered_id;
            ui.request_details_tab = DetailsTab::Body;
          }
        }
      } else if (kind == HoverKind::Jump) {
        const auto* jp = find_ptr(s.jump_points, hovered_id);
        if (jp) {
          ImGui::Text("%s", jp->name.c_str());

          const bool surveyed = (!ui.fog_of_war) ||
                                sim.is_jump_point_surveyed_by_faction(viewer_faction_id, jp->id);
          ImGui::TextDisabled("Surveyed: %s", surveyed ? "Yes" : "No");

          if (!surveyed) {
            ImGui::TextDisabled("To: (unknown)");
          } else if (const auto* other = find_ptr(s.jump_points, jp->linked_jump_id)) {
            if (const auto* dest = find_ptr(s.systems, other->system_id)) {
              if (!ui.fog_of_war || sim.is_system_discovered_by_faction(viewer_faction_id, dest->id)) {
                ImGui::TextDisabled("To: %s", dest->name.c_str());
              } else {
                ImGui::TextDisabled("To: (undiscovered system)");
              }
            } else {
              ImGui::TextDisabled("To: (unknown system)");
            }
          }

          if (ImGui::SmallButton("Center")) {
            pan = Vec2{-jp->position_mkm.x, -jp->position_mkm.y};
            ui.system_map_follow_selected = false;
          }
          if (can_issue_orders && ImGui::SmallButton("Travel")) {
            if (fleet_mode) {
              sim.issue_fleet_travel_via_jump(selected_fleet->id, hovered_id);
            } else if (selected_ship != kInvalidId) {
              sim.issue_travel_via_jump(selected_ship, hovered_id);
            }
          }
        }
      }
      ImGui::EndTooltip();
    }
  }

  // Legend / help
  ImGui::SetCursorScreenPos(ImVec2(origin.x + 10, origin.y + 10));
  ImGui::BeginChild("legend", ImVec2(320, 480), true);
  ImGui::Text("Controls");
  ImGui::BulletText("Mouse wheel: zoom (to cursor)");
  ImGui::BulletText("Middle drag: pan");
  ImGui::BulletText("R: reset view, F: follow selected");
  ImGui::BulletText("Left click: issue order to ship (Shift queues)");
  ImGui::BulletText("Right click: select ship/body (no orders)");
  ImGui::BulletText("Alt+Left click body: colonize (colony ship required)");
  ImGui::BulletText("Ctrl+Left click: issue order to fleet");
  ImGui::BulletText("Click body: move-to-body");
  ImGui::BulletText("Click jump point: travel via jump");
  ImGui::BulletText("Jump points are purple rings");

  ImGui::SeparatorText("Map overlays");
  ImGui::Checkbox("Starfield", &ui.system_map_starfield);
  ImGui::SameLine();
  ImGui::Checkbox("Grid", &ui.system_map_grid);
  ImGui::Checkbox("Order paths", &ui.system_map_order_paths);
  ImGui::SameLine();
  ImGui::Checkbox("Missiles", &ui.system_map_missile_salvos);
  ImGui::SameLine();
  ImGui::Checkbox("Formation preview", &ui.system_map_fleet_formation_preview);
  ImGui::Checkbox("Follow (F)", &ui.system_map_follow_selected);
  if (ImGui::Button("Reset view (R)")) {
    zoom = 1.0;
    pan = Vec2{0.0, 0.0};
    ui.system_map_follow_selected = false;
  }

  {
    const Vec2 w = to_world(mouse, center, scale, zoom, pan);
    ImGui::TextDisabled("Cursor: %.1f, %.1f mkm", w.x, w.y);
    ImGui::TextDisabled("Zoom: %.2fx", zoom);
  }

  ImGui::Separator();
  ImGui::Checkbox("Fog of war", &ui.fog_of_war);
  ImGui::Checkbox("Show sensor range", &ui.show_selected_sensor_range);
  ImGui::Checkbox("Weapon range (selected)", &ui.show_selected_weapon_range);
  ImGui::SameLine();
  ImGui::Checkbox("Fleet", &ui.show_fleet_weapon_ranges);
  ImGui::SameLine();
  ImGui::Checkbox("Hostiles", &ui.show_hostile_weapon_ranges);

  if (selected_fleet != nullptr) {
    ImGui::Separator();
    ImGui::Text("Selected fleet: %s", selected_fleet->name.c_str());

    static const char* kFormations[] = {"None", "Line abreast", "Column", "Wedge", "Ring"};
    int f = static_cast<int>(selected_fleet->formation);
    if (ImGui::Combo("Formation##map_fleet_form", &f, kFormations, IM_ARRAYSIZE(kFormations))) {
      sim.configure_fleet_formation(selected_fleet->id, static_cast<FleetFormation>(f),
                                    selected_fleet->formation_spacing_mkm);
    }
    double spacing = selected_fleet->formation_spacing_mkm;
    if (ImGui::InputDouble("Spacing (mkm)##map_fleet_form", &spacing, 5.0, 25.0, "%.1f")) {
      spacing = std::clamp(spacing, 0.0, 1.0e9);
      sim.configure_fleet_formation(selected_fleet->id, static_cast<FleetFormation>(f), spacing);
    }
  }
  ImGui::Checkbox("Show contacts", &ui.show_contact_markers);
  ImGui::SameLine();
  ImGui::Checkbox("Labels", &ui.show_contact_labels);

  ImGui::Separator();
  ImGui::Checkbox("Show minor bodies", &ui.show_minor_bodies);
  ImGui::SameLine();
  ImGui::Checkbox("Minor labels", &ui.show_minor_body_labels);
  ImGui::TextDisabled("(Minor labels appear at zoom >= 2x or when selected)");

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
