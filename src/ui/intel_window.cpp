#include "ui/intel_window.h"

#include "ui/map_render.h"

#include "core/simulation_sensors.h"

#include "nebula4x/core/contact_prediction.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nebula4x::ui {
namespace {

constexpr double kTwoPi = 6.283185307179586;

bool case_insensitive_contains(const std::string& haystack, const char* needle_cstr) {
  if (!needle_cstr) return true;
  if (needle_cstr[0] == '\0') return true;
  const std::string needle(needle_cstr);
  const auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      });
  return it != haystack.end();
}

// Keep faction colors consistent with the system map.
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

ImVec2 to_screen_radar(const Vec2& world_mkm, const ImVec2& center_px, double px_per_mkm, const Vec2& pan_mkm) {
  const double sx = (world_mkm.x + pan_mkm.x) * px_per_mkm;
  const double sy = (world_mkm.y + pan_mkm.y) * px_per_mkm;
  return ImVec2(static_cast<float>(center_px.x + sx), static_cast<float>(center_px.y + sy));
}

Vec2 to_world_radar(const ImVec2& screen_px, const ImVec2& center_px, double px_per_mkm, const Vec2& pan_mkm) {
  const double x = (screen_px.x - center_px.x) / px_per_mkm - pan_mkm.x;
  const double y = (screen_px.y - center_px.y) / px_per_mkm - pan_mkm.y;
  return Vec2{x, y};
}

double compute_auto_range_mkm(const Simulation& sim, Id viewer_faction_id, const StarSystem& sys, bool fog_of_war,
                              int contact_max_age_days) {
  const auto& s = sim.state();
  double maxd = 0.0;

  auto consider = [&](const Vec2& p) {
    const double d = p.length();
    if (std::isfinite(d)) maxd = std::max(maxd, d);
  };

  // Bodies and jump points (always "ground truth" for a discovered system).
  for (Id bid : sys.bodies) {
    const auto* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    consider(b->position_mkm);
  }
  for (Id jid : sys.jump_points) {
    const auto* j = find_ptr(s.jump_points, jid);
    if (!j) continue;
    consider(j->position_mkm);
  }

  // Friendly ships.
  for (Id shid : sys.ships) {
    const auto* sh = find_ptr(s.ships, shid);
    if (!sh) continue;
    if (viewer_faction_id != kInvalidId && sh->faction_id == viewer_faction_id) consider(sh->position_mkm);
  }

  // Detected hostiles (if fog-of-war), otherwise all ships.
  if (!fog_of_war || viewer_faction_id == kInvalidId) {
    for (Id shid : sys.ships) {
      const auto* sh = find_ptr(s.ships, shid);
      if (!sh) continue;
      consider(sh->position_mkm);
    }
  } else {
    for (Id shid : sim.detected_hostile_ships_in_system(viewer_faction_id, sys.id)) {
      const auto* sh = find_ptr(s.ships, shid);
      if (!sh) continue;
      consider(sh->position_mkm);
    }
    for (const auto& c : sim.recent_contacts_in_system(viewer_faction_id, sys.id, contact_max_age_days)) {
      consider(c.last_seen_position_mkm);
    }
  }

  // Sensor ranges (so the radar can "fit" the coverage rings).
  if (viewer_faction_id != kInvalidId) {
    for (const auto& src : sim_sensors::gather_sensor_sources(sim, viewer_faction_id, sys.id)) {
      const double d = src.pos_mkm.length() + std::max(0.0, src.range_mkm);
      if (std::isfinite(d)) maxd = std::max(maxd, d);
    }
  }

  // Margin + sane minimum.
  maxd = std::max(50.0, maxd * 1.08);
  if (!std::isfinite(maxd)) maxd = 500.0;
  return maxd;
}

struct SystemOption {
  Id id{kInvalidId};
  std::string name;
};

std::vector<SystemOption> build_system_options(const Simulation& sim, Id viewer_faction_id, bool fog_of_war) {
  const auto& s = sim.state();
  std::vector<SystemOption> out;
  out.reserve(s.systems.size());
  for (const auto& [sid, sys] : s.systems) {
    if (fog_of_war && viewer_faction_id != kInvalidId) {
      if (!sim.is_system_discovered_by_faction(viewer_faction_id, sid)) continue;
    }
    out.push_back(SystemOption{sid, sys.name});
  }
  std::sort(out.begin(), out.end(), [](const SystemOption& a, const SystemOption& b) { return a.name < b.name; });
  return out;
}

