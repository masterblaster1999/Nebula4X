#include "ui/system_map.h"

#include "ui/map_render.h"
#include "ui/map_label_placer.h"
#include "ui/minimap.h"
#include "ui/raymarch_nebula.h"
#include "ui/raytrace_sensor_heatmap.h"
#include "ui/ruler.h"

#include "ui/proc_render_engine.h"
#include "ui/proc_body_sprite_engine.h"
#include "ui/proc_icon_sprite_engine.h"
#include "ui/proc_jump_phenomena_sprite_engine.h"
#include "ui/proc_anomaly_phenomena_sprite_engine.h"
#include "ui/proc_trail_engine.h"
#include "ui/proc_flow_field_engine.h"
#include "ui/proc_gravity_contour_engine.h"
#include "ui/proc_particle_field_engine.h"

#include "ui/procgen_graphics.h"

#include "core/simulation_sensors.h"

#include "nebula4x/core/contact_prediction.h"
#include "nebula4x/core/fleet_formation.h"
#include "nebula4x/core/enum_strings.h"
#include "nebula4x/core/procgen_jump_phenomena.h"
#include "nebula4x/core/procgen_obscure.h"
#include "nebula4x/core/power.h"
#include "nebula4x/util/time.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <unordered_map>
#include <variant>
#include <vector>

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
  const double denom = std::max(1e-12, scale_px_per_mkm * zoom);
  const double x = (screen_px.x - center_px.x) / denom - pan_mkm.x;
  const double y = (screen_px.y - center_px.y) / denom - pan_mkm.y;
  return Vec2{x, y};
}


struct HeatmapSource {
  Vec2 pos_mkm{0.0, 0.0};
  double range_mkm{0.0};
  float weight{1.0f}; // 0..1 multiplier (used for threat weighting)
};

void draw_heatmap(ImDrawList* draw,
                  const ImVec2& origin,
                  const ImVec2& avail,
                  const ImVec2& center_px,
                  double scale_px_per_mkm,
                  double zoom,
                  const Vec2& pan_mkm,
                  int cells_x,
                  int cells_y,
                  const std::vector<HeatmapSource>& sources,
                  ImU32 base_col,
                  float opacity) {
  if (!draw) return;
  if (opacity <= 0.0f) return;
  if (cells_x <= 0 || cells_y <= 0) return;
  if (sources.empty()) return;

  const float cw = avail.x / static_cast<float>(cells_x);
  const float ch = avail.y / static_cast<float>(cells_y);

  const ImVec2 clip_p1(origin.x + avail.x, origin.y + avail.y);
  ImGui::PushClipRect(origin, clip_p1, true);

  for (int y = 0; y < cells_y; ++y) {
    const float y0 = origin.y + static_cast<float>(y) * ch;
    const float y1 = y0 + ch + 0.5f; // slight overlap to avoid gaps
    const float cy = y0 + ch * 0.5f;

    for (int x = 0; x < cells_x; ++x) {
      const float x0 = origin.x + static_cast<float>(x) * cw;
      const float x1 = x0 + cw + 0.5f;
      const float cx = x0 + cw * 0.5f;

      const Vec2 w = to_world(ImVec2(cx, cy), center_px, scale_px_per_mkm, zoom, pan_mkm);

      float best = 0.0f;
      for (const auto& src : sources) {
        if (src.range_mkm <= 1e-6) continue;

        const double dx = w.x - src.pos_mkm.x;
        const double dy = w.y - src.pos_mkm.y;
        const double r = src.range_mkm;
        const double r2 = r * r;
        const double d2 = dx * dx + dy * dy;
        if (d2 >= r2) continue;

        const double d = std::sqrt(std::max(0.0, d2));
        float v = static_cast<float>(1.0 - (d / r));
        v *= std::clamp(src.weight, 0.0f, 1.0f);
        if (v > best) best = v;
      }

      if (best <= 0.0f) continue;

      // Make the center a bit more readable without needing high resolution.
      best = std::pow(best, 0.75f);

      const float a = std::clamp(opacity * best, 0.0f, 1.0f);
      if (a <= 0.001f) continue;

      draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), modulate_alpha(base_col, a));
    }
  }

  ImGui::PopClipRect();
}

void draw_nebula_microfield_overlay(ImDrawList* draw,
                                   const ImVec2& origin,
                                   const ImVec2& avail,
                                   const ImVec2& center_px,
                                   double scale_px_per_mkm,
                                   double zoom,
                                   const Vec2& pan_mkm,
                                   const Simulation& sim,
                                   Id system_id,
                                   int cells_x,
                                   int cells_y,
                                   ImU32 base_col,
                                   float opacity) {
  if (!draw) return;
  if (opacity <= 0.0f) return;
  if (cells_x <= 0 || cells_y <= 0) return;

  const auto& s = sim.state();
  const auto* sys = find_ptr(s.systems, system_id);
  if (!sys) return;
  if (!(sys->nebula_density > 1e-6) && !sim.cfg().enable_nebula_microfields) return;

  const float cw = avail.x / static_cast<float>(cells_x);
  const float ch = avail.y / static_cast<float>(cells_y);

  const ImVec2 clip_p1(origin.x + avail.x, origin.y + avail.y);
  ImGui::PushClipRect(origin, clip_p1, true);

  for (int y = 0; y < cells_y; ++y) {
    const float y0 = origin.y + static_cast<float>(y) * ch;
    const float y1 = y0 + ch + 0.5f;
    const float cy = y0 + ch * 0.5f;

    for (int x = 0; x < cells_x; ++x) {
      const float x0 = origin.x + static_cast<float>(x) * cw;
      const float x1 = x0 + cw + 0.5f;
      const float cx = x0 + cw * 0.5f;

      const Vec2 w = to_world(ImVec2(cx, cy), center_px, scale_px_per_mkm, zoom, pan_mkm);
      const double d = sim.system_nebula_density_at(system_id, w);
      if (!(d > 1e-6)) continue;

      // Subtle contrast curve.
      const float a = opacity * static_cast<float>(std::pow(std::clamp(d, 0.0, 1.0), 1.35));
      if (a <= 0.003f) continue;

      draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), modulate_alpha(base_col, a));
    }
  }

  ImGui::PopClipRect();
}

void draw_nebula_storm_cell_overlay(ImDrawList* draw,
                                   const ImVec2& origin,
                                   const ImVec2& avail,
                                   const ImVec2& center_px,
                                   double scale_px_per_mkm,
                                   double zoom,
                                   const Vec2& pan_mkm,
                                   const Simulation& sim,
                                   Id system_id,
                                   int cells_x,
                                   int cells_y,
                                   ImU32 base_col,
                                   float opacity) {
  if (!draw) return;
  if (opacity <= 0.0f) return;
  if (cells_x <= 0 || cells_y <= 0) return;
  if (!sim.cfg().enable_nebula_storms) return;

  // Storms are temporal and optional; if no storm is active this overlay is a no-op.
  if (sim.system_storm_intensity(system_id) <= 1e-9) return;

  const float cw = avail.x / static_cast<float>(cells_x);
  const float ch = avail.y / static_cast<float>(cells_y);

  const ImVec2 clip_p1(origin.x + avail.x, origin.y + avail.y);
  ImGui::PushClipRect(origin, clip_p1, true);

  for (int y = 0; y < cells_y; ++y) {
    const float y0 = origin.y + static_cast<float>(y) * ch;
    const float y1 = y0 + ch + 0.5f;
    const float cy = y0 + ch * 0.5f;

    for (int x = 0; x < cells_x; ++x) {
      const float x0 = origin.x + static_cast<float>(x) * cw;
      const float x1 = x0 + cw + 0.5f;
      const float cx = x0 + cw * 0.5f;

      const Vec2 w = to_world(ImVec2(cx, cy), center_px, scale_px_per_mkm, zoom, pan_mkm);
      const double st = sim.system_storm_intensity_at(system_id, w);
      if (!(st > 1e-6)) continue;

      // Mild contrast curve so fronts read at modest resolutions.
      const float a = opacity * static_cast<float>(std::pow(std::clamp(st, 0.0, 1.0), 1.25));
      if (a <= 0.003f) continue;

      draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), modulate_alpha(base_col, a));
    }
  }

  ImGui::PopClipRect();
}



double sim_time_days(const GameState& s) {
  const double day = static_cast<double>(s.date.days_since_epoch());
  const double frac = static_cast<double>(std::clamp(s.hour_of_day, 0, 23)) / 24.0;
  return day + frac;
}

// Predict body positions at an arbitrary time t (days since epoch), without mutating sim state.
// Mirrors Simulation::recompute_body_positions (core).
std::unordered_map<Id, Vec2> predict_body_positions_at_time(const GameState& s, const StarSystem& sys, double t_days) {
  std::unordered_map<Id, Vec2> cache;
  cache.reserve(sys.bodies.size() * 2 + 8);

  std::unordered_set<Id> visiting;
  visiting.reserve(sys.bodies.size() * 2 + 8);

  const auto compute_pos = [&](Id id, const auto& self) -> Vec2 {
    if (id == kInvalidId) return {0.0, 0.0};

    if (auto it = cache.find(id); it != cache.end()) return it->second;

    const Body* b = find_ptr(s.bodies, id);
    if (!b) return {0.0, 0.0};

    // Break accidental cycles gracefully (treat as orbiting system origin).
    if (!visiting.insert(id).second) {
      cache[id] = {0.0, 0.0};
      return {0.0, 0.0};
    }

    // Orbit center: either system origin or a parent body's position at time t.
    Vec2 center{0.0, 0.0};
    if (b->parent_body_id != kInvalidId && b->parent_body_id != id) {
      const Body* parent = find_ptr(s.bodies, b->parent_body_id);
      if (parent && parent->system_id == b->system_id) {
        center = self(b->parent_body_id, self);
      }
    }

    Vec2 pos = center;
    if (b->orbit_radius_mkm > 1e-9) {
      const double a = std::max(0.0, b->orbit_radius_mkm);
      const double e = std::clamp(b->orbit_eccentricity, 0.0, 0.999999);
      const double period = std::max(1.0, b->orbit_period_days);

      // Mean anomaly advances linearly with time.
      double M = b->orbit_phase_radians + kTwoPi * (t_days / period);
      // Wrap for numerical stability.
      M = std::fmod(M, kTwoPi);
      if (M < 0.0) M += kTwoPi;

      // Solve Kepler's equation: M = E - e sin(E) for eccentric anomaly E.
      double E = (e < 0.8) ? M : (kTwoPi * 0.5); // start at pi for high-e orbits
      for (int it = 0; it < 12; ++it) {
        const double sE = std::sin(E);
        const double cE = std::cos(E);
        const double f = (E - e * sE) - M;
        const double fp = 1.0 - e * cE;
        if (std::fabs(fp) < 1e-12) break;
        E -= (f / fp);
        if (std::fabs(f) < 1e-10) break;
      }

      const double sE = std::sin(E);
      const double cE = std::cos(E);
      const double bsemi = a * std::sqrt(std::max(0.0, 1.0 - e * e));
      const double x = a * (cE - e);
      const double y = bsemi * sE;

      const double w = b->orbit_arg_periapsis_radians;
      const double cw = std::cos(w);
      const double sw = std::sin(w);
      const double rx = x * cw - y * sw;
      const double ry = x * sw + y * cw;

      pos = center + Vec2{rx, ry};
    }

    cache[id] = pos;
    visiting.erase(id);
    return pos;
  };

  // Ensure we compute at least all bodies in this system.
  for (Id bid : sys.bodies) {
    if (bid == kInvalidId) continue;
    const Body* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    if (b->system_id != sys.id) continue;
    (void)compute_pos(bid, compute_pos);
  }

  return cache;
}


// When measuring distances with the map ruler we want the endpoints to "snap" to
// nearby entities so players can easily measure ship-to-planet, jump-to-planet, etc.
// This helper returns the snapped world position (in mkm) when a visible entity is
// within a small pixel radius of the mouse, otherwise it returns the raw cursor world.
Vec2 snap_ruler_point(const Simulation& sim,
                      const UIState& ui,
                      const StarSystem& sys,
                      Id viewer_faction_id,
                      const std::vector<Id>& detected_hostiles,
                      const ImVec2& mouse_px,
                      const ImVec2& center_px,
                      double scale_px_per_mkm,
                      double zoom,
                      const Vec2& pan_mkm) {
  constexpr float kSnapRadiusPx = 14.0f;
  const float snap_d2 = kSnapRadiusPx * kSnapRadiusPx;

  float best_d2 = snap_d2;
  Vec2 best = to_world(mouse_px, center_px, scale_px_per_mkm, zoom, pan_mkm);
  bool found = false;

  auto consider = [&](const Vec2& w) {
    const ImVec2 p = to_screen(w, center_px, scale_px_per_mkm, zoom, pan_mkm);
    const float dx = mouse_px.x - p.x;
    const float dy = mouse_px.y - p.y;
    const float d2 = dx * dx + dy * dy;
    if (d2 <= best_d2) {
      best_d2 = d2;
      best = w;
      found = true;
    }
  };

  // Jump points.
  for (Id jid : sys.jump_points) {
    const auto* jp = find_ptr(sim.state().jump_points, jid);
    if (!jp) continue;
    consider(jp->position_mkm);
  }

  // Bodies.
  for (Id bid : sys.bodies) {
    const auto* b = find_ptr(sim.state().bodies, bid);
    if (!b) continue;
    const bool is_minor = (b->type == BodyType::Asteroid || b->type == BodyType::Comet);
    if (is_minor && !ui.show_minor_bodies) continue;
    consider(b->position_mkm);
  }

  // Ships (respect fog-of-war: snap only to friendly + detected hostiles).
  for (Id sid : sys.ships) {
    const auto* sh = find_ptr(sim.state().ships, sid);
    if (!sh) continue;
    if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
      if (sh->faction_id != viewer_faction_id) {
        if (std::find(detected_hostiles.begin(), detected_hostiles.end(), sid) == detected_hostiles.end()) {
          continue;
        }
      }
    }
    consider(sh->position_mkm);
  }

  return found ? best : to_world(mouse_px, center_px, scale_px_per_mkm, zoom, pan_mkm);
}

} // namespace

