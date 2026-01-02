#include "ui/panels.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/research_planner.h"
#include "nebula4x/util/event_export.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/strings.h"
#include "nebula4x/util/time.h"

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

const char* body_type_label(BodyType t) {
  switch (t) {
    case BodyType::Star: return "Star";
    case BodyType::Planet: return "Planet";
    case BodyType::Moon: return "Moon";
    case BodyType::Asteroid: return "Asteroid";
    case BodyType::GasGiant: return "Gas Giant";
    default: return "Unknown";
  }
}

const char* component_type_label(ComponentType t) {
  switch (t) {
    case ComponentType::Engine: return "Engine";
    case ComponentType::FuelTank: return "Fuel Tank";
    case ComponentType::Cargo: return "Cargo";
    case ComponentType::Sensor: return "Sensor";
    case ComponentType::Reactor: return "Reactor";
    case ComponentType::Weapon: return "Weapon";
    case ComponentType::Armor: return "Armor";
    case ComponentType::Shield: return "Shield";
    case ComponentType::ColonyModule: return "Colony Module";
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
    case EventCategory::Diplomacy: return "Diplomacy";
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
  double fuel_cap = 0.0;
  double fuel_use = 0.0;
  double cargo = 0.0;
  double sensor = 0.0;
  double colony_cap = 0.0;
  double troop_cap = 0.0;

  // Visibility / signature multiplier (product of component multipliers).
  // 1.0 = normal visibility; lower values are harder to detect.
  double sig_mult = 1.0;

  double weapon_damage = 0.0;
  double weapon_range = 0.0;
  double hp_bonus = 0.0;
  double max_shields = 0.0;
  double shield_regen = 0.0;

  // Power budgeting (prototype).
  double power_gen = 0.0;
  double power_use_total = 0.0;
  double power_use_engines = 0.0;
  double power_use_sensors = 0.0;
  double power_use_weapons = 0.0;
  double power_use_shields = 0.0;

  for (const auto& cid : d.components) {
    auto it = c.components.find(cid);
    if (it == c.components.end()) continue;
    const auto& comp = it->second;
    mass += comp.mass_tons;
    speed = std::max(speed, comp.speed_km_s);
    fuel_cap += comp.fuel_capacity_tons;
    fuel_use += comp.fuel_use_per_mkm;
    cargo += comp.cargo_tons;
    sensor = std::max(sensor, comp.sensor_range_mkm);
    colony_cap += comp.colony_capacity_millions;
    troop_cap += comp.troop_capacity;

    const double comp_sig =
        std::clamp(std::isfinite(comp.signature_multiplier) ? comp.signature_multiplier : 1.0, 0.0, 1.0);
    sig_mult *= comp_sig;
    if (comp.type == ComponentType::Weapon) {
      weapon_damage += comp.weapon_damage;
      weapon_range = std::max(weapon_range, comp.weapon_range_mkm);
    }

    if (comp.type == ComponentType::Reactor) {
      power_gen += comp.power_output;
    }
    power_use_total += comp.power_use;
    if (comp.type == ComponentType::Engine) power_use_engines += comp.power_use;
    if (comp.type == ComponentType::Sensor) power_use_sensors += comp.power_use;
    if (comp.type == ComponentType::Weapon) power_use_weapons += comp.power_use;
    if (comp.type == ComponentType::Shield) power_use_shields += comp.power_use;
    hp_bonus += comp.hp_bonus;

    if (comp.type == ComponentType::Shield) {
      max_shields += comp.shield_hp;
      shield_regen += comp.shield_regen_per_day;
    }
  }

  d.mass_tons = mass;
  d.speed_km_s = speed;
  d.fuel_capacity_tons = fuel_cap;
  d.fuel_use_per_mkm = fuel_use;
  d.cargo_tons = cargo;
  d.sensor_range_mkm = sensor;
  d.colony_capacity_millions = colony_cap;
  d.troop_capacity = troop_cap;
  d.signature_multiplier = std::clamp(sig_mult, 0.05, 1.0);

  d.power_generation = power_gen;
  d.power_use_total = power_use_total;
  d.power_use_engines = power_use_engines;
  d.power_use_sensors = power_use_sensors;
  d.power_use_weapons = power_use_weapons;
  d.power_use_shields = power_use_shields;
  d.weapon_damage = weapon_damage;
  d.weapon_range_mkm = weapon_range;
  d.max_shields = max_shields;
  d.shield_regen_per_day = shield_regen;
  d.max_hp = std::max(1.0, mass * 2.0 + hp_bonus);
  return d;
}

} // namespace

void draw_main_menu(Simulation& sim, UIState& ui, char* save_path, char* load_path, char* ui_prefs_path,
                    UIPrefActions& actions) {
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

    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("Controls", nullptr, &ui.show_controls_window);
      ImGui::MenuItem("Map", nullptr, &ui.show_map_window);
      ImGui::MenuItem("Details", nullptr, &ui.show_details_window);
      ImGui::MenuItem("Directory (Colonies/Bodies)", nullptr, &ui.show_directory_window);
      ImGui::MenuItem("Production (Shipyard/Construction Planner)", nullptr, &ui.show_production_window);
      ImGui::MenuItem("Economy (Industry/Mining/Tech Tree)", nullptr, &ui.show_economy_window);
      ImGui::MenuItem("Timeline (Event Timeline)", nullptr, &ui.show_timeline_window);
      ImGui::MenuItem("Design Studio (Blueprints)", nullptr, &ui.show_design_studio_window);
      ImGui::MenuItem("Intel (Contacts/Sensors)", nullptr, &ui.show_intel_window);
      ImGui::MenuItem("Diplomacy Graph (Relations)", nullptr, &ui.show_diplomacy_window);
      ImGui::MenuItem("Settings Window", nullptr, &ui.show_settings_window);
      ImGui::MenuItem("Status Bar", nullptr, &ui.show_status_bar);
      ImGui::MenuItem("Event Toasts", nullptr, &ui.show_event_toasts);
      ImGui::Separator();
      if (ImGui::MenuItem("Reset Window Layout")) {
        actions.reset_window_layout = true;
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
      if (ImGui::MenuItem("Command Palette", "Ctrl+P")) ui.show_command_palette = true;
      if (ImGui::MenuItem("Help / Shortcuts", "F1")) ui.show_help_window = true;

      ImGui::Separator();

      if (ImGui::MenuItem("Open Event Log")) {
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Log;
      }
      if (ImGui::MenuItem("Open Production Planner")) {
        ui.show_production_window = true;
      }
      if (ImGui::MenuItem("Open Design Studio")) {
        ui.show_design_studio_window = true;
      }
      if (ImGui::MenuItem("Open Timeline")) {
        ui.show_timeline_window = true;
      }
      if (ImGui::MenuItem("Open Intel")) {
        ui.show_intel_window = true;
      }
      if (ImGui::MenuItem("Open Diplomacy Graph")) {
        ui.show_diplomacy_window = true;
      }
      if (ImGui::MenuItem("Focus System Map")) {
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::System;
      }
      if (ImGui::MenuItem("Focus Galaxy Map")) {
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::Galaxy;
      }

      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Options")) {
      if (ImGui::BeginMenu("Theme")) {
        ImGui::TextDisabled("Backgrounds");
        ImGui::ColorEdit4("Clear (SDL)##theme", ui.clear_color);
        ImGui::ColorEdit4("System Map##theme", ui.system_map_bg);
        ImGui::ColorEdit4("Galaxy Map##theme", ui.galaxy_map_bg);

        ImGui::Separator();
        ImGui::Checkbox("Override window background##theme", &ui.override_window_bg);
        if (ui.override_window_bg) {
          ImGui::ColorEdit4("Window Bg##theme", ui.window_bg);
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Reset Theme Defaults")) {
          actions.reset_ui_theme = true;
        }
        ImGui::EndMenu();
      }

      if (ImGui::BeginMenu("UI Prefs")) {
        ImGui::InputText("Path##ui_prefs", ui_prefs_path, 256);
        ImGui::Checkbox("Autosave on exit##ui_prefs", &ui.autosave_ui_prefs);
        ImGui::Separator();
        if (ImGui::MenuItem("Load UI Prefs")) {
          actions.load_ui_prefs = true;
        }
        if (ImGui::MenuItem("Save UI Prefs")) {
          actions.save_ui_prefs = true;
        }
        ImGui::EndMenu();
      }

      ImGui::EndMenu();
    }

    {
      const auto& st = sim.state();
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%s %02d:00", st.date.to_string().c_str(), std::clamp(st.hour_of_day, 0, 23));
      ImGui::Text("  Date: %s", buf);
    }

    ImGui::EndMainMenuBar();
  }
}