void draw_diamond(ImDrawList* draw, const ImVec2& p, float r, ImU32 col_fill, ImU32 col_outline) {
  const ImVec2 a(p.x, p.y - r);
  const ImVec2 b(p.x + r, p.y);
  const ImVec2 c(p.x, p.y + r);
  const ImVec2 d(p.x - r, p.y);
  draw->AddTriangleFilled(a, b, c, col_fill);
  draw->AddTriangleFilled(a, c, d, col_fill);
  draw->AddLine(a, b, col_outline, 1.0f);
  draw->AddLine(b, c, col_outline, 1.0f);
  draw->AddLine(c, d, col_outline, 1.0f);
  draw->AddLine(d, a, col_outline, 1.0f);
}

void draw_triangle(ImDrawList* draw, const ImVec2& p, float r, ImU32 col_fill, ImU32 col_outline) {
  const ImVec2 a(p.x, p.y - r);
  const ImVec2 b(p.x + r * 0.86f, p.y + r * 0.60f);
  const ImVec2 c(p.x - r * 0.86f, p.y + r * 0.60f);
  draw->AddTriangleFilled(a, b, c, col_fill);
  draw->AddTriangle(a, b, c, col_outline, 1.0f);
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }

} // namespace

void draw_intel_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  (void)selected_colony;
  (void)selected_body;

  ImGui::SetNextWindowSize(ImVec2(1050, 680), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Intel", &ui.show_intel_window)) {
    ImGui::End();
    return;
  }

  auto& s = sim.state();

  const Ship* viewer_ship = (selected_ship != kInvalidId) ? find_ptr(s.ships, selected_ship) : nullptr;
  const Id viewer_faction_id = viewer_ship ? viewer_ship->faction_id : ui.viewer_faction_id;
  const Faction* viewer = (viewer_faction_id != kInvalidId) ? find_ptr(s.factions, viewer_faction_id) : nullptr;

  if (ui.fog_of_war) {
    if (!viewer) {
      ImGui::TextDisabled("Fog-of-war is enabled.");
      ImGui::TextDisabled("Select a ship (or set a viewer faction in Research) to view intel.");
      ImGui::End();
      return;
    }
    if (!sim.is_system_discovered_by_faction(viewer_faction_id, s.selected_system)) {
      ImGui::TextDisabled("Selected system is not discovered by the viewer faction.");
      ImGui::TextDisabled("(Pick a discovered system from the combo below.)");
    }
  }

  // --- Top bar ---
  {
    ImGui::SeparatorText("Context");

    // Viewer faction summary.
    if (viewer) {
      ImGui::Text("Viewer: %s", viewer->name.c_str());
    } else {
      ImGui::TextDisabled("Viewer: (none)");
    }
    if (viewer_ship) {
      ImGui::SameLine();
      ImGui::TextDisabled("(from selected ship)");
    }

    // System selector.
    const auto systems = build_system_options(sim, viewer_faction_id, ui.fog_of_war);
    const StarSystem* sys = find_ptr(s.systems, s.selected_system);
    const char* sys_label = sys ? sys->name.c_str() : "(none)";

    if (ImGui::BeginCombo("System", sys_label)) {
      for (const auto& opt : systems) {
        const bool sel = (opt.id == s.selected_system);
        if (ImGui::Selectable(opt.name.c_str(), sel)) {
          s.selected_system = opt.id;
          // Also clear any ship selection that is now out of system.
          if (selected_ship != kInvalidId) {
            const auto* sh = find_ptr(s.ships, selected_ship);
            if (!sh || sh->system_id != s.selected_system) {
              selected_ship = kInvalidId;
            }
          }
        }
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Scanline", &ui.intel_radar_scanline);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &ui.intel_radar_grid);
    ImGui::SameLine();
    ImGui::Checkbox("Sensors", &ui.intel_radar_show_sensors);
    ImGui::SameLine();
    ImGui::Checkbox("Sensor heat", &ui.intel_radar_sensor_heat);
  }

  const StarSystem* sys = find_ptr(s.systems, s.selected_system);
  if (!sys) {
    ImGui::TextDisabled("No system selected.");
    ImGui::End();
    return;
  }

  // --- Persistent radar view state ---
  struct RadarState {
    Id system_id{kInvalidId};
    Id viewer_faction_id{kInvalidId};
    Vec2 pan_mkm{0.0, 0.0};
    double zoom{1.0};
    double base_range_mkm{500.0};
  };
  static RadarState radar;

  if (radar.system_id != sys->id || radar.viewer_faction_id != viewer_faction_id) {
    radar.system_id = sys->id;
    radar.viewer_faction_id = viewer_faction_id;
    radar.pan_mkm = Vec2{0.0, 0.0};
    radar.zoom = 1.0;
    radar.base_range_mkm = compute_auto_range_mkm(sim, viewer_faction_id, *sys, ui.fog_of_war, ui.contact_max_age_days);
  }

  // --- Main split ---
  const ImGuiTableFlags split_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
  if (!ImGui::BeginTable("intel_split", 2, split_flags)) {
    ImGui::End();
    return;
  }

  // -------------------- Radar (left) --------------------
  ImGui::TableNextColumn();
  {
    ImGui::BeginChild("##intel_radar", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::min(avail.x, avail.y);
    const ImVec2 size(side, side);
    const ImVec2 center(origin.x + side * 0.5f, origin.y + side * 0.5f);
    const float radius_px = side * 0.48f;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 bg = IM_COL32(10, 12, 16, 255);
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), bg);

    // Subtle radial glow.
    for (int i = 0; i < 5; ++i) {
      const float t = (float)i / 4.0f;
      const float r = radius_px * lerp(1.05f, 0.30f, t);
      const float a = lerp(0.10f, 0.02f, t);
      draw->AddCircleFilled(center, r, modulate_alpha(IM_COL32(60, 80, 95, 255), a), 0);
    }
    draw->AddCircleFilled(center, radius_px, IM_COL32(12, 16, 20, 255), 0);

    // Compute radar scale.
    radar.base_range_mkm = std::max(10.0, radar.base_range_mkm);
    radar.zoom = std::clamp(radar.zoom, 0.10, 25.0);
    const double px_per_mkm = (double)radius_px / radar.base_range_mkm * radar.zoom;
    const double visible_range_mkm = radar.base_range_mkm / radar.zoom;

    // Interaction.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsWindowHovered() && mouse.x >= origin.x && mouse.y >= origin.y &&
                         mouse.x <= origin.x + size.x && mouse.y <= origin.y + size.y;

    if (hovered) {
      const float wheel = ImGui::GetIO().MouseWheel;
      if (std::abs(wheel) > 1e-6f) {
        const Vec2 before = to_world_radar(mouse, center, px_per_mkm, radar.pan_mkm);
        const double factor = std::pow(1.12, (double)wheel);
        radar.zoom = std::clamp(radar.zoom * factor, 0.10, 25.0);
        const double px_per_mkm2 = (double)radius_px / radar.base_range_mkm * radar.zoom;
        const Vec2 after = to_world_radar(mouse, center, px_per_mkm2, radar.pan_mkm);
        radar.pan_mkm = radar.pan_mkm + (after - before);
      }

      if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        radar.pan_mkm.x += (double)d.x / px_per_mkm;
        radar.pan_mkm.y += (double)d.y / px_per_mkm;
      }

      if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        radar.pan_mkm = Vec2{0.0, 0.0};
        radar.zoom = 1.0;
        radar.base_range_mkm = compute_auto_range_mkm(sim, viewer_faction_id, *sys, ui.fog_of_war, ui.contact_max_age_days);
      }
    }

    // Grid rings.
    if (ui.intel_radar_grid) {
      const double ring_step = nice_number_125(std::max(1.0, visible_range_mkm / 4.0));
      const ImU32 grid_col = modulate_alpha(IM_COL32(120, 150, 170, 255), 0.18f);
      for (double r_mkm = ring_step; r_mkm <= visible_range_mkm + 1e-6; r_mkm += ring_step) {
        const float r_px = (float)(r_mkm * px_per_mkm);
        draw->AddCircle(center, r_px, grid_col, 0, 1.0f);
      }
      draw->AddLine(ImVec2(center.x - radius_px, center.y), ImVec2(center.x + radius_px, center.y), grid_col, 1.0f);
      draw->AddLine(ImVec2(center.x, center.y - radius_px), ImVec2(center.x, center.y + radius_px), grid_col, 1.0f);
    }

    // Border.
    draw->AddCircle(center, radius_px, IM_COL32(220, 220, 230, 64), 0, 1.5f);

    // Scanline.
    double scan_ang = 0.0;
    if (ui.intel_radar_scanline) {
      scan_ang = std::fmod(ImGui::GetTime() * 0.65, kTwoPi);
      const ImVec2 end(center.x + static_cast<float>(std::cos(scan_ang)) * radius_px,
                          center.y + static_cast<float>(std::sin(scan_ang)) * radius_px);
      draw->AddLine(center, end, modulate_alpha(IM_COL32(0, 255, 200, 255), 0.14f), 2.0f);
      // A faint "trail" behind the sweep.
      for (int i = 1; i <= 10; ++i) {
        const double a = scan_ang - (double)i * 0.055;
        const float aa = 0.08f * (1.0f - (float)i / 10.0f);
        const ImVec2 e2(center.x + static_cast<float>(std::cos(a)) * radius_px,
                         center.y + static_cast<float>(std::sin(a)) * radius_px);
        draw->AddLine(center, e2, modulate_alpha(IM_COL32(0, 255, 200, 255), aa), 1.0f);
      }
    }

    // Gather render data.
    std::vector<sim_sensors::SensorSource> sensor_sources;
    if (ui.intel_radar_show_sensors && viewer_faction_id != kInvalidId) {
      sensor_sources = sim_sensors::gather_sensor_sources(sim, viewer_faction_id, sys->id);
    }

    std::vector<Contact> contacts;
    std::vector<Id> detected_hostiles;
    if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
      contacts = sim.recent_contacts_in_system(viewer_faction_id, sys->id, ui.contact_max_age_days);
      detected_hostiles = sim.detected_hostile_ships_in_system(viewer_faction_id, sys->id);
    }

    // Selected contact (for EW-aware sensor overlays).
    const ShipDesign* selected_contact_design = nullptr;
    double selected_contact_sig = 1.0;
    double selected_contact_ecm = 0.0;

    if (viewer && ui.selected_contact_ship_id != kInvalidId) {
      auto itc = viewer->ship_contacts.find(ui.selected_contact_ship_id);
      if (itc != viewer->ship_contacts.end()) {
        selected_contact_design = sim.find_design(itc->second.last_seen_design_id);
        if (selected_contact_design) {
          selected_contact_sig = std::isfinite(selected_contact_design->signature_multiplier)
                                   ? selected_contact_design->signature_multiplier
                                   : 1.0;
          selected_contact_sig = std::clamp(selected_contact_sig, 0.0,
                                            sim_sensors::max_signature_multiplier_for_detection(sim));
          selected_contact_ecm = std::isfinite(selected_contact_design->ecm_strength)
                                   ? std::max(0.0, selected_contact_design->ecm_strength)
                                   : 0.0;
        }
      }
    }

    // Sensor heat / rings.
    if (!sensor_sources.empty()) {
      const ImU32 fill_col = IM_COL32(0, 190, 255, 255);
      const ImU32 ring_col = IM_COL32(0, 220, 255, 255);
      for (const auto& src : sensor_sources) {
        const ImVec2 p = to_screen_radar(src.pos_mkm, center, px_per_mkm, radar.pan_mkm);
        const float rr = (float)(src.range_mkm * px_per_mkm);
        if (ui.intel_radar_sensor_heat && rr > 2.0f) {
          draw->AddCircleFilled(p, rr, modulate_alpha(fill_col, 0.03f), 0);
        }
        // Rings remain useful even when heat is off.
        draw->AddCircle(p, rr, modulate_alpha(ring_col, 0.14f), 0, 1.0f);
        draw->AddCircleFilled(p, 2.1f, modulate_alpha(IM_COL32(0, 220, 255, 255), 0.70f), 0);

        // If a contact is selected, draw the *effective* detection range against
        // that contact's assumed signature + ECM (ECCM counters).
        //
        // This helps explain why a contact may blink in/out near the edge of
        // sensor range under electronic warfare.
        if (selected_contact_design) {
          const double eccm = std::max(0.0, src.eccm_strength);
          double ew_mult = (1.0 + eccm) / (1.0 + selected_contact_ecm);
          if (!std::isfinite(ew_mult)) ew_mult = 1.0;
          ew_mult = std::clamp(ew_mult, 0.1, 10.0);

          const double rr2_mkm = std::max(0.0, src.range_mkm) * selected_contact_sig * ew_mult;
          const float rr2 = (float)(rr2_mkm * px_per_mkm);
          if (rr2 > 2.0f) {
            const ImU32 ew_col = IM_COL32(255, 200, 90, 255);
            draw->AddCircle(p, rr2, modulate_alpha(ew_col, 0.16f), 0, 1.2f);
          }
        }
      }
    }

    // Bodies.
    if (ui.intel_radar_show_bodies) {
      for (Id bid : sys->bodies) {
        const auto* b = find_ptr(s.bodies, bid);
        if (!b) continue;
        const ImVec2 p = to_screen_radar(b->position_mkm, center, px_per_mkm, radar.pan_mkm);
        float r = 2.4f;
        if (b->type == BodyType::Star) r = 4.2f;
        else if (b->type == BodyType::GasGiant) r = 3.4f;
        else if (b->type == BodyType::Planet) r = 3.0f;
        const ImU32 col = color_body(b->type);
        draw->AddCircleFilled(p, r, modulate_alpha(col, 0.90f), 0);
        draw->AddCircle(p, r + 0.5f, modulate_alpha(IM_COL32(0, 0, 0, 255), 0.65f), 0, 1.0f);
        if (ui.intel_radar_labels && radar.zoom >= 1.4) {
          draw->AddText(ImVec2(p.x + r + 3.0f, p.y - r - 2.0f), modulate_alpha(col, 0.85f), b->name.c_str());
        }
      }
    }

    // Jump points.
    if (ui.intel_radar_show_jump_points) {
      for (Id jid : sys->jump_points) {
        const auto* j = find_ptr(s.jump_points, jid);
        if (!j) continue;
        const ImVec2 p = to_screen_radar(j->position_mkm, center, px_per_mkm, radar.pan_mkm);
        draw_diamond(draw, p, 4.2f, modulate_alpha(IM_COL32(200, 120, 255, 255), 0.70f),
                     modulate_alpha(IM_COL32(0, 0, 0, 255), 0.65f));
        if (ui.intel_radar_labels && radar.zoom >= 1.8) {
          draw->AddText(ImVec2(p.x + 6.0f, p.y - 10.0f), modulate_alpha(IM_COL32(200, 120, 255, 255), 0.85f),
                        j->name.c_str());
        }
      }
    }

    // Friendly ships.
    if (ui.intel_radar_show_friendlies && viewer_faction_id != kInvalidId) {
      const ImU32 col_base = color_faction(viewer_faction_id);
      for (Id shid : sys->ships) {
        const auto* sh = find_ptr(s.ships, shid);
        if (!sh) continue;
        if (sh->faction_id != viewer_faction_id) continue;
        const ImVec2 p = to_screen_radar(sh->position_mkm, center, px_per_mkm, radar.pan_mkm);
        const bool sel = (sh->id == selected_ship);
        const float r = sel ? 6.5f : 5.0f;
        draw_triangle(draw, p, r, modulate_alpha(col_base, sel ? 0.95f : 0.72f),
                      modulate_alpha(IM_COL32(0, 0, 0, 255), 0.65f));
        if (ui.intel_radar_labels && radar.zoom >= 2.0) {
          draw->AddText(ImVec2(p.x + r + 3.0f, p.y - r - 2.0f), modulate_alpha(col_base, 0.85f), sh->name.c_str());
        }
      }
    }

    // Detected hostiles (actual ship positions).
    if (ui.intel_radar_show_hostiles && !detected_hostiles.empty()) {
      for (Id shid : detected_hostiles) {
        const auto* sh = find_ptr(s.ships, shid);
        if (!sh) continue;
        const ImU32 col = color_faction(sh->faction_id);
        const ImVec2 p = to_screen_radar(sh->position_mkm, center, px_per_mkm, radar.pan_mkm);
        draw_diamond(draw, p, 6.0f, modulate_alpha(col, 0.85f), modulate_alpha(IM_COL32(0, 0, 0, 255), 0.75f));
      }
    }

    // Contact blips (last known positions).
    if (ui.intel_radar_show_contacts && !contacts.empty()) {
      const double now_day = (double)s.date.days_since_epoch();
      for (const auto& c : contacts) {
        const int age = (int)std::max<double>(0.0, now_day - (double)c.last_seen_day);
        const float t = clamp01(ui.contact_max_age_days > 0 ? (float)age / (float)ui.contact_max_age_days : 0.0f);
        float a = lerp(0.95f, 0.25f, t);

        // If the target is currently detected, make the blip fully bright.
        if (viewer_faction_id != kInvalidId && sim.is_ship_detected_by_faction(viewer_faction_id, c.ship_id)) {
          a = 1.0f;
        }

        // Scanline "ping".
        if (ui.intel_radar_scanline) {
          const Vec2 rel = c.last_seen_position_mkm + radar.pan_mkm;
          const double ang = std::atan2(rel.y, rel.x);
          double d = std::fabs(std::remainder(ang - scan_ang, kTwoPi));
          if (d < 0.18) {
            const float boost = (float)((0.18 - d) / 0.18) * 0.22f;
            a = clamp01(a + boost);
          }
        }

        const ImU32 col = modulate_alpha(color_faction(c.last_seen_faction_id), a);
        const ImVec2 p = to_screen_radar(c.last_seen_position_mkm, center, px_per_mkm, radar.pan_mkm);
        draw->AddCircleFilled(p, 3.4f, col, 0);
        draw->AddCircle(p, 4.3f, modulate_alpha(IM_COL32(0, 0, 0, 255), a * 0.70f), 0, 1.0f);

        if (c.ship_id == ui.selected_contact_ship_id) {
          const float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 4.0f);
          const float rr = 10.0f + pulse * 5.0f;
          draw->AddCircle(p, rr, modulate_alpha(IM_COL32(255, 255, 255, 255), 0.30f + 0.25f * pulse), 0, 2.0f);
        }

        if (ui.intel_radar_labels && radar.zoom >= 2.4) {
          char buf[128];
          const char* nm = c.last_seen_name.empty() ? "Unknown" : c.last_seen_name.c_str();
          std::snprintf(buf, sizeof(buf), "%s (%dd)", nm, age);
          draw->AddText(ImVec2(p.x + 6.0f, p.y - 10.0f), modulate_alpha(col, 0.95f), buf);
        }
      }
    }

    // Picking: click blips to select.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      const ImVec2 m = mouse;
      // Find closest contact within a small radius.
      const float pick_r2 = 11.0f * 11.0f;
      Id best = kInvalidId;
      float best_d2 = pick_r2;
      for (const auto& c : contacts) {
        const ImVec2 p = to_screen_radar(c.last_seen_position_mkm, center, px_per_mkm, radar.pan_mkm);
        const float dx = m.x - p.x;
        const float dy = m.y - p.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
          best_d2 = d2;
          best = c.ship_id;
        }
      }
      if (best != kInvalidId) {
        ui.selected_contact_ship_id = best;
      }
    }

    // Simple overlay: scale info.
    {
      ImGui::SetCursorScreenPos(ImVec2(origin.x + 10, origin.y + 10));
      ImGui::BeginChild("##radar_legend", ImVec2(0, 0), false, ImGuiWindowFlags_NoBackground);
      ImGui::Text("Radar");
      ImGui::TextDisabled("Range: %.0f mkm", visible_range_mkm);
      ImGui::TextDisabled("Zoom: %.2fx", radar.zoom);
      ImGui::TextDisabled("R: reset | Wheel: zoom | MMB drag: pan");
      ImGui::EndChild();
    }

    ImGui::Dummy(size);
    ImGui::EndChild();
  }

  // -------------------- Contacts (right) --------------------
  ImGui::TableNextColumn();
  {
    ImGui::BeginChild("##intel_contacts", ImVec2(0, 0), false);

    ImGui::SeparatorText("Contacts");

    static char search_buf[128] = "";
    static int scope_idx = 0; // 0 = selected system, 1 = all systems
    static bool only_hostiles = true;
    static bool only_detected = false;

    const char* scope_labels[] = {"Selected system", "All systems"};
    ImGui::Combo("Scope", &scope_idx, scope_labels, IM_ARRAYSIZE(scope_labels));
    ImGui::SameLine();
    ImGui::Checkbox("Hostiles only", &only_hostiles);
    ImGui::SameLine();
    ImGui::Checkbox("Detected now", &only_detected);
    ImGui::InputTextWithHint("Search", "name / faction / design / system", search_buf, IM_ARRAYSIZE(search_buf));

    ImGui::SliderInt("Max age (days)", &ui.contact_max_age_days, 1, 365);
    ui.contact_max_age_days = std::clamp(ui.contact_max_age_days, 1, 3650);

    struct ContactRow {
      Contact c;
      int age_days{0};
      bool detected{false};
      bool hostile{true};
      std::string system_name;
      std::string faction_name;
      double dist_mkm{0.0};
    };

    std::vector<ContactRow> rows;
    if (viewer) {
      const std::int64_t now = s.date.days_since_epoch();
      rows.reserve(viewer->ship_contacts.size());

      for (const auto& kv : viewer->ship_contacts) {
        const Contact& c = kv.second;
        const int age = (int)std::max<std::int64_t>(0, now - c.last_seen_day);
        if (age > ui.contact_max_age_days) continue;
        if (scope_idx == 0 && c.system_id != sys->id) continue;

        const bool detected = sim.is_ship_detected_by_faction(viewer_faction_id, c.ship_id);
        if (only_detected && !detected) continue;

        const bool hostile = sim.are_factions_hostile(viewer_faction_id, c.last_seen_faction_id);
        if (only_hostiles && !hostile) continue;

        const auto* sys2 = find_ptr(s.systems, c.system_id);
        const auto* fac2 = find_ptr(s.factions, c.last_seen_faction_id);

        std::string hay;
        hay.reserve(128);
        hay += c.last_seen_name;
        hay += " ";
        hay += c.last_seen_design_id;
        if (sys2) {
          hay += " ";
          hay += sys2->name;
        }
        if (fac2) {
          hay += " ";
          hay += fac2->name;
        }
        if (!case_insensitive_contains(hay, search_buf)) continue;

        ContactRow r;
        r.c = c;
        r.age_days = age;
        r.detected = detected;
        r.hostile = hostile;
        r.system_name = sys2 ? sys2->name : "?";
        r.faction_name = fac2 ? fac2->name : (c.last_seen_faction_id == kInvalidId ? "?" : "(unknown)");
        r.dist_mkm = c.last_seen_position_mkm.length();
        rows.push_back(std::move(r));
      }
    }

    ImGui::TextDisabled("Showing %d contacts", (int)rows.size());

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                 ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                                 ImGuiTableFlags_ScrollY;

    const float table_h = std::max(200.0f, ImGui::GetContentRegionAvail().y * 0.55f);
    if (ImGui::BeginTable("intel_contacts_table", 7, flags, ImVec2(0, table_h))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
      ImGui::TableSetupColumn("Name", 0, 0.0f, 1);
      ImGui::TableSetupColumn("Faction", 0, 0.0f, 2);
      ImGui::TableSetupColumn("System", 0, 0.0f, 3);
      ImGui::TableSetupColumn("Detected", 0, 0.0f, 4);
      ImGui::TableSetupColumn("Design", 0, 0.0f, 5);
      ImGui::TableSetupColumn("Dist (mkm)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 6);
      ImGui::TableHeadersRow();

      if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
        if (sort->SpecsDirty && sort->SpecsCount > 0) {
          const ImGuiTableColumnSortSpecs* spec = &sort->Specs[0];
          const bool asc = (spec->SortDirection == ImGuiSortDirection_Ascending);
          auto lt = [&](auto a, auto b) { return asc ? (a < b) : (a > b); };
          std::sort(rows.begin(), rows.end(), [&](const ContactRow& a, const ContactRow& b) {
            switch (spec->ColumnUserID) {
              case 0: return lt(a.age_days, b.age_days);
              case 1: return lt(a.c.last_seen_name, b.c.last_seen_name);
              case 2: return lt(a.faction_name, b.faction_name);
              case 3: return lt(a.system_name, b.system_name);
              case 4: return lt((int)a.detected, (int)b.detected);
              case 5: return lt(a.c.last_seen_design_id, b.c.last_seen_design_id);
              case 6: return lt(a.dist_mkm, b.dist_mkm);
              default: return lt(a.age_days, b.age_days);
            }
          });
          sort->SpecsDirty = false;
        }
      }

      for (const auto& r : rows) {
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::Text("%dd", r.age_days);

        ImGui::TableNextColumn();
        const bool selected = (r.c.ship_id == ui.selected_contact_ship_id);
        const char* disp_name = r.c.last_seen_name.empty() ? "Unknown" : r.c.last_seen_name.c_str();
        std::string sel_label = std::string(disp_name) + "##contact_" + std::to_string(r.c.ship_id);
        if (ImGui::Selectable(sel_label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
          ui.selected_contact_ship_id = r.c.ship_id;
          // Jump context when selecting from an "all systems" list.
          if (scope_idx == 1) {
            s.selected_system = r.c.system_id;
          }
        }

        ImGui::TableNextColumn();
        ImGui::Text("%s", r.faction_name.c_str());

        ImGui::TableNextColumn();
        ImGui::Text("%s", r.system_name.c_str());

        ImGui::TableNextColumn();
        ImGui::Text("%s", r.detected ? "Yes" : "No");

        ImGui::TableNextColumn();
        ImGui::Text("%s", r.c.last_seen_design_id.empty() ? "(unknown)" : r.c.last_seen_design_id.c_str());

        ImGui::TableNextColumn();
        ImGui::Text("%.0f", r.dist_mkm);
      }

      ImGui::EndTable();
    }

    // --- Selected contact details ---
    if (viewer && ui.selected_contact_ship_id != kInvalidId) {
      auto it = viewer->ship_contacts.find(ui.selected_contact_ship_id);
      if (it == viewer->ship_contacts.end()) {
        ImGui::TextDisabled("Selected contact is no longer present.");
      } else {
        const Contact& c = it->second;
        const auto* sys2 = find_ptr(s.systems, c.system_id);
        const auto* fac2 = find_ptr(s.factions, c.last_seen_faction_id);
        const std::int64_t now = s.date.days_since_epoch();
        const int age = (int)std::max<std::int64_t>(0, now - c.last_seen_day);
        const auto pred = predict_contact_position(c, static_cast<int>(now), sim.cfg().contact_prediction_max_days);
        const Vec2 pred_pos = pred.predicted_position_mkm;

        ImGui::SeparatorText("Selected");
        ImGui::Text("%s", c.last_seen_name.empty() ? "Unknown contact" : c.last_seen_name.c_str());
        ImGui::TextDisabled("Faction: %s", fac2 ? fac2->name.c_str() : "?");
        ImGui::TextDisabled("System: %s", sys2 ? sys2->name.c_str() : "?");
        ImGui::TextDisabled("Age: %d days", age);
        ImGui::TextDisabled("Design: %s", c.last_seen_design_id.empty() ? "(unknown)" : c.last_seen_design_id.c_str());

        if (const auto* d = sim.find_design(c.last_seen_design_id)) {
          double sig0 = std::isfinite(d->signature_multiplier) ? d->signature_multiplier : 1.0;
          sig0 = std::clamp(sig0, 0.0, sim_sensors::max_signature_multiplier_for_detection(sim));
          const double ecm = std::isfinite(d->ecm_strength) ? std::max(0.0, d->ecm_strength) : 0.0;
          const double eccm = std::isfinite(d->eccm_strength) ? std::max(0.0, d->eccm_strength) : 0.0;

          ImGui::TextDisabled("Sig (design): %.0f%%", sig0 * 100.0);
          if (ecm > 0.0 || eccm > 0.0) {
            ImGui::TextDisabled("EW (design): ECM %.1f  ECCM %.1f", ecm, eccm);
          } else {
            ImGui::TextDisabled("EW (design): (none)");
          }

          if (viewer_faction_id != kInvalidId) {
            double best = 0.0;
            for (const auto& src : sim_sensors::gather_sensor_sources(sim, viewer_faction_id, c.system_id)) {
              const double src_eccm = std::max(0.0, src.eccm_strength);
              double ew_mult = (1.0 + src_eccm) / (1.0 + ecm);
              if (!std::isfinite(ew_mult)) ew_mult = 1.0;
              ew_mult = std::clamp(ew_mult, 0.1, 10.0);

              const double r = std::max(0.0, src.range_mkm) * sig0 * ew_mult;
              if (std::isfinite(r)) best = std::max(best, r);
            }
            if (best > 0.0) {
              ImGui::TextDisabled("Est. detect radius vs viewer: up to %.0f mkm", best);
              ImGui::TextDisabled("(Assumes target EMCON = Normal)");
            }
          }
        }
        ImGui::TextDisabled("Last pos: (%.1f, %.1f) mkm", c.last_seen_position_mkm.x, c.last_seen_position_mkm.y);
        ImGui::TextDisabled("Pred pos: (%.1f, %.1f) mkm (%dd extrap)", pred_pos.x, pred_pos.y, pred.extrapolated_days);
        if (pred.has_velocity) {
          ImGui::TextDisabled("Est vel: (%.2f, %.2f) mkm/day", pred.velocity_mkm_per_day.x, pred.velocity_mkm_per_day.y);
        }
        if (sim.cfg().enable_contact_uncertainty) {
          const double unc_now = sim.contact_uncertainty_radius_mkm(c, static_cast<int>(now));
          const double unc_last = c.last_seen_position_uncertainty_mkm;
          if (unc_now > 1e-3 || unc_last > 1e-3) {
            ImGui::TextDisabled("Uncertainty: ±%.0f mkm (last detect: ±%.0f mkm)", unc_now, unc_last);
          }
        }

        if (ImGui::Button("View system map")) {
          s.selected_system = c.system_id;
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::System;
        }
        ImGui::SameLine();
        if (ImGui::Button("Center system map")) {
          s.selected_system = c.system_id;
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::System;
          ui.request_system_map_center = true;
          ui.request_system_map_center_system_id = c.system_id;
          ui.request_system_map_center_x_mkm = pred_pos.x;
          ui.request_system_map_center_y_mkm = pred_pos.y;
          ui.request_system_map_center_zoom = 0.0; // leave as-is
        }
        ImGui::SameLine();
        if (ImGui::Button("Center radar")) {
          radar.pan_mkm = Vec2{-pred_pos.x, -pred_pos.y};
        }

        // Tactical actions (require a selected friendly ship).
        if (selected_ship != kInvalidId) {
          const auto* sh = find_ptr(s.ships, selected_ship);
          if (sh && sh->faction_id == viewer_faction_id) {
            const bool same_sys = (sh->system_id == c.system_id);
            if (!same_sys) {
              ImGui::TextDisabled("Select a ship in the same system to issue intercept/attack.");
            } else {
              if (ImGui::Button("Intercept (move)")) {
                sim.issue_move_to_point(selected_ship, pred_pos);
                ui.show_map_window = true;
                ui.request_map_tab = MapTab::System;
              }
              ImGui::SameLine();
              if (ImGui::Button("Attack")) {
                sim.issue_attack_ship(selected_ship, c.ship_id, ui.fog_of_war);
                ui.show_map_window = true;
                ui.request_map_tab = MapTab::System;
              }
              ImGui::SameLine();
              ImGui::TextDisabled("(lead pursuit + predicted track when possible)");
            }
          }
        }
      }
    }

    ImGui::EndChild();
  }

  ImGui::EndTable();

  ImGui::End();
}

} // namespace nebula4x::ui
