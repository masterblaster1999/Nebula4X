#include "ui/panels.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_set>
#include <utility>

#include "nebula4x/core/serialization.h"
#include "nebula4x/util/event_export.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {
namespace {

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

bool ends_with_ci(const std::string& s, const std::string& suffix) {
  if (suffix.size() > s.size()) return false;
  const std::string tail = s.substr(s.size() - suffix.size());
  return nebula4x::to_lower(tail) == nebula4x::to_lower(suffix);
}

void maybe_fix_export_extension(char* path, std::size_t cap, const char* desired_ext) {
  if (!path || cap == 0 || !desired_ext) return;
  if (path[0] == '\0') return;

  std::string p(path);
  const std::string pl = nebula4x::to_lower(p);
  const bool known_ext = ends_with_ci(pl, ".csv") || ends_with_ci(pl, ".json") || ends_with_ci(pl, ".jsonl");

  const std::size_t last_sep = p.find_last_of("/\\");
  const std::size_t last_dot = p.find_last_of('.');
  const bool has_ext = (last_dot != std::string::npos) && (last_sep == std::string::npos || last_dot > last_sep);

  // Only auto-tweak the suffix when the path looks like one of our common defaults.
  if (!(known_ext || !has_ext)) return;

  const std::string ext(desired_ext);
  if (has_ext) {
    p = p.substr(0, last_dot) + ext;
  } else {
    p += ext;
  }

  if (p.size() >= cap) p.resize(cap - 1);
#if defined(_MSC_VER)
  strncpy_s(path, cap, p.c_str(), _TRUNCATE);
#else
  std::strncpy(path, p.c_str(), cap);
  path[cap - 1] = '\0';
#endif
}

const char* ship_role_label(ShipRole r) {
  switch (r) {
    case ShipRole::Freighter: return "Freighter";
    case ShipRole::Surveyor: return "Surveyor";
    case ShipRole::Combatant: return "Combatant";
    default: return "Unknown";
  }
}

const char* component_type_label(ComponentType t) {
  switch (t) {
    case ComponentType::Engine: return "Engine";
    case ComponentType::Cargo: return "Cargo";
    case ComponentType::Sensor: return "Sensor";
    case ComponentType::Reactor: return "Reactor";
    case ComponentType::Weapon: return "Weapon";
    case ComponentType::Armor: return "Armor";
    default: return "Unknown";
  }
}

const char* event_level_label(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return "Info";
    case EventLevel::Warn: return "Warn";
    case EventLevel::Error: return "Error";
  }
  return "Info";
}

const char* event_category_label(EventCategory c) {
  switch (c) {
    case EventCategory::General: return "General";
    case EventCategory::Research: return "Research";
    case EventCategory::Shipyard: return "Shipyard";
    case EventCategory::Construction: return "Construction";
    case EventCategory::Movement: return "Movement";
    case EventCategory::Combat: return "Combat";
    case EventCategory::Intel: return "Intel";
    case EventCategory::Exploration: return "Exploration";
  }
  return "General";
}

const char* diplomacy_status_label(DiplomacyStatus s) {
  switch (s) {
    case DiplomacyStatus::Friendly: return "Friendly";
    case DiplomacyStatus::Neutral: return "Neutral";
    case DiplomacyStatus::Hostile: return "Hostile";
  }
  return "Hostile";
}

// UI combo ordering: Hostile, Neutral, Friendly.
int diplomacy_status_to_combo_idx(DiplomacyStatus s) {
  switch (s) {
    case DiplomacyStatus::Hostile: return 0;
    case DiplomacyStatus::Neutral: return 1;
    case DiplomacyStatus::Friendly: return 2;
  }
  return 0;
}

DiplomacyStatus diplomacy_status_from_combo_idx(int idx) {
  switch (idx) {
    case 1: return DiplomacyStatus::Neutral;
    case 2: return DiplomacyStatus::Friendly;
    default: return DiplomacyStatus::Hostile;
  }
}


std::vector<std::string> sorted_all_design_ids(const Simulation& sim) {
  std::vector<std::string> ids;
  ids.reserve(sim.content().designs.size() + sim.state().custom_designs.size());
  for (const auto& [id, _] : sim.content().designs) ids.push_back(id);
  for (const auto& [id, _] : sim.state().custom_designs) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

std::vector<std::string> sorted_buildable_design_ids(const Simulation& sim, Id faction_id) {
  auto ids = sorted_all_design_ids(sim);
  ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const std::string& id) {
              return !sim.is_design_buildable_for_faction(faction_id, id);
            }),
            ids.end());
  return ids;
}

std::vector<std::pair<Id, std::string>> sorted_factions(const GameState& s) {
  std::vector<std::pair<Id, std::string>> out;
  out.reserve(s.factions.size());
  for (const auto& [id, f] : s.factions) out.push_back({id, f.name});
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second < b.second; });
  return out;
}

std::vector<std::pair<Id, std::string>> sorted_systems(const GameState& s) {
  std::vector<std::pair<Id, std::string>> out;
  out.reserve(s.systems.size());
  for (const auto& [id, sys] : s.systems) {
    out.push_back({id, sys.name + " (" + std::to_string(static_cast<unsigned long long>(id)) + ")"});
  }
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second < b.second; });
  return out;
}

std::vector<std::pair<Id, std::string>> sorted_ships(const GameState& s) {
  std::vector<std::pair<Id, std::string>> out;
  out.reserve(s.ships.size());
  for (const auto& [id, sh] : s.ships) {
    out.push_back({id, sh.name + " (" + std::to_string(static_cast<unsigned long long>(id)) + ")"});
  }
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second < b.second; });
  return out;
}

std::vector<std::pair<Id, std::string>> sorted_colonies(const GameState& s) {
  std::vector<std::pair<Id, std::string>> out;
  out.reserve(s.colonies.size());
  for (const auto& [id, c] : s.colonies) {
    out.push_back({id, c.name + " (" + std::to_string(static_cast<unsigned long long>(id)) + ")"});
  }
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second < b.second; });
  return out;
}

std::vector<std::pair<Id, std::string>> sorted_fleets(const GameState& s) {
  std::vector<std::pair<Id, std::string>> out;
  out.reserve(s.fleets.size());
  for (const auto& [id, fl] : s.fleets) {
    out.push_back({id, fl.name + " (" + std::to_string(static_cast<unsigned long long>(id)) + ")"});
  }
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second < b.second; });
  return out;
}

bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

bool prereqs_met(const Faction& f, const TechDef& t) {
  for (const auto& p : t.prereqs) {
    if (!vec_contains(f.known_techs, p)) return false;
  }
  return true;
}

ShipDesign derive_preview_design(const ContentDB& c, ShipDesign d) {
  double mass = 0.0;
  double speed = 0.0;
  double cargo = 0.0;
  double sensor = 0.0;
  double weapon_damage = 0.0;
  double weapon_range = 0.0;
  double hp_bonus = 0.0;

  for (const auto& cid : d.components) {
    auto it = c.components.find(cid);
    if (it == c.components.end()) continue;
    const auto& comp = it->second;
    mass += comp.mass_tons;
    speed = std::max(speed, comp.speed_km_s);
    cargo += comp.cargo_tons;
    sensor = std::max(sensor, comp.sensor_range_mkm);
    if (comp.type == ComponentType::Weapon) {
      weapon_damage += comp.weapon_damage;
      weapon_range = std::max(weapon_range, comp.weapon_range_mkm);
    }
    hp_bonus += comp.hp_bonus;
  }

  d.mass_tons = mass;
  d.speed_km_s = speed;
  d.cargo_tons = cargo;
  d.sensor_range_mkm = sensor;
  d.weapon_damage = weapon_damage;
  d.weapon_range_mkm = weapon_range;
  d.max_hp = std::max(1.0, mass * 2.0 + hp_bonus);
  return d;
}

} // namespace

void draw_main_menu(Simulation& sim, char* save_path, char* load_path) {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Game")) {
      if (ImGui::MenuItem("New Game")) {
        sim.new_game();
      }

      ImGui::Separator();

      ImGui::TextDisabled("Save path");
      ImGui::InputText("##save_path", save_path, 256);
      if (ImGui::MenuItem("Save")) {
        try {
          write_text_file(save_path, serialize_game_to_json(sim.state()));
        } catch (const std::exception& e) {
          nebula4x::log::error(std::string("Save failed: ") + e.what());
        }
      }

      ImGui::Separator();

      ImGui::TextDisabled("Load path");
      ImGui::InputText("##load_path", load_path, 256);
      if (ImGui::MenuItem("Load")) {
        try {
          sim.load_game(deserialize_game_from_json(read_text_file(load_path)));
        } catch (const std::exception& e) {
          nebula4x::log::error(std::string("Load failed: ") + e.what());
        }
      }

      ImGui::EndMenu();
    }

    ImGui::Text("  Date: %s", sim.state().date.to_string().c_str());

    ImGui::EndMainMenuBar();
  }
}

void draw_left_sidebar(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony) {
  ImGui::Text("Turns");
  if (ImGui::Button("+1 day")) sim.advance_days(1);
  ImGui::SameLine();
  if (ImGui::Button("+5")) sim.advance_days(5);
  ImGui::SameLine();
  if (ImGui::Button("+30")) sim.advance_days(30);

  // --- Auto-run / time warp ---
  {
    static int max_days = 365;
    static bool stop_info = true;
    static bool stop_warn = true;
    static bool stop_error = true;
    static int category_idx = 0; // 0 = Any
    static Id faction_filter = kInvalidId;
    static Id system_filter = kInvalidId;
    static Id ship_filter = kInvalidId;
    static Id colony_filter = kInvalidId;
    static char message_contains[128] = "";
    static std::string last_status;

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Auto-run (pause on event)", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::InputInt("Max days##autorun", &max_days);
      max_days = std::clamp(max_days, 1, 36500);

      ImGui::Checkbox("Info##autorun", &stop_info);
      ImGui::SameLine();
      ImGui::Checkbox("Warn##autorun", &stop_warn);
      ImGui::SameLine();
      ImGui::Checkbox("Error##autorun", &stop_error);

      // Category filter.
      {
        const char* cats[] = {
            "Any",
            "General",
            "Research",
            "Shipyard",
            "Construction",
            "Movement",
            "Combat",
            "Intel",
            "Exploration",
        };
        ImGui::Combo("Category##autorun", &category_idx, cats, IM_ARRAYSIZE(cats));
      }

      // Faction filter.
      {
        auto& s = sim.state();
        const auto fac_list = sorted_factions(s);
        const auto* sel = find_ptr(s.factions, faction_filter);
        const char* label = (faction_filter == kInvalidId) ? "Any" : (sel ? sel->name.c_str() : "(missing)");

        if (ImGui::BeginCombo("Faction##autorun", label)) {
          if (ImGui::Selectable("Any", faction_filter == kInvalidId)) faction_filter = kInvalidId;
          for (const auto& [fid, name] : fac_list) {
            if (ImGui::Selectable(name.c_str(), faction_filter == fid)) faction_filter = fid;
          }
          ImGui::EndCombo();
        }
      }

      // Optional context filters.
      {
        auto& s = sim.state();

        // System filter.
        {
          const auto sys_list = sorted_systems(s);
          const auto* sel = find_ptr(s.systems, system_filter);
          const char* label = (system_filter == kInvalidId) ? "Any" : (sel ? sel->name.c_str() : "(missing)");
          if (ImGui::BeginCombo("System##autorun", label)) {
            if (ImGui::Selectable("Any", system_filter == kInvalidId)) system_filter = kInvalidId;
            for (const auto& [sid, name] : sys_list) {
              if (ImGui::Selectable(name.c_str(), system_filter == sid)) system_filter = sid;
            }
            ImGui::EndCombo();
          }
        }

        // Ship filter.
        {
          const auto ship_list = sorted_ships(s);
          const auto* sel = find_ptr(s.ships, ship_filter);
          const char* label = (ship_filter == kInvalidId) ? "Any" : (sel ? sel->name.c_str() : "(missing)");
          if (ImGui::BeginCombo("Ship##autorun", label)) {
            if (ImGui::Selectable("Any", ship_filter == kInvalidId)) ship_filter = kInvalidId;
            for (const auto& [shid, name] : ship_list) {
              if (ImGui::Selectable(name.c_str(), ship_filter == shid)) ship_filter = shid;
            }
            ImGui::EndCombo();
          }
        }

        // Colony filter.
        {
          const auto col_list = sorted_colonies(s);
          const auto* sel = find_ptr(s.colonies, colony_filter);
          const char* label = (colony_filter == kInvalidId) ? "Any" : (sel ? sel->name.c_str() : "(missing)");
          if (ImGui::BeginCombo("Colony##autorun", label)) {
            if (ImGui::Selectable("Any", colony_filter == kInvalidId)) colony_filter = kInvalidId;
            for (const auto& [cid, name] : col_list) {
              if (ImGui::Selectable(name.c_str(), colony_filter == cid)) colony_filter = cid;
            }
            ImGui::EndCombo();
          }
        }
      }

      ImGui::InputText("Message contains##autorun", message_contains, IM_ARRAYSIZE(message_contains));

      if (ImGui::Button("Run until event##autorun")) {
        EventStopCondition stop;
        stop.stop_on_info = stop_info;
        stop.stop_on_warn = stop_warn;
        stop.stop_on_error = stop_error;
        stop.filter_category = false;
        stop.category = EventCategory::General;
        stop.faction_id = faction_filter;
        stop.system_id = system_filter;
        stop.ship_id = ship_filter;
        stop.colony_id = colony_filter;
        stop.message_contains = message_contains;

        if (category_idx > 0) {
          static const EventCategory cat_vals[] = {
              EventCategory::General,
              EventCategory::Research,
              EventCategory::Shipyard,
              EventCategory::Construction,
              EventCategory::Movement,
              EventCategory::Combat,
              EventCategory::Intel,
              EventCategory::Exploration,
          };
          const int idx = category_idx - 1;
          if (idx >= 0 && idx < (int)IM_ARRAYSIZE(cat_vals)) {
            stop.filter_category = true;
            stop.category = cat_vals[idx];
          }
        }

        auto res = sim.advance_until_event(max_days, stop);

        if (res.hit) {
          // Jump UI context to the event payload when possible.
          auto& s = sim.state();
          if (res.event.system_id != kInvalidId) s.selected_system = res.event.system_id;
          if (res.event.colony_id != kInvalidId) selected_colony = res.event.colony_id;
          if (res.event.ship_id != kInvalidId) {
            if (find_ptr(s.ships, res.event.ship_id)) selected_ship = res.event.ship_id;
          }

          last_status = "Paused on event after " + std::to_string(res.days_advanced) + " day(s): " + res.event.message;
        } else {
          last_status = "No matching events in " + std::to_string(res.days_advanced) + " day(s).";
        }
      }

      if (!last_status.empty()) {
        ImGui::TextWrapped("%s", last_status.c_str());
      }
    }
  }

  ImGui::Separator();
  ImGui::Text("Systems");
  const Ship* viewer_ship_for_fow = (selected_ship != kInvalidId) ? find_ptr(sim.state().ships, selected_ship) : nullptr;
  const Id viewer_faction_id_for_fow = viewer_ship_for_fow ? viewer_ship_for_fow->faction_id : ui.viewer_faction_id;

  for (const auto& [id, sys] : sim.state().systems) {
    if (ui.fog_of_war && viewer_faction_id_for_fow != kInvalidId) {
      if (!sim.is_system_discovered_by_faction(viewer_faction_id_for_fow, id)) {
        continue;
      }
    }
    const bool sel = (sim.state().selected_system == id);
    if (ImGui::Selectable(sys.name.c_str(), sel)) {
      sim.state().selected_system = id;
      // If we have a selected ship that isn't in this system, deselect it.
      if (selected_ship != kInvalidId) {
        const auto* sh = find_ptr(sim.state().ships, selected_ship);
        if (!sh || sh->system_id != id) selected_ship = kInvalidId;
      }
    }
  }

  ImGui::Separator();
  ImGui::Text("Ships (in system)");

  const auto* sys = find_ptr(sim.state().systems, sim.state().selected_system);
  if (!sys) {
    ImGui::TextDisabled("No system selected");
    return;
  }

  const Ship* viewer_ship = (selected_ship != kInvalidId) ? find_ptr(sim.state().ships, selected_ship) : nullptr;
  const Id viewer_faction_id = viewer_ship ? viewer_ship->faction_id : ui.viewer_faction_id;

  if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
    if (!sim.is_system_discovered_by_faction(viewer_faction_id, sim.state().selected_system)) {
      ImGui::TextDisabled("System not discovered by viewer faction");
      ImGui::TextDisabled("(Select a ship or faction in the Research tab to change view)");
      return;
    }
  }

  for (Id sid : sys->ships) {
    const auto* sh = find_ptr(sim.state().ships, sid);
    if (!sh) continue;

    // Fog-of-war: only show friendly ships and detected hostiles, based on the selected ship's faction.
    if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
      if (sh->faction_id != viewer_faction_id && !sim.is_ship_detected_by_faction(viewer_faction_id, sid)) {
        continue;
      }
    }

    const auto* fac = find_ptr(sim.state().factions, sh->faction_id);
    const std::string fac_name = fac ? fac->name : std::string("Faction ") + std::to_string(sh->faction_id);

    const Id fleet_id = sim.fleet_for_ship(sid);
    const auto* fl = (fleet_id != kInvalidId) ? find_ptr(sim.state().fleets, fleet_id) : nullptr;

    std::string label = sh->name;
    if (fl) label += " <" + fl->name + ">";
    label += "  (HP " + std::to_string(static_cast<int>(sh->hp)) + ")  [" + fac_name + "]##" +
             std::to_string(static_cast<unsigned long long>(sh->id));

    if (ImGui::Selectable(label.c_str(), selected_ship == sid)) {
      selected_ship = sid;
      ui.selected_fleet_id = fleet_id;
    }
  }


  ImGui::Separator();
  ImGui::Text("Fleets (in system)");
  bool any_fleets = false;
  for (const auto& [fid, fl] : sim.state().fleets) {
    // Fog-of-war: only show fleets belonging to the view faction.
    if (ui.fog_of_war && viewer_faction_id != kInvalidId && fl.faction_id != viewer_faction_id) {
      continue;
    }

    int in_sys = 0;
    for (Id sid : fl.ship_ids) {
      const auto* sh = find_ptr(sim.state().ships, sid);
      if (sh && sh->system_id == sys->id) ++in_sys;
    }
    if (in_sys == 0) continue;

    any_fleets = true;
    std::string label = fl.name + " (" + std::to_string(in_sys) + "/" + std::to_string((int)fl.ship_ids.size()) + ")";
    label += "##fleet_" + std::to_string(static_cast<unsigned long long>(fid));

    if (ImGui::Selectable(label.c_str(), ui.selected_fleet_id == fid)) {
      ui.selected_fleet_id = fid;

      // Prefer selecting the leader ship if it's in this system.
      Id pick_ship = fl.leader_ship_id;
      const auto* leader = (pick_ship != kInvalidId) ? find_ptr(sim.state().ships, pick_ship) : nullptr;
      if (!leader || leader->system_id != sys->id) {
        pick_ship = kInvalidId;
        for (Id sid : fl.ship_ids) {
          const auto* sh = find_ptr(sim.state().ships, sid);
          if (sh && sh->system_id == sys->id) {
            pick_ship = sid;
            break;
          }
        }
      }
      if (pick_ship != kInvalidId) selected_ship = pick_ship;
    }
  }
  if (!any_fleets) {
    ImGui::TextDisabled("(none)");
  }

  ImGui::Separator();
  ImGui::Text("Jump Points");
  if (sys->jump_points.empty()) {
    ImGui::TextDisabled("(none)");
  } else {
    for (Id jid : sys->jump_points) {
      const auto* jp = find_ptr(sim.state().jump_points, jid);
      if (!jp) continue;
      const auto* dest = find_ptr(sim.state().jump_points, jp->linked_jump_id);
      const auto* dest_sys = dest ? find_ptr(sim.state().systems, dest->system_id) : nullptr;

      const char* dest_label = "(unknown)";
      if (dest_sys) {
        // Fog-of-war: don't leak destination system names unless discovered.
        if (!ui.fog_of_war || viewer_faction_id_for_fow == kInvalidId ||
            sim.is_system_discovered_by_faction(viewer_faction_id_for_fow, dest_sys->id)) {
          dest_label = dest_sys->name.c_str();
        }
      }

      ImGui::BulletText("%s -> %s", jp->name.c_str(), dest_label);
    }
  }

  ImGui::Separator();
  ImGui::Text("Colonies");
  for (const auto& [cid, c] : sim.state().colonies) {
    std::string label = c.name + "##" + std::to_string(cid);
    if (ImGui::Selectable(label.c_str(), selected_colony == cid)) {
      selected_colony = cid;
    }
  }
}