void draw_left_sidebar(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony) {
  ImGui::Text("Turns");
  if (ImGui::Button("+1 hour")) sim.advance_hours(1);
  ImGui::SameLine();
  if (ImGui::Button("+6h")) sim.advance_hours(6);
  ImGui::SameLine();
  if (ImGui::Button("+12h")) sim.advance_hours(12);

  {
    bool subday = sim.subday_economy_enabled();
    if (ImGui::Checkbox("Sub-day economy", &subday)) {
      sim.set_subday_economy_enabled(subday);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "When enabled, mining/industry, research, shipyards, construction, terraforming, and docked repairs\n"
          "advance proportionally on sub-day turns (+1h/+6h/+12h).\n"
          "When disabled, most economy systems tick only at the midnight day boundary.");
    }
  }

  ImGui::Separator();
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

      // Granularity for auto-run checks. Smaller steps stop closer to the
      // triggering event when using sub-day turn ticks.
      static int step_idx = 3;  // 0=1h,1=6h,2=12h,3=1d
      const int step_hours_opts[] = {1, 6, 12, 24};
      ImGui::SetNextItemWidth(110.0f);
      ImGui::Combo("Step##autorun", &step_idx, "1h\0" "6h\0" "12h\0" "1d\0");

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
            "Diplomacy",
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
              EventCategory::Diplomacy,
          };
          const int idx = category_idx - 1;
          if (idx >= 0 && idx < (int)IM_ARRAYSIZE(cat_vals)) {
            stop.filter_category = true;
            stop.category = cat_vals[idx];
          }
        }

        const int step_hours = step_hours_opts[std::clamp(step_idx, 0, 3)];
        const int max_hours = max_days * 24;
        auto res = sim.advance_until_event_hours(max_hours, stop, step_hours);

        const auto fmt_dur = [](int hours) {
          const int days = hours / 24;
          const int rem = hours % 24;
          if (days <= 0) return std::to_string(hours) + "h";
          return std::to_string(days) + "d " + std::to_string(rem) + "h";
        };

        if (res.hit) {
          // Jump UI context to the event payload when possible.
          auto& s = sim.state();
          if (res.event.system_id != kInvalidId) s.selected_system = res.event.system_id;
          if (res.event.colony_id != kInvalidId) selected_colony = res.event.colony_id;
          if (res.event.ship_id != kInvalidId) {
            if (find_ptr(s.ships, res.event.ship_id)) selected_ship = res.event.ship_id;
          }

          const std::string ts = format_datetime(nebula4x::Date(res.event.day), res.event.hour);
          last_status = "Paused on event after " + fmt_dur(res.hours_advanced) + ": [" + ts + "] " + res.event.message;
        } else {
          last_status = "No matching events within " + fmt_dur(max_hours) + " (advanced " + fmt_dur(res.hours_advanced) + ").";
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

void draw_right_sidebar(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  auto& s = sim.state();

  static int faction_combo_idx = 0;
  const auto factions = sorted_factions(s);
  if (!factions.empty()) {
    // Allow other windows to request that the details panel focus a specific faction.
    if (ui.request_focus_faction_id != kInvalidId) {
      for (int i = 0; i < (int)factions.size(); ++i) {
        if (factions[(std::size_t)i].first == ui.request_focus_faction_id) {
          faction_combo_idx = i;
          break;
        }
      }
      ui.request_focus_faction_id = kInvalidId;
    }
    faction_combo_idx = std::clamp(faction_combo_idx, 0, static_cast<int>(factions.size()) - 1);
  }
  const Id selected_faction_id = factions.empty() ? kInvalidId : factions[faction_combo_idx].first;
  Faction* selected_faction = factions.empty() ? nullptr : find_ptr(s.factions, selected_faction_id);

  // Share the currently selected faction with other panels for fog-of-war/exploration view.
  ui.viewer_faction_id = selected_faction_id;

  if (ImGui::BeginTabBar("details_tabs")) {
    const DetailsTab req_tab = ui.request_details_tab;
    auto flags_for = [&](DetailsTab t) -> ImGuiTabItemFlags {
      return (req_tab == t) ? ImGuiTabItemFlags_SetSelected : 0;
    };
    // --- Ship tab ---
    if (ImGui::BeginTabItem("Ship", nullptr, flags_for(DetailsTab::Ship))) {
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
          if (d->max_shields > 0.0) {
            ImGui::Text("Shields: %.0f / %.0f (+%.1f/day)", std::max(0.0, sh->shields), d->max_shields,
                        d->shield_regen_per_day);
          } else {
            ImGui::TextDisabled("Shields: (none)");
          }
          ImGui::Text("HP: %.0f / %.0f", sh->hp, d->max_hp);
          if (d->fuel_use_per_mkm > 0.0) {
            const double cap = std::max(0.0, d->fuel_capacity_tons);
            const double cur = std::max(0.0, sh->fuel_tons);
            if (cap > 0.0) {
              const double range = cur / d->fuel_use_per_mkm;
              ImGui::Text("Fuel: %.0f / %.0f t  (use %.2f t/mkm, range %.0f mkm)", cur, cap,
                          d->fuel_use_per_mkm, range);
            } else {
              ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Fuel: 0 t  (needs fuel tanks)");
            }
          } else if (d->fuel_capacity_tons > 0.0) {
            ImGui::Text("Fuel: %.0f / %.0f t", std::max(0.0, sh->fuel_tons), d->fuel_capacity_tons);
          } else {
            ImGui::TextDisabled("Fuel: (none)");
          }

          // Power budget + per-ship power policy
          {
            const double gen = std::max(0.0, d->power_generation);
            const double use = std::max(0.0, d->power_use_total);
            if (gen > 0.0 || use > 0.0) {
              if (use <= gen + 1e-9) {
                ImGui::Text("Power: %.1f gen / %.1f use", gen, use);
              } else {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                  "Power: %.1f gen / %.1f use (DEFICIT %.1f)", gen, use, use - gen);
              }
            } else {
              ImGui::TextDisabled("Power: (none)");
            }

            // Ensure save/mod corruption can't create duplicate priorities.
            sanitize_power_policy(sh->power_policy);

            ImGui::Spacing();
            ImGui::TextDisabled("Power policy");

            ImGui::PushID("power_policy");
            bool changed = false;
            changed |= ImGui::Checkbox("Engines", &sh->power_policy.engines_enabled);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Shields", &sh->power_policy.shields_enabled);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Weapons", &sh->power_policy.weapons_enabled);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Sensors", &sh->power_policy.sensors_enabled);

            ImGui::TextDisabled("Priority (top = keep online). Drag to reorder:");
            for (int i = 0; i < 4; ++i) {
              const PowerSubsystem subsys = sh->power_policy.priority[(std::size_t)i];
              std::string label = std::string(power_subsystem_label(subsys)) + "##prio" + std::to_string(i);
              ImGui::Selectable(label.c_str(), false);

              if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("PWR_PRIO", &i, sizeof(int));
                ImGui::Text("%s", power_subsystem_label(subsys));
                ImGui::EndDragDropSource();
              }
              if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PWR_PRIO")) {
                  if (payload->DataSize == sizeof(int)) {
                    const int src = *static_cast<const int*>(payload->Data);
                    if (src >= 0 && src < 4 && src != i) {
                      std::swap(sh->power_policy.priority[(std::size_t)src], sh->power_policy.priority[(std::size_t)i]);
                      changed = true;
                    }
                  }
                }
                ImGui::EndDragDropTarget();
              }
            }

            // Quick presets
            if (ImGui::SmallButton("Default")) {
              sh->power_policy.priority = {PowerSubsystem::Engines, PowerSubsystem::Shields, PowerSubsystem::Weapons,
                                           PowerSubsystem::Sensors};
              changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Recon")) {
              sh->power_policy.priority = {PowerSubsystem::Sensors, PowerSubsystem::Engines, PowerSubsystem::Shields,
                                           PowerSubsystem::Weapons};
              changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Offense")) {
              sh->power_policy.priority = {PowerSubsystem::Weapons, PowerSubsystem::Engines, PowerSubsystem::Shields,
                                           PowerSubsystem::Sensors};
              changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Defense")) {
              sh->power_policy.priority = {PowerSubsystem::Shields, PowerSubsystem::Engines, PowerSubsystem::Weapons,
                                           PowerSubsystem::Sensors};
              changed = true;
            }

            const Id fleet_id = sim.fleet_for_ship(sh->id);
            if (fleet_id != kInvalidId) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Apply to Fleet")) {
                if (auto* fl = find_ptr(s.fleets, fleet_id)) {
                  for (Id sid : fl->ship_ids) {
                    if (auto* other = find_ptr(s.ships, sid)) other->power_policy = sh->power_policy;
                  }
                }
              }
            }

            if (changed) sanitize_power_policy(sh->power_policy);

            const auto p = compute_power_allocation(gen, d->power_use_engines, d->power_use_shields,
                                                    d->power_use_weapons, d->power_use_sensors, sh->power_policy);
            const char* eng = sh->power_policy.engines_enabled ? (p.engines_online ? "ON" : "OFF") : "DIS";
            const char* shld = sh->power_policy.shields_enabled ? (p.shields_online ? "ON" : "OFF") : "DIS";
            const char* weap = sh->power_policy.weapons_enabled ? (p.weapons_online ? "ON" : "OFF") : "DIS";
            const char* sens = sh->power_policy.sensors_enabled ? (p.sensors_online ? "ON" : "OFF") : "DIS";

            ImGui::TextDisabled("Online: Engines %s, Shields %s, Weapons %s, Sensors %s  (avail %.1f)",
                                eng, shld, weap, sens, p.available);

            ImGui::PopID();
          }
          ImGui::Text("Cargo: %.0f / %.0f t", cargo_used_tons, d->cargo_tons);

          const bool has_sensors = (d->sensor_range_mkm > 1e-9);
          if (has_sensors) {
            int mode_i = static_cast<int>(sh->sensor_mode);
            const char* modes[] = {"Passive", "Normal", "Active"};
            if (ImGui::Combo("Sensor mode##sensor_mode", &mode_i, modes, IM_ARRAYSIZE(modes))) {
              mode_i = std::clamp(mode_i, 0, 2);
              sh->sensor_mode = static_cast<SensorMode>(mode_i);
            }

            const Id fleet_id = sim.fleet_for_ship(sh->id);
            if (fleet_id != kInvalidId) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Apply to Fleet##sensor_mode_fleet")) {
                if (auto* fl = find_ptr(s.fleets, fleet_id)) {
                  for (Id sid : fl->ship_ids) {
                    if (auto* other = find_ptr(s.ships, sid)) other->sensor_mode = sh->sensor_mode;
                  }
                }
              }
            }

            const double gen = std::max(0.0, d->power_generation);
            const auto p = compute_power_allocation(gen, d->power_use_engines, d->power_use_shields,
                                                    d->power_use_weapons, d->power_use_sensors, sh->power_policy);

            // Effective sensor range is only meaningful when sensors are online.
            double range_eff = 0.0;
            if (p.sensors_online) {
              double mult = 1.0;
              if (sh->sensor_mode == SensorMode::Passive) mult = sim.cfg().sensor_mode_passive_range_multiplier;
              else if (sh->sensor_mode == SensorMode::Active) mult = sim.cfg().sensor_mode_active_range_multiplier;
              if (!std::isfinite(mult) || mult < 0.0) mult = 0.0;
              range_eff = std::max(0.0, d->sensor_range_mkm) * mult;
            }

            // Effective signature includes both design stealth and EMCON.
            double sig_eff = std::clamp(std::isfinite(d->signature_multiplier) ? d->signature_multiplier : 1.0, 0.0, 1.0);

            const SensorMode sig_mode = sh->power_policy.sensors_enabled ? sh->sensor_mode : SensorMode::Passive;
            double sig_mult = 1.0;
            if (sig_mode == SensorMode::Passive) sig_mult = sim.cfg().sensor_mode_passive_signature_multiplier;
            else if (sig_mode == SensorMode::Active) sig_mult = sim.cfg().sensor_mode_active_signature_multiplier;
            if (!std::isfinite(sig_mult) || sig_mult < 0.0) sig_mult = 0.0;

            sig_eff *= sig_mult;
            const double max_sig = std::max(1.0, std::isfinite(sim.cfg().sensor_mode_active_signature_multiplier)
                                                    ? sim.cfg().sensor_mode_active_signature_multiplier
                                                    : 1.0);
            sig_eff = std::clamp(sig_eff, 0.0, max_sig);

            ImGui::Text("Sensor: %.0f mkm (effective %.0f mkm)", d->sensor_range_mkm, range_eff);
            ImGui::Text("Signature: %.0f%% (effective %.0f%%)", d->signature_multiplier * 100.0, sig_eff * 100.0);

            if (!sh->power_policy.sensors_enabled) {
              ImGui::TextDisabled("Note: Sensors disabled by power policy -> signature treated as Passive.");
            } else if (!p.sensors_online) {
              ImGui::TextDisabled("Note: Sensors offline due to power availability / load shedding.");
            }
          } else {
            ImGui::Text("Sensor: 0 mkm");
            ImGui::Text("Signature: %.0f%%", d->signature_multiplier * 100.0);
            ImGui::TextDisabled("Sensor mode: (no sensors)");
          }
          if (d->colony_capacity_millions > 0.0) {
            ImGui::Text("Colony capacity: %.0f M", d->colony_capacity_millions);
            if (sh->colonists_millions > 0.0) {
              ImGui::Text("Colonists: %.1f / %.1f M", sh->colonists_millions, d->colony_capacity_millions);
            } else {
              ImGui::TextDisabled("Colonists: 0 / %.1f M", d->colony_capacity_millions);
            }
          } else {
            ImGui::TextDisabled("Colony capacity: (none)");
          }
          if (d->weapon_damage > 0.0) {
            ImGui::Text("Beam weapons: %.1f dmg/day  (Range %.1f mkm)", d->weapon_damage, d->weapon_range_mkm);
          } else {
            ImGui::TextDisabled("Beam weapons: (none)");
          }

          if (d->missile_damage > 0.0 && d->missile_range_mkm > 0.0) {
            ImGui::Text("Missiles: %.1f dmg/salvo  (Range %.1f mkm, Speed %.1f mkm/day, Reload %.1f d)",
                        d->missile_damage, d->missile_range_mkm, d->missile_speed_mkm_per_day,
                        d->missile_reload_days);
            ImGui::TextDisabled("Missile cooldown: %.1f d", std::max(0.0, sh->missile_cooldown_days));
          } else {
            ImGui::TextDisabled("Missiles: (none)");
          }

          if (d->point_defense_damage > 0.0 && d->point_defense_range_mkm > 0.0) {
            ImGui::Text("Point defense: %.1f intercept  (Range %.1f mkm)", d->point_defense_damage,
                        d->point_defense_range_mkm);
          } else {
            ImGui::TextDisabled("Point defense: (none)");
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
          if (sh->auto_explore) {
            sh->auto_freight = false;
            sh->auto_salvage = false;
            sh->auto_colonize = false;
            sh->auto_tanker = false;
          }
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "When enabled, this ship will automatically travel to the nearest frontier system\n"
              "and jump into undiscovered systems whenever it has no queued orders.");
        }

        const bool can_auto_freight = (d && d->cargo_tons > 0.0);
        if (!can_auto_freight) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-freight minerals when idle", &sh->auto_freight)) {
          if (sh->auto_freight) {
            sh->auto_explore = false;
            sh->auto_salvage = false;
            sh->auto_colonize = false;
            sh->auto_tanker = false;
          }
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

        const bool can_auto_salvage = (d && d->cargo_tons > 0.0);
        if (!can_auto_salvage) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-salvage wrecks when idle", &sh->auto_salvage)) {
          if (sh->auto_salvage) {
            sh->auto_explore = false;
            sh->auto_freight = false;
            sh->auto_colonize = false;
            sh->auto_tanker = false;
          }
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "When enabled, this ship will automatically seek out known wrecks, salvage minerals into its cargo\n"
              "hold, and return the minerals to the nearest friendly colony when idle.");
        }
        if (!can_auto_salvage) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(requires a cargo hold)");
        }

        const bool can_auto_colonize = (d && d->colony_capacity_millions > 0.0);
        if (!can_auto_colonize) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-colonize when idle", &sh->auto_colonize)) {
          if (sh->auto_colonize) {
            sh->auto_explore = false;
            sh->auto_freight = false;
            sh->auto_salvage = false;
            sh->auto_tanker = false;
          }
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "When enabled, this ship will automatically attempt to colonize the best available body\n"
              "in your discovered map whenever it has no queued orders.\n\n"
              "Note: Colonization consumes the colonizer ship.");
        }
        if (!can_auto_colonize) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(requires a colony module)");
        }

        const bool can_auto_refuel = (d && d->fuel_capacity_tons > 0.0);
        if (!can_auto_refuel) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-refuel when low fuel (idle)", &sh->auto_refuel)) {
          // No mutual exclusion: this is a safety automation that can coexist
          // with auto-explore/auto-freight.
          sh->auto_refuel_threshold_fraction =
              std::clamp(sh->auto_refuel_threshold_fraction, 0.0, 1.0);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "When enabled, if this ship is idle and its fuel level drops below the configured threshold,\n"
              "it will automatically route to the nearest friendly colony to refuel.");
        }

        if (can_auto_refuel && sh->auto_refuel) {
          float thresh_pct = static_cast<float>(sh->auto_refuel_threshold_fraction * 100.0);
          if (ImGui::SliderFloat("Refuel threshold", &thresh_pct, 0.0f, 100.0f, "%.0f%%")) {
            sh->auto_refuel_threshold_fraction =
                std::clamp(static_cast<double>(thresh_pct) / 100.0, 0.0, 1.0);
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Auto-refuel triggers when fuel is below this fraction of capacity.\n"
                "Example: 25%% = refuel when below 25%%.");
          }
        }

        if (!can_auto_refuel) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(requires fuel tanks)");
        }

        const bool can_auto_tanker = (d && d->fuel_capacity_tons > 0.0);
        if (!can_auto_tanker) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-tanker: refuel other ships when idle", &sh->auto_tanker)) {
          if (sh->auto_tanker) {
            // Mutually exclusive with mission-style automation (explore/freight/salvage/colonize).
            sh->auto_explore = false;
            sh->auto_freight = false;
            sh->auto_salvage = false;
            sh->auto_colonize = false;
            sh->auto_tanker_reserve_fraction =
                std::clamp(sh->auto_tanker_reserve_fraction, 0.0, 1.0);
          }
        }
        if (ImGui::IsItemHovered()) {
          const auto& cfg = sim.cfg();
          ImGui::SetTooltip(
              "When enabled, this ship will act as a fuel tanker. If it is idle, it will automatically\n"
              "seek out a friendly idle ship with auto-refuel disabled that is below the request threshold\n"
              "and transfer fuel ship-to-ship.\n\n"
              "Request threshold: %.0f%%\n"
              "Fill target: %.0f%%\n\n"
              "Tip: Detach ships from fleets to use auto-tasks.",
              cfg.auto_tanker_request_threshold_fraction * 100.0,
              cfg.auto_tanker_fill_target_fraction * 100.0);
        }

        if (can_auto_tanker && sh->auto_tanker) {
          float reserve_pct = static_cast<float>(sh->auto_tanker_reserve_fraction * 100.0);
          if (ImGui::SliderFloat("Tanker reserve", &reserve_pct, 0.0f, 100.0f, "%.0f%%")) {
            sh->auto_tanker_reserve_fraction =
                std::clamp(static_cast<double>(reserve_pct) / 100.0, 0.0, 1.0);
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Auto-tanker will never transfer fuel below this fraction of its own capacity.\n"
                "Example: 25%% reserve means keep at least 25%% of tanks.");
          }
        }

        if (!can_auto_tanker) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(requires fuel tanks)");
        }

        const bool can_auto_repair = (d && d->max_hp > 0.0);
        if (!can_auto_repair) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-repair when damaged (idle)", &sh->auto_repair)) {
          // Like auto-refuel, this is a safety automation that can coexist with other modes.
          sh->auto_repair_threshold_fraction =
              std::clamp(sh->auto_repair_threshold_fraction, 0.0, 1.0);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "When enabled, if this ship is idle and its HP drops below the configured threshold,\n"
              "it will automatically route to the nearest mutual-friendly shipyard for repairs.");
        }

        if (can_auto_repair && sh->auto_repair) {
          float thresh_pct = static_cast<float>(sh->auto_repair_threshold_fraction * 100.0);
          if (ImGui::SliderFloat("Repair threshold", &thresh_pct, 0.0f, 100.0f, "%.0f%%")) {
            sh->auto_repair_threshold_fraction =
                std::clamp(static_cast<double>(thresh_pct) / 100.0, 0.0, 1.0);
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Auto-repair triggers when HP is below this fraction of max HP.\n"
                "Example: 75%% = seek repairs when below 75%%.");
          }
        }

        if (!can_auto_repair) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(requires a valid design)");
        }

        // Repair scheduling priority when docked at a shipyard.
        {
          int rp = static_cast<int>(sh->repair_priority);
          const char* labels[] = {"Low", "Normal", "High"};
          if (ImGui::Combo("Repair priority", &rp, labels, IM_ARRAYSIZE(labels))) {
            rp = std::clamp(rp, 0, 2);
            sh->repair_priority = static_cast<RepairPriority>(rp);
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "When multiple damaged ships are docked at the same shipyard, repair capacity is\n"
                "allocated in priority order. Higher priority ships are repaired first.");
          }
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
        const int repeat_remaining = ship_orders ? ship_orders->repeat_count_remaining : 0;
        const bool template_saved = (repeat_len > 0);
        const bool can_repeat_from_template = template_saved;

        if (repeat_on) {
          if (repeat_remaining < 0) {
            ImGui::Text("Repeat: ON  (infinite, template %d orders)", repeat_len);
          } else if (repeat_remaining == 0) {
            ImGui::Text("Repeat: ON  (stop after current cycle, template %d orders)", repeat_len);
          } else {
            ImGui::Text("Repeat: ON  (repeats remaining %d, template %d orders)", repeat_remaining, repeat_len);
          }
        } else {
          if (template_saved) {
            ImGui::Text("Repeat: OFF  (template saved: %d orders)", repeat_len);
          } else {
            ImGui::Text("Repeat: OFF");
          }
        }

        // Repeat controls
        if (repeat_on && ship_orders) {
          bool infinite = (ship_orders->repeat_count_remaining < 0);
          if (ImGui::Checkbox("Repeat indefinitely", &infinite)) {
            // If toggling to finite, default to 1 remaining refill.
            sim.set_order_repeat_count(selected_ship, infinite ? -1 : 1);
          }

          if (!infinite) {
            int count = ship_orders->repeat_count_remaining;
            if (count < 0) count = 0;
            if (ImGui::InputInt("Repeats remaining", &count)) {
              if (count < 0) count = 0;
              sim.set_order_repeat_count(selected_ship, count);
            }
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip("How many times the saved template will be re-enqueued after the current queue completes.\n0 = stop after this cycle.");
            }
          }

          if (ImGui::SmallButton("Stop after current cycle")) {
            sim.set_order_repeat_count(selected_ship, 0);
          }
        }

        ImGui::Spacing();
        const bool queue_has_orders = ship_orders && !ship_orders->queue.empty();
        if (!repeat_on) {
          if (!queue_has_orders) ImGui::BeginDisabled();
          if (ImGui::SmallButton("Enable repeat from queue")) {
            if (!sim.enable_order_repeat(selected_ship)) {
              nebula4x::log::warn("Couldn't enable repeat (queue empty?).");
            }
          }
          if (!queue_has_orders) ImGui::EndDisabled();

          if (template_saved) {
            ImGui::SameLine();
            if (!can_repeat_from_template) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Start repeat from saved template")) {
              if (!sim.enable_order_repeat_from_template(selected_ship)) {
                nebula4x::log::warn("Couldn't start repeat from template.");
              }
            }
            if (!can_repeat_from_template) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::SmallButton("Clear saved template")) {
              sim.disable_order_repeat(selected_ship);
            }
          }
        } else {
          if (!queue_has_orders) ImGui::BeginDisabled();
          if (ImGui::SmallButton("Update template from queue")) {
            if (!sim.update_order_repeat_template(selected_ship)) {
              nebula4x::log::warn("Couldn't update repeat template (queue empty?).");
            }
          }
          if (!queue_has_orders) ImGui::EndDisabled();

          ImGui::SameLine();
          if (ImGui::SmallButton("Stop repeat")) {
            sim.stop_order_repeat_keep_template(selected_ship);
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Disable repeat (clear)")) {
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
          static bool smart_apply = true;
          static bool strip_travel_orders_on_save = false;
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
          ImGui::SameLine();
          ImGui::Checkbox("Smart apply (auto-route)", &smart_apply);
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "When enabled, the template is compiled into a valid route from the ship's predicted system\n"
                "(after any queued jumps) to each order's required system, preventing 'invalid system' drops.");
          }

          const bool can_apply = !selected_template.empty();
          if (!can_apply) ImGui::BeginDisabled();
          if (ImGui::SmallButton("Apply to this ship")) {
            if (smart_apply) {
              std::string err;
              if (!sim.apply_order_template_to_ship_smart(selected_ship, selected_template, append_when_applying,
                                                         ui.fog_of_war, &err)) {
                status = err.empty() ? "Smart apply failed." : err;
              } else {
                status = "Applied template to ship (smart).";
              }
            } else {
              if (!sim.apply_order_template_to_ship(selected_ship, selected_template, append_when_applying)) {
                status = "Apply failed (missing template or ship).";
              } else {
                status = "Applied template to ship.";
              }
            }
          }
          if (!can_apply) ImGui::EndDisabled();

          if (ui.selected_fleet_id != kInvalidId) {
            ImGui::SameLine();
            const bool has_fleet = (find_ptr(s.fleets, ui.selected_fleet_id) != nullptr);
            const bool can_apply_fleet = can_apply && has_fleet;
            if (!can_apply_fleet) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Apply to selected fleet")) {
              if (smart_apply) {
                std::string err;
                if (!sim.apply_order_template_to_fleet_smart(ui.selected_fleet_id, selected_template,
                                                           append_when_applying, ui.fog_of_war, &err)) {
                  status = err.empty() ? "Smart apply to fleet failed." : err;
                } else {
                  status = "Applied template to fleet (smart).";
                }
              } else {
                if (!sim.apply_order_template_to_fleet(ui.selected_fleet_id, selected_template, append_when_applying)) {
                  status = "Apply to fleet failed (missing template or fleet).";
                } else {
                  status = "Applied template to fleet.";
                }
              }
            }
            if (!can_apply_fleet) ImGui::EndDisabled();
          }

          ImGui::Spacing();
          ImGui::InputText("Save name##tmpl_save", save_name_buf, IM_ARRAYSIZE(save_name_buf));
          ImGui::Checkbox("Overwrite existing##tmpl_overwrite", &overwrite_existing);
          ImGui::SameLine();
          ImGui::Checkbox("Strip TravelViaJump (portable)", &strip_travel_orders_on_save);
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "If enabled, TravelViaJump orders are removed when saving the template.\n"
                "Combined with Smart apply, this makes templates more portable between starting systems.");
          }

          const bool can_save = ship_orders && !ship_orders->queue.empty();
          if (!can_save) ImGui::BeginDisabled();
          if (ImGui::SmallButton("Save current queue as template")) {
            std::string err;
            if (!ship_orders || ship_orders->queue.empty()) {
              status = "No queued orders to save.";
            } else {
              std::vector<Order> to_save;
              if (strip_travel_orders_on_save) {
                to_save.reserve(ship_orders->queue.size());
                for (const auto& o : ship_orders->queue) {
                  if (!std::holds_alternative<TravelViaJump>(o)) to_save.push_back(o);
                }
              }

              const auto& src = strip_travel_orders_on_save ? to_save : ship_orders->queue;
              if (src.empty()) {
                status = "Nothing to save after stripping travel orders.";
              } else if (sim.save_order_template(save_name_buf, src, overwrite_existing, &err)) {
                status = std::string("Saved template: ") + save_name_buf;
                selected_template = save_name_buf;
                std::snprintf(rename_buf, sizeof(rename_buf), "%s", selected_template.c_str());
                confirm_delete = false;
              } else {
                status = err.empty() ? "Save failed." : err;
              }
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

            const bool restrict = ui.fog_of_war;
            if (const auto plan = sim.plan_jump_route_for_ship_to_pos(selected_ship, sel_col_body->system_id,
                                                                     sel_col_body->position_mkm, restrict,
                                                                     /*include_queued_jumps=*/true)) {
              ImGui::TextDisabled("Estimated travel time to colony: %.1f days", plan->total_eta_days);
            } else {
              ImGui::TextDisabled("No known route to this colony.");
            }
          }

          const bool friendly = sim.are_factions_mutual_friendly(sh->faction_id, sel_col->faction_id);
          const bool own_colony = (sel_col->faction_id == sh->faction_id);
          if (!friendly) {
            ImGui::Spacing();
            ImGui::TextDisabled("This colony is not friendly.");
            ImGui::Text("Defenders: %.1f", sel_col->ground_forces);

            // --- Orbital bombardment ---
            {
              const auto* d2 = sim.find_design(sh->design_id);
              const double w_dmg = d2 ? d2->weapon_damage : 0.0;
              const double w_range = d2 ? d2->weapon_range_mkm : 0.0;

              ImGui::Spacing();
              ImGui::Text("Orbital bombardment");
              if (d2) {
                ImGui::TextDisabled("Beam weapons: %.1f dmg/day, range %.1f mkm", w_dmg, w_range);
              } else {
                ImGui::TextDisabled("Beam weapons: (unknown design)");
              }

              static int bombard_days = 7;
              ImGui::InputInt("Bombard days (-1 = indefinite)", &bombard_days);

              const bool can_bombard = (w_dmg > 1e-9 && w_range > 1e-9);
              if (!can_bombard) ImGui::BeginDisabled();
              if (ImGui::Button("Bombard")) {
                if (can_bombard) {
                  if (!sim.issue_bombard_colony(selected_ship, selected_colony, bombard_days, ui.fog_of_war)) {
                    nebula4x::log::warn("Couldn't queue bombard order (no known route?).");
                  }
                }
              }
              if (!can_bombard) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered()) {
                  ImGui::SetTooltip("Ship has no weapons.");
                }
              }
            }

            const auto* d2 = sim.find_design(sh->design_id);
            const double cap2 = d2 ? d2->troop_capacity : 0.0;
            ImGui::Text("Embarked troops: %.1f / %.1f", sh->troops, cap2);
            if (sh->troops <= 1e-9 || cap2 <= 1e-9) {
              ImGui::BeginDisabled();
              ImGui::Button("Invade (requires troops)");
              ImGui::EndDisabled();
            } else {
              if (ImGui::Button("Invade (disembark all troops)")) {
                if (!sim.issue_invade_colony(selected_ship, selected_colony, ui.fog_of_war)) {
                  nebula4x::log::warn("Couldn't queue invade order (no known route?).");
                }
              }
            }
          } else {
            // --- Minerals ---
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
            if (!own_colony) ImGui::BeginDisabled();
            if (ImGui::Button("Scrap Ship")) {
              if (own_colony) {
                if (!sim.issue_scrap_ship(selected_ship, selected_colony, ui.fog_of_war)) {
                  nebula4x::log::warn("Couldn't queue scrap order.");
                }
              }
            }
            if (!own_colony) {
              ImGui::EndDisabled();
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scrapping requires an owned colony.");
            }

            // --- Troops ---
            ImGui::Separator();
            ImGui::Text("Troops");
            const auto* d2 = sim.find_design(sh->design_id);
            const double cap2 = d2 ? d2->troop_capacity : 0.0;
            ImGui::Text("Embarked: %.1f / %.1f", sh->troops, cap2);
            ImGui::Text("Colony garrison: %.1f", sel_col->ground_forces);

            static double troop_amount = 0.0;
            ImGui::InputDouble("Strength##Troops (0 = max)", &troop_amount, 10.0, 100.0, "%.1f");

            if (cap2 <= 1e-9) {
              ImGui::TextDisabled("(This design has no troop bays.)");
            } else {
              if (!own_colony) {
                ImGui::TextDisabled("(Troop transfer requires an owned colony.)");
                ImGui::BeginDisabled();
              }
              if (ImGui::Button("Load Troops")) {
                if (!sim.issue_load_troops(selected_ship, selected_colony, troop_amount, ui.fog_of_war)) {
                  nebula4x::log::warn("Couldn't queue load troops order.");
                }
              }
              ImGui::SameLine();
              if (ImGui::Button("Unload Troops")) {
                if (!sim.issue_unload_troops(selected_ship, selected_colony, troop_amount, ui.fog_of_war)) {
                  nebula4x::log::warn("Couldn't queue unload troops order.");
                }
              }
              if (!own_colony) {
                ImGui::EndDisabled();
              }
            }
          }

          // --- Colonists ---
          ImGui::Separator();
          ImGui::Text("Colonists");
          const double cap_col = d2 ? d2->colony_capacity_millions : 0.0;
          ImGui::Text("Embarked: %.1f / %.1f M", sh->colonists_millions, cap_col);
          ImGui::Text("Colony population: %.1f M", sel_col->population_millions);

          static double colonist_amount = 0.0;
          ImGui::InputDouble("Millions##Colonists (0 = max)", &colonist_amount, 10.0, 50.0, "%.1f");

          if (cap_col <= 1e-9) {
            ImGui::TextDisabled("(This design has no colony modules / passenger capacity.)");
          } else {
            if (!own_colony) {
              ImGui::TextDisabled("(Colonist transfer requires an owned colony.)");
              ImGui::BeginDisabled();
            }
            if (ImGui::Button("Load Colonists")) {
              if (!sim.issue_load_colonists(selected_ship, selected_colony, colonist_amount, ui.fog_of_war)) {
                nebula4x::log::warn("Couldn't queue load colonists order.");
              }
            }
            ImGui::SameLine();
            if (ImGui::Button("Unload Colonists")) {
              if (!sim.issue_unload_colonists(selected_ship, selected_colony, colonist_amount, ui.fog_of_war)) {
                nebula4x::log::warn("Couldn't queue unload colonists order.");
              }
            }
            if (!own_colony) ImGui::EndDisabled();
          }
        }

        // --- Wreck salvage ---
        {
          ImGui::Separator();
          ImGui::TextUnformatted("Wreck salvage");
          ImGui::TextDisabled(
              "Queue a salvage order to collect minerals from a wreck (auto-routes via jump points).\n"
              "If 'Tons' is 0, the ship will take as much as it can in one pass.");

          const auto* sh_design = sim.find_design(sh->design_id);
          const double cargo_cap = sh_design ? sh_design->cargo_tons : 0.0;
          const double cargo_used = [&]() {
            double u = 0.0;
            for (const auto& [_, v] : sh->cargo) u += v;
            return u;
          }();
          ImGui::Text("Cargo: %.1f / %.1f", cargo_used, cargo_cap);

          if (cargo_cap <= 1e-6) {
            ImGui::TextDisabled("(This design has no cargo holds.)");
          }

          // Build a list of known wrecks (respecting fog-of-war).
          std::vector<Id> wreck_ids;
          std::vector<std::string> wreck_labels;
          wreck_ids.reserve(s.wrecks.size());
          wreck_labels.reserve(s.wrecks.size());

          const Id viewer_faction_id = sh ? sh->faction_id : ui.viewer_faction_id;
          for (const auto& [wid, w] : s.wrecks) {
            if (ui.fog_of_war && !sim.is_system_discovered_by_faction(viewer_faction_id, w.system_id)) {
              continue;
            }
            const auto* wsys = find_ptr(s.systems, w.system_id);
            const std::string sys_name = wsys ? wsys->name : std::string("Unknown System");
            double total = 0.0;
            for (const auto& [_, v] : w.minerals) total += v;
            std::string label = sys_name + ": " + (w.name.empty() ? ("Wreck " + std::to_string(wid)) : w.name);
            label += " (" + fmt::format("{:.1f}", total) + " t)";
            wreck_ids.push_back(wid);
            wreck_labels.push_back(std::move(label));
          }

          // Stable ordering for the combo.
          std::vector<size_t> order;
          order.reserve(wreck_ids.size());
          for (size_t i = 0; i < wreck_ids.size(); ++i) order.push_back(i);
          std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return wreck_labels[a] < wreck_labels[b]; });

          static Id salvage_wreck_id = kInvalidId;
          static std::string salvage_mineral;
          static double salvage_tons = 0.0;

          if (!order.empty()) {
            bool found = false;
            for (size_t idx : order) {
              if (wreck_ids[idx] == salvage_wreck_id) {
                found = true;
                break;
              }
            }
            if (!found) salvage_wreck_id = wreck_ids[order.front()];
          } else {
            salvage_wreck_id = kInvalidId;
          }

          if (order.empty()) {
            ImGui::TextDisabled("(No known wrecks.)");
          } else {
            // Wreck combo
            const auto current_label = [&]() -> std::string {
              for (size_t idx : order) {
                if (wreck_ids[idx] == salvage_wreck_id) return wreck_labels[idx];
              }
              return wreck_labels[order.front()];
            }();

            if (ImGui::BeginCombo("Wreck##salvage", current_label.c_str())) {
              for (size_t idx : order) {
                const bool selected = (wreck_ids[idx] == salvage_wreck_id);
                if (ImGui::Selectable(wreck_labels[idx].c_str(), selected)) {
                  salvage_wreck_id = wreck_ids[idx];
                  salvage_mineral.clear();
                }
                if (selected) ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }

            // Mineral combo (depends on selected wreck)
            std::vector<std::string> minerals;
            if (const auto* w = find_ptr(s.wrecks, salvage_wreck_id)) {
              minerals.reserve(w->minerals.size());
              for (const auto& [k, v] : w->minerals) {
                if (v > 1e-9) minerals.push_back(k);
              }
              std::sort(minerals.begin(), minerals.end());
            }
            const std::string mineral_label = salvage_mineral.empty() ? std::string("<All>") : salvage_mineral;
            if (ImGui::BeginCombo("Mineral##salvage", mineral_label.c_str())) {
              if (ImGui::Selectable("<All>", salvage_mineral.empty())) {
                salvage_mineral.clear();
              }
              for (const auto& m : minerals) {
                const bool selected = (salvage_mineral == m);
                if (ImGui::Selectable(m.c_str(), selected)) {
                  salvage_mineral = m;
                }
                if (selected) ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }

            ImGui::InputDouble("Tons##salvage (0 = max)", &salvage_tons, 10.0, 100.0, "%.1f");
            if (salvage_tons < 0.0) salvage_tons = 0.0;

            const bool can_issue = (salvage_wreck_id != kInvalidId) && (cargo_cap > 1e-6);
            if (!can_issue) ImGui::BeginDisabled();
            if (ImGui::Button("Salvage")) {
              if (!sim.issue_salvage_wreck(selected_ship, salvage_wreck_id, salvage_mineral, salvage_tons,
                                           ui.fog_of_war)) {
                nebula4x::log::warn("Couldn't queue salvage wreck order.");
              }
            }
            if (!can_issue) ImGui::EndDisabled();
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

            // --- Fuel Transfer (Ship-to-Ship Refueling) ---
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Fuel Transfer");
            ImGui::TextDisabled("Transfers fuel from this ship's tanks to the target ship (ship-to-ship refueling).");
            ImGui::TextDisabled("Both ships must have fuel tanks. Tons <= 0 transfers as much as possible.");

            static double ship_transfer_fuel_tons = 0.0;

            if (target_ship_idx < 0) {
              ImGui::TextDisabled("Select a target ship above to enable fuel transfer.");
            } else {
              const Id target_id = friendly_ships[target_ship_idx].first;
              const auto* tgt = find_ptr(s.ships, target_id);

              const auto* src_d = sim.find_design(sh->design_id);
              const auto* tgt_d = tgt ? sim.find_design(tgt->design_id) : nullptr;
              const double src_cap = src_d ? std::max(0.0, src_d->fuel_capacity_tons) : 0.0;
              const double tgt_cap = tgt_d ? std::max(0.0, tgt_d->fuel_capacity_tons) : 0.0;

              if (!tgt) {
                ImGui::TextDisabled("Target ship no longer exists.");
              } else if (src_cap <= 1e-9 || tgt_cap <= 1e-9) {
                ImGui::TextDisabled("Fuel transfer unavailable: one or both ships have no fuel capacity.");
              } else {
                ImGui::Text("Source fuel: %.1f / %.1f", sh->fuel_tons, src_cap);
                ImGui::Text("Target fuel: %.1f / %.1f", tgt->fuel_tons, tgt_cap);

                ImGui::InputDouble("Tons##Fuel (0 = max)", &ship_transfer_fuel_tons, 10.0, 100.0, "%.1f");

                if (ImGui::Button("Transfer Fuel to Target")) {
                  if (!sim.issue_transfer_fuel_to_ship(selected_ship, target_id, ship_transfer_fuel_tons, ui.fog_of_war)) {
                    nebula4x::log::warn("Couldn't queue fuel transfer order.");
                  }
                }
              }
            }
        // --- Troop Transfer (Ship-to-Ship) ---
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Troop Transfer");
            ImGui::TextDisabled("Transfers embarked troops from this ship to the target ship.");
            ImGui::TextDisabled("Both ships must have troop bays. Strength <= 0 transfers as much as possible.");

            static double ship_transfer_troops_strength = 0.0;

            if (target_ship_idx < 0) {
              ImGui::TextDisabled("Select a target ship above to enable troop transfer.");
            } else {
              const Id target_id = friendly_ships[target_ship_idx].first;
              const auto* tgt = find_ptr(s.ships, target_id);

              const auto* src_d = sim.find_design(sh->design_id);
              const auto* tgt_d = tgt ? sim.find_design(tgt->design_id) : nullptr;
              const double src_cap = src_d ? std::max(0.0, src_d->troop_capacity) : 0.0;
              const double tgt_cap = tgt_d ? std::max(0.0, tgt_d->troop_capacity) : 0.0;

              if (!tgt) {
                ImGui::TextDisabled("Target ship no longer exists.");
              } else if (src_cap <= 1e-9 || tgt_cap <= 1e-9) {
                ImGui::TextDisabled("Troop transfer unavailable: one or both ships have no troop capacity.");
              } else {
                ImGui::Text("Source troops: %.1f / %.1f", sh->troops, src_cap);
                ImGui::Text("Target troops: %.1f / %.1f", tgt->troops, tgt_cap);

                ImGui::InputDouble("Strength##TroopTransfer (0 = max)", &ship_transfer_troops_strength, 10.0, 100.0,
                                   "%.1f");

                if (ImGui::Button("Transfer Troops to Target")) {
                  if (!sim.issue_transfer_troops_to_ship(selected_ship, target_id, ship_transfer_troops_strength,
                                                         ui.fog_of_war)) {
                    nebula4x::log::warn("Couldn't queue troop transfer order.");
                  }
                }
              }
            }

            // --- Escort / Follow ---
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Escort / Follow");
            ImGui::TextDisabled(
                "Follows the target ship, maintaining a separation. Cross-system escort will route via jump points.");

            static double escort_follow_mkm = 1.0;
            if (!std::isfinite(escort_follow_mkm) || escort_follow_mkm < 0.0) escort_follow_mkm = 1.0;
            ImGui::InputDouble("Follow distance (mkm)##Escort", &escort_follow_mkm, 0.1, 1.0, "%.2f");

            if (target_ship_idx < 0) {
              ImGui::TextDisabled("Select a target ship above to enable escort.");
            } else {
              const Id target_id = friendly_ships[target_ship_idx].first;

              if (ImGui::Button("Queue Escort Order")) {
                if (!sim.issue_escort_ship(selected_ship, target_id, escort_follow_mkm, ui.fog_of_war)) {
                  nebula4x::log::warn("Couldn't queue escort order.");
                }
              }

              const Id fleet_id = sim.fleet_for_ship(selected_ship);
              if (fleet_id != kInvalidId) {
                ImGui::SameLine();
                if (ImGui::Button("Fleet: Queue Escort")) {
                  if (!sim.issue_fleet_escort_ship(fleet_id, target_id, escort_follow_mkm, ui.fog_of_war)) {
                    nebula4x::log::warn("Couldn't queue fleet escort order.");
                  }
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
    if (ImGui::BeginTabItem("Fleet", nullptr, flags_for(DetailsTab::Fleet))) {
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
    if (ImGui::BeginTabItem("Colony", nullptr, flags_for(DetailsTab::Colony))) {
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

        // --- Ground forces / training ---
        ImGui::Separator();
        ImGui::Text("Ground forces");
        ImGui::Text("Garrison: %.1f", colony->ground_forces);
        const double forts = sim.fortification_points(*colony);
        if (forts > 1e-9) ImGui::Text("Fortifications: %.1f", forts);

        // Active battle status
        if (auto itb = s.ground_battles.find(colony->id); itb != s.ground_battles.end()) {
          const auto& b = itb->second;
          ImGui::TextDisabled("Ground battle in progress");
          ImGui::Text("Attacker: %.1f", b.attacker_strength);
          ImGui::Text("Defender: %.1f", b.defender_strength);
          ImGui::Text("Days: %d", b.days_fought);
        }

        const double train_pts = sim.troop_training_points_per_day(*colony);
        if (train_pts > 1e-9) {
          ImGui::Text("Training: %.1f pts/day", train_pts);
        } else {
          ImGui::TextDisabled("Training: 0 (build a Training Facility)");
        }
        ImGui::Text("Training queue: %.1f", colony->troop_training_queue);

        static double queue_strength = 0.0;
        ImGui::InputDouble("Queue strength##troop_train", &queue_strength, 50.0, 200.0, "%.1f");
        if (ImGui::Button("Add to queue")) {
          if (!sim.enqueue_troop_training(colony->id, queue_strength)) {
            nebula4x::log::warn("Couldn't enqueue troop training.");
          }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear queue")) {
          sim.clear_troop_training_queue(colony->id);
        }

        // --- Terraforming ---
        ImGui::Separator();
        ImGui::Text("Terraforming");
        const Body* b = find_ptr(s.bodies, colony->body_id);
        if (!b) {
          ImGui::TextDisabled("Body missing.");
        } else {
          const double tf_pts = sim.terraforming_points_per_day(*colony);
          ImGui::Text("Points/day: %.1f", tf_pts);
          ImGui::Text("Temp: %.1f K", b->surface_temp_k);
          ImGui::Text("Atmosphere: %.3f atm", b->atmosphere_atm);

          const bool has_target = (b->terraforming_target_temp_k > 0.0 || b->terraforming_target_atm > 0.0);
          if (has_target) {
            ImGui::Text("Target temp: %.1f K", b->terraforming_target_temp_k);
            ImGui::Text("Target atm: %.3f", b->terraforming_target_atm);
            if (b->terraforming_complete) ImGui::TextDisabled("(complete)");
          } else {
            ImGui::TextDisabled("No target set.");
          }

          static double target_temp = 288.0;
          static double target_atm = 1.0;
          ImGui::InputDouble("Target temp (K)##tf", &target_temp, 1.0, 10.0, "%.1f");
          ImGui::InputDouble("Target atm##tf", &target_atm, 0.01, 0.1, "%.3f");

          if (ImGui::Button("Set target")) {
            if (!sim.set_terraforming_target(colony->body_id, target_temp, target_atm)) {
              nebula4x::log::warn("Couldn't set terraforming target.");
            }
          }
          ImGui::SameLine();
          if (ImGui::Button("Clear target")) {
            sim.clear_terraforming_target(colony->body_id);
          }
        }

        // --- Habitability / Life support ---
        ImGui::Separator();
        ImGui::Text("Habitability / Life Support");
        if (!sim.config().enable_habitability) {
          ImGui::TextDisabled("Disabled in SimConfig.");
        } else if (!b) {
          ImGui::TextDisabled("Body missing.");
        } else {
          const double hab = sim.body_habitability(b->id);
          const double required = sim.required_habitation_capacity_millions(*colony);
          const double have = sim.habitation_capacity_millions(*colony);

          ImGui::Text("Habitability: %.1f%%", hab * 100.0);
          if (required <= 1e-6) {
            ImGui::TextDisabled("No habitation support required.");
          } else {
            ImGui::Text("Habitation support: %.0fM / %.0fM required", have, required);
            if (have + 1e-6 < required) {
              ImGui::Text("Shortfall: %.0fM (population will decline)", required - have);
            } else {
              ImGui::TextDisabled("Supported (domed). Population grows slowly unless the world is terraformed.");
            }
          }
        }


        ImGui::Separator();
        if (ImGui::TreeNodeEx("Mineral reserves (auto-freight)", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::TextDisabled("Auto-freight will not export minerals below these amounts (tons).");
          ImGui::TextDisabled("Effective reserve = max(manual reserve, local queue reserve).");

          // Sorted list of minerals mentioned in either stockpiles or reserves.
          std::vector<std::string> minerals;
          minerals.reserve(colony->minerals.size() + colony->mineral_reserves.size());
          for (const auto& [k, _] : colony->minerals) minerals.push_back(k);
          for (const auto& [k, _] : colony->mineral_reserves) minerals.push_back(k);
          std::sort(minerals.begin(), minerals.end());
          minerals.erase(std::unique(minerals.begin(), minerals.end()), minerals.end());

          const ImGuiTableFlags rflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                         ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
          if (ImGui::BeginTable("colony_reserves_table", 4, rflags)) {
            ImGui::TableSetupColumn("Mineral", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Stockpile", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Reserve", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableHeadersRow();

            for (const auto& mineral : minerals) {
              ImGui::TableNextRow();

              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted(mineral.c_str());

              ImGui::TableSetColumnIndex(1);
              const auto it_have = colony->minerals.find(mineral);
              const double have = (it_have == colony->minerals.end()) ? 0.0 : it_have->second;
              ImGui::Text("%.1f", have);

              ImGui::TableSetColumnIndex(2);
              double reserve = 0.0;
              if (auto it = colony->mineral_reserves.find(mineral); it != colony->mineral_reserves.end()) {
                reserve = it->second;
              }
              ImGui::PushID(mineral.c_str());
              ImGui::SetNextItemWidth(100.0f);
              if (ImGui::InputDouble("##reserve", &reserve, 0.0, 0.0, "%.1f")) {
                reserve = std::max(0.0, reserve);
                if (reserve <= 1e-9) {
                  colony->mineral_reserves.erase(mineral);
                } else {
                  colony->mineral_reserves[mineral] = reserve;
                }
              }

              ImGui::TableSetColumnIndex(3);
              if (ImGui::SmallButton("X")) {
                colony->mineral_reserves.erase(mineral);
              }
              ImGui::PopID();
            }

            ImGui::EndTable();
          }

          static char add_reserve_mineral[64] = "";
          static double add_reserve_tons = 0.0;

          ImGui::Separator();
          ImGui::Text("Add / set reserve");
          ImGui::InputText("Mineral##add_reserve_mineral", add_reserve_mineral,
                           IM_ARRAYSIZE(add_reserve_mineral));
          ImGui::InputDouble("Tons##add_reserve_tons", &add_reserve_tons, 0.0, 0.0, "%.1f");
          add_reserve_tons = std::max(0.0, add_reserve_tons);

          if (ImGui::SmallButton("Set reserve")) {
            const std::string m(add_reserve_mineral);
            if (!m.empty()) {
              if (add_reserve_tons <= 1e-9) {
                colony->mineral_reserves.erase(m);
              } else {
                colony->mineral_reserves[m] = add_reserve_tons;
              }
            }
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Clear all")) {
            colony->mineral_reserves.clear();
          }

          ImGui::TreePop();
        }


        // --- Mineral targets (auto-freight import) ---
        ImGui::Separator();
        if (ImGui::TreeNodeEx("Mineral targets (auto-freight import)", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::TextDisabled("Auto-freight will try to import minerals to reach these target stockpiles (tons).");
          ImGui::TextDisabled("Targets also act as a soft export floor (like a reserve).");
          ImGui::TextDisabled("Effective export floor = max(local queue reserve, manual reserve, target).");

          // Sorted list of minerals mentioned in stockpiles, reserves, or targets.
          std::vector<std::string> minerals;
          minerals.reserve(colony->minerals.size() + colony->mineral_reserves.size() + colony->mineral_targets.size());
          for (const auto& [k, _] : colony->minerals) minerals.push_back(k);
          for (const auto& [k, _] : colony->mineral_reserves) minerals.push_back(k);
          for (const auto& [k, _] : colony->mineral_targets) minerals.push_back(k);
          std::sort(minerals.begin(), minerals.end());
          minerals.erase(std::unique(minerals.begin(), minerals.end()), minerals.end());

          const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                         ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
          if (ImGui::BeginTable("colony_targets_table", 4, tflags)) {
            ImGui::TableSetupColumn("Mineral", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Stockpile", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableHeadersRow();

            for (const auto& mineral : minerals) {
              ImGui::TableNextRow();

              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted(mineral.c_str());

              ImGui::TableSetColumnIndex(1);
              const auto it_have = colony->minerals.find(mineral);
              const double have = (it_have == colony->minerals.end()) ? 0.0 : it_have->second;
              ImGui::Text("%.1f", have);

              ImGui::TableSetColumnIndex(2);
              double target = 0.0;
              if (auto it = colony->mineral_targets.find(mineral); it != colony->mineral_targets.end()) {
                target = it->second;
              }
              ImGui::PushID((std::string("tgt_") + mineral).c_str());
              ImGui::SetNextItemWidth(100.0f);
              if (ImGui::InputDouble("##target", &target, 0.0, 0.0, "%.1f")) {
                target = std::max(0.0, target);
                if (target <= 1e-9) {
                  colony->mineral_targets.erase(mineral);
                } else {
                  colony->mineral_targets[mineral] = target;
                }
              }

              ImGui::TableSetColumnIndex(3);
              if (ImGui::SmallButton("X")) {
                colony->mineral_targets.erase(mineral);
              }
              ImGui::PopID();
            }

            ImGui::EndTable();
          }

          static char add_target_mineral[64] = "";
          static double add_target_tons = 0.0;

          ImGui::Separator();
          ImGui::Text("Add / set target");
          ImGui::InputText("Mineral##add_target_mineral", add_target_mineral, IM_ARRAYSIZE(add_target_mineral));
          ImGui::InputDouble("Tons##add_target_tons", &add_target_tons, 0.0, 0.0, "%.1f");
          add_target_tons = std::max(0.0, add_target_tons);

          if (ImGui::SmallButton("Set target")) {
            const std::string m(add_target_mineral);
            if (!m.empty()) {
              if (add_target_tons <= 1e-9) {
                colony->mineral_targets.erase(m);
              } else {
                colony->mineral_targets[m] = add_target_tons;
              }
            }
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Clear all##targets")) {
            colony->mineral_targets.clear();
          }

          ImGui::TreePop();
        }


        // Body-level mineral deposits (finite mining).
        ImGui::Separator();
        ImGui::Text("Body deposits");
        const Body* body = find_ptr(s.bodies, colony->body_id);
        if (!body) {
          ImGui::TextDisabled("(body not found)");
        } else if (body->mineral_deposits.empty()) {
          ImGui::TextDisabled("(not modeled / unlimited)");
        } else {
          // Estimate colony mining rates by inspecting mining installations.
          std::unordered_map<std::string, double> rate_per_day;
          for (const auto& [inst_id, count] : colony->installations) {
            if (count <= 0) continue;
            const auto it = sim.content().installations.find(inst_id);
            if (it == sim.content().installations.end()) continue;
            const InstallationDef& def = it->second;
            if (def.produces_per_day.empty()) continue;
            const bool mining = def.mining ||
                                (!def.mining && nebula4x::to_lower(def.id).find("mine") != std::string::npos);
            if (!mining) continue;
            for (const auto& [mineral, per_day] : def.produces_per_day) {
              rate_per_day[mineral] += per_day * static_cast<double>(count);
            }
          }

          std::vector<std::string> deps;
          deps.reserve(body->mineral_deposits.size());
          for (const auto& [mineral, _] : body->mineral_deposits) deps.push_back(mineral);
          std::sort(deps.begin(), deps.end());

          for (const auto& mineral : deps) {
            const auto itv = body->mineral_deposits.find(mineral);
            const double left = (itv == body->mineral_deposits.end()) ? 0.0 : itv->second;
            const auto itr = rate_per_day.find(mineral);
            const double rate = (itr == rate_per_day.end()) ? 0.0 : itr->second;

            if (left <= 1e-9) {
              ImGui::BulletText("%s: %.0f  (depleted)", mineral.c_str(), left);
              continue;
            }

            if (rate > 1e-9) {
              const double eta_days = left / rate;
              const double eta_years = eta_days / 365.25;
              ImGui::BulletText("%s: %.0f  (%.2f/day, ETA %.0f d / %.1f y)",
                                mineral.c_str(), left, rate, eta_days, eta_years);
            } else {
              ImGui::BulletText("%s: %.0f", mineral.c_str(), left);
            }
          }
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
        if (ImGui::TreeNode("Installation targets (auto-build)")) {
          ImGui::TextDisabled("The simulation will auto-queue construction orders to reach these counts.");
          ImGui::TextDisabled("Auto-queued orders are marked [AUTO] in the construction queue.");
          ImGui::SameLine();
          if (ImGui::SmallButton("Clear all targets")) {
            colony->installation_targets.clear();
          }

          // Pending quantities from the construction queue (split manual vs auto).
          std::unordered_map<std::string, int> pending_manual;
          std::unordered_map<std::string, int> pending_auto;
          pending_manual.reserve(colony->construction_queue.size() * 2);
          pending_auto.reserve(colony->construction_queue.size() * 2);
          for (const auto& ord : colony->construction_queue) {
            if (ord.installation_id.empty()) continue;
            const int qty = std::max(0, ord.quantity_remaining);
            if (qty <= 0) continue;
            if (ord.auto_queued) {
              pending_auto[ord.installation_id] += qty;
            } else {
              pending_manual[ord.installation_id] += qty;
            }
          }

          // Buildable (unlocked) installations for this colony's faction.
          const auto* fac_for_colony_targets = find_ptr(s.factions, colony->faction_id);
          std::vector<std::string> buildable;
          if (fac_for_colony_targets) {
            for (const auto& id : fac_for_colony_targets->unlocked_installations) {
              if (!sim.is_installation_buildable_for_faction(fac_for_colony_targets->id, id)) continue;
              buildable.push_back(id);
            }
          } else {
            for (const auto& [id, _] : sim.content().installations) buildable.push_back(id);
          }
          std::sort(buildable.begin(), buildable.end());
          buildable.erase(std::unique(buildable.begin(), buildable.end()), buildable.end());

          // Union: buildable + already targeted + already installed.
          std::vector<std::string> all_ids = buildable;
          for (const auto& [id, _] : colony->installation_targets) all_ids.push_back(id);
          for (const auto& [id, _] : colony->installations) all_ids.push_back(id);
          std::sort(all_ids.begin(), all_ids.end());
          all_ids.erase(std::unique(all_ids.begin(), all_ids.end()), all_ids.end());

          const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                         ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
          if (ImGui::BeginTable("colony_installation_targets_table", 6, tflags)) {
            ImGui::TableSetupColumn("Installation", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Have", ImGuiTableColumnFlags_WidthFixed, 48.0f);
            ImGui::TableSetupColumn("Manual Q", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableSetupColumn("Auto Q", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableSetupColumn("Need", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableHeadersRow();

            for (const auto& id : all_ids) {
              const auto itdef = sim.content().installations.find(id);
              const std::string nm2 = (itdef == sim.content().installations.end()) ? id : itdef->second.name;

              const int have = [&]() -> int {
                auto it = colony->installations.find(id);
                return (it == colony->installations.end()) ? 0 : it->second;
              }();

              const int man = pending_manual[id];
              const int aut = pending_auto[id];

              int tgt = 0;
              if (auto it = colony->installation_targets.find(id); it != colony->installation_targets.end()) tgt = it->second;
              tgt = std::max(0, tgt);

              const int need = std::max(0, tgt - (have + man + aut));

              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::Text("%s", nm2.c_str());
              if (!sim.is_installation_buildable_for_faction(colony->faction_id, id)) {
                ImGui::SameLine();
                ImGui::TextDisabled("(locked)");
              }

              ImGui::TableSetColumnIndex(1);
              ImGui::Text("%d", have);

              ImGui::TableSetColumnIndex(2);
              ImGui::Text("%d", man);

              ImGui::TableSetColumnIndex(3);
              ImGui::Text("%d", aut);

              ImGui::TableSetColumnIndex(4);
              ImGui::PushID(id.c_str());
              int edit = tgt;
              ImGui::SetNextItemWidth(56.0f);
              if (ImGui::DragInt("##tgt", &edit, 1.0f, 0, 100000)) {
                edit = std::max(0, edit);
                if (edit == 0) colony->installation_targets.erase(id);
                else colony->installation_targets[id] = edit;
              }
              ImGui::PopID();

              ImGui::TableSetColumnIndex(5);
              ImGui::Text("%d", need);
            }

            ImGui::EndTable();
          }

          ImGui::TreePop();
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
      std::string nm = def ? def->name : ord.installation_id;
      if (ord.auto_queued) nm += " [AUTO]";

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
      if (bo.auto_queued && !is_refit) {
        nm = "[AUTO] " + nm;
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
          if (const auto* colony_body = find_ptr(s.bodies, colony->body_id)) {
            if (const auto* sys = find_ptr(s.systems, colony_body->system_id)) {
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

    // --- Body (planet) tab ---
    if (ImGui::BeginTabItem("Body", nullptr, flags_for(DetailsTab::Body))) {
      // If no body selected explicitly, fall back to the selected colony's body.
      Id body_id = selected_body;
      if (body_id == kInvalidId && selected_colony != kInvalidId) {
        if (const auto* c = find_ptr(s.colonies, selected_colony)) {
          body_id = c->body_id;
        }
      }

      if (selected_body == kInvalidId && body_id != kInvalidId) {
        selected_body = body_id;
      }

      if (body_id == kInvalidId) {
        ImGui::TextDisabled("No body selected (select a colony, use Directory, or right-click a body on the system map)");
      } else {
        const Body* b = find_ptr(s.bodies, body_id);
        if (!b) {
          ImGui::TextDisabled("Selected body no longer exists");
        } else {
          const StarSystem* sys = find_ptr(s.systems, b->system_id);
          ImGui::Text("%s", b->name.c_str());
          ImGui::Separator();
          ImGui::Text("Type: %s", body_type_label(b->type));
          ImGui::Text("System: %s", sys ? sys->name.c_str() : "(unknown)");
          if (b->parent_body_id != kInvalidId) {
            const Body* parent = find_ptr(s.bodies, b->parent_body_id);
            ImGui::Text("Orbits: %s", parent ? parent->name.c_str() : "(missing parent)");
          } else {
            ImGui::Text("Orbits: (system origin)");
          }

          ImGui::Text("a: %.2f mkm (%.2f AU)", b->orbit_radius_mkm, b->orbit_radius_mkm / 149.6);
          ImGui::Text("Period: %.2f days", b->orbit_period_days);
          if (std::abs(b->orbit_eccentricity) > 1e-4) {
            const double e = b->orbit_eccentricity;
            const double peri = b->orbit_radius_mkm * (1.0 - e);
            const double apo = b->orbit_radius_mkm * (1.0 + e);
            ImGui::Text("e: %.3f", e);
            ImGui::Text("Periapsis: %.2f mkm (%.2f AU)", peri, peri / 149.6);
            ImGui::Text("Apoapsis: %.2f mkm (%.2f AU)", apo, apo / 149.6);
            ImGui::Text("ω: %.1f°", b->orbit_arg_periapsis_radians * 57.29577951308232);
          }
          ImGui::Text("Pos: (%.2f, %.2f) mkm", b->position_mkm.x, b->position_mkm.y);

          // Physical metadata (optional).
          if (b->mass_solar > 0.0) ImGui::Text("Mass: %.3f Msun", b->mass_solar);
          if (b->luminosity_solar > 0.0) ImGui::Text("Luminosity: %.3f Lsun", b->luminosity_solar);
          if (b->mass_earths > 0.0) ImGui::Text("Mass: %.3f Mearth", b->mass_earths);
          if (b->radius_km > 0.0) ImGui::Text("Radius: %.0f km", b->radius_km);
          if (b->surface_temp_k > 0.0) ImGui::Text("Temp: %.0f K", b->surface_temp_k);
          if (b->atmosphere_atm > 0.0 || b->terraforming_target_atm > 0.0) {
            ImGui::Text("Atmosphere: %.3f atm", b->atmosphere_atm);
          }
          if (b->terraforming_target_temp_k > 0.0 || b->terraforming_target_atm > 0.0) {
            ImGui::Text("Terraform target: %.1f K, %.3f atm", b->terraforming_target_temp_k, b->terraforming_target_atm);
            if (b->terraforming_complete) ImGui::TextDisabled("(terraforming complete)");
          }

          // Colony on this body (if any).
          Id colony_here = kInvalidId;
          for (const auto& [cid, c] : s.colonies) {
            if (c.body_id == body_id) {
              colony_here = cid;
              break;
            }
          }

          if (colony_here != kInvalidId) {
            if (const Colony* c = find_ptr(s.colonies, colony_here)) {
              const Faction* fac = find_ptr(s.factions, c->faction_id);
              ImGui::SeparatorText("Colony");
              ImGui::Text("Name: %s", c->name.c_str());
              ImGui::Text("Faction: %s", fac ? fac->name.c_str() : "(unknown)");
              ImGui::Text("Population: %.2f M", c->population_millions);
              if (ImGui::Button("Select colony")) {
                selected_colony = colony_here;
              }
            }
          } else {
            ImGui::TextDisabled("Colony: (none)");
          }

          ImGui::SeparatorText("Mineral deposits");
          if (b->mineral_deposits.empty()) {
            ImGui::TextDisabled("(none)");
          } else {
            // Sort by amount descending for easier scanning.
            std::vector<std::pair<std::string, double>> deps;
            deps.reserve(b->mineral_deposits.size());
            for (const auto& [k, v] : b->mineral_deposits) deps.emplace_back(k, v);
            std::sort(deps.begin(), deps.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

            if (ImGui::BeginTable("body_deposits", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
              ImGui::TableSetupColumn("Mineral");
              ImGui::TableSetupColumn("Amount");
              ImGui::TableHeadersRow();
              for (const auto& [k, v] : deps) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(k.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.1f", v);
              }
              ImGui::EndTable();
            }
          }
        }
      }
      ImGui::EndTabItem();
    }


    // --- Logistics tab ---
    if (ImGui::BeginTabItem("Logistics", nullptr, flags_for(DetailsTab::Logistics))) {
      if (!selected_faction) {
        ImGui::TextDisabled("No faction selected.");
      } else {
        ImGui::SeparatorText("Auto-freight");
        ImGui::TextWrapped(
            "Enable Auto-freight on cargo ships to have them automatically haul minerals between your colonies "
            "whenever they are idle. Auto-freight tries to relieve mineral shortages that stall shipyards, "
            "unpaid construction orders, and colony stockpile targets (set in Colony Details).");

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
          std::string reason;
          switch (n.kind) {
            case LogisticsNeedKind::Shipyard:
              reason = "Shipyard";
              break;
            case LogisticsNeedKind::Construction:
              reason = "Construction";
              break;
            case LogisticsNeedKind::IndustryInput:
              reason = "Industry";
              break;
            case LogisticsNeedKind::StockpileTarget:
              reason = "Target";
              break;
            case LogisticsNeedKind::Fuel:
              reason = "Fuel";
              break;
          }
          if (!n.context_id.empty()) reason += (":" + n.context_id);
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
              if (so->repeat) {
                if (so->repeat_count_remaining < 0) {
                  order_str += " (repeat inf)";
                } else if (so->repeat_count_remaining == 0) {
                  order_str += " (repeat stop)";
                } else {
                  order_str += " (repeat " + std::to_string(so->repeat_count_remaining) + ")";
                }
              }
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

        ImGui::SeparatorText("Auto-shipyards");
        ImGui::TextWrapped(
            "Set desired counts of ship designs to maintain. The simulation will automatically enqueue shipyard build orders "
            "(marked [AUTO] in shipyard queues) across your colonies to reach these targets. Manual build/refit orders are never modified.");

        int shipyard_colonies = 0;
        int shipyard_installations = 0;
        for (const auto& [cid2, c2] : s.colonies) {
          if (c2.faction_id != selected_faction_id) continue;
          const auto it_yard = c2.installations.find("shipyard");
          const int yards = (it_yard != c2.installations.end()) ? it_yard->second : 0;
          if (yards <= 0) continue;
          shipyard_colonies += 1;
          shipyard_installations += yards;
        }
        if (shipyard_installations <= 0) {
          ImGui::TextDisabled("No shipyards owned by this faction.");
        } else {
          ImGui::TextDisabled("%d shipyard colony(ies), %d shipyard installation(s).", shipyard_colonies, shipyard_installations);
        }

        if (ImGui::Button("Clear ship build targets")) {
          selected_faction->ship_design_targets.clear();
        }

        // Add / update a target.
        {
          const auto buildable = sorted_buildable_design_ids(sim, selected_faction_id);
          static int ship_target_design_idx = 0;
          static int ship_target_count = 1;
          if (buildable.empty()) {
            ImGui::TextDisabled("No buildable ship designs.");
          } else {
            if (ship_target_design_idx < 0 || ship_target_design_idx >= static_cast<int>(buildable.size())) ship_target_design_idx = 0;
            const std::string& did = buildable[static_cast<std::size_t>(ship_target_design_idx)];
            if (ImGui::BeginCombo("Design##ship_targets", did.c_str())) {
              for (int i = 0; i < static_cast<int>(buildable.size()); ++i) {
                const bool is_selected = (i == ship_target_design_idx);
                if (ImGui::Selectable(buildable[static_cast<std::size_t>(i)].c_str(), is_selected)) ship_target_design_idx = i;
                if (is_selected) ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::InputInt("Target##ship_targets", &ship_target_count);
            if (ship_target_count < 0) ship_target_count = 0;
            ImGui::SameLine();
            if (ImGui::Button("Set##ship_targets")) {
              if (ship_target_count <= 0) selected_faction->ship_design_targets.erase(did);
              else selected_faction->ship_design_targets[did] = ship_target_count;
            }
          }
        }

        // Compute current counts and pending shipyard builds.
        std::unordered_map<std::string, int> have_by_design;
        have_by_design.reserve(s.ships.size());
        for (const auto& [sid, sh] : s.ships) {
          if (sh.faction_id != selected_faction_id) continue;
          if (sh.design_id.empty()) continue;
          have_by_design[sh.design_id] += 1;
        }

        std::unordered_map<std::string, int> pending_manual_by_design;
        std::unordered_map<std::string, int> pending_auto_by_design;
        for (const auto& [cid2, c2] : s.colonies) {
          if (c2.faction_id != selected_faction_id) continue;
          const auto it_yard = c2.installations.find("shipyard");
          const int yards = (it_yard != c2.installations.end()) ? it_yard->second : 0;
          if (yards <= 0) continue;
          for (const auto& bo : c2.shipyard_queue) {
            if (bo.refit_ship_id != kInvalidId) continue;
            if (bo.design_id.empty()) continue;
            if (bo.auto_queued) pending_auto_by_design[bo.design_id] += 1;
            else pending_manual_by_design[bo.design_id] += 1;
          }
        }

        if (selected_faction->ship_design_targets.empty()) {
          ImGui::TextDisabled("No ship design targets set.");
        } else if (ImGui::BeginTable("ship_design_targets_table", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
          ImGui::TableSetupColumn("Design", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 70.0f);
          ImGui::TableSetupColumn("Have", ImGuiTableColumnFlags_WidthFixed, 60.0f);
          ImGui::TableSetupColumn("Pending (M)", ImGuiTableColumnFlags_WidthFixed, 95.0f);
          ImGui::TableSetupColumn("Pending (A)", ImGuiTableColumnFlags_WidthFixed, 95.0f);
          ImGui::TableSetupColumn("Need (A)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
          ImGui::TableHeadersRow();

          std::vector<std::string> ids;
          ids.reserve(selected_faction->ship_design_targets.size());
          for (const auto& [did, t] : selected_faction->ship_design_targets) {
            if (t > 0) ids.push_back(did);
          }
          std::sort(ids.begin(), ids.end());
          ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

          for (const auto& did : ids) {
            auto it = selected_faction->ship_design_targets.find(did);
            if (it == selected_faction->ship_design_targets.end()) continue;
            int target = it->second;
            if (target <= 0) continue;

            const int have_n = (have_by_design.find(did) != have_by_design.end()) ? have_by_design[did] : 0;
            const int man_n = (pending_manual_by_design.find(did) != pending_manual_by_design.end()) ? pending_manual_by_design[did] : 0;
            const int auto_n = (pending_auto_by_design.find(did) != pending_auto_by_design.end()) ? pending_auto_by_design[did] : 0;
            const int need_auto = std::max(0, target - (have_n + man_n));

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const auto* d = sim.find_design(did);
            if (d) {
              ImGui::Text("%s (%s)", d->name.c_str(), did.c_str());
            } else {
              ImGui::TextDisabled("%s", did.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(did.c_str());
            ImGui::SetNextItemWidth(60.0f);
            int t_edit = target;
            if (ImGui::InputInt("##target", &t_edit)) {
              if (t_edit < 0) t_edit = 0;
              if (t_edit <= 0) selected_faction->ship_design_targets.erase(did);
              else selected_faction->ship_design_targets[did] = t_edit;
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", have_n);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", man_n);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", auto_n);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d", need_auto);
          }

          ImGui::EndTable();
        }
      }
      ImGui::EndTabItem();
    }

    // --- Research tab ---
    if (ImGui::BeginTabItem("Research", nullptr, flags_for(DetailsTab::Research))) {
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

        // Also show a computed RP/day so players can reason about timelines.
        {
          double rp_per_day = 0.0;
          for (const auto& [cid, col] : sim.state().colonies) {
            if (col.faction_id != selected_faction->id) continue;
            for (const auto& [inst_id, count] : col.installations) {
              if (count <= 0) continue;
              const auto it = sim.content().installations.find(inst_id);
              if (it == sim.content().installations.end()) continue;
              rp_per_day += it->second.research_points_per_day * static_cast<double>(count);
            }
          }
          ImGui::Text("Research Points/day: %.1f", rp_per_day);
        }

        ImGui::Separator();
        ImGui::Text("Tech browser");

        static char tech_search[128] = "";
        ImGui::InputTextWithHint("Search", "Type to filter techs...", tech_search, IM_ARRAYSIZE(tech_search));

        static bool show_known = true;
        static bool show_locked = true;
        static bool show_researchable = true;
        ImGui::Checkbox("Known", &show_known);
        ImGui::SameLine();
        ImGui::Checkbox("Locked", &show_locked);
        ImGui::SameLine();
        ImGui::Checkbox("Researchable", &show_researchable);

        // Build a deterministic, filtered list of tech ids.
        std::vector<std::string> tech_ids;
        tech_ids.reserve(sim.content().techs.size());
        for (const auto& [tid, _] : sim.content().techs) tech_ids.push_back(tid);
        std::sort(tech_ids.begin(), tech_ids.end(), [&](const std::string& a, const std::string& b) {
          const auto ia = sim.content().techs.find(a);
          const auto ib = sim.content().techs.find(b);
          const std::string an = (ia == sim.content().techs.end()) ? a : ia->second.name;
          const std::string bn = (ib == sim.content().techs.end()) ? b : ib->second.name;
          if (an != bn) return an < bn;
          return a < b;
        });

        std::vector<std::string> filtered;
        filtered.reserve(tech_ids.size());
        for (const auto& tid : tech_ids) {
          const auto it = sim.content().techs.find(tid);
          if (it == sim.content().techs.end()) continue;
          const TechDef& t = it->second;

          const bool known = vec_contains(selected_faction->known_techs, tid);
          const bool researchable = prereqs_met(*selected_faction, t);
          const bool locked = (!known && !researchable);

          if (known && !show_known) continue;
          if (locked && !show_locked) continue;
          if (researchable && !known && !show_researchable) continue;

          if (tech_search[0] != '\0') {
            const std::string hay = t.name + " " + t.id;
            if (!case_insensitive_contains(hay, tech_search)) continue;
          }

          filtered.push_back(tid);
        }

        static int tech_sel = 0;
        if (!filtered.empty()) tech_sel = std::clamp(tech_sel, 0, static_cast<int>(filtered.size()) - 1);

        // Layout: list (left) + details (right)
        const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##tech_browser", 2, flags)) {
          ImGui::TableSetupColumn("List", ImGuiTableColumnFlags_WidthStretch, 0.55f);
          ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.45f);
          ImGui::TableNextRow();

          // --- List column ---
          ImGui::TableSetColumnIndex(0);
          if (filtered.empty()) {
            ImGui::TextDisabled("(no techs match filter)");
          } else {
            if (ImGui::BeginListBox("##tech_list", ImVec2(-1, 220))) {
              for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
                const std::string& tid = filtered[i];
                const auto it = sim.content().techs.find(tid);
                if (it == sim.content().techs.end()) continue;
                const TechDef& t = it->second;

                const bool known = vec_contains(selected_faction->known_techs, tid);
                const bool active = (selected_faction->active_research_id == tid);
                const bool queued = (std::find(selected_faction->research_queue.begin(),
                                               selected_faction->research_queue.end(), tid) !=
                                     selected_faction->research_queue.end());
                const bool researchable = prereqs_met(*selected_faction, t);

                std::string prefix;
                if (active) prefix = "[A] ";
                else if (known) prefix = "[K] ";
                else if (queued) prefix = "[Q] ";
                else if (researchable) prefix = "[R] ";
                else prefix = "[L] ";

                std::string label = prefix + t.name;
                if (t.cost > 0.0) label += "  (" + std::to_string(static_cast<int>(t.cost)) + ")";
                label += "##" + tid;

                const bool sel = (tech_sel == i);
                if (ImGui::Selectable(label.c_str(), sel)) tech_sel = i;
              }
              ImGui::EndListBox();
            }
          }

          // --- Details column ---
          ImGui::TableSetColumnIndex(1);

          if (!filtered.empty()) {
            const std::string chosen_id = filtered[tech_sel];
            const auto it = sim.content().techs.find(chosen_id);
            const TechDef* chosen = (it == sim.content().techs.end()) ? nullptr : &it->second;

            if (chosen) {
              const bool known = vec_contains(selected_faction->known_techs, chosen_id);
              const bool active = (selected_faction->active_research_id == chosen_id);
              const bool queued = (std::find(selected_faction->research_queue.begin(),
                                             selected_faction->research_queue.end(), chosen_id) !=
                                   selected_faction->research_queue.end());
              const bool researchable = prereqs_met(*selected_faction, *chosen);

              ImGui::TextWrapped("%s", chosen->name.c_str());
              ImGui::TextDisabled("id: %s", chosen->id.c_str());
              ImGui::Text("Cost: %.0f", chosen->cost);

              if (known) ImGui::TextDisabled("Status: known");
              else if (active) ImGui::TextDisabled("Status: active");
              else if (queued) ImGui::TextDisabled("Status: queued");
              else if (researchable) ImGui::TextDisabled("Status: researchable");
              else ImGui::TextDisabled("Status: locked (missing prereqs)");

              ImGui::Separator();
              ImGui::Text("Prerequisites");
              if (chosen->prereqs.empty()) {
                ImGui::TextDisabled("(none)");
              } else {
                for (const auto& pre : chosen->prereqs) {
                  const auto itp = sim.content().techs.find(pre);
                  const std::string pname = (itp == sim.content().techs.end()) ? pre : itp->second.name;
                  const bool have = vec_contains(selected_faction->known_techs, pre);
                  ImGui::BulletText("%s%s (%s)", have ? "[ok] " : "[missing] ", pname.c_str(), pre.c_str());
                }
              }

              ImGui::Separator();
              ImGui::Text("Effects");
              if (chosen->effects.empty()) {
                ImGui::TextDisabled("(none)");
              } else {
                for (const auto& eff : chosen->effects) {
                  ImGui::BulletText("%s: %s", eff.type.c_str(), eff.value.c_str());
                }
              }

              ImGui::Separator();
              static std::string last_plan_error;

              const auto plan = nebula4x::compute_research_plan(sim.content(), *selected_faction, chosen_id);
              if (ImGui::CollapsingHeader("Plan", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!plan.ok()) {
                  ImGui::TextDisabled("(cannot compute plan)");
                  for (const auto& e : plan.errors) {
                    ImGui::BulletText("%s", e.c_str());
                  }
                } else {
                  ImGui::Text("Steps: %d", static_cast<int>(plan.plan.tech_ids.size()));
                  ImGui::Text("Total cost: %.0f", plan.plan.total_cost);
                  if (ImGui::BeginChild("##plan_list", ImVec2(-1, 100), true)) {
                    for (const auto& tid : plan.plan.tech_ids) {
                      const auto it2 = sim.content().techs.find(tid);
                      const std::string nm = (it2 == sim.content().techs.end()) ? tid : it2->second.name;
                      ImGui::BulletText("%s", nm.c_str());
                    }
                    ImGui::EndChild();
                  }
                }
              }

              const bool can_act = (!known);
              const bool can_set_active = can_act;

              if (!can_act) {
                ImGui::TextDisabled("(already researched)");
              }

              if (!can_set_active) ImGui::BeginDisabled();
              if (ImGui::Button("Set Active")) {
                selected_faction->active_research_id = chosen_id;
                selected_faction->active_research_progress = 0.0;
              }
              if (!can_set_active) ImGui::EndDisabled();

              ImGui::SameLine();
              if (!can_act) ImGui::BeginDisabled();
              if (ImGui::Button("Add to Queue")) {
                selected_faction->research_queue.push_back(chosen_id);
              }
              if (!can_act) ImGui::EndDisabled();

              if (!can_act) ImGui::BeginDisabled();
              if (ImGui::Button("Queue with prereqs")) {
                const auto plan2 = nebula4x::compute_research_plan(sim.content(), *selected_faction, chosen_id);
                if (!plan2.ok()) {
                  last_plan_error.clear();
                  for (const auto& e : plan2.errors) {
                    if (!last_plan_error.empty()) last_plan_error += "\n";
                    last_plan_error += e;
                  }
                  ImGui::OpenPopup("Research plan error");
                } else {
                  for (const auto& tid : plan2.plan.tech_ids) {
                    if (vec_contains(selected_faction->known_techs, tid)) continue;
                    if (selected_faction->active_research_id == tid) continue;
                    if (std::find(selected_faction->research_queue.begin(), selected_faction->research_queue.end(), tid) !=
                        selected_faction->research_queue.end()) {
                      continue;
                    }
                    selected_faction->research_queue.push_back(tid);
                  }
                }
              }
              ImGui::SameLine();
              if (ImGui::Button("Replace queue with plan")) {
                const auto plan2 = nebula4x::compute_research_plan(sim.content(), *selected_faction, chosen_id);
                if (!plan2.ok()) {
                  last_plan_error.clear();
                  for (const auto& e : plan2.errors) {
                    if (!last_plan_error.empty()) last_plan_error += "\n";
                    last_plan_error += e;
                  }
                  ImGui::OpenPopup("Research plan error");
                } else {
                  selected_faction->research_queue.clear();
                  for (const auto& tid : plan2.plan.tech_ids) {
                    if (vec_contains(selected_faction->known_techs, tid)) continue;
                    if (selected_faction->active_research_id == tid) continue;
                    selected_faction->research_queue.push_back(tid);
                  }
                }
              }
              if (!can_act) ImGui::EndDisabled();

              if (ImGui::BeginPopupModal("Research plan error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("Could not compute a valid research plan:");
                ImGui::Separator();
                ImGui::TextUnformatted(last_plan_error.c_str());
                ImGui::Separator();
                if (ImGui::Button("OK")) {
                  ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
              }
            }
          }

          ImGui::EndTable();
        }

        ImGui::EndTabItem();
      }
    }


    // --- Diplomacy tab ---
    if (ImGui::BeginTabItem("Diplomacy", nullptr, flags_for(DetailsTab::Diplomacy))) {
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
            "Diplomatic stances are used for rules-of-engagement: ships will only auto-engage factions they consider "
            "Hostile. Issuing an Attack order against a non-hostile faction will automatically set the relationship "
            "to Hostile once contact is confirmed.\n\n"
            "Mutual Friendly stances also enable cooperation: allied sensor coverage + discovered systems are shared, "
            "and ships may refuel/repair/transfer minerals at allied colonies.");

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
    if (ImGui::BeginTabItem("Design", nullptr, flags_for(DetailsTab::Design))) {
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

        // Allow other windows (e.g. production planner) to request that a
        // particular design becomes selected.
        if (!ui.request_focus_design_id.empty() && !all_ids.empty()) {
          const auto it = std::find(all_ids.begin(), all_ids.end(), ui.request_focus_design_id);
          if (it != all_ids.end()) {
            design_sel = static_cast<int>(std::distance(all_ids.begin(), it));
          }
          ui.request_focus_design_id.clear();
        }

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
            if (d->fuel_use_per_mkm > 0.0) {
              if (d->fuel_capacity_tons > 0.0) {
                ImGui::Text("Fuel: %.0f t  (use %.2f t/mkm, range %.0f mkm)", d->fuel_capacity_tons, d->fuel_use_per_mkm, d->fuel_capacity_tons / d->fuel_use_per_mkm);
              } else {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Fuel: 0 t  (needs fuel tanks)");
              }
            } else if (d->fuel_capacity_tons > 0.0) {
              ImGui::Text("Fuel: %.0f t", d->fuel_capacity_tons);
            } else {
              ImGui::TextDisabled("Fuel: (none)");
            }

            // Power budget (prototype)
            {
              const double gen = std::max(0.0, d->power_generation);
              const double use = std::max(0.0, d->power_use_total);
              if (gen > 0.0 || use > 0.0) {
                if (use <= gen + 1e-9) {
                  ImGui::Text("Power: %.1f gen / %.1f use", gen, use);
                } else {
                  ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                    "Power: %.1f gen / %.1f use (DEFICIT %.1f)", gen, use, use - gen);
                }

                const auto p = compute_power_allocation(gen, d->power_use_engines, d->power_use_shields,
                                                        d->power_use_weapons, d->power_use_sensors);
                ImGui::TextDisabled("Online: Engines %s, Shields %s, Weapons %s, Sensors %s  (avail %.1f)",
                                    p.engines_online ? "ON" : "OFF", p.shields_online ? "ON" : "OFF",
                                    p.weapons_online ? "ON" : "OFF", p.sensors_online ? "ON" : "OFF",
                                    p.available);
              } else {
                ImGui::TextDisabled("Power: (none)");
              }
            }
            ImGui::Text("HP: %.0f", d->max_hp);
            if (d->max_shields > 0.0) {
              ImGui::Text("Shields: %.0f (+%.1f/day)", d->max_shields, d->shield_regen_per_day);
            } else {
              ImGui::TextDisabled("Shields: (none)");
            }
            // A design isn't carrying cargo; only an instantiated ship has a cargo manifes.
            const double cargo_used_tons = 0.0;
            ImGui::Text("Cargo: %.0f / %.0f t", cargo_used_tons, d->cargo_tons);
            ImGui::Text("Sensor: %.0f mkm", d->sensor_range_mkm);
            ImGui::Text("Signature: %.0f%%", d->signature_multiplier * 100.0);
            if (d->colony_capacity_millions > 0.0) {
              ImGui::Text("Colony capacity: %.0f M", d->colony_capacity_millions);
            }
            if (d->weapon_damage > 0.0) {
              ImGui::Text("Beam weapons: %.1f (range %.1f)", d->weapon_damage, d->weapon_range_mkm);
            } else {
              ImGui::TextDisabled("Beam weapons: (none)");
            }

            if (d->missile_damage > 0.0 && d->missile_range_mkm > 0.0) {
              ImGui::Text("Missiles: %.1f dmg/salvo (range %.1f, speed %.1f, reload %.1f d)", d->missile_damage,
                          d->missile_range_mkm, d->missile_speed_mkm_per_day, d->missile_reload_days);
            } else {
              ImGui::TextDisabled("Missiles: (none)");
            }

            if (d->point_defense_damage > 0.0 && d->point_defense_range_mkm > 0.0) {
              ImGui::Text("Point defense: %.1f (range %.1f)", d->point_defense_damage, d->point_defense_range_mkm);
            } else {
              ImGui::TextDisabled("Point defense: (none)");
            }
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
              case ComponentType::FuelTank: return 2;
              case ComponentType::Cargo: return 3;
              case ComponentType::ColonyModule: return 4;
              case ComponentType::Sensor: return 5;
              case ComponentType::Weapon: return 6;
              case ComponentType::Armor: return 7;
              case ComponentType::Shield: return 8;
              default: return 99;
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
        const char* filters[] = {"All", "Engine", "Fuel Tank", "Cargo", "Sensor", "Reactor", "Weapon", "Armor", "Shield", "Colony Module"};
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
                : (comp_filter == 2) ? ComponentType::FuelTank
                : (comp_filter == 3) ? ComponentType::Cargo
                : (comp_filter == 4) ? ComponentType::Sensor
                : (comp_filter == 5) ? ComponentType::Reactor
                : (comp_filter == 6) ? ComponentType::Weapon
                : (comp_filter == 7) ? ComponentType::Armor
                : (comp_filter == 8) ? ComponentType::Shield
                : (comp_filter == 9) ? ComponentType::ColonyModule
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
              if (c.power_output > 0.0) ImGui::TextDisabled("Power output: %.1f", c.power_output);
              if (c.power_use > 0.0) ImGui::TextDisabled("Power use: %.1f", c.power_use);
              if (c.cargo_tons > 0.0) ImGui::TextDisabled("Cargo: %.0f t", c.cargo_tons);
              if (c.fuel_capacity_tons > 0.0) ImGui::TextDisabled("Fuel cap: %.0f t", c.fuel_capacity_tons);
              if (c.fuel_use_per_mkm > 0.0) ImGui::TextDisabled("Fuel use: %.2f t/mkm", c.fuel_use_per_mkm);
              if (c.sensor_range_mkm > 0.0) ImGui::TextDisabled("Sensor: %.0f mkm", c.sensor_range_mkm);
              if (c.colony_capacity_millions > 0.0)
                ImGui::TextDisabled("Colony capacity: %.0f M", c.colony_capacity_millions);
              if (c.weapon_damage > 0.0) ImGui::TextDisabled("Beam weapon: %.1f (range %.1f)", c.weapon_damage, c.weapon_range_mkm);
              if (c.missile_damage > 0.0)
                ImGui::TextDisabled("Missile: %.1f (range %.1f, speed %.1f, reload %.1f d)", c.missile_damage,
                                    c.missile_range_mkm, c.missile_speed_mkm_per_day, c.missile_reload_days);
              if (c.point_defense_damage > 0.0)
                ImGui::TextDisabled("Point defense: %.1f (range %.1f)", c.point_defense_damage,
                                    c.point_defense_range_mkm);
              if (c.hp_bonus > 0.0) ImGui::TextDisabled("HP bonus: %.0f", c.hp_bonus);
              if (c.shield_hp > 0.0) {
                ImGui::TextDisabled("Shield: %.0f (+%.1f/day)", c.shield_hp, c.shield_regen_per_day);
              }
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
        if (preview.fuel_use_per_mkm > 0.0) {
          if (preview.fuel_capacity_tons > 0.0) {
            ImGui::Text("Fuel: %.0f t  (use %.2f t/mkm, range %.0f mkm)", preview.fuel_capacity_tons,
                        preview.fuel_use_per_mkm, preview.fuel_capacity_tons / preview.fuel_use_per_mkm);
          } else {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Fuel: 0 t  (needs fuel tanks)");
          }
        } else if (preview.fuel_capacity_tons > 0.0) {
          ImGui::Text("Fuel: %.0f t", preview.fuel_capacity_tons);
        } else {
          ImGui::TextDisabled("Fuel: (none)");
        }

        // Power budget (prototype).
        {
          const double gen = std::max(0.0, preview.power_generation);
          const double use = std::max(0.0, preview.power_use_total);
          if (gen > 0.0 || use > 0.0) {
            if (use <= gen + 1e-9) {
              ImGui::Text("Power: %.1f gen / %.1f use", gen, use);
            } else {
              ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Power: %.1f gen / %.1f use  (DEFICIT %.1f)",
                                gen, use, use - gen);
            }

            // Show load-shedding outcome using the same logic as the simulation.
            const auto p = compute_power_allocation(gen, preview.power_use_engines, preview.power_use_shields,
                                                    preview.power_use_weapons, preview.power_use_sensors);
            ImGui::TextDisabled("Load shed: Engines %s  Shields %s  Weapons %s  Sensors %s", p.engines_online ? "ON" : "OFF",
                                p.shields_online ? "ON" : "OFF", p.weapons_online ? "ON" : "OFF",
                                p.sensors_online ? "ON" : "OFF");

            if (preview.power_use_engines > 0.0 || preview.power_use_shields > 0.0 || preview.power_use_weapons > 0.0 ||
                preview.power_use_sensors > 0.0) {
              ImGui::TextDisabled("Use breakdown: Eng %.1f  Sh %.1f  Wpn %.1f  Sen %.1f", preview.power_use_engines,
                                  preview.power_use_shields, preview.power_use_weapons, preview.power_use_sensors);
            }
          } else {
            ImGui::TextDisabled("Power: (none)");
          }
        }
        ImGui::Text("HP: %.0f", preview.max_hp);
        if (preview.max_shields > 0.0) {
          ImGui::Text("Shields: %.0f (+%.1f/day)", preview.max_shields, preview.shield_regen_per_day);
        } else {
          ImGui::TextDisabled("Shields: (none)");
        }
        ImGui::Text("Cargo: %.0f t", preview.cargo_tons);
        ImGui::Text("Sensor: %.0f mkm", preview.sensor_range_mkm);
        ImGui::Text("Signature: %.0f%%", preview.signature_multiplier * 100.0);
        if (preview.colony_capacity_millions > 0.0)
          ImGui::Text("Colony capacity: %.0f M", preview.colony_capacity_millions);
        if (preview.weapon_damage > 0.0) {
          ImGui::Text("Beam weapons: %.1f (range %.1f)", preview.weapon_damage, preview.weapon_range_mkm);
        } else {
          ImGui::TextDisabled("Beam weapons: (none)");
        }

        if (preview.missile_damage > 0.0 && preview.missile_range_mkm > 0.0) {
          ImGui::Text("Missiles: %.1f dmg/salvo (range %.1f, speed %.1f, reload %.1f d)", preview.missile_damage,
                      preview.missile_range_mkm, preview.missile_speed_mkm_per_day, preview.missile_reload_days);
        } else {
          ImGui::TextDisabled("Missiles: (none)");
        }

        if (preview.point_defense_damage > 0.0 && preview.point_defense_range_mkm > 0.0) {
          ImGui::Text("Point defense: %.1f (range %.1f)", preview.point_defense_damage, preview.point_defense_range_mkm);
        } else {
          ImGui::TextDisabled("Point defense: (none)");
        }

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
    if (ImGui::BeginTabItem("Contacts", nullptr, flags_for(DetailsTab::Contacts))) {
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

      if (ImGui::BeginTabItem(log_label.c_str(), nullptr, flags_for(DetailsTab::Log))) {
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
              "Diplomacy",
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
                EventCategory::Diplomacy,
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
            out += std::string("[") + format_datetime(d, ev.hour) + "] #" +
                   std::to_string(static_cast<unsigned long long>(ev.seq)) +
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
          const std::string dt = format_datetime(d, ev.hour);
          ImGui::BulletText("[%s] #%llu [%s] %s: %s", dt.c_str(),
                            static_cast<unsigned long long>(ev.seq), event_category_label(ev.category),
                            event_level_label(ev.level), ev.message.c_str());

          ImGui::PushID(i);
          ImGui::SameLine();
          if (ImGui::SmallButton("Copy")) {
            std::string line = std::string("[") + dt + "] #" +
                              std::to_string(static_cast<unsigned long long>(ev.seq)) + " [" +
                              event_category_label(ev.category) + "] " + event_level_label(ev.level) + ": " +
                              ev.message;
            ImGui::SetClipboardText(line.c_str());
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Timeline")) {
            ui.show_timeline_window = true;
            ui.request_focus_event_seq = ev.seq;
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

    // Consume any programmatic tab selection request once we have rendered the tab bar.
    if (req_tab != DetailsTab::None) ui.request_details_tab = DetailsTab::None;
  }
}

void draw_settings_window(UIState& ui, char* ui_prefs_path, UIPrefActions& actions) {
  ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Settings", &ui.show_settings_window)) {
    ImGui::End();
    return;
  }

  ImGui::SeparatorText("Theme & Backgrounds");
  ImGui::ColorEdit4("Clear background (SDL)", ui.clear_color);
  ImGui::ColorEdit4("System map background", ui.system_map_bg);
  ImGui::ColorEdit4("Galaxy map background", ui.galaxy_map_bg);
  ImGui::Checkbox("Override window background", &ui.override_window_bg);
  if (ui.override_window_bg) {
    ImGui::ColorEdit4("Window background", ui.window_bg);
  }
  if (ImGui::Button("Reset theme defaults")) {
    actions.reset_ui_theme = true;
  }

  ImGui::SeparatorText("Map rendering");
  ImGui::Checkbox("System: starfield", &ui.system_map_starfield);
  ImGui::SameLine();
  ImGui::Checkbox("Galaxy: starfield", &ui.galaxy_map_starfield);
  ImGui::Checkbox("System: grid", &ui.system_map_grid);
  ImGui::SameLine();
  ImGui::Checkbox("Galaxy: grid", &ui.galaxy_map_grid);
  ImGui::Checkbox("System: order paths", &ui.system_map_order_paths);
  ImGui::SameLine();
  ImGui::Checkbox("System: fleet formation preview", &ui.system_map_fleet_formation_preview);
  ImGui::SameLine();
  ImGui::Checkbox("Galaxy: selected route", &ui.galaxy_map_selected_route);
  ImGui::Checkbox("System: follow selected ship", &ui.system_map_follow_selected);

  ImGui::Checkbox("System: weapon range rings (selected)", &ui.show_selected_weapon_range);
  ImGui::SameLine();
  ImGui::Checkbox("Fleet", &ui.show_fleet_weapon_ranges);
  ImGui::SameLine();
  ImGui::Checkbox("Hostiles", &ui.show_hostile_weapon_ranges);

  ImGui::SeparatorText("Exploration & Intel overlays");
  ImGui::Checkbox("Selected: sensor range ring", &ui.show_selected_sensor_range);
  ImGui::Checkbox("System: contact markers", &ui.show_contact_markers);
  ImGui::SameLine();
  ImGui::Checkbox("Labels##contacts", &ui.show_contact_labels);
  ImGui::Checkbox("System: minor bodies", &ui.show_minor_bodies);
  ImGui::SameLine();
  ImGui::Checkbox("Labels##minor_bodies", &ui.show_minor_body_labels);

  ImGui::Checkbox("Galaxy: labels", &ui.show_galaxy_labels);
  ImGui::SameLine();
  ImGui::Checkbox("Jump lines", &ui.show_galaxy_jump_lines);
  ImGui::SameLine();
  ImGui::Checkbox("Unknown exits (unsurveyed / undiscovered)", &ui.show_galaxy_unknown_exits);
  ImGui::SameLine();
  ImGui::Checkbox("Intel alerts", &ui.show_galaxy_intel_alerts);

  ImGui::SliderInt("Contact max age (days)", &ui.contact_max_age_days, 1, 3650);
  ui.contact_max_age_days = std::clamp(ui.contact_max_age_days, 1, 3650);

  ImGui::SliderFloat("Starfield density", &ui.map_starfield_density, 0.0f, 4.0f, "%.2fx");
  ui.map_starfield_density = std::clamp(ui.map_starfield_density, 0.0f, 4.0f);
  ImGui::SliderFloat("Starfield parallax", &ui.map_starfield_parallax, 0.0f, 1.0f, "%.2f");
  ui.map_starfield_parallax = std::clamp(ui.map_starfield_parallax, 0.0f, 1.0f);
  ImGui::SliderFloat("Grid opacity", &ui.map_grid_opacity, 0.0f, 1.0f, "%.2f");
  ui.map_grid_opacity = std::clamp(ui.map_grid_opacity, 0.0f, 1.0f);
  ImGui::SliderFloat("Route opacity", &ui.map_route_opacity, 0.0f, 1.0f, "%.2f");
  ui.map_route_opacity = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);

  ImGui::SeparatorText("UI prefs file");
  ImGui::InputText("Path##ui_prefs_path", ui_prefs_path, 256);
  ImGui::Checkbox("Autosave on exit", &ui.autosave_ui_prefs);
  if (ImGui::Button("Load UI prefs")) {
    actions.load_ui_prefs = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Save UI prefs")) {
    actions.save_ui_prefs = true;
  }

  ImGui::SeparatorText("HUD & Accessibility");
  ImGui::SliderFloat("UI scale", &ui.ui_scale, 0.65f, 2.5f, "%.2fx");
  ui.ui_scale = std::clamp(ui.ui_scale, 0.65f, 2.5f);
  ImGui::Checkbox("Status bar", &ui.show_status_bar);
  ImGui::Checkbox("Event toasts (warn/error)", &ui.show_event_toasts);
  if (ui.show_event_toasts) {
    ImGui::SliderFloat("Toast duration (sec)", &ui.event_toast_duration_sec, 1.0f, 30.0f, "%.0f");
    ui.event_toast_duration_sec = std::clamp(ui.event_toast_duration_sec, 0.5f, 60.0f);
  }
  ImGui::TextDisabled(
      "Shortcuts: Ctrl+P palette, F1 help, Ctrl+S save, Ctrl+O load, Ctrl+0 diplomacy, Ctrl+7 timeline, Ctrl+8 design studio, Ctrl+9 intel, Space +1 day.");

  ImGui::SeparatorText("Timeline");
  ImGui::Checkbox("Show timeline minimap", &ui.timeline_show_minimap);
  ImGui::Checkbox("Show timeline grid", &ui.timeline_show_grid);
  ImGui::Checkbox("Show lane labels", &ui.timeline_show_labels);
  ImGui::Checkbox("Compact rows", &ui.timeline_compact_rows);
  ImGui::Checkbox("Follow now by default", &ui.timeline_follow_now);
  ImGui::SliderFloat("Lane height##timeline", &ui.timeline_lane_height, 18.0f, 56.0f, "%.0f px");
  ui.timeline_lane_height = std::clamp(ui.timeline_lane_height, 18.0f, 80.0f);
  ImGui::SliderFloat("Marker size##timeline", &ui.timeline_marker_size, 2.5f, 7.0f, "%.1f px");
  ui.timeline_marker_size = std::clamp(ui.timeline_marker_size, 2.0f, 12.0f);

  ImGui::SeparatorText("Design Studio");
  ImGui::Checkbox("Show grid##design_studio", &ui.design_studio_show_grid);
  ImGui::Checkbox("Show labels##design_studio", &ui.design_studio_show_labels);
  ImGui::Checkbox("Compare by default##design_studio", &ui.design_studio_show_compare);
  ImGui::Checkbox("Power overlay##design_studio", &ui.design_studio_show_power_overlay);

  ImGui::SeparatorText("Intel");
  ImGui::Checkbox("Radar: scanline", &ui.intel_radar_scanline);
  ImGui::SameLine();
  ImGui::Checkbox("Grid/range rings", &ui.intel_radar_grid);
  ImGui::Checkbox("Radar: sensor coverage", &ui.intel_radar_show_sensors);
  ImGui::SameLine();
  ImGui::Checkbox("Heat##intel", &ui.intel_radar_sensor_heat);
  ImGui::Checkbox("Radar: bodies", &ui.intel_radar_show_bodies);
  ImGui::SameLine();
  ImGui::Checkbox("Jump points", &ui.intel_radar_show_jump_points);
  ImGui::Checkbox("Radar: friendlies", &ui.intel_radar_show_friendlies);
  ImGui::SameLine();
  ImGui::Checkbox("Hostiles", &ui.intel_radar_show_hostiles);
  ImGui::SameLine();
  ImGui::Checkbox("Contacts", &ui.intel_radar_show_contacts);
  ImGui::Checkbox("Radar: labels", &ui.intel_radar_labels);

  ImGui::SeparatorText("Diplomacy Graph");
  ImGui::Checkbox("Starfield##dipl", &ui.diplomacy_graph_starfield);
  ImGui::SameLine();
  ImGui::Checkbox("Grid##dipl", &ui.diplomacy_graph_grid);
  ImGui::Checkbox("Labels##dipl", &ui.diplomacy_graph_labels);
  ImGui::SameLine();
  ImGui::Checkbox("Arrows##dipl", &ui.diplomacy_graph_arrows);
  ImGui::Checkbox("Dim non-selected##dipl", &ui.diplomacy_graph_dim_nonfocus);
  ImGui::Checkbox("Show Hostile##dipl", &ui.diplomacy_graph_show_hostile);
  ImGui::SameLine();
  ImGui::Checkbox("Neutral##dipl", &ui.diplomacy_graph_show_neutral);
  ImGui::SameLine();
  ImGui::Checkbox("Friendly##dipl", &ui.diplomacy_graph_show_friendly);
  {
    const char* layouts[] = {"Radial", "Force", "Circle"};
    ui.diplomacy_graph_layout = std::clamp(ui.diplomacy_graph_layout, 0, 2);
    ImGui::Combo("Layout##dipl", &ui.diplomacy_graph_layout, layouts, IM_ARRAYSIZE(layouts));
  }

  ImGui::SeparatorText("Windows");
  ImGui::Checkbox("Controls", &ui.show_controls_window);
  ImGui::Checkbox("Map", &ui.show_map_window);
  ImGui::Checkbox("Details", &ui.show_details_window);
  ImGui::Checkbox("Directory", &ui.show_directory_window);
  ImGui::Checkbox("Production", &ui.show_production_window);
  ImGui::Checkbox("Economy", &ui.show_economy_window);
  ImGui::Checkbox("Timeline", &ui.show_timeline_window);
  ImGui::Checkbox("Design Studio", &ui.show_design_studio_window);
  ImGui::Checkbox("Intel", &ui.show_intel_window);
  ImGui::Checkbox("Diplomacy Graph", &ui.show_diplomacy_window);
  if (ImGui::Button("Reset window layout")) {
    actions.reset_window_layout = true;
  }

  ImGui::SeparatorText("Docking");
  ImGui::Checkbox("Hold Shift to dock", &ui.docking_with_shift);
  ImGui::Checkbox("Always show tab bars", &ui.docking_always_tab_bar);
  ImGui::Checkbox("Transparent docking preview", &ui.docking_transparent_payload);
  {
    const char* ini = ImGui::GetIO().IniFilename;
    ImGui::TextDisabled("Layout file: %s", (ini && ini[0]) ? ini : "(none)");
  }

  ImGui::SeparatorText("Notes");
  ImGui::TextWrapped(
      "Theme/layout settings are stored separately from save-games. Use 'UI Prefs' to persist your UI theme "
      "(including background colors) and window visibility.");

  ImGui::End();
}

void draw_directory_window(Simulation& sim, UIState& ui, Id& selected_colony, Id& selected_body) {
  auto& s = sim.state();

  ImGui::SetNextWindowSize(ImVec2(860, 520), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Directory", &ui.show_directory_window)) {
    ImGui::End();
    return;
  }

  if (!ImGui::BeginTabBar("directory_tabs")) {
    ImGui::End();
    return;
  }

  // --- Colonies tab ---
  if (ImGui::BeginTabItem("Colonies")) {
    static char search[128] = "";
    static int faction_filter_idx = 0; // 0 = All
    static int system_filter_idx = 0;  // 0 = All

    const auto factions = sorted_factions(s);
    const auto systems = sorted_systems(s);

    // Filter controls
    ImGui::InputTextWithHint("Search##colony", "name / system / body", search, IM_ARRAYSIZE(search));

    {
      std::vector<const char*> labels;
      labels.reserve(factions.size() + 1);
      labels.push_back("All factions");
      for (const auto& p : factions) labels.push_back(p.second.c_str());
      if (labels.size() > 1) {
        faction_filter_idx = std::clamp(faction_filter_idx, 0, static_cast<int>(labels.size()) - 1);
      } else {
        faction_filter_idx = 0;
      }
      ImGui::Combo("Faction##colony", &faction_filter_idx, labels.data(), static_cast<int>(labels.size()));
    }

    {
      std::vector<const char*> labels;
      labels.reserve(systems.size() + 1);
      labels.push_back("All systems");
      for (const auto& p : systems) labels.push_back(p.second.c_str());
      if (labels.size() > 1) {
        system_filter_idx = std::clamp(system_filter_idx, 0, static_cast<int>(labels.size()) - 1);
      } else {
        system_filter_idx = 0;
      }
      ImGui::Combo("System##colony", &system_filter_idx, labels.data(), static_cast<int>(labels.size()));
    }

    const Id faction_filter = (faction_filter_idx <= 0 || factions.empty()) ? kInvalidId : factions[faction_filter_idx - 1].first;
    const Id system_filter = (system_filter_idx <= 0 || systems.empty()) ? kInvalidId : systems[system_filter_idx - 1].first;

    struct ColonyRow {
      Id id{kInvalidId};
      Id faction_id{kInvalidId};
      Id system_id{kInvalidId};
      Id body_id{kInvalidId};
      std::string name;
      std::string system;
      std::string body;
      std::string faction;
      double pop{0.0};
      double cp_day{0.0};
      double fuel{0.0};
      int shipyards{0};
    };

    std::vector<ColonyRow> rows;
    rows.reserve(s.colonies.size());

    for (const auto& [cid, c] : s.colonies) {
      if (faction_filter != kInvalidId && c.faction_id != faction_filter) continue;

      const Body* b = (c.body_id != kInvalidId) ? find_ptr(s.bodies, c.body_id) : nullptr;
      const StarSystem* sys = b ? find_ptr(s.systems, b->system_id) : nullptr;
      if (system_filter != kInvalidId && (!sys || sys->id != system_filter)) continue;

      const Faction* fac = find_ptr(s.factions, c.faction_id);

      // Search matches colony name, body, or system.
      if (!case_insensitive_contains(c.name, search) &&
          !(b && case_insensitive_contains(b->name, search)) &&
          !(sys && case_insensitive_contains(sys->name, search))) {
        continue;
      }

      ColonyRow r;
      r.id = cid;
      r.faction_id = c.faction_id;
      r.system_id = sys ? sys->id : kInvalidId;
      r.body_id = c.body_id;
      r.name = c.name;
      r.system = sys ? sys->name : "?";
      r.body = b ? b->name : "?";
      r.faction = fac ? fac->name : "?";
      r.pop = c.population_millions;
      r.cp_day = sim.construction_points_per_day(c);
      if (auto it = c.minerals.find("Fuel"); it != c.minerals.end()) r.fuel = it->second;
      if (auto it = c.installations.find("shipyard"); it != c.installations.end()) r.shipyards = it->second;
      rows.push_back(std::move(r));
    }

    ImGui::Separator();
    ImGui::TextDisabled("Showing %d / %d colonies", (int)rows.size(), (int)s.colonies.size());

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                 ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                                 ImGuiTableFlags_ScrollY;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("colony_directory", 8, flags, ImVec2(avail.x, avail.y))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
      ImGui::TableSetupColumn("System", 0, 0.0f, 1);
      ImGui::TableSetupColumn("Body", 0, 0.0f, 2);
      ImGui::TableSetupColumn("Faction", 0, 0.0f, 3);
      ImGui::TableSetupColumn("Pop (M)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 4);
      ImGui::TableSetupColumn("CP/day", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 5);
      ImGui::TableSetupColumn("Fuel", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 6);
      ImGui::TableSetupColumn("Shipyards", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 7);
      ImGui::TableHeadersRow();

      if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
        if (sort->SpecsDirty && sort->SpecsCount > 0) {
          const ImGuiTableColumnSortSpecs* spec = &sort->Specs[0];
          const bool asc = (spec->SortDirection == ImGuiSortDirection_Ascending);
          auto cmp = [&](const ColonyRow& a, const ColonyRow& b) {
            auto lt = [&](auto x, auto y) { return asc ? (x < y) : (x > y); };
            switch (spec->ColumnUserID) {
              case 0: return lt(a.name, b.name);
              case 1: return lt(a.system, b.system);
              case 2: return lt(a.body, b.body);
              case 3: return lt(a.faction, b.faction);
              case 4: return lt(a.pop, b.pop);
              case 5: return lt(a.cp_day, b.cp_day);
              case 6: return lt(a.fuel, b.fuel);
              case 7: return lt(a.shipyards, b.shipyards);
              default: return lt(a.name, b.name);
            }
          };
          std::stable_sort(rows.begin(), rows.end(), cmp);
          sort->SpecsDirty = false;
        }
      }

      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(rows.size()));
      while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
          const ColonyRow& r = rows[i];
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          const bool is_sel = (selected_colony == r.id);
          std::string label = r.name + "##colony_" + std::to_string((int)r.id);
          if (ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_SpanAllColumns)) {
            selected_colony = r.id;
            if (r.system_id != kInvalidId) s.selected_system = r.system_id;
            selected_body = r.body_id;
          }

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(r.system.c_str());
          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(r.body.c_str());
          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(r.faction.c_str());
          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%.2f", r.pop);
          ImGui::TableSetColumnIndex(5);
          ImGui::Text("%.1f", r.cp_day);
          ImGui::TableSetColumnIndex(6);
          ImGui::Text("%.1f", r.fuel);
          ImGui::TableSetColumnIndex(7);
          ImGui::Text("%d", r.shipyards);
        }
      }

      ImGui::EndTable();
    }

    ImGui::EndTabItem();
  }

  // --- Bodies tab ---
  if (ImGui::BeginTabItem("Bodies")) {
    static char search[128] = "";
    static int system_filter_idx = 0; // 0 = All
    static int type_filter_idx = 0;   // 0 = All
    static bool only_colonized = false;

    const auto systems = sorted_systems(s);

    ImGui::InputTextWithHint("Search##body", "name / system", search, IM_ARRAYSIZE(search));
    {
      std::vector<const char*> labels;
      labels.reserve(systems.size() + 1);
      labels.push_back("All systems");
      for (const auto& p : systems) labels.push_back(p.second.c_str());
      if (labels.size() > 1) {
        system_filter_idx = std::clamp(system_filter_idx, 0, static_cast<int>(labels.size()) - 1);
      } else {
        system_filter_idx = 0;
      }
      ImGui::Combo("System##body", &system_filter_idx, labels.data(), static_cast<int>(labels.size()));
    }

    {
      const char* types[] = {"All", "Star", "Planet", "Moon", "Asteroid", "Comet", "Gas Giant"};
      ImGui::Combo("Type##body", &type_filter_idx, types, IM_ARRAYSIZE(types));
    }
    ImGui::Checkbox("Only colonized##body", &only_colonized);

    const Id system_filter = (system_filter_idx <= 0 || systems.empty()) ? kInvalidId : systems[system_filter_idx - 1].first;

    auto type_ok = [&](BodyType t) {
      switch (type_filter_idx) {
        case 1: return t == BodyType::Star;
        case 2: return t == BodyType::Planet;
        case 3: return t == BodyType::Moon;
        case 4: return t == BodyType::Asteroid;
        case 5: return t == BodyType::Comet;
        case 6: return t == BodyType::GasGiant;
        default: return true;
      }
    };

    // Precompute body->colony mapping.
    std::unordered_map<Id, Id> body_to_colony;
    body_to_colony.reserve(s.colonies.size() * 2);
    for (const auto& [cid, c] : s.colonies) {
      if (c.body_id != kInvalidId) body_to_colony[c.body_id] = cid;
    }

    struct BodyRow {
      Id id{kInvalidId};
      Id system_id{kInvalidId};
      BodyType type{BodyType::Planet};
      std::string name;
      std::string system;
      double orbit{0.0};
      double deposits{0.0};
      Id colony_id{kInvalidId};
      double colony_pop{0.0};
    };

    std::vector<BodyRow> rows;
    rows.reserve(s.bodies.size());

    for (const auto& [bid, b] : s.bodies) {
      const StarSystem* sys = find_ptr(s.systems, b.system_id);
      if (system_filter != kInvalidId && b.system_id != system_filter) continue;
      if (!type_ok(b.type)) continue;

      const auto itc = body_to_colony.find(bid);
      const Id colony_id = (itc == body_to_colony.end()) ? kInvalidId : itc->second;
      if (only_colonized && colony_id == kInvalidId) continue;

      if (!case_insensitive_contains(b.name, search) && !(sys && case_insensitive_contains(sys->name, search))) {
        continue;
      }

      double dep_total = 0.0;
      for (const auto& [_, v] : b.mineral_deposits) dep_total += std::max(0.0, v);

      BodyRow r;
      r.id = bid;
      r.system_id = b.system_id;
      r.type = b.type;
      r.name = b.name;
      r.system = sys ? sys->name : "?";
      r.orbit = b.orbit_radius_mkm;
      r.deposits = dep_total;
      r.colony_id = colony_id;
      if (const Colony* c = (colony_id != kInvalidId) ? find_ptr(s.colonies, colony_id) : nullptr) {
        r.colony_pop = c->population_millions;
      }
      rows.push_back(std::move(r));
    }

    ImGui::Separator();
    ImGui::TextDisabled("Showing %d bodies", (int)rows.size());

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                 ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                                 ImGuiTableFlags_ScrollY;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("body_directory", 7, flags, ImVec2(avail.x, avail.y))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
      ImGui::TableSetupColumn("Type", 0, 0.0f, 1);
      ImGui::TableSetupColumn("System", 0, 0.0f, 2);
      ImGui::TableSetupColumn("Orbit (mkm)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 3);
      ImGui::TableSetupColumn("Deposits", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 4);
      ImGui::TableSetupColumn("Colonized", 0, 0.0f, 5);
      ImGui::TableSetupColumn("Pop (M)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 6);
      ImGui::TableHeadersRow();

      if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
        if (sort->SpecsDirty && sort->SpecsCount > 0) {
          const ImGuiTableColumnSortSpecs* spec = &sort->Specs[0];
          const bool asc = (spec->SortDirection == ImGuiSortDirection_Ascending);
          auto cmp = [&](const BodyRow& a, const BodyRow& b) {
            auto lt = [&](auto x, auto y) { return asc ? (x < y) : (x > y); };
            switch (spec->ColumnUserID) {
              case 0: return lt(a.name, b.name);
              case 1: return lt((int)a.type, (int)b.type);
              case 2: return lt(a.system, b.system);
              case 3: return lt(a.orbit, b.orbit);
              case 4: return lt(a.deposits, b.deposits);
              case 5: return lt(a.colony_id != kInvalidId, b.colony_id != kInvalidId);
              case 6: return lt(a.colony_pop, b.colony_pop);
              default: return lt(a.name, b.name);
            }
          };
          std::stable_sort(rows.begin(), rows.end(), cmp);
          sort->SpecsDirty = false;
        }
      }

      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(rows.size()));
      while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
          const BodyRow& r = rows[i];
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          const bool is_sel = (selected_body == r.id);
          std::string label = r.name + "##body_" + std::to_string((int)r.id);
          if (ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_SpanAllColumns)) {
            selected_body = r.id;
            if (r.system_id != kInvalidId) s.selected_system = r.system_id;
            if (r.colony_id != kInvalidId) selected_colony = r.colony_id;
          }

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(body_type_label(r.type));
          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(r.system.c_str());
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%.1f", r.orbit);
          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%.1f", r.deposits);
          ImGui::TableSetColumnIndex(5);
          if (r.colony_id != kInvalidId) {
            ImGui::TextUnformatted("Yes");
          } else {
            ImGui::TextDisabled("No");
          }
          ImGui::TableSetColumnIndex(6);
          if (r.colony_id != kInvalidId) {
            ImGui::Text("%.2f", r.colony_pop);
          } else {
            ImGui::TextDisabled("-");
          }
        }
      }

      ImGui::EndTable();
    }

    ImGui::EndTabItem();
  }

  // --- Wrecks tab ---
  if (ImGui::BeginTabItem("Wrecks")) {
    static char search[128] = "";
    static int system_filter_idx = 0; // 0 = All

    const auto systems = sorted_systems(s);

    ImGui::InputTextWithHint("Search##wreck", "name / system / source", search, IM_ARRAYSIZE(search));
    {
      std::vector<const char*> labels;
      labels.reserve(systems.size() + 1);
      labels.push_back("All systems");
      for (const auto& p : systems) labels.push_back(p.second.c_str());
      if (labels.size() > 1) {
        system_filter_idx = std::clamp(system_filter_idx, 0, static_cast<int>(labels.size()) - 1);
      } else {
        system_filter_idx = 0;
      }
      ImGui::Combo("System##wreck", &system_filter_idx, labels.data(), static_cast<int>(labels.size()));
    }

    const Id system_filter = (system_filter_idx <= 0 || systems.empty()) ? kInvalidId : systems[system_filter_idx - 1].first;

    if (ui.fog_of_war && ui.viewer_faction_id != kInvalidId) {
      ImGui::TextDisabled("Fog-of-war is enabled: only discovered systems are listed.");
    }

    struct WreckRow {
      Id id{kInvalidId};
      Id system_id{kInvalidId};
      Vec2 pos{0.0, 0.0};
      std::string name;
      std::string system;
      std::string source;
      double total{0.0};
      std::int64_t age_days{0};
    };

    std::vector<WreckRow> rows;
    rows.reserve(s.wrecks.size());

    const std::int64_t cur_day = s.date.days_since_epoch();

    for (const auto& [wid, w] : s.wrecks) {
      if (system_filter != kInvalidId && w.system_id != system_filter) continue;
      if (ui.fog_of_war && ui.viewer_faction_id != kInvalidId) {
        if (!sim.is_system_discovered_by_faction(ui.viewer_faction_id, w.system_id)) continue;
      }

      const StarSystem* sys = find_ptr(s.systems, w.system_id);

      // Search matches wreck name, system, or source design.
      if (!case_insensitive_contains(w.name, search) &&
          !(sys && case_insensitive_contains(sys->name, search)) &&
          !case_insensitive_contains(w.source_design_id, search)) {
        continue;
      }

      double total = 0.0;
      for (const auto& [_, v] : w.minerals) total += std::max(0.0, v);

      WreckRow r;
      r.id = wid;
      r.system_id = w.system_id;
      r.pos = w.position_mkm;
      r.name = w.name.empty() ? (std::string("Wreck ") + std::to_string((int)wid)) : w.name;
      r.system = sys ? sys->name : "?";
      r.total = total;
      r.age_days = (w.created_day == 0) ? 0 : std::max<std::int64_t>(0, cur_day - w.created_day);

      // Compact source label.
      if (!w.source_design_id.empty()) {
        r.source = w.source_design_id;
      } else if (w.source_ship_id != kInvalidId) {
        r.source = std::string("Ship ") + std::to_string((int)w.source_ship_id);
      } else {
        r.source = "-";
      }
      rows.push_back(std::move(r));
    }

    ImGui::Separator();
    ImGui::TextDisabled("Showing %d / %d wrecks", (int)rows.size(), (int)s.wrecks.size());

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                 ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                                 ImGuiTableFlags_ScrollY;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("wreck_directory", 6, flags, ImVec2(avail.x, avail.y))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
      ImGui::TableSetupColumn("System", 0, 0.0f, 1);
      ImGui::TableSetupColumn("Total (t)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 2);
      ImGui::TableSetupColumn("Age (d)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 3);
      ImGui::TableSetupColumn("Source", 0, 0.0f, 4);
      ImGui::TableSetupColumn("Center", ImGuiTableColumnFlags_NoSort, 0.0f, 5);
      ImGui::TableHeadersRow();

      if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
        if (sort->SpecsDirty && sort->SpecsCount > 0) {
          const ImGuiTableColumnSortSpecs* spec = &sort->Specs[0];
          const bool asc = (spec->SortDirection == ImGuiSortDirection_Ascending);
          auto cmp = [&](const WreckRow& a, const WreckRow& b) {
            auto lt = [&](auto x, auto y) { return asc ? (x < y) : (x > y); };
            switch (spec->ColumnUserID) {
              case 0: return lt(a.name, b.name);
              case 1: return lt(a.system, b.system);
              case 2: return lt(a.total, b.total);
              case 3: return lt(a.age_days, b.age_days);
              case 4: return lt(a.source, b.source);
              default: return lt(a.name, b.name);
            }
          };
          std::stable_sort(rows.begin(), rows.end(), cmp);
          sort->SpecsDirty = false;
        }
      }

      static Id selected_wreck = kInvalidId;

      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(rows.size()));
      while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
          const WreckRow& r = rows[i];
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          const bool is_sel = (selected_wreck == r.id);
          std::string label = r.name + "##wreck_" + std::to_string((int)r.id);
          if (ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_SpanAllColumns)) {
            selected_wreck = r.id;
            if (r.system_id != kInvalidId) {
              s.selected_system = r.system_id;
              ui.request_map_tab = MapTab::System;
              ui.request_system_map_center = true;
              ui.request_system_map_center_system_id = r.system_id;
              ui.request_system_map_center_x_mkm = r.pos.x;
              ui.request_system_map_center_y_mkm = r.pos.y;
              ui.request_system_map_center_zoom = 0.0;
            }
          }

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(r.system.c_str());
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%.1f", r.total);
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%lld", static_cast<long long>(r.age_days));
          ImGui::TableSetColumnIndex(4);
          ImGui::TextUnformatted(r.source.c_str());
          ImGui::TableSetColumnIndex(5);
          std::string b = "Go##wreck_go_" + std::to_string((int)r.id);
          if (ImGui::SmallButton(b.c_str())) {
            if (r.system_id != kInvalidId) {
              s.selected_system = r.system_id;
              ui.request_map_tab = MapTab::System;
              ui.request_system_map_center = true;
              ui.request_system_map_center_system_id = r.system_id;
              ui.request_system_map_center_x_mkm = r.pos.x;
              ui.request_system_map_center_y_mkm = r.pos.y;
              ui.request_system_map_center_zoom = 0.0;
            }
          }
        }
      }

      ImGui::EndTable();
    }

    ImGui::EndTabItem();
  }

  ImGui::EndTabBar();
  ImGui::End();
}

} // namespace nebula4x::ui