void draw_system_map(Simulation& sim,
                     UIState& ui,
                     Id& selected_ship,
                     Id& selected_colony,
                     Id& selected_body,
                     double& zoom,
                     Vec2& pan,
                     ProcRenderEngine* proc_engine,
                     ProcParticleFieldEngine* particle_engine,
                     ProcBodySpriteEngine* body_sprites,
                     ProcIconSpriteEngine* icon_sprites,
                     ProcJumpPhenomenaSpriteEngine* jump_fx,
                     ProcAnomalyPhenomenaSpriteEngine* anomaly_fx,
                     ProcTrailEngine* trail_engine,
                     ProcFlowFieldEngine* flow_engine,
                     ProcGravityContourEngine* gravity_engine) {
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


  // --- Heatmap sources (computed once per frame) ---
  ui.system_map_heatmap_opacity = std::clamp(ui.system_map_heatmap_opacity, 0.0f, 1.0f);
  ui.system_map_heatmap_resolution = std::clamp(ui.system_map_heatmap_resolution, 16, 200);

  // Sensor sources cache (for coverage rings + heatmap).
  std::vector<sim_sensors::SensorSource> sensor_sources;
  double sensor_coverage_sig = 1.0;
  double sensor_coverage_sig_max = 1.0;
  if ((ui.show_faction_sensor_coverage || ui.system_map_sensor_heatmap) && viewer_faction_id != kInvalidId) {
    sensor_sources = sim_sensors::gather_sensor_sources(sim, viewer_faction_id, sys->id);

    // Sort by range so large fields are processed first (nicer alpha blending & early-outs).
    std::sort(sensor_sources.begin(), sensor_sources.end(),
              [](const sim_sensors::SensorSource& a, const sim_sensors::SensorSource& b) {
                return a.range_mkm > b.range_mkm;
              });

    sensor_coverage_sig_max = std::max(0.05, sim_sensors::max_signature_multiplier_for_detection(sim));
    sensor_coverage_sig = std::clamp<double>(static_cast<double>(ui.faction_sensor_coverage_signature), 0.05,
                                             sensor_coverage_sig_max);

    // Keep UI state clamped so it persists cleanly.
    ui.faction_sensor_coverage_signature = static_cast<float>(sensor_coverage_sig);
  }

  int sensor_sources_drawn = 0;

  // Stats for the experimental LOS ray-traced sensor heatmap.
  SensorRaytraceHeatmapStats sensor_rt_stats;
  bool sensor_rt_stats_valid = false;

  // Hostile threat sources for the tactical threat heatmap.
  std::vector<HeatmapSource> threat_sources;
  if (ui.system_map_threat_heatmap && viewer_faction_id != kInvalidId) {
    std::vector<Id> hostile_ids;
    hostile_ids.reserve(sys->ships.size());

    if (ui.fog_of_war) {
      hostile_ids = detected_hostiles;
    } else {
      for (Id sid : sys->ships) {
        const Ship* sh = find_ptr(s.ships, sid);
        if (!sh) continue;
        if (sh->faction_id == viewer_faction_id) continue;
        const DiplomacyStatus ds = sim.diplomatic_status(viewer_faction_id, sh->faction_id);
        if (ds != DiplomacyStatus::Hostile) continue;
        hostile_ids.push_back(sid);
      }
    }

    std::vector<HeatmapSource> raw;
    raw.reserve(hostile_ids.size());

    double max_w = 0.0;
    for (Id sid : hostile_ids) {
      const Ship* sh = find_ptr(s.ships, sid);
      if (!sh) continue;
      const ShipDesign* d = sim.find_design(sh->design_id);
      if (!d) continue;

      const double r = std::max(d->weapon_range_mkm, d->missile_range_mkm);
      if (r <= 1e-6) continue;

      double w = std::max(0.0, d->weapon_damage + d->missile_damage);
      if (w <= 1e-6) w = 1.0;

      HeatmapSource src;
      src.pos_mkm = sh->position_mkm;
      src.range_mkm = r;
      src.weight = static_cast<float>(w);
      raw.push_back(src);

      max_w = std::max(max_w, w);
    }

    if (!raw.empty()) {
      max_w = std::max(1.0, max_w);
      threat_sources = raw;
      for (auto& src : threat_sources) {
        const double w = std::max(0.0, static_cast<double>(src.weight));
        src.weight = static_cast<float>(std::sqrt(w / max_w));
      }
    }
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
  const double scale = std::max(1e-9, fit / max_r);

  // Input handling.
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool mouse_in_rect =
      (mouse.x >= origin.x && mouse.x <= origin.x + avail.x && mouse.y >= origin.y && mouse.y <= origin.y + avail.y);

  // Map viewport rectangle (content area).
  const ImVec2 view_p0 = origin;
  const ImVec2 view_p1(origin.x + avail.x, origin.y + avail.y);

  // Legend overlay rectangle (top-left). The legend is drawn later in the frame using
  // SetCursorScreenPos/BeginChild, so we must treat this area as UI now or
  // clicks/scrolls will 'fall through' to the map interactions.
  const float legend_margin = 10.0f;
  const ImVec2 legend_desired(320.0f, 480.0f);
  const float legend_max_w = std::max(0.0f, avail.x - legend_margin * 2.0f);
  const float legend_max_h = std::max(0.0f, avail.y - legend_margin * 2.0f);
  ImVec2 legend_size(std::min(legend_desired.x, legend_max_w), std::min(legend_desired.y, legend_max_h));
  // Avoid (0,0) which in BeginChild means "use remaining region".
  legend_size.x = std::max(1.0f, legend_size.x);
  legend_size.y = std::max(1.0f, legend_size.y);
  const ImVec2 legend_p0(origin.x + legend_margin, origin.y + legend_margin);
  const ImVec2 legend_p1(legend_p0.x + legend_size.x, legend_p0.y + legend_size.y);

  // Clamp hit-test to visible viewport. This avoids blocking map interactions
  // in cases where the window is smaller than the overlay and the child window
  // is clipped by ImGui.
  const ImVec2 legend_hit_p0(std::max(legend_p0.x, view_p0.x), std::max(legend_p0.y, view_p0.y));
  const ImVec2 legend_hit_p1(std::min(legend_p1.x, view_p1.x), std::min(legend_p1.y, view_p1.y));
  const bool legend_hit_valid = legend_hit_p1.x > legend_hit_p0.x && legend_hit_p1.y > legend_hit_p0.y;
  const bool over_legend = legend_hit_valid && mouse_in_rect && point_in_rect(mouse, legend_hit_p0, legend_hit_p1);

  // Minimap rectangle (bottom-right overlay).
  const float mm_margin = 10.0f;
  float mm_w = std::clamp(avail.x * 0.28f, 150.0f, 300.0f);
  float mm_h = std::clamp(avail.y * 0.23f, 105.0f, 240.0f);
  const float mm_max_w = std::max(0.0f, avail.x - mm_margin * 2.0f);
  const float mm_max_h = std::max(0.0f, avail.y - mm_margin * 2.0f);
  mm_w = std::min(mm_w, mm_max_w);
  mm_h = std::min(mm_h, mm_max_h);
  const bool minimap_has_room = (mm_w >= 10.0f && mm_h >= 10.0f);
  const ImVec2 mm_p1(view_p1.x - mm_margin, view_p1.y - mm_margin);
  const ImVec2 mm_p0(mm_p1.x - mm_w, mm_p1.y - mm_h);
  bool minimap_enabled = ui.system_map_show_minimap && minimap_has_room;

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
    if (ImGui::IsKeyPressed(ImGuiKey_M)) {
      ui.system_map_show_minimap = !ui.system_map_show_minimap;
      minimap_enabled = ui.system_map_show_minimap && minimap_has_room;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_T)) {
      ui.system_map_time_preview = !ui.system_map_time_preview;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_H)) {
      if (ImGui::GetIO().KeyShift) {
        ui.system_map_sensor_heatmap = !ui.system_map_sensor_heatmap;
      } else {
        ui.system_map_threat_heatmap = !ui.system_map_threat_heatmap;
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_N)) {
      ui.system_map_nebula_microfield_overlay = !ui.system_map_nebula_microfield_overlay;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_G)) {
      ui.system_map_gravity_contours_overlay = !ui.system_map_gravity_contours_overlay;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_J)) {
      ui.system_map_jump_phenomena = !ui.system_map_jump_phenomena;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_A)) {
      ui.system_map_anomaly_phenomena = !ui.system_map_anomaly_phenomena;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_S)) {
      ui.system_map_storm_cell_overlay = !ui.system_map_storm_cell_overlay;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_W)) {
      ui.system_map_flow_field_overlay = !ui.system_map_flow_field_overlay;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_B)) {
      ui.system_map_particle_field = !ui.system_map_particle_field;
    }

    const float tstep = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
      ui.system_map_time_preview_days =
          std::clamp(ui.system_map_time_preview_days - tstep, -365.0f, 365.0f);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
      ui.system_map_time_preview_days =
          std::clamp(ui.system_map_time_preview_days + tstep, -365.0f, 365.0f);
    }
  }

  const ImVec2 mm_hit_p0(std::max(mm_p0.x, view_p0.x), std::max(mm_p0.y, view_p0.y));
  const ImVec2 mm_hit_p1(std::min(mm_p1.x, view_p1.x), std::min(mm_p1.y, view_p1.y));
  const bool mm_hit_valid = minimap_enabled && mm_hit_p1.x > mm_hit_p0.x && mm_hit_p1.y > mm_hit_p0.y;
  const bool over_minimap = mm_hit_valid && mouse_in_rect && point_in_rect(mouse, mm_hit_p0, mm_hit_p1);

  MinimapTransform mm;
  if (minimap_enabled) {
    // System map uses absolute in-system coords centered at (0,0).
    const Vec2 wmin{-max_r, -max_r};
    const Vec2 wmax{+max_r, +max_r};
    mm = make_minimap_transform(mm_p0, mm_p1, wmin, wmax);
  }

  // Zoom via wheel (zoom to cursor).
  if (hovered && mouse_in_rect && !over_minimap && !over_legend) {
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
      const double denom = std::max(1e-12, scale * zoom);
      pan.x += d.x / denom;
      pan.y += d.y / denom;
    }
  }

  // Minimap pan/teleport: click+drag to set the view center.
  if (hovered && mouse_in_rect && over_minimap && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const Vec2 target = minimap_px_to_world(mm, mouse);
    pan = Vec2{-target.x, -target.y};
    ui.system_map_follow_selected = false;
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


  // --- Time preview (planning overlay) ---
  // A non-simulative UI-only overlay that shows where bodies will be along their
  // orbits, and where ships will drift based on their current velocity, at a
  // configurable offset from the current in-game time.
  const bool time_preview_enabled = ui.system_map_time_preview;
  const double time_preview_days =
      std::clamp<double>(static_cast<double>(ui.system_map_time_preview_days), -365.0, 365.0);
  ui.system_map_time_preview_days = static_cast<float>(time_preview_days);

  const bool time_preview_active = time_preview_enabled && (std::fabs(time_preview_days) > 1e-6);
  const double t_now_days = sim_time_days(s);

  std::unordered_map<Id, Vec2> body_pos_future;
  std::vector<std::unordered_map<Id, Vec2>> body_pos_trail;
  int body_trail_steps = 0;

  if (time_preview_active) {
    body_pos_future = predict_body_positions_at_time(s, *sys, t_now_days + time_preview_days);

    if (ui.system_map_time_preview_trails && std::fabs(time_preview_days) > 0.25) {
      const double abs_d = std::fabs(time_preview_days);
      body_trail_steps = 16;
      if (abs_d > 30.0) body_trail_steps = 32;
      if (abs_d > 180.0) body_trail_steps = 48;
      if (abs_d > 365.0) body_trail_steps = 64; // (clamped above, but harmless)

      body_pos_trail.reserve(static_cast<std::size_t>(body_trail_steps) + 1);
      for (int i = 0; i <= body_trail_steps; ++i) {
        const double a = (body_trail_steps > 0) ? (static_cast<double>(i) / static_cast<double>(body_trail_steps))
                                                : 1.0;
        const double t = t_now_days + time_preview_days * a;
        body_pos_trail.emplace_back(predict_body_positions_at_time(s, *sys, t));
      }
    }
  }

  // --- Map ruler (hold D) ---
  // This is intentionally a temporary "mode" activated by holding a key so it doesn't
  // interfere with the normal left-click order workflow.
  static RulerState ruler;
  static std::uint64_t ruler_state_gen = 0;
  static Id ruler_system_id = kInvalidId;
  if (ruler_state_gen != sim.state_generation() || ruler_system_id != sys->id) {
    clear_ruler(ruler);
    ruler_state_gen = sim.state_generation();
    ruler_system_id = sys->id;
  }

  bool ruler_consumed_left = false;
  bool ruler_consumed_right = false;
  {
    const ImGuiIO& io = ImGui::GetIO();
    const bool ruler_key = hovered && mouse_in_rect && !over_minimap && !over_legend && !io.WantTextInput &&
                           ImGui::IsKeyDown(ImGuiKey_D) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;

    if (ruler_key) {
      // D + Right click clears the ruler.
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered()) {
        clear_ruler(ruler);
        ruler_consumed_right = true;
      }

      // D + Left click starts a new measurement, and D + drag updates it.
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
        const Vec2 p = snap_ruler_point(sim, ui, *sys, viewer_faction_id, detected_hostiles, mouse, center, scale,
                                        zoom, pan);
        begin_ruler(ruler, p);
        ruler_consumed_left = true;
      }

      if (ruler.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Vec2 p = snap_ruler_point(sim, ui, *sys, viewer_faction_id, detected_hostiles, mouse, center, scale,
                                        zoom, pan);
        update_ruler(ruler, p);
      }

      if (ruler.dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const Vec2 p = snap_ruler_point(sim, ui, *sys, viewer_faction_id, detected_hostiles, mouse, center, scale,
                                        zoom, pan);
        end_ruler(ruler, p);
      }
    }
  }

  auto* draw = ImGui::GetWindowDrawList();
  const ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(ui.system_map_bg[0], ui.system_map_bg[1], ui.system_map_bg[2],
                                                         ui.system_map_bg[3]));
  draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), bg);
  draw->AddRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(60, 60, 60, 255));

  // Map chrome.
  {
    const float pan_px_x = static_cast<float>(-pan.x * scale * zoom);
    const float pan_px_y = static_cast<float>(-pan.y * scale * zoom);
    const std::uint32_t chrome_seed = hash_u32(static_cast<std::uint32_t>(sys->id) ^ 0xA3C59AC3u);

    // Experimental: ray-marched SDF nebula background.
    if (ui.map_raymarch_nebula && ui.map_raymarch_nebula_alpha > 0.0f) {
      RaymarchNebulaStyle rs;
      rs.enabled = true;
      rs.alpha = ui.map_raymarch_nebula_alpha;
      rs.parallax = ui.map_raymarch_nebula_parallax;
      rs.max_depth = ui.map_raymarch_nebula_max_depth;
      rs.error_threshold = ui.map_raymarch_nebula_error_threshold;
      rs.spp = ui.map_raymarch_nebula_spp;
      rs.max_steps = ui.map_raymarch_nebula_max_steps;
      rs.animate = ui.map_raymarch_nebula_animate;
      rs.time_scale = ui.map_raymarch_nebula_time_scale;
      rs.debug_overlay = ui.map_raymarch_nebula_debug;

      RaymarchNebulaStats stats;
      draw_raymarched_nebula(draw, origin, avail, bg, pan_px_x, pan_px_y, chrome_seed ^ 0xD1CEB00Bu, rs,
                             rs.debug_overlay ? &stats : nullptr);
    }

    // Starfield / procedural background.
    //
    // The legacy path draws stars directly into the ImDrawList every frame.
    // The procedural engine path rasterizes tiles on-demand, uploads textures,
    // then just draws textured quads while panning.
    if (ui.map_proc_render_engine && proc_engine && proc_engine->ready()) {
      if (ui.map_proc_render_clear_cache_requested) {
        proc_engine->clear();
        ui.map_proc_render_clear_cache_requested = false;
      }

      ProcRenderConfig pcfg;
      pcfg.tile_px = ui.map_proc_render_tile_px;
      pcfg.max_cached_tiles = ui.map_proc_render_cache_tiles;
      pcfg.star_density = ui.system_map_starfield ? ui.map_starfield_density : 0.0f;
      pcfg.parallax = ui.map_starfield_parallax;
      pcfg.nebula_enable = ui.map_proc_render_nebula_enable;
      pcfg.nebula_strength = ui.map_proc_render_nebula_strength;
      pcfg.nebula_scale = ui.map_proc_render_nebula_scale;
      pcfg.nebula_warp = ui.map_proc_render_nebula_warp;
      pcfg.debug_show_tile_bounds = ui.map_proc_render_debug_tiles;

      // Subtle tinting so the procedural textures respect user theme colors.
      const ImVec4 bg4 = ImGui::ColorConvertU32ToFloat4(bg);
      const ImVec4 tint4(0.75f + 0.25f * bg4.x, 0.75f + 0.25f * bg4.y, 0.75f + 0.25f * bg4.z, 1.0f);
      const ImU32 tint_u32 = ImGui::ColorConvertFloat4ToU32(tint4);

      proc_engine->draw_background(draw, origin, avail, tint_u32, pan_px_x, pan_px_y, chrome_seed, pcfg);
    } else {
      StarfieldStyle sf;
      sf.enabled = ui.system_map_starfield;
      sf.density = ui.map_starfield_density;
      sf.parallax = ui.map_starfield_parallax;
      sf.alpha = 1.0f;
      draw_starfield(draw, origin, avail, bg, pan_px_x, pan_px_y, chrome_seed, sf);
    }

    // Procedural particle field: deterministic dust/sparkles in screen space.
    // Drawn after starfield/proc background and before nebula microfields.
    if (ui.system_map_particle_field && particle_engine && ui.map_particle_opacity > 0.0f) {
      ProcParticleFieldConfig pcfg;
      pcfg.enabled = true;
      pcfg.tile_px = ui.map_particle_tile_px;
      pcfg.particles_per_tile = ui.map_particle_particles_per_tile;
      pcfg.layers = ui.map_particle_layers;
      pcfg.layer0_parallax = ui.map_particle_layer0_parallax;
      pcfg.layer1_parallax = ui.map_particle_layer1_parallax;
      pcfg.layer2_parallax = ui.map_particle_layer2_parallax;
      pcfg.opacity = ui.map_particle_opacity;
      pcfg.base_radius_px = ui.map_particle_base_radius_px;
      pcfg.radius_jitter_px = ui.map_particle_radius_jitter_px;
      pcfg.twinkle_strength = ui.map_particle_twinkle_strength;
      pcfg.twinkle_speed = ui.map_particle_twinkle_speed;
      pcfg.animate_drift = ui.map_particle_drift;
      pcfg.drift_px_per_day = ui.map_particle_drift_px_per_day;
      pcfg.sparkles = ui.map_particle_sparkles;
      pcfg.sparkle_chance = ui.map_particle_sparkle_chance;
      pcfg.sparkle_length_px = ui.map_particle_sparkle_length_px;
      pcfg.debug_tile_bounds = ui.map_particle_debug_tiles;

      // Tint towards the theme background so the field belongs.
      const ImVec4 bg4 = ImGui::ColorConvertU32ToFloat4(bg);
      const ImVec4 tint4(0.85f + 0.15f * bg4.x, 0.85f + 0.15f * bg4.y, 0.90f + 0.10f * bg4.z, 1.0f);
      const ImU32 tint_u32 = ImGui::ColorConvertFloat4ToU32(tint4);

      particle_engine->draw_particles(draw, origin, avail, tint_u32, pan_px_x, pan_px_y,
                                      chrome_seed ^ 0x51A7EEDu, pcfg);

      const auto st = particle_engine->stats();
      ui.map_particle_last_frame_layers_drawn = st.layers_drawn;
      ui.map_particle_last_frame_tiles_drawn = st.tiles_drawn;
      ui.map_particle_last_frame_particles_drawn = st.particles_drawn;
    }

// Nebula microfield overlay: shows local pockets/filaments of nebula density.
    // Drawn after the starfield but before heatmaps/grid.
    if (ui.system_map_nebula_microfield_overlay && ui.system_map_nebula_overlay_opacity > 0.0f) {
      const int cells_x = std::clamp(ui.system_map_nebula_overlay_resolution, 16, 260);
      const float aspect = (avail.x > 1.0f) ? (avail.y / avail.x) : 1.0f;
      const int cells_y = std::clamp(static_cast<int>(std::round(static_cast<float>(cells_x) * aspect)), 8, 260);

      draw_nebula_microfield_overlay(draw, origin, avail, center, scale, zoom, pan, sim, sys->id,
                                    cells_x, cells_y, IM_COL32(0, 150, 230, 255),
                                    ui.system_map_nebula_overlay_opacity * 0.75f);
    }

    // Storm cell overlay: spatial storm intensity field (only visible during active storms).
    // Drawn after the starfield but before heatmaps/grid.
    if (ui.system_map_storm_cell_overlay && ui.system_map_storm_overlay_opacity > 0.0f) {
      const int cells_x = std::clamp(ui.system_map_storm_overlay_resolution, 16, 260);
      const float aspect = (avail.x > 1.0f) ? (avail.y / avail.x) : 1.0f;
      const int cells_y = std::clamp(static_cast<int>(std::round(static_cast<float>(cells_x) * aspect)), 8, 260);

      draw_nebula_storm_cell_overlay(draw, origin, avail, center, scale, zoom, pan, sim, sys->id,
                                    cells_x, cells_y, IM_COL32(255, 160, 80, 255),
                                    ui.system_map_storm_overlay_opacity * 0.75f);
    }

    // Procedural flow field: stylized "space weather" streamlines (curl-noise).
    // Drawn after nebula/storm overlays, before tactical heatmaps/grid.
    if (flow_engine) {
      ProcFlowFieldConfig fcfg;
      fcfg.enabled = ui.system_map_flow_field_overlay;
      fcfg.animate = ui.system_map_flow_field_animate;
      fcfg.opacity = ui.system_map_flow_field_opacity;
      fcfg.thickness_px = ui.system_map_flow_field_thickness_px;
      fcfg.step_px = ui.system_map_flow_field_step_px;
      fcfg.highlight_wavelength_px = ui.system_map_flow_field_highlight_wavelength_px;
      fcfg.animate_speed_cycles_per_day = ui.system_map_flow_field_animate_speed_cycles_per_day;
      fcfg.mask_by_nebula = ui.system_map_flow_field_mask_nebula;
      fcfg.mask_by_storms = ui.system_map_flow_field_mask_storms;
      fcfg.nebula_threshold = ui.system_map_flow_field_nebula_threshold;
      fcfg.storm_threshold = ui.system_map_flow_field_storm_threshold;
      fcfg.field_scale_mkm = ui.system_map_flow_field_scale_mkm;
      fcfg.tile_px = ui.system_map_flow_field_tile_px;
      fcfg.max_cached_tiles = ui.system_map_flow_field_cache_tiles;
      fcfg.lines_per_tile = ui.system_map_flow_field_lines_per_tile;
      fcfg.steps_per_line = ui.system_map_flow_field_steps_per_line;
      fcfg.debug_tile_bounds = ui.system_map_flow_field_debug_tiles;

      const std::uint32_t flow_seed = static_cast<std::uint32_t>(sys->id) ^ 0xA79F0F4Bu;
      flow_engine->draw_streamlines(draw, origin, avail, center, scale, zoom, pan, sim, sys->id,
                                   flow_seed, fcfg, IM_COL32(140, 220, 255, 255));

      const ProcFlowFieldStats st = flow_engine->stats();
      ui.system_map_flow_field_stats_cache_tiles = st.cache_tiles;
      ui.system_map_flow_field_stats_tiles_used = st.tiles_used_this_frame;
      ui.system_map_flow_field_stats_tiles_generated = st.tiles_generated_this_frame;
      ui.system_map_flow_field_stats_lines_drawn = st.lines_drawn;
      ui.system_map_flow_field_stats_segments_drawn = st.segments_drawn;
    }

    // Tactical heatmaps: coarse raster overlays for coverage/threat fields.
    // Drawn after the starfield but before the grid so the grid remains readable.
    if ((ui.system_map_sensor_heatmap || ui.system_map_threat_heatmap) && ui.system_map_heatmap_opacity > 0.0f) {
      const int cells_x = std::clamp(ui.system_map_heatmap_resolution, 16, 200);
      const float aspect = (avail.x > 1.0f) ? (avail.y / avail.x) : 1.0f;
      const int cells_y = std::clamp(static_cast<int>(std::round(static_cast<float>(cells_x) * aspect)), 8, 200);

      if (ui.system_map_sensor_heatmap && viewer_faction_id != kInvalidId && !sensor_sources.empty()) {
        // Two modes:
        //  - Fast grid (legacy)
        //  - Experimental LOS ray-traced shading (UI-only)
        if (ui.system_map_sensor_heatmap_raytrace) {
          std::vector<RaytraceSensorSource> srcs;
          srcs.reserve(sensor_sources.size());
          for (const auto& ssrc : sensor_sources) {
            const double r = ssrc.range_mkm * sensor_coverage_sig;
            if (r <= 1e-6) continue;
            RaytraceSensorSource hs;
            hs.pos_mkm = ssrc.pos_mkm;
            hs.range_mkm = r;
            hs.weight = 1.0f;
            hs.env_src_multiplier = sim.system_sensor_environment_multiplier_at(sys->id, ssrc.pos_mkm);
            srcs.push_back(hs);
          }

          SensorRaytraceHeatmapSettings hset;
          hset.max_depth = std::clamp(ui.system_map_sensor_raytrace_max_depth, 0, 10);
          hset.error_threshold = std::clamp(ui.system_map_sensor_raytrace_error_threshold, 0.0f, 0.5f);
          hset.spp = std::clamp(ui.system_map_sensor_raytrace_spp, 1, 16);
          hset.los_samples = std::clamp(ui.system_map_sensor_raytrace_los_samples, 1, 64);
          hset.los_strength = std::clamp(ui.system_map_sensor_raytrace_los_strength, 0.0f, 1.0f);
          hset.debug = ui.system_map_sensor_raytrace_debug;

          sensor_rt_stats = SensorRaytraceHeatmapStats{};
          draw_raytraced_sensor_heatmap(draw, origin, avail, center, scale, zoom, pan, sim, sys->id, srcs,
                                       IM_COL32(0, 170, 255, 255), ui.system_map_heatmap_opacity * 0.65f,
                                       chrome_seed ^ 0x51C0F00Du, hset,
                                       ui.system_map_sensor_raytrace_debug ? &sensor_rt_stats : nullptr);
          sensor_rt_stats_valid = ui.system_map_sensor_raytrace_debug;
        } else {
          std::vector<HeatmapSource> srcs;
          srcs.reserve(sensor_sources.size());
          for (const auto& ssrc : sensor_sources) {
            const double r = ssrc.range_mkm * sensor_coverage_sig;
            if (r <= 1e-6) continue;
            HeatmapSource hs;
            hs.pos_mkm = ssrc.pos_mkm;
            hs.range_mkm = r;
            hs.weight = 1.0f;
            srcs.push_back(hs);
          }

          draw_heatmap(draw, origin, avail, center, scale, zoom, pan, cells_x, cells_y, srcs,
                       IM_COL32(0, 170, 255, 255), ui.system_map_heatmap_opacity * 0.65f);
        }
      }

      if (ui.system_map_threat_heatmap && viewer_faction_id != kInvalidId && !threat_sources.empty()) {
        draw_heatmap(draw, origin, avail, center, scale, zoom, pan, cells_x, cells_y, threat_sources,
                     IM_COL32(255, 90, 90, 255), ui.system_map_heatmap_opacity * 0.75f);
      }

      // Procedural gravitational "well" contours (visualises a simplified
      // potential field sourced by system body masses).
      if (ui.system_map_gravity_contours_overlay && gravity_engine && sys) {
        ProcGravityContourConfig gcfg;
        gcfg.tile_px = std::clamp(ui.system_map_gravity_contours_tile_px, 128, 1024);
        gcfg.max_cached_tiles = std::clamp(ui.system_map_gravity_contours_cache_tiles, 16, 4096);
        gcfg.samples_per_tile = std::clamp(ui.system_map_gravity_contours_samples_per_tile, 8, 96);
        gcfg.contour_levels = std::clamp(ui.system_map_gravity_contours_levels, 1, 32);
        gcfg.level_spacing_decades = std::clamp(ui.system_map_gravity_contours_level_spacing_decades, 0.05f, 2.0f);
        gcfg.level_offset_decades = std::clamp(ui.system_map_gravity_contours_level_offset_decades, -6.0f, 6.0f);
        gcfg.opacity = std::clamp(ui.system_map_gravity_contours_opacity, 0.0f, 1.0f);
        gcfg.thickness_px = std::clamp(ui.system_map_gravity_contours_thickness_px, 0.5f, 5.0f);
        gcfg.softening_min_mkm = std::clamp(ui.system_map_gravity_contours_softening_min_mkm, 0.0005f, 250.0f);
        gcfg.softening_radius_mult = std::clamp(ui.system_map_gravity_contours_softening_radius_mult, 0.1f, 32.0f);
        gcfg.debug_tile_bounds = ui.system_map_gravity_contours_debug_tiles;

        const ImU32 contour_col = ImGui::GetColorU32(ImGuiCol_PlotLines);
        gravity_engine->draw_contours(draw, sim, sys->id, origin, avail, center, scale, zoom, pan,
                                      chrome_seed ^ 0xDEC0ADDEu, gcfg, contour_col);

        ui.system_map_gravity_contours_stats_cache_tiles = gravity_engine->stats().cache_tiles;
        ui.system_map_gravity_contours_stats_tiles_used = gravity_engine->stats().tiles_used;
        ui.system_map_gravity_contours_stats_tiles_generated = gravity_engine->stats().tiles_generated;
        ui.system_map_gravity_contours_stats_segments_drawn = gravity_engine->stats().segments_drawn;
      }
    }

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
    const double units_per_px = 1.0 / std::max(1e-12, scale * zoom);
    draw_scale_bar(draw, origin, avail, units_per_px, IM_COL32(220, 220, 220, 255), sb, "mkm");

    // Environmental overlay: system-level nebula + storms (affects sensors & movement).
    if (sys) {
      const double neb = std::clamp(sys->nebula_density, 0.0, 1.0);
      const bool has_storm = sim.system_has_storm(sys->id);
      if (neb > 0.01 || has_storm) {
        const double env = sim.system_sensor_environment_multiplier(sys->id);
        const double speed_env = sim.system_movement_speed_multiplier(sys->id);

        char buf[160];
        std::snprintf(buf,
                      sizeof(buf),
                      "Nebula %.0f%%  (Sensors x%.2f  Speed x%.2f)",
                      neb * 100.0,
                      env,
                      speed_env);
        draw->AddText(ImVec2(origin.x + 8.0f, origin.y + 8.0f), IM_COL32(170, 200, 255, 210), buf);

        if (has_storm) {
          const double cur = sim.system_storm_intensity(sys->id);
          const double peak = std::clamp(sys->storm_peak_intensity, 0.0, 1.0);
          const std::int64_t now = s.date.days_since_epoch();
          const int days_left = (sys->storm_end_day > now) ? static_cast<int>(sys->storm_end_day - now) : 0;

          char buf2[160];
          std::snprintf(buf2,
                        sizeof(buf2),
                        "Storm %.0f%%  (peak %.0f%%, %dd left)",
                        cur * 100.0,
                        peak * 100.0,
                        days_left);
          draw->AddText(ImVec2(origin.x + 8.0f, origin.y + 26.0f), IM_COL32(255, 210, 170, 220), buf2);
        }
      }
    }
    // Time preview badge (top-left).
    if (time_preview_enabled) {
      const int dd = static_cast<int>(std::round(time_preview_days));
      // Show the absolute target date (for the integer day offset).
      const std::string target_dt =
          nebula4x::format_datetime(s.date.days_since_epoch() + static_cast<std::int64_t>(dd), s.hour_of_day);

      char buf[196];
      std::snprintf(buf, sizeof(buf), "Time preview %+dd  (%s)  [T]", dd, target_dt.c_str());
      draw->AddText(ImVec2(origin.x + 8.0f, origin.y + 44.0f), IM_COL32(190, 210, 255, 210), buf);
    }

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

  struct MapLabelCandidate {
    Id id{kInvalidId};
    ImVec2 anchor{0.0f, 0.0f};
    float dx{0.0f};
    float dy{0.0f};
    const char* text{nullptr};
    ImU32 col{0};
    float priority{0.0f};
    int preferred_quadrant{0};
  };

  std::vector<MapLabelCandidate> map_labels;
  map_labels.reserve(sys->bodies.size() + sys->jump_points.size() + 8);
  const bool declutter_labels = !ImGui::GetIO().KeyAlt; // Alt = show all labels.

  // --- Procedural body sprites (optional) ---
  // These are cached CPU-rastered sprites uploaded to the active UI renderer backend.
  const std::uint32_t body_sprite_seed_base = hash_u32(static_cast<std::uint32_t>(sys->id) ^ 0xB0D1E5u);
  Vec2 primary_light_pos{0.0, 0.0};
  bool have_primary_light = false;
  for (Id bid : sys->bodies) {
    if (const auto* bb = find_ptr(s.bodies, bid)) {
      if (bb->type == BodyType::Star) {
        primary_light_pos = bb->position_mkm;
        have_primary_light = true;
        break;
      }
    }
  }

  ProcBodySpriteConfig body_sprite_cfg;
  body_sprite_cfg.sprite_px = ui.system_map_body_sprite_px;
  body_sprite_cfg.max_cached_sprites = ui.system_map_body_sprite_cache;
  body_sprite_cfg.light_steps = ui.system_map_body_sprite_light_steps;
  body_sprite_cfg.enable_rings = ui.system_map_body_sprite_rings;
  body_sprite_cfg.ring_probability = ui.system_map_body_sprite_ring_chance;
  body_sprite_cfg.ambient = ui.system_map_body_sprite_ambient;
  body_sprite_cfg.diffuse_strength = ui.system_map_body_sprite_diffuse;
  body_sprite_cfg.specular_strength = ui.system_map_body_sprite_specular;
  body_sprite_cfg.specular_power = ui.system_map_body_sprite_specular_power;

  const bool use_body_sprites =
      ui.system_map_body_sprites && body_sprites && body_sprites->ready() && body_sprite_cfg.sprite_px >= 16;

  if (use_body_sprites && ui.system_map_body_sprite_clear_cache_requested) {
    body_sprites->clear();
    ui.system_map_body_sprite_clear_cache_requested = false;
  }

  // --- Procedural contact icons (optional) ---
  ProcIconSpriteConfig icon_cfg;
  icon_cfg.sprite_px = ui.system_map_contact_icon_px;
  icon_cfg.max_cached_sprites = ui.system_map_contact_icon_cache;
  icon_cfg.ship_icon_size_px = ui.system_map_ship_icon_size_px;
  icon_cfg.ship_thrusters = ui.system_map_ship_icon_thrusters;
  icon_cfg.ship_thruster_opacity = ui.system_map_ship_icon_thruster_opacity;
  icon_cfg.ship_thruster_length_px = ui.system_map_ship_icon_thruster_length_px;
  icon_cfg.ship_thruster_width_px = ui.system_map_ship_icon_thruster_width_px;
  icon_cfg.missile_icon_size_px = ui.system_map_missile_icon_size_px;
  icon_cfg.wreck_icon_size_px = ui.system_map_wreck_icon_size_px;
  icon_cfg.anomaly_icon_size_px = ui.system_map_anomaly_icon_size_px;
  icon_cfg.anomaly_pulse = ui.system_map_anomaly_icon_pulse;
  icon_cfg.debug_bounds = ui.system_map_contact_icon_debug_bounds;

  const bool use_contact_icons = ui.system_map_contact_icons && icon_sprites && icon_sprites->ready() &&
                                 icon_cfg.sprite_px >= 16 && icon_cfg.ship_icon_size_px >= 6.0f;

  if (use_contact_icons && ui.system_map_contact_icon_clear_cache_requested) {
    icon_sprites->clear();
    ui.system_map_contact_icon_clear_cache_requested = false;
  }

  // --- Procedural jump-point phenomena sprites (optional) ---
  ProcJumpPhenomenaSpriteConfig jump_cfg;
  jump_cfg.sprite_px = ui.system_map_jump_phenomena_sprite_px;
  jump_cfg.max_cached_sprites = ui.system_map_jump_phenomena_cache;
  jump_cfg.size_mult = std::clamp(ui.system_map_jump_phenomena_size_mult, 1.0f, 16.0f);
  jump_cfg.opacity = std::clamp(ui.system_map_jump_phenomena_opacity, 0.0f, 1.0f);
  jump_cfg.animate = ui.system_map_jump_phenomena_animate;
  jump_cfg.animate_speed_cycles_per_day = std::clamp(ui.system_map_jump_phenomena_anim_speed_cycles_per_day, 0.0f, 4.0f);
  jump_cfg.pulse = ui.system_map_jump_phenomena_pulse;
  jump_cfg.pulse_speed_cycles_per_day = std::clamp(ui.system_map_jump_phenomena_pulse_cycles_per_day, 0.0f, 6.0f);
  jump_cfg.filaments = ui.system_map_jump_phenomena_filaments;
  jump_cfg.filaments_max = std::clamp(ui.system_map_jump_phenomena_filaments_max, 0, 64);
  jump_cfg.filament_strength = std::clamp(ui.system_map_jump_phenomena_filament_strength, 0.0f, 4.0f);
  jump_cfg.debug_bounds = ui.system_map_jump_phenomena_debug_bounds;

  const bool use_jump_fx = ui.system_map_jump_phenomena && jump_fx && jump_fx->ready() && jump_cfg.sprite_px >= 16;
  if (use_jump_fx && ui.system_map_jump_phenomena_clear_cache_requested) {
    jump_fx->clear();
    ui.system_map_jump_phenomena_clear_cache_requested = false;
  }

  // --- Procedural anomaly phenomena sprites (optional) ---
  ProcAnomalyPhenomenaSpriteConfig anomaly_cfg;
  anomaly_cfg.sprite_px = ui.system_map_anomaly_phenomena_sprite_px;
  anomaly_cfg.max_cached_sprites = ui.system_map_anomaly_phenomena_cache;
  anomaly_cfg.size_mult = std::clamp(ui.system_map_anomaly_phenomena_size_mult, 1.0f, 24.0f);
  anomaly_cfg.opacity = std::clamp(ui.system_map_anomaly_phenomena_opacity, 0.0f, 1.0f);
  anomaly_cfg.animate = ui.system_map_anomaly_phenomena_animate;
  anomaly_cfg.animate_speed_cycles_per_day =
      std::clamp(ui.system_map_anomaly_phenomena_anim_speed_cycles_per_day, 0.0f, 6.0f);
  anomaly_cfg.pulse = ui.system_map_anomaly_phenomena_pulse;
  anomaly_cfg.pulse_speed_cycles_per_day =
      std::clamp(ui.system_map_anomaly_phenomena_pulse_cycles_per_day, 0.0f, 6.0f);
  anomaly_cfg.filaments = ui.system_map_anomaly_phenomena_filaments;
  anomaly_cfg.filaments_max = std::clamp(ui.system_map_anomaly_phenomena_filaments_max, 0, 64);
  anomaly_cfg.filament_strength = std::clamp(ui.system_map_anomaly_phenomena_filament_strength, 0.0f, 4.0f);
  anomaly_cfg.glyph_overlay = ui.system_map_anomaly_phenomena_glyph_overlay;
  anomaly_cfg.glyph_strength = std::clamp(ui.system_map_anomaly_phenomena_glyph_strength, 0.0f, 1.0f);
  anomaly_cfg.debug_bounds = ui.system_map_anomaly_phenomena_debug_bounds;

  const bool use_anomaly_fx = ui.system_map_anomaly_phenomena && anomaly_fx && anomaly_fx->ready() &&
                              anomaly_cfg.sprite_px >= 16 && anomaly_cfg.size_mult >= 1.0f;
  if (use_anomaly_fx && ui.system_map_anomaly_phenomena_clear_cache_requested) {
    anomaly_fx->clear();
    ui.system_map_anomaly_phenomena_clear_cache_requested = false;
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

    // Time preview overlay (planning): future orbital position + swept trail.
    if (time_preview_active) {
      auto itf = body_pos_future.find(bid);
      if (itf != body_pos_future.end()) {
        const Vec2 future_w = itf->second;
        const ImVec2 pf = to_screen(future_w, center, scale, zoom, pan);

        const float dx = pf.x - p.x;
        const float dy = pf.y - p.y;
        const float d2 = dx * dx + dy * dy;
        const bool moved = d2 > 4.0f; // > ~2px

        const ImU32 base = color_body(b->type);

        if (ui.system_map_time_preview_trails && !body_pos_trail.empty()) {
          const ImU32 col_trail = modulate_alpha(base, 0.18f);

          ImVec2 prev = p;
          if (auto it0 = body_pos_trail.front().find(bid); it0 != body_pos_trail.front().end()) {
            prev = to_screen(it0->second, center, scale, zoom, pan);
          }

          for (const auto& sample : body_pos_trail) {
            auto it = sample.find(bid);
            if (it == sample.end()) continue;
            const ImVec2 pt = to_screen(it->second, center, scale, zoom, pan);
            draw->AddLine(prev, pt, col_trail, 1.0f);
            prev = pt;
          }
        }

        if (ui.system_map_time_preview_vectors && moved) {
          const ImU32 col_vec = modulate_alpha(base, 0.35f);
          draw->AddLine(p, pf, col_vec, 1.0f);
          add_arrowhead(draw, p, pf, col_vec, 7.0f);
        }

        // Ghost marker at the preview time.
        if (moved) {
          const ImU32 col_ghost = modulate_alpha(base, 0.60f);
          draw->AddCircle(pf, r + 2.0f, col_ghost, 0, 1.75f);
          draw->AddCircleFilled(pf, std::max(1.0f, r * 0.25f), modulate_alpha(base, 0.14f), 0);
        }
      }
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
    // Prefer cached procedural sprites when enabled; fall back to glyphs for
    // extremely small bodies or if the sprite engine isn't available.
    bool drew_sprite = false;
    if (use_body_sprites && r >= 2.0f) {
      const std::uint32_t bid_lo = static_cast<std::uint32_t>(bid);
      const std::uint32_t bid_hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(bid) >> 32);
      const std::uint32_t seed = hash_u32(body_sprite_seed_base ^ bid_lo ^ (bid_hi * 0x9E3779B1u) ^ 0xC0FFEE11u);

      Vec2 light_dir{1.0, 0.0};
      if (have_primary_light) {
        light_dir = (primary_light_pos - b->position_mkm);
        if (light_dir.length_squared() <= 1e-12) {
          light_dir = Vec2{1.0, 0.0};
        } else {
          light_dir = light_dir.normalized();
        }
      }

      const auto sprite = body_sprites->get_body_sprite(*b, seed, light_dir, body_sprite_cfg);
      if (sprite.tex_id) {
        const float sr = std::max(0.01f, sprite.sphere_radius_norm);
        const float half_size = r / sr;
        const ImVec2 a{p.x - half_size, p.y - half_size};
        const ImVec2 bmax{p.x + half_size, p.y + half_size};
        draw->AddImage(sprite.tex_id, a, bmax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE);
        drew_sprite = true;
      }
    }

    if (!drew_sprite) {
      // Use deterministic procedural glyphs for readability / identity.
      if (r >= 2.0f) {
        procgen_gfx::draw_body_glyph(draw, p, r, *b, 1.0f, /*selected=*/false);
      } else {
        draw->AddCircleFilled(p, r, color_body(b->type), 0);
      }
    }

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
      float pr = 10.0f;
      if (bid == selected_body) pr += 1000.0f;
      if (colonized_bodies.find(bid) != colonized_bodies.end()) pr += 600.0f;
      if (!is_minor) pr += 240.0f; else pr += 60.0f;

      // When zoomed out, avoid flooding the map with minor labels unless the user holds Alt.
      if (declutter_labels && is_minor && zoom < 1.6f && pr < 500.0f) {
        // skip
      } else {
        ImU32 col = IM_COL32(200, 200, 200, 255);
        if (bid == selected_body) col = IM_COL32(255, 235, 160, 255);
        else if (colonized_bodies.find(bid) != colonized_bodies.end()) col = IM_COL32(210, 255, 220, 255);

        MapLabelCandidate c;
        c.id = bid;
        c.anchor = p;
        c.dx = 6.0f;
        c.dy = 6.0f;
        c.text = b->name.c_str();
        c.col = col;
        c.priority = pr;
        c.preferred_quadrant = 1; // bottom-right
        map_labels.push_back(c);
      }
    }
  }

  // Jump points (guard overlays)
  std::unordered_map<Id, int> guarded_jump_points;
  guarded_jump_points.reserve(sys->jump_points.size() + 4);

  Id selected_guard_jp = kInvalidId;
  double selected_guard_radius_mkm = 0.0;

  if (viewer_faction_id != kInvalidId) {
    for (const auto& [fid, fl] : s.fleets) {
      (void)fid;
      if (fl.faction_id != viewer_faction_id) continue;
      if (fl.mission.type != FleetMissionType::GuardJumpPoint) continue;
      if (fl.mission.guard_jump_point_id == kInvalidId) continue;
      guarded_jump_points[fl.mission.guard_jump_point_id] += 1;
    }

    if (selected_fleet && selected_fleet->mission.type == FleetMissionType::GuardJumpPoint) {
      selected_guard_jp = selected_fleet->mission.guard_jump_point_id;
      selected_guard_radius_mkm = selected_fleet->mission.guard_jump_radius_mkm;
    }
  }

  for (Id jid : sys->jump_points) {
    const auto* jp = find_ptr(s.jump_points, jid);
    if (!jp) continue;

    const ImVec2 p = to_screen(jp->position_mkm, center, scale, zoom, pan);
    const float r = 6.0f;
    const bool surveyed =
        (!ui.fog_of_war) || (viewer_faction_id != kInvalidId && sim.is_jump_point_surveyed_by_faction(viewer_faction_id, jid));
    const ImU32 col = (surveyed ? color_jump() : IM_COL32(90, 90, 100, 255));
    const ImU32 text_col = (surveyed ? IM_COL32(200, 200, 200, 255) : IM_COL32(140, 140, 150, 255));

    // Procedural jump phenomena visualization (cached sprite + optional filaments).
    if (use_jump_fx && (surveyed || ui.system_map_jump_phenomena_reveal_unsurveyed)) {
      const std::uint32_t seed =
          hash_u32(static_cast<std::uint32_t>(jid) ^ static_cast<std::uint32_t>(sys->id) ^ 0x4A504658u);
      const auto ph = procgen_jump_phenomena::generate(*jp);

      // Map stability->hue (stable=cyan, unstable=magenta), turbulence/shear->saturation.
      const float stability = static_cast<float>(ph.stability01);
      const float turbulence = static_cast<float>(ph.turbulence01);
      const float shear = static_cast<float>(ph.shear01);
      const float hue = std::clamp(0.85f + (0.52f - 0.85f) * stability, 0.0f, 1.0f);
      const float sat = std::clamp(0.52f + 0.38f * turbulence + 0.18f * shear, 0.10f, 1.0f);
      const float val = std::clamp(0.70f + 0.25f * (1.0f - stability) + 0.12f * turbulence, 0.15f, 1.0f);

      float cr = 1.0f, cg = 1.0f, cb = 1.0f;
      ImGui::ColorConvertHSVtoRGB(hue, sat, val, cr, cg, cb);

      float alpha = jump_cfg.opacity;
      const double t_anim_days = t_now_days + (ImGui::GetTime() / 86400.0);
      if (jump_cfg.pulse && jump_cfg.pulse_speed_cycles_per_day > 1e-6f) {
        const double phase = t_anim_days * static_cast<double>(jump_cfg.pulse_speed_cycles_per_day) * kTwoPi;
        const float pulse = 0.75f + 0.25f * std::sinf(static_cast<float>(phase + static_cast<double>(jid) * 0.017));
        alpha *= pulse;
      }
      alpha = std::clamp(alpha, 0.0f, 1.0f);

      const ImU32 tint = ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, alpha));

      float angle = 0.0f;
      if (jump_cfg.animate && jump_cfg.animate_speed_cycles_per_day > 1e-6f) {
        const double phase = t_anim_days * static_cast<double>(jump_cfg.animate_speed_cycles_per_day) * kTwoPi;
        angle = static_cast<float>(phase + static_cast<double>(seed) * 0.000004);
      }

      const auto spr = jump_fx->get_jump_sprite(*jp, seed, jump_cfg);
      if (spr.tex_id) {
        const float size_px = (r * 2.0f) * jump_cfg.size_mult;
        ProcJumpPhenomenaSpriteEngine::draw_sprite_rotated(draw, spr.tex_id, p, size_px, angle, tint);
        if (jump_cfg.debug_bounds) {
          const ImVec2 a{p.x - 0.5f * size_px, p.y - 0.5f * size_px};
          const ImVec2 b{p.x + 0.5f * size_px, p.y + 0.5f * size_px};
          draw->AddRect(a, b, IM_COL32(255, 0, 255, 140), 0.0f, 0, 1.0f);
        }
      }

      // Optional vector filaments (low-cost, time-animated) for high shear.
      if (jump_cfg.filaments) {
        const float radius_px = r * jump_cfg.size_mult;
        ProcJumpPhenomenaSpriteEngine::draw_filaments(draw, p, radius_px, ph, tint, t_anim_days, jump_cfg);
      }
    }

    // Guard overlays: show which jump points are currently being guarded by fleets
    // of the viewer faction, and draw the selected fleet's response radius.
    const auto itg = guarded_jump_points.find(jid);
    if (itg != guarded_jump_points.end()) {
      draw->AddCircle(p, r + 3.0f, IM_COL32(0, 255, 140, 170), 0, 2.0f);
      if (itg->second > 1) {
        draw->AddCircle(p, r + 5.0f, IM_COL32(0, 255, 140, 110), 0, 1.5f);
      }
    }

    if (jid == selected_guard_jp) {
      draw->AddCircle(p, r + 6.0f, IM_COL32(255, 220, 80, 220), 0, 2.5f);
      if (selected_guard_radius_mkm > 1e-9) {
        const float rr = static_cast<float>(selected_guard_radius_mkm * scale * zoom);
        if (rr > r + 8.0f && rr < 5000.0f) {
          draw->AddCircle(p, rr, IM_COL32(0, 255, 140, 70), 0, 1.25f);
        }
      }
    }

    procgen_gfx::draw_jump_glyph(draw, p, r, static_cast<std::uint32_t>(jid), col, 1.0f, surveyed);

    {
      float pr = 260.0f;
      if (jid == selected_guard_jp) pr += 800.0f;
      if (itg != guarded_jump_points.end()) pr += 120.0f + 20.0f * std::logf((float)itg->second + 1.0f);

      MapLabelCandidate c;
      c.id = jid;
      c.anchor = p;
      c.dx = 6.0f;
      c.dy = 6.0f;
      c.text = jp->name.c_str();
      c.col = text_col;
      c.priority = pr;
      c.preferred_quadrant = 0; // top-right
      map_labels.push_back(c);
    }
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
              } else if constexpr (std::is_same_v<T, nebula4x::TravelViaJump> || std::is_same_v<T, nebula4x::SurveyJumpPoint>) {
                const auto* jp = find_ptr(s.jump_points, o.jump_point_id);
                if (!jp || jp->system_id != sys->id) return std::nullopt;
                return jp->position_mkm;
              } else if constexpr (std::is_same_v<T, nebula4x::AttackShip>) {
                // Fog-of-war safety: only reveal the true target position when
                // the attacking faction currently detects the ship.
                if (const auto* tgt = find_ptr(s.ships, o.target_ship_id); tgt && tgt->system_id == sys->id) {
                  if (sim.is_ship_detected_by_faction(sh->faction_id, o.target_ship_id)) {
                    return tgt->position_mkm;
                  }
                }

                // Otherwise show last-known / search waypoint so the player can
                // understand what their ship is doing without leaking intel.
                if (o.has_last_known) {
                  return o.last_known_position_mkm + (o.has_search_offset ? o.search_offset_mkm : Vec2{0.0, 0.0});
                }
                return std::nullopt;
              } else if constexpr (std::is_same_v<T, nebula4x::EscortShip> ||
                                   std::is_same_v<T, nebula4x::TransferCargoToShip> ||
                                   std::is_same_v<T, nebula4x::TransferFuelToShip> ||
                                   std::is_same_v<T, nebula4x::TransferTroopsToShip>) {
                if (const auto* tgt = find_ptr(s.ships, o.target_ship_id); tgt && tgt->system_id == sys->id) {
                  return tgt->position_mkm;
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

  // Optional: sensor coverage overlay for the viewer faction.
  if (ui.show_faction_sensor_coverage && viewer_faction_id != kInvalidId && !sensor_sources.empty()) {
    const int max_draw = std::clamp(ui.faction_sensor_coverage_max_sources, 1, 4096);
    ui.faction_sensor_coverage_max_sources = max_draw;

    const float alpha = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);
    const ImU32 col_outline = modulate_alpha(IM_COL32(0, 170, 255, 255), 0.16f * alpha);
    const ImU32 col_fill = modulate_alpha(IM_COL32(0, 170, 255, 255), 0.03f * alpha);

    for (const auto& src : sensor_sources) {
      if (src.range_mkm <= 1e-6) {
        continue;
      }
      const double r_mkm = src.range_mkm * sensor_coverage_sig;
      if (r_mkm <= 1e-6) {
        continue;
      }
      const float r_px = static_cast<float>(r_mkm * scale * zoom);
      const ImVec2 p = to_screen(src.pos_mkm, center, scale, zoom, pan);

      if (ui.faction_sensor_coverage_fill) {
        draw->AddCircleFilled(p, r_px, col_fill, 0);
      }
      draw->AddCircle(p, r_px, col_outline, 0, 1.0f);

      sensor_sources_drawn += 1;
      if (sensor_sources_drawn >= max_draw) {
        break;
      }
    }
  }

  // --- Procedural motion trails (vector FX) ---
  //
  // Trails are recorded in world-space (mkm) and rendered via ImDrawList.
  // They are backend-agnostic (no textures), so they also work in the
  // SDL_Renderer2 fallback.
  if (trail_engine) {
    // Clamp values even if config was edited manually in a JSON file.
    ui.system_map_motion_trails_max_age_days = std::clamp(ui.system_map_motion_trails_max_age_days, 0.25f, 60.0f);
    ui.system_map_motion_trails_sample_hours = std::clamp(ui.system_map_motion_trails_sample_hours, 0.05f, 72.0f);
    ui.system_map_motion_trails_min_seg_px = std::clamp(ui.system_map_motion_trails_min_seg_px, 0.5f, 64.0f);
    ui.system_map_motion_trails_thickness_px = std::clamp(ui.system_map_motion_trails_thickness_px, 0.5f, 12.0f);
    ui.system_map_motion_trails_alpha = std::clamp(ui.system_map_motion_trails_alpha, 0.0f, 1.0f);

    const bool enabled = ui.system_map_motion_trails;
    if (enabled) {
      const double max_age_days = static_cast<double>(ui.system_map_motion_trails_max_age_days);
      const double sample_interval_days = static_cast<double>(ui.system_map_motion_trails_sample_hours) / 24.0;
      const double px_per_mkm = std::max(1e-12, scale * zoom);
      const double min_dist_mkm = static_cast<double>(ui.system_map_motion_trails_min_seg_px) / px_per_mkm;

      // Always sample all *visible* ships so selecting a ship later shows its recent history.
      for (Id sid : sys->ships) {
        const Ship* sh = find_ptr(s.ships, sid);
        if (!sh) continue;

        if (ui.fog_of_war && viewer_faction_id != kInvalidId && sh->faction_id != viewer_faction_id) {
          if (!sim.is_ship_detected_by_faction(viewer_faction_id, sid)) {
            continue; // don't cache hidden info
          }
        }

        trail_engine->sample_ship(sys->id, sid, sh->position_mkm, t_now_days, sample_interval_days, min_dist_mkm,
                                  max_age_days);
      }

      // Optional: missile salvo trails (respects the same fog-of-war filtering used by the main drawing).
      if (ui.system_map_motion_trails_missiles && ui.system_map_missile_salvos) {
        for (const auto& [mid, ms] : s.missile_salvos) {
          if (ms.system_id != sys->id) continue;

          if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
            if (ms.attacker_id != kInvalidId) {
              const Ship* attacker = find_ptr(s.ships, ms.attacker_id);
              if (attacker && attacker->faction_id != viewer_faction_id) {
                if (!sim.is_ship_detected_by_faction(viewer_faction_id, ms.attacker_id)) {
                  continue;
                }
              }
            }
            if (ms.target_ship_id != kInvalidId) {
              const Ship* target = find_ptr(s.ships, ms.target_ship_id);
              if (target && target->faction_id != viewer_faction_id) {
                if (!sim.is_ship_detected_by_faction(viewer_faction_id, ms.target_ship_id)) {
                  continue;
                }
              }
            }
          }

          trail_engine->sample_missile(sys->id, mid, ms.pos_mkm, t_now_days, sample_interval_days, min_dist_mkm,
                                       std::min(max_age_days, 2.0));
        }
      }

      // Recompute counts after sampling so settings/legend can show accurate numbers.
      trail_engine->rebuild_stats();

      const ProcTrailStats& ts = trail_engine->stats();
      ui.system_map_motion_trails_stats_systems = ts.systems;
      ui.system_map_motion_trails_stats_tracks = ts.ship_tracks + ts.missile_tracks;
      ui.system_map_motion_trails_stats_points = ts.points;
      ui.system_map_motion_trails_stats_pruned_points_this_frame = ts.points_pruned_this_frame;
      ui.system_map_motion_trails_stats_pruned_tracks_this_frame = ts.tracks_pruned_this_frame;

      // Draw ship trails beneath icons.
      const float base_thickness = ui.system_map_motion_trails_thickness_px;
      const float base_alpha = ui.system_map_motion_trails_alpha;

      auto should_draw_ship = [&](Id sid, const Ship* sh) {
        if (ui.system_map_motion_trails_all_ships) return true;
        if (sid == selected_ship) return true;
        if (selected_fleet_members.find(sid) != selected_fleet_members.end()) return true;
        // If nothing is selected, default to showing the viewer faction.
        if (selected_ship == kInvalidId && selected_fleet_members.empty() && viewer_faction_id != kInvalidId) {
          return sh && sh->faction_id == viewer_faction_id;
        }
        return false;
      };

      for (Id sid : sys->ships) {
        const Ship* sh = find_ptr(s.ships, sid);
        if (!sh) continue;

        if (!should_draw_ship(sid, sh)) continue;

        if (ui.fog_of_war && viewer_faction_id != kInvalidId && sh->faction_id != viewer_faction_id) {
          if (!sim.is_ship_detected_by_faction(viewer_faction_id, sid)) {
            continue;
          }
        }

        const auto* tr = trail_engine->ship_track(sys->id, sid);
        if (!tr || tr->points.size() < 2) continue;

        // Base color: faction color, but highlight selection.
        ImU32 base_col_u32 = (sid == selected_ship) ? IM_COL32(255, 255, 255, 255) : color_faction(sh->faction_id);

        // Speed-based brightness (optional).
        float speed_mul = 1.0f;
        if (ui.system_map_motion_trails_speed_brighten) {
          const ShipDesign* d = sim.find_design(sh->design_id);
          const double vmax = d ? std::max(1e-9, d->max_speed_kms) : 0.0;
          if (vmax > 0.0) {
            const double v_km_s = (sh->velocity_mkm_per_day.length() * 1e6) / 86400.0;
            const double frac = std::clamp(v_km_s / vmax, 0.0, 1.0);
            speed_mul = static_cast<float>(0.55 + 0.45 * frac);
          }
        }

        // Draw segment-by-segment to get a cheap age-based fade gradient.
        for (std::size_t i = 1; i < tr->points.size(); ++i) {
          const auto& a = tr->points[i - 1];
          const auto& b = tr->points[i];
          const double dt = b.t_days - a.t_days;
          if (dt <= 1e-12) continue;

          const double seg_age = t_now_days - b.t_days;
          double fade = 1.0;
          if (max_age_days > 1e-9) {
            const double x = std::clamp(seg_age / max_age_days, 0.0, 1.0);
            // Slight gamma so the newest part is more readable.
            fade = std::pow(1.0 - x, 1.6);
          }

          float alpha = base_alpha * static_cast<float>(fade) * speed_mul;
          if (alpha <= 0.001f) continue;

          const Vec2 seg = b.pos_mkm - a.pos_mkm;
          const double seg_len_px = seg.length() * px_per_mkm;
          if (seg_len_px < 0.25) {
            continue; // avoid tiny segments when zoomed far out
          }

          ImVec4 col4 = ImGui::ColorConvertU32ToFloat4(base_col_u32);
          col4.w = std::clamp(col4.w * alpha, 0.0f, 1.0f);
          const ImU32 col = ImGui::ColorConvertFloat4ToU32(col4);

          const ImVec2 p0 = to_screen(a.pos_mkm, center, scale, zoom, pan);
          const ImVec2 p1 = to_screen(b.pos_mkm, center, scale, zoom, pan);

          draw->AddLine(p0, p1, col, base_thickness);
        }
      }
    } else {
      // Keep UI-visible stats reasonably fresh even when disabled.
      const ProcTrailStats& ts = trail_engine->stats();
      ui.system_map_motion_trails_stats_systems = ts.systems;
      ui.system_map_motion_trails_stats_tracks = ts.ship_tracks + ts.missile_tracks;
      ui.system_map_motion_trails_stats_points = ts.points;
      ui.system_map_motion_trails_stats_pruned_points_this_frame = ts.points_pruned_this_frame;
      ui.system_map_motion_trails_stats_pruned_tracks_this_frame = ts.tracks_pruned_this_frame;
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

    // Time preview overlay (planning): inertial projection (based on current velocity).
    const bool show_time_preview_ship =
        time_preview_active &&
        (ui.system_map_time_preview_all_ships || is_selected ||
         (selected_fleet != nullptr && selected_fleet->leader_ship_id == sid));

    if (show_time_preview_ship) {
      const Vec2 future_w = sh->position_mkm + sh->velocity_mkm_per_day * time_preview_days;
      const ImVec2 pf = to_screen(future_w, center, scale, zoom, pan);

      const float dx = pf.x - p.x;
      const float dy = pf.y - p.y;
      const float d2 = dx * dx + dy * dy;
      const bool moved = d2 > 4.0f; // > ~2px

      ImU32 preview_col = color_faction(sh->faction_id);
      if (viewer_faction_id != kInvalidId) {
        if (ds == DiplomacyStatus::Friendly) {
          preview_col = IM_COL32(120, 255, 180, 255);
        } else if (ds == DiplomacyStatus::Hostile) {
          preview_col = IM_COL32(255, 120, 90, 255);
        }
      }

      if (ui.system_map_time_preview_vectors && moved) {
        const ImU32 col_vec = modulate_alpha(preview_col, 0.35f);
        draw->AddLine(p, pf, col_vec, 1.0f);
        add_arrowhead(draw, p, pf, col_vec, 7.0f);
      }

      if (ui.system_map_time_preview_trails && moved) {
        const ImU32 col_dot = modulate_alpha(preview_col, 0.20f);
        // Three "breadcrumb" dots along the projected vector.
        for (int i = 1; i <= 3; ++i) {
          const float t = static_cast<float>(i) / 4.0f;
          const ImVec2 pt(p.x + dx * t, p.y + dy * t);
          draw->AddCircleFilled(pt, 1.6f, col_dot, 0);
        }
      }

      // Ghost marker at preview time.
      if (moved) {
        const ImU32 col_ghost = modulate_alpha(preview_col, 0.55f);
        draw->AddCircle(pf, 6.0f, col_ghost, 0, 1.5f);
        draw->AddCircleFilled(pf, 2.0f, modulate_alpha(preview_col, 0.12f), 0);
      }
    }

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

    if (use_contact_icons) {
      const std::uint32_t ship_seed = hash_u32(static_cast<std::uint32_t>(sid) ^ body_sprite_seed_base ^ 0x51EEDu);
      const auto sprite = icon_sprites->get_ship_icon(*sh, d, ship_seed, icon_cfg);

      // Heading points along current velocity. In screen-space, y+ is down, which matches the map's screen transform.
      const Vec2 v = sh->velocity_mkm_per_day;
      const double speed_mkm_day = v.length();
      float angle = 0.0f;
      float speed01 = 0.0f;
      if (speed_mkm_day > 1e-10) {
        angle = static_cast<float>(std::atan2(v.y, v.x));
        speed01 = 1.0f;
        if (d && d->max_speed_kms > 0.0) {
          const double max_mkm_day = d->max_speed_kms * 0.0864; // 1 km/s = 0.0864 mkm/day
          if (max_mkm_day > 1e-12) {
            speed01 = static_cast<float>(std::clamp(speed_mkm_day / max_mkm_day, 0.0, 1.0));
          }
        }
      }

      float sz = icon_cfg.ship_icon_size_px;
      if (is_selected) sz *= 1.15f;

      // Thruster plume (drawn behind the sprite).
      if (icon_cfg.ship_thrusters && speed01 > 0.05f) {
        icon_sprites->draw_ship_thruster(draw, p, sz, angle, speed01, icon_cfg);
      }

      // Subtle drop shadow to make icons pop over the background.
      ProcIconSpriteEngine::add_image_rotated(draw, sprite.tex_id, ImVec2(p.x + 1.0f, p.y + 1.0f), sz, angle,
                                              IM_COL32(0, 0, 0, 140));
      ProcIconSpriteEngine::add_image_rotated(draw, sprite.tex_id, p, sz, angle, ship_col);

      if (is_selected) {
        const float halo = sz * 0.60f + 2.0f;
        draw->AddCircle(p, halo, IM_COL32(0, 255, 140, 255), 0, 1.5f);
      }
      if (is_fleet_member) {
        const float halo = sz * 0.60f + 5.0f;
        draw->AddCircle(p, halo, IM_COL32(0, 160, 255, 200), 0, 1.5f);
      }
    } else {
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

      Vec2 pos_mkm = ms.pos_mkm;
      if (pos_mkm.length() <= 1e-12) {
        // Legacy path (pre-homing missiles): infer position from ETA on a
        // straight-line track from launch -> target-at-launch.
        const double total = std::max(1e-6, ms.eta_days_total);
        const double rem = std::max(0.0, ms.eta_days_remaining);
        const double frac = std::clamp(1.0 - rem / total, 0.0, 1.0);
        pos_mkm = ms.launch_pos_mkm + (ms.target_pos_mkm - ms.launch_pos_mkm) * frac;
      }

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
      if (use_contact_icons) {
        const std::uint32_t seed = hash_u32(static_cast<std::uint32_t>(mid) ^ body_sprite_seed_base ^ 0xA15511Eu);
        const auto sprite = icon_sprites->get_missile_icon(seed, icon_cfg);
        const float angle = std::atan2(t.y - p.y, t.x - p.x);
        const float sz = std::max(6.0f, icon_cfg.missile_icon_size_px);
        ProcIconSpriteEngine::add_image_rotated(draw, sprite.tex_id, ImVec2(p.x + 1.0f, p.y + 1.0f), sz, angle,
                                                IM_COL32(0, 0, 0, 140));
        ProcIconSpriteEngine::add_image_rotated(draw, sprite.tex_id, p, sz, angle, base);
      } else {
        draw->AddCircleFilled(ImVec2(p.x + 1.0f, p.y + 1.0f), 2.7f, IM_COL32(0, 0, 0, 140));
        draw->AddCircleFilled(p, 2.7f, base);
      }
    }
  }

  // Wreck markers (salvageable debris)
  for (const auto& [wid, w] : s.wrecks) {
    if (w.system_id != sys->id) continue;
    const ImVec2 p = to_screen(w.position_mkm, center, scale, zoom, pan);
    if (use_contact_icons) {
      const std::uint32_t seed = hash_u32(static_cast<std::uint32_t>(wid) ^ body_sprite_seed_base ^ 0xD00DF00Du);
      const auto sprite = icon_sprites->get_wreck_icon(w, seed, icon_cfg);

      // Deterministic rotation for variety.
      const float angle = static_cast<float>((seed % 360u) * (kPi / 180.0));
      const float sz = std::max(8.0f, icon_cfg.wreck_icon_size_px);
      const ImU32 col = IM_COL32(160, 160, 160, 220);

      ProcIconSpriteEngine::add_image_rotated(draw, sprite.tex_id, ImVec2(p.x + 1.0f, p.y + 1.0f), sz, angle,
                                              IM_COL32(0, 0, 0, 140));
      ProcIconSpriteEngine::add_image_rotated(draw, sprite.tex_id, p, sz, angle, col);
    } else {
      const float r = 5.0f;
      const ImU32 c = IM_COL32(160, 160, 160, 200);
      draw->AddLine(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), c, 2.0f);
      draw->AddLine(ImVec2(p.x - r, p.y + r), ImVec2(p.x + r, p.y - r), c, 2.0f);
    }
  }

  // Anomaly markers (unresolved points of interest)
  for (const auto& [aid, a] : s.anomalies) {
    if (a.system_id != sys->id) continue;
    if (a.resolved) continue;

    if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
      if (!sim.is_anomaly_discovered_by_faction(viewer_faction_id, aid)) continue;
    }

    const ImVec2 p = to_screen(a.position_mkm, center, scale, zoom, pan);

    // Procedural color: anomalies get a stable tint derived from their kind, with
    // hazard/reward slightly influencing saturation/value.
    const std::uint64_t kind_h = nebula4x::procgen_obscure::fnv1a_64(a.kind);
    const float kind_hue = static_cast<float>(kind_h % 360ull) / 360.0f;
    const float hz = (a.hazard_chance > 1e-9 && a.hazard_damage > 1e-9)
                         ? std::clamp(static_cast<float>(a.hazard_chance * std::clamp(a.hazard_damage / 20.0, 0.0, 1.0)),
                                      0.0f,
                                      1.0f)
                         : 0.0f;
    double tot_mins = 0.0;
    for (const auto& [_, t] : a.mineral_reward) tot_mins += std::max(0.0, t);
    const float rw = std::clamp(static_cast<float>((a.research_reward / 200.0) + (tot_mins / 20000.0) +
                                                   (!a.unlock_component_id.empty() ? 0.25 : 0.0)),
                                0.0f,
                                1.0f);

    const float sat = std::clamp(0.55f + 0.25f * hz, 0.25f, 1.0f);
    const float val = std::clamp(0.95f + 0.10f * rw - 0.08f * hz, 0.25f, 1.0f);

    const ImU32 col = modulate_alpha(ImColor::HSV(kind_hue, sat, val), 0.92f);

    const std::uint32_t seed = hash_u32(static_cast<std::uint32_t>(aid) ^ body_sprite_seed_base ^ 0xA1100A1Du);

    // Optional: procedural phenomena halo + filaments.
    if (use_anomaly_fx) {
      const auto sprite = anomaly_fx->get_anomaly_sprite(a, seed, anomaly_cfg);

      const float base_sz = std::max(8.0f, icon_cfg.anomaly_icon_size_px);
      const float fx_sz = std::max(10.0f, base_sz) * anomaly_cfg.size_mult;

      const double t_anim_days = t_now_days + (ImGui::GetTime() / 86400.0);
      const float base_rot = static_cast<float>((seed % 360u) * (kPi / 180.0));
      const float angle = anomaly_cfg.animate
                              ? (base_rot + static_cast<float>(t_anim_days * anomaly_cfg.animate_speed_cycles_per_day * kTwoPi))
                              : base_rot;

      float pulse_mul = 1.0f;
      if (anomaly_cfg.pulse && anomaly_cfg.pulse_speed_cycles_per_day > 1e-6f) {
        const double cyc = t_anim_days * static_cast<double>(anomaly_cfg.pulse_speed_cycles_per_day);
        pulse_mul = 0.72f + 0.28f * static_cast<float>(0.5 + 0.5 * std::sin(cyc * kTwoPi + static_cast<double>(seed & 1023u)));
      }

      const ImU32 fx_tint = modulate_alpha(col, anomaly_cfg.opacity * pulse_mul);

      // Soft shadow first.
      ProcAnomalyPhenomenaSpriteEngine::draw_sprite_rotated(draw, sprite, ImVec2(p.x + 1.0f, p.y + 1.0f), fx_sz, angle,
                                                           IM_COL32(0, 0, 0, 110));
      // Then the sprite + filament overlays.
      ProcAnomalyPhenomenaSpriteEngine::draw_sprite_rotated(draw, sprite, p, fx_sz, angle, fx_tint);
      ProcAnomalyPhenomenaSpriteEngine::draw_filaments(draw, p, fx_sz * 0.95f, a, seed, t_anim_days, fx_tint, anomaly_cfg);
    }

    // Center marker (contact icon or fallback glyph).
    if (use_contact_icons) {
      const auto icon_sprite = icon_sprites->get_anomaly_icon(a, seed, icon_cfg);

      // Deterministic rotation for variety (icons are symmetric so this is subtle).
      const float angle = static_cast<float>((seed % 360u) * (kPi / 180.0));
      const float sz = std::max(8.0f, icon_cfg.anomaly_icon_size_px);

      ProcIconSpriteEngine::add_image_rotated(draw, icon_sprite.tex_id, ImVec2(p.x + 1.0f, p.y + 1.0f), sz, angle,
                                              IM_COL32(0, 0, 0, 140));
      ProcIconSpriteEngine::add_image_rotated(draw, icon_sprite.tex_id, p, sz, angle, col);

      if (icon_cfg.anomaly_pulse && !use_anomaly_fx) {
        icon_sprites->draw_anomaly_pulse(draw, p, sz, static_cast<float>(ImGui::GetTime()), col, icon_cfg);
      }
    } else {
      // Subtle shadow + question-mark glyph (simple + recognizable).
      draw->AddCircleFilled(ImVec2(p.x + 1.0f, p.y + 1.0f), 7.0f, IM_COL32(0, 0, 0, 140));
      draw->AddText(ImVec2(p.x - 3.5f, p.y - 8.0f), col, "?");
    }
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

      std::unordered_map<Id, Vec2> member_pos;
      member_pos.reserve(c.members.size() * 2);
      for (Id sid : c.members) {
        if (const auto* sh = find_ptr(s.ships, sid)) {
          member_pos.emplace(sid, sh->position_mkm);
        }
      }

      const auto offsets = compute_fleet_formation_offsets(selected_fleet->formation,
                                                           selected_fleet->formation_spacing_mkm, leader_id,
                                                           leader_pos, raw_target, c.members, &member_pos);
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

  // Contact markers (fog-of-war memory)
  if (!recent_contacts.empty() && viewer_faction_id != kInvalidId) {
    const int now = static_cast<int>(s.date.days_since_epoch());

    for (const auto& c : recent_contacts) {
      // Don't draw a contact marker if the ship is currently detected (we already draw the ship itself).
      if (c.ship_id != kInvalidId && sim.is_ship_detected_by_faction(viewer_faction_id, c.ship_id)) continue;

      const int age = std::max(0, now - c.last_seen_day);

      // Predict a "best guess" position from the last two detections.
      const auto pred = predict_contact_position(c, now, sim.cfg().contact_prediction_max_days);
      const Vec2 pred_pos = pred.predicted_position_mkm;

      const ImVec2 p_pred = to_screen(pred_pos, center, scale, zoom, pan);
      const ImVec2 p_last = to_screen(c.last_seen_position_mkm, center, scale, zoom, pan);

      const float t_age = 1.0f -
                          (ui.contact_max_age_days > 0 ? (static_cast<float>(age) / static_cast<float>(ui.contact_max_age_days))
                                                       : 1.0f);
      const int a = std::clamp(static_cast<int>(60 + 140 * std::clamp(t_age, 0.0f, 1.0f)), 40, 220);
      const ImU32 col = IM_COL32(255, 180, 0, a);
      const ImU32 col_faint = modulate_alpha(col, 0.55f);
      const ImU32 col_ring = modulate_alpha(col, 0.30f);

      // Uncertainty ring centered on predicted position.
      double unc_mkm = 0.0;
      if (ui.show_contact_uncertainty) {
        unc_mkm = sim.contact_uncertainty_radius_mkm(c, now);
        if (unc_mkm > 1e-6) {
          const float r_px = static_cast<float>(unc_mkm * scale * zoom);
          // Skip degenerate or absurdly huge rings.
          if (r_px > 1.5f && r_px < 1.0e6f) {
            draw->AddCircle(p_pred, r_px, col_ring, 0, 1.25f);
          }
        }
      }

      // Draw a faint "track" from last seen -> predicted (if non-trivial).
      const float dx_lp = p_pred.x - p_last.x;
      const float dy_lp = p_pred.y - p_last.y;
      const float d2_lp = dx_lp * dx_lp + dy_lp * dy_lp;
      const bool show_track = d2_lp > 25.0f; // > ~5px
      if (show_track) {
        draw->AddLine(p_last, p_pred, col_ring, 1.0f);
        draw->AddCircle(p_last, 4.0f, col_faint, 0, 1.25f);
        draw->AddLine(ImVec2(p_last.x - 3.0f, p_last.y - 3.0f), ImVec2(p_last.x + 3.0f, p_last.y + 3.0f), col_faint, 1.5f);
        draw->AddLine(ImVec2(p_last.x - 3.0f, p_last.y + 3.0f), ImVec2(p_last.x + 3.0f, p_last.y - 3.0f), col_faint, 1.5f);
      }

      // Predicted marker.
      draw->AddCircle(p_pred, 6.0f, col, 0, 2.0f);
      draw->AddLine(ImVec2(p_pred.x - 5, p_pred.y - 5), ImVec2(p_pred.x + 5, p_pred.y + 5), col, 2.0f);
      draw->AddLine(ImVec2(p_pred.x - 5, p_pred.y + 5), ImVec2(p_pred.x + 5, p_pred.y - 5), col, 2.0f);

      // Highlight the actively selected contact (from Intel window / previous clicks).
      if (ui.selected_contact_ship_id != kInvalidId && c.ship_id == ui.selected_contact_ship_id) {
        const float t_p = (float)ImGui::GetTime();
        const float pulse = 0.5f + 0.5f * std::sin(t_p * 4.0f);
        const float r = 10.0f + pulse * 4.0f;
        draw->AddCircle(p_pred, r, IM_COL32(255, 230, 140, 190), 0, 2.5f);
      }

      if (ui.show_contact_labels) {
        std::string lbl = c.last_seen_name.empty() ? std::string("Unknown") : c.last_seen_name;
        lbl += "  (" + std::to_string(age) + "d";
        if (ui.show_contact_uncertainty && unc_mkm > 1e-3) {
          // Round for readability.
          const int unc_i = static_cast<int>(std::round(unc_mkm));
          lbl += ", ±" + std::to_string(unc_i) + " mkm";
        }
        lbl += ")";
        draw->AddText(ImVec2(p_pred.x + 8, p_pred.y + 8), IM_COL32(240, 220, 180, 220), lbl.c_str());
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
  if (hovered && !over_minimap && !over_legend && !ruler_consumed_left && can_issue_orders &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
          const bool alt = ImGui::GetIO().KeyAlt;
          if (fleet_mode) {
            if (alt) {
              sim.issue_fleet_survey_jump_point(selected_fleet->id, picked_jump, /*transit_when_done=*/false, ui.fog_of_war);
            } else {
              sim.issue_fleet_travel_via_jump(selected_fleet->id, picked_jump);
            }
          } else {
            if (alt) {
              sim.issue_survey_jump_point(selected_ship, picked_jump, /*transit_when_done=*/false, ui.fog_of_war);
            } else {
              sim.issue_travel_via_jump(selected_ship, picked_jump);
            }
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
  if (hovered && !over_minimap && !over_legend && !ruler_consumed_right && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
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
          const int now = static_cast<int>(s.date.days_since_epoch());
          for (const auto& c : recent_contacts) {
            if (c.ship_id == kInvalidId) continue;
            // Skip contacts that are currently detected (the real ship marker is pickable).
            if (viewer_faction_id != kInvalidId && sim.is_ship_detected_by_faction(viewer_faction_id, c.ship_id)) continue;
            const auto pred = predict_contact_position(c, now, sim.cfg().contact_prediction_max_days);
            const ImVec2 p_pred = to_screen(pred.predicted_position_mkm, center, scale, zoom, pan);
            const ImVec2 p_last = to_screen(c.last_seen_position_mkm, center, scale, zoom, pan);

            float best_d2_this = std::numeric_limits<float>::max();
            {
              const float dx = mp.x - p_pred.x;
              const float dy = mp.y - p_pred.y;
              best_d2_this = dx * dx + dy * dy;
            }
            {
              const float dx = mp.x - p_last.x;
              const float dy = mp.y - p_last.y;
              const float d2 = dx * dx + dy * dy;
              best_d2_this = std::min(best_d2_this, d2);
            }

            if (best_d2_this <= best_contact_d2) {
              best_contact_d2 = best_d2_this;
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
  if (hovered && mouse_in_rect && !over_minimap && !over_legend && !ImGui::IsAnyItemHovered() &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
    const ImVec2 mp = mouse;
    constexpr float kHoverRadiusPx = 18.0f;
    const float hover_d2 = kHoverRadiusPx * kHoverRadiusPx;

    enum class HoverKind { None, Ship, Missile, Anomaly, Wreck, Body, Jump };
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

        Vec2 pos_mkm = ms.pos_mkm;
        if (pos_mkm.length() <= 1e-12) {
          const double total = std::max(1e-6, ms.eta_days_total);
          const double rem = std::max(0.0, ms.eta_days_remaining);
          const double frac = std::clamp(1.0 - rem / total, 0.0, 1.0);
          pos_mkm = ms.launch_pos_mkm + (ms.target_pos_mkm - ms.launch_pos_mkm) * frac;
        }

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
      for (const auto& [aid, a] : s.anomalies) {
        if (a.system_id != sys->id) continue;
        if (a.resolved) continue;

        if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
          if (!sim.is_anomaly_discovered_by_faction(viewer_faction_id, aid)) continue;
        }

        const ImVec2 p = to_screen(a.position_mkm, center, scale, zoom, pan);
        const float dx = mp.x - p.x;
        const float dy = mp.y - p.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= best_d2) {
          best_d2 = d2;
          kind = HoverKind::Anomaly;
          hovered_id = aid;
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

          // Movement feedback: show last-tick velocity vector.
          {
            const double v_mkm_day = sh->velocity_mkm_per_day.length();
            const double sec_per_day = std::max(1e-9, sim.cfg().seconds_per_day);
            const double v_km_s = (v_mkm_day * 1e6) / sec_per_day;
            ImGui::TextDisabled("Velocity: (%.2f, %.2f) mkm/day (%.1f km/s)",
                                sh->velocity_mkm_per_day.x, sh->velocity_mkm_per_day.y, v_km_s);
          }

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
          if (ms->speed_mkm_per_day > 1e-9) {
            ImGui::TextDisabled("Speed: %.1f mkm/day", ms->speed_mkm_per_day);
          }
          if (ms->range_remaining_mkm > 1e-9) {
            ImGui::TextDisabled("Range remaining: %.1f mkm", ms->range_remaining_mkm);
          }
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
      } else if (kind == HoverKind::Anomaly) {
        const auto* a = find_ptr(s.anomalies, hovered_id);
        if (a) {
          const std::string nm = a->name.empty()
                                     ? (std::string("Anomaly ") + std::to_string(static_cast<int>(a->id)))
                                     : a->name;

          ImGui::TextUnformatted(nm.c_str());
          if (!a->kind.empty()) ImGui::TextDisabled("Kind: %s", a->kind.c_str());
          ImGui::TextDisabled("Investigation: %d days", std::max(1, a->investigation_days));

          // Microfield-aware context: anomalies hidden in dense pockets are harder to spot.
          {
            const double neb_local = sim.system_nebula_density_at(a->system_id, a->position_mkm);
            const double env_mult = sim.system_sensor_environment_multiplier_at(a->system_id, a->position_mkm);
            if (neb_local > 1e-6 || env_mult < 0.999) {
              ImGui::TextDisabled("Local nebula: %.0f%% (env x%.2f)", std::clamp(neb_local, 0.0, 1.0) * 100.0, std::clamp(env_mult, 0.0, 1.0));
            }
          }

          if (a->research_reward > 1e-9) {
            ImGui::TextDisabled("Reward: +%.1f RP", a->research_reward);
          }

          // Mineral cache reward (if any).
          {
            double total = 0.0;
            for (const auto& [_, t] : a->mineral_reward) total += std::max(0.0, t);
            if (total > 1e-6) {
              ImGui::TextDisabled("Minerals: %.0f tons", total);

              // Show up to 6 minerals (largest first).
              std::vector<std::pair<std::string, double>> items;
              items.reserve(a->mineral_reward.size());
              for (const auto& [m, t] : a->mineral_reward) {
                if (!(t > 1e-6)) continue;
                items.emplace_back(m, t);
              }
              std::sort(items.begin(), items.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
              int shown = 0;
              for (const auto& [m, t] : items) {
                if (shown++ >= 6) break;
                ImGui::BulletText("%s: %.1f", m.c_str(), t);
              }
            }
          }

          // Investigation hazard (non-lethal).
          if (a->hazard_chance > 1e-6 && a->hazard_damage > 1e-6) {
            ImGui::TextDisabled("Hazard: %.0f%% / %.1f dmg", a->hazard_chance * 100.0, a->hazard_damage);
          }

          // Procedural signature (stable per anomaly). Helpful for e.g. comparing repeated reports.
          {
            const std::string sig = nebula4x::procgen_obscure::anomaly_signature_code(*a);
            const std::string glyph = nebula4x::procgen_obscure::anomaly_signature_glyph(*a);
            ImGui::Separator();
            ImGui::TextDisabled("Signature: %s", sig.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 230));
            ImGui::TextUnformatted(glyph.c_str());
            ImGui::PopStyleColor();

            // If the anomaly phenomena engine is enabled, embed a small preview
            // (matches the System Map halo so users can visually correlate).
            if (use_anomaly_fx) {
              const std::uint32_t seed = hash_u32(static_cast<std::uint32_t>(a->id) ^ body_sprite_seed_base ^ 0xA1100A1Du);

              // Same tint logic as the on-map render.
              const std::uint64_t kind_h = nebula4x::procgen_obscure::fnv1a_64(a->kind);
              const float kind_hue = static_cast<float>(kind_h % 360ull) / 360.0f;
              const float hz = (a->hazard_chance > 1e-9 && a->hazard_damage > 1e-9)
                                   ? std::clamp(static_cast<float>(a->hazard_chance *
                                                                  std::clamp(a->hazard_damage / 20.0, 0.0, 1.0)),
                                                0.0f,
                                                1.0f)
                                   : 0.0f;
              double tot_mins = 0.0;
              for (const auto& [_, t] : a->mineral_reward) tot_mins += std::max(0.0, t);
              const float rw = std::clamp(static_cast<float>((a->research_reward / 200.0) + (tot_mins / 20000.0) +
                                                             (!a->unlock_component_id.empty() ? 0.25 : 0.0)),
                                          0.0f,
                                          1.0f);
              const float sat = std::clamp(0.55f + 0.25f * hz, 0.25f, 1.0f);
              const float val = std::clamp(0.95f + 0.10f * rw - 0.08f * hz, 0.25f, 1.0f);

              const ImU32 col = modulate_alpha(ImColor::HSV(kind_hue, sat, val), 0.92f);

              const auto sprite = anomaly_fx->get_anomaly_sprite(*a, seed, anomaly_cfg);
              ImGui::Spacing();
              ImGui::TextDisabled("Procedural preview");
              const ImVec4 tint = ImGui::ColorConvertU32ToFloat4(col);
              ImGui::Image(sprite.tex_id, ImVec2(72.0f, 72.0f), ImVec2(0, 0), ImVec2(1, 1), tint);
            }
          }

          if (!a->unlock_component_id.empty()) {
            const auto itc = sim.content().components.find(a->unlock_component_id);
            const std::string cname = (itc != sim.content().components.end() && !itc->second.name.empty())
                                          ? itc->second.name
                                          : a->unlock_component_id;
            ImGui::TextDisabled("Unlock: %s", cname.c_str());
          }

          if (ImGui::SmallButton("Center")) {
            pan = Vec2{-a->position_mkm.x, -a->position_mkm.y};
            ui.system_map_follow_selected = false;
          }

          // Convenience action for the currently selected ship.
          if (selected_ship != kInvalidId && viewer_faction_id != kInvalidId) {
            const Ship* sh = find_ptr(s.ships, selected_ship);
            if (sh && sh->faction_id == viewer_faction_id && !a->resolved) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Investigate")) {
                (void)sim.issue_investigate_anomaly(selected_ship, a->id, /*restrict_to_discovered=*/ui.fog_of_war);
                ui.request_details_tab = DetailsTab::Ship;
              }
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
                                (viewer_faction_id != kInvalidId &&
                                 sim.is_jump_point_surveyed_by_faction(viewer_faction_id, jp->id));
          ImGui::TextDisabled("Surveyed: %s", surveyed ? "Yes" : "No");

          // If we're running time-based surveys, show current progress.
          const double required_points = sim.jump_survey_required_points_for_jump(jp->id);
          if (!surveyed && ui.fog_of_war && required_points > 1e-9) {
            if (const auto* fac = find_ptr(s.factions, viewer_faction_id)) {
              auto itp = fac->jump_survey_progress.find(jp->id);
              if (itp != fac->jump_survey_progress.end() && std::isfinite(itp->second) && itp->second > 1e-9) {
                const double frac = std::clamp(itp->second / required_points, 0.0, 1.0);
                ImGui::TextDisabled("Survey progress: %.0f%%", frac * 100.0);
              }
              ImGui::TextDisabled("Required: %.0f pts", required_points);
            }
          }


          if (!surveyed) {
            ImGui::TextDisabled("To: (unknown)");
          } else if (const auto* other = find_ptr(s.jump_points, jp->linked_jump_id)) {
            if (const auto* dest = find_ptr(s.systems, other->system_id)) {
              if (!ui.fog_of_war || viewer_faction_id == kInvalidId ||
                  sim.is_system_discovered_by_faction(viewer_faction_id, dest->id)) {
                ImGui::TextDisabled("To: %s", dest->name.c_str());
              } else {
                ImGui::TextDisabled("To: (undiscovered system)");
              }
            } else {
              ImGui::TextDisabled("To: (unknown system)");
            }
          }


          // Jump-point phenomena readout.
          // In fog-of-war mode we only reveal this once the jump point is surveyed.
          const bool show_phenomena = (!ui.fog_of_war) || surveyed;
          if (sim.cfg().enable_jump_point_phenomena) {
            if (show_phenomena) {
              const auto ph = nebula4x::procgen_jump_phenomena::generate(*jp);

              // Optional: show a small live preview of the procedural phenomena sprite.
              if (jump_fx && jump_fx->ready()) {
                const std::uint32_t seed = hash_u32(static_cast<std::uint32_t>(jp->id) ^
                                                    static_cast<std::uint32_t>(jp->system_id) ^
                                                    0x4A504658u);
                const auto spr = jump_fx->get_jump_sprite(*jp, seed, jump_cfg);
                if (spr.tex_id) {
                  float rr = 0.0f, gg = 0.0f, bb = 0.0f;
                  const float stability = std::clamp(static_cast<float>(ph.stability01), 0.0f, 1.0f);
                  const float turb = std::clamp(static_cast<float>(ph.turbulence01), 0.0f, 1.0f);
                  const float shear = std::clamp(static_cast<float>(ph.shear01), 0.0f, 1.0f);
                  const float hue = std::clamp(0.85f + (0.52f - 0.85f) * stability, 0.0f, 1.0f);
                  const float sat = std::clamp(0.52f + 0.38f * turb + 0.18f * shear, 0.25f, 1.0f);
                  const float val = std::clamp(0.70f + 0.25f * (1.0f - stability) + 0.12f * turb, 0.25f, 1.0f);
                  ImGui::ColorConvertHSVtoRGB(hue, sat, val, rr, gg, bb);
                  ImGui::TextDisabled("Phenomena preview");
                  ImGui::Image(spr.tex_id, ImVec2(72.0f, 72.0f), ImVec2(0, 0), ImVec2(1, 1), ImVec4(rr, gg, bb, 1.0f));
                }
              }

              if (ImGui::TreeNode("Phenomena")) {
                if (!ph.signature_code.empty()) {
                  ImGui::TextDisabled("Signature: %s", ph.signature_code.c_str());
                }
                ImGui::TextDisabled("Stability: %.0f%%", std::clamp(ph.stability01, 0.0, 1.0) * 100.0);
                ImGui::TextDisabled("Turbulence: %.0f%%  Shear: %.0f%%",
                                    std::clamp(ph.turbulence01, 0.0, 1.0) * 100.0,
                                    std::clamp(ph.shear01, 0.0, 1.0) * 100.0);

                // Survey difficulty (relative to global base).
                const double base_req = sim.cfg().jump_survey_points_required;
                const double req = sim.jump_survey_required_points_for_jump(jp->id);
                if (base_req > 1e-9 && req > 1e-9) {
                  ImGui::TextDisabled("Survey diff: x%.2f", req / base_req);
                }

                // Current transit risk estimate (same formula as Simulation::tick_ships).
                if (sim.cfg().jump_phenomena_transit_hazard_strength > 1e-9) {
                  const double storm = sim.cfg().enable_nebula_storms ? sim.system_storm_intensity_at(jp->system_id, jp->position_mkm) : 0.0;
                  const double neb = std::clamp(sim.system_nebula_density_at(jp->system_id, jp->position_mkm), 0.0, 1.0);

                  double risk = std::clamp(ph.hazard_chance01, 0.0, 1.0);
                  risk *= std::max(0.0, sim.cfg().jump_phenomena_transit_hazard_strength);
                  if (surveyed) {
                    risk *= std::clamp(sim.cfg().jump_phenomena_hazard_surveyed_multiplier, 0.0, 1.0);
                  }
                  risk *= (1.0 + std::max(0.0, sim.cfg().jump_phenomena_storm_hazard_bonus) * storm);
                  risk *= (1.0 + 0.25 * neb);
                  risk = std::clamp(risk, 0.0, 1.0);

                  ImGui::TextDisabled("Transit risk (now): %.0f%%", risk * 100.0);

                  if (ph.hazard_damage_frac > 1e-6) {
                    ImGui::TextDisabled("Damage scale: %.0f%% max HP", std::clamp(ph.hazard_damage_frac, 0.0, 1.0) * 100.0);
                  }
                  if (ph.misjump_dispersion_mkm > 1e-6) {
                    ImGui::TextDisabled("Misjump dispersion: %.0f mkm", std::max(0.0, ph.misjump_dispersion_mkm));
                  }
                  if (ph.subsystem_glitch_chance01 > 1e-6) {
                    ImGui::TextDisabled("Subsystem glitch: %.0f%% (sev %.0f%%)",
                                        std::clamp(ph.subsystem_glitch_chance01, 0.0, 1.0) * 100.0,
                                        std::clamp(ph.subsystem_glitch_severity01, 0.0, 1.0) * 100.0);
                  }
                }

                if (!ph.stamp.empty()) {
                  ImGui::Separator();
                  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
                  ImGui::TextUnformatted(ph.stamp.c_str());
                  ImGui::PopStyleVar();
                }

                ImGui::TreePop();
              }
            } else {
              ImGui::TextDisabled("Phenomena: (unknown; survey to reveal)");
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

          if (can_issue_orders && !surveyed) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Survey")) {
              if (fleet_mode) {
                sim.issue_fleet_survey_jump_point(selected_fleet->id, hovered_id, /*transit_when_done=*/false, ui.fog_of_war);
              } else if (selected_ship != kInvalidId) {
                sim.issue_survey_jump_point(selected_ship, hovered_id, /*transit_when_done=*/false, ui.fog_of_war);
              }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Survey+Transit")) {
              if (fleet_mode) {
                sim.issue_fleet_survey_jump_point(selected_fleet->id, hovered_id, /*transit_when_done=*/true, ui.fog_of_war);
              } else if (selected_ship != kInvalidId) {
                sim.issue_survey_jump_point(selected_ship, hovered_id, /*transit_when_done=*/true, ui.fog_of_war);
              }
            }
          }
        }
      }
      ImGui::EndTooltip();
    }
  }

  // Ruler overlay (hold D + drag).
  if (ruler.active()) {
    const ImVec2 a = to_screen(ruler.start, center, scale, zoom, pan);
    const ImVec2 b = to_screen(ruler.end, center, scale, zoom, pan);

    const ImU32 col = IM_COL32(80, 220, 255, 235);
    draw_ruler_line(draw, a, b, col);

    // Summary label.
    const double dist_mkm = (ruler.end - ruler.start).length();
    const double au = dist_mkm / 149597.8707; // 1 AU in million km.

    // Speed basis: selected ship if present, otherwise fleet (slowest member).
    double speed_km_s = 0.0;
    if (selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(s.ships, selected_ship)) speed_km_s = sh->speed_km_s;
    } else if (selected_fleet && !selected_fleet->ship_ids.empty()) {
      double slowest = std::numeric_limits<double>::infinity();
      for (Id sid : selected_fleet->ship_ids) {
        if (const auto* sh = find_ptr(s.ships, sid)) slowest = std::min(slowest, sh->speed_km_s);
      }
      if (std::isfinite(slowest)) speed_km_s = slowest;
    }

    const double mkm_per_day = speed_km_s * sim.cfg().seconds_per_day / 1e6;
    char buf[192];
    if (mkm_per_day > 1e-9) {
      const double eta_days = dist_mkm / mkm_per_day;
      const std::string eta = format_duration_days(eta_days);
      std::snprintf(buf, sizeof(buf), "Δ %.1f mkm (%.3f AU)  @ %.0f km/s → %s", dist_mkm, au, speed_km_s,
                    eta.c_str());
    } else {
      std::snprintf(buf, sizeof(buf), "Δ %.1f mkm (%.3f AU)", dist_mkm, au);
    }

    const ImVec2 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    draw_ruler_label(draw, ImVec2(mid.x + 8.0f, mid.y + 8.0f), buf);
  }

  // Decluttered body/jump-point labels: draw after most overlays for legibility.
  if (!map_labels.empty()) {
    const ImVec2 vmin = origin;
    const ImVec2 vmax = ImVec2(origin.x + avail.x, origin.y + avail.y);

    const float area = std::max(1.0f, avail.x * avail.y);
    const float z = std::clamp(static_cast<float>(zoom), 0.35f, 3.5f);
    const int budget = static_cast<int>(std::clamp((area / (175.0f * 44.0f)) * (0.55f + 0.75f * z), 20.0f, 650.0f));

    std::stable_sort(map_labels.begin(), map_labels.end(), [](const MapLabelCandidate& a, const MapLabelCandidate& b) {
      if (a.priority != b.priority) return a.priority > b.priority;
      return a.id < b.id;
    });

    if (!declutter_labels) {
      for (const auto& c : map_labels) {
        const ImVec2 sz = ImGui::CalcTextSize(c.text);
        ImVec2 pos = c.anchor;
        if (c.preferred_quadrant == 1) {
          pos = ImVec2(c.anchor.x + c.dx, c.anchor.y + c.dy);
        } else if (c.preferred_quadrant == 0) {
          pos = ImVec2(c.anchor.x + c.dx, c.anchor.y - c.dy);
        } else if (c.preferred_quadrant == 2) {
          pos = ImVec2(c.anchor.x - c.dx - sz.x, c.anchor.y - c.dy);
        } else {
          pos = ImVec2(c.anchor.x - c.dx - sz.x, c.anchor.y + c.dy);
        }

        if (pos.x > vmax.x || pos.y > vmax.y) continue;
        if (pos.x + sz.x < vmin.x || pos.y + sz.y < vmin.y) continue;
        draw->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 170), c.text);
        draw->AddText(pos, c.col, c.text);
      }
    } else {
      LabelPlacer placer(vmin, vmax, 88.0f);
      int placed = 0;
      for (const auto& c : map_labels) {
        if (placed >= budget && c.priority < 800.0f) break;
        const ImVec2 sz = ImGui::CalcTextSize(c.text);
        ImVec2 pos;
        if (!placer.place_near(c.anchor, c.dx, c.dy, sz, 2.0f, c.preferred_quadrant, &pos)) continue;
        draw->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 170), c.text);
        draw->AddText(pos, c.col, c.text);
        ++placed;
      }
    }
  }

  // Minimap overlay (bottom-right).
  if (minimap_enabled) {
    const ImU32 mm_bg = IM_COL32(8, 8, 10, 200);
    const ImU32 mm_border = over_minimap ? IM_COL32(235, 235, 235, 220) : IM_COL32(90, 90, 95, 220);
    draw->AddRectFilled(mm_p0, mm_p1, mm_bg, 4.0f);
    draw->AddRect(mm_p0, mm_p1, mm_border, 4.0f);

    // Center star marker.
    {
      ImVec2 p0 = world_to_minimap_px(mm, Vec2{0.0, 0.0});
      p0 = clamp_to_rect(p0, mm_p0, mm_p1);
      draw->AddCircleFilled(p0, 3.0f, IM_COL32(255, 230, 180, 220), 0);
    }

    // Bodies.
    for (Id bid : sys->bodies) {
      const auto* b = find_ptr(s.bodies, bid);
      if (!b) continue;
      const bool is_minor = (b->type == BodyType::Asteroid || b->type == BodyType::Comet);
      if (!ui.show_minor_bodies && is_minor && selected_body != bid) continue;

      ImVec2 p = world_to_minimap_px(mm, b->position_mkm);
      p = clamp_to_rect(p, mm_p0, mm_p1);

      float r = 2.0f;
      ImU32 col = IM_COL32(190, 190, 200, 190);
      if (b->type == BodyType::GasGiant) col = IM_COL32(150, 210, 255, 200);
      if (b->type == BodyType::Asteroid || b->type == BodyType::Comet) {
        r = 1.4f;
        col = IM_COL32(160, 160, 170, 160);
      }
      if (bid == selected_body) {
        r = 3.2f;
        col = IM_COL32(245, 245, 245, 255);
      }
      draw->AddCircleFilled(p, r, col, 0);
      // Time preview ghost (future body position).
      if (time_preview_active) {
        if (auto itf = body_pos_future.find(bid); itf != body_pos_future.end()) {
          ImVec2 pf = world_to_minimap_px(mm, itf->second);
          pf = clamp_to_rect(pf, mm_p0, mm_p1);
          const float dx = pf.x - p.x;
          const float dy = pf.y - p.y;
          if ((dx * dx + dy * dy) > 1.0f) {
            draw->AddCircle(pf, r + 1.2f, modulate_alpha(col, 0.75f), 0, 1.1f);
          }
        }
      }

    }

    // Jump points.
    for (Id jid : sys->jump_points) {
      const auto* jp = find_ptr(s.jump_points, jid);
      if (!jp) continue;
      ImVec2 p = world_to_minimap_px(mm, jp->position_mkm);
      p = clamp_to_rect(p, mm_p0, mm_p1);
      draw->AddCircle(p, 3.5f, IM_COL32(160, 110, 255, 210), 0, 1.25f);
    }

    // Ships (friendly + detected hostiles under FoW).
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

      ImVec2 p = world_to_minimap_px(mm, sh->position_mkm);
      p = clamp_to_rect(p, mm_p0, mm_p1);
      const float r = (sid == selected_ship) ? 3.0f : 1.8f;
      ImU32 col = modulate_alpha(color_faction(sh->faction_id), 0.85f);
      if (sid == selected_ship) col = IM_COL32(245, 245, 245, 255);
      draw->AddCircleFilled(p, r, col, 0);

      // Time preview ghost (future ship position). By default this is only drawn
      // for the selected ship / fleet leader unless "All ships" is enabled.
      if (time_preview_active) {
        const bool show_time_preview_ship = (ui.system_map_time_preview_all_ships || sid == selected_ship ||
                                             (selected_fleet != nullptr && selected_fleet->leader_ship_id == sid));
        if (show_time_preview_ship) {
          const Vec2 future_w = sh->position_mkm + sh->velocity_mkm_per_day * time_preview_days;
          ImVec2 pf = world_to_minimap_px(mm, future_w);
          pf = clamp_to_rect(pf, mm_p0, mm_p1);
          const float dx = pf.x - p.x;
          const float dy = pf.y - p.y;
          if ((dx * dx + dy * dy) > 1.0f) {
            draw->AddCircle(pf, r + 1.4f, modulate_alpha(col, 0.70f), 0, 1.0f);
          }
        }
      }
    }

    // Recent-contact markers (optional).
    if (ui.fog_of_war && ui.show_contact_markers && !recent_contacts.empty() && viewer_faction_id != kInvalidId) {
      const int now = static_cast<int>(s.date.days_since_epoch());
      for (const auto& c : recent_contacts) {
        if (c.ship_id == kInvalidId) continue;
        if (sim.is_ship_detected_by_faction(viewer_faction_id, c.ship_id)) continue;
        const auto pred = predict_contact_position(c, now, sim.cfg().contact_prediction_max_days);
        ImVec2 p = world_to_minimap_px(mm, pred.predicted_position_mkm);
        p = clamp_to_rect(p, mm_p0, mm_p1);
        const ImU32 col = IM_COL32(255, 180, 0, 170);
        draw->AddLine(ImVec2(p.x - 3, p.y - 3), ImVec2(p.x + 3, p.y + 3), col, 1.5f);
        draw->AddLine(ImVec2(p.x - 3, p.y + 3), ImVec2(p.x + 3, p.y - 3), col, 1.5f);
      }
    }

    // Viewport rectangle.
    const Vec2 view_tl = to_world(origin, center, scale, zoom, pan);
    const Vec2 view_br = to_world(ImVec2(origin.x + avail.x, origin.y + avail.y), center, scale, zoom, pan);
    const ImVec2 pv0 = world_to_minimap_px(mm, view_tl);
    const ImVec2 pv1 = world_to_minimap_px(mm, view_br);
    ImVec2 a(std::min(pv0.x, pv1.x), std::min(pv0.y, pv1.y));
    ImVec2 b(std::max(pv0.x, pv1.x), std::max(pv0.y, pv1.y));
    a = clamp_to_rect(a, mm_p0, mm_p1);
    b = clamp_to_rect(b, mm_p0, mm_p1);
    draw->AddRectFilled(a, b, IM_COL32(255, 255, 255, 22));
    draw->AddRect(a, b, IM_COL32(255, 255, 255, 170), 0.0f, 0, 1.5f);

    // Label.
    draw->AddText(ImVec2(mm_p0.x + 6.0f, mm_p0.y + 4.0f), IM_COL32(210, 210, 210, 200), "Minimap (M)");
  }

  // Legend / help
  ImGui::SetCursorScreenPos(legend_p0);
  ImGui::BeginChild("legend", legend_size, true);
  ImGui::Text("Controls");
  ImGui::BulletText("Mouse wheel: zoom (to cursor)");
  ImGui::BulletText("Middle drag: pan");
  ImGui::BulletText("Minimap (M): click/drag to pan");
  ImGui::BulletText("Hold D + drag: ruler (distance + ETA)");
  ImGui::BulletText("R: reset view, F: follow selected");
  ImGui::BulletText("T: time preview, [ / ]: adjust days (Shift = 10d)");
  ImGui::BulletText("H: threat heatmap, Shift+H: sensor heatmap");
  ImGui::BulletText("N: nebula overlay (microfields)");
  ImGui::BulletText("G: gravity contours overlay");
  ImGui::BulletText("S: storm cells overlay");
  ImGui::BulletText("W: flow field overlay");
  ImGui::BulletText("B: dust particles");
  ImGui::BulletText("Left click: issue order to ship (Shift queues)");
  ImGui::BulletText("Right click: select ship/body (no orders)");
  ImGui::BulletText("Alt+Left click body: colonize (colony ship required)");
  ImGui::BulletText("Ctrl+Left click: issue order to fleet");
  ImGui::BulletText("Click body: move-to-body");
  ImGui::BulletText("Click jump point: travel via jump");
  ImGui::BulletText("Jump points are purple rings");
  ImGui::BulletText("Alt + click a jump point to Survey it (no transit)");

  ImGui::SeparatorText("Map overlays");
  ImGui::Checkbox("Starfield", &ui.system_map_starfield);
  ImGui::SameLine();
  ImGui::Checkbox("Grid", &ui.system_map_grid);
  ImGui::SameLine();
  ImGui::Checkbox("Dust (B)", &ui.system_map_particle_field);

  ImGui::SameLine();
  ImGui::Checkbox("Nebula (N)", &ui.system_map_nebula_microfield_overlay);

  ImGui::SameLine();
  ImGui::Checkbox("Gravity (G)", &ui.system_map_gravity_contours_overlay);

  // Custom tile-based procedural background engine (stars + optional haze).
  ImGui::Checkbox("Procedural background (tiles)", &ui.map_proc_render_engine);
  if (ui.map_proc_render_engine) {
    ImGui::Indent();
    ui.map_proc_render_tile_px = std::clamp(ui.map_proc_render_tile_px, 64, 512);
    ui.map_proc_render_cache_tiles = std::clamp(ui.map_proc_render_cache_tiles, 16, 512);
    ui.map_proc_render_nebula_strength = std::clamp(ui.map_proc_render_nebula_strength, 0.0f, 1.0f);
    ui.map_proc_render_nebula_scale = std::clamp(ui.map_proc_render_nebula_scale, 0.25f, 4.0f);
    ui.map_proc_render_nebula_warp = std::clamp(ui.map_proc_render_nebula_warp, 0.0f, 2.0f);

    ImGui::SliderInt("Tile size (px)##proc_bg", &ui.map_proc_render_tile_px, 64, 512);
    ImGui::SliderInt("Cache limit (tiles)##proc_bg", &ui.map_proc_render_cache_tiles, 16, 512);
    ImGui::Checkbox("Nebula haze##proc_bg", &ui.map_proc_render_nebula_enable);
    if (ui.map_proc_render_nebula_enable) {
      ImGui::SliderFloat("Strength##proc_bg", &ui.map_proc_render_nebula_strength, 0.0f, 1.0f, "%.2f");
      ImGui::SliderFloat("Scale##proc_bg", &ui.map_proc_render_nebula_scale, 0.25f, 4.0f, "%.2f");
      ImGui::SliderFloat("Warp##proc_bg", &ui.map_proc_render_nebula_warp, 0.0f, 2.0f, "%.2f");
    }
    ImGui::Checkbox("Debug tile bounds##proc_bg", &ui.map_proc_render_debug_tiles);

    const ProcRenderStats* pst = nullptr;
    ProcRenderStats st_local;
    if (proc_engine && proc_engine->ready()) {
      st_local = proc_engine->stats();
      pst = &st_local;
    }

    if (ImGui::SmallButton("Clear cache##proc_bg")) {
      if (proc_engine && proc_engine->ready()) {
        proc_engine->clear();
      } else {
        ui.map_proc_render_clear_cache_requested = true;
      }
    }
    ImGui::SameLine();
    const int cached = pst ? pst->cache_tiles : ui.map_proc_render_stats_cache_tiles;
    const int gen = pst ? pst->generated_this_frame : ui.map_proc_render_stats_generated_this_frame;
    const float gen_ms = pst ? static_cast<float>(pst->gen_ms_this_frame) : ui.map_proc_render_stats_gen_ms_this_frame;
    const float up_ms = pst ? static_cast<float>(pst->upload_ms_this_frame) : ui.map_proc_render_stats_upload_ms_this_frame;
    ImGui::TextDisabled("Cached %d | +%d | Gen %.2fms | Upload %.2fms", cached, gen, gen_ms, up_ms);
    ImGui::Unindent();
  }

  ImGui::Separator();
  ImGui::TextDisabled("Procedural particle field (dust)");
  ImGui::Checkbox("System map##particle_field_sys", &ui.system_map_particle_field);
  ImGui::SameLine();
  ImGui::Checkbox("Galaxy map##particle_field_gal", &ui.galaxy_map_particle_field);

  const bool pf_enabled = ui.system_map_particle_field || ui.galaxy_map_particle_field;
  if (pf_enabled) {
    // Clamp to reasonable ranges.
    ui.map_particle_tile_px = std::clamp(ui.map_particle_tile_px, 64, 512);
    ui.map_particle_particles_per_tile = std::clamp(ui.map_particle_particles_per_tile, 8, 512);
    ui.map_particle_layers = std::clamp(ui.map_particle_layers, 1, 3);
    ui.map_particle_opacity = std::clamp(ui.map_particle_opacity, 0.0f, 1.0f);
    ui.map_particle_base_radius_px = std::clamp(ui.map_particle_base_radius_px, 0.5f, 3.0f);
    ui.map_particle_radius_jitter_px = std::clamp(ui.map_particle_radius_jitter_px, 0.0f, 4.0f);
    ui.map_particle_twinkle_strength = std::clamp(ui.map_particle_twinkle_strength, 0.0f, 1.0f);
    ui.map_particle_twinkle_speed = std::clamp(ui.map_particle_twinkle_speed, 0.0f, 4.0f);
    ui.map_particle_drift_px_per_day = std::clamp(ui.map_particle_drift_px_per_day, 0.0f, 100.0f);
    ui.map_particle_layer0_parallax = std::clamp(ui.map_particle_layer0_parallax, 0.0f, 1.0f);
    ui.map_particle_layer1_parallax = std::clamp(ui.map_particle_layer1_parallax, 0.0f, 1.0f);
    ui.map_particle_layer2_parallax = std::clamp(ui.map_particle_layer2_parallax, 0.0f, 1.0f);
    ui.map_particle_sparkle_chance = std::clamp(ui.map_particle_sparkle_chance, 0.0f, 0.5f);
    ui.map_particle_sparkle_length_px = std::clamp(ui.map_particle_sparkle_length_px, 1.0f, 24.0f);

    ImGui::Indent();
    ImGui::SliderFloat("Opacity##particle_field", &ui.map_particle_opacity, 0.0f, 1.0f, "%.2f");
    ImGui::SliderInt("Tile size (px)##particle_field", &ui.map_particle_tile_px, 64, 512);
    ImGui::SliderInt("Particles per tile##particle_field", &ui.map_particle_particles_per_tile, 8, 512);
    ImGui::SliderInt("Layers##particle_field", &ui.map_particle_layers, 1, 3);

    ImGui::SliderFloat("Base radius (px)##particle_field", &ui.map_particle_base_radius_px, 0.5f, 3.0f, "%.2f");
    ImGui::SliderFloat("Radius jitter (px)##particle_field", &ui.map_particle_radius_jitter_px, 0.0f, 4.0f, "%.2f");

    ImGui::SliderFloat("Twinkle strength##particle_field", &ui.map_particle_twinkle_strength, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Twinkle speed##particle_field", &ui.map_particle_twinkle_speed, 0.0f, 4.0f, "%.2f");

    ImGui::Checkbox("Drift (simulation-time)##particle_field", &ui.map_particle_drift);
    if (ui.map_particle_drift) {
      ImGui::SliderFloat("Drift speed (px/day)##particle_field", &ui.map_particle_drift_px_per_day, 0.0f, 100.0f, "%.1f");
    }

    if (ui.map_particle_layers >= 1) {
      ImGui::SliderFloat("Layer 0 parallax##particle_field", &ui.map_particle_layer0_parallax, 0.0f, 1.0f, "%.2f");
    }
    if (ui.map_particle_layers >= 2) {
      ImGui::SliderFloat("Layer 1 parallax##particle_field", &ui.map_particle_layer1_parallax, 0.0f, 1.0f, "%.2f");
    }
    if (ui.map_particle_layers >= 3) {
      ImGui::SliderFloat("Layer 2 parallax##particle_field", &ui.map_particle_layer2_parallax, 0.0f, 1.0f, "%.2f");
    }

    ImGui::Checkbox("Sparkles##particle_field", &ui.map_particle_sparkles);
    if (ui.map_particle_sparkles) {
      ImGui::SliderFloat("Chance##particle_field", &ui.map_particle_sparkle_chance, 0.0f, 0.5f, "%.3f");
      ImGui::SliderFloat("Length (px)##particle_field", &ui.map_particle_sparkle_length_px, 1.0f, 24.0f, "%.1f");
    }

    ImGui::Checkbox("Debug tile bounds##particle_field", &ui.map_particle_debug_tiles);

    ImGui::TextDisabled("Last frame: L%d | Tiles %d | Particles %d",
                        ui.map_particle_last_frame_layers_drawn,
                        ui.map_particle_last_frame_tiles_drawn,
                        ui.map_particle_last_frame_particles_drawn);
    ImGui::Unindent();
  }

  ImGui::Separator();
  ImGui::TextDisabled("Procedural body sprites (system map)");
  if (!body_sprites) {
    ImGui::TextDisabled("(Engine unavailable in this build)");
  } else {
    ImGui::Checkbox("Enable##body_sprites", &ui.system_map_body_sprites);
    if (ui.system_map_body_sprites) {
      ui.system_map_body_sprite_px = std::clamp(ui.system_map_body_sprite_px, 32, 256);
      ui.system_map_body_sprite_cache = std::clamp(ui.system_map_body_sprite_cache, 64, 2048);
      ui.system_map_body_sprite_light_steps = std::clamp(ui.system_map_body_sprite_light_steps, 4, 128);
      ui.system_map_body_sprite_ring_chance = std::clamp(ui.system_map_body_sprite_ring_chance, 0.0f, 1.0f);
      ui.system_map_body_sprite_ambient = std::clamp(ui.system_map_body_sprite_ambient, 0.0f, 1.0f);
      ui.system_map_body_sprite_diffuse = std::clamp(ui.system_map_body_sprite_diffuse, 0.0f, 2.0f);
      ui.system_map_body_sprite_specular = std::clamp(ui.system_map_body_sprite_specular, 0.0f, 2.0f);
      ui.system_map_body_sprite_specular_power = std::clamp(ui.system_map_body_sprite_specular_power, 1.0f, 128.0f);

      ImGui::Indent();
      ImGui::SliderInt("Sprite resolution##body_sprites", &ui.system_map_body_sprite_px, 32, 256);
      ImGui::SliderInt("Cache limit##body_sprites", &ui.system_map_body_sprite_cache, 64, 2048);
      ImGui::SliderInt("Light steps##body_sprites", &ui.system_map_body_sprite_light_steps, 4, 128);
      ImGui::Checkbox("Rings on gas giants##body_sprites", &ui.system_map_body_sprite_rings);
      if (ui.system_map_body_sprite_rings) {
        ImGui::SliderFloat("Ring chance##body_sprites", &ui.system_map_body_sprite_ring_chance, 0.0f, 1.0f, "%.2f");
      }
      ImGui::SliderFloat("Ambient##body_sprites", &ui.system_map_body_sprite_ambient, 0.0f, 1.0f, "%.2f");
      ImGui::SliderFloat("Diffuse##body_sprites", &ui.system_map_body_sprite_diffuse, 0.0f, 2.0f, "%.2f");
      ImGui::SliderFloat("Specular##body_sprites", &ui.system_map_body_sprite_specular, 0.0f, 2.0f, "%.2f");
      ImGui::SliderFloat("Specular power##body_sprites", &ui.system_map_body_sprite_specular_power, 1.0f, 128.0f, "%.1f");
      if (ImGui::Button("Clear sprite cache##body_sprites")) {
        ui.system_map_body_sprite_clear_cache_requested = true;
      }
      const auto& st = body_sprites->stats();
      ImGui::TextDisabled("Cached %d | +%d | Gen %.2fms | Upload %.2fms",
                          st.cache_sprites,
                          st.generated_this_frame,
                          st.gen_ms_this_frame,
                          st.upload_ms_this_frame);
      ImGui::Unindent();
    }
  }

  ImGui::Separator();
  ImGui::TextDisabled("Procedural contact icons (system map)");
  if (!icon_sprites) {
    ImGui::TextDisabled("(Engine unavailable in this build)");
  } else {
    ImGui::Checkbox("Enable##contact_icons", &ui.system_map_contact_icons);
    if (ui.system_map_contact_icons) {
      ui.system_map_contact_icon_px = std::clamp(ui.system_map_contact_icon_px, 16, 256);
      ui.system_map_contact_icon_cache = std::clamp(ui.system_map_contact_icon_cache, 64, 4096);
      ui.system_map_ship_icon_size_px = std::clamp(ui.system_map_ship_icon_size_px, 8.0f, 64.0f);
      ui.system_map_missile_icon_size_px = std::clamp(ui.system_map_missile_icon_size_px, 6.0f, 48.0f);
      ui.system_map_wreck_icon_size_px = std::clamp(ui.system_map_wreck_icon_size_px, 8.0f, 64.0f);
      ui.system_map_anomaly_icon_size_px = std::clamp(ui.system_map_anomaly_icon_size_px, 8.0f, 64.0f);
      ui.system_map_ship_icon_thruster_opacity = std::clamp(ui.system_map_ship_icon_thruster_opacity, 0.0f, 1.0f);
      ui.system_map_ship_icon_thruster_length_px = std::clamp(ui.system_map_ship_icon_thruster_length_px, 0.0f, 64.0f);
      ui.system_map_ship_icon_thruster_width_px = std::clamp(ui.system_map_ship_icon_thruster_width_px, 0.0f, 32.0f);

      ImGui::Indent();
      ImGui::SliderInt("Sprite resolution##contact_icons", &ui.system_map_contact_icon_px, 16, 256);
      ImGui::SliderInt("Cache limit##contact_icons", &ui.system_map_contact_icon_cache, 64, 4096);
      ImGui::SliderFloat("Ship size (px)##contact_icons", &ui.system_map_ship_icon_size_px, 8.0f, 64.0f, "%.0f");
      ImGui::SliderFloat("Missile size (px)##contact_icons", &ui.system_map_missile_icon_size_px, 6.0f, 48.0f, "%.0f");
      ImGui::SliderFloat("Wreck size (px)##contact_icons", &ui.system_map_wreck_icon_size_px, 8.0f, 64.0f, "%.0f");
      ImGui::SliderFloat("Anomaly size (px)##contact_icons", &ui.system_map_anomaly_icon_size_px, 8.0f, 64.0f, "%.0f");
      ImGui::Checkbox("Thrusters##contact_icons", &ui.system_map_ship_icon_thrusters);
      if (ui.system_map_ship_icon_thrusters) {
        ImGui::SliderFloat("Thruster opacity##contact_icons", &ui.system_map_ship_icon_thruster_opacity, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Thruster length##contact_icons", &ui.system_map_ship_icon_thruster_length_px, 0.0f, 64.0f, "%.0fpx");
        ImGui::SliderFloat("Thruster width##contact_icons", &ui.system_map_ship_icon_thruster_width_px, 0.0f, 32.0f, "%.0fpx");
      }
      ImGui::Checkbox("Anomaly pulse##contact_icons", &ui.system_map_anomaly_icon_pulse);
      ImGui::Checkbox("Debug bounds##contact_icons", &ui.system_map_contact_icon_debug_bounds);
      if (ImGui::Button("Clear icon cache##contact_icons")) {
        ui.system_map_contact_icon_clear_cache_requested = true;
      }
      ImGui::TextDisabled("Cached %d | +%d | Gen %.2fms | Upload %.2fms",
                          ui.system_map_contact_icon_stats_cache_sprites,
                          ui.system_map_contact_icon_stats_generated_this_frame,
                          ui.system_map_contact_icon_stats_gen_ms_this_frame,
                          ui.system_map_contact_icon_stats_upload_ms_this_frame);
      if (!icon_sprites->ready()) {
        ImGui::TextDisabled("(Renderer backend does not support icon textures)");
      }
      ImGui::Unindent();
    }
  }

  ImGui::Separator();
  ImGui::TextDisabled("Jump phenomena (procedural, system map) (J)");
  if (!jump_fx) {
    ImGui::TextDisabled("(Engine unavailable in this build)");
  } else {
    ImGui::Checkbox("Enable##jump_phenomena", &ui.system_map_jump_phenomena);
    if (ui.system_map_jump_phenomena) {
      ui.system_map_jump_phenomena_sprite_px = std::clamp(ui.system_map_jump_phenomena_sprite_px, 16, 256);
      ui.system_map_jump_phenomena_cache = std::clamp(ui.system_map_jump_phenomena_cache, 8, 2048);
      ui.system_map_jump_phenomena_size_mult = std::clamp(ui.system_map_jump_phenomena_size_mult, 1.0f, 16.0f);
      ui.system_map_jump_phenomena_opacity = std::clamp(ui.system_map_jump_phenomena_opacity, 0.0f, 1.0f);
      ui.system_map_jump_phenomena_anim_speed_cycles_per_day =
          std::clamp(ui.system_map_jump_phenomena_anim_speed_cycles_per_day, 0.0f, 4.0f);
      ui.system_map_jump_phenomena_pulse_cycles_per_day =
          std::clamp(ui.system_map_jump_phenomena_pulse_cycles_per_day, 0.0f, 4.0f);
      ui.system_map_jump_phenomena_filament_strength = std::clamp(ui.system_map_jump_phenomena_filament_strength, 0.0f, 4.0f);
      ui.system_map_jump_phenomena_filaments_max = std::clamp(ui.system_map_jump_phenomena_filaments_max, 0, 48);

      ImGui::Indent();
      ImGui::SliderInt("Sprite resolution##jump_phenomena", &ui.system_map_jump_phenomena_sprite_px, 16, 256);
      ImGui::SliderInt("Cache limit##jump_phenomena", &ui.system_map_jump_phenomena_cache, 8, 2048);
      ImGui::SliderFloat("Size (× glyph)##jump_phenomena", &ui.system_map_jump_phenomena_size_mult, 1.0f, 16.0f, "%.2f");
      ImGui::SliderFloat("Opacity##jump_phenomena", &ui.system_map_jump_phenomena_opacity, 0.0f, 1.0f, "%.2f");
      ImGui::Checkbox("Reveal unsurveyed (spoilers)##jump_phenomena", &ui.system_map_jump_phenomena_reveal_unsurveyed);
      ImGui::Checkbox("Animate rotation##jump_phenomena", &ui.system_map_jump_phenomena_animate);
      if (ui.system_map_jump_phenomena_animate) {
        ImGui::SliderFloat("Rotation speed (cycles/day)##jump_phenomena", &ui.system_map_jump_phenomena_anim_speed_cycles_per_day, 0.0f, 2.0f, "%.2f");
      }
      ImGui::Checkbox("Pulse##jump_phenomena", &ui.system_map_jump_phenomena_pulse);
      if (ui.system_map_jump_phenomena_pulse) {
        ImGui::SliderFloat("Pulse speed (cycles/day)##jump_phenomena", &ui.system_map_jump_phenomena_pulse_cycles_per_day, 0.0f, 2.0f, "%.2f");
      }
      ImGui::Checkbox("Filaments##jump_phenomena", &ui.system_map_jump_phenomena_filaments);
      if (ui.system_map_jump_phenomena_filaments) {
        ImGui::SliderFloat("Filament strength##jump_phenomena", &ui.system_map_jump_phenomena_filament_strength, 0.0f, 4.0f, "%.2f");
        ImGui::SliderInt("Filaments (max)##jump_phenomena", &ui.system_map_jump_phenomena_filaments_max, 0, 48);
      }
      ImGui::Checkbox("Debug bounds##jump_phenomena", &ui.system_map_jump_phenomena_debug_bounds);
      if (ImGui::Button("Clear jump cache##jump_phenomena")) {
        ui.system_map_jump_phenomena_clear_cache_requested = true;
      }
      ImGui::TextDisabled("Cached %d | +%d | Gen %.2fms | Upload %.2fms",
                          ui.system_map_jump_phenomena_stats_cache_sprites,
                          ui.system_map_jump_phenomena_stats_generated_this_frame,
                          ui.system_map_jump_phenomena_stats_gen_ms_this_frame,
                          ui.system_map_jump_phenomena_stats_upload_ms_this_frame);
      if (!jump_fx->ready()) {
        ImGui::TextDisabled("(Renderer backend does not support jump textures)");
      }
      ImGui::Unindent();
    }
  }

  ImGui::Separator();
  ImGui::TextDisabled("Anomaly phenomena (procedural, system map) (A)");
  if (!anomaly_fx) {
    ImGui::TextDisabled("(Engine unavailable in this build)");
  } else {
    ImGui::Checkbox("Enable##anomaly_phenomena", &ui.system_map_anomaly_phenomena);
    if (ui.system_map_anomaly_phenomena) {
      ui.system_map_anomaly_phenomena_sprite_px = std::clamp(ui.system_map_anomaly_phenomena_sprite_px, 16, 256);
      ui.system_map_anomaly_phenomena_cache = std::clamp(ui.system_map_anomaly_phenomena_cache, 8, 2048);
      ui.system_map_anomaly_phenomena_size_mult = std::clamp(ui.system_map_anomaly_phenomena_size_mult, 1.0f, 16.0f);
      ui.system_map_anomaly_phenomena_opacity = std::clamp(ui.system_map_anomaly_phenomena_opacity, 0.0f, 1.0f);
      ui.system_map_anomaly_phenomena_anim_speed_cycles_per_day =
          std::clamp(ui.system_map_anomaly_phenomena_anim_speed_cycles_per_day, 0.0f, 4.0f);
      ui.system_map_anomaly_phenomena_pulse_cycles_per_day =
          std::clamp(ui.system_map_anomaly_phenomena_pulse_cycles_per_day, 0.0f, 4.0f);
      ui.system_map_anomaly_phenomena_filament_strength = std::clamp(ui.system_map_anomaly_phenomena_filament_strength, 0.0f, 4.0f);
      ui.system_map_anomaly_phenomena_filaments_max = std::clamp(ui.system_map_anomaly_phenomena_filaments_max, 0, 48);
      ui.system_map_anomaly_phenomena_glyph_strength = std::clamp(ui.system_map_anomaly_phenomena_glyph_strength, 0.0f, 2.0f);

      ImGui::Indent();
      ImGui::SliderInt("Sprite resolution##anomaly_phenomena", &ui.system_map_anomaly_phenomena_sprite_px, 16, 256);
      ImGui::SliderInt("Cache limit##anomaly_phenomena", &ui.system_map_anomaly_phenomena_cache, 8, 2048);
      ImGui::SliderFloat("Size (× glyph)##anomaly_phenomena", &ui.system_map_anomaly_phenomena_size_mult, 1.0f, 16.0f, "%.2f");
      ImGui::SliderFloat("Opacity##anomaly_phenomena", &ui.system_map_anomaly_phenomena_opacity, 0.0f, 1.0f, "%.2f");
      ImGui::Checkbox("Animate rotation##anomaly_phenomena", &ui.system_map_anomaly_phenomena_animate);
      if (ui.system_map_anomaly_phenomena_animate) {
        ImGui::SliderFloat("Rotation speed (cycles/day)##anomaly_phenomena", &ui.system_map_anomaly_phenomena_anim_speed_cycles_per_day, 0.0f, 2.0f, "%.2f");
      }
      ImGui::Checkbox("Pulse##anomaly_phenomena", &ui.system_map_anomaly_phenomena_pulse);
      if (ui.system_map_anomaly_phenomena_pulse) {
        ImGui::SliderFloat("Pulse speed (cycles/day)##anomaly_phenomena", &ui.system_map_anomaly_phenomena_pulse_cycles_per_day, 0.0f, 2.0f, "%.2f");
      }
      ImGui::Checkbox("Filaments##anomaly_phenomena", &ui.system_map_anomaly_phenomena_filaments);
      if (ui.system_map_anomaly_phenomena_filaments) {
        ImGui::SliderFloat("Filament strength##anomaly_phenomena", &ui.system_map_anomaly_phenomena_filament_strength, 0.0f, 4.0f, "%.2f");
        ImGui::SliderInt("Filaments (max)##anomaly_phenomena", &ui.system_map_anomaly_phenomena_filaments_max, 0, 48);
      }
      ImGui::Checkbox("Glyph overlay##anomaly_phenomena", &ui.system_map_anomaly_phenomena_glyph_overlay);
      if (ui.system_map_anomaly_phenomena_glyph_overlay) {
        ImGui::SliderFloat("Glyph strength##anomaly_phenomena", &ui.system_map_anomaly_phenomena_glyph_strength, 0.0f, 1.5f, "%.2f");
      }
      ImGui::Checkbox("Debug bounds##anomaly_phenomena", &ui.system_map_anomaly_phenomena_debug_bounds);
      if (ImGui::Button("Clear anomaly cache##anomaly_phenomena")) {
        ui.system_map_anomaly_phenomena_clear_cache_requested = true;
      }
      ImGui::TextDisabled("Cached %d | +%d | Gen %.2fms | Upload %.2fms",
                          ui.system_map_anomaly_phenomena_stats_cache_sprites,
                          ui.system_map_anomaly_phenomena_stats_generated_this_frame,
                          ui.system_map_anomaly_phenomena_stats_gen_ms_this_frame,
                          ui.system_map_anomaly_phenomena_stats_upload_ms_this_frame);
      if (!anomaly_fx->ready()) {
        ImGui::TextDisabled("(Renderer backend does not support anomaly textures)");
      }
      ImGui::Unindent();
    }
  }

  ImGui::Separator();
  ImGui::TextDisabled("Motion trails (procedural, system map)");
  if (!trail_engine) {
    ImGui::TextDisabled("(Engine unavailable in this build)");
  } else {
    ImGui::Checkbox("Enable##motion_trails", &ui.system_map_motion_trails);
    if (ui.system_map_motion_trails) {
      ui.system_map_motion_trails_max_age_days = std::clamp(ui.system_map_motion_trails_max_age_days, 0.25f, 60.0f);
      ui.system_map_motion_trails_sample_hours = std::clamp(ui.system_map_motion_trails_sample_hours, 0.05f, 72.0f);
      ui.system_map_motion_trails_min_seg_px = std::clamp(ui.system_map_motion_trails_min_seg_px, 0.5f, 64.0f);
      ui.system_map_motion_trails_thickness_px = std::clamp(ui.system_map_motion_trails_thickness_px, 0.5f, 12.0f);
      ui.system_map_motion_trails_alpha = std::clamp(ui.system_map_motion_trails_alpha, 0.0f, 1.0f);

      ImGui::Indent();
      ImGui::Checkbox("All visible ships##motion_trails", &ui.system_map_motion_trails_all_ships);
      ImGui::SameLine();
      ImGui::Checkbox("Missile trails##motion_trails", &ui.system_map_motion_trails_missiles);
      ImGui::Checkbox("Speed brightening##motion_trails", &ui.system_map_motion_trails_speed_brighten);
      ImGui::SliderFloat("Retention (days)##motion_trails", &ui.system_map_motion_trails_max_age_days, 0.25f, 60.0f, "%.2f");
      ImGui::SliderFloat("Sample interval (hours)##motion_trails", &ui.system_map_motion_trails_sample_hours, 0.05f, 24.0f, "%.2f");
      ImGui::SliderFloat("Min segment (px)##motion_trails", &ui.system_map_motion_trails_min_seg_px, 0.5f, 24.0f, "%.1f");
      ImGui::SliderFloat("Thickness (px)##motion_trails", &ui.system_map_motion_trails_thickness_px, 0.5f, 8.0f, "%.1f");
      ImGui::SliderFloat("Opacity##motion_trails", &ui.system_map_motion_trails_alpha, 0.0f, 1.0f, "%.2f");
      if (ImGui::Button("Clear trails##motion_trails")) {
        ui.system_map_motion_trails_clear_requested = true;
      }
      ImGui::TextDisabled("Systems %d | Tracks %d | Points %d | Pruned %d pts / %d tracks",
                          ui.system_map_motion_trails_stats_systems,
                          ui.system_map_motion_trails_stats_tracks,
                          ui.system_map_motion_trails_stats_points,
                          ui.system_map_motion_trails_stats_pruned_points_this_frame,
                          ui.system_map_motion_trails_stats_pruned_tracks_this_frame);
      ImGui::Unindent();
    }
  }
  if (ui.system_map_nebula_microfield_overlay) {
    ui.system_map_nebula_overlay_opacity = std::clamp(ui.system_map_nebula_overlay_opacity, 0.0f, 1.0f);
    ui.system_map_nebula_overlay_resolution = std::clamp(ui.system_map_nebula_overlay_resolution, 16, 260);
    ImGui::SliderFloat("Opacity##nebula_overlay", &ui.system_map_nebula_overlay_opacity, 0.0f, 1.0f, "%.2f");
    ImGui::SliderInt("Resolution##nebula_overlay", &ui.system_map_nebula_overlay_resolution, 16, 260);
  }
  ImGui::Checkbox("Storm cells (S)", &ui.system_map_storm_cell_overlay);
  if (ui.system_map_storm_cell_overlay) {
    ui.system_map_storm_overlay_opacity = std::clamp(ui.system_map_storm_overlay_opacity, 0.0f, 1.0f);
    ui.system_map_storm_overlay_resolution = std::clamp(ui.system_map_storm_overlay_resolution, 16, 260);
    ImGui::SliderFloat("Opacity##storm_overlay", &ui.system_map_storm_overlay_opacity, 0.0f, 1.0f, "%.2f");
    ImGui::SliderInt("Resolution##storm_overlay", &ui.system_map_storm_overlay_resolution, 16, 260);
  }
  ImGui::Checkbox("Flow field (W)", &ui.system_map_flow_field_overlay);
  if (ui.system_map_flow_field_overlay) {
    ui.system_map_flow_field_opacity = std::clamp(ui.system_map_flow_field_opacity, 0.0f, 1.0f);
    ui.system_map_flow_field_thickness_px = std::clamp(ui.system_map_flow_field_thickness_px, 0.5f, 6.0f);
    ui.system_map_flow_field_step_px = std::clamp(ui.system_map_flow_field_step_px, 1.0f, 24.0f);
    ui.system_map_flow_field_highlight_wavelength_px = std::clamp(ui.system_map_flow_field_highlight_wavelength_px, 32.0f, 800.0f);
    ui.system_map_flow_field_animate_speed_cycles_per_day = std::clamp(ui.system_map_flow_field_animate_speed_cycles_per_day, 0.0f, 2.0f);

    ImGui::Indent();
    ImGui::Checkbox("Animate##flowfield", &ui.system_map_flow_field_animate);
    ImGui::SameLine();
    ImGui::Checkbox("Mask nebula##flowfield", &ui.system_map_flow_field_mask_nebula);
    ImGui::SameLine();
    ImGui::Checkbox("Mask storms##flowfield", &ui.system_map_flow_field_mask_storms);
    ImGui::SliderFloat("Opacity##flowfield", &ui.system_map_flow_field_opacity, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Thickness (px)##flowfield", &ui.system_map_flow_field_thickness_px, 0.5f, 6.0f, "%.2f");
    ImGui::SliderFloat("Step (px)##flowfield", &ui.system_map_flow_field_step_px, 1.0f, 24.0f, "%.1f");
    ImGui::SliderFloat("Speed (cycles/day)##flowfield", &ui.system_map_flow_field_animate_speed_cycles_per_day, 0.0f, 2.0f, "%.2f");
    if (ImGui::Button("Clear cache##flowfield")) {
      ui.system_map_flow_field_clear_requested = true;
    }
    ImGui::TextDisabled("Tiles %d | Used %d | +%d | Seg %d",
                        ui.system_map_flow_field_stats_cache_tiles,
                        ui.system_map_flow_field_stats_tiles_used,
                        ui.system_map_flow_field_stats_tiles_generated,
                        ui.system_map_flow_field_stats_segments_drawn);
    ImGui::Unindent();
  }

  ImGui::Checkbox("Minimap (M)", &ui.system_map_show_minimap);
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
    const bool want_env_readout = (ui.system_map_nebula_microfield_overlay || ui.system_map_storm_cell_overlay);
    if (want_env_readout) {
      const double d = sim.system_nebula_density_at(sys->id, w);
      const double st = (sim.cfg().enable_nebula_storms ? sim.system_storm_intensity_at(sys->id, w) : 0.0);
      if (d > 1e-6 || st > 1e-6) {
        const double env = sim.system_sensor_environment_multiplier_at(sys->id, w);
        const double speed = sim.system_movement_speed_multiplier_at(sys->id, w);
        if (st > 1e-6 && d > 1e-6) {
          ImGui::TextDisabled("Nebula here: %.0f%%  Storm here: %.0f%%  (Sensors x%.2f  Speed x%.2f)",
                              d * 100.0,
                              st * 100.0,
                              env,
                              speed);
        } else if (st > 1e-6) {
          ImGui::TextDisabled("Storm here: %.0f%%  (Sensors x%.2f  Speed x%.2f)", st * 100.0, env, speed);
        } else {
          ImGui::TextDisabled("Nebula here: %.0f%%  (Sensors x%.2f  Speed x%.2f)", d * 100.0, env, speed);
        }
      }
    }
  }

  ImGui::SeparatorText("Time preview (T)");
  ImGui::Checkbox("Enable##timeprev", &ui.system_map_time_preview);
  ImGui::SameLine();
  ImGui::Checkbox("Vectors##timeprev", &ui.system_map_time_preview_vectors);
  ImGui::SameLine();
  ImGui::Checkbox("Trails##timeprev", &ui.system_map_time_preview_trails);
  ImGui::Checkbox("All ships##timeprev", &ui.system_map_time_preview_all_ships);

  ImGui::SliderFloat("Days##timeprev", &ui.system_map_time_preview_days, -365.0f, 365.0f, "%.0f");

  if (ImGui::SmallButton("Now##timeprev")) ui.system_map_time_preview_days = 0.0f;
  ImGui::SameLine();
  if (ImGui::SmallButton("+7##timeprev")) ui.system_map_time_preview_days = 7.0f;
  ImGui::SameLine();
  if (ImGui::SmallButton("+30##timeprev")) ui.system_map_time_preview_days = 30.0f;
  ImGui::SameLine();
  if (ImGui::SmallButton("+180##timeprev")) ui.system_map_time_preview_days = 180.0f;

  {
    const int dd = static_cast<int>(std::round(ui.system_map_time_preview_days));
    ImGui::TextDisabled("Target: %s",
                        nebula4x::format_datetime(s.date.days_since_epoch() + static_cast<std::int64_t>(dd),
                                                  s.hour_of_day)
                            .c_str());
    ImGui::TextDisabled("Bodies: Kepler orbit prediction. Ships: inertial extrapolation.");
  }


  ImGui::SeparatorText("Ruler (hold D)");
  ImGui::TextDisabled("Snaps to visible bodies/ships/jumps. D + Right click clears.");
  if (ruler.active()) {
    const double dist_mkm = (ruler.end - ruler.start).length();
    const double au = dist_mkm / 149597.8707;

    double speed_km_s = 0.0;
    if (selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(s.ships, selected_ship)) speed_km_s = sh->speed_km_s;
    } else if (selected_fleet && !selected_fleet->ship_ids.empty()) {
      double slowest = std::numeric_limits<double>::infinity();
      for (Id sid : selected_fleet->ship_ids) {
        if (const auto* sh = find_ptr(s.ships, sid)) slowest = std::min(slowest, sh->speed_km_s);
      }
      if (std::isfinite(slowest)) speed_km_s = slowest;
    }
    const double mkm_per_day = speed_km_s * sim.cfg().seconds_per_day / 1e6;

    ImGui::Text("Δ %.1f mkm  (%.3f AU)", dist_mkm, au);
    if (mkm_per_day > 1e-9) {
      const double eta_days = dist_mkm / mkm_per_day;
      ImGui::TextDisabled("@ %.0f km/s → %s", speed_km_s, format_duration_days(eta_days).c_str());
    } else {
      ImGui::TextDisabled("(Select a ship/fleet for ETA)");
    }

    if (ImGui::SmallButton("Clear ruler")) {
      clear_ruler(ruler);
    }
  } else {
    ImGui::TextDisabled("No measurement");
  }

  ImGui::Separator();
  ImGui::Checkbox("Fog of war", &ui.fog_of_war);
  ImGui::Checkbox("Show sensor range", &ui.show_selected_sensor_range);
  ImGui::Checkbox("Sensor coverage (faction)", &ui.show_faction_sensor_coverage);
  if (ui.show_faction_sensor_coverage) {
    ImGui::SameLine();
    ImGui::Checkbox("Fill##sensor_cov", &ui.faction_sensor_coverage_fill);

    const float max_sig = static_cast<float>(
        std::max(0.05, sim_sensors::max_signature_multiplier_for_detection(sim)));
    ImGui::SliderFloat("Target signature##sensor_cov", &ui.faction_sensor_coverage_signature, 0.05f, max_sig,
                      "%.2f");
    ImGui::InputInt("Max sources##sensor_cov", &ui.faction_sensor_coverage_max_sources);
    ui.faction_sensor_coverage_max_sources =
        std::clamp(ui.faction_sensor_coverage_max_sources, 1, 4096);

    ImGui::TextDisabled("Sources: %d (drawn %d)", static_cast<int>(sensor_sources.size()),
                        sensor_sources_drawn);

    const Vec2 w = to_world(mouse, center, scale, zoom, pan);
    const double sig = std::clamp<double>(
        static_cast<double>(ui.faction_sensor_coverage_signature), 0.05,
        std::max(0.05, sim_sensors::max_signature_multiplier_for_detection(sim)));
    if (viewer_faction_id != kInvalidId && !sensor_sources.empty()) {
      const bool det = sim_sensors::any_source_detects(sensor_sources, w, sig);
      ImGui::TextColored(det ? ImVec4(0.3f, 0.95f, 0.45f, 1.0f)
                          : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                         "Detect @cursor (sig %.2f): %s", sig, det ? "YES" : "NO");
    } else if (viewer_faction_id == kInvalidId) {
      ImGui::TextDisabled("Select a ship to define view faction");
    }
  }
  ImGui::SeparatorText("Heatmaps (H)");
  ImGui::Checkbox("Threat##heatmap", &ui.system_map_threat_heatmap);
  ImGui::SameLine();
  ImGui::Checkbox("Sensor##heatmap", &ui.system_map_sensor_heatmap);

  if (ui.system_map_sensor_heatmap) {
    ImGui::SameLine();
    ImGui::Checkbox("LOS raytrace##heatmap", &ui.system_map_sensor_heatmap_raytrace);
    if (ui.system_map_sensor_heatmap_raytrace) {
      ImGui::TextDisabled("Experimental LOS shading (visual-only)");

      ui.system_map_sensor_raytrace_los_strength =
          std::clamp(ui.system_map_sensor_raytrace_los_strength, 0.0f, 1.0f);
      ui.system_map_sensor_raytrace_los_samples =
          std::clamp(ui.system_map_sensor_raytrace_los_samples, 1, 64);
      ui.system_map_sensor_raytrace_spp = std::clamp(ui.system_map_sensor_raytrace_spp, 1, 16);
      ui.system_map_sensor_raytrace_max_depth = std::clamp(ui.system_map_sensor_raytrace_max_depth, 0, 10);
      ui.system_map_sensor_raytrace_error_threshold =
          std::clamp(ui.system_map_sensor_raytrace_error_threshold, 0.0f, 0.5f);

      ImGui::SliderFloat("LOS strength##heatmap", &ui.system_map_sensor_raytrace_los_strength, 0.0f, 1.0f, "%.2f");
      ImGui::SliderInt("LOS samples##heatmap", &ui.system_map_sensor_raytrace_los_samples, 1, 32);
      ImGui::SliderInt("Adaptive depth##heatmap", &ui.system_map_sensor_raytrace_max_depth, 0, 10);
      ImGui::SliderFloat("Detail threshold##heatmap", &ui.system_map_sensor_raytrace_error_threshold, 0.0f, 0.25f, "%.3f");
      ImGui::SliderInt("Stochastic spp##heatmap", &ui.system_map_sensor_raytrace_spp, 1, 8);
      ImGui::Checkbox("Debug quads##heatmap", &ui.system_map_sensor_raytrace_debug);
      ImGui::TextDisabled("(Does not change sim detection yet)");

      if (ui.system_map_sensor_raytrace_debug && sensor_rt_stats_valid) {
        ImGui::TextDisabled("Quads: tested %d  leaf %d", sensor_rt_stats.quads_tested, sensor_rt_stats.quads_leaf);
        ImGui::TextDisabled("Point evals: %d", sensor_rt_stats.point_evals);
        ImGui::TextDisabled("LOS env samples: %d", sensor_rt_stats.los_env_samples);
      }
    }
  }

  ui.system_map_heatmap_opacity = std::clamp(ui.system_map_heatmap_opacity, 0.0f, 1.0f);
  ui.system_map_heatmap_resolution = std::clamp(ui.system_map_heatmap_resolution, 16, 200);

  ImGui::SliderFloat("Opacity##heatmap", &ui.system_map_heatmap_opacity, 0.0f, 1.0f, "%.2f");
  ImGui::SliderInt("Resolution##heatmap", &ui.system_map_heatmap_resolution, 16, 160);

  {
    const int cells_x = std::clamp(ui.system_map_heatmap_resolution, 16, 200);
    const float aspect = (avail.x > 1.0f) ? (avail.y / avail.x) : 1.0f;
    const int cells_y = std::clamp(static_cast<int>(std::round(static_cast<float>(cells_x) * aspect)), 8, 200);
    ImGui::TextDisabled("Cells: %dx%d", cells_x, cells_y);
  }

  if ((ui.system_map_sensor_heatmap || ui.system_map_threat_heatmap) && viewer_faction_id == kInvalidId) {
    ImGui::TextDisabled("Select a ship to define view faction");
  } else {
    if (ui.system_map_sensor_heatmap) {
      ImGui::TextDisabled("Sensor sources: %d  (sig %.2f)",
                          static_cast<int>(sensor_sources.size()),
                          sensor_coverage_sig);
    }
    if (ui.system_map_threat_heatmap) {
      ImGui::TextDisabled("Threat sources: %d", static_cast<int>(threat_sources.size()));
    }
  }

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
  ImGui::SameLine();
  ImGui::Checkbox("Uncertainty", &ui.show_contact_uncertainty);

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