void draw_right_sidebar(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony) {
  auto& s = sim.state();

  static int faction_combo_idx = 0;
  const auto factions = sorted_factions(s);
  if (!factions.empty()) {
    faction_combo_idx = std::clamp(faction_combo_idx, 0, static_cast<int>(factions.size()) - 1);
  }
  const Id selected_faction_id = factions.empty() ? kInvalidId : factions[faction_combo_idx].first;
  Faction* selected_faction = factions.empty() ? nullptr : find_ptr(s.factions, selected_faction_id);

  // Share the currently selected faction with other panels for fog-of-war/exploration view.
  ui.viewer_faction_id = selected_faction_id;

  if (ImGui::BeginTabBar("details_tabs")) {
    // --- Ship tab ---
    if (ImGui::BeginTabItem("Ship")) {
      if (selected_ship == kInvalidId) {
        ImGui::TextDisabled("No ship selected");
        ImGui::EndTabItem();
      } else if (auto* sh = find_ptr(s.ships, selected_ship)) {
        const auto* sys = find_ptr(s.systems, sh->system_id);
        const auto* fac = find_ptr(s.factions, sh->faction_id);
        const auto* d = sim.find_design(sh->design_id);

        ImGui::Text("%s", sh->name.c_str());
        ImGui::Separator();
        ImGui::Text("Faction: %s", fac ? fac->name.c_str() : "(unknown)");
        ImGui::Text("System: %s", sys ? sys->name.c_str() : "(unknown)");
        ImGui::Text("Pos: (%.2f, %.2f) mkm", sh->position_mkm.x, sh->position_mkm.y);
        ImGui::Text("Speed: %.1f km/s", sh->speed_km_s);

        double cargo_used_tons = 0.0;
        for (const auto& [_, t] : sh->cargo) cargo_used_tons += std::max(0.0, t);

        if (d) {
          ImGui::Text("Design: %s (%s)", d->name.c_str(), ship_role_label(d->role));
          ImGui::Text("Mass: %.0f t", d->mass_tons);
          ImGui::Text("HP: %.0f / %.0f", sh->hp, d->max_hp);
          ImGui::Text("Cargo: %.0f / %.0f t", cargo_used_tons, d->cargo_tons);
          ImGui::Text("Sensor: %.0f mkm", d->sensor_range_mkm);
          if (d->weapon_damage > 0.0) {
            ImGui::Text("Weapons: %.1f dmg/day  (Range %.1f mkm)", d->weapon_damage, d->weapon_range_mkm);
          } else {
            ImGui::TextDisabled("Weapons: (none)");
          }
        } else {
          ImGui::TextDisabled("Design definition missing: %s", sh->design_id.c_str());
        }


        // --- Fleet (membership / quick actions) ---
        const Id ship_fleet_id = sim.fleet_for_ship(sh->id);
        const Fleet* ship_fleet = (ship_fleet_id != kInvalidId) ? find_ptr(s.fleets, ship_fleet_id) : nullptr;

        ImGui::Separator();
        ImGui::Text("Fleet");
        if (!ship_fleet) {
          ImGui::TextDisabled("(none)");

          static Id last_ship_for_new_fleet = kInvalidId;
          static char new_fleet_name[128] = "New Fleet";
          static std::string fleet_action_status;

          if (last_ship_for_new_fleet != sh->id) {
            std::snprintf(new_fleet_name, sizeof(new_fleet_name), "%s Fleet", sh->name.c_str());
            last_ship_for_new_fleet = sh->id;
          }

          ImGui::InputText("New fleet name", new_fleet_name, IM_ARRAYSIZE(new_fleet_name));
          if (ImGui::SmallButton("Create fleet from this ship")) {
            std::string err;
            const Id fid = sim.create_fleet(sh->faction_id, new_fleet_name, {sh->id}, &err);
            if (fid != kInvalidId) {
              ui.selected_fleet_id = fid;
              fleet_action_status = "Created fleet.";
            } else {
              fleet_action_status = err.empty() ? "Create fleet failed." : err;
            }
          }

          if (ui.selected_fleet_id != kInvalidId) {
            const Fleet* tgt = find_ptr(s.fleets, ui.selected_fleet_id);
            if (tgt && tgt->faction_id == sh->faction_id) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Add to selected fleet")) {
                std::string err;
                if (sim.add_ship_to_fleet(tgt->id, sh->id, &err)) {
                  fleet_action_status = "Added to fleet.";
                } else {
                  fleet_action_status = err.empty() ? "Add to fleet failed." : err;
                }
              }
            }
          }

          if (!fleet_action_status.empty()) {
            ImGui::TextWrapped("%s", fleet_action_status.c_str());
          }
        } else {
          ImGui::Text("%s  (%d ships)", ship_fleet->name.c_str(), (int)ship_fleet->ship_ids.size());
          const Ship* leader =
              (ship_fleet->leader_ship_id != kInvalidId) ? find_ptr(s.ships, ship_fleet->leader_ship_id) : nullptr;
          ImGui::TextDisabled("Leader: %s", leader ? leader->name.c_str() : "(none)");

          if (ImGui::SmallButton("Select fleet")) {
            ui.selected_fleet_id = ship_fleet->id;
          }

          ImGui::SameLine();
          if (ImGui::SmallButton("Set as leader")) {
            sim.set_fleet_leader(ship_fleet->id, sh->id);
          }

          ImGui::SameLine();
          const Id fid = ship_fleet->id;
          if (ImGui::SmallButton("Remove from fleet")) {
            sim.remove_ship_from_fleet(fid, sh->id);
            if (ui.selected_fleet_id == fid && !find_ptr(s.fleets, fid)) {
              ui.selected_fleet_id = kInvalidId;
            }
          }
        }

        ImGui::Separator();
        ImGui::Text("Automation");

        const bool in_fleet = (ship_fleet != nullptr);
        if (in_fleet) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-explore when idle", &sh->auto_explore)) {
          if (sh->auto_explore) sh->auto_freight = false;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "When enabled, this ship will automatically travel to the nearest frontier system\n"
              "and jump into undiscovered systems whenever it has no queued orders.");
        }

        const bool can_auto_freight = (d && d->cargo_tons > 0.0);
        if (!can_auto_freight) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-freight minerals when idle", &sh->auto_freight)) {
          if (sh->auto_freight) sh->auto_explore = false;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "When enabled, this ship will automatically haul minerals between your colonies\n"
              "to relieve shipyard/construction stalls (only when the ship has no queued orders).");
        }
        if (!can_auto_freight) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(requires a cargo hold)");
        }

        if (in_fleet) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(disabled while in a fleet)");
        }

        ImGui::Separator();
        ImGui::Text("Orders");
        auto* ship_orders = find_ptr(s.ship_orders, selected_ship);
        const bool has_orders = (ship_orders && !ship_orders->queue.empty());

        // Editable queue view (drag-and-drop reorder, duplicate/delete, etc.)
        if (!has_orders) {
          ImGui::TextDisabled("(none)");
        } else {
          int delete_idx = -1;
          int dup_idx = -1;
          int move_from = -1;
          int move_to = -1;

          auto& q = ship_orders->queue;

          ImGui::TextDisabled("Drag+drop to reorder. Tip: if repeat is ON, edits do not update the repeat template unless you sync it.");

          const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
          if (ImGui::BeginTable("ship_orders_table", 4, flags)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 24.0f);
            ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Move", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(q.size()); ++i) {
              ImGui::TableNextRow();

              ImGui::TableSetColumnIndex(0);
              ImGui::Text("%d", i);

              ImGui::TableSetColumnIndex(1);
              const std::string ord_str = order_to_string(q[static_cast<std::size_t>(i)]);
              const std::string row_id = "##ship_order_row_" + std::to_string(static_cast<unsigned long long>(i));
              ImGui::Selectable((ord_str + row_id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

              if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("N4X_SHIP_ORDER_IDX", &i, sizeof(int));
                ImGui::Text("Move: %s", ord_str.c_str());
                ImGui::EndDragDropSource();
              }

              if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("N4X_SHIP_ORDER_IDX")) {
                  if (payload && payload->DataSize == sizeof(int)) {
                    const int src = *static_cast<const int*>(payload->Data);
                    move_from = src;
                    move_to = i;
                  }
                }
                ImGui::EndDragDropTarget();
              }

              ImGui::TableSetColumnIndex(2);
              const bool can_up = (i > 0);
              const bool can_down = (i + 1 < static_cast<int>(q.size()));
              if (!can_up) ImGui::BeginDisabled();
              if (ImGui::SmallButton(("Up##ship_order_up_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
                move_from = i;
                move_to = i - 1;
              }
              if (!can_up) ImGui::EndDisabled();

              ImGui::SameLine();
              if (!can_down) ImGui::BeginDisabled();
              if (ImGui::SmallButton(("Dn##ship_order_dn_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
                move_from = i;
                move_to = i + 1;
              }
              if (!can_down) ImGui::EndDisabled();

              ImGui::TableSetColumnIndex(3);
              if (ImGui::SmallButton(("Dup##ship_order_dup_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
                dup_idx = i;
              }
              ImGui::SameLine();
              if (ImGui::SmallButton(("Del##ship_order_del_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
                delete_idx = i;
              }
            }

            // Extra drop target at end: move to end of queue.
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("Drop here to move to end");
            if (ImGui::BeginDragDropTarget()) {
              if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("N4X_SHIP_ORDER_IDX")) {
                if (payload && payload->DataSize == sizeof(int)) {
                  const int src = *static_cast<const int*>(payload->Data);
                  move_from = src;
                  move_to = static_cast<int>(q.size()); // Simulation clamps to end.
                }
              }
              ImGui::EndDragDropTarget();
            }

            ImGui::EndTable();
          }

          // Apply edits after rendering to avoid iterator invalidation mid-loop.
          if (dup_idx >= 0) {
            sim.duplicate_queued_order(selected_ship, dup_idx);
          }
          if (delete_idx >= 0) {
            sim.delete_queued_order(selected_ship, delete_idx);
          }
          if (move_from >= 0 && move_to >= 0) {
            sim.move_queued_order(selected_ship, move_from, move_to);
          }
        }

        const bool repeat_on = ship_orders ? ship_orders->repeat : false;
        const int repeat_len = ship_orders ? static_cast<int>(ship_orders->repeat_template.size()) : 0;
        if (repeat_on) {
          ImGui::Text("Repeat: ON  (template %d orders)", repeat_len);
        } else {
          ImGui::Text("Repeat: OFF");
        }

        ImGui::Spacing();
        if (!repeat_on) {
          if (ImGui::SmallButton("Enable repeat")) {
            if (!sim.enable_order_repeat(selected_ship)) {
              nebula4x::log::warn("Couldn't enable repeat (queue empty?).");
            }
          }
        } else {
          if (ImGui::SmallButton("Update repeat template")) {
            if (!sim.update_order_repeat_template(selected_ship)) {
              nebula4x::log::warn("Couldn't update repeat template (queue empty?).");
            }
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Disable repeat")) {
            sim.disable_order_repeat(selected_ship);
          }
        }

        ImGui::Spacing();
        if (ImGui::SmallButton("Cancel current")) {
          sim.cancel_current_order(selected_ship);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear orders")) {
          sim.clear_orders(selected_ship);
        }

        // --- Order template library ---
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Order Templates", ImGuiTreeNodeFlags_DefaultOpen)) {
          // Persist UI selections between frames.
          static std::string selected_template;
          static char save_name_buf[128] = "";
          static char rename_buf[128] = "";
          static bool overwrite_existing = false;
          static bool append_when_applying = true;
          static bool confirm_delete = false;
          static std::string status;

          const auto names = sim.order_template_names();

          auto exists = [&](const std::string& nm) {
            return std::find(names.begin(), names.end(), nm) != names.end();
          };

          if (!names.empty()) {
            if (selected_template.empty() || !exists(selected_template)) {
              selected_template = names.front();
              std::snprintf(rename_buf, sizeof(rename_buf), "%s", selected_template.c_str());
            }
          } else {
            selected_template.clear();
          }

          const char* label = selected_template.empty() ? "(none)" : selected_template.c_str();
          if (ImGui::BeginCombo("Template##order_template_pick", label)) {
            if (ImGui::Selectable("(none)", selected_template.empty())) {
              selected_template.clear();
            }
            for (const auto& nm : names) {
              const bool sel = (selected_template == nm);
              if (ImGui::Selectable((nm + "##tmpl_sel_" + nm).c_str(), sel)) {
                selected_template = nm;
                std::snprintf(rename_buf, sizeof(rename_buf), "%s", selected_template.c_str());
                confirm_delete = false;
              }
            }
            ImGui::EndCombo();
          }

          ImGui::Checkbox("Append when applying", &append_when_applying);

          const bool can_apply = !selected_template.empty();
          if (!can_apply) ImGui::BeginDisabled();
          if (ImGui::SmallButton("Apply to this ship")) {
            if (!sim.apply_order_template_to_ship(selected_ship, selected_template, append_when_applying)) {
              status = "Apply failed (missing template or ship).";
            } else {
              status = "Applied template to ship.";
            }
          }
          if (!can_apply) ImGui::EndDisabled();

          if (ui.selected_fleet_id != kInvalidId) {
            ImGui::SameLine();
            const bool has_fleet = (find_ptr(s.fleets, ui.selected_fleet_id) != nullptr);
            const bool can_apply_fleet = can_apply && has_fleet;
            if (!can_apply_fleet) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Apply to selected fleet")) {
              if (!sim.apply_order_template_to_fleet(ui.selected_fleet_id, selected_template, append_when_applying)) {
                status = "Apply to fleet failed (missing template or fleet).";
              } else {
                status = "Applied template to fleet.";
              }
            }
            if (!can_apply_fleet) ImGui::EndDisabled();
          }

          ImGui::Spacing();
          ImGui::InputText("Save name##tmpl_save", save_name_buf, IM_ARRAYSIZE(save_name_buf));
          ImGui::Checkbox("Overwrite existing##tmpl_overwrite", &overwrite_existing);

          const bool can_save = ship_orders && !ship_orders->queue.empty();
          if (!can_save) ImGui::BeginDisabled();
          if (ImGui::SmallButton("Save current queue as template")) {
            std::string err;
            if (!ship_orders || ship_orders->queue.empty()) {
              status = "No queued orders to save.";
            } else if (sim.save_order_template(save_name_buf, ship_orders->queue, overwrite_existing, &err)) {
              status = std::string("Saved template: ") + save_name_buf;
              selected_template = save_name_buf;
              std::snprintf(rename_buf, sizeof(rename_buf), "%s", selected_template.c_str());
              confirm_delete = false;
            } else {
              status = err.empty() ? "Save failed." : err;
            }
          }
          if (!can_save) ImGui::EndDisabled();

          ImGui::Spacing();
          if (selected_template.empty()) {
            ImGui::TextDisabled("Select a template to rename/delete.");
          } else {
            ImGui::InputText("Rename to##tmpl_rename", rename_buf, IM_ARRAYSIZE(rename_buf));

            if (ImGui::SmallButton("Rename selected")) {
              std::string err;
              if (sim.rename_order_template(selected_template, rename_buf, &err)) {
                status = "Renamed template.";
                selected_template = rename_buf;
                confirm_delete = false;
              } else {
                status = err.empty() ? "Rename failed." : err;
              }
            }

            ImGui::SameLine();
            ImGui::Checkbox("Confirm delete##tmpl_confirm", &confirm_delete);
            ImGui::SameLine();
            if (!confirm_delete) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Delete##tmpl_delete")) {
              sim.delete_order_template(selected_template);
              status = "Deleted template.";
              selected_template.clear();
              confirm_delete = false;
            }
            if (!confirm_delete) ImGui::EndDisabled();
          }

          if (!status.empty()) {
            ImGui::TextWrapped("%s", status.c_str());
          }
        }

        ImGui::Separator();
        ImGui::Text("Cargo detail");
        if (d) {
          ImGui::Text("Used: %.0f / %.0f t", cargo_used_tons, d->cargo_tons);
        } else {
          ImGui::Text("Used: %.0f t", cargo_used_tons);
        }

        if (sh->cargo.empty()) {
          ImGui::TextDisabled("(empty)");
        } else {
          std::vector<std::pair<std::string, double>> cargo_list;
          cargo_list.reserve(sh->cargo.size());
          for (const auto& [k, v] : sh->cargo) cargo_list.emplace_back(k, v);
          std::sort(cargo_list.begin(), cargo_list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

          for (const auto& [k, v] : cargo_list) {
            ImGui::BulletText("%s: %.1f t", k.c_str(), v);
          }
        }

        // --- Colony Transfer ---
        ImGui::Spacing();
        ImGui::Text("Transfer with selected colony");
        ImGui::TextDisabled("Load/unload is an order: the ship will move to the colony body, then transfer in one day.");

        const Colony* sel_col = (selected_colony != kInvalidId) ? find_ptr(s.colonies, selected_colony) : nullptr;
        const Body* sel_col_body = sel_col ? find_ptr(s.bodies, sel_col->body_id) : nullptr;

        if (!sel_col) {
          ImGui::TextDisabled("No colony selected.");
        } else if (!sel_col_body) {
          ImGui::TextDisabled("Selected colony body missing.");
        } else if (sel_col->faction_id != sh->faction_id) {
          ImGui::TextDisabled("Selected colony is not friendly.");
        } else {
          ImGui::Text("Colony: %s", sel_col->name.c_str());

          if (sel_col_body->system_id != sh->system_id) {
            std::string dest_label = "(unknown)";
            const auto* dest_sys = find_ptr(s.systems, sel_col_body->system_id);
            if (dest_sys && (!ui.fog_of_war || sim.is_system_discovered_by_faction(sh->faction_id, dest_sys->id))) {
              dest_label = dest_sys->name;
            }
            ImGui::TextDisabled("Colony is in a different system %s. Order will auto-route via jump points.",
                                dest_label.c_str());
          }

          std::vector<std::string> minerals;
          minerals.reserve(sel_col->minerals.size() + sh->cargo.size());
          for (const auto& [k, _] : sel_col->minerals) minerals.push_back(k);
          for (const auto& [k, _] : sh->cargo) minerals.push_back(k);
          std::sort(minerals.begin(), minerals.end());
          minerals.erase(std::unique(minerals.begin(), minerals.end()), minerals.end());

          static int mineral_idx = 0;
          static double transfer_tons = 0.0;

          const int max_idx = static_cast<int>(minerals.size());
          mineral_idx = std::max(0, std::min(mineral_idx, max_idx));

          const std::string current_label = (mineral_idx == 0) ? std::string("All minerals") : minerals[mineral_idx - 1];

          if (ImGui::BeginCombo("Mineral##Col", current_label.c_str())) {
            if (ImGui::Selectable("All minerals", mineral_idx == 0)) mineral_idx = 0;
            for (int i = 0; i < static_cast<int>(minerals.size()); ++i) {
              const bool selected = (mineral_idx == i + 1);
              if (ImGui::Selectable(minerals[i].c_str(), selected)) mineral_idx = i + 1;
            }
            ImGui::EndCombo();
          }

          ImGui::InputDouble("Tons##Col (0 = max)", &transfer_tons, 10.0, 100.0, "%.1f");

          const std::string mineral_id = (mineral_idx == 0) ? std::string() : minerals[mineral_idx - 1];

          if (ImGui::Button("Load##Col")) {
            if (!sim.issue_load_mineral(selected_ship, selected_colony, mineral_id, transfer_tons, ui.fog_of_war)) {
              nebula4x::log::warn("Couldn't queue load order (no known route?).");
            }
          }
          ImGui::SameLine();
          if (ImGui::Button("Unload##Col")) {
            if (!sim.issue_unload_mineral(selected_ship, selected_colony, mineral_id, transfer_tons, ui.fog_of_war)) {
              nebula4x::log::warn("Couldn't queue unload order (no known route?).");
            }
          }
          ImGui::SameLine();
          if (ImGui::Button("Scrap Ship")) {
              if(!sim.issue_scrap_ship(selected_ship, selected_colony, ui.fog_of_war)) {
                  nebula4x::log::warn("Couldn't queue scrap order.");
              }
          }
        }

        // --- Ship-to-Ship Transfer ---
        ImGui::Separator();
        ImGui::Text("Ship-to-Ship Transfer");
        ImGui::TextDisabled("Transfers cargo to another friendly ship in the same system.");
        
        static int target_ship_idx = -1;
        std::vector<std::pair<Id, std::string>> friendly_ships;
        if (sys) {
            for (Id sid : sys->ships) {
                if (sid == selected_ship) continue;
                const auto* other = find_ptr(s.ships, sid);
                if (other && other->faction_id == sh->faction_id) {
                    friendly_ships.push_back({sid, other->name});
                }
            }
        }

        if (friendly_ships.empty()) {
            ImGui::TextDisabled("No other friendly ships in system.");
        } else {
            // Validate selection index
            if (target_ship_idx >= static_cast<int>(friendly_ships.size())) target_ship_idx = -1;
            
            const char* current_ship_label = (target_ship_idx >= 0) ? friendly_ships[target_ship_idx].second.c_str() : "Select Target...";
            if (ImGui::BeginCombo("Target Ship", current_ship_label)) {
                for(int i=0; i < static_cast<int>(friendly_ships.size()); ++i) {
                     const bool selected = (target_ship_idx == i);
                     if (ImGui::Selectable(friendly_ships[i].second.c_str(), selected)) target_ship_idx = i;
                }
                ImGui::EndCombo();
            }

            // Reuse mineral list from ship cargo only
            std::vector<std::string> ship_minerals;
            for(const auto& [k, v] : sh->cargo) ship_minerals.push_back(k);
            std::sort(ship_minerals.begin(), ship_minerals.end());
            
            static int ship_min_idx = 0;
            static double ship_transfer_tons = 0.0;
            // Ensure index is valid
            if (ship_min_idx > static_cast<int>(ship_minerals.size())) ship_min_idx = 0;
            
            const std::string cur_ship_min_label = (ship_min_idx == 0) ? "All minerals" : ship_minerals[ship_min_idx - 1];

            if (ImGui::BeginCombo("Mineral##Ship", cur_ship_min_label.c_str())) {
                if (ImGui::Selectable("All minerals", ship_min_idx == 0)) ship_min_idx = 0;
                for(int i=0; i< static_cast<int>(ship_minerals.size()); ++i) {
                    const bool selected = (ship_min_idx == i+1);
                    if (ImGui::Selectable(ship_minerals[i].c_str(), selected)) ship_min_idx = i+1;
                }
                ImGui::EndCombo();
            }
            
            ImGui::InputDouble("Tons##Ship (0 = max)", &ship_transfer_tons, 10.0, 100.0, "%.1f");
            
            if (ImGui::Button("Transfer to Target")) {
                if (target_ship_idx >= 0) {
                    const Id target_id = friendly_ships[target_ship_idx].first;
                    const std::string min_id = (ship_min_idx == 0) ? "" : ship_minerals[ship_min_idx-1];
                    if (!sim.issue_transfer_cargo_to_ship(selected_ship, target_id, min_id, ship_transfer_tons, ui.fog_of_war)) {
                         nebula4x::log::warn("Couldn't queue transfer order.");
                    }
                }
            }
        }


        ImGui::Separator();
        ImGui::Text("Quick orders");

        // Simple scheduling primitive.
        static int wait_days = 1;
        wait_days = std::clamp(wait_days, 1, 365000); // ~1000 years, just a safety cap.
        ImGui::InputInt("Wait (days)", &wait_days);
        if (ImGui::Button("Queue wait")) {
          sim.issue_wait_days(selected_ship, wait_days);
        }

        if (ImGui::Button("Move to (0,0)")) {
          sim.issue_move_to_point(selected_ship, {0.0, 0.0});
        }
        if (ImGui::Button("Move to Earth")) {
          const auto* sys2 = find_ptr(s.systems, sh->system_id);
          if (sys2) {
            for (Id bid : sys2->bodies) {
              const auto* b = find_ptr(s.bodies, bid);
              if (b && b->name == "Earth") {
                if (!sim.issue_move_to_body(selected_ship, b->id, ui.fog_of_war)) {
                  nebula4x::log::warn("Couldn't issue move-to-body order.");
                }
                break;
              }
            }
          }
        }

        // Orbit button logic
        if (sel_col_body && sel_col_body->system_id == sh->system_id) {
            std::string btn_label = "Orbit " + sel_col->name;
            if (ImGui::Button(btn_label.c_str())) {
                // Orbit indefinitely (-1)
                if (!sim.issue_orbit_body(selected_ship, sel_col_body->id, -1, ui.fog_of_war)) {
                    nebula4x::log::warn("Couldn't issue orbit order.");
                }
            }
        }

        // Jump point travel
        const auto* sh_sys = find_ptr(s.systems, sh->system_id);
        if (sh_sys && !sh_sys->jump_points.empty()) {
          ImGui::Spacing();
          ImGui::Text("Jump travel");
          for (Id jid : sh_sys->jump_points) {
            const auto* jp = find_ptr(s.jump_points, jid);
            if (!jp) continue;
            const auto* dest = find_ptr(s.jump_points, jp->linked_jump_id);
            const auto* dest_sys = dest ? find_ptr(s.systems, dest->system_id) : nullptr;

            std::string btn = "Travel via " + jp->name;
            if (dest_sys) {
              // Fog-of-war: hide destination names until the system is discovered by this ship's faction.
              if (!ui.fog_of_war || sim.is_system_discovered_by_faction(sh->faction_id, dest_sys->id)) {
                btn += " -> " + dest_sys->name;
              } else {
                btn += " -> (unknown)";
              }
            }

            if (ImGui::Button((btn + "##" + std::to_string(jid)).c_str())) {
              sim.issue_travel_via_jump(selected_ship, jid);
            }
          }
        }

        // Combat: list hostiles in this system
        if (sh_sys) {
          std::vector<Id> hostiles =
              sim.detected_hostile_ships_in_system(sh->faction_id, sh->system_id);

          ImGui::Spacing();
          ImGui::Text("Combat");
          if (hostiles.empty()) {
            ImGui::TextDisabled("No detected hostiles in system");
          } else {
            ImGui::TextDisabled("Ships with weapons auto-fire once/day if in range.");
            for (Id hid : hostiles) {
              const auto* other = find_ptr(s.ships, hid);
              if (!other) continue;
              const auto* od = sim.find_design(other->design_id);
              const double range = d ? d->weapon_range_mkm : 0.0;
              const double dist = (other->position_mkm - sh->position_mkm).length();

              std::string label = other->name;
              label += " (HP " + std::to_string(static_cast<int>(other->hp)) + ")";
              if (od && od->weapon_damage > 0.0) label += " [armed]";

              ImGui::BulletText("%s  dist %.2f mkm", label.c_str(), dist);
              if (range > 0.0) {
                ImGui::SameLine();
                if (ImGui::SmallButton(("Attack##" + std::to_string(hid)).c_str())) {
                  sim.issue_attack_ship(sh->id, hid, ui.fog_of_war);
                }
              }
            }
          }
        }

        ImGui::EndTabItem();
      } else {
        ImGui::TextDisabled("Selected ship no longer exists");
        ImGui::EndTabItem();
      }
    }

    // --- Fleet tab ---
    if (ImGui::BeginTabItem("Fleet")) {
      // Keep selection valid.
      if (ui.selected_fleet_id != kInvalidId && !find_ptr(s.fleets, ui.selected_fleet_id)) {
        ui.selected_fleet_id = kInvalidId;
      }

      static std::string fleet_status;

      // Fleet selector
      const Fleet* selected_fleet = (ui.selected_fleet_id != kInvalidId) ? find_ptr(s.fleets, ui.selected_fleet_id) : nullptr;
      const char* fleet_label = selected_fleet ? selected_fleet->name.c_str() : "(none)";
      if (ImGui::BeginCombo("Selected fleet", fleet_label)) {
        if (ImGui::Selectable("(none)", ui.selected_fleet_id == kInvalidId)) {
          ui.selected_fleet_id = kInvalidId;
        }

        const auto fleet_list = sorted_fleets(s);
        for (const auto& [fid, _] : fleet_list) {
          const auto* fl = find_ptr(s.fleets, fid);
          if (!fl) continue;
          std::string item = fl->name + " (" + std::to_string((int)fl->ship_ids.size()) + ")";
          const bool is_sel = (ui.selected_fleet_id == fid);
          if (ImGui::Selectable((item + "##fleet_pick_" + std::to_string(static_cast<unsigned long long>(fid))).c_str(), is_sel)) {
            ui.selected_fleet_id = fid;
            // Focus on leader if present
            if (fl->leader_ship_id != kInvalidId) {
              if (const auto* leader = find_ptr(s.ships, fl->leader_ship_id)) {
                selected_ship = leader->id;
                s.selected_system = leader->system_id;
              }
            }
          }
        }
        ImGui::EndCombo();
      }

      // --- Create fleet ---
      ImGui::Separator();
      ImGui::Text("Create fleet");
      static char create_name[128] = "New Fleet";
      static Id create_faction_id = kInvalidId;
      static bool include_selected_ship = true;
      static bool include_unassigned_in_system = false;

      // Default faction: selected ship -> viewer faction -> first faction
      if (create_faction_id == kInvalidId) {
        if (selected_ship != kInvalidId) {
          if (const auto* sh = find_ptr(s.ships, selected_ship)) create_faction_id = sh->faction_id;
        }
        if (create_faction_id == kInvalidId) create_faction_id = ui.viewer_faction_id;
        if (create_faction_id == kInvalidId && !factions.empty()) create_faction_id = factions.front().first;
      }

      const auto* create_fac = find_ptr(s.factions, create_faction_id);
      const char* create_fac_label = create_fac ? create_fac->name.c_str() : "(none)";
      if (ImGui::BeginCombo("Faction##fleet_create_faction", create_fac_label)) {
        for (const auto& [fid, nm] : factions) {
          const bool sel = (create_faction_id == fid);
          if (ImGui::Selectable((nm + "##fleet_create_fac_" + std::to_string(static_cast<unsigned long long>(fid))).c_str(), sel)) {
            create_faction_id = fid;
          }
        }
        ImGui::EndCombo();
      }

      ImGui::InputText("Name##fleet_create_name", create_name, IM_ARRAYSIZE(create_name));
      ImGui::Checkbox("Include selected ship", &include_selected_ship);
      ImGui::Checkbox("Include unassigned ships in current system", &include_unassigned_in_system);

      if (ImGui::SmallButton("Create fleet")) {
        std::vector<Id> members;

        if (include_selected_ship && selected_ship != kInvalidId) {
          if (const auto* sh = find_ptr(s.ships, selected_ship)) {
            if (sh->faction_id == create_faction_id) members.push_back(sh->id);
          }
        }

        if (include_unassigned_in_system) {
          const auto* sys = (s.selected_system != kInvalidId) ? find_ptr(s.systems, s.selected_system) : nullptr;
          if (sys) {
            for (Id sid : sys->ships) {
              const auto* sh = find_ptr(s.ships, sid);
              if (!sh) continue;
              if (sh->faction_id != create_faction_id) continue;
              if (sim.fleet_for_ship(sid) != kInvalidId) continue;
              if (std::find(members.begin(), members.end(), sid) == members.end()) members.push_back(sid);
            }
          }
        }

        if (members.empty()) {
          fleet_status = "No eligible ships selected for new fleet.";
        } else {
          std::string err;
          const Id fid = sim.create_fleet(create_faction_id, create_name, members, &err);
          if (fid != kInvalidId) {
            ui.selected_fleet_id = fid;
            fleet_status = "Created fleet.";
          } else {
            fleet_status = err.empty() ? "Create fleet failed." : err;
          }
        }
      }

      if (!fleet_status.empty()) {
        ImGui::TextWrapped("%s", fleet_status.c_str());
      }

      // Refresh selected_fleet pointer after create/disband operations.
      selected_fleet = (ui.selected_fleet_id != kInvalidId) ? find_ptr(s.fleets, ui.selected_fleet_id) : nullptr;
      if (!selected_fleet) {
        ImGui::Separator();
        ImGui::TextDisabled("No fleet selected.");
        ImGui::EndTabItem();
      } else {
        const auto* fac = find_ptr(s.factions, selected_fleet->faction_id);
        const Ship* leader =
            (selected_fleet->leader_ship_id != kInvalidId) ? find_ptr(s.ships, selected_fleet->leader_ship_id) : nullptr;

        // --- Fleet details ---
        ImGui::Separator();
        ImGui::Text("Details");
        ImGui::Text("Faction: %s", fac ? fac->name.c_str() : "(unknown)");
        ImGui::Text("Ships: %d", (int)selected_fleet->ship_ids.size());

        static Id rename_for = kInvalidId;
        static char rename_buf[128] = "";
        if (rename_for != selected_fleet->id) {
          std::snprintf(rename_buf, sizeof(rename_buf), "%s", selected_fleet->name.c_str());
          rename_for = selected_fleet->id;
        }

        ImGui::InputText("Name##fleet_rename", rename_buf, IM_ARRAYSIZE(rename_buf));
        if (ImGui::SmallButton("Rename")) {
          if (sim.rename_fleet(selected_fleet->id, rename_buf)) {
            fleet_status = "Renamed fleet.";
          } else {
            fleet_status = "Rename failed (empty name?).";
          }
        }

        const char* leader_label = leader ? leader->name.c_str() : "(none)";
        if (ImGui::BeginCombo("Leader##fleet_leader", leader_label)) {
          for (Id sid : selected_fleet->ship_ids) {
            const auto* sh = find_ptr(s.ships, sid);
            if (!sh) continue;
            const bool sel = (selected_fleet->leader_ship_id == sid);
            std::string item = sh->name + "##leader_pick_" + std::to_string(static_cast<unsigned long long>(sid));
            if (ImGui::Selectable(item.c_str(), sel)) {
              sim.set_fleet_leader(selected_fleet->id, sid);
            }
          }
          ImGui::EndCombo();
        }

        // --- Formation configuration ---
        ImGui::Separator();
        ImGui::Text("Formation");
        {
          const char* kFormationNames[] = {
              "None",
              "Line Abreast",
              "Column",
              "Wedge",
              "Ring",
          };
          int formation_idx = static_cast<int>(selected_fleet->formation);
          if (formation_idx < 0 || formation_idx >= static_cast<int>(IM_ARRAYSIZE(kFormationNames))) formation_idx = 0;
          if (ImGui::Combo("Type##fleet_formation", &formation_idx, kFormationNames, IM_ARRAYSIZE(kFormationNames))) {
            sim.configure_fleet_formation(selected_fleet->id, static_cast<FleetFormation>(formation_idx),
                                          selected_fleet->formation_spacing_mkm);
          }

          double spacing = selected_fleet->formation_spacing_mkm;
          if (ImGui::InputDouble("Spacing mkm##fleet_formation_spacing", &spacing, 0.25, 1.0, "%.2f")) {
            spacing = std::max(0.0, spacing);
            sim.configure_fleet_formation(selected_fleet->id, selected_fleet->formation, spacing);
          }
          ImGui::TextDisabled("Applied as a target offset for MoveToPoint + AttackShip orders.");
        }

        // --- Membership management ---
        ImGui::Separator();
        ImGui::Text("Members");
        Id remove_ship_id = kInvalidId;
        for (Id sid : selected_fleet->ship_ids) {
          const auto* sh = find_ptr(s.ships, sid);
          if (!sh) continue;

          std::string row = sh->name + "##fleet_member_" + std::to_string(static_cast<unsigned long long>(sid));
          if (ImGui::Selectable(row.c_str(), selected_ship == sid)) {
            selected_ship = sid;
            s.selected_system = sh->system_id;
          }
          ImGui::SameLine();
          if (ImGui::SmallButton(("Remove##fleet_rm_" + std::to_string(static_cast<unsigned long long>(sid))).c_str())) {
            remove_ship_id = sid;
          }
        }
        if (remove_ship_id != kInvalidId) {
          const Id fid = selected_fleet->id;
          sim.remove_ship_from_fleet(fid, remove_ship_id);
          if (!find_ptr(s.fleets, fid)) {
            ui.selected_fleet_id = kInvalidId;
          }
        }

        ImGui::Spacing();
        if (selected_ship != kInvalidId) {
          const auto* sh = find_ptr(s.ships, selected_ship);
          if (sh && sh->faction_id == selected_fleet->faction_id) {
            if (ImGui::SmallButton("Add selected ship##fleet_add_selected")) {
              std::string err;
              if (sim.add_ship_to_fleet(selected_fleet->id, sh->id, &err)) {
                fleet_status = "Added ship to fleet.";
              } else {
                fleet_status = err.empty() ? "Add ship failed." : err;
              }
            }
          }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Disband fleet")) {
          const Id fid = selected_fleet->id;
          sim.disband_fleet(fid);
          ui.selected_fleet_id = kInvalidId;
          fleet_status = "Disbanded fleet.";
        }

        // --- Orders ---
        ImGui::Separator();
        ImGui::Text("Orders");
        ImGui::TextDisabled("Tip: Ctrl+click on the System Map or Ctrl+Right click on the Galaxy Map routes the fleet.");

        if (ImGui::SmallButton("Clear fleet orders")) {
          sim.clear_fleet_orders(selected_fleet->id);
        }

        ImGui::Spacing();
        static int wait_days = 5;
        ImGui::InputInt("Wait days##fleet_wait", &wait_days);
        wait_days = std::max(1, wait_days);
        if (ImGui::SmallButton("Issue Wait")) {
          sim.issue_fleet_wait_days(selected_fleet->id, wait_days);
        }

        ImGui::Spacing();
        static double move_x = 0.0;
        static double move_y = 0.0;
        ImGui::InputDouble("X mkm##fleet_move_x", &move_x);
        ImGui::InputDouble("Y mkm##fleet_move_y", &move_y);
        if (ImGui::SmallButton("Move to point")) {
          sim.issue_fleet_move_to_point(selected_fleet->id, Vec2{move_x, move_y});
        }

        // Move / orbit body in selected system
        const auto* sys = (s.selected_system != kInvalidId) ? find_ptr(s.systems, s.selected_system) : nullptr;
        if (sys) {
          static Id body_target = kInvalidId;
          const Body* bt = (body_target != kInvalidId) ? find_ptr(s.bodies, body_target) : nullptr;
          const char* body_label = bt ? bt->name.c_str() : "(select body)";
          if (ImGui::BeginCombo("Body##fleet_body", body_label)) {
            for (Id bid : sys->bodies) {
              const auto* b = find_ptr(s.bodies, bid);
              if (!b) continue;
              const bool sel = (body_target == bid);
              std::string item = b->name + "##fleet_body_" + std::to_string(static_cast<unsigned long long>(bid));
              if (ImGui::Selectable(item.c_str(), sel)) {
                body_target = bid;
              }
            }
            ImGui::EndCombo();
          }

          if (body_target != kInvalidId) {
            if (ImGui::SmallButton("Move to body")) {
              sim.issue_fleet_move_to_body(selected_fleet->id, body_target, ui.fog_of_war);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Orbit body")) {
              sim.issue_fleet_orbit_body(selected_fleet->id, body_target, ui.fog_of_war);
            }
          }
        }

        // Travel to system
        {
          const auto systems = sorted_systems(s);
          static Id target_system = kInvalidId;
          const auto* tsys = (target_system != kInvalidId) ? find_ptr(s.systems, target_system) : nullptr;
          const char* sys_label = tsys ? tsys->name.c_str() : "(select system)";
          if (ImGui::BeginCombo("Travel to system##fleet_travel_sys", sys_label)) {
            for (const auto& [sid, nm] : systems) {
              const bool sel = (target_system == sid);
              if (ImGui::Selectable((nm + "##fleet_travel_" + std::to_string(static_cast<unsigned long long>(sid))).c_str(), sel)) {
                target_system = sid;
              }
            }
            ImGui::EndCombo();
          }

          if (target_system != kInvalidId) {
            if (ImGui::SmallButton("Travel")) {
              if (!sim.issue_fleet_travel_to_system(selected_fleet->id, target_system, ui.fog_of_war)) {
                fleet_status = "No known jump route to that system.";
              }
            }
          }
        }

        // Combat quick actions
        {
          Id combat_system = leader ? leader->system_id : kInvalidId;
          if (combat_system != kInvalidId) {
            std::vector<Id> hostiles;
            if (ui.fog_of_war) {
              hostiles = sim.detected_hostile_ships_in_system(selected_fleet->faction_id, combat_system);
            } else {
              const auto* csys = find_ptr(s.systems, combat_system);
              if (csys) {
                for (Id sid : csys->ships) {
                  const auto* sh = find_ptr(s.ships, sid);
                  if (sh && sh->faction_id != selected_fleet->faction_id &&
                      sim.are_factions_hostile(selected_fleet->faction_id, sh->faction_id))
                    hostiles.push_back(sid);
                }
              }
            }

            ImGui::Spacing();
            ImGui::Text("Combat");
            if (hostiles.empty()) {
              ImGui::TextDisabled("(no hostiles)");
            } else {
              for (Id hid : hostiles) {
                const auto* other = find_ptr(s.ships, hid);
                if (!other) continue;
                ImGui::BulletText("%s (HP %.0f)", other->name.c_str(), other->hp);
                ImGui::SameLine();
                if (ImGui::SmallButton(("Attack##fleet_attack_" + std::to_string(static_cast<unsigned long long>(hid))).c_str())) {
                  sim.issue_fleet_attack_ship(selected_fleet->id, hid, ui.fog_of_war);
                }
              }
            }
          }
        }

        // Cargo: load/unload from selected colony
        if (selected_colony != kInvalidId) {
          ImGui::Spacing();
          ImGui::Text("Cargo (selected colony)");
          static char mineral_name[64] = "Duranium";
          static double mineral_tons = 100.0;
          ImGui::InputText("Mineral##fleet_mineral", mineral_name, IM_ARRAYSIZE(mineral_name));
          ImGui::InputDouble("Tons##fleet_mineral_tons", &mineral_tons);
          mineral_tons = std::max(0.0, mineral_tons);

          if (ImGui::SmallButton("Load")) {
            sim.issue_fleet_load_mineral(selected_fleet->id, selected_colony, mineral_name, mineral_tons, ui.fog_of_war);
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Unload")) {
            sim.issue_fleet_unload_mineral(selected_fleet->id, selected_colony, mineral_name, mineral_tons, ui.fog_of_war);
          }
        }

        ImGui::EndTabItem();
      }
    }

    // --- Colony tab ---
    if (ImGui::BeginTabItem("Colony")) {
      if (selected_colony == kInvalidId) {
        ImGui::TextDisabled("No colony selected");
        ImGui::EndTabItem();
      } else if (auto* colony = find_ptr(s.colonies, selected_colony)) {
        ImGui::Text("%s", colony->name.c_str());
        ImGui::Separator();
        ImGui::Text("Population: %.0f M", colony->population_millions);

        ImGui::Separator();
        ImGui::Text("Minerals");
        for (const auto& [k, v] : colony->minerals) {
          ImGui::BulletText("%s: %.1f", k.c_str(), v);
        }

        ImGui::Separator();
        ImGui::Text("Installations");
        for (const auto& [k, v] : colony->installations) {
          const auto it = sim.content().installations.find(k);
          const std::string nm = (it == sim.content().installations.end()) ? k : it->second.name;
          if (it != sim.content().installations.end() && it->second.sensor_range_mkm > 0.0) {
            ImGui::BulletText("%s: %d  (Sensor %.0f mkm)", nm.c_str(), v, it->second.sensor_range_mkm);
          } else {
            ImGui::BulletText("%s: %d", nm.c_str(), v);
          }
        }

        ImGui::Separator();
        ImGui::Text("Construction");
        const double cp_per_day = sim.construction_points_per_day(*colony);
        ImGui::Text("Construction Points/day: %.1f", cp_per_day);

if (colony->construction_queue.empty()) {
  ImGui::TextDisabled("Queue empty");
} else {
  int delete_idx = -1;
  int move_from = -1;
  int move_to = -1;

  ImGui::TextDisabled("Drag+drop to reorder. Stalled orders (missing minerals) no longer block later orders.");

  const ImGuiTableFlags qflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                 ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
  if (ImGui::BeginTable("construction_queue_table", 6, qflags)) {
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Move", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableHeadersRow();

    auto missing_mineral_for = [&](const InstallationDef& def) -> std::string {
      for (const auto& [mineral, cost] : def.build_costs) {
        if (cost <= 0.0) continue;
        const auto it2 = colony->minerals.find(mineral);
        const double have = (it2 == colony->minerals.end()) ? 0.0 : it2->second;
        if (have + 1e-9 < cost) return mineral;
      }
      return {};
    };

    for (int i = 0; i < static_cast<int>(colony->construction_queue.size()); ++i) {
      const auto& ord = colony->construction_queue[static_cast<std::size_t>(i)];
      const auto it = sim.content().installations.find(ord.installation_id);
      const InstallationDef* def = (it == sim.content().installations.end()) ? nullptr : &it->second;
      const std::string nm = def ? def->name : ord.installation_id;

      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%d", i);

      ImGui::TableSetColumnIndex(1);
      const std::string row_id = "##construction_row_" + std::to_string(static_cast<unsigned long long>(i));
      ImGui::Selectable((nm + row_id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("N4X_CONSTRUCTION_ORDER_IDX", &i, sizeof(int));
        ImGui::Text("Move: %s", nm.c_str());
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("N4X_CONSTRUCTION_ORDER_IDX")) {
          if (payload && payload->DataSize == sizeof(int)) {
            const int src = *static_cast<const int*>(payload->Data);
            move_from = src;
            move_to = i;
          }
        }
        ImGui::EndDragDropTarget();
      }

      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%d", ord.quantity_remaining);

      ImGui::TableSetColumnIndex(3);
      if (!def) {
        ImGui::TextDisabled("(unknown installation)");
      } else if (ord.minerals_paid && def->construction_cost > 0.0) {
        const double done = def->construction_cost - ord.cp_remaining;
        const float frac = static_cast<float>(std::clamp(done / def->construction_cost, 0.0, 1.0));
        ImGui::ProgressBar(frac, ImVec2(-1, 0),
                           (std::to_string(static_cast<int>(done)) + " / " +
                            std::to_string(static_cast<int>(def->construction_cost)) + " CP")
                               .c_str());
      } else if (!ord.minerals_paid && !def->build_costs.empty()) {
        const std::string missing = missing_mineral_for(*def);
        if (!missing.empty()) {
          ImGui::TextDisabled("STALLED (need %s)", missing.c_str());
        } else {
          ImGui::TextDisabled("Ready");
        }
      } else if (ord.minerals_paid) {
        ImGui::TextDisabled("In progress");
      } else {
        ImGui::TextDisabled("Waiting");
      }

      ImGui::TableSetColumnIndex(4);
      const bool can_up = (i > 0);
      const bool can_down = (i + 1 < static_cast<int>(colony->construction_queue.size()));
      if (!can_up) ImGui::BeginDisabled();
      if (ImGui::SmallButton(("Up##const_up_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
        move_from = i;
        move_to = i - 1;
      }
      if (!can_up) ImGui::EndDisabled();

      ImGui::SameLine();
      if (!can_down) ImGui::BeginDisabled();
      if (ImGui::SmallButton(("Dn##const_dn_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
        move_from = i;
        move_to = i + 1;
      }
      if (!can_down) ImGui::EndDisabled();

      ImGui::TableSetColumnIndex(5);
      if (ImGui::SmallButton(("Del##const_del_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
        delete_idx = i;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Delete this build order. If minerals were already paid for the current unit, they will be refunded.");
      }
    }

    // Extra drop target at end: move to end.
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::TextDisabled("Drop here to move to end");
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("N4X_CONSTRUCTION_ORDER_IDX")) {
        if (payload && payload->DataSize == sizeof(int)) {
          const int src = *static_cast<const int*>(payload->Data);
          move_from = src;
          move_to = static_cast<int>(colony->construction_queue.size());
        }
      }
      ImGui::EndDragDropTarget();
    }

    ImGui::EndTable();
  }

  if (delete_idx >= 0) {
    sim.delete_construction_order(colony->id, delete_idx, true);
  }
  if (move_from >= 0 && move_to >= 0) {
    sim.move_construction_order(colony->id, move_from, move_to);
  }
}

        // Enqueue new construction
        const auto* fac_for_colony = find_ptr(s.factions, colony->faction_id);
        static int inst_sel = 0;
        static int inst_qty = 1;
        static std::string inst_status;

        std::vector<std::string> buildable_installations;
        if (fac_for_colony) {
          for (const auto& id : fac_for_colony->unlocked_installations) {
            if (!sim.is_installation_buildable_for_faction(fac_for_colony->id, id)) continue;
            buildable_installations.push_back(id);
          }
        } else {
          for (const auto& [id, _] : sim.content().installations) buildable_installations.push_back(id);
        }
        std::sort(buildable_installations.begin(), buildable_installations.end());

        if (buildable_installations.empty()) {
          ImGui::TextDisabled("No buildable installations unlocked");
        } else {
          inst_sel = std::clamp(inst_sel, 0, static_cast<int>(buildable_installations.size()) - 1);

          // Build labels
          std::vector<std::string> label_storage;
          std::vector<const char*> labels;
          label_storage.reserve(buildable_installations.size());
          labels.reserve(buildable_installations.size());

          for (const auto& id : buildable_installations) {
            const auto it = sim.content().installations.find(id);
            const std::string nm = (it == sim.content().installations.end()) ? id : it->second.name;
            label_storage.push_back(nm + "##" + id);
          }
          for (const auto& s2 : label_storage) labels.push_back(s2.c_str());

          ImGui::Combo("Installation", &inst_sel, labels.data(), static_cast<int>(labels.size()));
          ImGui::InputInt("Qty", &inst_qty);
          inst_qty = std::clamp(inst_qty, 1, 100);

          const std::string chosen_id = buildable_installations[inst_sel];
          if (const auto it = sim.content().installations.find(chosen_id); it != sim.content().installations.end()) {
            const InstallationDef& def = it->second;
            ImGui::Text("Cost: %.0f CP", def.construction_cost);
            if (!def.build_costs.empty()) {
              ImGui::Text("Mineral costs:");
              for (const auto& [mineral, cost] : def.build_costs) {
                ImGui::BulletText("%s: %.0f", mineral.c_str(), cost);
              }
            }
          }

          if (ImGui::Button("Enqueue construction")) {
            if (sim.enqueue_installation_build(colony->id, chosen_id, inst_qty)) {
              inst_status = "Enqueued: " + chosen_id + " x" + std::to_string(inst_qty);
            } else {
              inst_status = "Failed to enqueue (locked or invalid)";
            }
          }
          if (!inst_status.empty()) {
            ImGui::TextDisabled("%s", inst_status.c_str());
          }
        }

        ImGui::Separator();
        ImGui::Text("Shipyard");

        const InstallationDef* shipyard_def = nullptr;
        if (const auto it = sim.content().installations.find("shipyard"); it != sim.content().installations.end()) {
          shipyard_def = &it->second;
        }

        const auto it_yard = colony->installations.find("shipyard");
        const bool has_yard = (it_yard != colony->installations.end() && it_yard->second > 0);
        if (!has_yard) {
          ImGui::TextDisabled("No shipyard present");
          ImGui::EndTabItem();
        } else {
          if (shipyard_def && !shipyard_def->build_costs_per_ton.empty()) {
            ImGui::Text("Build costs (per ton)");
            for (const auto& [mineral, cost_per_ton] : shipyard_def->build_costs_per_ton) {
              ImGui::BulletText("%s: %.2f", mineral.c_str(), cost_per_ton);
            }
            ImGui::Spacing();
          } else {
            ImGui::TextDisabled("Build costs: (free / not configured)");
          }

const int shipyard_count = it_yard->second;
const double build_rate_tpd =
    (shipyard_def && shipyard_def->build_rate_tons_per_day > 0.0)
        ? shipyard_def->build_rate_tons_per_day * static_cast<double>(shipyard_count)
        : 0.0;

if (colony->shipyard_queue.empty()) {
  ImGui::TextDisabled("Queue empty");
} else {
  int delete_idx = -1;
  int move_from = -1;
  int move_to = -1;

  ImGui::TextDisabled("Drag+drop to reorder.");

  const ImGuiTableFlags qflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                 ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
  if (ImGui::BeginTable("shipyard_queue_table", 6, qflags)) {
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Remaining", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Move", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < static_cast<int>(colony->shipyard_queue.size()); ++i) {
      const auto& bo = colony->shipyard_queue[static_cast<std::size_t>(i)];
      const bool is_refit = (bo.refit_ship_id != kInvalidId);
      const Ship* refit_ship = is_refit ? find_ptr(s.ships, bo.refit_ship_id) : nullptr;

      const auto* d = sim.find_design(bo.design_id);
      const std::string design_nm = d ? d->name : bo.design_id;

      std::string nm = design_nm;
      if (is_refit) {
        const std::string ship_nm =
            refit_ship ? refit_ship->name : ("Ship #" + std::to_string(static_cast<int>(bo.refit_ship_id)));
        nm = "REFIT: " + ship_nm + " -> " + design_nm;
      }

      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%d", i);

      ImGui::TableSetColumnIndex(1);
      const std::string row_id = "##shipyard_row_" + std::to_string(static_cast<unsigned long long>(i));
      ImGui::Selectable((nm + row_id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("N4X_SHIPYARD_ORDER_IDX", &i, sizeof(int));
        ImGui::Text("Move: %s", nm.c_str());
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("N4X_SHIPYARD_ORDER_IDX")) {
          if (payload && payload->DataSize == sizeof(int)) {
            const int src = *static_cast<const int*>(payload->Data);
            move_from = src;
            move_to = i;
          }
        }
        ImGui::EndDragDropTarget();
      }

      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%.1f tons", bo.tons_remaining);

      ImGui::TableSetColumnIndex(3);

      // Stalls that are specific to refits.
      std::string stall_reason;
      if (is_refit) {
        if (!refit_ship) {
          stall_reason = "ship missing";
        } else if (!sim.is_ship_docked_at_colony(refit_ship->id, colony->id)) {
          stall_reason = "ship not docked";
        }
      }

      if (build_rate_tpd > 1e-9 && stall_reason.empty()) {
        const double eta = bo.tons_remaining / build_rate_tpd;
        ImGui::TextDisabled("ETA: %.0f days", eta);
      } else if (!stall_reason.empty()) {
        ImGui::TextDisabled("ETA: (stalled)");
      } else {
        ImGui::TextDisabled("ETA: (unknown)");
      }

      if (shipyard_def && !shipyard_def->build_costs_per_ton.empty()) {
        // Remaining mineral costs for this order.
        std::string cost_line;
        for (const auto& [mineral, cost_per_ton] : shipyard_def->build_costs_per_ton) {
          if (cost_per_ton <= 0.0) continue;
          const double remaining = bo.tons_remaining * cost_per_ton;
          if (!cost_line.empty()) cost_line += ", ";
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.1f", remaining);
          cost_line += mineral + " " + buf;
        }
        if (!cost_line.empty()) {
          ImGui::TextDisabled("Remaining: %s", cost_line.c_str());
        }

        if (stall_reason.empty()) {
          // Simple stall hint: if any required mineral is at 0, the shipyard cannot progress.
          std::string missing;
          for (const auto& [mineral, cost_per_ton] : shipyard_def->build_costs_per_ton) {
            if (cost_per_ton <= 0.0) continue;
            const auto it = colony->minerals.find(mineral);
            const double have = (it == colony->minerals.end()) ? 0.0 : it->second;
            if (have <= 1e-9) {
              missing = mineral;
              break;
            }
          }
          if (!missing.empty()) {
            stall_reason = "need " + missing;
          }
        }
      }

      if (!stall_reason.empty()) {
        ImGui::TextDisabled("STALLED (%s)", stall_reason.c_str());
      }

      ImGui::TableSetColumnIndex(4);
      const bool can_up = (i > 0);
      const bool can_down = (i + 1 < static_cast<int>(colony->shipyard_queue.size()));
      if (!can_up) ImGui::BeginDisabled();
      if (ImGui::SmallButton(("Up##yard_up_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
        move_from = i;
        move_to = i - 1;
      }
      if (!can_up) ImGui::EndDisabled();

      ImGui::SameLine();
      if (!can_down) ImGui::BeginDisabled();
      if (ImGui::SmallButton(("Dn##yard_dn_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
        move_from = i;
        move_to = i + 1;
      }
      if (!can_down) ImGui::EndDisabled();

      ImGui::TableSetColumnIndex(5);
      if (ImGui::SmallButton(("Del##yard_del_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
        delete_idx = i;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Delete this ship build order. No refunds (prototype).");
      }
    }

    // Extra drop target at end: move to end.
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::TextDisabled("Drop here to move to end");
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("N4X_SHIPYARD_ORDER_IDX")) {
        if (payload && payload->DataSize == sizeof(int)) {
          const int src = *static_cast<const int*>(payload->Data);
          move_from = src;
          move_to = static_cast<int>(colony->shipyard_queue.size());
        }
      }
      ImGui::EndDragDropTarget();
    }

    ImGui::EndTable();
  }

  if (delete_idx >= 0) {
    sim.delete_shipyard_order(colony->id, delete_idx);
  }
  if (move_from >= 0 && move_to >= 0) {
    sim.move_shipyard_order(colony->id, move_from, move_to);
  }
}



          static int selected_design_idx = 0;
          const auto ids = sorted_buildable_design_ids(sim, colony->faction_id);
          if (!ids.empty()) {
            selected_design_idx = std::clamp(selected_design_idx, 0, static_cast<int>(ids.size()) - 1);
            std::vector<const char*> labels;
            labels.reserve(ids.size());
            for (const auto& id : ids) labels.push_back(id.c_str());
            ImGui::Combo("Design", &selected_design_idx, labels.data(), static_cast<int>(labels.size()));
            if (ImGui::Button("Enqueue build")) {
              sim.enqueue_build(colony->id, ids[selected_design_idx]);
            }
          }

          ImGui::SeparatorText("Refit ship");

          // Candidate ships: owned, docked here, not in fleets, not already queued for refit.
          std::unordered_set<Id> already_refitting;
          for (const auto& bo : colony->shipyard_queue) {
            if (bo.refit_ship_id != kInvalidId) already_refitting.insert(bo.refit_ship_id);
          }

          std::vector<Id> docked_ships;
          if (const auto* body = find_ptr(s.bodies, colony->body_id)) {
            if (const auto* sys = find_ptr(s.systems, body->system_id)) {
              for (Id sid : sys->ships) {
                const Ship* sh = find_ptr(s.ships, sid);
                if (!sh) continue;
                if (sh->faction_id != colony->faction_id) continue;
                if (already_refitting.count(sid)) continue;
                if (sim.fleet_for_ship(sid) != kInvalidId) continue;
                if (!sim.is_ship_docked_at_colony(sid, colony->id)) continue;
                docked_ships.push_back(sid);
              }
            }
          }
          std::sort(docked_ships.begin(), docked_ships.end());

          static int refit_ship_sel = 0;
          static int refit_design_sel = 0;
          static std::string refit_status;

          if (docked_ships.empty()) {
            ImGui::TextDisabled("No eligible ships docked here (must be detached from fleets).");
          } else if (ids.empty()) {
            ImGui::TextDisabled("No buildable designs available.");
          } else {
            refit_ship_sel = std::clamp(refit_ship_sel, 0, static_cast<int>(docked_ships.size()) - 1);
            refit_design_sel = std::clamp(refit_design_sel, 0, static_cast<int>(ids.size()) - 1);

            // Ship label list
            std::vector<std::string> ship_label_storage;
            std::vector<const char*> ship_labels;
            ship_label_storage.reserve(docked_ships.size());
            ship_labels.reserve(docked_ships.size());
            for (Id sid : docked_ships) {
              const Ship* sh = find_ptr(s.ships, sid);
              ship_label_storage.push_back((sh ? sh->name : std::string("Ship ") + std::to_string((int)sid)) + "##" +
                                           std::to_string((int)sid));
            }
            for (const auto& s2 : ship_label_storage) ship_labels.push_back(s2.c_str());

            // Design label list (reuse ids already built for the build enqueue combo)
            std::vector<const char*> design_labels;
            design_labels.reserve(ids.size());
            for (const auto& id : ids) design_labels.push_back(id.c_str());

            ImGui::Combo("Ship", &refit_ship_sel, ship_labels.data(), static_cast<int>(ship_labels.size()));
            ImGui::Combo("Target design", &refit_design_sel, design_labels.data(), static_cast<int>(design_labels.size()));

            const Id chosen_ship = docked_ships[refit_ship_sel];
            const std::string chosen_design = ids[refit_design_sel];

            const double work_tons = sim.estimate_refit_tons(chosen_ship, chosen_design);
            if (build_rate_tpd > 1e-9 && work_tons > 0.0) {
              ImGui::TextDisabled("Work: %.1f tons (multiplier %.2f)  |  Base ETA: %.0f days",
                                  work_tons, sim.cfg().ship_refit_tons_multiplier, work_tons / build_rate_tpd);
            } else if (work_tons > 0.0) {
              ImGui::TextDisabled("Work: %.1f tons (multiplier %.2f)", work_tons, sim.cfg().ship_refit_tons_multiplier);
            }

            if (ImGui::Button("Enqueue refit")) {
              std::string err;
              if (sim.enqueue_refit(colony->id, chosen_ship, chosen_design, &err)) {
                refit_status = "Queued refit.";
              } else {
                refit_status = "Failed: " + err;
              }
            }
            if (!refit_status.empty()) {
              ImGui::TextDisabled("%s", refit_status.c_str());
            }
          }

          ImGui::EndTabItem();
        }
      } else {
        ImGui::TextDisabled("Selected colony no longer exists");
        ImGui::EndTabItem();
      }
    }


    // --- Logistics tab ---
    if (ImGui::BeginTabItem("Logistics")) {
      if (!selected_faction) {
        ImGui::TextDisabled("No faction selected.");
      } else {
        ImGui::SeparatorText("Auto-freight");
        ImGui::TextWrapped(
            "Enable Auto-freight on cargo ships to have them automatically haul minerals between your colonies "
            "whenever they are idle. Auto-freight tries to relieve mineral shortages that stall shipyards or "
            "unpaid construction orders.");

        if (ImGui::Button("Enable auto-freight for all freighters")) {
          for (auto& [sid, ship] : s.ships) {
            if (ship.faction_id != selected_faction_id) continue;
            const auto* d = sim.find_design(ship.design_id);
            if (!d || d->cargo_tons <= 0.0) continue;
            if (sim.fleet_for_ship(sid) != kInvalidId) continue;
            ship.auto_freight = true;
            ship.auto_explore = false;
          }
        }
        ImGui::SameLine();
        if (ImGui::Button("Disable auto-freight for this faction")) {
          for (auto& [sid, ship] : s.ships) {
            if (ship.faction_id != selected_faction_id) continue;
            ship.auto_freight = false;
          }
        }

        ImGui::SeparatorText("Colony mineral shortfalls");
        const auto needs = sim.logistics_needs_for_faction(selected_faction_id);
        struct NeedRow {
          Id colony_id{kInvalidId};
          std::string mineral;
          double missing{0.0};
          std::string reason;
        };
        std::vector<NeedRow> rows;
        rows.reserve(needs.size());
        for (const auto& n : needs) {
          if (n.missing_tons <= 1e-9) continue;
          std::string reason = (n.kind == LogisticsNeedKind::Shipyard) ? "Shipyard" : "Construction";
          if (n.kind == LogisticsNeedKind::Construction && !n.context_id.empty()) reason += (":" + n.context_id);
          rows.push_back(NeedRow{n.colony_id, n.mineral, n.missing_tons, reason});
        }
        std::sort(rows.begin(), rows.end(), [](const NeedRow& a, const NeedRow& b) {
          if (a.missing != b.missing) return a.missing > b.missing;
          if (a.colony_id != b.colony_id) return a.colony_id < b.colony_id;
          return a.mineral < b.mineral;
        });

        if (rows.empty()) {
          ImGui::TextDisabled("No mineral shortfalls detected.");
        } else if (ImGui::BeginTable("##logistics_needs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
          ImGui::TableSetupColumn("Colony");
          ImGui::TableSetupColumn("Mineral");
          ImGui::TableSetupColumn("Missing (t)");
          ImGui::TableSetupColumn("Reason");
          ImGui::TableHeadersRow();

          for (const auto& r : rows) {
            const Colony* c = find_ptr(s.colonies, r.colony_id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (c) {
              ImGui::TextUnformatted(c->name.c_str());
            } else {
              ImGui::Text("Colony %d", (int)r.colony_id);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(r.mineral.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", r.missing);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(r.reason.c_str());
          }
          ImGui::EndTable();
        }

        ImGui::SeparatorText("Auto-freight ships");
        std::vector<Id> ship_ids_sorted;
        ship_ids_sorted.reserve(s.ships.size());
        for (const auto& [sid, _] : s.ships) ship_ids_sorted.push_back(sid);
        std::sort(ship_ids_sorted.begin(), ship_ids_sorted.end());

        int shown = 0;
        if (ImGui::BeginTable("##logistics_ships", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
          ImGui::TableSetupColumn("Ship");
          ImGui::TableSetupColumn("System");
          ImGui::TableSetupColumn("Next order");
          ImGui::TableSetupColumn("Cargo");
          ImGui::TableSetupColumn("Notes");
          ImGui::TableHeadersRow();

          for (Id sid : ship_ids_sorted) {
            const Ship* sh = find_ptr(s.ships, sid);
            if (!sh) continue;
            if (sh->faction_id != selected_faction_id) continue;
            if (!sh->auto_freight) continue;

            const auto* d = sim.find_design(sh->design_id);
            const double cap = d ? std::max(0.0, d->cargo_tons) : 0.0;
            double used = 0.0;
            for (const auto& [_, tons] : sh->cargo) used += std::max(0.0, tons);

            const StarSystem* sys = find_ptr(s.systems, sh->system_id);
            const bool in_fleet = (sim.fleet_for_ship(sid) != kInvalidId);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(sh->name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(sys ? sys->name.c_str() : "?");
            ImGui::TableSetColumnIndex(2);
            const ShipOrders* so = find_ptr(s.ship_orders, sid);
            if (!so || so->queue.empty()) {
              ImGui::TextDisabled("Idle");
            } else {
              std::string order_str = order_to_string(so->queue.front());
              if (so->repeat) order_str += " (repeat)";
              ImGui::TextUnformatted(order_str.c_str());
            }
            ImGui::TableSetColumnIndex(3);
            if (cap > 0.0) {
              ImGui::Text("%.1f / %.1f", used, cap);
            } else {
              ImGui::TextDisabled("-");
            }
            ImGui::TableSetColumnIndex(4);
            if (in_fleet) {
              ImGui::TextDisabled("In fleet (no auto tasks)");
            }
            ++shown;
          }
          ImGui::EndTable();
        }
        if (shown == 0) ImGui::TextDisabled("No ships have Auto-freight enabled.");
      }
      ImGui::EndTabItem();
    }

    // --- Research tab ---
    if (ImGui::BeginTabItem("Research")) {
      if (factions.empty() || !selected_faction) {
        ImGui::TextDisabled("No factions available");
        ImGui::EndTabItem();
      } else {
        ImGui::Text("Faction");
        std::vector<const char*> fac_labels;
        fac_labels.reserve(factions.size());
        for (const auto& p : factions) fac_labels.push_back(p.second.c_str());
        ImGui::Combo("##faction", &faction_combo_idx, fac_labels.data(), static_cast<int>(fac_labels.size()));

        ImGui::Separator();
        ImGui::Text("Research Points (bank): %.1f", selected_faction->research_points);

        // Faction control / AI profile.
        {
          const char* labels[] = {"Player (Manual)", "AI (Passive)", "AI (Explorer)", "AI (Pirate Raiders)"};
          auto to_idx = [](FactionControl c) {
            switch (c) {
              case FactionControl::Player: return 0;
              case FactionControl::AI_Passive: return 1;
              case FactionControl::AI_Explorer: return 2;
              case FactionControl::AI_Pirate: return 3;
            }
            return 0;
          };
          auto from_idx = [](int idx) {
            switch (idx) {
              case 1: return FactionControl::AI_Passive;
              case 2: return FactionControl::AI_Explorer;
              case 3: return FactionControl::AI_Pirate;
              default: return FactionControl::Player;
            }
          };

          int control_idx = to_idx(selected_faction->control);
          if (ImGui::Combo("Control", &control_idx, labels, IM_ARRAYSIZE(labels))) {
            selected_faction->control = from_idx(control_idx);
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "AI profiles generate orders for idle ships.\n"
                "Ships with queued orders are left alone.");
          }
        }

        // Active
        if (!selected_faction->active_research_id.empty()) {
          const auto it = sim.content().techs.find(selected_faction->active_research_id);
          const TechDef* tech = (it == sim.content().techs.end()) ? nullptr : &it->second;
          const double cost = tech ? tech->cost : 0.0;
          ImGui::Text("Active: %s", tech ? tech->name.c_str() : selected_faction->active_research_id.c_str());
          if (cost > 0.0) {
            const float frac = static_cast<float>(std::clamp(selected_faction->active_research_progress / cost, 0.0, 1.0));
            ImGui::ProgressBar(frac, ImVec2(-1, 0), (std::to_string(static_cast<int>(selected_faction->active_research_progress)) +
                                                    " / " + std::to_string(static_cast<int>(cost)))
                                                       .c_str());
          }
        } else {
          ImGui::TextDisabled("Active: (none)");
        }

        ImGui::Separator();
        ImGui::Text("Queue");
        if (selected_faction->research_queue.empty()) {
          ImGui::TextDisabled("(empty)");
        } else {
          for (size_t i = 0; i < selected_faction->research_queue.size(); ++i) {
            const auto& id = selected_faction->research_queue[i];
            const auto it = sim.content().techs.find(id);
            const char* nm = (it == sim.content().techs.end()) ? id.c_str() : it->second.name.c_str();
            ImGui::BulletText("%s", nm);
          }
        }

        ImGui::Separator();
        ImGui::Text("Available techs");

        // Compute available lis.
        std::vector<std::string> available;
        for (const auto& [tid, tech] : sim.content().techs) {
          if (vec_contains(selected_faction->known_techs, tid)) continue;
          if (!prereqs_met(*selected_faction, tech)) continue;
          available.push_back(tid);
        }
        std::sort(available.begin(), available.end());

        static int tech_sel = 0;
        if (!available.empty()) tech_sel = std::clamp(tech_sel, 0, static_cast<int>(available.size()) - 1);

        if (available.empty()) {
          ImGui::TextDisabled("(none)");
        } else {
          // List box
          if (ImGui::BeginListBox("##techs", ImVec2(-1, 180))) {
            for (int i = 0; i < static_cast<int>(available.size()); ++i) {
              const bool sel = (tech_sel == i);
              const auto it = sim.content().techs.find(available[i]);
              const std::string label = (it == sim.content().techs.end()) ? available[i] : (it->second.name + "##" + available[i]);
              if (ImGui::Selectable(label.c_str(), sel)) tech_sel = i;
            }
            ImGui::EndListBox();
          }

          const std::string chosen_id = available[tech_sel];
          const auto it = sim.content().techs.find(chosen_id);
          const TechDef* chosen = (it == sim.content().techs.end()) ? nullptr : &it->second;

          if (chosen) {
            ImGui::Text("Cost: %.0f", chosen->cost);
            if (!chosen->effects.empty()) {
              ImGui::Text("Effects:");
              for (const auto& eff : chosen->effects) {
                ImGui::BulletText("%s: %s", eff.type.c_str(), eff.value.c_str());
              }
            }
          }

          if (ImGui::Button("Set Active")) {
            selected_faction->active_research_id = chosen_id;
            selected_faction->active_research_progress = 0.0;
          }
          ImGui::SameLine();
          if (ImGui::Button("Add to Queue")) {
            selected_faction->research_queue.push_back(chosen_id);
          }
        }

        ImGui::EndTabItem();
      }
    }


    // --- Diplomacy tab ---
    if (ImGui::BeginTabItem("Diplomacy")) {
      if (factions.empty() || !selected_faction) {
        ImGui::TextDisabled("No factions available");
      } else {
        ImGui::Text("Faction");
        std::vector<const char*> fac_labels;
        fac_labels.reserve(factions.size());
        for (const auto& p : factions) fac_labels.push_back(p.second.c_str());
        ImGui::Combo("##faction_diplomacy", &faction_combo_idx, fac_labels.data(), static_cast<int>(fac_labels.size()));

        ImGui::Separator();
        ImGui::TextWrapped(
            "Diplomatic stances are currently used as simple rules-of-engagement: ships will only auto-engage "
            "factions they consider Hostile. Issuing an Attack order against a non-hostile faction will automatically "
            "set the relationship to Hostile once contact is confirmed.");

        static bool reciprocal = true;
        ImGui::Checkbox("Reciprocal edits (set both directions)", &reciprocal);

        if (ImGui::Button("Set all to Neutral")) {
          for (const auto& [fid, _] : factions) {
            if (fid == selected_faction_id) continue;
            sim.set_diplomatic_status(selected_faction_id, fid, DiplomacyStatus::Neutral, reciprocal,
                                      /*push_event=*/true);
          }
        }
        ImGui::SameLine();
        if (ImGui::Button("Set all to Friendly")) {
          for (const auto& [fid, _] : factions) {
            if (fid == selected_faction_id) continue;
            sim.set_diplomatic_status(selected_faction_id, fid, DiplomacyStatus::Friendly, reciprocal,
                                      /*push_event=*/true);
          }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset all to Hostile (clear overrides)")) {
          for (const auto& [fid, _] : factions) {
            if (fid == selected_faction_id) continue;
            sim.set_diplomatic_status(selected_faction_id, fid, DiplomacyStatus::Hostile, reciprocal,
                                      /*push_event=*/true);
          }
        }

        ImGui::Spacing();

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##diplomacy_table", 3, flags)) {
          ImGui::TableSetupColumn("Other faction");
          ImGui::TableSetupColumn("Your stance");
          ImGui::TableSetupColumn("Their stance");
          ImGui::TableHeadersRow();

          const char* opts[] = {"Hostile", "Neutral", "Friendly"};

          for (const auto& [other_id, other_name] : factions) {
            if (other_id == selected_faction_id) continue;
            const DiplomacyStatus out_st = sim.diplomatic_status(selected_faction_id, other_id);
            const DiplomacyStatus in_st = sim.diplomatic_status(other_id, selected_faction_id);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(other_name.c_str());

            ImGui::TableSetColumnIndex(1);
            int combo_idx = diplomacy_status_to_combo_idx(out_st);
            const std::string combo_id = "##dip_" + std::to_string(static_cast<unsigned long long>(selected_faction_id)) +
                                         "_" + std::to_string(static_cast<unsigned long long>(other_id));
            if (ImGui::Combo(combo_id.c_str(), &combo_idx, opts, IM_ARRAYSIZE(opts))) {
              sim.set_diplomatic_status(selected_faction_id, other_id, diplomacy_status_from_combo_idx(combo_idx),
                                        reciprocal, /*push_event=*/true);
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(diplomacy_status_label(in_st));
          }

          ImGui::EndTable();
        }
      }
      ImGui::EndTabItem();
    }

    // --- Ship design tab ---
    if (ImGui::BeginTabItem("Design")) {
      if (factions.empty() || !selected_faction) {
        ImGui::TextDisabled("No factions available");
        ImGui::EndTabItem();
      } else {
        ImGui::Text("Design for faction");
        std::vector<const char*> fac_labels;
        fac_labels.reserve(factions.size());
        for (const auto& p : factions) fac_labels.push_back(p.second.c_str());
        ImGui::Combo("##faction_design", &faction_combo_idx, fac_labels.data(), static_cast<int>(fac_labels.size()));

        ImGui::Separator();
        ImGui::Text("Existing designs");
        const auto all_ids = sorted_all_design_ids(sim);

        static int design_sel = 0;
        if (!all_ids.empty()) design_sel = std::clamp(design_sel, 0, static_cast<int>(all_ids.size()) - 1);

        if (ImGui::BeginListBox("##designs", ImVec2(-1, 160))) {
          for (int i = 0; i < static_cast<int>(all_ids.size()); ++i) {
            const bool sel = (design_sel == i);
            const auto* d = sim.find_design(all_ids[i]);
            const std::string label = d ? (d->name + "##" + all_ids[i]) : all_ids[i];
            if (ImGui::Selectable(label.c_str(), sel)) design_sel = i;
          }
          ImGui::EndListBox();
        }

        if (!all_ids.empty()) {
          const auto* d = sim.find_design(all_ids[design_sel]);
          if (d) {
            ImGui::Text("ID: %s", d->id.c_str());
            ImGui::Text("Role: %s", ship_role_label(d->role));
            ImGui::Text("Mass: %.0f t", d->mass_tons);
            ImGui::Text("Speed: %.1f km/s", d->speed_km_s);
            ImGui::Text("HP: %.0f", d->max_hp);
            // A design isn't carrying cargo; only an instantiated ship has a cargo manifes.
            const double cargo_used_tons = 0.0;
            ImGui::Text("Cargo: %.0f / %.0f t", cargo_used_tons, d->cargo_tons);
            ImGui::Text("Sensor: %.0f mkm", d->sensor_range_mkm);
            if (d->weapon_damage > 0.0) ImGui::Text("Weapons: %.1f (range %.1f)", d->weapon_damage, d->weapon_range_mkm);
          }
        }

        ImGui::Separator();
        ImGui::Text("Create / edit custom design");

        static char new_id[64] = "";
        static char new_name[64] = "";
        static int role_idx = 0;
        static std::vector<std::string> comp_list;
        static std::string status;

        const char* roles[] = {"Freighter", "Surveyor", "Combatant"};
        role_idx = std::clamp(role_idx, 0, 2);

        // --- Editor helpers ---
        // Seed the editor from the currently selected design (either load the custom
        // design for editing, or clone any design to a new custom id).
        if (!all_ids.empty()) {
          const auto* seed = sim.find_design(all_ids[design_sel]);
          if (seed) {
            const bool is_custom = (sim.state().custom_designs.find(seed->id) != sim.state().custom_designs.end());
            const bool is_builtin = (sim.content().designs.find(seed->id) != sim.content().designs.end());

            auto copy_to_buf = [](char* dst, size_t dst_size, const std::string& src) {
              if (!dst || dst_size == 0) return;
              std::snprintf(dst, dst_size, "%s", src.c_str());
              dst[dst_size - 1] = '\0';
            };

            auto role_to_idx = [](ShipRole r) -> int {
              switch (r) {
                case ShipRole::Freighter: return 0;
                case ShipRole::Surveyor: return 1;
                case ShipRole::Combatant: return 2;
                default: return 0;
              }
            };

            auto make_unique_custom_id = [&](const std::string& base) -> std::string {
              std::string stem = base.empty() ? "custom_design" : base;
              // Built-in ids can't be used for custom upserts.
              if (sim.content().designs.find(stem) != sim.content().designs.end()) stem += "_custom";

              std::string out = stem;
              int n = 2;
              while (sim.content().designs.find(out) != sim.content().designs.end() ||
                     sim.state().custom_designs.find(out) != sim.state().custom_designs.end()) {
                out = stem + std::to_string(n++);
              }
              return out;
            };

            ImGui::Spacing();
            ImGui::TextDisabled("Seed editor from selected design");

            if (is_custom) {
              if (ImGui::SmallButton("Load custom##design_load")) {
                copy_to_buf(new_id, sizeof(new_id), seed->id);
                copy_to_buf(new_name, sizeof(new_name), seed->name);
                role_idx = role_to_idx(seed->role);
                comp_list = seed->components;
                status = "Loaded custom design: " + seed->id;
              }
              if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Loads the selected custom design into the editor for editing.");
              }
              ImGui::SameLine();
            }

            const char* clone_label = is_builtin ? "Clone built-in##design_clone" : "Clone##design_clone";
            if (ImGui::SmallButton(clone_label)) {
              const std::string new_custom_id = make_unique_custom_id(seed->id);
              copy_to_buf(new_id, sizeof(new_id), new_custom_id);
              copy_to_buf(new_name, sizeof(new_name), seed->name);
              role_idx = role_to_idx(seed->role);
              comp_list = seed->components;
              status = "Cloned design: " + seed->id + " -> " + new_custom_id;
            }
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip(is_builtin ? "Built-in designs can't be overwritten; this makes a new custom id."
                                           : "Copies the selected design into the editor under a new id.");
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##design_clear")) {
              new_id[0] = '\0';
              new_name[0] = '\0';
              role_idx = 0;
              comp_list.clear();
              status = "Cleared editor.";
            }
          }
        }

        ImGui::InputText("Design ID", new_id, sizeof(new_id));
        ImGui::InputText("Name", new_name, sizeof(new_name));
        ImGui::Combo("Role", &role_idx, roles, IM_ARRAYSIZE(roles));

        ImGui::Spacing();
        ImGui::Text("Components");

        ImGui::SameLine();
        if (ImGui::SmallButton("Sort##comp_sort")) {
          auto type_rank = [](ComponentType t) {
            switch (t) {
              case ComponentType::Engine: return 0;
              case ComponentType::Reactor: return 1;
              case ComponentType::Cargo: return 2;
              case ComponentType::Sensor: return 3;
              case ComponentType::Weapon: return 4;
              case ComponentType::Armor: return 5;
              default: return 6;
            }
          };

          std::sort(comp_list.begin(), comp_list.end(), [&](const std::string& a, const std::string& b) {
            const auto ita = sim.content().components.find(a);
            const auto itb = sim.content().components.find(b);
            const ComponentDef* ca = (ita == sim.content().components.end()) ? nullptr : &ita->second;
            const ComponentDef* cb = (itb == sim.content().components.end()) ? nullptr : &itb->second;

            const int ra = ca ? type_rank(ca->type) : 999;
            const int rb = cb ? type_rank(cb->type) : 999;
            if (ra != rb) return ra < rb;

            const std::string& na = ca ? ca->name : a;
            const std::string& nb = cb ? cb->name : b;
            if (na != nb) return na < nb;
            return a < b;
          });
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##comp_clear")) {
          comp_list.clear();
        }

        // Show current components with remove buttons.
        if (comp_list.empty()) {
          ImGui::TextDisabled("(none)");
        }
        for (int i = 0; i < static_cast<int>(comp_list.size());) {
          const auto& cid = comp_list[static_cast<size_t>(i)];
          const auto it = sim.content().components.find(cid);
          const char* cname = (it == sim.content().components.end()) ? cid.c_str() : it->second.name.c_str();
          ImGui::BulletText("%s", cname);
          ImGui::SameLine();
          if (ImGui::SmallButton(("Remove##" + std::to_string(i)).c_str())) {
            comp_list.erase(comp_list.begin() + i);
            continue; // don't advance index
          }
          ++i;
        }

        // Available components (unlocked)
        ImGui::Spacing();
        ImGui::Text("Add component");

        static int comp_filter = 0; // 0=All
        const char* filters[] = {"All", "Engine", "Cargo", "Sensor", "Reactor", "Weapon", "Armor"};
        ImGui::Combo("Filter", &comp_filter, filters, IM_ARRAYSIZE(filters));

        static char comp_search[64] = "";
        ImGui::InputText("Search##comp_search", comp_search, sizeof(comp_search));
        ImGui::SameLine();
        ImGui::TextDisabled("(name or id...)");

        std::vector<std::string> avail_components;
        for (const auto& [cid, cdef] : sim.content().components) {
          // Only show unlocked for this faction (unless it's already in the design).
          const bool unlocked = vec_contains(selected_faction->unlocked_components, cid);
          const bool in_design = vec_contains(comp_list, cid);
          if (!unlocked && !in_design) continue;

          if (comp_search[0] != '\0') {
            if (!case_insensitive_contains(cid, comp_search) && !case_insensitive_contains(cdef.name, comp_search)) continue;
          }

          if (comp_filter != 0) {
            const ComponentType desired =
                (comp_filter == 1) ? ComponentType::Engine
                : (comp_filter == 2) ? ComponentType::Cargo
                : (comp_filter == 3) ? ComponentType::Sensor
                : (comp_filter == 4) ? ComponentType::Reactor
                : (comp_filter == 5) ? ComponentType::Weapon
                : (comp_filter == 6) ? ComponentType::Armor
                                    : ComponentType::Unknown;
            if (cdef.type != desired) continue;
          }
          avail_components.push_back(cid);
        }
        std::sort(avail_components.begin(), avail_components.end());

        static int add_comp_idx = 0;
        if (!avail_components.empty()) add_comp_idx = std::clamp(add_comp_idx, 0, static_cast<int>(avail_components.size()) - 1);

        if (avail_components.empty()) {
          ImGui::TextDisabled("No unlocked components match filter");
        } else {
          std::vector<const char*> comp_labels;
          comp_labels.reserve(avail_components.size());
          std::vector<std::string> comp_label_storage;
          comp_label_storage.reserve(avail_components.size());

          for (const auto& cid : avail_components) {
            const auto it = sim.content().components.find(cid);
            const auto& cdef = it->second;
            std::string lbl = cdef.name + " (" + component_type_label(cdef.type) + ")##" + cid;
            comp_label_storage.push_back(std::move(lbl));
          }
          for (const auto& s2 : comp_label_storage) comp_labels.push_back(s2.c_str());

          ImGui::Combo("Component", &add_comp_idx, comp_labels.data(), static_cast<int>(comp_labels.size()));

          // Quick preview of the selected component.
          if (!avail_components.empty()) {
            const auto it = sim.content().components.find(avail_components[add_comp_idx]);
            if (it != sim.content().components.end()) {
              const auto& c = it->second;
              ImGui::TextDisabled("Selected: %s (%s)", c.name.c_str(), component_type_label(c.type));
              ImGui::TextDisabled("Mass: %.0f t", c.mass_tons);
              if (c.speed_km_s > 0.0) ImGui::TextDisabled("Speed: %.1f km/s", c.speed_km_s);
              if (c.power > 0.0) ImGui::TextDisabled("Power: %.1f", c.power);
              if (c.cargo_tons > 0.0) ImGui::TextDisabled("Cargo: %.0f t", c.cargo_tons);
              if (c.sensor_range_mkm > 0.0) ImGui::TextDisabled("Sensor: %.0f mkm", c.sensor_range_mkm);
              if (c.weapon_damage > 0.0) ImGui::TextDisabled("Weapon: %.1f (range %.1f)", c.weapon_damage, c.weapon_range_mkm);
              if (c.hp_bonus > 0.0) ImGui::TextDisabled("HP bonus: %.0f", c.hp_bonus);
            }
          }

          if (ImGui::Button("Add")) {
            comp_list.push_back(avail_components[add_comp_idx]);
          }
        }

        // Preview stats
        ShipDesign preview;
        preview.id = new_id;
        preview.name = new_name;
        preview.role = (role_idx == 0) ? ShipRole::Freighter : (role_idx == 1) ? ShipRole::Surveyor : ShipRole::Combatant;
        preview.components = comp_list;
        preview = derive_preview_design(sim.content(), preview);

        ImGui::Separator();
        ImGui::Text("Preview");
        ImGui::Text("Mass: %.0f t", preview.mass_tons);
        ImGui::Text("Speed: %.1f km/s", preview.speed_km_s);
        ImGui::Text("HP: %.0f", preview.max_hp);
        ImGui::Text("Cargo: %.0f t", preview.cargo_tons);
        ImGui::Text("Sensor: %.0f mkm", preview.sensor_range_mkm);
        if (preview.weapon_damage > 0.0) ImGui::Text("Weapons: %.1f (range %.1f)", preview.weapon_damage, preview.weapon_range_mkm);

        if (ImGui::Button("Save custom design")) {
          std::string err;
          if (sim.upsert_custom_design(preview, &err)) {
            status = "Saved custom design: " + preview.id;
          } else {
            status = "Error: " + err;
          }
        }
        if (!status.empty()) {
          ImGui::Spacing();
          ImGui::TextWrapped("%s", status.c_str());
        }

        ImGui::EndTabItem();
      }
    }

    // --- Contacts / intel tab ---
    if (ImGui::BeginTabItem("Contacts")) {
      // Default viewer faction: use selected ship's faction if available, otherwise use the faction combo.
      Id viewer_faction_id = selected_faction_id;
      if (selected_ship != kInvalidId) {
        if (const auto* sh = find_ptr(s.ships, selected_ship)) viewer_faction_id = sh->faction_id;
      }

      Faction* viewer = (viewer_faction_id == kInvalidId) ? nullptr : find_ptr(s.factions, viewer_faction_id);

      if (!viewer) {
        ImGui::TextDisabled("Select a faction (Research tab) or select a ship to view contacts");
        ImGui::EndTabItem();
      } else {
        const auto* sys = find_ptr(s.systems, s.selected_system);
        const char* sys_name = sys ? sys->name.c_str() : "(none)";

        ImGui::Text("Viewer: %s", viewer->name.c_str());
        ImGui::TextDisabled("Contacts are last-known snapshots from sensors; they may be stale.");

        ImGui::Separator();
        ImGui::Checkbox("Fog of war", &ui.fog_of_war);
        ImGui::SameLine();
        ImGui::Checkbox("Show contact markers", &ui.show_contact_markers);

        ImGui::InputInt("Show <= days old", &ui.contact_max_age_days);
        ui.contact_max_age_days = std::clamp(ui.contact_max_age_days, 1, 365);

        static bool only_current_system = true;
        ImGui::Checkbox("Only selected system", &only_current_system);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", sys_name);

        const int now = static_cast<int>(s.date.days_since_epoch());

        struct Row {
          Contact c;
          int age{0};
        };
        std::vector<Row> rows;
        rows.reserve(viewer->ship_contacts.size());

        for (const auto& [_, c] : viewer->ship_contacts) {
          if (only_current_system && c.system_id != s.selected_system) continue;
          const int age = now - c.last_seen_day;
          if (age < 0) continue;
          if (age > ui.contact_max_age_days) continue;
          rows.push_back(Row{c, age});
        }

        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
          if (a.age != b.age) return a.age < b.age; // younger first
          return a.c.ship_id < b.c.ship_id;
        });

        ImGui::Separator();
        ImGui::Text("Contacts: %d", (int)rows.size());

        if (rows.empty()) {
          ImGui::TextDisabled("(none)");
        } else {
          for (const auto& r : rows) {
            const auto* sys2 = find_ptr(s.systems, r.c.system_id);
            const char* sys2_name = sys2 ? sys2->name.c_str() : "(unknown system)";

            std::string title = r.c.last_seen_name.empty() ? ("Contact #" + std::to_string(r.c.ship_id)) : r.c.last_seen_name;
            title += "##contact_" + std::to_string(r.c.ship_id);

            if (ImGui::TreeNode(title.c_str())) {
              ImGui::Text("System: %s", sys2_name);
              ImGui::Text("Age: %d day(s)", r.age);
              ImGui::Text("Last known pos: (%.2f, %.2f) mkm", r.c.last_seen_position_mkm.x, r.c.last_seen_position_mkm.y);
              if (!r.c.last_seen_design_id.empty()) ImGui::Text("Last seen design: %s", r.c.last_seen_design_id.c_str());

              const bool detected_now = sim.is_ship_detected_by_faction(viewer->id, r.c.ship_id);
              ImGui::Text("Currently detected: %s", detected_now ? "yes" : "no");

              if (ImGui::SmallButton(("View system##" + std::to_string(r.c.ship_id)).c_str())) {
                s.selected_system = r.c.system_id;
              }

              // If the player has a ship selected in the same system, offer quick actions.
              if (selected_ship != kInvalidId) {
                const auto* my_ship = find_ptr(s.ships, selected_ship);
                if (my_ship && my_ship->faction_id == viewer->id && my_ship->system_id == r.c.system_id) {
                  ImGui::SameLine();
                  if (ImGui::SmallButton(("Investigate##" + std::to_string(r.c.ship_id)).c_str())) {
                    sim.issue_move_to_point(selected_ship, r.c.last_seen_position_mkm);
                  }

                  ImGui::SameLine();
                  bool hostile = true;
                  std::string btn;
                  if (!detected_now) {
                    btn = "Intercept";
                  } else {
                    hostile = sim.are_factions_hostile(viewer->id, r.c.last_seen_faction_id);
                    btn = hostile ? "Attack" : "Declare War + Attack";
                  }
                  if (ImGui::SmallButton((btn + "##" + std::to_string(r.c.ship_id)).c_str())) {
                    // If not currently detected, this will issue an intercept based on the stored contact snapshot.
                    sim.issue_attack_ship(selected_ship, r.c.ship_id, ui.fog_of_war);
                  }
                  if (detected_now && !hostile && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "This target is not currently Hostile. Issuing an Attack will automatically set the stance to Hostile once contact is confirmed.");
                  }
                }
              }

              ImGui::TreePop();
            }
          }
        }

        ImGui::EndTabItem();
      }
    }

    // --- Event log tab ---
    {
      const std::uint64_t newest_seq = (s.next_event_seq > 0) ? (s.next_event_seq - 1) : 0;
      // UIState isn't persisted; it can be out of sync after New Game / Load.
      if (ui.last_seen_event_seq > newest_seq) ui.last_seen_event_seq = 0;

      int unread = 0;
      for (const auto& ev : s.events) {
        if (ev.seq > ui.last_seen_event_seq) ++unread;
      }

      std::string log_label;
      if (unread > 0) {
        log_label = "Log (" + std::to_string(unread) + ")###log_tab";
      } else {
        log_label = "Log###log_tab";
      }

      if (ImGui::BeginTabItem(log_label.c_str())) {
        // Mark everything up to the newest event as "seen" while the tab is open.
        if (newest_seq > ui.last_seen_event_seq) ui.last_seen_event_seq = newest_seq;

        ImGui::Text("Event log (saved with game)");
        ImGui::TextDisabled("Entries: %d   (unread when opened: %d)", (int)s.events.size(), unread);

        static bool show_info = true;
        static bool show_warn = true;
        static bool show_error = true;
        static int category_idx = 0; // 0=All
        static Id faction_filter = kInvalidId;
        static Id system_filter = kInvalidId;
        static Id ship_filter = kInvalidId;
        static Id colony_filter = kInvalidId;
        static int max_show = 200;
        static char search_buf[128] = "";

        ImGui::Checkbox("Info", &show_info);
        ImGui::SameLine();
        ImGui::Checkbox("Warn", &show_warn);
        ImGui::SameLine();
        ImGui::Checkbox("Error", &show_error);

        // Category filter.
        {
          const char* cats[] = {
              "All",
              "General",
              "Research",
              "Shipyard",
              "Construction",
              "Movement",
              "Combat",
              "Intel",
              "Exploration",
          };
          ImGui::Combo("Category", &category_idx, cats, IM_ARRAYSIZE(cats));
        }

        // Faction filter.
        {
          const auto fac_list = sorted_factions(s);
          const auto* sel = find_ptr(s.factions, faction_filter);
          const char* label = (faction_filter == kInvalidId) ? "All" : (sel ? sel->name.c_str() : "(missing)");

          if (ImGui::BeginCombo("Faction", label)) {
            if (ImGui::Selectable("All", faction_filter == kInvalidId)) faction_filter = kInvalidId;
            for (const auto& [fid, name] : fac_list) {
              if (ImGui::Selectable(name.c_str(), faction_filter == fid)) faction_filter = fid;
            }
            ImGui::EndCombo();
          }
        }

        // Optional context filters.
        {
          // System filter.
          const auto sys_list = sorted_systems(s);
          const auto* sel = find_ptr(s.systems, system_filter);
          const char* label = (system_filter == kInvalidId) ? "All" : (sel ? sel->name.c_str() : "(missing)");
          if (ImGui::BeginCombo("System", label)) {
            if (ImGui::Selectable("All", system_filter == kInvalidId)) system_filter = kInvalidId;
            for (const auto& [sid, name] : sys_list) {
              if (ImGui::Selectable(name.c_str(), system_filter == sid)) system_filter = sid;
            }
            ImGui::EndCombo();
          }

          // Ship filter.
          const auto ship_list = sorted_ships(s);
          const auto* sel_sh = find_ptr(s.ships, ship_filter);
          const char* label_sh = (ship_filter == kInvalidId) ? "All" : (sel_sh ? sel_sh->name.c_str() : "(missing)");
          if (ImGui::BeginCombo("Ship", label_sh)) {
            if (ImGui::Selectable("All", ship_filter == kInvalidId)) ship_filter = kInvalidId;
            for (const auto& [shid, name] : ship_list) {
              if (ImGui::Selectable(name.c_str(), ship_filter == shid)) ship_filter = shid;
            }
            ImGui::EndCombo();
          }

          // Colony filter.
          const auto col_list = sorted_colonies(s);
          const auto* sel_c = find_ptr(s.colonies, colony_filter);
          const char* label_c = (colony_filter == kInvalidId) ? "All" : (sel_c ? sel_c->name.c_str() : "(missing)");
          if (ImGui::BeginCombo("Colony", label_c)) {
            if (ImGui::Selectable("All", colony_filter == kInvalidId)) colony_filter = kInvalidId;
            for (const auto& [cid, name] : col_list) {
              if (ImGui::Selectable(name.c_str(), colony_filter == cid)) colony_filter = cid;
            }
            ImGui::EndCombo();
          }
        }

        ImGui::InputText("Search", search_buf, IM_ARRAYSIZE(search_buf));

        ImGui::InputInt("Show last N", &max_show);
        max_show = std::clamp(max_show, 10, 5000);

        static char export_path[256] = "events.csv";
        static std::string export_status;

        ImGui::SameLine();
        if (ImGui::SmallButton("Clear log")) {
          s.events.clear();
          export_status = "Event log cleared.";
        }

        // Collect visible indices (newest-first) based on filters + limit.
        std::vector<int> rows;
        rows.reserve(std::min(max_show, static_cast<int>(s.events.size())));
        for (int i = static_cast<int>(s.events.size()) - 1; i >= 0 && (int)rows.size() < max_show; --i) {
          const auto& ev = s.events[static_cast<std::size_t>(i)];
          const bool ok = (ev.level == EventLevel::Info && show_info) || (ev.level == EventLevel::Warn && show_warn) ||
                          (ev.level == EventLevel::Error && show_error);
          if (!ok) continue;

          if (!case_insensitive_contains(ev.message, search_buf)) continue;

          // Category filter.
          if (category_idx > 0) {
            static const EventCategory cat_vals[] = {
                EventCategory::General,
                EventCategory::Research,
                EventCategory::Shipyard,
                EventCategory::Construction,
                EventCategory::Movement,
                EventCategory::Combat,
                EventCategory::Intel,
                EventCategory::Exploration,
            };
            const int idx = category_idx - 1;
            if (idx < 0 || idx >= (int)IM_ARRAYSIZE(cat_vals)) continue;
            if (ev.category != cat_vals[idx]) continue;
          }

          // Faction filter (match either primary or secondary).
          if (faction_filter != kInvalidId) {
            if (ev.faction_id != faction_filter && ev.faction_id2 != faction_filter) continue;
          }

          // Context filters.
          if (system_filter != kInvalidId) {
            if (ev.system_id != system_filter) continue;
          }
          if (ship_filter != kInvalidId) {
            if (ev.ship_id != ship_filter) continue;
          }
          if (colony_filter != kInvalidId) {
            if (ev.colony_id != colony_filter) continue;
          }

          rows.push_back(i);
        }

        ImGui::InputText("Export path", export_path, IM_ARRAYSIZE(export_path));

        if (ImGui::SmallButton("Copy visible")) {
          std::string out;
          out.reserve(rows.size() * 96);
          for (int idx : rows) {
            const auto& ev = s.events[static_cast<std::size_t>(idx)];
            const nebula4x::Date d(ev.day);
            out += std::string("[") + d.to_string() + "] #" + std::to_string(static_cast<unsigned long long>(ev.seq)) +
                   " [" + event_category_label(ev.category) + "] " + event_level_label(ev.level) + ": " + ev.message;
            out.push_back('\n');
          }
          ImGui::SetClipboardText(out.c_str());
          export_status = "Copied " + std::to_string(rows.size()) + " event(s) to clipboard.";
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Export CSV")) {
          try {
            maybe_fix_export_extension(export_path, IM_ARRAYSIZE(export_path), ".csv");
            if (export_path[0] == '\0') {
              export_status = "Export failed: export path is empty.";
            } else {
              // Export in chronological order (oldest to newest within the visible set).
              std::vector<const SimEvent*> visible;
              visible.reserve(rows.size());
              for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                visible.push_back(&s.events[static_cast<std::size_t>(*it)]);
              }

              write_text_file(export_path, nebula4x::events_to_csv(s, visible));
              export_status =
                  "Exported CSV (" + std::to_string(rows.size()) + " event(s)) to " + std::string(export_path);
            }
          } catch (const std::exception& e) {
            export_status = std::string("Export failed: ") + e.what();
            nebula4x::log::error(export_status);
          }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Export JSON")) {
          try {
            maybe_fix_export_extension(export_path, IM_ARRAYSIZE(export_path), ".json");
            if (export_path[0] == '\0') {
              export_status = "Export failed: export path is empty.";
            } else {
              // Export in chronological order (oldest to newest within the visible set).
              std::vector<const SimEvent*> visible;
              visible.reserve(rows.size());
              for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                visible.push_back(&s.events[static_cast<std::size_t>(*it)]);
              }

              write_text_file(export_path, nebula4x::events_to_json(s, visible));
              export_status =
                  "Exported JSON (" + std::to_string(rows.size()) + " event(s)) to " + std::string(export_path);
            }
          } catch (const std::exception& e) {
            export_status = std::string("Export failed: ") + e.what();
            nebula4x::log::error(export_status);
          }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Export JSONL")) {
          try {
            maybe_fix_export_extension(export_path, IM_ARRAYSIZE(export_path), ".jsonl");
            if (export_path[0] == '\0') {
              export_status = "Export failed: export path is empty.";
            } else {
              // Export in chronological order (oldest to newest within the visible set).
              std::vector<const SimEvent*> visible;
              visible.reserve(rows.size());
              for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                visible.push_back(&s.events[static_cast<std::size_t>(*it)]);
              }

              write_text_file(export_path, nebula4x::events_to_jsonl(s, visible));
              export_status =
                  "Exported JSONL (" + std::to_string(rows.size()) + " event(s)) to " + std::string(export_path);
            }
          } catch (const std::exception& e) {
            export_status = std::string("Export failed: ") + e.what();
            nebula4x::log::error(export_status);
          }
        }

        if (!export_status.empty()) {
          ImGui::TextWrapped("%s", export_status.c_str());
        }

        ImGui::Separator();

        int shown = 0;
        for (int i : rows) {
          const auto& ev = s.events[static_cast<std::size_t>(i)];
          const nebula4x::Date d(ev.day);
          ImGui::BulletText("[%s] #%llu [%s] %s: %s", d.to_string().c_str(),
                            static_cast<unsigned long long>(ev.seq), event_category_label(ev.category),
                            event_level_label(ev.level), ev.message.c_str());

          ImGui::PushID(i);
          ImGui::SameLine();
          if (ImGui::SmallButton("Copy")) {
            std::string line = std::string("[") + d.to_string() + "] #" +
                              std::to_string(static_cast<unsigned long long>(ev.seq)) + " [" +
                              event_category_label(ev.category) + "] " + event_level_label(ev.level) + ": " +
                              ev.message;
            ImGui::SetClipboardText(line.c_str());
          }
          if (ev.system_id != kInvalidId) {
            ImGui::SameLine();
            if (ImGui::SmallButton("View system")) {
              s.selected_system = ev.system_id;
            }
          }
          if (ev.colony_id != kInvalidId) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Select colony")) {
              selected_colony = ev.colony_id;
            }
          }
          if (ev.ship_id != kInvalidId) {
            if (const auto* sh = find_ptr(s.ships, ev.ship_id)) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Select ship")) {
                selected_ship = ev.ship_id;
                s.selected_system = sh->system_id;
              }
            }
          }
          ImGui::PopID();
          ++shown;
        }

        if (shown == 0) {
          ImGui::TextDisabled("(none)");
        }

        ImGui::EndTabItem();
      }
    }

    ImGui::EndTabBar();
  }
}

} // namespace nebula4x::ui
