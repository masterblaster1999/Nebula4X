#include "ui/panels.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/procgen_obscure.h"
#include "nebula4x/core/procgen_surface.h"
#include "nebula4x/core/procgen_design_forge.h"
#include "nebula4x/core/research_planner.h"
#include "nebula4x/core/research_schedule.h"
#include "nebula4x/core/order_planner.h"
#include "nebula4x/core/colony_schedule.h"
#include "nebula4x/core/ground_battle_forecast.h"
#include "nebula4x/core/date.h"
#include "nebula4x/core/terraforming_schedule.h"
#include "nebula4x/util/autosave.h"
#include "nebula4x/util/event_export.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/sorted_keys.h"
#include "nebula4x/util/strings.h"
#include "nebula4x/util/time.h"

#include "ui/screen_reader.h"

#include "ui/notifications.h"
#include "ui/window_management.h"

#include "ui/hotkeys_window.h"

#include "ui/procedural_theme.h"
#include "ui/procedural_layout.h"
#include "ui/procgen_graphics.h"

namespace nebula4x::ui {
namespace {

using nebula4x::util::sorted_keys;

bool case_insensitive_contains_sv(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  const auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      });
  return it != haystack.end();
}

bool case_insensitive_contains(const std::string& haystack, const char* needle_cstr) {
  if (!needle_cstr) return true;
  return case_insensitive_contains_sv(std::string_view(haystack), std::string_view(needle_cstr));
}

bool case_insensitive_contains(const std::string& haystack, const std::string& needle) {
  return case_insensitive_contains_sv(std::string_view(haystack), std::string_view(needle));
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

std::string format_fixed(double v, int decimals) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss << std::setprecision(decimals) << v;
  return oss.str();
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
    case EventCategory::Terraforming: return "Terraforming";
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

const char* treaty_type_label(TreatyType t) {
  switch (t) {
    case TreatyType::Ceasefire: return "Ceasefire";
    case TreatyType::NonAggressionPact: return "Non-Aggression Pact";
    case TreatyType::Alliance: return "Alliance";
    case TreatyType::TradeAgreement: return "Trade Agreement";
    case TreatyType::ResearchAgreement: return "Research Agreement";
  }
  return "Treaty";
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
      if (ImGui::MenuItem("New Game...")) {
        ui.show_new_game_modal = true;
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

      ImGui::Separator();

      if (ImGui::BeginMenu("Autosave")) {
        ImGui::Checkbox("Enable autosave", &ui.autosave_game_enabled);

        ImGui::SliderInt("Interval (hours)", &ui.autosave_game_interval_hours, 1, 24 * 14);
        ui.autosave_game_interval_hours = std::clamp(ui.autosave_game_interval_hours, 1, 24 * 365);

        ImGui::SliderInt("Keep newest", &ui.autosave_game_keep_files, 1, 50);
        ui.autosave_game_keep_files = std::clamp(ui.autosave_game_keep_files, 1, 500);

        ImGui::TextDisabled("Directory");
        ImGui::InputText("##autosave_dir", ui.autosave_game_dir, 256);

        if (ImGui::MenuItem("Save autosave snapshot now")) {
          ui.request_autosave_game_now = true;
        }

        if (ImGui::MenuItem("Prune old autosaves")) {
          nebula4x::AutosaveConfig cfg;
          cfg.enabled = ui.autosave_game_enabled;
          cfg.interval_hours = ui.autosave_game_interval_hours;
          cfg.keep_files = ui.autosave_game_keep_files;
          cfg.dir = ui.autosave_game_dir;
          cfg.prefix = "autosave_";
          cfg.extension = ".json";

          std::string err;
          const int pruned = nebula4x::prune_autosaves(cfg, &err);
          if (pruned < 0) {
            nebula4x::log::warn(std::string("Autosave prune failed: ") + (err.empty() ? "(unknown)" : err));
          } else {
            nebula4x::log::info("Pruned " + std::to_string(pruned) + " autosave files.");
          }
        }

        if (ImGui::BeginMenu("Load autosave")) {
          nebula4x::AutosaveConfig cfg;
          cfg.enabled = ui.autosave_game_enabled;
          cfg.interval_hours = ui.autosave_game_interval_hours;
          cfg.keep_files = ui.autosave_game_keep_files;
          cfg.dir = ui.autosave_game_dir;
          cfg.prefix = "autosave_";
          cfg.extension = ".json";

          const auto scan = nebula4x::scan_autosaves(cfg, 24);
          if (!scan.ok) {
            ImGui::TextDisabled("(scan failed)");
          } else if (scan.files.empty()) {
            ImGui::TextDisabled("(none found)");
          } else {
            for (const auto& f : scan.files) {
              if (ImGui::MenuItem(f.filename.c_str())) {
                try {
                  sim.load_game(deserialize_game_from_json(read_text_file(f.path)));
                  nebula4x::log::info(std::string("Loaded autosave: ") + f.filename);
                } catch (const std::exception& e) {
                  nebula4x::log::error(std::string("Load autosave failed: ") + e.what());
                }
              }
            }
          }

          ImGui::EndMenu();
        }

        if (!ui.last_autosave_game_error.empty()) {
          ImGui::Separator();
          ImGui::TextDisabled("Last autosave error:");
          ImGui::TextWrapped("%s", ui.last_autosave_game_error.c_str());
        } else if (!ui.last_autosave_game_path.empty()) {
          ImGui::Separator();
          ImGui::TextDisabled("Last autosave:");
          ImGui::TextWrapped("%s", ui.last_autosave_game_path.c_str());
        }

        ImGui::EndMenu();
      }

      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("Controls", nullptr, &ui.show_controls_window);
      ImGui::MenuItem("Map", nullptr, &ui.show_map_window);
      ImGui::MenuItem("Details", nullptr, &ui.show_details_window);
      ImGui::MenuItem("Window Manager", "Ctrl+Shift+W", &ui.show_window_manager_window);
      if (ImGui::MenuItem("Focus Mode (Map only)", "F10", focus_mode_enabled(ui))) {
        toggle_focus_mode(ui);
      }
      ImGui::Separator();
      ImGui::MenuItem("Directory (Colonies/Bodies)", nullptr, &ui.show_directory_window);
      ImGui::MenuItem("Production (Shipyard/Construction Planner)", nullptr, &ui.show_production_window);
      ImGui::MenuItem("Economy (Industry/Mining/Tech Tree)", nullptr, &ui.show_economy_window);
      ImGui::MenuItem("Planner (Forecast Dashboard)", nullptr, &ui.show_planner_window);
      ImGui::MenuItem("Regions (Sectors Overview)", "Ctrl+Shift+R", &ui.show_regions_window);
      ImGui::MenuItem("Star Atlas (Constellations)", nullptr, &ui.show_star_atlas_window);
      ImGui::MenuItem("Freight Planner (Auto-freight Preview)", nullptr, &ui.show_freight_window);
      ImGui::MenuItem("Mine Planner (Auto-mine Preview)", nullptr, &ui.show_mine_window);
      ImGui::MenuItem("Fuel Planner (Auto-tanker Preview)", nullptr, &ui.show_fuel_window);
      ImGui::MenuItem("Salvage Planner (Wreck Salvage Runs)", nullptr, &ui.show_salvage_window);
      ImGui::MenuItem("Contracts (Mission Board)", nullptr, &ui.show_contracts_window);
      ImGui::MenuItem("Sustainment Planner (Fleet Base Targets)", nullptr, &ui.show_sustainment_window);
      ImGui::MenuItem("Troop Logistics (Auto-troop Preview)", nullptr, &ui.show_troop_window);
      ImGui::MenuItem("Population Logistics (Auto-colonist Preview)", nullptr, &ui.show_colonist_window);
      ImGui::MenuItem("Terraforming Planner", nullptr, &ui.show_terraforming_window);
      ImGui::MenuItem("Fleet Manager", "Ctrl+Shift+F", &ui.show_fleet_manager_window);
      ImGui::MenuItem("Advisor (Issues)", "Ctrl+Shift+A", &ui.show_advisor_window);
      ImGui::MenuItem("Colony Profiles (Automation Presets)", "Ctrl+Shift+B", &ui.show_colony_profiles_window);
      ImGui::MenuItem("Ship Profiles (Automation Presets)", "Ctrl+Shift+M", &ui.show_ship_profiles_window);
      ImGui::MenuItem("Automation Center (Ship Automation)", "Ctrl+Shift+O", &ui.show_automation_center_window);
      ImGui::MenuItem("Shipyard Targets (Design Targets)", "Ctrl+Shift+Y", &ui.show_shipyard_targets_window);
      ImGui::MenuItem("Survey Network (Jump Survey)", "Ctrl+Shift+J", &ui.show_survey_network_window);
      ImGui::MenuItem("Time Warp (Until Event)", nullptr, &ui.show_time_warp_window);
      ImGui::MenuItem("Timeline (Event Timeline)", nullptr, &ui.show_timeline_window);
      {
        const int inbox_unread = notifications_unread_count(ui);
        std::string label = "Notification Center";
        if (inbox_unread > 0) label += " (" + std::to_string(inbox_unread) + ")";
        ImGui::MenuItem(label.c_str(), "F3", &ui.show_notifications_window);
      }
      ImGui::MenuItem("Design Studio (Blueprints)", nullptr, &ui.show_design_studio_window);
      ImGui::MenuItem("Balance Lab (Duel Tournament)", nullptr, &ui.show_balance_lab_window);
      ImGui::MenuItem("Intel (Contacts/Sensors)", nullptr, &ui.show_intel_window);
      ImGui::MenuItem("Intel Notebook (Notes + Journal)", "Ctrl+Shift+I", &ui.show_intel_notebook_window);
      ImGui::MenuItem("Diplomacy Graph (Relations)", nullptr, &ui.show_diplomacy_window);
      ImGui::MenuItem("Victory & Score", nullptr, &ui.show_victory_window);
      ImGui::MenuItem("Settings Window", nullptr, &ui.show_settings_window);
      ImGui::MenuItem("Save Tools (Diff/Patch)", nullptr, &ui.show_save_tools_window);
      ImGui::MenuItem("Time Machine (State History)", "Ctrl+Shift+D", &ui.show_time_machine_window);
      ImGui::MenuItem("Compare / Diff (Entities)", "Ctrl+Shift+X", &ui.show_compare_window);
      ImGui::MenuItem("OmniSearch (JSON + Commands)", "Ctrl+F", &ui.show_omni_search_window);
      ImGui::MenuItem("Navigator (History + Bookmarks)", "Ctrl+Shift+N", &ui.show_navigator_window);
      ImGui::MenuItem("Entity Inspector (ID Resolver)", "Ctrl+G", &ui.show_entity_inspector_window);
      ImGui::MenuItem("Reference Graph (Entity IDs)", "Ctrl+Shift+G", &ui.show_reference_graph_window);
      ImGui::MenuItem("Watchboard (JSON Pins)", nullptr, &ui.show_watchboard_window);
      ImGui::MenuItem("Data Lenses (Procedural Tables)", nullptr, &ui.show_data_lenses_window);
      ImGui::MenuItem("Dashboards (Procedural Charts)", nullptr, &ui.show_dashboards_window);
      ImGui::MenuItem("Pivot Tables (Procedural Aggregations)", nullptr, &ui.show_pivot_tables_window);
      if (ImGui::BeginMenu("Custom Panels")) {
        ImGui::MenuItem("UI Forge (Editor)", "Ctrl+Shift+U", &ui.show_ui_forge_window);
        ImGui::MenuItem("Context Forge (Procedural)", "Ctrl+Shift+C", &ui.show_context_forge_window);
        ImGui::Separator();
        if (ui.ui_forge_panels.empty()) {
          ImGui::TextDisabled("(no custom panels yet)");
        } else {
          for (auto& p : ui.ui_forge_panels) {
            std::string label = p.name.empty() ? ("Panel " + std::to_string(p.id)) : p.name;
            ImGui::MenuItem(label.c_str(), nullptr, &p.open);
          }
        }
        ImGui::EndMenu();
      }

      if (ImGui::BeginMenu("Accessibility")) {
        ImGui::MenuItem("Enable Screen Reader (Narration)", "Ctrl+Alt+R", &ui.screen_reader_enabled);
        ImGui::Separator();
        ImGui::MenuItem("Speak Focused Controls", nullptr, &ui.screen_reader_speak_focus);
        ImGui::MenuItem("Speak Hovered Controls", nullptr, &ui.screen_reader_speak_hover);
        ImGui::MenuItem("Speak Window Focus", nullptr, &ui.screen_reader_speak_windows);
        ImGui::MenuItem("Speak Toasts", nullptr, &ui.screen_reader_speak_toasts);
        ImGui::MenuItem("Speak Selection Changes", nullptr, &ui.screen_reader_speak_selection);

        ImGui::Separator();
        if (ImGui::MenuItem("Repeat Last", "Ctrl+Alt+.")) {
          ScreenReader::instance().repeat_last();
        }
        ImGui::EndMenu();
      }

      ImGui::MenuItem("Status Bar", nullptr, &ui.show_status_bar);
      ImGui::MenuItem("Event Toasts", nullptr, &ui.show_event_toasts);
      ImGui::Separator();
      if (ImGui::MenuItem("Reset Window Layout")) {
        actions.reset_window_layout = true;
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
      if (ImGui::MenuItem("Command Console", "Ctrl+P")) ui.show_command_palette = true;
      if (ImGui::MenuItem("OmniSearch (JSON + Commands)", "Ctrl+F")) ui.show_omni_search_window = true;
      if (ImGui::MenuItem("Entity Inspector (ID Resolver)", "Ctrl+G")) ui.show_entity_inspector_window = true;
      if (ImGui::MenuItem("Reference Graph (Entity IDs)", "Ctrl+Shift+G")) ui.show_reference_graph_window = true;
      if (ImGui::MenuItem("Help / Shortcuts", "F1")) ui.show_help_window = true;

      ImGui::Separator();

      if (ImGui::MenuItem("Open Event Log")) {
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Log;
      }
      if (ImGui::MenuItem("Open Production Planner")) {
        ui.show_production_window = true;
      }
      if (ImGui::MenuItem("Open Planner")) {
        ui.show_planner_window = true;
      }
      if (ImGui::MenuItem("Open Regions (Sectors Overview)", "Ctrl+Shift+R")) {
        ui.show_regions_window = true;
      }
      if (ImGui::MenuItem("Open Freight Planner")) {
        ui.show_freight_window = true;
      }
      if (ImGui::MenuItem("Open Mine Planner")) {
        ui.show_mine_window = true;
      }
      if (ImGui::MenuItem("Open Fuel Planner")) {
        ui.show_fuel_window = true;
      }
      if (ImGui::MenuItem("Open Salvage Planner")) {
        ui.show_salvage_window = true;
      }
      if (ImGui::MenuItem("Open Sustainment Planner")) {
        ui.show_sustainment_window = true;
      }
      if (ImGui::MenuItem("Open Troop Logistics")) {
        ui.show_troop_window = true;
      }
      if (ImGui::MenuItem("Open Population Logistics")) {
        ui.show_colonist_window = true;
      }
      if (ImGui::MenuItem("Open Terraforming Planner")) {
        ui.show_terraforming_window = true;
      }
      if (ImGui::MenuItem("Open Colony Profiles")) {
        ui.show_colony_profiles_window = true;
      }
      if (ImGui::MenuItem("Open Ship Profiles")) {
        ui.show_ship_profiles_window = true;
      }
      if (ImGui::MenuItem("Open Time Warp")) {
        ui.show_time_warp_window = true;
      }
      if (ImGui::MenuItem("Open Design Studio")) {
        ui.show_design_studio_window = true;
      }
      if (ImGui::MenuItem("Open Balance Lab")) {
        ui.show_balance_lab_window = true;
      }
      if (ImGui::MenuItem("Open ProcGen Atlas")) {
        ui.show_procgen_atlas_window = true;
      }
      if (ImGui::MenuItem("Open Star Atlas (Constellations)")) {
        ui.show_star_atlas_window = true;
      }
      if (ImGui::MenuItem("Open Timeline")) {
        ui.show_timeline_window = true;
      }
      if (ImGui::MenuItem("Open Intel")) {
        ui.show_intel_window = true;
      }
      if (ImGui::MenuItem("Open Intel Notebook", "Ctrl+Shift+I")) {
        ui.show_intel_notebook_window = true;
      }
      if (ImGui::MenuItem("Open Diplomacy Graph")) {
        ui.show_diplomacy_window = true;
      }
      if (ImGui::MenuItem("Open Victory & Score")) {
        ui.show_victory_window = true;
      }
      if (ImGui::MenuItem("Open Save Tools (Diff/Patch)")) {
        ui.show_save_tools_window = true;
      }
      if (ImGui::MenuItem("Open Time Machine (State History)", "Ctrl+Shift+D")) {
        ui.show_time_machine_window = true;
      }
      if (ImGui::MenuItem("Open Compare / Diff (Entities)", "Ctrl+Shift+X")) {
        ui.show_compare_window = true;
      }
      if (ImGui::MenuItem("Open JSON Explorer")) {
        ui.show_json_explorer_window = true;
      }
      if (ImGui::MenuItem("Open Content Validation", "Ctrl+Shift+V")) {
        ui.show_content_validation_window = true;
      }
      if (ImGui::MenuItem("Open State Doctor", "Ctrl+Shift+K")) {
        ui.show_state_doctor_window = true;
      }
	      if (ImGui::MenuItem("Open Trace Viewer (Performance)")) {
	        ui.show_trace_viewer_window = true;
	      }
      if (ImGui::MenuItem("Open Watchboard (JSON Pins)")) {
        ui.show_watchboard_window = true;
      }
      if (ImGui::MenuItem("Open Data Lenses (Procedural Tables)")) {
        ui.show_data_lenses_window = true;
      }
      if (ImGui::MenuItem("Open Dashboards (Procedural Charts)")) {
        ui.show_dashboards_window = true;
      }
      if (ImGui::MenuItem("Open Pivot Tables (Procedural Aggregations)")) {
        ui.show_pivot_tables_window = true;
      }
      if (ImGui::MenuItem("Open UI Forge (Custom Panels)", "Ctrl+Shift+U")) {
        ui.show_ui_forge_window = true;
      }
      if (ImGui::MenuItem("Open Context Forge (Procedural)", "Ctrl+Shift+C")) {
        ui.show_context_forge_window = true;
      }
      if (ImGui::MenuItem("Layout Profiles", "Ctrl+Shift+L")) {
        ui.show_layout_profiles_window = true;
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

  {
    bool heat = sim.ship_heat_enabled();
    if (ImGui::Checkbox("Ship heat", &heat)) {
      sim.set_ship_heat_enabled(heat);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Experimental thermal model. Ships accumulate heat based on online power usage and cool over time.\n"
          "High heat reduces speed/sensors/weapons/shield regen and extreme overheating can damage hull.");
    }
  }

  {
    bool subsys = sim.ship_subsystem_damage_enabled();
    if (ImGui::Checkbox("Subsystem damage", &subsys)) {
      sim.set_ship_subsystem_damage_enabled(subsys);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Experimental combat critical hits. Hull damage can reduce engines/weapons/sensors/shields integrity.\n"
          "Integrity reduces performance until repaired at a shipyard.");
    }
  }

  {
    bool los = sim.sensor_los_attenuation_enabled();
    if (ImGui::Checkbox("Sensor LOS attenuation", &los)) {
      sim.set_sensor_los_attenuation_enabled(los);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Experimental sensor model: applies an extra line-of-sight occlusion factor by\n"
          "ray-marching the local nebula microfield / storm-cell environment between\n"
          "sensor source and target. Uses an SDF-style distance estimate (f/|grad f|)\n"
          "for adaptive step sizes and deterministic jitter to reduce sampling artifacts.");
    }

    if (los) {
      double strength = sim.sensor_los_strength();
      const double los_min = 0.0;
      const double los_max = 3.0;
      if (ImGui::SliderScalar("LOS strength", ImGuiDataType_Double, &strength, &los_min, &los_max, "%.2f")) {
        sim.set_sensor_los_strength(strength);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Higher values make dense pockets block sensors more aggressively.");
      }
    }
  }



  {
    bool beam = sim.beam_los_attenuation_enabled();
    if (ImGui::Checkbox("Beam LOS attenuation", &beam)) {
      sim.set_beam_los_attenuation_enabled(beam);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Experimental combat model: beam weapons apply a transmission multiplier based on the\n"
          "nebula microfield / storm-cell environment *along the line of fire*.\n"
          "Computed via adaptive ray-marching with an SDF-style distance estimate and deterministic jitter.");
    }

    if (beam) {
      double strength = sim.beam_los_strength();
      const double beam_los_min = 0.0;
      const double beam_los_max = 3.0;
      if (ImGui::SliderScalar("Beam LOS strength", ImGuiDataType_Double, &strength, &beam_los_min, &beam_los_max,
                              "%.2f")) {
        sim.set_beam_los_strength(strength);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("0 disables attenuation even when enabled. Higher values make dense pockets more punishing.");
      }

      bool scatter = sim.beam_scatter_splash_enabled();
      if (ImGui::Checkbox("Beam scatter splash", &scatter)) {
        sim.set_beam_scatter_splash_enabled(scatter);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Converts a fraction of medium-loss (1 - LOS multiplier) into low-intensity splash damage\n"
            "around the beam segment. Makes fighting inside heavy terrain more chaotic.");
      }

      if (scatter) {
        double frac = sim.beam_scatter_fraction_of_lost();
        const double scatter_frac_min = 0.0;
        const double scatter_frac_max = 0.75;
        if (ImGui::SliderScalar("Scatter fraction", ImGuiDataType_Double, &frac, &scatter_frac_min, &scatter_frac_max,
                                "%.2f")) {
          sim.set_beam_scatter_fraction_of_lost(frac);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Fraction of (1 - LOS) damage converted into splash (rest is absorbed/lost).");
        }

        double rad = sim.beam_scatter_radius_mkm();
        const double scatter_rad_min = 0.05;
        const double scatter_rad_max = 2.0;
        if (ImGui::SliderScalar("Scatter radius (mkm)", ImGuiDataType_Double, &rad, &scatter_rad_min, &scatter_rad_max,
                                "%.2f")) {
          sim.set_beam_scatter_radius_mkm(rad);
        }

        bool ff = sim.beam_scatter_can_hit_friendly();
        if (ImGui::Checkbox("Scatter friendly fire", &ff)) {
          sim.set_beam_scatter_can_hit_friendly(ff);
        }
      }
    }
  }

  {
    bool nav = sim.terrain_aware_navigation_enabled();
    if (ImGui::Checkbox("Terrain-aware navigation", &nav)) {
      sim.set_terrain_aware_navigation_enabled(nav);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Experimental movement steering: casts a small fan of candidate headings (rays) around\n"
          "the direct-to-target vector and chooses the lowest estimated travel-time path through\n"
          "nebula microfields / storm cells. Uses SDF-style adaptive ray steps (f/|grad f|) and\n"
          "deterministic jitter for stable stochastic sampling.");
    }

    if (nav) {
      ImGui::Indent();

      double strength = sim.terrain_nav_strength();
      const double nav_strength_min = 0.0;
      const double nav_strength_max = 1.0;
      if (ImGui::SliderScalar("Nav strength", ImGuiDataType_Double, &strength, &nav_strength_min, &nav_strength_max,
                              "%.2f")) {
        sim.set_terrain_nav_strength(strength);
      }

      double look = sim.terrain_nav_lookahead_mkm();
      const double nav_look_min = 25.0;
      const double nav_look_max = 6000.0;
      if (ImGui::SliderScalar("Lookahead (mkm)", ImGuiDataType_Double, &look, &nav_look_min, &nav_look_max, "%.0f")) {
        sim.set_terrain_nav_lookahead_mkm(look);
      }

      int rays = sim.terrain_nav_rays();
      if (ImGui::SliderInt("Candidate rays", &rays, 3, 21)) {
        sim.set_terrain_nav_rays(rays);
      }

      double ang = sim.terrain_nav_max_angle_deg();
      const double nav_ang_min = 5.0;
      const double nav_ang_max = 85.0;
      if (ImGui::SliderScalar("Max angle (deg)", ImGuiDataType_Double, &ang, &nav_ang_min, &nav_ang_max, "%.0f")) {
        sim.set_terrain_nav_max_angle_deg(ang);
      }

      double tp = sim.terrain_nav_turn_penalty();
      const double nav_tp_min = 0.0;
      const double nav_tp_max = 2.0;
      if (ImGui::SliderScalar("Turn penalty", ImGuiDataType_Double, &tp, &nav_tp_min, &nav_tp_max, "%.2f")) {
        sim.set_terrain_nav_turn_penalty(tp);
      }

      ImGui::TextDisabled(
          "Tip: enable Microfields + Storm cells overlays in the System map to see what the nav is avoiding.");

      ImGui::Unindent();
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

// Subsystem integrity (0..1).
{
  auto clamp01 = [](double x) {
    if (!std::isfinite(x)) return 1.0;
    return std::clamp(x, 0.0, 1.0);
  };
  const double ei = clamp01(sh->engines_integrity);
  const double wi = clamp01(sh->weapons_integrity);
  const double si = clamp01(sh->sensors_integrity);
  const double shi = clamp01(sh->shields_integrity);

  const bool any_damaged =
      (std::abs(ei - 1.0) > 1e-6) || (std::abs(wi - 1.0) > 1e-6) || (std::abs(si - 1.0) > 1e-6) ||
      (std::abs(shi - 1.0) > 1e-6);

  if (any_damaged || sim.ship_subsystem_damage_enabled()) {
    ImGui::Text(
        "Subsystems: Engines %.0f%% (x%.2f), Weapons %.0f%% (x%.2f), Sensors %.0f%% (x%.2f), Shields %.0f%% (x%.2f)",
        ei * 100.0, sim.ship_subsystem_engine_multiplier(*sh), wi * 100.0,
        sim.ship_subsystem_weapon_output_multiplier(*sh), si * 100.0, sim.ship_subsystem_sensor_range_multiplier(*sh),
        shi * 100.0, sim.ship_subsystem_shield_multiplier(*sh));

    if (!sim.ship_subsystem_damage_enabled()) {
      ImGui::TextDisabled(
          "(Combat critical hits disabled; integrity may still be affected by maintenance/repairs.)");
    }
  }
}

if (sim.cfg().enable_ship_maintenance) {

            const double m = std::clamp(sh->maintenance_condition, 0.0, 1.0);
            const double min_spd = std::clamp(sim.cfg().ship_maintenance_min_speed_multiplier, 0.0, 1.0);
            const double min_cbt = std::clamp(sim.cfg().ship_maintenance_min_combat_multiplier, 0.0, 1.0);
            const double spd_mult = min_spd + (1.0 - min_spd) * m;
            const double cbt_mult = min_cbt + (1.0 - min_cbt) * m;

            ImGui::Text("Maintenance: %.0f%%  (Speed x%.2f, Combat x%.2f)", 100.0 * m, spd_mult, cbt_mult);


// Deterministic subsystem malfunctions (optional maintenance extension).
{
  const double start = std::clamp(sim.cfg().ship_maintenance_breakdown_start_fraction, 0.0, 1.0);
  const double rate0 = std::max(0.0, sim.cfg().ship_maintenance_breakdown_rate_per_day_at_zero);
  if (rate0 > 1e-12 && start > 1e-9 && m + 1e-9 < start) {
    const double exponent = std::max(0.1, sim.cfg().ship_maintenance_breakdown_exponent);
    const double x = std::clamp((start - m) / start, 0.0, 1.0);
    const double rate = rate0 * std::pow(x, exponent);
    const double p = 1.0 - std::exp(-rate);
    ImGui::TextDisabled("Breakdown risk: ~%.2f%%/day (when not docked at shipyard)", 100.0 * p);
  }
}


            const std::string& res = sim.cfg().ship_maintenance_resource_id;
            if (!res.empty()) {
              const auto it = sh->cargo.find(res);
              const double in_cargo = (it == sh->cargo.end()) ? 0.0 : std::max(0.0, it->second);
              const double req_per_day =
                  std::max(0.0, d->mass_tons) * std::max(0.0, sim.cfg().ship_maintenance_tons_per_day_per_mass_ton);
              ImGui::TextDisabled("Spare parts (%s): %.1f t in cargo  (need %.2f t/day)", res.c_str(), in_cargo,
                                  req_per_day);
            }
          }
          // Crew training / experience
          {
            double gp = sh->crew_grade_points;
            if (!std::isfinite(gp) || gp < 0.0) gp = sim.cfg().crew_initial_grade_points;

            double comp = sh->crew_complement;
            if (!std::isfinite(comp) || comp < 0.0) comp = 1.0;
            comp = std::clamp(comp, 0.0, 1.0);

            const double base_bonus = sim.crew_grade_bonus_for_points(gp);
            const double eff_bonus = sim.crew_grade_bonus(*sh);

            const char* tier = (gp < 100.0)  ? "Green"
                               : (gp < 400.0)  ? "Regular"
                               : (gp < 900.0)  ? "Trained"
                               : (gp < 1600.0) ? "Veteran"
                               : "Elite";

            if (sim.cfg().enable_crew_casualties) {
              ImGui::Text("Crew: %.0f pts  (%s, %+0.1f%% base, %.0f%% complement, %+0.1f%% effective)", gp, tier,
                          base_bonus * 100.0, comp * 100.0, eff_bonus * 100.0);
            } else {
              ImGui::Text("Crew: %.0f pts  (%s, %+0.1f%% combat)", gp, tier, base_bonus * 100.0);
            }

            if (!sim.cfg().enable_crew_experience) {
              ImGui::TextDisabled("Crew experience disabled in SimConfig");
            }
            if (!sim.cfg().enable_crew_casualties) {
              ImGui::TextDisabled("Crew casualties disabled in SimConfig");
            }
          }

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

          // Missile ammo / munitions
          if (d->missile_launcher_count > 0) {
            if (d->missile_ammo_capacity > 0) {
              const int cap_ammo = std::max(0, d->missile_ammo_capacity);
              int cur_ammo = sh->missile_ammo;
              if (cur_ammo < 0) cur_ammo = cap_ammo;
              cur_ammo = std::clamp(cur_ammo, 0, cap_ammo);
              ImGui::Text("Missile ammo: %d / %d", cur_ammo, cap_ammo);

              if (auto it = sh->cargo.find("Munitions"); it != sh->cargo.end()) {
                const double cargo_mun = std::max(0.0, it->second);
                if (cargo_mun >= 1.0 - 1e-9) {
                  ImGui::TextDisabled("Munitions (cargo): %.0f", std::floor(cargo_mun + 1e-9));
                }
              }
            } else {
              ImGui::Text("Missile ammo: Unlimited");
            }
          }


          // Missile salvos in flight (incoming/outgoing)
          {
            struct MissileRow {
              bool incoming{false};
              Id mid{kInvalidId};
              double eta_days{0.0};
            };

            std::vector<MissileRow> rows;
            rows.reserve(s.missile_salvos.size());

            int incoming_count = 0;
            int outgoing_count = 0;
            double incoming_payload = 0.0;
            double outgoing_payload = 0.0;
            double incoming_earliest = 1e30;
            double outgoing_earliest = 1e30;

            for (const auto& [mid, ms] : s.missile_salvos) {
              if (ms.target_ship_id == sh->id) {
                ++incoming_count;
                incoming_payload += std::max(0.0, ms.damage);
                const double eta = std::max(0.0, ms.eta_days_remaining);
                incoming_earliest = std::min(incoming_earliest, eta);
                rows.push_back(MissileRow{true, mid, eta});
              }
              if (ms.attacker_ship_id == sh->id && ms.target_ship_id != sh->id) {
                ++outgoing_count;
                outgoing_payload += std::max(0.0, ms.damage);
                const double eta = std::max(0.0, ms.eta_days_remaining);
                outgoing_earliest = std::min(outgoing_earliest, eta);
                rows.push_back(MissileRow{false, mid, eta});
              }
            }

            if (incoming_count > 0 || outgoing_count > 0) {
              ImGui::Spacing();
              if (ImGui::CollapsingHeader("Missile salvos in flight", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (incoming_count > 0) {
                  ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.35f, 1.0f),
                                    "Incoming: %d  (payload %.1f, earliest ETA %s)", incoming_count, incoming_payload,
                                    format_duration_days(incoming_earliest).c_str());
                } else {
                  ImGui::TextDisabled("Incoming: none");
                }

                if (outgoing_count > 0) {
                  ImGui::TextDisabled("Outgoing: %d  (payload %.1f, earliest ETA %s)", outgoing_count, outgoing_payload,
                                      format_duration_days(outgoing_earliest).c_str());
                } else {
                  ImGui::TextDisabled("Outgoing: none");
                }

                std::sort(rows.begin(), rows.end(), [](const MissileRow& a, const MissileRow& b) {
                  if (a.eta_days < b.eta_days) return true;
                  if (a.eta_days > b.eta_days) return false;
                  if (a.incoming != b.incoming) return a.incoming; // incoming first when equal ETA
                  return a.mid < b.mid;
                });

                const ImGuiTableFlags tbl_flags =
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;

                if (ImGui::BeginTable("missile_salvos_table", 6, tbl_flags)) {
                  ImGui::TableSetupColumn("Dir");
                  ImGui::TableSetupColumn("Other ship");
                  ImGui::TableSetupColumn("ETA");
                  ImGui::TableSetupColumn("Payload");
                  ImGui::TableSetupColumn("Range");
                  ImGui::TableSetupColumn("Actions");
                  ImGui::TableHeadersRow();

                  for (const MissileRow& row : rows) {
                    const MissileSalvo* ms = find_ptr(s.missile_salvos, row.mid);
                    if (!ms) continue;

                    const Id other_id = row.incoming ? ms->attacker_ship_id : ms->target_ship_id;
                    const Ship* other = find_ptr(s.ships, other_id);

                    std::string other_label = other ? other->name : ("Ship #" + std::to_string(other_id));
                    if (other && other->name.empty()) other_label = "Ship #" + std::to_string(other_id);

                    const double payload0 = (ms->damage_initial > 1e-12) ? ms->damage_initial : std::max(0.0, ms->damage);
                    const std::string payload_s =
                        format_fixed(std::max(0.0, ms->damage), 1) + "/" + format_fixed(payload0, 1);

                    std::string range_s;
                    if (!std::isfinite(ms->range_remaining_mkm) || ms->range_remaining_mkm > 1e18) {
                      range_s = "inf";
                    } else {
                      range_s = format_fixed(std::max(0.0, ms->range_remaining_mkm), 1);
                    }

                    ImGui::PushID(("ms_" + std::to_string(static_cast<unsigned long long>(row.mid))).c_str());

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.incoming ? "IN" : "OUT");

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(other_label.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(format_duration_days(row.eta_days).c_str());

                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(payload_s.c_str());

                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(range_s.c_str());

                    ImGui::TableSetColumnIndex(5);
                    if (other && ImGui::SmallButton("Select")) {
                      selected_ship = other->id;
                      sim.state().selected_system = other->system_id;
                    }
                    if (other) {
                      ImGui::SameLine();
                      if (ImGui::SmallButton("Center")) {
                        sim.state().selected_system = other->system_id;
                        ui.request_map_tab = MapTab::System;

                        ui.request_system_map_center = true;
                        ui.request_system_map_center_system_id = other->system_id;
                        ui.request_system_map_center_x_mkm = other->position_mkm.x;
                        ui.request_system_map_center_y_mkm = other->position_mkm.y;
                      }
                    }

                    ImGui::PopID();
                  }

                  ImGui::EndTable();
                }
              }
            }
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
              range_eff *= sim.ship_heat_sensor_range_multiplier(*sh);
              range_eff *= sim.ship_subsystem_sensor_range_multiplier(*sh);
              if (!std::isfinite(range_eff) || range_eff < 0.0) range_eff = 0.0;
            }

            const double sig_base =
                std::clamp(std::isfinite(d->signature_multiplier) ? d->signature_multiplier : 1.0, 0.0, 1.0);

            // Effective signature includes design stealth, EMCON, and (optionally) thermal bloom from heat.
            const SensorMode sig_mode = sh->power_policy.sensors_enabled ? sh->sensor_mode : SensorMode::Passive;
            double sig_emcon_mult = 1.0;
            if (sig_mode == SensorMode::Passive) sig_emcon_mult = sim.cfg().sensor_mode_passive_signature_multiplier;
            else if (sig_mode == SensorMode::Active) sig_emcon_mult = sim.cfg().sensor_mode_active_signature_multiplier;
            if (!std::isfinite(sig_emcon_mult) || sig_emcon_mult < 0.0) sig_emcon_mult = 0.0;

            const double sig_heat_mult = sim.ship_heat_signature_multiplier(*sh);
            const double sig_eff = sim.ship_effective_signature_multiplier(*sh, d);

            ImGui::Text("Sensor: %.0f mkm (effective %.0f mkm)", d->sensor_range_mkm, range_eff);
            ImGui::Text("Signature: %.0f%% (effective %.0f%%)", sig_base * 100.0, sig_eff * 100.0);

            if (sim.ship_heat_enabled() && sim.cfg().ship_heat_signature_multiplier_per_fraction > 0.0) {
              ImGui::TextDisabled("Signature factors: EMCON x%.2f  Thermal x%.2f (heat %.0f%%)",
                                  sig_emcon_mult, sig_heat_mult, sim.ship_heat_fraction(*sh) * 100.0);
            } else {
              ImGui::TextDisabled("Signature factors: EMCON x%.2f", sig_emcon_mult);
            }

            if (sim.ship_heat_enabled()) {
              const double cap =
                  std::max(0.0, sim.cfg().ship_heat_base_capacity_per_mass_ton) * std::max(0.0, d->mass_tons) +
                  std::max(0.0, d->heat_capacity_bonus);
              if (cap > 1e-9) {
                ImGui::TextDisabled("Heat: %.0f / %.0f (%.0f%%)", sh->heat, cap, sim.ship_heat_fraction(*sh) * 100.0);
              } else {
                ImGui::TextDisabled("Heat: (no capacity)");
              }
            }
            if (d->ecm_strength > 0.0 || d->eccm_strength > 0.0) {
              ImGui::Text("EW: ECM %.1f  ECCM %.1f", d->ecm_strength, d->eccm_strength);
            } else {
              ImGui::TextDisabled("EW: (none)");
            }

            if (!sh->power_policy.sensors_enabled) {
              ImGui::TextDisabled("Note: Sensors disabled by power policy -> signature treated as Passive.");
            } else if (!p.sensors_online) {
              ImGui::TextDisabled("Note: Sensors offline due to power availability / load shedding.");
            }
          } else {
            ImGui::Text("Sensor: 0 mkm");
            const double sig_base =
                std::clamp(std::isfinite(d->signature_multiplier) ? d->signature_multiplier : 1.0, 0.0, 1.0);
            const double sig_heat_mult = sim.ship_heat_signature_multiplier(*sh);
            const double sig_eff = sim.ship_effective_signature_multiplier(*sh, d);
            ImGui::Text("Signature: %.0f%% (effective %.0f%%)", sig_base * 100.0, sig_eff * 100.0);

            if (sim.ship_heat_enabled() && sim.cfg().ship_heat_signature_multiplier_per_fraction > 0.0) {
              ImGui::TextDisabled("Thermal bloom: x%.2f (heat %.0f%%)", sig_heat_mult, sim.ship_heat_fraction(*sh) * 100.0);
            }

            if (sim.ship_heat_enabled()) {
              const double cap =
                  std::max(0.0, sim.cfg().ship_heat_base_capacity_per_mass_ton) * std::max(0.0, d->mass_tons) +
                  std::max(0.0, d->heat_capacity_bonus);
              if (cap > 1e-9) {
                ImGui::TextDisabled("Heat: %.0f / %.0f (%.0f%%)", sh->heat, cap, sim.ship_heat_fraction(*sh) * 100.0);
              } else {
                ImGui::TextDisabled("Heat: (no capacity)");
              }
            }

            ImGui::TextDisabled("Sensor mode: (no sensors)");
            if (d->ecm_strength > 0.0 || d->eccm_strength > 0.0) {
              ImGui::Text("EW: ECM %.1f  ECCM %.1f", d->ecm_strength, d->eccm_strength);
            } else {
              ImGui::TextDisabled("EW: (none)");
            }
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

          // --- Combat doctrine (AttackShip positioning) ---
          {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("Combat doctrine");
            {
              const char* fire_labels[] = {"Weapons free", "Orders only", "Hold fire"};
              int fire_i = static_cast<int>(sh->combat_doctrine.fire_control);
              if (ImGui::Combo("Fire control##fire_control", &fire_i, fire_labels, IM_ARRAYSIZE(fire_labels))) {
                sh->combat_doctrine.fire_control = static_cast<FireControlMode>(fire_i);
              }
              if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Weapons free: auto-engage detected hostiles in range.\n"
                    "Orders only: only fire when executing explicit combat orders (AttackShip/BombardColony).\n"
                    "Hold fire: never fire offensive weapons (point defense still works).");
              }

              const char* prio_labels[] = {"Nearest", "Weakest", "Threat", "Largest"};
              int prio_i = static_cast<int>(sh->combat_doctrine.targeting_priority);
              if (ImGui::Combo("Target priority##target_priority", &prio_i, prio_labels, IM_ARRAYSIZE(prio_labels))) {
                sh->combat_doctrine.targeting_priority = static_cast<TargetingPriority>(prio_i);
              }
              if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Automatic target selection (Weapons free mode only).\n"
                    "Nearest: closest detected hostile.\n"
                    "Weakest: lowest HP (ties by distance).\n"
                    "Threat: highest combat threat (ties by distance).\n"
                    "Largest: highest mass (ties by distance).");
              }
            }

            const double beam_range = std::max(0.0, d->weapon_range_mkm);
            const double missile_range = std::max(0.0, d->missile_range_mkm);

            int mode_i = static_cast<int>(sh->combat_doctrine.range_mode);
            const char* mode_labels[] = {"Auto", "Beam", "Missile", "Max", "Min", "Custom"};
            if (ImGui::Combo("Range mode##eng_range_mode", &mode_i, mode_labels, IM_ARRAYSIZE(mode_labels))) {
              mode_i = std::clamp(mode_i, 0, 5);
              sh->combat_doctrine.range_mode = static_cast<EngagementRangeMode>(mode_i);
            }

            if (sh->combat_doctrine.range_mode == EngagementRangeMode::Custom) {
              double cr = sh->combat_doctrine.custom_range_mkm;
              if (ImGui::InputDouble("Custom range (mkm)##eng_custom", &cr, 1.0, 10.0, "%.1f")) {
                sh->combat_doctrine.custom_range_mkm = std::max(0.0, cr);
              }
            }

            float frac = static_cast<float>(sh->combat_doctrine.range_fraction);
            if (ImGui::SliderFloat("Range fraction##eng_frac", &frac, 0.05f, 1.0f, "%.2f")) {
              sh->combat_doctrine.range_fraction = std::clamp(static_cast<double>(frac), 0.0, 1.0);
            }

            double min_r = sh->combat_doctrine.min_range_mkm;
            if (ImGui::InputDouble("Min range (mkm)##eng_min", &min_r, 0.05, 0.5, "%.2f")) {
              sh->combat_doctrine.min_range_mkm = std::max(0.0, min_r);
            }

            ImGui::Checkbox("Kite if too close##eng_kite", &sh->combat_doctrine.kite_if_too_close);
            if (sh->combat_doctrine.kite_if_too_close) {
              float db = static_cast<float>(sh->combat_doctrine.kite_deadband_fraction);
              if (ImGui::SliderFloat("Kite deadband##eng_db", &db, 0.0f, 0.50f, "%.2f")) {
                sh->combat_doctrine.kite_deadband_fraction = std::clamp(static_cast<double>(db), 0.0, 0.90);
              }
            }

            // Preview the effective standoff range (ignoring boarding adjustments).
            auto select_range = [&](EngagementRangeMode mode) -> double {
              switch (mode) {
                case EngagementRangeMode::Beam: return beam_range;
                case EngagementRangeMode::Missile: return missile_range;
                case EngagementRangeMode::Max: return std::max(beam_range, missile_range);
                case EngagementRangeMode::Min: {
                  double r = 0.0;
                  if (beam_range > 1e-9) r = beam_range;
                  if (missile_range > 1e-9) r = (r > 1e-9) ? std::min(r, missile_range) : missile_range;
                  return r;
                }
                case EngagementRangeMode::Custom: return std::max(0.0, sh->combat_doctrine.custom_range_mkm);
                case EngagementRangeMode::Auto:
                default:
                  return (beam_range > 1e-9) ? beam_range : ((missile_range > 1e-9) ? missile_range : 0.0);
              }
            };

            const double base = select_range(sh->combat_doctrine.range_mode);
            const double frac_cl = std::clamp(sh->combat_doctrine.range_fraction, 0.0, 1.0);
            const double min_cl = std::max(0.0, sh->combat_doctrine.min_range_mkm);
            double desired = base * frac_cl;
            if (base <= 1e-9) desired = min_cl;
            desired = std::max(desired, min_cl);
            if (!std::isfinite(desired)) desired = min_cl;

            ImGui::Text("Beam range: %.1f mkm | Missile range: %.1f mkm", beam_range, missile_range);
            ImGui::Text("Desired standoff: %.1f mkm", desired);

            // --- Disengagement / auto-retreat ---
            {
              ImGui::Spacing();
              ImGui::Separator();
              ImGui::TextDisabled("Disengagement");

              ImGui::Checkbox("Auto-retreat##ret_auto", &sh->combat_doctrine.auto_retreat);
              if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "When enabled, the ship will automatically suspend its current order queue and execute an\n"
                    "emergency retreat plan when it is damaged (or runs out of missiles), then resume orders once\n"
                    "it is safe and sufficiently repaired.");
              }

              if (sh->combat_doctrine.auto_retreat) {
                float trig = static_cast<float>(sh->combat_doctrine.retreat_hp_trigger_fraction);
                if (ImGui::SliderFloat("Retreat HP fraction##ret_hp_trig", &trig, 0.05f, 0.95f, "%.2f")) {
                  sh->combat_doctrine.retreat_hp_trigger_fraction =
                      std::clamp(static_cast<double>(trig), 0.0, 1.0);
                  sh->combat_doctrine.retreat_hp_resume_fraction = std::max(
                      sh->combat_doctrine.retreat_hp_resume_fraction, sh->combat_doctrine.retreat_hp_trigger_fraction);
                }
                float res = static_cast<float>(sh->combat_doctrine.retreat_hp_resume_fraction);
                if (ImGui::SliderFloat("Resume HP fraction##ret_hp_res", &res, 0.05f, 1.0f, "%.2f")) {
                  sh->combat_doctrine.retreat_hp_resume_fraction = std::max(
                      sh->combat_doctrine.retreat_hp_trigger_fraction,
                      std::clamp(static_cast<double>(res), 0.0, 1.0));
                }

                ImGui::Checkbox("Retreat when low on missiles##ret_mis_en", &sh->combat_doctrine.retreat_when_out_of_missiles);
                if (sh->combat_doctrine.retreat_when_out_of_missiles) {
                  float ammo = static_cast<float>(sh->combat_doctrine.retreat_missile_ammo_trigger_fraction);
                  if (ImGui::SliderFloat("Missile ammo fraction##ret_mis_frac", &ammo, 0.0f, 1.0f, "%.2f")) {
                    sh->combat_doctrine.retreat_missile_ammo_trigger_fraction =
                        std::clamp(static_cast<double>(ammo), 0.0, 1.0);
                  }
                }

                // Live status indicator.
                if (const auto* so_view = find_ptr(s.ship_orders, sh->id)) {
                  if (so_view->suspended) {
                    ImGui::TextDisabled("Status: emergency retreat (orders suspended)");
                  }
                }
              }
            }

            if (ImGui::SmallButton("Reset##eng_reset")) {
              sh->combat_doctrine = ShipCombatDoctrine{};
            }

            const Id fleet_id = sim.fleet_for_ship(sh->id);
            if (fleet_id != kInvalidId) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Apply to Fleet##eng_apply_fleet")) {
                if (auto* fl = find_ptr(s.fleets, fleet_id)) {
                  for (Id sid : fl->ship_ids) {
                    if (auto* other = find_ptr(s.ships, sid)) other->combat_doctrine = sh->combat_doctrine;
                  }
                }
              }
            }
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
            sh->auto_mine = false;
            sh->auto_colonize = false;
            sh->auto_tanker = false;
            sh->auto_troop_transport = false;
            sh->auto_colonist_transport = false;
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
            sh->auto_mine = false;
            sh->auto_colonize = false;
            sh->auto_tanker = false;
            sh->auto_troop_transport = false;
            sh->auto_colonist_transport = false;
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

        const bool can_auto_troop = (d && d->troop_capacity > 0.0);
        if (!can_auto_troop) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-troop transport when idle", &sh->auto_troop_transport)) {
          if (sh->auto_troop_transport) {
            // Mutually exclusive with mission-style automation (explore/freight/salvage/mine/colonize/tanker).
            sh->auto_explore = false;
            sh->auto_freight = false;
            sh->auto_salvage = false;
            sh->auto_mine = false;
            sh->auto_colonize = false;
            sh->auto_tanker = false;
            sh->auto_colonist_transport = false;
          }
        }
        if (ImGui::IsItemHovered()) {
          const auto& cfg = sim.cfg();
          ImGui::SetTooltip(
              "When enabled, this troop transport will automatically move ground troops between your colonies\n"
              "to satisfy garrison targets and reinforce defensive ground battles (when idle).\n\n"
              "Min transfer: %.1f strength\n"
              "Max take fraction: %.0f%%",
              cfg.auto_troop_min_transfer_strength,
              cfg.auto_troop_max_take_fraction_of_surplus * 100.0);
        }
        if (!can_auto_troop) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(requires troop transport capacity)");
        }


        const bool can_auto_colonist_transport = (d && d->colony_capacity_millions > 0.0);
        if (!can_auto_colonist_transport) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-colonist transport when idle", &sh->auto_colonist_transport)) {
          if (sh->auto_colonist_transport) {
            // Mutually exclusive with mission-style automation (explore/freight/salvage/mine/colonize/tanker/troop).
            sh->auto_explore = false;
            sh->auto_freight = false;
            sh->auto_salvage = false;
            sh->auto_mine = false;
            sh->auto_colonize = false;
            sh->auto_tanker = false;
            sh->auto_troop_transport = false;
          }
        }
        if (ImGui::IsItemHovered()) {
          const auto& cfg = sim.cfg();
          ImGui::SetTooltip(
              "When enabled, this ship will automatically ferry colonists between your colonies to satisfy\n"
              "population targets (import) and population reserves (export floors) when idle.\n\n"
              "Configure this per colony:\n"
              "- Population target (M)\n"
              "- Population reserve (M)\n\n"
              "Min transfer: %.1f M\n"
              "Max take fraction: %.0f%%",
              cfg.auto_colonist_min_transfer_millions,
              cfg.auto_colonist_max_take_fraction_of_surplus * 100.0);
        }
        if (!can_auto_colonist_transport) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(requires colonist capacity)");
        }

        const bool can_auto_salvage = (d && d->cargo_tons > 0.0);
        if (!can_auto_salvage) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-salvage wrecks when idle", &sh->auto_salvage)) {
          if (sh->auto_salvage) {
            sh->auto_explore = false;
            sh->auto_freight = false;
            sh->auto_mine = false;
            sh->auto_colonize = false;
            sh->auto_tanker = false;
            sh->auto_troop_transport = false;
            sh->auto_colonist_transport = false;
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

        const bool can_auto_mine = (d && d->cargo_tons > 0.0 && d->mining_tons_per_day > 0.0);
        if (!can_auto_mine) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-mine deposits when idle", &sh->auto_mine)) {
          if (sh->auto_mine) {
            sh->auto_explore = false;
            sh->auto_freight = false;
            sh->auto_salvage = false;
            sh->auto_colonize = false;
            sh->auto_tanker = false;
            sh->auto_troop_transport = false;
            sh->auto_colonist_transport = false;
          }
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "When enabled, this ship will automatically mine the best known deposit in your discovered map\n"
              "whenever it is idle, then deliver its cargo to a friendly colony when full.");
        }

        if (can_auto_mine && sh->auto_mine) {
          // Home colony selector.
          std::string home_label = "Nearest";
          if (sh->auto_mine_home_colony_id != kInvalidId) {
            if (const Colony* hc = find_ptr(s.colonies, sh->auto_mine_home_colony_id)) {
              home_label = hc->name;
            }
          }
          if (ImGui::BeginCombo("Mining home colony", home_label.c_str())) {
            const bool nearest_selected = (sh->auto_mine_home_colony_id == kInvalidId);
            if (ImGui::Selectable("Nearest", nearest_selected)) {
              sh->auto_mine_home_colony_id = kInvalidId;
            }
            if (nearest_selected) ImGui::SetItemDefaultFocus();

            for (Id cid : sorted_keys(s.colonies)) {
              const Colony* c = find_ptr(s.colonies, cid);
              if (!c) continue;
              if (c->faction_id != sh->faction_id) continue;
              const bool selected = (sh->auto_mine_home_colony_id == cid);
              std::string label = c->name + "##minehome_" + std::to_string(static_cast<std::uint64_t>(cid));
              if (ImGui::Selectable(label.c_str(), selected)) {
                sh->auto_mine_home_colony_id = cid;
              }
              if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
          }

          // Mineral filter selector.
          const auto& res = sim.content().resources;
          std::vector<std::string> mineable;
          if (!res.empty()) {
            mineable.reserve(res.size());
            for (const auto& [id, def] : res) {
              if (!def.mineable) continue;
              if (id == "Fuel") continue;
              mineable.push_back(id);
            }
            std::sort(mineable.begin(), mineable.end());
          } else {
            mineable = {"Boronide", "Corbomite", "Corundium", "Duranium", "Gallicite", "Mercassium",
                        "Neutronium", "Sorium", "Tritanium", "Uridium", "Vendarite"};
          }

          auto mineral_label = [&]() -> std::string {
            if (sh->auto_mine_mineral.empty()) return "(All minerals)";
            if (!res.empty()) {
              if (auto it = res.find(sh->auto_mine_mineral); it != res.end() && !it->second.name.empty()) {
                return it->second.name;
              }
            }
            return sh->auto_mine_mineral;
          }();

          if (ImGui::BeginCombo("Mining target mineral", mineral_label.c_str())) {
            const bool all_selected = sh->auto_mine_mineral.empty();
            if (ImGui::Selectable("(All minerals)", all_selected)) {
              sh->auto_mine_mineral.clear();
            }
            if (all_selected) ImGui::SetItemDefaultFocus();

            for (const auto& id : mineable) {
              const bool selected = (sh->auto_mine_mineral == id);
              std::string label = id;
              if (!res.empty()) {
                if (auto it = res.find(id); it != res.end() && !it->second.name.empty()) label = it->second.name;
              }
              label += "##mine_mineral_" + id;
              if (ImGui::Selectable(label.c_str(), selected)) {
                sh->auto_mine_mineral = id;
              }
              if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
          }
        }

        if (!can_auto_mine) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(requires mining gear + cargo hold)");
        }

        const bool can_auto_colonize = (d && d->colony_capacity_millions > 0.0);
        if (!can_auto_colonize) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-colonize when idle", &sh->auto_colonize)) {
          if (sh->auto_colonize) {
            sh->auto_explore = false;
            sh->auto_freight = false;
            sh->auto_salvage = false;
            sh->auto_mine = false;
            sh->auto_tanker = false;
            sh->auto_troop_transport = false;
            sh->auto_colonist_transport = false;
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

        const bool can_auto_rearm = (d && d->missile_ammo_capacity > 0);
        if (!can_auto_rearm) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-rearm missiles when low ammo (idle)", &sh->auto_rearm)) {
          // Safety automation (can coexist with explore/freight/etc)
          sh->auto_rearm_threshold_fraction =
              std::clamp(sh->auto_rearm_threshold_fraction, 0.0, 1.0);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "When enabled, if this ship is idle and its missile ammo drops below the configured threshold,\n"
              "it will automatically route to the nearest friendly colony with Munitions to rearm.\n\n"
              "Tip: You can also carry Munitions in cargo for forward reloading.");
        }

        if (can_auto_rearm && sh->auto_rearm) {
          float thresh_pct = static_cast<float>(sh->auto_rearm_threshold_fraction * 100.0);
          if (ImGui::SliderFloat("Rearm threshold", &thresh_pct, 0.0f, 100.0f, "%.0f%%")) {
            sh->auto_rearm_threshold_fraction =
                std::clamp(static_cast<double>(thresh_pct) / 100.0, 0.0, 1.0);
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Auto-rearm triggers when missile ammo is below this fraction of magazine capacity.\n"
                "Example: 25%% = rearm when below 25%%.");
          }
        }

        if (!can_auto_rearm) {
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("(requires finite missile ammo)");
        }

        const bool can_auto_tanker = (d && d->fuel_capacity_tons > 0.0);
        if (!can_auto_tanker) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Auto-tanker: refuel other ships when idle", &sh->auto_tanker)) {
          if (sh->auto_tanker) {
            // Mutually exclusive with mission-style automation (explore/freight/salvage/colonize).
            sh->auto_explore = false;
            sh->auto_freight = false;
            sh->auto_salvage = false;
            sh->auto_mine = false;
            sh->auto_colonize = false;
            sh->auto_troop_transport = false;
            sh->auto_colonist_transport = false;
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

const auto plan = nebula4x::compute_order_plan(sim, selected_ship);
const auto* plan_ship = find_ptr(s.ships, selected_ship);
const auto* plan_design = plan_ship ? sim.find_design(plan_ship->design_id) : nullptr;
const double plan_fuel_cap = plan_design ? std::max(0.0, plan_design->fuel_capacity_tons) : 0.0;

if (plan.ok) {
  if (plan.steps.empty()) {
    ImGui::TextDisabled("Plan: (no queued orders)");
  } else if (plan_fuel_cap > 1e-9) {
    ImGui::TextDisabled("Plan: +%.2f d | Fuel: %.0f -> %.0f / %.0f",
                        plan.total_eta_days, plan.start_fuel_tons, plan.end_fuel_tons, plan_fuel_cap);
  } else {
    ImGui::TextDisabled("Plan: +%.2f d", plan.total_eta_days);
  }
  if (plan.truncated) {
    ImGui::SameLine();
    ImGui::TextDisabled("(truncated: %s)", plan.truncated_reason.c_str());
  }
}

          ImGui::TextDisabled("Drag+drop to reorder. Tip: if repeat is ON, edits do not update the repeat template unless you sync it.");

          const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
          if (ImGui::BeginTable("ship_orders_table", 6, flags)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 24.0f);
            ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ETA (d)", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn("Fuel (t)", ImGuiTableColumnFlags_WidthFixed, 86.0f);
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
if (i < static_cast<int>(plan.steps.size())) {
  const auto& stp = plan.steps[static_cast<std::size_t>(i)];
  ImGui::Text("%.2f", stp.eta_days);
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::Text("Δ %.2f d", stp.delta_days);
    ImGui::Text("Fuel: %.0f -> %.0f t", stp.fuel_before_tons, stp.fuel_after_tons);
    if (!stp.note.empty()) {
      ImGui::Separator();
      ImGui::TextUnformatted(stp.note.c_str());
    }
    ImGui::EndTooltip();
  }
} else {
  ImGui::TextDisabled("--");
}

ImGui::TableSetColumnIndex(3);
if (i < static_cast<int>(plan.steps.size())) {
  const auto& stp = plan.steps[static_cast<std::size_t>(i)];
  if (plan_fuel_cap > 1e-9) {
    ImGui::Text("%.0f/%.0f", stp.fuel_after_tons, plan_fuel_cap);
  } else {
    ImGui::Text("%.0f", stp.fuel_after_tons);
  }
} else {
  ImGui::TextDisabled("--");
}

ImGui::TableSetColumnIndex(4);
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

              ImGui::TableSetColumnIndex(5);
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
          const bool trade_partner = sim.are_factions_trade_partners(sh->faction_id, sel_col->faction_id);
          const bool own_colony = (sel_col->faction_id == sh->faction_id);
          const auto treaties_here = sim.treaties_between(sh->faction_id, sel_col->faction_id);
          const bool hostile_actions_blocked_by_treaty = !treaties_here.empty();
          if (!trade_partner) {
            ImGui::Spacing();
            ImGui::TextDisabled("This colony is not friendly.");
            if (hostile_actions_blocked_by_treaty) {
              ImGui::TextDisabled("Hostile actions are blocked by an active treaty.");
            }
            ImGui::Text("Defenders: %.1f", sel_col->ground_forces);

            // --- Invasion advisor (deterministic forecast) ---
            {
              const double forts_here = sim.fortification_points(*sel_col);
              if (forts_here > 1e-9) ImGui::Text("Fortifications: %.1f", forts_here);

              // Defender ground fire support (installation weapons).
              double defender_arty_weapon = 0.0;
              {
                for (const auto& [inst_id, count] : sel_col->installations) {
                  if (count <= 0) continue;
                  const auto it = sim.content().installations.find(inst_id);
                  if (it == sim.content().installations.end()) continue;
                  const double wd = it->second.weapon_damage;
                  if (wd <= 0.0) continue;
                  defender_arty_weapon += wd * static_cast<double>(count);
                }
                defender_arty_weapon = std::max(0.0, defender_arty_weapon);
              }
              if (defender_arty_weapon > 1e-9) {
                ImGui::Text("Artillery: %.1f dmg/day", defender_arty_weapon);
              }

              // Square-law baseline; add a small margin so players don't sit right on the knife-edge.
              const double req = nebula4x::square_law_required_attacker_strength(
                  sim.cfg(), sel_col->ground_forces, forts_here, defender_arty_weapon, 1.05);

              ImGui::Text("Advisor: ~%.1f troops to win", req);
              if (sh->troops > 1e-9) {
                const double shortfall = std::max(0.0, req - sh->troops);
                if (shortfall > 1e-9) {
                  ImGui::TextDisabled("Shortfall: %.1f", shortfall);
                }

                const auto fc_now = nebula4x::forecast_ground_battle(sim.cfg(), sh->troops, sel_col->ground_forces,
                                                                      forts_here, defender_arty_weapon);
                if (fc_now.ok && !fc_now.truncated) {
                  const char* winner = (fc_now.winner == nebula4x::GroundBattleWinner::Attacker) ? "attacker" : "defender";
                  ImGui::TextDisabled("If invade now: %s wins in ~%d d", winner, fc_now.days_to_resolve);
                }
              }
            }

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

              const bool has_weapons = (w_dmg > 1e-9 && w_range > 1e-9);
              const bool can_bombard = has_weapons && !hostile_actions_blocked_by_treaty;
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
                  if (hostile_actions_blocked_by_treaty) {
                    ImGui::SetTooltip("Hostile actions are blocked by an active treaty.");
                  } else if (!has_weapons) {
                    ImGui::SetTooltip("Ship has no weapons.");
                  }
                }
              }
            }

            const auto* d2 = sim.find_design(sh->design_id);
            const double cap2 = d2 ? d2->troop_capacity : 0.0;
            ImGui::Text("Embarked troops: %.1f / %.1f", sh->troops, cap2);
            if (hostile_actions_blocked_by_treaty) {
              ImGui::BeginDisabled();
              ImGui::Button("Invade (blocked by treaty)");
              ImGui::EndDisabled();
              if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Hostile actions are blocked by an active treaty.");
              }
            } else if (sh->troops <= 1e-9 || cap2 <= 1e-9) {
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
            if (!friendly && !own_colony) {
              ImGui::Spacing();
              ImGui::TextDisabled("Trade partner colony: logistics access via Trade Agreement.");
            }
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
          const auto* d2_col = sim.find_design(sh->design_id);
          const double cap_col = d2_col ? d2_col->colony_capacity_millions : 0.0;
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
            label += " (" + format_fixed(total, 1) + " t)";
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
            ImGui::SameLine();
            if (ImGui::Button("Salvage & Deliver")) {
              if (!sim.issue_salvage_wreck_loop(selected_ship, salvage_wreck_id, /*dropoff_colony_id=*/kInvalidId,
                                                ui.fog_of_war)) {
                nebula4x::log::warn("Couldn't queue salvage loop order.");
              }
            }
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip("Repeatedly salvages the wreck and unloads at the nearest friendly colony until depleted.");
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



        // --- Mission automation ---
        ImGui::Separator();
        ImGui::Text("Mission");
        ImGui::TextDisabled("Automation for fleets: defend, patrol, hunt, escort, or explore. Disable to regain full manual control.");

        Fleet* fleet_mut = find_ptr(s.fleets, selected_fleet->id);
        if (fleet_mut) {
          static const char* kMissionNames[] = {
            "None",
            "Defend colony",
            "Patrol system",
            "Hunt hostiles",
            "Escort freighters",
            "Explore systems",
            "Patrol region",
            "Assault colony",
            "Blockade colony",
            "Patrol route",
            "Guard jump point",
            "Patrol circuit (waypoints)",
          };
          static_assert(IM_ARRAYSIZE(kMissionNames) == 1 + static_cast<int>(FleetMissionType::PatrolCircuit),
                        "Update mission names array");
          int mission_idx = static_cast<int>(fleet_mut->mission.type);
          if (mission_idx < 0 || mission_idx >= static_cast<int>(IM_ARRAYSIZE(kMissionNames))) mission_idx = 0;
          if (ImGui::Combo("Type##fleet_mission_type", &mission_idx, kMissionNames, IM_ARRAYSIZE(kMissionNames))) {
            fleet_mut->mission.type = static_cast<FleetMissionType>(mission_idx);
            fleet_mut->mission.sustainment_mode = FleetSustainmentMode::None;
            fleet_mut->mission.sustainment_colony_id = kInvalidId;
            fleet_mut->mission.last_target_ship_id = kInvalidId;
            fleet_mut->mission.escort_active_ship_id = kInvalidId;
            fleet_mut->mission.escort_last_retarget_day = 0;

            // Best-effort defaults.
            if (fleet_mut->mission.type == FleetMissionType::DefendColony && fleet_mut->mission.defend_colony_id == kInvalidId) {
              for (Id cid : sorted_keys(s.colonies)) {
                const auto* c = find_ptr(s.colonies, cid);
                if (!c) continue;
                if (c->faction_id != fleet_mut->faction_id) continue;
                fleet_mut->mission.defend_colony_id = cid;
                break;
              }
            }
            if (fleet_mut->mission.type == FleetMissionType::PatrolSystem && fleet_mut->mission.patrol_system_id == kInvalidId) {
              if (s.selected_system != kInvalidId) fleet_mut->mission.patrol_system_id = s.selected_system;
            }
            if (fleet_mut->mission.type == FleetMissionType::PatrolCircuit && fleet_mut->mission.patrol_circuit_system_ids.empty()) {
              Id seed = kInvalidId;
              if (s.selected_system != kInvalidId && (!ui.fog_of_war || sim.is_system_discovered_by_faction(fleet_mut->faction_id, s.selected_system))) {
                seed = s.selected_system;
              } else if (leader) {
                seed = leader->system_id;
              }
              if (seed != kInvalidId) {
                fleet_mut->mission.patrol_circuit_system_ids.push_back(seed);
              }
              fleet_mut->mission.patrol_leg_index = 0;
            }
            if (fleet_mut->mission.type == FleetMissionType::GuardJumpPoint && fleet_mut->mission.guard_jump_point_id == kInvalidId) {
              Id sys_id = kInvalidId;
              if (s.selected_system != kInvalidId && (!ui.fog_of_war || sim.is_system_discovered_by_faction(fleet_mut->faction_id, s.selected_system))) {
                sys_id = s.selected_system;
              } else if (leader) {
                sys_id = leader->system_id;
              }

              const auto* sys = (sys_id != kInvalidId) ? find_ptr(s.systems, sys_id) : nullptr;
              if (sys && !sys->jump_points.empty()) {
                auto jps = sys->jump_points;
                std::sort(jps.begin(), jps.end());
                fleet_mut->mission.guard_jump_point_id = jps.front();
              }

              if (fleet_mut->mission.guard_jump_radius_mkm <= 0.0) fleet_mut->mission.guard_jump_radius_mkm = 50.0;
              if (fleet_mut->mission.guard_jump_dwell_days <= 0) fleet_mut->mission.guard_jump_dwell_days = 3;
              fleet_mut->mission.guard_last_alert_day = 0;
            }
            if (fleet_mut->mission.type == FleetMissionType::PatrolRoute) {
              ImGui::Spacing();
              ImGui::Text("Patrol route");

              // Endpoint A (discovered systems only).
              const StarSystem* sys_a = (fleet_mut->mission.patrol_route_a_system_id != kInvalidId)
                                          ? find_ptr(s.systems, fleet_mut->mission.patrol_route_a_system_id)
                                          : nullptr;
              const char* a_label = sys_a ? sys_a->name.c_str() : "(select system)";
              if (ImGui::BeginCombo("Endpoint A##fleet_mission_patrol_route_a", a_label)) {
                for (Id sid : sorted_keys(s.systems)) {
                  const auto* sys = find_ptr(s.systems, sid);
                  if (!sys) continue;
                  if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, sid)) continue;
                  const bool sel = (fleet_mut->mission.patrol_route_a_system_id == sid);
                  if (ImGui::Selectable((sys->name + "##pat_route_a_" + std::to_string(static_cast<unsigned long long>(sid))).c_str(), sel)) {
                    fleet_mut->mission.patrol_route_a_system_id = sid;

                    // Keep endpoints distinct when possible.
                    if (fleet_mut->mission.patrol_route_b_system_id == sid) {
                      for (Id other : sorted_keys(s.systems)) {
                        if (other == sid) continue;
                        if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, other)) continue;
                        fleet_mut->mission.patrol_route_b_system_id = other;
                        break;
                      }
                    }

                    // Restart the route direction so the next leg is deterministic.
                    fleet_mut->mission.patrol_leg_index = 0;
                  }
                }
                ImGui::EndCombo();
              }

              // Endpoint B.
              const StarSystem* sys_b = (fleet_mut->mission.patrol_route_b_system_id != kInvalidId)
                                          ? find_ptr(s.systems, fleet_mut->mission.patrol_route_b_system_id)
                                          : nullptr;
              const char* b_label = sys_b ? sys_b->name.c_str() : "(select system)";
              if (ImGui::BeginCombo("Endpoint B##fleet_mission_patrol_route_b", b_label)) {
                for (Id sid : sorted_keys(s.systems)) {
                  const auto* sys = find_ptr(s.systems, sid);
                  if (!sys) continue;
                  if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, sid)) continue;
                  const bool sel = (fleet_mut->mission.patrol_route_b_system_id == sid);
                  if (ImGui::Selectable((sys->name + "##pat_route_b_" + std::to_string(static_cast<unsigned long long>(sid))).c_str(), sel)) {
                    fleet_mut->mission.patrol_route_b_system_id = sid;

                    // Keep endpoints distinct when possible.
                    if (fleet_mut->mission.patrol_route_a_system_id == sid) {
                      for (Id other : sorted_keys(s.systems)) {
                        if (other == sid) continue;
                        if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, other)) continue;
                        fleet_mut->mission.patrol_route_a_system_id = other;
                        break;
                      }
                    }

                    fleet_mut->mission.patrol_leg_index = 0;
                  }
                }
                ImGui::EndCombo();
              }

              if (ImGui::SmallButton("Swap endpoints##fleet_mission_patrol_route_swap")) {
                std::swap(fleet_mut->mission.patrol_route_a_system_id, fleet_mut->mission.patrol_route_b_system_id);
                fleet_mut->mission.patrol_leg_index = 0;
              }

              int dwell = std::max(1, fleet_mut->mission.patrol_dwell_days);
              if (ImGui::InputInt("Dwell days##fleet_mission_patrol_route_dwell", &dwell)) {
                fleet_mut->mission.patrol_dwell_days = std::max(1, dwell);
              }

              ImGui::TextDisabled(
                  "Shuttles between two systems and engages detected hostiles along the way.\n"
                  "Useful for guarding trade lanes and suppressing piracy across multiple regions.");
            }

            if (fleet_mut->mission.type == FleetMissionType::GuardJumpPoint) {
              ImGui::Spacing();
              ImGui::Text("Guard jump point");

              const JumpPoint* seljp = (fleet_mut->mission.guard_jump_point_id != kInvalidId)
                                         ? find_ptr(s.jump_points, fleet_mut->mission.guard_jump_point_id)
                                         : nullptr;
              std::string jp_label = seljp ? seljp->name : std::string("(select jump point)");
              if (seljp) {
                if (const auto* sys = find_ptr(s.systems, seljp->system_id)) {
                  jp_label = sys->name + ": " + seljp->name;
                }
              }

              if (ImGui::BeginCombo("Jump point##fleet_mission_guard_jp", jp_label.c_str())) {
                // Show jump points in discovered systems (for this faction).
                for (Id sys_id : sorted_keys(s.systems)) {
                  const auto* sys = find_ptr(s.systems, sys_id);
                  if (!sys) continue;
                  if (ui.fog_of_war && !sim.is_system_discovered_by_faction(fleet_mut->faction_id, sys_id)) continue;

                  for (Id jid : sys->jump_points) {
                    const auto* jp = find_ptr(s.jump_points, jid);
                    if (!jp) continue;
                    if (jp->system_id != sys_id) continue;

                    std::string label = sys->name + ": " + jp->name;

                    // Try to show destination if surveyed / no fog.
                    const bool surveyed = (!ui.fog_of_war) || sim.is_jump_point_surveyed_by_faction(fleet_mut->faction_id, jid);
                    if (surveyed && jp->linked_jump_id != kInvalidId) {
                      if (const auto* other = find_ptr(s.jump_points, jp->linked_jump_id)) {
                        if (const auto* dst = find_ptr(s.systems, other->system_id)) {
                          label += " -> " + dst->name;
                        }
                      }
                    } else {
                      label += " -> ???";
                    }

                    const bool sel = (fleet_mut->mission.guard_jump_point_id == jid);
                    const std::string key = label + "##guard_jp_" + std::to_string(static_cast<unsigned long long>(jid));
                    if (ImGui::Selectable(key.c_str(), sel)) {
                      fleet_mut->mission.guard_jump_point_id = jid;
                      fleet_mut->mission.guard_last_alert_day = 0;

                      // Handy navigation hint: jump to system when selecting.
                      s.selected_system = sys_id;
                    }
                  }
                }

                ImGui::EndCombo();
              }

              double rr = fleet_mut->mission.guard_jump_radius_mkm;
              if (ImGui::InputDouble("Response radius mkm##fleet_mission_guard_r", &rr, 10.0, 100.0, "%.1f")) {
                fleet_mut->mission.guard_jump_radius_mkm = std::max(0.0, rr);
              }

              int dwell = std::max(1, fleet_mut->mission.guard_jump_dwell_days);
              if (ImGui::InputInt("Loiter days##fleet_mission_guard_dwell", &dwell)) {
                fleet_mut->mission.guard_jump_dwell_days = std::max(1, dwell);
              }

              ImGui::TextDisabled(
                  "Moves to the selected jump point and waits.\n"
                  "When a detected hostile enters the response radius, the fleet will intercept.\n"
                  "Tip: Guarding jump points also contributes to pirate suppression in that region.");
            }


            if (fleet_mut->mission.type == FleetMissionType::PatrolRegion) {
              fleet_mut->mission.patrol_region_system_index = 0;
              fleet_mut->mission.patrol_region_waypoint_index = 0;

              // Best-effort default: region of the currently selected system (or the fleet leader).
              if (fleet_mut->mission.patrol_region_id == kInvalidId) {
                Id rid = kInvalidId;
                if (s.selected_system != kInvalidId) {
                  if (const auto* sys = find_ptr(s.systems, s.selected_system)) rid = sys->region_id;
                }
                if (rid == kInvalidId) {
                  if (const auto* lead = find_ptr(s.ships, selected_fleet->leader_ship_id)) {
                    if (const auto* sys = find_ptr(s.systems, lead->system_id)) rid = sys->region_id;
                  }
                }
                if (rid == kInvalidId) {
                  for (Id sid : sorted_keys(s.systems)) {
                    const auto* sys = find_ptr(s.systems, sid);
                    if (!sys) continue;
                    if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, sid)) continue;
                    rid = sys->region_id;
                    if (rid != kInvalidId) break;
                  }
                }
                fleet_mut->mission.patrol_region_id = rid;
              }
            }

            if (fleet_mut->mission.type == FleetMissionType::Explore) {
              fleet_mut->mission.explore_survey_first = true;
              fleet_mut->mission.explore_allow_transit = true;
            }

            if (fleet_mut->mission.type == FleetMissionType::AssaultColony) {
              fleet_mut->mission.assault_bombard_executed = false;
              if (fleet_mut->mission.assault_colony_id == kInvalidId && selected_colony != kInvalidId) {
                fleet_mut->mission.assault_colony_id = selected_colony;
              }
            } else {
              fleet_mut->mission.assault_colony_id = kInvalidId;
              fleet_mut->mission.assault_staging_colony_id = kInvalidId;
              fleet_mut->mission.assault_bombard_executed = false;
            }


            if (fleet_mut->mission.type == FleetMissionType::BlockadeColony) {
              if (fleet_mut->mission.blockade_colony_id == kInvalidId) {
                // Best-effort default: use the currently selected colony if any.
                // Otherwise pick the first discovered non-owned colony.
                if (selected_colony != kInvalidId) {
                  fleet_mut->mission.blockade_colony_id = selected_colony;
                } else {
                  for (Id cid : sorted_keys(s.colonies)) {
                    const auto* c = find_ptr(s.colonies, cid);
                    if (!c) continue;
                    if (c->faction_id == fleet_mut->faction_id) continue;
                    const auto* body = find_ptr(s.bodies, c->body_id);
                    if (!body || body->system_id == kInvalidId) continue;
                    if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, body->system_id)) continue;
                    fleet_mut->mission.blockade_colony_id = cid;
                    break;
                  }
                }
              }
              fleet_mut->mission.blockade_radius_mkm = std::max(0.0, fleet_mut->mission.blockade_radius_mkm);
            }
          }

          if (fleet_mut->mission.type != FleetMissionType::None) {
            if (ImGui::SmallButton("Start mission (clear orders)##fleet_mission_start")) {
              sim.clear_fleet_orders(selected_fleet->id);
              fleet_status = "Mission started.";
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop mission##fleet_mission_stop")) {
              fleet_mut->mission = FleetMission{};
              fleet_status = "Mission stopped.";
            }

            // Mission-specific config.
            if (fleet_mut->mission.type == FleetMissionType::DefendColony) {
              ImGui::Spacing();
              ImGui::Text("Defend colony");

              // Colony picker (owned colonies only).
              const Colony* selc = (fleet_mut->mission.defend_colony_id != kInvalidId)
                                     ? find_ptr(s.colonies, fleet_mut->mission.defend_colony_id)
                                     : nullptr;
              const char* col_label = selc ? selc->name.c_str() : "(select colony)";
              if (ImGui::BeginCombo("Colony##fleet_mission_defend_colony", col_label)) {
                for (Id cid : sorted_keys(s.colonies)) {
                  const auto* c = find_ptr(s.colonies, cid);
                  if (!c) continue;
                  if (c->faction_id != fleet_mut->faction_id) continue;
                  const bool sel = (fleet_mut->mission.defend_colony_id == cid);
                  if (ImGui::Selectable((c->name + "##def_col_" + std::to_string(static_cast<unsigned long long>(cid))).c_str(), sel)) {
                    fleet_mut->mission.defend_colony_id = cid;
                  }
                }
                ImGui::EndCombo();
              }

              double r = fleet_mut->mission.defend_radius_mkm;
              if (ImGui::InputDouble("Response radius mkm##fleet_mission_defend_r", &r, 10.0, 100.0, "%.1f")) {
                fleet_mut->mission.defend_radius_mkm = std::max(0.0, r);
              }
              ImGui::TextDisabled("0 = whole system (may chase far targets).\nHigher = stay closer to the defended colony.");
            }

            if (fleet_mut->mission.type == FleetMissionType::PatrolSystem) {
              ImGui::Spacing();
              ImGui::Text("Patrol system");

              // System picker (discovered systems only).
              const StarSystem* selsys = (fleet_mut->mission.patrol_system_id != kInvalidId)
                                           ? find_ptr(s.systems, fleet_mut->mission.patrol_system_id)
                                           : nullptr;
              const char* sys_label = selsys ? selsys->name.c_str() : "(select system)";
              if (ImGui::BeginCombo("System##fleet_mission_patrol_sys", sys_label)) {
                for (Id sid : sorted_keys(s.systems)) {
                  const auto* sys = find_ptr(s.systems, sid);
                  if (!sys) continue;
                  if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, sid)) continue;
                  const bool sel = (fleet_mut->mission.patrol_system_id == sid);
                  if (ImGui::Selectable((sys->name + "##pat_sys_" + std::to_string(static_cast<unsigned long long>(sid))).c_str(), sel)) {
                    fleet_mut->mission.patrol_system_id = sid;
                  }
                }
                ImGui::EndCombo();
              }

              int dwell = std::max(1, fleet_mut->mission.patrol_dwell_days);
              if (ImGui::InputInt("Dwell days##fleet_mission_patrol_dwell", &dwell)) {
                fleet_mut->mission.patrol_dwell_days = std::max(1, dwell);
              }
              ImGui::TextDisabled("Patrol waypoints: jump points first, then major bodies.");
            }

            if (fleet_mut->mission.type == FleetMissionType::PatrolCircuit) {
              ImGui::Spacing();
              ImGui::Text("Patrol circuit");

              auto add_wp = [&](Id sid) {
                if (sid == kInvalidId) return;
                if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, sid)) return;
                auto& wps = fleet_mut->mission.patrol_circuit_system_ids;
                // Keep the circuit simple by de-duplicating while preserving order (move to end).
                wps.erase(std::remove(wps.begin(), wps.end(), sid), wps.end());
                wps.push_back(sid);
                fleet_mut->mission.patrol_leg_index = 0;
              };

              // Quick add from current UI selection.
              ImGui::BeginDisabled(s.selected_system == kInvalidId ||
                                  !sim.is_system_discovered_by_faction(fleet_mut->faction_id, s.selected_system));
              if (ImGui::SmallButton("Add selected system##fleet_mission_circuit_add_selected")) {
                add_wp(s.selected_system);
              }
              ImGui::EndDisabled();

              ImGui::SameLine();
              if (ImGui::SmallButton("Clear##fleet_mission_circuit_clear")) {
                fleet_mut->mission.patrol_circuit_system_ids.clear();
                fleet_mut->mission.patrol_leg_index = 0;
              }

              // Add from a combo list (discovered systems only).
              const char* add_label = "(choose system)";
              if (ImGui::BeginCombo("Add waypoint##fleet_mission_circuit_add", add_label)) {
                for (Id sid : sorted_keys(s.systems)) {
                  const auto* sys = find_ptr(s.systems, sid);
                  if (!sys) continue;
                  if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, sid)) continue;

                  const std::string key = sys->name + "##circuit_add_" + std::to_string(static_cast<unsigned long long>(sid));
                  if (ImGui::Selectable(key.c_str(), false)) {
                    add_wp(sid);
                  }
                }
                ImGui::EndCombo();
              }

              auto& wps = fleet_mut->mission.patrol_circuit_system_ids;
              if (wps.empty()) {
                ImGui::TextDisabled("No waypoints. Add at least one system.");
              } else {
                const int n = static_cast<int>(wps.size());
                const int cur = (n > 0) ? (std::max(0, fleet_mut->mission.patrol_leg_index) % n) : 0;

                if (ImGui::BeginTable("fleet_mission_patrol_circuit_wps", 3,
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                  ImGui::TableSetupColumn("Waypoint", ImGuiTableColumnFlags_WidthStretch);
                  ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                  ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 56.0f);
                  ImGui::TableHeadersRow();

                  int remove_idx = -1;
                  int move_from = -1;
                  int move_to = -1;

                  for (int i = 0; i < n; ++i) {
                    const Id sid = wps[i];
                    const auto* sys = find_ptr(s.systems, sid);
                    const std::string name = sys ? sys->name : ("(unknown #" + std::to_string(static_cast<unsigned long long>(sid)) + ")");

                    ImGui::TableNextRow();

                    // Waypoint label.
                    ImGui::TableNextColumn();
                    const bool is_cur = (i == cur);
                    const std::string row_key = std::to_string(i + 1) + ". " + name + "##circuit_wp_" +
                                                std::to_string(static_cast<unsigned long long>(sid));
                    if (ImGui::Selectable(row_key.c_str(), is_cur, ImGuiSelectableFlags_SpanAllColumns)) {
                      s.selected_system = sid;
                    }

                    // Order controls.
                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(i == 0);
                    if (ImGui::SmallButton(("Up##circuit_up_" + std::to_string(i)).c_str())) {
                      move_from = i;
                      move_to = i - 1;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(i == n - 1);
                    if (ImGui::SmallButton(("Down##circuit_dn_" + std::to_string(i)).c_str())) {
                      move_from = i;
                      move_to = i + 1;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::SmallButton(("Remove##circuit_rm_" + std::to_string(i)).c_str())) {
                      remove_idx = i;
                    }

                    // Start index control.
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton(("Here##circuit_here_" + std::to_string(i)).c_str())) {
                      fleet_mut->mission.patrol_leg_index = i;
                    }
                  }

                  if (move_from >= 0 && move_to >= 0 && move_from != move_to) {
                    std::swap(wps[move_from], wps[move_to]);
                    // Keep the start index stable relative to the list.
                    if (fleet_mut->mission.patrol_leg_index == move_from) fleet_mut->mission.patrol_leg_index = move_to;
                    if (fleet_mut->mission.patrol_leg_index == move_to) fleet_mut->mission.patrol_leg_index = move_from;
                  }
                  if (remove_idx >= 0 && remove_idx < static_cast<int>(wps.size())) {
                    wps.erase(wps.begin() + remove_idx);
                    fleet_mut->mission.patrol_leg_index = 0;
                  }

                  ImGui::EndTable();
                }
              }

              int dwell = std::max(1, fleet_mut->mission.patrol_dwell_days);
              if (ImGui::InputInt("Dwell days##fleet_mission_circuit_dwell", &dwell)) {
                fleet_mut->mission.patrol_dwell_days = std::max(1, dwell);
              }

              ImGui::TextDisabled(
                  "Visits the waypoint systems in order (loop) and engages detected hostiles in-system.\n"
                  "Tip: On the Galaxy Map, Ctrl+Alt+Right click a system to add it to the selected fleet's circuit.");
            }

            if (fleet_mut->mission.type == FleetMissionType::PatrolRoute) {
              ImGui::Spacing();
              ImGui::Text("Patrol route");

              // Endpoint A (discovered systems only).
              const StarSystem* sys_a = (fleet_mut->mission.patrol_route_a_system_id != kInvalidId)
                                          ? find_ptr(s.systems, fleet_mut->mission.patrol_route_a_system_id)
                                          : nullptr;
              const char* a_label = sys_a ? sys_a->name.c_str() : "(select system)";
              if (ImGui::BeginCombo("Endpoint A##fleet_mission_patrol_route_a", a_label)) {
                for (Id sid : sorted_keys(s.systems)) {
                  const auto* sys = find_ptr(s.systems, sid);
                  if (!sys) continue;
                  if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, sid)) continue;
                  const bool sel = (fleet_mut->mission.patrol_route_a_system_id == sid);
                  if (ImGui::Selectable((sys->name + "##pat_route_a_" + std::to_string(static_cast<unsigned long long>(sid))).c_str(), sel)) {
                    fleet_mut->mission.patrol_route_a_system_id = sid;

                    // Keep endpoints distinct when possible.
                    if (fleet_mut->mission.patrol_route_b_system_id == sid) {
                      for (Id other : sorted_keys(s.systems)) {
                        if (other == sid) continue;
                        if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, other)) continue;
                        fleet_mut->mission.patrol_route_b_system_id = other;
                        break;
                      }
                    }

                    // Restart the route direction so the next leg is deterministic.
                    fleet_mut->mission.patrol_leg_index = 0;
                  }
                }
                ImGui::EndCombo();
              }

              // Endpoint B.
              const StarSystem* sys_b = (fleet_mut->mission.patrol_route_b_system_id != kInvalidId)
                                          ? find_ptr(s.systems, fleet_mut->mission.patrol_route_b_system_id)
                                          : nullptr;
              const char* b_label = sys_b ? sys_b->name.c_str() : "(select system)";
              if (ImGui::BeginCombo("Endpoint B##fleet_mission_patrol_route_b", b_label)) {
                for (Id sid : sorted_keys(s.systems)) {
                  const auto* sys = find_ptr(s.systems, sid);
                  if (!sys) continue;
                  if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, sid)) continue;
                  const bool sel = (fleet_mut->mission.patrol_route_b_system_id == sid);
                  if (ImGui::Selectable((sys->name + "##pat_route_b_" + std::to_string(static_cast<unsigned long long>(sid))).c_str(), sel)) {
                    fleet_mut->mission.patrol_route_b_system_id = sid;

                    // Keep endpoints distinct when possible.
                    if (fleet_mut->mission.patrol_route_a_system_id == sid) {
                      for (Id other : sorted_keys(s.systems)) {
                        if (other == sid) continue;
                        if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, other)) continue;
                        fleet_mut->mission.patrol_route_a_system_id = other;
                        break;
                      }
                    }

                    fleet_mut->mission.patrol_leg_index = 0;
                  }
                }
                ImGui::EndCombo();
              }

              if (ImGui::SmallButton("Swap endpoints##fleet_mission_patrol_route_swap")) {
                std::swap(fleet_mut->mission.patrol_route_a_system_id, fleet_mut->mission.patrol_route_b_system_id);
                fleet_mut->mission.patrol_leg_index = 0;
              }

              int dwell = std::max(1, fleet_mut->mission.patrol_dwell_days);
              if (ImGui::InputInt("Dwell days##fleet_mission_patrol_route_dwell", &dwell)) {
                fleet_mut->mission.patrol_dwell_days = std::max(1, dwell);
              }

              ImGui::TextDisabled(
                  "Shuttles between two systems and engages detected hostiles along the way.\n"
                  "Useful for guarding trade lanes and suppressing piracy across multiple regions.");
            }

            if (fleet_mut->mission.type == FleetMissionType::GuardJumpPoint) {
              ImGui::Spacing();
              ImGui::Text("Guard jump point");

              const JumpPoint* seljp = (fleet_mut->mission.guard_jump_point_id != kInvalidId)
                                         ? find_ptr(s.jump_points, fleet_mut->mission.guard_jump_point_id)
                                         : nullptr;
              std::string jp_label = seljp ? seljp->name : std::string("(select jump point)");
              if (seljp) {
                if (const auto* sys = find_ptr(s.systems, seljp->system_id)) {
                  jp_label = sys->name + ": " + seljp->name;
                }
              }

              if (ImGui::BeginCombo("Jump point##fleet_mission_guard_jp", jp_label.c_str())) {
                // Show jump points in discovered systems (for this faction).
                for (Id sys_id : sorted_keys(s.systems)) {
                  const auto* sys = find_ptr(s.systems, sys_id);
                  if (!sys) continue;
                  if (ui.fog_of_war && !sim.is_system_discovered_by_faction(fleet_mut->faction_id, sys_id)) continue;

                  for (Id jid : sys->jump_points) {
                    const auto* jp = find_ptr(s.jump_points, jid);
                    if (!jp) continue;
                    if (jp->system_id != sys_id) continue;

                    std::string label = sys->name + ": " + jp->name;

                    // Try to show destination if surveyed / no fog.
                    const bool surveyed = (!ui.fog_of_war) || sim.is_jump_point_surveyed_by_faction(fleet_mut->faction_id, jid);
                    if (surveyed && jp->linked_jump_id != kInvalidId) {
                      if (const auto* other = find_ptr(s.jump_points, jp->linked_jump_id)) {
                        if (const auto* dst = find_ptr(s.systems, other->system_id)) {
                          label += " -> " + dst->name;
                        }
                      }
                    } else {
                      label += " -> ???";
                    }

                    const bool sel = (fleet_mut->mission.guard_jump_point_id == jid);
                    const std::string key = label + "##guard_jp_" + std::to_string(static_cast<unsigned long long>(jid));
                    if (ImGui::Selectable(key.c_str(), sel)) {
                      fleet_mut->mission.guard_jump_point_id = jid;
                      fleet_mut->mission.guard_last_alert_day = 0;

                      // Handy navigation hint: jump to system when selecting.
                      s.selected_system = sys_id;
                    }
                  }
                }

                ImGui::EndCombo();
              }

              double rr = fleet_mut->mission.guard_jump_radius_mkm;
              if (ImGui::InputDouble("Response radius mkm##fleet_mission_guard_r", &rr, 10.0, 100.0, "%.1f")) {
                fleet_mut->mission.guard_jump_radius_mkm = std::max(0.0, rr);
              }

              int dwell = std::max(1, fleet_mut->mission.guard_jump_dwell_days);
              if (ImGui::InputInt("Loiter days##fleet_mission_guard_dwell", &dwell)) {
                fleet_mut->mission.guard_jump_dwell_days = std::max(1, dwell);
              }

              ImGui::TextDisabled(
                  "Moves to the selected jump point and waits.\n"
                  "When a detected hostile enters the response radius, the fleet will intercept.\n"
                  "Tip: Guarding jump points also contributes to pirate suppression in that region.");
            }


            if (fleet_mut->mission.type == FleetMissionType::PatrolRegion) {
              ImGui::Spacing();
              ImGui::Text("Patrol region");

              const Region* selreg = (fleet_mut->mission.patrol_region_id != kInvalidId)
                                       ? find_ptr(s.regions, fleet_mut->mission.patrol_region_id)
                                       : nullptr;
              const char* reg_label = selreg ? selreg->name.c_str() : "(select region)";
              if (ImGui::BeginCombo("Region##fleet_mission_patrol_region", reg_label)) {
                for (Id rid : sorted_keys(s.regions)) {
                  const auto* reg = find_ptr(s.regions, rid);
                  if (!reg) continue;

                  // Only show regions with at least one discovered system for this faction.
                  bool has_visible_system = false;
                  for (Id sid : sorted_keys(s.systems)) {
                    const auto* sys = find_ptr(s.systems, sid);
                    if (!sys) continue;
                    if (sys->region_id != rid) continue;
                    if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, sid)) continue;
                    has_visible_system = true;
                    break;
                  }
                  if (!has_visible_system) continue;

                  const bool sel = (fleet_mut->mission.patrol_region_id == rid);
                  const std::string key = reg->name + "##pat_reg_" + std::to_string(static_cast<unsigned long long>(rid));
                  if (ImGui::Selectable(key.c_str(), sel)) {
                    fleet_mut->mission.patrol_region_id = rid;
                    fleet_mut->mission.patrol_region_system_index = 0;
                    fleet_mut->mission.patrol_region_waypoint_index = 0;
                  }
                }
                ImGui::EndCombo();
              }

              int dwell = std::max(1, fleet_mut->mission.patrol_region_dwell_days);
              if (ImGui::InputInt("Dwell days##fleet_mission_patrol_region_dwell", &dwell)) {
                fleet_mut->mission.patrol_region_dwell_days = std::max(1, dwell);
              }

              ImGui::TextDisabled("Patrols all discovered systems in the region, visiting friendly colonies, then jump points, then major bodies.\nEngages detected hostiles anywhere in-region (requires sensor coverage).\nTip: Use sensors / listening posts across the region for rapid response.");
            }

            if (fleet_mut->mission.type == FleetMissionType::HuntHostiles) {
              ImGui::Spacing();
              ImGui::Text("Hunt hostiles");
              int age = std::max(0, fleet_mut->mission.hunt_max_contact_age_days);
              if (ImGui::InputInt("Max contact age (days)##fleet_mission_hunt_age", &age)) {
                fleet_mut->mission.hunt_max_contact_age_days = std::max(0, age);
              }
              ImGui::TextDisabled("Chases recent hostile contacts across discovered jump routes.");
            }

            if (fleet_mut->mission.type == FleetMissionType::EscortFreighters) {
              ImGui::Spacing();
              ImGui::Text("Escort freighters");

              // Runtime status.
              const Ship* active = (fleet_mut->mission.escort_active_ship_id != kInvalidId)
                                      ? find_ptr(s.ships, fleet_mut->mission.escort_active_ship_id)
                                      : nullptr;
              if (active) {
                std::string sys_name;
                if (const auto* sys = find_ptr(s.systems, active->system_id)) sys_name = sys->name;
                const std::string msg = "Active: " + active->name + (sys_name.empty() ? "" : " (" + sys_name + ")");
                ImGui::TextDisabled(msg.c_str());
              } else {
                ImGui::TextDisabled("Active: (none)");
              }

              // Target ship picker.
              const Ship* fixed = (fleet_mut->mission.escort_target_ship_id != kInvalidId)
                                     ? find_ptr(s.ships, fleet_mut->mission.escort_target_ship_id)
                                     : nullptr;
              const char* tgt_label = fixed ? fixed->name.c_str() : "Auto (best eligible)";
              if (ImGui::BeginCombo("Target ship##fleet_mission_escort_target", tgt_label)) {
                const bool sel_auto = (fleet_mut->mission.escort_target_ship_id == kInvalidId);
                if (ImGui::Selectable("Auto (best eligible)", sel_auto)) {
                  fleet_mut->mission.escort_target_ship_id = kInvalidId;
                }
                ImGui::Separator();

                for (Id sid : sorted_keys(s.ships)) {
                  const auto* sh = find_ptr(s.ships, sid);
                  if (!sh) continue;
                  if (!sim.are_factions_mutual_friendly(fleet_mut->faction_id, sh->faction_id)) continue;
                  if (sim.fleet_for_ship(sid) != kInvalidId) continue;

                  const auto* d = sim.find_design(sh->design_id);
                  const ShipRole r = d ? d->role : ShipRole::Unknown;
                  if (!(r == ShipRole::Freighter || r == ShipRole::Surveyor || r == ShipRole::Unknown)) continue;

                  std::string label = sh->name;
                  if (sh->auto_freight) label += " [auto]";
                  if (const auto* sys = find_ptr(s.systems, sh->system_id)) label += " (" + sys->name + ")";

                  const bool sel = (fleet_mut->mission.escort_target_ship_id == sid);
                  const std::string key = label + "##fleet_mission_escort_ship_" + std::to_string(static_cast<unsigned long long>(sid));
                  if (ImGui::Selectable(key.c_str(), sel)) {
                    fleet_mut->mission.escort_target_ship_id = sid;
                  }
                }
                ImGui::EndCombo();
              }

              ImGui::Checkbox("Auto-select only auto-freight targets##fleet_mission_escort_only_auto", &fleet_mut->mission.escort_only_auto_freight);
              int interval = std::max(0, fleet_mut->mission.escort_retarget_interval_days);
              if (ImGui::InputInt("Retarget interval (days)##fleet_mission_escort_interval", &interval)) {
                fleet_mut->mission.escort_retarget_interval_days = std::max(0, interval);
              }

              double follow = fleet_mut->mission.escort_follow_distance_mkm;
              if (ImGui::InputDouble("Follow distance mkm##fleet_mission_escort_follow", &follow, 0.5, 5.0, "%.1f")) {
                fleet_mut->mission.escort_follow_distance_mkm = std::max(0.0, follow);
              }

              double r = fleet_mut->mission.escort_defense_radius_mkm;
              if (ImGui::InputDouble("Defense radius mkm##fleet_mission_escort_def_r", &r, 10.0, 100.0, "%.1f")) {
                fleet_mut->mission.escort_defense_radius_mkm = std::max(0.0, r);
              }
              ImGui::TextDisabled("Automatically escorts a civilian ship and intercepts detected hostiles near it.\n0 radius = anywhere in-system.");
            }

            if (fleet_mut->mission.type == FleetMissionType::AssaultColony) {
              ImGui::Spacing();
              ImGui::Text("Assault colony");

              // --- Target colony ---
              const Colony* tgt = (fleet_mut->mission.assault_colony_id != kInvalidId)
                                    ? find_ptr(s.colonies, fleet_mut->mission.assault_colony_id)
                                    : nullptr;
              const char* tgt_label = tgt ? tgt->name.c_str() : "(select colony)";
              if (ImGui::BeginCombo("Target##fleet_mission_assault_target", tgt_label)) {
                for (Id cid : sorted_keys(s.colonies)) {
                  const auto* c = find_ptr(s.colonies, cid);
                  if (!c) continue;
                  if (c->faction_id == fleet_mut->faction_id) continue;

                  const auto* body = find_ptr(s.bodies, c->body_id);
                  if (!body || body->system_id == kInvalidId) continue;
                  if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, body->system_id)) continue;

                  std::string label = c->name;
                  if (const auto* own = find_ptr(s.factions, c->faction_id)) {
                    label += " (" + own->name + ")";
                  }
                  if (const auto* sys = find_ptr(s.systems, body->system_id)) {
                    label += " - " + sys->name;
                  }

                  const bool sel = (fleet_mut->mission.assault_colony_id == cid);
                  const std::string key = label + "##fleet_mission_assault_target_" + std::to_string(static_cast<unsigned long long>(cid));
                  if (ImGui::Selectable(key.c_str(), sel)) {
                    fleet_mut->mission.assault_colony_id = cid;
                    fleet_mut->mission.assault_bombard_executed = false;
                  }
                }
                ImGui::EndCombo();
              }

              if (selected_colony != kInvalidId) {
                const Colony* sc = find_ptr(s.colonies, selected_colony);
                if (sc && sc->faction_id != fleet_mut->faction_id) {
                  if (ImGui::SmallButton("Use selected colony##fleet_mission_assault_use_selected")) {
                    fleet_mut->mission.assault_colony_id = selected_colony;
                    fleet_mut->mission.assault_bombard_executed = false;
                  }
                }
              }

              // --- Staging ---
              ImGui::Checkbox("Auto stage troops##fleet_mission_assault_auto_stage", &fleet_mut->mission.assault_auto_stage);
              const Colony* stage = (fleet_mut->mission.assault_staging_colony_id != kInvalidId)
                                      ? find_ptr(s.colonies, fleet_mut->mission.assault_staging_colony_id)
                                      : nullptr;
              const char* stage_label = stage ? stage->name.c_str() : "(auto)";
              if (ImGui::BeginCombo("Staging colony##fleet_mission_assault_stage", stage_label)) {
                const bool sel_auto = (fleet_mut->mission.assault_staging_colony_id == kInvalidId);
                if (ImGui::Selectable("(auto)", sel_auto)) {
                  fleet_mut->mission.assault_staging_colony_id = kInvalidId;
                }
                ImGui::Separator();
                for (Id cid : sorted_keys(s.colonies)) {
                  const auto* c = find_ptr(s.colonies, cid);
                  if (!c) continue;
                  if (c->faction_id != fleet_mut->faction_id) continue;
                  const auto* body = find_ptr(s.bodies, c->body_id);
                  if (!body || body->system_id == kInvalidId) continue;
                  if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, body->system_id)) continue;

                  std::string label = c->name;
                  if (const auto* sys = find_ptr(s.systems, body->system_id)) {
                    label += " - " + sys->name;
                  }

                  const bool sel = (fleet_mut->mission.assault_staging_colony_id == cid);
                  const std::string key = label + "##fleet_mission_assault_stage_" + std::to_string(static_cast<unsigned long long>(cid));
                  if (ImGui::Selectable(key.c_str(), sel)) {
                    fleet_mut->mission.assault_staging_colony_id = cid;
                  }
                }
                ImGui::EndCombo();
              }

              double margin = std::clamp(fleet_mut->mission.assault_troop_margin_factor, 1.0, 10.0);
              if (ImGui::InputDouble("Troop margin##fleet_mission_assault_margin", &margin, 0.05, 0.25, "%.2f")) {
                fleet_mut->mission.assault_troop_margin_factor = std::clamp(margin, 1.0, 10.0);
              }

              // --- Bombardment ---
              ImGui::Checkbox("Bombard before invade##fleet_mission_assault_bombard", &fleet_mut->mission.assault_use_bombardment);
              int bd = fleet_mut->mission.assault_bombard_days;
              if (!fleet_mut->mission.assault_use_bombardment) {
                ImGui::BeginDisabled();
              }
              if (ImGui::InputInt("Bombard days##fleet_mission_assault_bombard_days", &bd)) {
                // 0 disables bombardment; -1 means bombard indefinitely.
                fleet_mut->mission.assault_bombard_days = std::clamp(bd, -1, 3650);
                if (fleet_mut->mission.assault_bombard_days == 0) {
                  fleet_mut->mission.assault_use_bombardment = false;
                }
              }
              if (!fleet_mut->mission.assault_use_bombardment) {
                ImGui::EndDisabled();
              }

              if (ImGui::SmallButton("Reset assault progress##fleet_mission_assault_reset")) {
                fleet_mut->mission.assault_bombard_executed = false;
              }

              // --- Quick status ---
              double embarked = 0.0;
              double cap = 0.0;
              for (Id sid : selected_fleet->ship_ids) {
                const auto* sh = find_ptr(s.ships, sid);
                if (!sh) continue;
                const auto* d = sim.find_design(sh->design_id);
                embarked += std::max(0.0, sh->troops);
                if (d) cap += std::max(0.0, d->troop_capacity);
              }
              ImGui::TextDisabled("Embarked troops: %.1f / %.1f", embarked, cap);

              if (tgt) {
                ImGui::TextDisabled("Target garrison: %.1f (garrison target %.1f)",
                                    std::max(0.0, tgt->ground_forces), std::max(0.0, tgt->garrison_target_strength));
              }

              ImGui::TextDisabled("Stages troops (optional), bombards once (optional), then invades with troop ships.\nTip: Use 'Start mission' to clear any existing orders before running.");
            }



            if (fleet_mut->mission.type == FleetMissionType::BlockadeColony) {
              ImGui::Spacing();
              ImGui::Text("Blockade colony");

              // Colony picker (discovered colonies).
              const Colony* tgt = (fleet_mut->mission.blockade_colony_id != kInvalidId)
                                     ? find_ptr(s.colonies, fleet_mut->mission.blockade_colony_id)
                                     : nullptr;
              const char* col_label = tgt ? tgt->name.c_str() : "(select colony)";
              if (ImGui::BeginCombo("Target##fleet_mission_blockade_colony", col_label)) {
                for (Id cid : sorted_keys(s.colonies)) {
                  const auto* c = find_ptr(s.colonies, cid);
                  if (!c) continue;

                  const auto* body = find_ptr(s.bodies, c->body_id);
                  if (!body || body->system_id == kInvalidId) continue;
                  if (!sim.is_system_discovered_by_faction(fleet_mut->faction_id, body->system_id)) continue;

                  std::string label = c->name;
                  if (const auto* sys = find_ptr(s.systems, body->system_id)) {
                    label += " - " + sys->name;
                  }
                  if (c->faction_id == fleet_mut->faction_id) {
                    label += " (own)";
                  } else if (c->faction_id != kInvalidId) {
                    if (const auto* col_fac = find_ptr(s.factions, c->faction_id)) {
                      label += " (" + col_fac->name + ")";
                    }
                  }

                  const bool sel = (fleet_mut->mission.blockade_colony_id == cid);
                  const std::string key = label + "##blk_col_" + std::to_string(static_cast<unsigned long long>(cid));
                  if (ImGui::Selectable(key.c_str(), sel)) {
                    fleet_mut->mission.blockade_colony_id = cid;
                    tgt = c;
                  }
                }
                ImGui::EndCombo();
              }

              const double default_radius = std::max(0.0, sim.cfg().blockade_radius_mkm);
              double r = std::max(0.0, fleet_mut->mission.blockade_radius_mkm);
              if (ImGui::InputDouble("Radius mkm##fleet_mission_blockade_radius", &r, 0.5, 2.0, "%.2f")) {
                fleet_mut->mission.blockade_radius_mkm = std::max(0.0, r);
              }
              if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("0 uses the global SimConfig blockade radius (%.2f mkm).", default_radius);
              }

              const double eff_radius = (fleet_mut->mission.blockade_radius_mkm > 0.0)
                                          ? fleet_mut->mission.blockade_radius_mkm
                                          : default_radius;
              ImGui::TextDisabled("Effective radius: %.2f mkm", eff_radius);

              if (tgt) {
                if (tgt->faction_id == fleet_mut->faction_id) {
                  ImGui::TextDisabled("Note: targeting an owned colony produces no blockade pressure.");
                }

                const auto bs = sim.blockade_status_for_colony(tgt->id);
                ImGui::TextDisabled("Pressure: %.2f   Output: %.0f%%", bs.pressure, bs.output_multiplier * 100.0);
                ImGui::TextDisabled("Hostiles: %d ship(s), power %.1f   Defenders: %d ship(s), power %.1f",
                                    bs.hostile_ships, bs.hostile_power, bs.defender_ships, bs.defender_power);

                const auto* body = find_ptr(s.bodies, tgt->body_id);
                if (body) {
                  const auto* sys = find_ptr(s.systems, body->system_id);
                  if (sys) {
                    ImGui::TextDisabled("System: %s", sys->name.c_str());
                  }

                  if (ImGui::SmallButton("Focus target##fleet_mission_blockade_focus")) {
                    selected_ship = kInvalidId;
                    selected_colony = tgt->id;
                    selected_body = tgt->body_id;
                    s.selected_system = body->system_id;
                    ui.request_details_tab = DetailsTab::Colony;
                    ui.show_map_window = true;
                    ui.request_map_tab = MapTab::System;
                    ui.request_focus_faction_id = fleet_mut->faction_id;
                    ui.request_system_map_center = true;
                    ui.request_system_map_center_system_id = body->system_id;
                    ui.request_system_map_center_x_mkm = body->position_mkm.x;
                    ui.request_system_map_center_y_mkm = body->position_mkm.y;
                    ui.request_system_map_center_zoom = 0.0;
                  }
                }
              }

              ImGui::TextDisabled("Maintains a hostile presence near the colony and disrupts its activity.");
              ImGui::TextDisabled("Effect: reduces repairs, troop training, and terraforming efficiency (best-effort).");
              ImGui::TextDisabled("(See Logistics tab for blockade summaries.)");
            }
            if (fleet_mut->mission.type == FleetMissionType::Explore) {
              ImGui::Spacing();
              ImGui::Text("Explore systems");
            
              ImGui::Checkbox("Survey exits before transiting##fleet_mission_explore_survey_first", &fleet_mut->mission.explore_survey_first);
              ImGui::Checkbox("Transit to undiscovered systems##fleet_mission_explore_allow_transit", &fleet_mut->mission.explore_allow_transit);
              ImGui::Checkbox("Survey+transit frontier exits##fleet_mission_explore_survey_transit", &fleet_mut->mission.explore_survey_transit_when_done);
              ImGui::Checkbox("Investigate anomalies##fleet_mission_explore_anoms", &fleet_mut->mission.explore_investigate_anomalies);
              ImGui::Checkbox("Salvage wrecks when safe##fleet_mission_explore_wrecks", &fleet_mut->mission.explore_salvage_wrecks);
            
              ImGui::TextDisabled("Surveys unknown jump points, then transits surveyed exits into undiscovered systems\nwhen enabled. Routes to the best frontier system when idle.");
            }

            // Sustainment toggles.
            ImGui::Spacing();
            ImGui::Text("Sustainment");
            ImGui::Checkbox("Auto-refuel##fleet_mission_auto_refuel", &fleet_mut->mission.auto_refuel);
            if (fleet_mut->mission.auto_refuel) {
              float thr = static_cast<float>(std::clamp(fleet_mut->mission.refuel_threshold_fraction, 0.0, 1.0));
              float res = static_cast<float>(std::clamp(fleet_mut->mission.refuel_resume_fraction, 0.0, 1.0));
              ImGui::SliderFloat("Refuel at##fleet_mission_refuel_thr", &thr, 0.0f, 1.0f, "%.2f");
              ImGui::SliderFloat("Resume at##fleet_mission_refuel_res", &res, 0.0f, 1.0f, "%.2f");
              fleet_mut->mission.refuel_threshold_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
              fleet_mut->mission.refuel_resume_fraction = std::clamp(static_cast<double>(res), 0.0, 1.0);
            }

            ImGui::Checkbox("Auto-repair##fleet_mission_auto_repair", &fleet_mut->mission.auto_repair);
            if (fleet_mut->mission.auto_repair) {
              float thr = static_cast<float>(std::clamp(fleet_mut->mission.repair_threshold_fraction, 0.0, 1.0));
              float res = static_cast<float>(std::clamp(fleet_mut->mission.repair_resume_fraction, 0.0, 1.0));
              ImGui::SliderFloat("Repair at##fleet_mission_repair_thr", &thr, 0.0f, 1.0f, "%.2f");
              ImGui::SliderFloat("Resume at##fleet_mission_repair_res", &res, 0.0f, 1.0f, "%.2f");
              fleet_mut->mission.repair_threshold_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
              fleet_mut->mission.repair_resume_fraction = std::clamp(static_cast<double>(res), 0.0, 1.0);
            }

            ImGui::Checkbox("Auto-rearm (munitions)##fleet_mission_auto_rearm", &fleet_mut->mission.auto_rearm);
            if (fleet_mut->mission.auto_rearm) {
              float thr = static_cast<float>(std::clamp(fleet_mut->mission.rearm_threshold_fraction, 0.0, 1.0));
              float res = static_cast<float>(std::clamp(fleet_mut->mission.rearm_resume_fraction, 0.0, 1.0));
              ImGui::SliderFloat("Rearm at##fleet_mission_rearm_thr", &thr, 0.0f, 1.0f, "%.2f");
              ImGui::SliderFloat("Resume at##fleet_mission_rearm_res", &res, 0.0f, 1.0f, "%.2f");
              fleet_mut->mission.rearm_threshold_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
              fleet_mut->mission.rearm_resume_fraction = std::clamp(static_cast<double>(res), 0.0, 1.0);
              ImGui::TextDisabled("Seeks Munitions at a trade-partner port.");
            }

            if (sim.cfg().enable_ship_maintenance && !sim.cfg().ship_maintenance_resource_id.empty()) {
              ImGui::Checkbox("Auto-maintenance##fleet_mission_auto_maintenance", &fleet_mut->mission.auto_maintenance);
              if (fleet_mut->mission.auto_maintenance) {
                float thr = static_cast<float>(std::clamp(fleet_mut->mission.maintenance_threshold_fraction, 0.0, 1.0));
                float res = static_cast<float>(std::clamp(fleet_mut->mission.maintenance_resume_fraction, 0.0, 1.0));
                ImGui::SliderFloat("Maintenance at##fleet_mission_maint_thr", &thr, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Resume at##fleet_mission_maint_res", &res, 0.0f, 1.0f, "%.2f");
                fleet_mut->mission.maintenance_threshold_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
                fleet_mut->mission.maintenance_resume_fraction = std::clamp(static_cast<double>(res), 0.0, 1.0);
                ImGui::TextDisabled(("Uses " + sim.cfg().ship_maintenance_resource_id + " stockpiles.").c_str());
              }
            }

            ImGui::TextDisabled("Priority: Refuel > Repair > Rearm > Maintenance.");

            // Status
            if (fleet_mut->mission.sustainment_mode != FleetSustainmentMode::None) {
              const char* mode = "Sustaining";
              switch (fleet_mut->mission.sustainment_mode) {
                case FleetSustainmentMode::Refuel: mode = "Refueling"; break;
                case FleetSustainmentMode::Repair: mode = "Repairing"; break;
                case FleetSustainmentMode::Rearm: mode = "Rearming"; break;
                case FleetSustainmentMode::Maintenance: mode = "Maintaining"; break;
                case FleetSustainmentMode::None: default: break;
              }
              std::string at = "(unknown)";
              if (fleet_mut->mission.sustainment_colony_id != kInvalidId) {
                if (const auto* c = find_ptr(s.colonies, fleet_mut->mission.sustainment_colony_id)) {
                  at = c->name;
                }
              }
              ImGui::TextDisabled((std::string(mode) + " at " + at).c_str());
            }
          }
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

        // Population logistics (optional): targets/reserves for auto-colonist transport.
        ImGui::Indent();
        double pop_target = colony->population_target_millions;
        double pop_reserve = colony->population_reserve_millions;

        if (ImGui::InputDouble("Population target (M)##col_pop_target", &pop_target, 10.0, 100.0, "%.0f")) {
          colony->population_target_millions = std::max(0.0, pop_target);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("If current population is below this value, auto-colonist transports may deliver colonists here.");
        }

        if (ImGui::InputDouble("Population reserve (M)##col_pop_reserve", &pop_reserve, 10.0, 100.0, "%.0f")) {
          colony->population_reserve_millions = std::max(0.0, pop_reserve);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Auto-colonist transports will not export population below this value.");
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Set reserve = current##col_pop_reserve_cur")) {
          colony->population_reserve_millions = std::max(0.0, colony->population_millions);
        }

        const double pop_floor = std::max(colony->population_target_millions, colony->population_reserve_millions);
        const double pop_surplus = (pop_floor > 1e-9) ? std::max(0.0, colony->population_millions - pop_floor) : 0.0;
        const double pop_deficit = std::max(0.0, colony->population_target_millions - colony->population_millions);
        ImGui::TextDisabled("Floor: %.0f M | Surplus: %.0f M | Deficit: %.0f M", pop_floor, pop_surplus, pop_deficit);
        ImGui::Unindent();



        // Blockade status (if enabled).
        if (sim.cfg().enable_blockades) {
          const auto bs = sim.blockade_status_for_colony(colony->id);
          if (bs.pressure > 1e-6) {
            ImGui::TextDisabled("Blockade: pressure %.2f, output %.0f%%", bs.pressure, bs.output_multiplier * 100.0);
            ImGui::TextDisabled("Hostiles: %d ship(s), power %.1f | Defenders: %d ship(s), power %.1f",
                                bs.hostile_ships, bs.hostile_power, bs.defender_ships, bs.defender_power);
          } else {
            ImGui::TextDisabled("Blockade: none");
          }
        }

        // Trade prosperity (if enabled).
        if (sim.cfg().enable_trade_prosperity) {
          const auto tp = sim.trade_prosperity_status_for_colony(colony->id);
          const double bonus_pct = tp.output_bonus * 100.0;
          if (bonus_pct > 0.05) {
            ImGui::TextDisabled("Trade prosperity: +%.1f%% output (x%.3f)", bonus_pct, tp.output_multiplier);
          } else {
            ImGui::TextDisabled("Trade prosperity: none");
          }
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (tp.trade_partner_count > 0 && tp.treaty_market_boost > 1.0001) {
              ImGui::Text("Market size: %.2f (effective %.2f, factor %.2f)", tp.market_size, tp.effective_market_size,
                          tp.market_factor);
              ImGui::Text("Trade partners: %d (treaty boost x%.2f)", tp.trade_partner_count, tp.treaty_market_boost);
            } else {
              ImGui::Text("Market size: %.2f (factor %.2f)", tp.market_size, tp.market_factor);
              ImGui::Text("Trade partners: %d", tp.trade_partner_count);
            }
            ImGui::Text("Hub score: %.2f", tp.hub_score);
            ImGui::Text("Population factor: %.2f", tp.pop_factor);
            ImGui::Text("Piracy risk: %.2f", tp.piracy_risk);
            ImGui::Text("Blockade pressure: %.2f", tp.blockade_pressure);
            // Recent merchant losses can depress trade even if pirates have
            // moved on (insurance / confidence effect).
            ImGui::Text("Shipping loss pressure: %.2f", tp.shipping_loss_pressure);
            {
              Id sys_id = kInvalidId;
              if (const Body* b = find_ptr(s.bodies, colony->body_id)) {
                sys_id = b->system_id;
              }
              if (sys_id != kInvalidId) {
                const auto loss = sim.civilian_shipping_loss_status_for_system(sys_id);
                if (loss.recent_wrecks > 0) {
                  ImGui::Text("Recent merchant wrecks: %d (score %.2f)", loss.recent_wrecks, loss.score);
                }
              }
            }
            ImGui::Text("Max bonus: +%.0f%%", sim.cfg().trade_prosperity_max_output_bonus * 100.0);
            ImGui::EndTooltip();
          }
        }


        if (sim.cfg().enable_colony_conditions) {
          ImGui::Separator();

          const auto stab = sim.colony_stability_status_for_colony(colony->id);
          ImGui::Text("Stability: %.0f%%", stab.stability * 100.0);
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Habitability: %.2f", stab.habitability);
            ImGui::Text("Habitation shortfall: %.0f%%", stab.habitation_shortfall_frac * 100.0);
            ImGui::Text("Trade bonus: +%.0f%%", stab.trade_bonus * 100.0);
            ImGui::Text("Piracy risk: %.0f%%", stab.piracy_risk * 100.0);
            ImGui::Text("Shipping loss pressure: %.0f%%", stab.shipping_loss_pressure * 100.0);
            ImGui::Text("Blockade pressure: %.0f%%", stab.blockade_pressure * 100.0);
            ImGui::Text("Conditions delta: %+0.2f", stab.condition_delta);
            ImGui::EndTooltip();
          }

          ImGui::Text("Conditions");
          if (colony->conditions.empty()) {
            ImGui::TextDisabled("(none)");
          } else {
            static std::string resolve_status;
            bool resolved_this_frame = false;

            for (std::size_t i = 0; i < colony->conditions.size(); ++i) {
              const auto& cond = colony->conditions[i];
              if (cond.id.empty()) continue;

              const std::string name = sim.colony_condition_display_name(cond.id);
              const std::string desc = sim.colony_condition_description(cond.id);
              const bool positive = sim.colony_condition_is_positive(cond.id);
              const int days = static_cast<int>(std::ceil(std::max(0.0, cond.remaining_days)));

              const auto cost = sim.colony_condition_resolve_cost(colony->id, cond);

              bool affordable = true;
              for (const auto& [mineral, amt] : cost) {
                const auto it = colony->minerals.find(mineral);
                const double have = it == colony->minerals.end() ? 0.0 : std::max(0.0, it->second);
                if (have + 1e-9 < amt) affordable = false;
              }

              ImGui::PushID(static_cast<int>(i));
              ImGui::Bullet();
              ImGui::SameLine();
              ImGui::Text("%s%s", name.c_str(), positive ? " (positive)" : "");
              const bool hovered_name = ImGui::IsItemHovered();
              ImGui::SameLine();
              ImGui::TextDisabled("(%dd)", days);

              if (!cost.empty()) {
                ImGui::SameLine();
                if (!affordable) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Resolve")) {
                  std::string err;
                  const std::string cond_id = cond.id;
                  if (!sim.resolve_colony_condition(colony->id, cond_id, &err)) {
                    resolve_status = err;
                  } else {
                    resolve_status.clear();
                  }
                  resolved_this_frame = true;
                }
                if (!affordable) ImGui::EndDisabled();
              }

              if (hovered_name) {
                ImGui::BeginTooltip();
                if (!desc.empty()) {
                  ImGui::TextWrapped("%s", desc.c_str());
                } else {
                  ImGui::TextWrapped("%s", cond.id.c_str());
                }

                ImGui::Separator();
                ImGui::Text("Remaining: %d days", days);
                ImGui::Text("Severity: %.2f", cond.severity);

                const auto eff = sim.colony_condition_multipliers_for_condition(cond);
                ImGui::Separator();
                ImGui::Text("Effects (multipliers)");
                ImGui::Text("Mining:        x%.2f", eff.mining);
                ImGui::Text("Industry:      x%.2f", eff.industry);
                ImGui::Text("Research:      x%.2f", eff.research);
                ImGui::Text("Construction:  x%.2f", eff.construction);
                ImGui::Text("Shipyard:      x%.2f", eff.shipyard);
                ImGui::Text("Terraforming:  x%.2f", eff.terraforming);
                ImGui::Text("Troop training:x%.2f", eff.troop_training);
                ImGui::Text("Pop growth:    x%.2f", eff.pop_growth);

                if (!cost.empty()) {
                  ImGui::Separator();
                  ImGui::Text("Resolve cost");
                  for (const auto& [mineral, amt] : cost) {
                    const auto it = colony->minerals.find(mineral);
                    const double have = it == colony->minerals.end() ? 0.0 : std::max(0.0, it->second);
                    ImGui::Text("%s: %.0f / %.0f", mineral.c_str(), have, amt);
                  }
                  if (!affordable) ImGui::TextDisabled("(insufficient stockpile)");
                }

                ImGui::EndTooltip();
              }

              ImGui::PopID();
              if (resolved_this_frame) break;
            }

            if (!resolve_status.empty()) {
              ImGui::TextWrapped("%s", resolve_status.c_str());
            }
          }
        }

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

        // --- Garrison target automation (QoL) ---
        // Keeps enough *auto-queued* training in the queue to reach a desired
        // garrison strength, without consuming or pruning manual training.
        {
          double target = colony->garrison_target_strength;
          ImGui::InputDouble("Garrison target##garrison_target", &target, 50.0, 200.0, "%.1f");
          target = std::max(0.0, target);
          colony->garrison_target_strength = target;

          if (ImGui::SmallButton("Set = current##garrison_target_now")) {
            colony->garrison_target_strength = std::max(0.0, colony->ground_forces);
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Clear##garrison_target_clear")) {
            colony->garrison_target_strength = 0.0;
          }

          if (colony->garrison_target_strength > 1e-9) {
            const double auto_q = std::clamp(colony->troop_training_auto_queued, 0.0, colony->troop_training_queue);
            const double manual_q = std::max(0.0, colony->troop_training_queue - auto_q);
            const double needed = std::max(0.0, colony->garrison_target_strength - colony->ground_forces);

            ImGui::TextDisabled("Need: %.1f  Queued: %.1f (manual %.1f / auto %.1f)",
                                needed, colony->troop_training_queue, manual_q, auto_q);
          } else {
            ImGui::TextDisabled("(No target; training queue is fully manual)");
          }
        }

        // Active battle status
        if (auto itb = s.ground_battles.find(colony->id); itb != s.ground_battles.end()) {
          const auto& b = itb->second;
          ImGui::TextDisabled("Ground battle in progress");
          ImGui::Text("Attacker: %.1f", b.attacker_strength);
          ImGui::Text("Defender: %.1f", b.defender_strength);
          ImGui::Text("Days: %d", b.days_fought);

          // Defender artillery (installation weapons).
          double defender_arty_weapon = 0.0;
          {
            for (const auto& [inst_id, count] : colony->installations) {
              if (count <= 0) continue;
              const auto it = sim.content().installations.find(inst_id);
              if (it == sim.content().installations.end()) continue;
              const double wd = it->second.weapon_damage;
              if (wd <= 0.0) continue;
              defender_arty_weapon += wd * static_cast<double>(count);
            }
            defender_arty_weapon = std::max(0.0, defender_arty_weapon);
          }

          const auto fc = nebula4x::forecast_ground_battle(sim.cfg(), b.attacker_strength, b.defender_strength, forts,
                                                           defender_arty_weapon);
          if (fc.ok) {
            if (fc.truncated) {
              ImGui::TextDisabled("Forecast: %s", fc.truncated_reason.c_str());
            } else {
              const char* winner = (fc.winner == nebula4x::GroundBattleWinner::Attacker) ? "attacker" : "defender";
              ImGui::Text("Forecast: %s wins in ~%d d", winner, fc.days_to_resolve);
              ImGui::Text("End: att %.1f / def %.1f", fc.attacker_end, fc.defender_end);
              ImGui::Text("Defense bonus: %.2f", fc.defense_bonus);
            }
          } else {
            ImGui::TextDisabled("Forecast: unavailable");
          }
        }

        const double train_pts = sim.troop_training_points_per_day(*colony);
        if (train_pts > 1e-9) {
          ImGui::Text("Training: %.1f pts/day", train_pts);
        } else {
          ImGui::TextDisabled("Training: 0 (build a Training Facility)");
        }
        {
          const double auto_q = std::clamp(colony->troop_training_auto_queued, 0.0, colony->troop_training_queue);
          const double manual_q = std::max(0.0, colony->troop_training_queue - auto_q);
          ImGui::Text("Training queue: %.1f (manual %.1f / auto %.1f)", colony->troop_training_queue, manual_q, auto_q);

          const double str_per_day = train_pts * std::max(0.0, sim.cfg().troop_strength_per_training_point);
          if (str_per_day > 1e-9 && colony->troop_training_queue > 1e-9) {
            const double eta_days = colony->troop_training_queue / str_per_day;
            ImGui::TextDisabled("ETA: ~%.1f days (if minerals available)", eta_days);
          }
        }

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

            // Best-effort ETA forecast (based on current points/day and current mineral stockpiles).
            {
              TerraformingScheduleOptions opt;
              opt.max_days = 36500;
              TerraformingSchedule sched = estimate_terraforming_schedule(sim, b->id, opt);
              if (sched.ok && sched.has_target) {
                if (sched.complete && sched.days_to_complete <= 0) {
                  // Already complete.
                } else if (sched.stalled) {
                  if (!sched.stall_reason.empty()) {
                    ImGui::TextDisabled("ETA: stalled (%s)", sched.stall_reason.c_str());
                  } else {
                    ImGui::TextDisabled("ETA: stalled");
                  }
                } else if (sched.truncated) {
                  ImGui::TextDisabled("ETA: >%d days (forecast limit)", opt.max_days);
                } else if (sched.days_to_complete > 0) {
                  const Date eta_date = sim.state().date.add_days(sched.days_to_complete);
                  ImGui::TextDisabled("ETA: %d days (%s)", sched.days_to_complete, eta_date.to_string().c_str());
                } else {
                  ImGui::TextDisabled("ETA: unknown");
                }

                if (!opt.ignore_mineral_costs && (sched.duranium_per_point > 0.0 || sched.neutronium_per_point > 0.0)) {
                  ImGui::TextDisabled("Forecast consumption (no replenishment): D %.0f, N %.0f", sched.duranium_consumed,
                                      sched.neutronium_consumed);
                }
              }
            }
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
          ImGui::SameLine();
          if (ImGui::SmallButton("Planner##tf")) ui.show_terraforming_window = true;
        }

        // --- Habitability / Life support ---
        ImGui::Separator();
        ImGui::Text("Habitability / Life Support");
        if (!sim.cfg().enable_habitability) {
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
        {
          std::vector<std::string> inst_ids;
          inst_ids.reserve(colony->installations.size());
          for (const auto& [k, _] : colony->installations) inst_ids.push_back(k);
          std::sort(inst_ids.begin(), inst_ids.end());

          for (const auto& k : inst_ids) {
            const int v = colony->installations.at(k);
            const auto it = sim.content().installations.find(k);
            const std::string nm = (it == sim.content().installations.end()) ? k : it->second.name;

            std::vector<std::string> tags;
            if (it != sim.content().installations.end()) {
              const auto& def = it->second;
              if (def.sensor_range_mkm > 0.0) {
                tags.push_back("Sensor " + std::to_string(static_cast<int>(std::round(def.sensor_range_mkm))) + " mkm");
              }
              if (def.weapon_damage > 0.0 && def.weapon_range_mkm > 0.0) {
                tags.push_back("Weapon " + format_fixed(def.weapon_damage, 1) + "/day @ " +
                               format_fixed(def.weapon_range_mkm, 1) + " mkm");
              }
              if (def.point_defense_damage > 0.0 && def.point_defense_range_mkm > 0.0) {
                tags.push_back("PD " + format_fixed(def.point_defense_damage, 1) + "/day @ " +
                               format_fixed(def.point_defense_range_mkm, 1) + " mkm");
              }
            }

            if (tags.empty()) {
              ImGui::BulletText("%s: %d", nm.c_str(), v);
            } else {
              std::string extra;
              for (std::size_t i = 0; i < tags.size(); ++i) {
                if (i) extra += "; ";
                extra += tags[i];
              }
              ImGui::BulletText("%s: %d  (%s)", nm.c_str(), v, extra.c_str());
            }
          }
        }

        if (ImGui::TreeNode("Installation targets (auto-build)")) {
          ImGui::TextDisabled("The simulation will auto-queue construction orders to reach these counts.");
          ImGui::TextDisabled("Auto-queued orders are marked [AUTO] in the construction queue.");
          ImGui::SameLine();
          if (ImGui::SmallButton("Clear all targets")) {
            colony->installation_targets.clear();
          }

          static char inst_filter[64] = "";
          ImGui::SetNextItemWidth(220.0f);
          ImGui::InputTextWithHint("##inst_filter", "Filter installations (name/id)...", inst_filter,
                                  sizeof(inst_filter));

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

              if (!case_insensitive_contains(nm2, inst_filter) && !case_insensitive_contains(id, inst_filter)) {
                continue;
              }

              const int have = [&]() -> int {
                auto inst_it = colony->installations.find(id);
                return (inst_it == colony->installations.end()) ? 0 : inst_it->second;
              }();

              const int man = pending_manual[id];
              const int aut = pending_auto[id];

              int tgt = 0;
              if (auto tgt_it = colony->installation_targets.find(id); tgt_it != colony->installation_targets.end()) tgt = tgt_it->second;
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

        ImGui::Separator();
        ImGui::Text("Construction");
        const double cp_per_day = sim.construction_points_per_day(*colony);
        ImGui::Text("Construction Points/day: %.1f", cp_per_day);

        // --- Production forecast (best-effort) ---
        if (ImGui::TreeNodeEx("Production forecast (best-effort)", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::TextDisabled(
              "Simulates local mining/industry + shipyard + construction day-by-day. "
              "Does not model freight/imports or global auto-shipyard balancing.");

          static int forecast_days = 3650;
          static int forecast_max_events = 256;
          static bool forecast_include_shipyard = true;
          static bool forecast_include_construction = true;
          static bool forecast_include_auto_targets = true;

          static Id cached_colony_id = kInvalidId;
          static std::int64_t cached_day = -1;
          static int cached_days = 0;
          static int cached_max_events = 0;
          static bool cached_shipyard = true;
          static bool cached_construction = true;
          static bool cached_auto_targets = true;
          static ColonySchedule cached_sched;
          static bool has_cached = false;

          bool recompute = false;
          if (ImGui::SmallButton("Recompute##colony_forecast")) {
            recompute = true;
          }
          ImGui::SameLine();
          ImGui::SetNextItemWidth(90.0f);
          ImGui::InputInt("Max days##colony_forecast_days", &forecast_days);
          ImGui::SameLine();
          ImGui::SetNextItemWidth(90.0f);
          ImGui::InputInt("Max events##colony_forecast_events", &forecast_max_events);
          ImGui::Checkbox("Include shipyard##colony_forecast_shipyard", &forecast_include_shipyard);
          ImGui::SameLine();
          ImGui::Checkbox("Include construction##colony_forecast_construction", &forecast_include_construction);
          ImGui::SameLine();
          ImGui::Checkbox("Auto-build targets##colony_forecast_autotgt", &forecast_include_auto_targets);

          forecast_days = std::clamp(forecast_days, 0, 36500);
          forecast_max_events = std::clamp(forecast_max_events, 0, 4096);

          const std::int64_t now_day = sim.state().date.days_since_epoch();
          if (!has_cached || recompute || cached_colony_id != colony->id || cached_day != now_day ||
              cached_days != forecast_days || cached_max_events != forecast_max_events ||
              cached_shipyard != forecast_include_shipyard ||
              cached_construction != forecast_include_construction ||
              cached_auto_targets != forecast_include_auto_targets) {
            ColonyScheduleOptions opt;
            opt.max_days = forecast_days;
            opt.max_events = forecast_max_events;
            opt.include_shipyard = forecast_include_shipyard;
            opt.include_construction = forecast_include_construction;
            opt.include_auto_construction_targets = forecast_include_auto_targets;
            cached_sched = estimate_colony_schedule(sim, colony->id, opt);
            cached_colony_id = colony->id;
            cached_day = now_day;
            cached_days = forecast_days;
            cached_max_events = forecast_max_events;
            cached_shipyard = forecast_include_shipyard;
            cached_construction = forecast_include_construction;
            cached_auto_targets = forecast_include_auto_targets;
            has_cached = true;
          }

          if (!cached_sched.ok) {
            ImGui::TextDisabled("Forecast unavailable.");
            ImGui::TreePop();
          } else {
            if (cached_sched.stalled) {
              ImGui::TextDisabled("Stalled: %s", cached_sched.stall_reason.c_str());
            }
            if (cached_sched.truncated) {
              ImGui::TextDisabled("Truncated: %s", cached_sched.truncated_reason.c_str());
            }

            ImGui::TextDisabled("Rates: CP/day %.1f, Shipyard tons/day %.1f", cached_sched.construction_cp_per_day_start,
                                cached_sched.shipyard_tons_per_day_start);
            ImGui::TextDisabled("Multipliers: mining %.2f, industry %.2f, construction %.2f, shipyard %.2f",
                                cached_sched.mining_multiplier, cached_sched.industry_multiplier,
                                cached_sched.construction_multiplier, cached_sched.shipyard_multiplier);

            const ImGuiTableFlags fflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("colony_forecast_table", 5, fflags)) {
              ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 24.0f);
              ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
              ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
              ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 80.0f);
              ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 110.0f);
              ImGui::TableHeadersRow();

              int idx = 0;
              for (const auto& ev : cached_sched.events) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", idx++);

                ImGui::TableSetColumnIndex(1);
                const char* kind = "Note";
                if (ev.kind == ColonyScheduleEventKind::ShipyardComplete) kind = "Shipyard";
                if (ev.kind == ColonyScheduleEventKind::ConstructionComplete) kind = "Construction";
                ImGui::TextUnformatted(kind);

                ImGui::TableSetColumnIndex(2);
                if (ev.auto_queued) {
                  ImGui::Text("[AUTO] %s", ev.detail.c_str());
                } else {
                  ImGui::TextUnformatted(ev.detail.c_str());
                }

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("D+%d", ev.day);

                ImGui::TableSetColumnIndex(4);
                const std::string date = sim.state().date.add_days(ev.day).to_string();
                ImGui::TextUnformatted(date.c_str());
              }

              ImGui::EndTable();
            }

            ImGui::TreePop();
          }
        }

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

      // Right-click: edit optional post-build metadata (profile / fleet / rally).
      if (ImGui::BeginPopupContextItem(("yard_ctx_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
        auto& bo_mut = colony->shipyard_queue[static_cast<std::size_t>(i)];

        Id colony_sys_id = kInvalidId;
        if (const auto* cb = find_ptr(s.bodies, colony->body_id)) colony_sys_id = cb->system_id;

        ImGui::TextDisabled("Shipyard order metadata");
        ImGui::Separator();

        if (ImGui::BeginMenu("Apply Ship Profile")) {
          if (ImGui::MenuItem("<none>", nullptr, bo_mut.apply_ship_profile_name.empty())) {
            bo_mut.apply_ship_profile_name.clear();
          }

          if (const auto* fac = find_ptr(s.factions, colony->faction_id)) {
            std::vector<std::string> names;
            names.reserve(fac->ship_profiles.size());
            for (const auto& kv : fac->ship_profiles) names.push_back(kv.first);
            std::sort(names.begin(), names.end());

            if (names.empty()) {
              ImGui::TextDisabled("No ship profiles defined.");
            } else {
              for (const auto& nm2 : names) {
                const bool selected = (bo_mut.apply_ship_profile_name == nm2);
                if (ImGui::MenuItem(nm2.c_str(), nullptr, selected)) bo_mut.apply_ship_profile_name = nm2;
              }
            }
          } else {
            ImGui::TextDisabled("No faction / no profiles.");
          }

          ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Assign to Fleet")) {
          if (ImGui::MenuItem("<none>", nullptr, bo_mut.assign_to_fleet_id == kInvalidId)) {
            bo_mut.assign_to_fleet_id = kInvalidId;
          }

          std::vector<Id> fleet_ids;
          fleet_ids.reserve(s.fleets.size());
          for (const auto& [fid2, fl] : s.fleets) {
            if (fl.faction_id == colony->faction_id) fleet_ids.push_back(fid2);
          }
          std::sort(fleet_ids.begin(), fleet_ids.end());

          if (fleet_ids.empty()) {
            ImGui::TextDisabled("No fleets.");
          } else {
            auto fleet_system = [&](const Fleet& fl) -> Id {
              if (fl.leader_ship_id != kInvalidId) {
                if (const auto* sh = find_ptr(s.ships, fl.leader_ship_id)) return sh->system_id;
              }
              for (Id sid : fl.ship_ids) {
                if (const auto* sh = find_ptr(s.ships, sid)) return sh->system_id;
              }
              return kInvalidId;
            };

            for (Id fid2 : fleet_ids) {
              const auto* fl = find_ptr(s.fleets, fid2);
              if (!fl) continue;

              std::string label = fl->name.empty()
                                     ? ("Fleet #" + std::to_string(static_cast<unsigned long long>(fid2)))
                                     : fl->name;
              label += "##fleet_pick_" + std::to_string(static_cast<unsigned long long>(fid2));

              const Id fs = fleet_system(*fl);
              const bool sys_ok = (colony_sys_id == kInvalidId || fs == kInvalidId || fs == colony_sys_id);
              if (!sys_ok) ImGui::BeginDisabled();

              const bool selected = (bo_mut.assign_to_fleet_id == fid2);
              if (ImGui::MenuItem(label.c_str(), nullptr, selected)) bo_mut.assign_to_fleet_id = fid2;

              if (!sys_ok) {
                if (ImGui::IsItemHovered()) {
                  ImGui::SetTooltip(
                      "Fleet appears to be in a different system; assignment is disabled for safety.");
                }
                ImGui::EndDisabled();
              }
            }
          }

          ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Rally to Colony")) {
          if (ImGui::MenuItem("<none>", nullptr, bo_mut.rally_to_colony_id == kInvalidId)) {
            bo_mut.rally_to_colony_id = kInvalidId;
          }

          std::vector<Id> colony_ids;
          colony_ids.reserve(s.colonies.size());
          for (const auto& [cid2, c2] : s.colonies) {
            if (c2.faction_id == colony->faction_id) colony_ids.push_back(cid2);
          }
          std::sort(colony_ids.begin(), colony_ids.end());

          if (colony_ids.empty()) {
            ImGui::TextDisabled("No colonies.");
          } else {
            for (Id cid2 : colony_ids) {
              const auto* c2 = find_ptr(s.colonies, cid2);
              if (!c2) continue;

              std::string label = c2->name.empty()
                                     ? ("Colony #" + std::to_string(static_cast<unsigned long long>(cid2)))
                                     : c2->name;
              label += "##rally_pick_" + std::to_string(static_cast<unsigned long long>(cid2));

              const bool selected = (bo_mut.rally_to_colony_id == cid2);
              if (ImGui::MenuItem(label.c_str(), nullptr, selected)) bo_mut.rally_to_colony_id = cid2;
            }
          }

          ImGui::EndMenu();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Note: Rally is skipped if fleet assignment succeeds.");
        if (ImGui::MenuItem("Clear all metadata")) {
          bo_mut.apply_ship_profile_name.clear();
          bo_mut.assign_to_fleet_id = kInvalidId;
          bo_mut.rally_to_colony_id = kInvalidId;
        }

        ImGui::EndPopup();
      }

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

          // Auto-shipyard (ship design targets) toggle.
          {
            bool auto_enabled = colony->shipyard_auto_build_enabled;
            if (ImGui::Checkbox("Auto-shipyard enabled (design targets)", &auto_enabled)) {
              colony->shipyard_auto_build_enabled = auto_enabled;
            }
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip(
                  "When enabled, this colony's shipyards may receive auto-queued build orders to satisfy Shipyard Targets.\n"
                  "Manual shipyard orders are never affected.");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Open Targets")) ui.show_shipyard_targets_window = true;
          }
          ImGui::Spacing();

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

      // Show attached post-build metadata as compact tags in the order label.
      // (Right-click the row to edit.)
      if (!bo.apply_ship_profile_name.empty()) {
        nm += " [P:" + bo.apply_ship_profile_name + "]";
      }
      if (bo.assign_to_fleet_id != kInvalidId) {
        std::string fl_nm =
            "Fleet #" + std::to_string(static_cast<unsigned long long>(bo.assign_to_fleet_id));
        if (const auto* fl = find_ptr(s.fleets, bo.assign_to_fleet_id)) {
          if (!fl->name.empty()) fl_nm = fl->name;
        }
        nm += " [F:" + fl_nm + "]";
      }
      if (bo.rally_to_colony_id != kInvalidId) {
        std::string rc_nm =
            "Colony #" + std::to_string(static_cast<unsigned long long>(bo.rally_to_colony_id));
        if (const auto* rc = find_ptr(s.colonies, bo.rally_to_colony_id)) {
          if (!rc->name.empty()) rc_nm = rc->name;
        }
        nm += " [R:" + rc_nm + "]";
      }


      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%d", i);

      ImGui::TableSetColumnIndex(1);
      const std::string row_id = "##shipyard_row_" + std::to_string(static_cast<unsigned long long>(i));
      ImGui::Selectable((nm + row_id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

      // Right-click: edit optional post-build metadata (profile / fleet / rally).
      if (ImGui::BeginPopupContextItem(("yard_ctx_" + std::to_string(static_cast<unsigned long long>(i))).c_str())) {
        auto& bo_mut = colony->shipyard_queue[static_cast<std::size_t>(i)];

        Id colony_sys_id = kInvalidId;
        if (const auto* cb = find_ptr(s.bodies, colony->body_id)) colony_sys_id = cb->system_id;

        ImGui::TextDisabled("Shipyard order metadata");
        ImGui::Separator();

        if (ImGui::BeginMenu("Apply Ship Profile")) {
          if (ImGui::MenuItem("<none>", nullptr, bo_mut.apply_ship_profile_name.empty())) {
            bo_mut.apply_ship_profile_name.clear();
          }

          if (const auto* fac = find_ptr(s.factions, colony->faction_id)) {
            std::vector<std::string> names;
            names.reserve(fac->ship_profiles.size());
            for (const auto& kv : fac->ship_profiles) names.push_back(kv.first);
            std::sort(names.begin(), names.end());

            if (names.empty()) {
              ImGui::TextDisabled("No ship profiles defined.");
            } else {
              for (const auto& nm2 : names) {
                const bool selected = (bo_mut.apply_ship_profile_name == nm2);
                if (ImGui::MenuItem(nm2.c_str(), nullptr, selected)) bo_mut.apply_ship_profile_name = nm2;
              }
            }
          } else {
            ImGui::TextDisabled("No faction / no profiles.");
          }

          ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Assign to Fleet")) {
          if (ImGui::MenuItem("<none>", nullptr, bo_mut.assign_to_fleet_id == kInvalidId)) {
            bo_mut.assign_to_fleet_id = kInvalidId;
          }

          std::vector<Id> fleet_ids;
          fleet_ids.reserve(s.fleets.size());
          for (const auto& [fid2, fl] : s.fleets) {
            if (fl.faction_id == colony->faction_id) fleet_ids.push_back(fid2);
          }
          std::sort(fleet_ids.begin(), fleet_ids.end());

          if (fleet_ids.empty()) {
            ImGui::TextDisabled("No fleets.");
          } else {
            auto fleet_system = [&](const Fleet& fl) -> Id {
              if (fl.leader_ship_id != kInvalidId) {
                if (const auto* sh = find_ptr(s.ships, fl.leader_ship_id)) return sh->system_id;
              }
              for (Id sid : fl.ship_ids) {
                if (const auto* sh = find_ptr(s.ships, sid)) return sh->system_id;
              }
              return kInvalidId;
            };

            for (Id fid2 : fleet_ids) {
              const auto* fl = find_ptr(s.fleets, fid2);
              if (!fl) continue;

              std::string label = fl->name.empty()
                                     ? ("Fleet #" + std::to_string(static_cast<unsigned long long>(fid2)))
                                     : fl->name;
              label += "##fleet_pick_" + std::to_string(static_cast<unsigned long long>(fid2));

              const Id fs = fleet_system(*fl);
              const bool sys_ok = (colony_sys_id == kInvalidId || fs == kInvalidId || fs == colony_sys_id);
              if (!sys_ok) ImGui::BeginDisabled();

              const bool selected = (bo_mut.assign_to_fleet_id == fid2);
              if (ImGui::MenuItem(label.c_str(), nullptr, selected)) bo_mut.assign_to_fleet_id = fid2;

              if (!sys_ok) {
                if (ImGui::IsItemHovered()) {
                  ImGui::SetTooltip(
                      "Fleet appears to be in a different system; assignment is disabled for safety.");
                }
                ImGui::EndDisabled();
              }
            }
          }

          ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Rally to Colony")) {
          if (ImGui::MenuItem("<none>", nullptr, bo_mut.rally_to_colony_id == kInvalidId)) {
            bo_mut.rally_to_colony_id = kInvalidId;
          }

          std::vector<Id> colony_ids;
          colony_ids.reserve(s.colonies.size());
          for (const auto& [cid2, c2] : s.colonies) {
            if (c2.faction_id == colony->faction_id) colony_ids.push_back(cid2);
          }
          std::sort(colony_ids.begin(), colony_ids.end());

          if (colony_ids.empty()) {
            ImGui::TextDisabled("No colonies.");
          } else {
            for (Id cid2 : colony_ids) {
              const auto* c2 = find_ptr(s.colonies, cid2);
              if (!c2) continue;

              std::string label = c2->name.empty()
                                     ? ("Colony #" + std::to_string(static_cast<unsigned long long>(cid2)))
                                     : c2->name;
              label += "##rally_pick_" + std::to_string(static_cast<unsigned long long>(cid2));

              const bool selected = (bo_mut.rally_to_colony_id == cid2);
              if (ImGui::MenuItem(label.c_str(), nullptr, selected)) bo_mut.rally_to_colony_id = cid2;
            }
          }

          ImGui::EndMenu();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Note: Rally is skipped if fleet assignment succeeds.");
        if (ImGui::MenuItem("Clear all metadata")) {
          bo_mut.apply_ship_profile_name.clear();
          bo_mut.assign_to_fleet_id = kInvalidId;
          bo_mut.rally_to_colony_id = kInvalidId;
        }

        ImGui::EndPopup();
      }

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

          // --- Procedural surface stamp (flavor) ---
          // Deterministic ASCII micro-maps for bodies to make exploration/colonization feel less abstract.
          if (ImGui::CollapsingHeader("Procedural surface", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID("procgen_surface");

            // Cache stamps per body id to avoid re-running the generator every frame.
            static std::unordered_map<Id, nebula4x::procgen_surface::Flavor> cache;
            auto it_flavor = cache.find(body_id);
            if (it_flavor == cache.end()) {
              it_flavor = cache.emplace(body_id, nebula4x::procgen_surface::flavor(*b, /*w=*/26, /*h=*/12)).first;
            }
            const nebula4x::procgen_surface::Flavor& f = it_flavor->second;

            ImGui::Text("Biome: %s", f.biome.c_str());

            if (!f.quirks.empty()) {
              ImGui::Text("Quirks:");
              for (const auto& q : f.quirks) {
                ImGui::BulletText("%s", q.name.c_str());
                if (ImGui::IsItemHovered() && !q.desc.empty()) {
                  ImGui::BeginTooltip();
                  ImGui::TextWrapped("%s", q.desc.c_str());
                  ImGui::EndTooltip();
                }
              }
            } else {
              ImGui::TextDisabled("No notable quirks detected.");
            }

            if (!f.legend.empty()) {
              ImGui::TextDisabled("%s", f.legend.c_str());
            }

            static int stamp_view_mode = 0; // 0 = pixel, 1 = ASCII
            ImGui::TextDisabled("Stamp:");
            ImGui::SameLine();
            ImGui::RadioButton("Pixel", &stamp_view_mode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("ASCII", &stamp_view_mode, 1);

            const float line_h = ImGui::GetTextLineHeightWithSpacing();
            // Stamp includes a border: (h + 2) lines, plus a little breathing room.
            const float stamp_h = line_h * 15.0f;
            if (ImGui::BeginChild("procgen_surface_stamp", ImVec2(0.0f, stamp_h), true, ImGuiWindowFlags_NoScrollbar)) {
              if (stamp_view_mode == 0) {
                const procgen_gfx::SurfaceStampGrid& g = procgen_gfx::cached_surface_stamp_grid(body_id, f.stamp);
                const procgen_gfx::SurfacePalette pal = procgen_gfx::palette_for_body(*b);

                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 sz = ImGui::GetContentRegionAvail();
                dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), IM_COL32(0, 0, 0, 70));
                procgen_gfx::draw_surface_stamp_pixels(dl, p0, sz, g, pal, 1.0f, true);
              } else {
                ImGui::TextUnformatted(f.stamp.c_str());
              }
            }
            ImGui::EndChild();

            if (ImGui::SmallButton("Copy stamp")) {
              ImGui::SetClipboardText(f.stamp.c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Refresh")) {
              cache.erase(body_id);
            }

            ImGui::PopID();
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
            "unpaid construction orders, troop training, and colony stockpile targets (set in Colony Details).");

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
            case LogisticsNeedKind::TroopTraining:
              reason = "Troop training";
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
            case LogisticsNeedKind::Rearm:
              reason = "Rearm";
              break;
            case LogisticsNeedKind::Maintenance:
              reason = "Maintenance";
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


        ImGui::SeparatorText("Blockades");
        if (!sim.cfg().enable_blockades) {
          ImGui::TextDisabled("Blockades are disabled in SimConfig.");
        } else {
          ImGui::TextWrapped(
              "Blockade pressure is computed from hostile presence near a colony body and reduces certain colony outputs "
              "(repairs, troop training, and terraforming). Use the Fleet Mission 'Blockade colony' to apply pressure.");

          // --- Owned colonies under blockade ---
          {
            struct BlockadeRow {
              Id colony_id{kInvalidId};
              Id body_id{kInvalidId};
              Id system_id{kInvalidId};
              BlockadeStatus bs{};
            };

            std::vector<BlockadeRow> owned;
            owned.reserve(s.colonies.size());
            for (const auto& [cid, c] : s.colonies) {
              if (c.faction_id != selected_faction_id) continue;
              const auto bs = sim.blockade_status_for_colony(cid);
              if (bs.pressure <= 1e-6) continue;
              Id sys_id = kInvalidId;
              if (const auto* body = find_ptr(s.bodies, c.body_id)) sys_id = body->system_id;
              owned.push_back(BlockadeRow{cid, c.body_id, sys_id, bs});
            }
            std::sort(owned.begin(), owned.end(), [](const BlockadeRow& a, const BlockadeRow& b) {
              if (a.bs.pressure != b.bs.pressure) return a.bs.pressure > b.bs.pressure;
              return a.colony_id < b.colony_id;
            });

            ImGui::Text("Owned colonies under blockade");
            if (owned.empty()) {
              ImGui::TextDisabled("None.");
            } else if (ImGui::BeginTable("##blockades_owned", 7,
                                         ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
              ImGui::TableSetupColumn("Colony");
              ImGui::TableSetupColumn("System");
              ImGui::TableSetupColumn("Output");
              ImGui::TableSetupColumn("Pressure");
              ImGui::TableSetupColumn("Hostiles");
              ImGui::TableSetupColumn("Defenders");
              ImGui::TableSetupColumn("Go");
              ImGui::TableHeadersRow();

              for (const auto& r : owned) {
                const Colony* c = find_ptr(s.colonies, r.colony_id);
                const StarSystem* sys = (r.system_id != kInvalidId) ? find_ptr(s.systems, r.system_id) : nullptr;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(c ? c->name.c_str() : "?");

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(sys ? sys->name.c_str() : "?");

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.0f%%", r.bs.output_multiplier * 100.0);

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f", r.bs.pressure);

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%d / %.1f", r.bs.hostile_ships, r.bs.hostile_power);

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%d / %.1f", r.bs.defender_ships, r.bs.defender_power);

                ImGui::TableSetColumnIndex(6);
                const std::string btn = "Focus##blk_own_" + std::to_string(static_cast<unsigned long long>(r.colony_id));
                if (ImGui::SmallButton(btn.c_str())) {
                  selected_ship = kInvalidId;
                  selected_colony = r.colony_id;
                  selected_body = r.body_id;
                  if (r.system_id != kInvalidId) s.selected_system = r.system_id;
                  ui.request_details_tab = DetailsTab::Colony;
                  ui.request_focus_faction_id = selected_faction_id;
                  ui.show_map_window = true;
                  ui.request_map_tab = MapTab::System;
                }
              }
              ImGui::EndTable();
            }
          }

          ImGui::Spacing();
          ImGui::Separator();

          // --- Active blockade missions ---
          {
            struct MissionRow {
              Id fleet_id{kInvalidId};
              Id target_colony_id{kInvalidId};
              Id target_body_id{kInvalidId};
              Id system_id{kInvalidId};
              double dist_mkm{-1.0};
              double eff_radius_mkm{0.0};
              bool in_range{false};
              BlockadeStatus bs{};
            };

            std::vector<MissionRow> missions;
            missions.reserve(s.fleets.size());
            for (const auto& [fid, fl] : s.fleets) {
              if (fl.faction_id != selected_faction_id) continue;
              if (fl.mission.type != FleetMissionType::BlockadeColony) continue;
              if (fl.mission.blockade_colony_id == kInvalidId) continue;

              const Colony* c = find_ptr(s.colonies, fl.mission.blockade_colony_id);
              if (!c) continue;
              const Body* body = find_ptr(s.bodies, c->body_id);
              if (!body) continue;

              const double def_rad = std::max(0.0, sim.cfg().blockade_radius_mkm);
              const double eff_rad = (fl.mission.blockade_radius_mkm > 0.0) ? fl.mission.blockade_radius_mkm : def_rad;

              double dist = -1.0;
              if (const auto* lead = find_ptr(s.ships, fl.leader_ship_id)) {
                if (lead->system_id == body->system_id) {
                  dist = (lead->position_mkm - body->position_mkm).length();
                }
              }

              const bool in_rng = (dist >= 0.0) && (dist <= eff_rad + 1e-6);
              const auto bs = sim.blockade_status_for_colony(c->id);
              missions.push_back(MissionRow{fid, c->id, c->body_id, body->system_id, dist, eff_rad, in_rng, bs});
            }
            std::sort(missions.begin(), missions.end(), [](const MissionRow& a, const MissionRow& b) {
              if (a.bs.pressure != b.bs.pressure) return a.bs.pressure > b.bs.pressure;
              return a.fleet_id < b.fleet_id;
            });

            ImGui::Text("Active blockade missions");
            if (missions.empty()) {
              ImGui::TextDisabled("None.");
            } else if (ImGui::BeginTable("##blockades_missions", 8,
                                         ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
              ImGui::TableSetupColumn("Fleet");
              ImGui::TableSetupColumn("Target");
              ImGui::TableSetupColumn("System");
              ImGui::TableSetupColumn("Dist");
              ImGui::TableSetupColumn("In range");
              ImGui::TableSetupColumn("Pressure");
              ImGui::TableSetupColumn("Output");
              ImGui::TableSetupColumn("Go");
              ImGui::TableHeadersRow();

              for (const auto& r : missions) {
                const Fleet* fl = find_ptr(s.fleets, r.fleet_id);
                const Colony* c = find_ptr(s.colonies, r.target_colony_id);
                const StarSystem* sys = (r.system_id != kInvalidId) ? find_ptr(s.systems, r.system_id) : nullptr;

                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(fl ? fl->name.c_str() : "?");

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(c ? c->name.c_str() : "?");

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(sys ? sys->name.c_str() : "?");

                ImGui::TableSetColumnIndex(3);
                if (r.dist_mkm >= 0.0) {
                  ImGui::Text("%.2f", r.dist_mkm);
                } else {
                  ImGui::TextUnformatted("-");
                }

                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(r.in_range ? "yes" : "no");

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%.2f", r.bs.pressure);

                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%.0f%%", r.bs.output_multiplier * 100.0);

                ImGui::TableSetColumnIndex(7);
                const std::string b1 = "Fleet##blk_fleet_" + std::to_string(static_cast<unsigned long long>(r.fleet_id));
                if (ImGui::SmallButton(b1.c_str())) {
                  ui.selected_fleet_id = r.fleet_id;
                  ui.request_details_tab = DetailsTab::Fleet;
                  ui.request_focus_faction_id = selected_faction_id;
                  if (sys) s.selected_system = sys->id;
                  ui.show_map_window = true;
                  ui.request_map_tab = MapTab::System;
                }
                ImGui::SameLine();
                const std::string b2 = "Target##blk_tgt_" + std::to_string(static_cast<unsigned long long>(r.target_colony_id));
                if (ImGui::SmallButton(b2.c_str())) {
                  selected_ship = kInvalidId;
                  selected_colony = r.target_colony_id;
                  selected_body = r.target_body_id;
                  if (sys) s.selected_system = sys->id;
                  ui.request_details_tab = DetailsTab::Colony;
                  ui.request_focus_faction_id = selected_faction_id;
                  ui.show_map_window = true;
                  ui.request_map_tab = MapTab::System;
                }
              }
              ImGui::EndTable();
            }
          }
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

        // --- Research forecast (best-effort) ---
        {
          ImGui::Separator();
          ImGui::Text("Forecast");

          const auto sched = nebula4x::estimate_research_schedule(sim, selected_faction->id);
          if (!sched.ok) {
            ImGui::TextDisabled("(cannot compute forecast)");
          } else {
            ImGui::Text("RP/day: %.1f  (base %.1f × mult %.2f)",
                        sched.effective_rp_per_day,
                        sched.base_rp_per_day,
                        sched.research_multiplier);

            if (sched.stalled) {
              ImGui::TextDisabled("Forecast stalled: %s", sched.stall_reason.c_str());
            }
            if (sched.truncated) {
              ImGui::TextDisabled("Forecast truncated: %s", sched.truncated_reason.c_str());
            }

            if (!sched.items.empty()) {
              const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
              if (ImGui::BeginTable("##research_forecast", 3, tf)) {
                ImGui::TableSetupColumn("Tech", ImGuiTableColumnFlags_WidthStretch, 0.60f);
                ImGui::TableSetupColumn("ETA (days)", ImGuiTableColumnFlags_WidthFixed, 0.18f);
                ImGui::TableSetupColumn("Complete on", ImGuiTableColumnFlags_WidthStretch, 0.22f);
                ImGui::TableHeadersRow();

                for (const auto& it : sched.items) {
                  const auto ti = sim.content().techs.find(it.tech_id);
                  const std::string nm = (ti == sim.content().techs.end()) ? it.tech_id : ti->second.name;

                  std::string label = nm;
                  if (it.was_active_at_start) label = "[A] " + label;

                  ImGui::TableNextRow();

                  ImGui::TableSetColumnIndex(0);
                  ImGui::TextUnformatted(label.c_str());

                  ImGui::TableSetColumnIndex(1);
                  ImGui::Text("D+%d", std::max(0, it.end_day));

                  ImGui::TableSetColumnIndex(2);
                  const std::string date = sim.state().date.add_days(static_cast<std::int64_t>(it.end_day)).to_string();
                  ImGui::TextUnformatted(date.c_str());
                }

                ImGui::EndTable();
              }
            } else if (!sched.stalled) {
              ImGui::TextDisabled("(no active research / queue)");
            }
          }
        }
        // --- Reverse engineering ---
        {
          ImGui::Separator();
          ImGui::Text("Reverse engineering");

          if (selected_faction->reverse_engineering_progress.empty()) {
            ImGui::TextDisabled("(no active reverse engineering)");
          } else {
            ImGui::TextDisabled(
                "Progress is earned automatically when salvaging foreign ship wrecks and\n"
                "can also be discovered as schematic fragments when resolving anomalies.\n"
                "Once a component reaches 100%, it is added to your unlocked component list.");

            std::vector<std::string> cids;
            cids.reserve(selected_faction->reverse_engineering_progress.size());
            for (const auto& [cid, _] : selected_faction->reverse_engineering_progress) {
              if (!cid.empty()) cids.push_back(cid);
            }
            std::sort(cids.begin(), cids.end());
            cids.erase(std::unique(cids.begin(), cids.end()), cids.end());

            const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##reverse_engineering", 3, tf)) {
              ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthStretch, 0.55f);
              ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch, 0.30f);
              ImGui::TableSetupColumn("Points", ImGuiTableColumnFlags_WidthStretch, 0.15f);
              ImGui::TableHeadersRow();

              for (const auto& cid : cids) {
                const auto it = selected_faction->reverse_engineering_progress.find(cid);
                if (it == selected_faction->reverse_engineering_progress.end()) continue;
                const double pts = it->second;
                const double req = sim.reverse_engineering_points_required_for_component(cid);
                const float frac = (req > 0.0) ? static_cast<float>(std::clamp(pts / req, 0.0, 1.0)) : 0.0f;

                const auto itc = sim.content().components.find(cid);
                const std::string name = (itc == sim.content().components.end()) ? cid : itc->second.name;

                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", name.c_str());
                if (ImGui::IsItemHovered()) {
                  ImGui::SetTooltip("%s", cid.c_str());
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::ProgressBar(frac, ImVec2(-1, 0));

                ImGui::TableSetColumnIndex(2);
                if (req > 0.0) {
                  ImGui::Text("%.1f/%.1f", pts, req);
                } else {
                  ImGui::TextDisabled("%.1f/?", pts);
                }
              }

              ImGui::EndTable();
            }
          }
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
            "Mutual Friendly stances enable full cooperation: allied sensor coverage + discovered systems are shared, "
            "and ships may refuel/repair/transfer minerals at allied colonies.\n\n"
            "Trade Agreements (treaties) grant port access for refuel/rearm/transfer minerals without a full alliance.\n\n"
            "Research Agreements exchange star charts and improve research output/collaboration without granting port access.");

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

        // --- Offers ---
        ImGui::SeparatorText("Diplomatic offers");
        {
          static std::string last_offer_error;

          const auto incoming = sim.incoming_diplomatic_offers(selected_faction_id);
          if (incoming.empty()) {
            ImGui::TextDisabled("No incoming offers.");
          } else {
            ImGui::Text("Incoming offers (%d)", (int)incoming.size());
            const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##dip_offers_in", 6, tflags)) {
              ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_None, 0.16f);
              ImGui::TableSetupColumn("Treaty", ImGuiTableColumnFlags_None, 0.18f);
              ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_None, 0.14f);
              ImGui::TableSetupColumn("Expires", ImGuiTableColumnFlags_None, 0.14f);
              ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_None, 0.26f);
              ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_None, 0.12f);
              ImGui::TableHeadersRow();

              const int now_day = static_cast<int>(s.date.days_since_epoch());
              for (const auto& o : incoming) {
                const Faction* from = find_ptr(s.factions, o.from_faction_id);
                const std::string from_name = from ? from->name : std::string("<unknown>");
                const std::string treaty_name = treaty_type_label(o.treaty_type);

                const bool indefinite = (o.treaty_duration_days < 0);
                // NOTE: use (std::max)(...) to avoid macro collisions on some platforms (e.g. Windows headers).
                const int expires_in = (o.expire_day < 0) ? -1 : (std::max)(0, o.expire_day - now_day);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(from_name.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(treaty_name.c_str());

                ImGui::TableSetColumnIndex(2);
                if (indefinite) {
                  ImGui::TextUnformatted("Indefinite");
                } else {
                  ImGui::Text("%d days", o.treaty_duration_days);
                }

                ImGui::TableSetColumnIndex(3);
                if (expires_in < 0) {
                  ImGui::TextUnformatted("Never");
                } else {
                  ImGui::Text("%d days", expires_in);
                }

                ImGui::TableSetColumnIndex(4);
                if (o.message.empty()) {
                  ImGui::TextDisabled("(no message)");
                } else {
                  ImGui::TextUnformatted(o.message.c_str());
                }

                ImGui::TableSetColumnIndex(5);
                const std::string accept_id = "Accept##offer_" + std::to_string(static_cast<unsigned long long>(o.id));
                const std::string decline_id = "Decline##offer_" + std::to_string(static_cast<unsigned long long>(o.id));
                if (ImGui::SmallButton(accept_id.c_str())) {
                  std::string err;
                  if (!sim.accept_diplomatic_offer(o.id, /*push_event=*/true, &err)) {
                    last_offer_error = err.empty() ? "Failed to accept offer." : err;
                    ImGui::OpenPopup("Diplomacy error##offers");
                  }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(decline_id.c_str())) {
                  std::string err;
                  if (!sim.decline_diplomatic_offer(o.id, /*push_event=*/true, &err)) {
                    last_offer_error = err.empty() ? "Failed to decline offer." : err;
                    ImGui::OpenPopup("Diplomacy error##offers");
                  }
                }
              }

              ImGui::EndTable();
            }
          }

          // Outgoing offers (for visibility / debugging).
          {
            std::vector<DiplomaticOffer> outgoing;
            outgoing.reserve(s.diplomatic_offers.size());
            for (const auto& [oid, o] : s.diplomatic_offers) {
              (void)oid;
              if (o.from_faction_id == selected_faction_id) outgoing.push_back(o);
            }
            std::sort(outgoing.begin(), outgoing.end(), [](const DiplomaticOffer& a, const DiplomaticOffer& b) { return a.id < b.id; });

            if (!outgoing.empty()) {
              ImGui::Spacing();
              ImGui::Text("Outgoing offers (%d)", (int)outgoing.size());
              const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
              if (ImGui::BeginTable("##dip_offers_out", 5, tflags)) {
                ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_None, 0.18f);
                ImGui::TableSetupColumn("Treaty", ImGuiTableColumnFlags_None, 0.22f);
                ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_None, 0.16f);
                ImGui::TableSetupColumn("Expires", ImGuiTableColumnFlags_None, 0.16f);
                ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_None, 0.28f);
                ImGui::TableHeadersRow();

                const int now_day = static_cast<int>(s.date.days_since_epoch());
                for (const auto& o : outgoing) {
                  const Faction* to = find_ptr(s.factions, o.to_faction_id);
                  const std::string to_name = to ? to->name : std::string("<unknown>");
                  const bool indefinite = (o.treaty_duration_days < 0);
                  // NOTE: use (std::max)(...) to avoid macro collisions on some platforms (e.g. Windows headers).
                  const int expires_in = (o.expire_day < 0) ? -1 : (std::max)(0, o.expire_day - now_day);

                  ImGui::TableNextRow();
                  ImGui::TableSetColumnIndex(0);
                  ImGui::TextUnformatted(to_name.c_str());
                  ImGui::TableSetColumnIndex(1);
                  ImGui::TextUnformatted(treaty_type_label(o.treaty_type));
                  ImGui::TableSetColumnIndex(2);
                  if (indefinite) {
                    ImGui::TextUnformatted("Indefinite");
                  } else {
                    ImGui::Text("%d days", o.treaty_duration_days);
                  }
                  ImGui::TableSetColumnIndex(3);
                  if (expires_in < 0) {
                    ImGui::TextUnformatted("Never");
                  } else {
                    ImGui::Text("%d days", expires_in);
                  }
                  ImGui::TableSetColumnIndex(4);
                  if (o.message.empty()) {
                    ImGui::TextDisabled("(no message)");
                  } else {
                    ImGui::TextUnformatted(o.message.c_str());
                  }
                }

                ImGui::EndTable();
              }
            }
          }

          if (ImGui::BeginPopupModal("Diplomacy error##offers", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(last_offer_error.c_str());
            ImGui::Separator();
            if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
          }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Send offer");
        {
          static int offer_target_idx = 0;
          static int offer_type_idx = 0;
          static bool offer_indefinite = true;
          static int offer_treaty_days = 180;
          static bool offer_never_expires = false;
          static int offer_expires_days = 30;
          static char offer_message[256] = "";
          static std::string last_send_error;

          // Build a stable list of possible recipients (exclude self).
          std::vector<std::pair<Id, std::string>> others;
          others.reserve(factions.size());
          for (const auto& p : factions) {
            if (p.first == selected_faction_id) continue;
            others.push_back(p);
          }
          if (others.empty()) {
            ImGui::TextDisabled("No other factions to contact.");
          } else {
            offer_target_idx = std::clamp(offer_target_idx, 0, (int)others.size() - 1);

            std::vector<const char*> labels;
            labels.reserve(others.size());
            for (const auto& p : others) labels.push_back(p.second.c_str());
            ImGui::Combo("To", &offer_target_idx, labels.data(), (int)labels.size());

            const TreatyType offer_types[] = {TreatyType::Ceasefire, TreatyType::NonAggressionPact, TreatyType::TradeAgreement, TreatyType::ResearchAgreement, TreatyType::Alliance};
            const char* offer_type_labels[] = {"Ceasefire", "Non-Aggression Pact", "Trade Agreement", "Research Agreement", "Alliance"};
            offer_type_idx = std::clamp(offer_type_idx, 0, (int)IM_ARRAYSIZE(offer_types) - 1);
            ImGui::Combo("Treaty type", &offer_type_idx, offer_type_labels, IM_ARRAYSIZE(offer_type_labels));

            ImGui::Checkbox("Indefinite treaty", &offer_indefinite);
            if (!offer_indefinite) {
              offer_treaty_days = std::clamp(offer_treaty_days, 1, 36500);
              ImGui::InputInt("Treaty duration (days)", &offer_treaty_days);
              offer_treaty_days = std::clamp(offer_treaty_days, 1, 36500);
            }

            ImGui::Checkbox("Offer never expires", &offer_never_expires);
            if (!offer_never_expires) {
              offer_expires_days = std::clamp(offer_expires_days, 1, 36500);
              ImGui::InputInt("Offer expires in (days)", &offer_expires_days);
              offer_expires_days = std::clamp(offer_expires_days, 1, 36500);
            }

            ImGui::InputTextWithHint("Message", "optional note to include with the offer", offer_message, IM_ARRAYSIZE(offer_message));

            const Id to_id = others[(std::size_t)offer_target_idx].first;
            if (ImGui::Button("Send offer")) {
              std::string err;
              const TreatyType tt = offer_types[offer_type_idx];
              const int dur = offer_indefinite ? -1 : offer_treaty_days;
              const int exp = offer_never_expires ? 0 : offer_expires_days;
              const Id oid = sim.create_diplomatic_offer(selected_faction_id, to_id, tt, dur, exp, /*push_event=*/true, &err,
                                                        std::string(offer_message));
              if (oid == kInvalidId) {
                last_send_error = err.empty() ? "Failed to send offer." : err;
                ImGui::OpenPopup("Diplomacy error##send_offer");
              } else {
                // Keep message around, but show a small confirmation in logs.
                last_send_error.clear();
              }
            }
            if (ImGui::BeginPopupModal("Diplomacy error##send_offer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
              ImGui::TextUnformatted(last_send_error.c_str());
              ImGui::Separator();
              if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
              ImGui::EndPopup();
            }
          }
        }

        // --- Active treaties ---
        ImGui::Spacing();
        ImGui::SeparatorText("Active treaties");
        {
          std::vector<Treaty> mine;
          mine.reserve(s.treaties.size());
          for (const auto& [tid, t] : s.treaties) {
            (void)tid;
            if (t.faction_a == selected_faction_id || t.faction_b == selected_faction_id) mine.push_back(t);
          }
          std::sort(mine.begin(), mine.end(), [](const Treaty& a, const Treaty& b) { return a.id < b.id; });

          if (mine.empty()) {
            ImGui::TextDisabled("No active treaties involving this faction.");
          } else {
            static std::string last_treaty_error;
            const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##dip_treaties", 5, tflags)) {
              ImGui::TableSetupColumn("With", ImGuiTableColumnFlags_None, 0.22f);
              ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_None, 0.22f);
              ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_None, 0.18f);
              ImGui::TableSetupColumn("End", ImGuiTableColumnFlags_None, 0.18f);
              ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_None, 0.20f);
              ImGui::TableHeadersRow();

              const std::int64_t now_day = s.date.days_since_epoch();
              for (const auto& t : mine) {
                const Id other = (t.faction_a == selected_faction_id) ? t.faction_b : t.faction_a;
                const Faction* of = find_ptr(s.factions, other);
                const std::string other_name = of ? of->name : std::string("<unknown>");
                const bool indefinite = (t.duration_days < 0);
                const std::int64_t end_day = indefinite ? -1 : (t.start_day + static_cast<std::int64_t>(t.duration_days));
                const std::int64_t remaining = indefinite ? -1 : std::max<std::int64_t>(0, end_day - now_day);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(other_name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(treaty_type_label(t.type));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("Day %lld", static_cast<long long>(t.start_day));
                ImGui::TableSetColumnIndex(3);
                if (indefinite) {
                  ImGui::TextUnformatted("Indefinite");
                } else {
                  ImGui::Text("Day %lld (%lld left)", static_cast<long long>(end_day), static_cast<long long>(remaining));
                }
                ImGui::TableSetColumnIndex(4);
                const std::string cancel_id = "Cancel##treaty_" + std::to_string(static_cast<unsigned long long>(t.id));
                if (ImGui::SmallButton(cancel_id.c_str())) {
                  std::string err;
                  if (!sim.cancel_treaty(t.id, /*push_event=*/true, &err)) {
                    last_treaty_error = err.empty() ? "Failed to cancel treaty." : err;
                    ImGui::OpenPopup("Diplomacy error##treaty");
                  }
                }
              }

              ImGui::EndTable();
            }

            if (ImGui::BeginPopupModal("Diplomacy error##treaty", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
              ImGui::TextUnformatted(last_treaty_error.c_str());
              ImGui::Separator();
              if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
              ImGui::EndPopup();
            }
          }
        }

        // --- Stances ---
        ImGui::Spacing();
        ImGui::SeparatorText("Stances");
        {
          const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
          if (ImGui::BeginTable("##diplomacy_table", 3, tflags)) {
            ImGui::TableSetupColumn("Other faction", ImGuiTableColumnFlags_None, 0.36f);
            ImGui::TableSetupColumn("Your stance (base)", ImGuiTableColumnFlags_None, 0.38f);
            ImGui::TableSetupColumn("Their stance (effective)", ImGuiTableColumnFlags_None, 0.26f);
            ImGui::TableHeadersRow();

            const char* opts[] = {"Hostile", "Neutral", "Friendly"};

            for (const auto& [other_id, other_name] : factions) {
              if (other_id == selected_faction_id) continue;
              const DiplomacyStatus out_base = sim.diplomatic_status_base(selected_faction_id, other_id);
              const DiplomacyStatus out_eff = sim.diplomatic_status(selected_faction_id, other_id);
              const DiplomacyStatus in_eff = sim.diplomatic_status(other_id, selected_faction_id);

              // Precompute treaty list for tooltip/context.
              const auto ts = sim.treaties_between(selected_faction_id, other_id);
              bool has_treaty = !ts.empty();

              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted(other_name.c_str());
              if (has_treaty && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Active treaties:");
                for (const auto& t : ts) {
                  const bool indefinite = (t.duration_days < 0);
                  if (indefinite) {
                    ImGui::BulletText("%s (indefinite)", treaty_type_label(t.type));
                  } else {
                    const std::int64_t now_day = s.date.days_since_epoch();
                    const std::int64_t end_day = t.start_day + static_cast<std::int64_t>(t.duration_days);
                    const std::int64_t remaining = std::max<std::int64_t>(0, end_day - now_day);
                    ImGui::BulletText("%s (%lld days left)", treaty_type_label(t.type), static_cast<long long>(remaining));
                  }
                }
                ImGui::EndTooltip();
              }

              ImGui::TableSetColumnIndex(1);
              int combo_idx = diplomacy_status_to_combo_idx(out_base);
              const std::string combo_id = "##dip_" + std::to_string(static_cast<unsigned long long>(selected_faction_id)) +
                                           "_" + std::to_string(static_cast<unsigned long long>(other_id));
              if (ImGui::Combo(combo_id.c_str(), &combo_idx, opts, IM_ARRAYSIZE(opts))) {
                sim.set_diplomatic_status(selected_faction_id, other_id, diplomacy_status_from_combo_idx(combo_idx),
                                          reciprocal, /*push_event=*/true);
              }
              if (out_eff != out_base) {
                ImGui::SameLine();
                ImGui::TextDisabled("(effective: %s)", diplomacy_status_label(out_eff));
              }

              ImGui::TableSetColumnIndex(2);
              ImGui::TextUnformatted(diplomacy_status_label(in_eff));
            }

            ImGui::EndTable();
          }
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

        // --- Auto-forge helper state (local to the design editor) ---
        static bool edit_forge_use_constraints = false;
        static bool edit_forge_only_meeting_constraints = true;
        static bool edit_forge_require_power_balance = false;
        static int edit_forge_seed = 0;
        static int edit_forge_quality = 16;
        static int edit_forge_mutations = 5;
        static int edit_forge_max_components = 14;
        static bool edit_forge_prefer_missiles = false;
        static bool edit_forge_prefer_shields = true;
        static bool edit_forge_include_ecm_eccm = true;

        static float edit_forge_min_speed_km_s = 0.0f;
        static float edit_forge_min_range_mkm = 0.0f;
        static float edit_forge_max_mass_tons = 0.0f;
        static float edit_forge_min_cargo_tons = 0.0f;
        static float edit_forge_min_mining_tons_per_day = 0.0f;
        static float edit_forge_min_colony_capacity_millions = 0.0f;
        static float edit_forge_min_troop_capacity = 0.0f;
        static float edit_forge_min_sensor_range_mkm = 0.0f;
        static float edit_forge_max_signature_multiplier = 0.0f;
        static float edit_forge_min_beam_damage = 0.0f;
        static float edit_forge_min_missile_damage = 0.0f;
        static float edit_forge_min_point_defense_damage = 0.0f;
        static float edit_forge_min_shields = 0.0f;
        static float edit_forge_min_hp = 0.0f;
        static float edit_forge_min_ecm_strength = 0.0f;
        static float edit_forge_min_eccm_strength = 0.0f;
        static float edit_forge_min_power_margin = 0.0f;
        static std::string edit_forge_last_debug;

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

        // --- Auto forge (Design Forge) ---
        if (ImGui::CollapsingHeader("Auto forge (Design Forge)", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::TextDisabled("Generate variants from the current component list using only the selected faction's unlocked components.");

          ImGui::SetNextItemWidth(120.0f);
          ImGui::InputInt("Seed", &edit_forge_seed);
          ImGui::SameLine();
          ImGui::SetNextItemWidth(140.0f);
          ImGui::SliderInt("Quality", &edit_forge_quality, 1, 64);
          ImGui::SameLine();
          ImGui::SetNextItemWidth(140.0f);
          ImGui::SliderInt("Mutations", &edit_forge_mutations, 0, 10);

          ImGui::SetNextItemWidth(140.0f);
          ImGui::SliderInt("Max components", &edit_forge_max_components, 6, 32);

          ImGui::Checkbox("Prefer missiles", &edit_forge_prefer_missiles);
          ImGui::SameLine();
          ImGui::Checkbox("Prefer shields", &edit_forge_prefer_shields);
          ImGui::SameLine();
          ImGui::Checkbox("Include ECM/ECCM", &edit_forge_include_ecm_eccm);

          ImGui::Spacing();
          ImGui::Checkbox("Constraints", &edit_forge_use_constraints);
          ImGui::SameLine();
          ImGui::Checkbox("Only valid outputs", &edit_forge_only_meeting_constraints);
          ImGui::SameLine();
          ImGui::Checkbox("Require power balance", &edit_forge_require_power_balance);

          if (edit_forge_use_constraints) {
            if (ImGui::BeginTable("design_editor_forge_constraints", 2, ImGuiTableFlags_SizingFixedFit)) {
              auto row_float = [&](const char* label, float* v) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(label);
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(160.0f);
                ImGui::InputFloat((std::string("##") + label).c_str(), v, 0.0f, 0.0f, "%.2f");
              };

              row_float("Min speed (km/s)", &edit_forge_min_speed_km_s);
              row_float("Min range (mkm)", &edit_forge_min_range_mkm);
              row_float("Max mass (t, 0=off)", &edit_forge_max_mass_tons);
              row_float("Min cargo (t)", &edit_forge_min_cargo_tons);
              row_float("Min mining (t/day)", &edit_forge_min_mining_tons_per_day);
              row_float("Min colony cap (M)", &edit_forge_min_colony_capacity_millions);
              row_float("Min troop cap", &edit_forge_min_troop_capacity);
              row_float("Min sensor range (mkm)", &edit_forge_min_sensor_range_mkm);
              row_float("Max signature mult (0=off)", &edit_forge_max_signature_multiplier);
              row_float("Min beam dmg", &edit_forge_min_beam_damage);
              row_float("Min missile dmg", &edit_forge_min_missile_damage);
              row_float("Min point-defense dmg", &edit_forge_min_point_defense_damage);
              row_float("Min shields", &edit_forge_min_shields);
              row_float("Min HP", &edit_forge_min_hp);
              row_float("Min ECM strength", &edit_forge_min_ecm_strength);
              row_float("Min ECCM strength", &edit_forge_min_eccm_strength);
              if (edit_forge_require_power_balance) {
                row_float("Min power margin", &edit_forge_min_power_margin);
              } else {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Min power margin");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("(enable Require power balance)");
              }
              ImGui::EndTable();
            }
            ImGui::TextDisabled("0 disables a constraint.");
          }

          if (ImGui::Button("Forge and apply best")) {
            if (!selected_faction) {
              edit_forge_last_debug = "No faction selected.";
            } else {
              ShipDesign base;
              base.id = new_id;
              base.name = new_name;
              base.role = (role_idx == 0) ? ShipRole::Freighter : (role_idx == 1) ? ShipRole::Surveyor : ShipRole::Combatant;
              base.components = comp_list;

              DesignForgeOptions opt;
              opt.desired_count = 16;
              opt.candidate_multiplier = std::clamp(edit_forge_quality, 1, 64);
              opt.mutations_per_candidate = std::clamp(edit_forge_mutations, 0, 10);
              opt.role = base.role;
              opt.prefer_missiles = edit_forge_prefer_missiles;
              opt.prefer_shields = edit_forge_prefer_shields;
              opt.include_ecm_eccm = edit_forge_include_ecm_eccm;
              opt.id_prefix = "tmp";
              opt.name_prefix = "Forge";
              opt.max_components = std::clamp(edit_forge_max_components, 6, 64);

              if (edit_forge_use_constraints) {
                opt.constraints.min_speed_km_s = edit_forge_min_speed_km_s;
                opt.constraints.min_range_mkm = edit_forge_min_range_mkm;
                opt.constraints.max_mass_tons = edit_forge_max_mass_tons;
                opt.constraints.min_cargo_tons = edit_forge_min_cargo_tons;
                opt.constraints.min_mining_tons_per_day = edit_forge_min_mining_tons_per_day;
                opt.constraints.min_colony_capacity_millions = edit_forge_min_colony_capacity_millions;
                opt.constraints.min_troop_capacity = edit_forge_min_troop_capacity;
                opt.constraints.min_sensor_range_mkm = edit_forge_min_sensor_range_mkm;
                opt.constraints.max_signature_multiplier = edit_forge_max_signature_multiplier;
                opt.constraints.min_beam_damage = edit_forge_min_beam_damage;
                opt.constraints.min_missile_damage = edit_forge_min_missile_damage;
                opt.constraints.min_point_defense_damage = edit_forge_min_point_defense_damage;
                opt.constraints.min_shields = edit_forge_min_shields;
                opt.constraints.min_hp = edit_forge_min_hp;
                opt.constraints.min_ecm_strength = edit_forge_min_ecm_strength;
                opt.constraints.min_eccm_strength = edit_forge_min_eccm_strength;
                opt.constraints.require_power_balance = edit_forge_require_power_balance;
                opt.constraints.min_power_margin = edit_forge_min_power_margin;
                opt.only_meeting_constraints = edit_forge_only_meeting_constraints;
              }

              // Deterministic-ish seed based on user input and design id.
              std::uint64_t seed = static_cast<std::uint64_t>(static_cast<std::uint32_t>(edit_forge_seed));
              seed ^= static_cast<std::uint64_t>(selected_faction_id) * 0x9E3779B185EBCA87ull;
              seed ^= procgen_obscure::fnv1a_64(base.id);
              seed = procgen_obscure::splitmix64(seed);

              std::string dbg;
              auto forged = forge_design_variants(sim.content(), selected_faction->unlocked_components, base, seed, opt, &dbg);
              edit_forge_last_debug = dbg;

              if (forged.empty()) {
                status = "Forge produced no designs.";
              } else {
                const ForgedDesign* best = &forged.front();
                for (const auto& fd : forged) {
                  if (fd.meets_constraints) {
                    best = &fd;
                    break;
                  }
                }
                comp_list = best->design.components;
                status = std::string("Forge applied best variant. ") + (best->meets_constraints ? "(meets constraints)" : "(constraints unmet)");
              }
            }
          }
          if (!edit_forge_last_debug.empty()) {
            ImGui::TextDisabled("%s", edit_forge_last_debug.c_str());
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

    // --- Journal tab (curated narrative) ---
    {
      if (ImGui::BeginTabItem("Journal", nullptr, flags_for(DetailsTab::Journal))) {
        Id viewer_faction_id = selected_faction_id;
        if (selected_ship != kInvalidId) {
          if (const auto* sh = find_ptr(s.ships, selected_ship)) viewer_faction_id = sh->faction_id;
        }

        Faction* viewer = (viewer_faction_id == kInvalidId) ? nullptr : find_ptr(s.factions, viewer_faction_id);
        if (!viewer) {
          ImGui::TextDisabled("No faction selected");
          ImGui::EndTabItem();
        } else {
          ImGui::Text("Journal (saved with game)");
          ImGui::TextDisabled("Entries: %d", (int)viewer->journal.size());

          static int category_idx = 0; // 0=All
          static int max_show = 250;
          static char search_buf[128] = "";

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
          ImGui::InputText("Search", search_buf, IM_ARRAYSIZE(search_buf));
          ImGui::InputInt("Show last N", &max_show);
          max_show = std::clamp(max_show, 10, 5000);

          // Collect visible indices (newest-first) based on filters + limit.
          std::vector<int> rows;
          rows.reserve(std::min(max_show, static_cast<int>(viewer->journal.size())));
          for (int i = (int)viewer->journal.size() - 1; i >= 0 && (int)rows.size() < max_show; --i) {
            const auto& je = viewer->journal[(std::size_t)i];
            if (!case_insensitive_contains(je.title + " " + je.text, search_buf)) continue;

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
              if (je.category != cat_vals[idx]) continue;
            }

            rows.push_back(i);
          }

          if (ImGui::SmallButton("Copy visible")) {
            std::string out;
            out.reserve(rows.size() * 96);
            for (int idx : rows) {
              const auto& je = viewer->journal[(std::size_t)idx];
              const nebula4x::Date d(je.day);
              const std::string dt = format_datetime(d, je.hour);
              out += std::string("[") + dt + "] #" +
                     std::to_string((unsigned long long)je.seq) +
                     " [" + event_category_label(je.category) + "] " + je.title;
              out += "\n";
              if (!je.text.empty()) {
                out += je.text;
                out += "\n";
              }
              out += "\n";
            }
            ImGui::SetClipboardText(out.c_str());
          }

          ImGui::Separator();

          if (rows.empty()) {
            ImGui::TextDisabled("(no matching entries)");
          } else {
            for (int i : rows) {
              const auto& je = viewer->journal[(std::size_t)i];
              const nebula4x::Date d(je.day);
              const std::string dt = format_datetime(d, je.hour);

              const std::string header = std::string("[") + dt + "] #" +
                                         std::to_string((unsigned long long)je.seq) +
                                         " [" + event_category_label(je.category) + "] " + je.title;

              if (ImGui::TreeNode((header + "##journal_" + std::to_string((unsigned long long)je.seq)).c_str())) {
                if (!je.text.empty()) ImGui::TextWrapped("%s", je.text.c_str());

                // Navigation shortcuts.
                if (je.system_id != kInvalidId) {
                  if (ImGui::SmallButton("View system")) s.selected_system = je.system_id;
                }
                if (je.ship_id != kInvalidId) {
                  ImGui::SameLine();
                  if (ImGui::SmallButton("Select ship")) {
                    selected_ship = je.ship_id;
                    ui.selected_fleet_id = sim.fleet_for_ship(je.ship_id);
                    if (const auto* sh = find_ptr(s.ships, je.ship_id)) s.selected_system = sh->system_id;
                  }
                }
                if (je.colony_id != kInvalidId) {
                  ImGui::SameLine();
                  if (ImGui::SmallButton("Select colony")) {
                    selected_colony = je.colony_id;
                    if (const auto* c = find_ptr(s.colonies, je.colony_id)) {
                      selected_body = c->body_id;
                      if (const auto* b = find_ptr(s.bodies, c->body_id)) s.selected_system = b->system_id;
                    }
                  }
                }
                if (je.anomaly_id != kInvalidId) {
                  ImGui::SameLine();
                  if (ImGui::SmallButton("Center anomaly")) {
                    if (const auto* a = find_ptr(s.anomalies, je.anomaly_id)) {
                      s.selected_system = a->system_id;
                      ui.request_map_tab = MapTab::System;
                      ui.request_system_map_center = true;
                      ui.request_system_map_center_system_id = a->system_id;
                      ui.request_system_map_center_x_mkm = a->position_mkm.x;
                      ui.request_system_map_center_y_mkm = a->position_mkm.y;
                      ui.request_system_map_center_zoom = 0.0;
                    }
                  }
                }
                if (je.wreck_id != kInvalidId) {
                  ImGui::SameLine();
                  if (ImGui::SmallButton("Center wreck")) {
                    if (const auto* w = find_ptr(s.wrecks, je.wreck_id)) {
                      s.selected_system = w->system_id;
                      ui.request_map_tab = MapTab::System;
                      ui.request_system_map_center = true;
                      ui.request_system_map_center_system_id = w->system_id;
                      ui.request_system_map_center_x_mkm = w->position_mkm.x;
                      ui.request_system_map_center_y_mkm = w->position_mkm.y;
                      ui.request_system_map_center_zoom = 0.0;
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
  ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Settings", &ui.show_settings_window)) {
    ImGui::End();
    return;
  }

  // Narration: announce the settings window when it gains focus.
  ScreenReader::instance().observe_window("Settings");

  if (!ImGui::BeginTabBar("settings_tabs")) {
    ImGui::End();
    return;
  }

  // --- Theme tab ---
  if (ImGui::BeginTabItem("Theme")) {
    ImGui::SeparatorText("UI style");
    {
      const char* presets[] = {"Dark (default)", "Light", "Classic", "Nebula", "High Contrast", "Procedural"};
      ui.ui_style_preset = std::clamp(ui.ui_style_preset, 0, (int)IM_ARRAYSIZE(presets) - 1);
      ImGui::Combo("Preset##ui_style", &ui.ui_style_preset, presets, IM_ARRAYSIZE(presets));
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Select a built-in UI style preset.\n\n- Nebula: dark theme with cyan accents\n- High Contrast: stronger selection/focus highlighting\n- Procedural: seeded palette you can tweak/share");
      }
    }

    {
      const char* density[] = {"Comfortable", "Compact", "Spacious"};
      ui.ui_density = std::clamp(ui.ui_density, 0, (int)IM_ARRAYSIZE(density) - 1);
      ImGui::Combo("Density##ui_density", &ui.ui_density, density, IM_ARRAYSIZE(density));
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Controls padding/spacing sizing. Compact is useful for data-heavy windows.");
      }
    }

    ImGui::Checkbox("Scale padding/spacing with UI scale", &ui.ui_scale_style);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("When enabled, UI scale affects both font size and widget spacing.");
    }

    // Procedural theme controls (preset 5).
    if (ui.ui_style_preset == 5) {
      ImGui::SeparatorText("Procedural theme");

      ImGui::TextWrapped(
          "Generates a cohesive accent palette from a small set of parameters. "
          "Use the copy/paste buttons to share theme DNA with other players.");

      ImGui::InputInt("Seed##proc_theme", &ui.ui_procedural_theme_seed);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Seed controls the generated palette when 'Use seed for hue' is enabled.");
      }

      ImGui::SameLine();
      if (ImGui::SmallButton("Randomize##proc_theme_seed")) {
        // Mix the existing seed with time to get a new seed (good enough for UI cosmetics).
        std::uint32_t x = static_cast<std::uint32_t>(ui.ui_procedural_theme_seed);
        x ^= static_cast<std::uint32_t>(ImGui::GetTime() * 1000.0);
        x ^= 0x9e3779b9u;
        x = x * 0x85ebca6bu + 0xc2b2ae35u;
        ui.ui_procedural_theme_seed = static_cast<int>(x);
      }

      ImGui::SameLine();
      ImGui::Checkbox("Use seed for hue", &ui.ui_procedural_theme_use_seed_hue);
      if (!ui.ui_procedural_theme_use_seed_hue) {
        ImGui::SliderFloat("Hue (deg)", &ui.ui_procedural_theme_hue_deg, 0.0f, 360.0f, "%.0f");
        ui.ui_procedural_theme_hue_deg = std::clamp(ui.ui_procedural_theme_hue_deg, 0.0f, 360.0f);
      }

      {
        const char* variants[] = {"Analogous", "Complementary", "Triad", "Monochrome"};
        ui.ui_procedural_theme_variant = std::clamp(ui.ui_procedural_theme_variant, 0, (int)IM_ARRAYSIZE(variants) - 1);
        ImGui::Combo("Palette", &ui.ui_procedural_theme_variant, variants, IM_ARRAYSIZE(variants));
      }

      ImGui::SliderFloat("Accent saturation", &ui.ui_procedural_theme_saturation, 0.0f, 1.0f, "%.2f");
      ui.ui_procedural_theme_saturation = std::clamp(ui.ui_procedural_theme_saturation, 0.0f, 1.0f);
      ImGui::SliderFloat("Accent value", &ui.ui_procedural_theme_value, 0.0f, 1.0f, "%.2f");
      ui.ui_procedural_theme_value = std::clamp(ui.ui_procedural_theme_value, 0.0f, 1.0f);
      ImGui::SliderFloat("Background value", &ui.ui_procedural_theme_bg_value, 0.0f, 0.25f, "%.2f");
      ui.ui_procedural_theme_bg_value = std::clamp(ui.ui_procedural_theme_bg_value, 0.0f, 1.0f);
      ImGui::SliderFloat("Accent strength", &ui.ui_procedural_theme_accent_strength, 0.0f, 1.0f, "%.2f");
      ui.ui_procedural_theme_accent_strength = std::clamp(ui.ui_procedural_theme_accent_strength, 0.0f, 1.0f);

      ImGui::Checkbox("Animate hue", &ui.ui_procedural_theme_animate_hue);
      if (ui.ui_procedural_theme_animate_hue) {
        ImGui::SameLine();
        ImGui::SliderFloat(
            "Speed (deg/sec)", &ui.ui_procedural_theme_animate_speed_deg_per_sec, 0.0f, 60.0f, "%.1f");
        ui.ui_procedural_theme_animate_speed_deg_per_sec =
            std::clamp(ui.ui_procedural_theme_animate_speed_deg_per_sec, 0.0f, 180.0f);
      }

      ImGui::Checkbox("Sync map backgrounds", &ui.ui_procedural_theme_sync_backgrounds);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "When enabled, the procedural theme also drives the renderer clear color and map backgrounds.");
      }

      // Preview + share.
      {
        ProceduralThemeParams p;
        p.seed = ui.ui_procedural_theme_seed;
        p.use_seed_hue = ui.ui_procedural_theme_use_seed_hue;
        p.hue_deg = ui.ui_procedural_theme_hue_deg;
        p.variant = ui.ui_procedural_theme_variant;
        p.saturation = ui.ui_procedural_theme_saturation;
        p.value = ui.ui_procedural_theme_value;
        p.bg_value = ui.ui_procedural_theme_bg_value;
        p.accent_strength = ui.ui_procedural_theme_accent_strength;
        p.animate_hue = ui.ui_procedural_theme_animate_hue;
        p.animate_speed_deg_per_sec = ui.ui_procedural_theme_animate_speed_deg_per_sec;
        p.sync_backgrounds = ui.ui_procedural_theme_sync_backgrounds;

        const auto pal = compute_procedural_theme_palette(p, static_cast<float>(ImGui::GetTime()));

        ImGui::TextDisabled("Preview:");
        ImGui::SameLine();
        ImGui::ColorButton("##proc_acc1", pal.accent_primary, ImGuiColorEditFlags_NoTooltip, ImVec2(28, 18));
        ImGui::SameLine();
        ImGui::ColorButton("##proc_acc2", pal.accent_secondary, ImGuiColorEditFlags_NoTooltip, ImVec2(28, 18));
        ImGui::SameLine();
        ImGui::ColorButton("##proc_bg", pal.bg_window, ImGuiColorEditFlags_NoTooltip, ImVec2(28, 18));

        const std::string theme_str =
            std::string("nebula-theme-v1") +
            " seed=" + std::to_string(p.seed) +
            " use_seed_hue=" + std::to_string(p.use_seed_hue ? 1 : 0) +
            " hue=" + std::to_string((int)std::round(p.hue_deg)) +
            " variant=" + std::to_string(p.variant) +
            " sat=" + std::to_string(p.saturation) +
            " val=" + std::to_string(p.value) +
            " bg=" + std::to_string(p.bg_value) +
            " strength=" + std::to_string(p.accent_strength) +
            " anim=" + std::to_string(p.animate_hue ? 1 : 0) +
            " speed=" + std::to_string(p.animate_speed_deg_per_sec) +
            " sync_bg=" + std::to_string(p.sync_backgrounds ? 1 : 0);

        if (ImGui::SmallButton("Copy theme string")) {
          ImGui::SetClipboardText(theme_str.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Paste theme string")) {
          if (const char* clip = ImGui::GetClipboardText()) {
	            std::istringstream iss{std::string(clip)};
            std::string tok;
            while (iss >> tok) {
              const auto eq = tok.find('=');
              if (eq == std::string::npos) continue;
              const std::string key = tok.substr(0, eq);
              const std::string v = tok.substr(eq + 1);
              try {
                if (key == "seed") ui.ui_procedural_theme_seed = std::stoi(v);
                else if (key == "use_seed_hue") ui.ui_procedural_theme_use_seed_hue = (std::stoi(v) != 0);
                else if (key == "hue") ui.ui_procedural_theme_hue_deg = std::stof(v);
                else if (key == "variant") ui.ui_procedural_theme_variant = std::stoi(v);
                else if (key == "sat") ui.ui_procedural_theme_saturation = std::stof(v);
                else if (key == "val") ui.ui_procedural_theme_value = std::stof(v);
                else if (key == "bg") ui.ui_procedural_theme_bg_value = std::stof(v);
                else if (key == "strength") ui.ui_procedural_theme_accent_strength = std::stof(v);
                else if (key == "anim") ui.ui_procedural_theme_animate_hue = (std::stoi(v) != 0);
                else if (key == "speed") ui.ui_procedural_theme_animate_speed_deg_per_sec = std::stof(v);
                else if (key == "sync_bg") ui.ui_procedural_theme_sync_backgrounds = (std::stoi(v) != 0);
              } catch (...) {
                // ignore parse errors
              }
            }

            // Clamp after paste.
            ui.ui_procedural_theme_hue_deg = std::clamp(ui.ui_procedural_theme_hue_deg, 0.0f, 360.0f);
            ui.ui_procedural_theme_variant = std::clamp(ui.ui_procedural_theme_variant, 0, 3);
            ui.ui_procedural_theme_saturation = std::clamp(ui.ui_procedural_theme_saturation, 0.0f, 1.0f);
            ui.ui_procedural_theme_value = std::clamp(ui.ui_procedural_theme_value, 0.0f, 1.0f);
            ui.ui_procedural_theme_bg_value = std::clamp(ui.ui_procedural_theme_bg_value, 0.0f, 1.0f);
            ui.ui_procedural_theme_accent_strength = std::clamp(ui.ui_procedural_theme_accent_strength, 0.0f, 1.0f);
            ui.ui_procedural_theme_animate_speed_deg_per_sec =
                std::clamp(ui.ui_procedural_theme_animate_speed_deg_per_sec, 0.0f, 180.0f);
          }
        }
      }

      if (ui.override_window_bg) {
        ImGui::TextDisabled("Note: 'Override window background' overrides the procedural window tint.");
      }
    }

    ImGui::SeparatorText("Backgrounds");

    const bool procedural_bg_lock = (ui.ui_style_preset == 5 && ui.ui_procedural_theme_sync_backgrounds);
    if (procedural_bg_lock) ImGui::BeginDisabled();
    ImGui::ColorEdit4("Clear background", ui.clear_color);
    ImGui::ColorEdit4("System map background", ui.system_map_bg);
    ImGui::ColorEdit4("Galaxy map background", ui.galaxy_map_bg);
    ImGui::Checkbox("Override window background", &ui.override_window_bg);
    if (ui.override_window_bg) {
      ImGui::ColorEdit4("Window background", ui.window_bg);
    }
    if (procedural_bg_lock) {
      ImGui::EndDisabled();
      ImGui::TextDisabled("Background colors are driven by the procedural theme while 'Sync map backgrounds' is enabled.");
    }

    if (ImGui::Button("Reset theme defaults")) {
      actions.reset_ui_theme = true;
    }

    ImGui::EndTabItem();
  }

  // --- Map tab ---
  if (ImGui::BeginTabItem("Map")) {
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

    ImGui::Checkbox("System: nebula microfield overlay", &ui.system_map_nebula_microfield_overlay);
    if (ui.system_map_nebula_microfield_overlay) {
      ImGui::Indent();
      ImGui::SliderFloat("Nebula overlay opacity", &ui.system_map_nebula_overlay_opacity, 0.0f, 1.0f, "%.2f");
      ui.system_map_nebula_overlay_opacity = std::clamp(ui.system_map_nebula_overlay_opacity, 0.0f, 1.0f);
      ImGui::SliderInt("Nebula overlay resolution", &ui.system_map_nebula_overlay_resolution, 16, 260);
      ui.system_map_nebula_overlay_resolution = std::clamp(ui.system_map_nebula_overlay_resolution, 16, 260);
      ImGui::TextDisabled("Visualizes local nebula pockets/filaments that affect sensors and movement.");
      ImGui::Unindent();
    }

    ImGui::SeparatorText("System heatmaps");
    ImGui::Checkbox("System: sensor heatmap", &ui.system_map_sensor_heatmap);
    ImGui::SameLine();
    ImGui::Checkbox("Threat heatmap", &ui.system_map_threat_heatmap);

    ui.system_map_heatmap_opacity = std::clamp(ui.system_map_heatmap_opacity, 0.0f, 1.0f);
    ui.system_map_heatmap_resolution = std::clamp(ui.system_map_heatmap_resolution, 16, 200);
    ImGui::SliderFloat("Heatmap opacity", &ui.system_map_heatmap_opacity, 0.0f, 1.0f, "%.2f");
    ImGui::SliderInt("Heatmap resolution", &ui.system_map_heatmap_resolution, 16, 160);
    ImGui::TextDisabled("Shortcut: H (threat) / Shift+H (sensor)  —  also configurable in the system-map legend.");

    ImGui::Checkbox("Sensor: LOS raytrace heatmap (experimental)", &ui.system_map_sensor_heatmap_raytrace);
    if (ui.system_map_sensor_heatmap_raytrace) {
      ImGui::Indent();
      ui.system_map_sensor_raytrace_los_strength =
          std::clamp(ui.system_map_sensor_raytrace_los_strength, 0.0f, 1.0f);
      ui.system_map_sensor_raytrace_los_samples =
          std::clamp(ui.system_map_sensor_raytrace_los_samples, 1, 64);
      ui.system_map_sensor_raytrace_spp = std::clamp(ui.system_map_sensor_raytrace_spp, 1, 16);
      ui.system_map_sensor_raytrace_max_depth = std::clamp(ui.system_map_sensor_raytrace_max_depth, 0, 10);
      ui.system_map_sensor_raytrace_error_threshold =
          std::clamp(ui.system_map_sensor_raytrace_error_threshold, 0.0f, 0.5f);
      ImGui::SliderFloat("LOS strength", &ui.system_map_sensor_raytrace_los_strength, 0.0f, 1.0f, "%.2f");
      ImGui::SliderInt("LOS samples", &ui.system_map_sensor_raytrace_los_samples, 1, 32);
      ImGui::SliderInt("Adaptive depth", &ui.system_map_sensor_raytrace_max_depth, 0, 10);
      ImGui::SliderFloat("Detail threshold", &ui.system_map_sensor_raytrace_error_threshold, 0.0f, 0.25f, "%.3f");
      ImGui::SliderInt("Stochastic spp", &ui.system_map_sensor_raytrace_spp, 1, 8);
      ImGui::Checkbox("Debug quads", &ui.system_map_sensor_raytrace_debug);
      ImGui::TextDisabled("UI-only visualization: samples nebula/storm environment along the LOS ray.");
      ImGui::Unindent();
    }

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
    ImGui::SameLine();
    ImGui::Checkbox("Uncertainty##contacts", &ui.show_contact_uncertainty);
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
    ImGui::Checkbox("Galaxy: freight lanes", &ui.show_galaxy_freight_lanes);
    ImGui::TextDisabled("Draws current auto-freight routes (cargo orders) for the viewer faction.");

    ImGui::Checkbox("Galaxy: trade lanes", &ui.show_galaxy_trade_lanes);
    ImGui::SameLine();
    ImGui::Checkbox("Hubs", &ui.show_galaxy_trade_hubs);
    ImGui::TextDisabled("Procedural civilian trade overlay (markets + lanes).\n"
                        "This is informational for now, and will later feed piracy/blockade systems.");

    ImGui::Checkbox("Galaxy: fleet missions", &ui.show_galaxy_fleet_missions);
    if (ui.show_galaxy_fleet_missions) {
      ImGui::SliderFloat("Fleet mission opacity", &ui.galaxy_fleet_mission_alpha, 0.05f, 1.0f, "%.2f");
      ui.galaxy_fleet_mission_alpha = std::clamp(ui.galaxy_fleet_mission_alpha, 0.05f, 1.0f);
    }
    ImGui::TextDisabled("Draws patrol routes/circuits and other fleet mission geometry for the viewer faction.");

    ImGui::SliderInt("Contact max age (days)", &ui.contact_max_age_days, 1, 3650);
    ui.contact_max_age_days = std::clamp(ui.contact_max_age_days, 1, 3650);

    ImGui::SliderFloat("Starfield density", &ui.map_starfield_density, 0.0f, 4.0f, "%.2fx");
    ui.map_starfield_density = std::clamp(ui.map_starfield_density, 0.0f, 4.0f);
    ImGui::SliderFloat("Starfield parallax", &ui.map_starfield_parallax, 0.0f, 1.0f, "%.2f");
    ui.map_starfield_parallax = std::clamp(ui.map_starfield_parallax, 0.0f, 1.0f);

    ImGui::SeparatorText("Ray-marched nebula (SDF)");
    ImGui::Checkbox("Enable ray-marched nebula", &ui.map_raymarch_nebula);
    if (ui.map_raymarch_nebula) {
      ImGui::Indent();
      ImGui::SliderFloat("Nebula alpha", &ui.map_raymarch_nebula_alpha, 0.0f, 1.0f, "%.2f");
      ui.map_raymarch_nebula_alpha = std::clamp(ui.map_raymarch_nebula_alpha, 0.0f, 1.0f);
      ImGui::SliderFloat("Nebula parallax", &ui.map_raymarch_nebula_parallax, 0.0f, 1.0f, "%.2f");
      ui.map_raymarch_nebula_parallax = std::clamp(ui.map_raymarch_nebula_parallax, 0.0f, 1.0f);
      ImGui::SliderInt("Adaptive depth", &ui.map_raymarch_nebula_max_depth, 0, 10);
      ui.map_raymarch_nebula_max_depth = std::clamp(ui.map_raymarch_nebula_max_depth, 0, 10);
      ImGui::SliderFloat("Detail threshold", &ui.map_raymarch_nebula_error_threshold, 0.0f, 0.25f, "%.3f");
      ui.map_raymarch_nebula_error_threshold = std::clamp(ui.map_raymarch_nebula_error_threshold, 0.0f, 0.5f);
      ImGui::SliderInt("Samples (stochastic)", &ui.map_raymarch_nebula_spp, 1, 8);
      ui.map_raymarch_nebula_spp = std::clamp(ui.map_raymarch_nebula_spp, 1, 8);
      ImGui::SliderInt("Ray steps", &ui.map_raymarch_nebula_max_steps, 8, 160);
      ui.map_raymarch_nebula_max_steps = std::clamp(ui.map_raymarch_nebula_max_steps, 8, 160);
      ImGui::Checkbox("Animate", &ui.map_raymarch_nebula_animate);
      if (ui.map_raymarch_nebula_animate) {
        ImGui::SliderFloat("Time scale", &ui.map_raymarch_nebula_time_scale, 0.0f, 3.0f, "%.2f");
        ui.map_raymarch_nebula_time_scale = std::clamp(ui.map_raymarch_nebula_time_scale, 0.0f, 3.0f);
      }
      ImGui::Checkbox("Debug overlay", &ui.map_raymarch_nebula_debug);
      ImGui::TextDisabled("Sphere-traced SDF shells with stochastic sampling + adaptive subdivision.\n"
                          "Turn on Debug overlay to see quad/ray/step counts.");
      ImGui::Unindent();
    }
    ImGui::SliderFloat("Grid opacity", &ui.map_grid_opacity, 0.0f, 1.0f, "%.2f");
    ui.map_grid_opacity = std::clamp(ui.map_grid_opacity, 0.0f, 1.0f);
    ImGui::SliderFloat("Route opacity", &ui.map_route_opacity, 0.0f, 1.0f, "%.2f");
    ui.map_route_opacity = std::clamp(ui.map_route_opacity, 0.0f, 1.0f);

    ImGui::EndTabItem();
  }

  // --- HUD tab ---
  if (ImGui::BeginTabItem("HUD")) {
    ImGui::SeparatorText("HUD & Accessibility");
    ImGui::SliderFloat("UI scale", &ui.ui_scale, 0.65f, 2.5f, "%.2fx");
    ui.ui_scale = std::clamp(ui.ui_scale, 0.65f, 2.5f);

    ImGui::Checkbox("Status bar", &ui.show_status_bar);
    ImGui::Checkbox("Event toasts (warn/error)", &ui.show_event_toasts);
    if (ui.show_event_toasts) {
      ImGui::SliderFloat("Toast duration (sec)", &ui.event_toast_duration_sec, 1.0f, 30.0f, "%.0f");
      ui.event_toast_duration_sec = std::clamp(ui.event_toast_duration_sec, 0.5f, 60.0f);
    }

    ImGui::SeparatorText("Notification Center");
    {
      ImGui::Checkbox("Capture simulation events", &ui.notifications_capture_sim_events);
      if (ui.notifications_capture_sim_events) {
        ImGui::Indent();
        ImGui::Checkbox("Include info events", &ui.notifications_capture_info_events);
        ImGui::Unindent();
      }
      ImGui::Checkbox("Capture watchboard alerts", &ui.notifications_capture_watchboard_alerts);
      ImGui::Checkbox("Collapse duplicates", &ui.notifications_collapse_duplicates);
      ImGui::Checkbox("Auto-open on error", &ui.notifications_auto_open_on_error);

      ImGui::SliderInt("Max stored notifications", &ui.notifications_max_entries, 50, 2000);
      ui.notifications_max_entries = std::clamp(ui.notifications_max_entries, 25, 10000);
      ImGui::SliderInt("Retention (days, 0=forever)", &ui.notifications_keep_days, 0, 3650);
      ui.notifications_keep_days = std::clamp(ui.notifications_keep_days, 0, 36500);

      if (ImGui::Button("Open Notification Center (F3)")) ui.show_notifications_window = true;
      ImGui::SameLine();
      if (ImGui::Button("Clear read notifications")) notifications_clear_read(ui);
    }

    ImGui::SeparatorText("Command Console");
    {
      ImGui::SliderInt("Recent commands (0=disabled)", &ui.command_recent_limit, 0, 50);
      ui.command_recent_limit = std::clamp(ui.command_recent_limit, 0, 100);
    }

    ImGui::SeparatorText("Screen Reader (Narration)");
    {
      ScreenReader& sr = ScreenReader::instance();

      ImGui::Checkbox("Enable screen reader narration", &ui.screen_reader_enabled);
      sr.observe_item("Enable screen reader narration", "Toggles in-game narration using text-to-speech when available. Shortcut: Ctrl+Alt+R");

      if (ui.screen_reader_enabled) {
        ImGui::Indent();
        ImGui::Checkbox("Speak focused controls", &ui.screen_reader_speak_focus);
        sr.observe_item("Speak focused controls", "Announce controls as they receive keyboard navigation focus.");
        ImGui::Checkbox("Speak hovered controls", &ui.screen_reader_speak_hover);
        sr.observe_item("Speak hovered controls", "Announce controls under the mouse pointer (after a delay).");
        ImGui::Checkbox("Speak window focus", &ui.screen_reader_speak_windows);
        sr.observe_item("Speak window focus", "Announce window titles when they gain focus.");
        ImGui::Checkbox("Speak toasts", &ui.screen_reader_speak_toasts);
        sr.observe_item("Speak toasts", "Announce warn/error event toasts.");
        ImGui::Checkbox("Speak selection changes", &ui.screen_reader_speak_selection);
        sr.observe_item("Speak selection changes", "Announce when a ship/colony/body selection changes.");

        ImGui::SliderFloat("Voice rate", &ui.screen_reader_rate, 0.5f, 2.0f, "%.2fx");
        ui.screen_reader_rate = std::clamp(ui.screen_reader_rate, 0.5f, 2.0f);
        sr.observe_item("Voice rate", "Speech rate multiplier.");

        float vol_pct = ui.screen_reader_volume * 100.0f;
        if (ImGui::SliderFloat("Voice volume##sr_vol", &vol_pct, 0.0f, 100.0f, "%.0f%%")) {
          ui.screen_reader_volume = std::clamp(vol_pct / 100.0f, 0.0f, 1.0f);
        }
        sr.observe_item("Voice volume##sr_vol", "Speech volume.");

        ImGui::SliderFloat("Hover delay (sec)", &ui.screen_reader_hover_delay_sec, 0.0f, 5.0f, "%.2f");
        ui.screen_reader_hover_delay_sec = std::clamp(ui.screen_reader_hover_delay_sec, 0.0f, 5.0f);
        sr.observe_item("Hover delay (sec)", "How long the cursor must hover before narration triggers.");

        static char test_phrase[256] = "Nebula 4X screen reader test.";
        ImGui::InputText("Test phrase##sr_test", test_phrase, sizeof(test_phrase));
        sr.observe_item("Test phrase##sr_test", "Type a custom phrase to speak.");

        if (ImGui::Button("Speak test phrase")) {
          sr.speak(std::string(test_phrase), true);
        }
        sr.observe_item("Speak test phrase", "Speak the test phrase immediately.");
        ImGui::SameLine();
        if (ImGui::Button("Repeat last")) {
          sr.repeat_last();
        }
        sr.observe_item("Repeat last", "Repeat the last spoken line. Shortcut: Ctrl+Alt+.");

        ImGui::SeparatorText("Spoken history");
        const auto hist = sr.history_snapshot();
        ImGui::BeginChild("sr_history", ImVec2(0, 120), true);
        for (const auto& e : hist) {
          (void)e;
          ImGui::TextUnformatted(e.text.c_str());
        }
        ImGui::EndChild();

        if (ImGui::Button("Copy history")) {
          std::string joined;
          joined.reserve(hist.size() * 32);
          for (const auto& e : hist) {
            joined += e.text;
            joined += "\n";
          }
          ImGui::SetClipboardText(joined.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear history")) {
          sr.clear_history();
        }

        ImGui::Unindent();
      }
    }

    ImGui::SeparatorText("Shortcuts");
    ImGui::TextDisabled(
        "Keyboard shortcuts are now fully rebindable. See Settings > Hotkeys for the current bindings.\n"
        "Tip: the Command Console (Ctrl+P by default) also lists shortcuts next to actions.");

    ImGui::EndTabItem();
  }

  // --- Hotkeys tab ---
  if (ImGui::BeginTabItem("Hotkeys")) {
    draw_hotkeys_settings_tab(ui, actions);
    ImGui::EndTabItem();
  }

  // --- Timeline tab ---
  if (ImGui::BeginTabItem("Timeline")) {
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

    ImGui::EndTabItem();
  }

  // --- Design tab ---
  if (ImGui::BeginTabItem("Design")) {
    ImGui::SeparatorText("Design Studio");
    ImGui::Checkbox("Show grid##design_studio", &ui.design_studio_show_grid);
    ImGui::Checkbox("Show labels##design_studio", &ui.design_studio_show_labels);
    ImGui::Checkbox("Compare by default##design_studio", &ui.design_studio_show_compare);
    ImGui::Checkbox("Power overlay##design_studio", &ui.design_studio_show_power_overlay);
    ImGui::Checkbox("Heat overlay##design_studio", &ui.design_studio_show_heat_overlay);

    ImGui::EndTabItem();
  }

  // --- Intel tab ---
  if (ImGui::BeginTabItem("Intel")) {
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

    ImGui::EndTabItem();
  }

  // --- Diplomacy tab ---
  if (ImGui::BeginTabItem("Diplomacy")) {
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

    ImGui::EndTabItem();
  }

  // --- Windows tab ---
  if (ImGui::BeginTabItem("Windows")) {
    ImGui::SeparatorText("Windows");
    ImGui::Checkbox("Controls", &ui.show_controls_window);
    ImGui::Checkbox("Map", &ui.show_map_window);
    ImGui::Checkbox("Details", &ui.show_details_window);
    ImGui::Checkbox("Directory", &ui.show_directory_window);
    ImGui::Checkbox("Production", &ui.show_production_window);
    ImGui::Checkbox("Economy", &ui.show_economy_window);
    ImGui::Checkbox("Timeline", &ui.show_timeline_window);
    ImGui::Checkbox("Design Studio", &ui.show_design_studio_window);
    ImGui::Checkbox("Balance Lab", &ui.show_balance_lab_window);
    ImGui::Checkbox("Intel", &ui.show_intel_window);
    ImGui::Checkbox("Intel Notebook", &ui.show_intel_notebook_window);
    ImGui::Checkbox("Diplomacy Graph", &ui.show_diplomacy_window);
    ImGui::Checkbox("Victory & Score", &ui.show_victory_window);

    if (ImGui::Button("Reset window layout")) {
      actions.reset_window_layout = true;
    }

    ImGui::EndTabItem();
  }

  // --- Docking tab ---
  if (ImGui::BeginTabItem("Docking")) {
    ImGui::SeparatorText("Docking");
    ImGui::Checkbox("Hold Shift to dock", &ui.docking_with_shift);
    ImGui::Checkbox("Always show tab bars", &ui.docking_always_tab_bar);
    ImGui::Checkbox("Transparent docking preview", &ui.docking_transparent_payload);

    ImGui::SeparatorText("Detachable Windows");
#if NEBULA4X_UI_RENDERER_OPENGL2
#ifdef IMGUI_HAS_VIEWPORT
    ImGui::Checkbox("Enable detachable tool windows (multi-viewport)", &ui.viewports_enable);
    ImGui::BeginDisabled(!ui.viewports_enable);
    ImGui::Checkbox("No taskbar icons for tool windows", &ui.viewports_no_taskbar_icon);
    ImGui::Checkbox("Disable viewport auto-merge", &ui.viewports_no_auto_merge);
    ImGui::Checkbox("No OS window decorations (advanced)", &ui.viewports_no_decoration);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Tip: drag a window outside the main app window to detach it.");
#else
    ImGui::TextDisabled("This ImGui build does not have multi-viewport support enabled.");
#endif
#else
    ImGui::TextDisabled("Detachable OS windows require the OpenGL2 UI backend.");
    ImGui::TextDisabled("Reconfigure with -DNEBULA4X_UI_USE_OPENGL2=ON.");
#endif

    {
      const char* ini = ImGui::GetIO().IniFilename;
      ImGui::TextDisabled("Layout file: %s", (ini && ini[0]) ? ini : "(none)");
    }
    {
      ImGui::TextDisabled("Layout profile: %s", ui.layout_profile);
      ImGui::SameLine();
      if (ImGui::SmallButton("Manage...")) {
        ui.show_layout_profiles_window = true;
      }
    }

    ImGui::SeparatorText("Popup Windows");
    ImGui::Checkbox("Popup-first mode (new windows open floating)", &ui.window_popup_first_mode);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-focus popups", &ui.window_popup_auto_focus);
    ImGui::SliderFloat("Popup cascade step (px)", &ui.window_popup_cascade_step_px, 0.0f, 64.0f, "%.0f");
    if (ImGui::SmallButton("Open Window Manager...")) {
      ui.show_window_manager_window = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear per-window overrides")) {
      ui.window_launch_overrides.clear();
    }
    ImGui::TextDisabled("Tip: The Window Manager has one-click Pop Out buttons to undock any panel.");


    ImGui::SeparatorText("Procedural Layout Synthesizer");
    ImGui::TextWrapped(
        "Generate a docked workspace procedurally from a compact parameter set (seed + archetype). "
        "This uses Dear ImGui's DockBuilder API and can be saved as a layout profile.");

    static std::string proc_layout_paste_error;

    const char* modes[] = {"Balanced", "Command", "Data", "Debug", "Forge"};
    ui.ui_procedural_layout_mode = std::clamp(ui.ui_procedural_layout_mode, 0, (int)IM_ARRAYSIZE(modes) - 1);
    ImGui::Combo("Mode##proc_layout_mode", &ui.ui_procedural_layout_mode, modes, IM_ARRAYSIZE(modes));
    ImGui::InputInt("Seed##proc_layout_seed", &ui.ui_procedural_layout_seed);
    ImGui::SliderFloat("Variation##proc_layout_variation", &ui.ui_procedural_layout_variation, 0.0f, 1.0f, "%.2f");
    ui.ui_procedural_layout_variation = std::clamp(ui.ui_procedural_layout_variation, 0.0f, 1.0f);

    ImGui::Checkbox("Include tool/debug windows", &ui.ui_procedural_layout_include_tools);
    ImGui::Checkbox("Include UI Forge panels", &ui.ui_procedural_layout_include_forge_panels);
    if (ui.ui_procedural_layout_include_forge_panels) {
      ImGui::SliderInt("Max panels (0=all)##proc_layout_max_panels", &ui.ui_procedural_layout_max_forge_panels, 0, 16);
    }
    ImGui::Checkbox("Auto-open docked windows", &ui.ui_procedural_layout_auto_open_windows);
    ImGui::Checkbox("Auto-save to active profile", &ui.ui_procedural_layout_autosave_profile);

    {
      ProceduralLayoutParams p;
      p.seed = static_cast<std::uint32_t>(ui.ui_procedural_layout_seed);
      p.mode = ui.ui_procedural_layout_mode;
      p.variation = ui.ui_procedural_layout_variation;
      p.include_tools = ui.ui_procedural_layout_include_tools;
      p.include_forge_panels = ui.ui_procedural_layout_include_forge_panels;
      p.max_forge_panels = ui.ui_procedural_layout_max_forge_panels;
      p.auto_open_windows = ui.ui_procedural_layout_auto_open_windows;
      p.auto_save_profile = ui.ui_procedural_layout_autosave_profile;

      const std::string layout_str = encode_layout_dna(p);

      if (ImGui::SmallButton("Copy layout string")) {
        ImGui::SetClipboardText(layout_str.c_str());
        proc_layout_paste_error.clear();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Paste layout string")) {
        proc_layout_paste_error.clear();
        if (const char* clip = ImGui::GetClipboardText()) {
          ProceduralLayoutParams q;
          std::string err;
          if (decode_layout_dna(std::string(clip), &q, &err)) {
            ui.ui_procedural_layout_seed = static_cast<int>(q.seed);
            ui.ui_procedural_layout_mode = std::clamp(q.mode, 0, (int)IM_ARRAYSIZE(modes) - 1);
            ui.ui_procedural_layout_variation = std::clamp(q.variation, 0.0f, 1.0f);
            ui.ui_procedural_layout_include_tools = q.include_tools;
            ui.ui_procedural_layout_include_forge_panels = q.include_forge_panels;
            ui.ui_procedural_layout_max_forge_panels = std::clamp(q.max_forge_panels, 0, 32);
            ui.ui_procedural_layout_auto_open_windows = q.auto_open_windows;
            ui.ui_procedural_layout_autosave_profile = q.auto_save_profile;
          } else {
            proc_layout_paste_error = err.empty() ? std::string("Failed to parse layout string.") : err;
          }
        }
      }

      if (!proc_layout_paste_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", proc_layout_paste_error.c_str());
      }

      if (ImGui::Button("Generate layout##proc_layout")) {
        // Optionally open the windows that the layout expects so it's immediately visible.
        if (p.auto_open_windows) {
          apply_procedural_layout_visibility(ui, p);
        }
        ui.request_generate_procedural_layout = true;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Mutate seed")) {
        // Cheap deterministic "mutation" so the user can iterate quickly.
        std::uint32_t s = static_cast<std::uint32_t>(ui.ui_procedural_layout_seed);
        s = s * 1664525u + 1013904223u;
        ui.ui_procedural_layout_seed = static_cast<int>(s);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("Tip: Use Layout Profiles → Manage to save/share full workspaces.");
    }
    ImGui::EndTabItem();
  }

  // --- Persistence tab ---
  if (ImGui::BeginTabItem("Persistence")) {
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

    ImGui::SeparatorText("Notes");
    ImGui::TextWrapped(
        "Theme/layout settings are stored separately from save-games. Use 'UI Prefs' to persist your UI theme "
        "(including background colors) and window visibility.");

    ImGui::EndTabItem();
  }

  ImGui::EndTabBar();
  ImGui::End();
}

void draw_directory_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
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


  // --- Ships tab ---
  if (ImGui::BeginTabItem("Ships")) {
    static char search[128] = "";
    static int faction_filter_idx = 0; // 0 = All
    static int system_filter_idx = 0;  // 0 = All
    static int role_filter_idx = 0;    // 0 = All
    static bool only_idle = false;
    static bool only_automated = false;
    static bool only_damaged = false;
    static bool only_low_fuel = false;
    static bool only_overheated = false;
    static bool only_in_fleet = false;

    // Bulk selection (checkbox-based).
    static std::unordered_set<Id> bulk_selected;
    static std::uint64_t bulk_last_state_gen = 0;
    if (bulk_last_state_gen != sim.state_generation()) {
      bulk_selected.clear();
      bulk_last_state_gen = sim.state_generation();
    }

    const auto factions = sorted_factions(s);
    const auto systems = sorted_systems(s);

    // Fog-of-war guardrail: don't leak other factions' ships when FoW is enabled.
    const bool force_viewer_faction =
        (ui.fog_of_war && ui.viewer_faction_id != kInvalidId);

    ImGui::InputTextWithHint("Search##ship", "name / design / system / fleet", search, IM_ARRAYSIZE(search));

    // Faction filter.
    Id faction_filter = kInvalidId;
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

      if (force_viewer_faction) {
        // Snap to viewer faction and disable the control.
        int viewer_idx = 0; // All
        for (int i = 0; i < (int)factions.size(); ++i) {
          if (factions[(std::size_t)i].first == ui.viewer_faction_id) {
            viewer_idx = i + 1;
            break;
          }
        }
        faction_filter_idx = viewer_idx;
        ImGui::BeginDisabled();
      }

      ImGui::Combo("Faction##ship", &faction_filter_idx, labels.data(), static_cast<int>(labels.size()));

      if (force_viewer_faction) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(FoW: limited)");
      }

      if (faction_filter_idx > 0 && !factions.empty()) {
        faction_filter = factions[(std::size_t)faction_filter_idx - 1].first;
      }
      if (force_viewer_faction) faction_filter = ui.viewer_faction_id;
    }

    // System filter.
    Id system_filter = kInvalidId;
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

      ImGui::Combo("System##ship", &system_filter_idx, labels.data(), static_cast<int>(labels.size()));
      if (system_filter_idx > 0 && !systems.empty()) {
        system_filter = systems[(std::size_t)system_filter_idx - 1].first;
      }
    }

    // Role filter.
    ShipRole role_filter = ShipRole::Unknown;
    {
      const char* roles[] = {"All roles", "Combatant", "Surveyor", "Freighter", "Unknown"};
      role_filter_idx = std::clamp(role_filter_idx, 0, (int)IM_ARRAYSIZE(roles) - 1);
      ImGui::Combo("Role##ship", &role_filter_idx, roles, IM_ARRAYSIZE(roles));
      switch (role_filter_idx) {
        case 1: role_filter = ShipRole::Combatant; break;
        case 2: role_filter = ShipRole::Surveyor; break;
        case 3: role_filter = ShipRole::Freighter; break;
        case 4: role_filter = ShipRole::Unknown; break;
        default: break;
      }
    }

    ImGui::Checkbox("Only idle##ship", &only_idle);
    ImGui::SameLine();
    ImGui::Checkbox("Only automated##ship", &only_automated);
    ImGui::SameLine();
    ImGui::Checkbox("Only damaged##ship", &only_damaged);
    ImGui::SameLine();
    ImGui::Checkbox("Only low fuel##ship", &only_low_fuel);
    ImGui::SameLine();
    ImGui::Checkbox("Only overheated##ship", &only_overheated);
    ImGui::SameLine();
    ImGui::Checkbox("Only in fleet##ship", &only_in_fleet);

    struct ShipRow {
      Id id{kInvalidId};
      Id faction_id{kInvalidId};
      Id system_id{kInvalidId};
      Id fleet_id{kInvalidId};

      std::string name;
      std::string faction;
      std::string system;
      std::string fleet;
      std::string design;
      ShipRole role{ShipRole::Unknown};

      int orders{0};
      SensorMode sensor_mode{SensorMode::Normal};

      double hp_frac{1.0};
      double maint{1.0};
      double heat_frac{0.0};
      double fuel_frac{-1.0};

      std::string auto_flags;
      bool automated{false};
    };

    std::vector<ShipRow> rows;
    rows.reserve(s.ships.size());

    auto sensor_mode_label = [](SensorMode m) -> const char* {
      switch (m) {
        case SensorMode::Passive: return "Passive";
        case SensorMode::Normal: return "Normal";
        case SensorMode::Active: return "Active";
      }
      return "Normal";
    };

    const std::string q(search);

    for (Id sid : sorted_keys(s.ships)) {
      const Ship* sh = find_ptr(s.ships, sid);
      if (!sh) continue;

      if (faction_filter != kInvalidId && sh->faction_id != faction_filter) continue;
      if (system_filter != kInvalidId && sh->system_id != system_filter) continue;

      const ShipDesign* d = sim.find_design(sh->design_id);
      const ShipRole role = d ? d->role : ShipRole::Unknown;
      if (role_filter_idx != 0 && role != role_filter) continue;

      const StarSystem* sys = find_ptr(s.systems, sh->system_id);
      const Faction* fac = find_ptr(s.factions, sh->faction_id);

      const Id fleet_id = sim.fleet_for_ship(sh->id);
      const Fleet* fl = (fleet_id != kInvalidId) ? find_ptr(s.fleets, fleet_id) : nullptr;

      // Orders count (0 for missing record).
      int orders = 0;
      if (const ShipOrders* so = find_ptr(s.ship_orders, sh->id)) {
        orders = (int)so->queue.size();
      }

      const bool is_idle = (orders == 0);
      if (only_idle && !is_idle) continue;

      std::string auto_s;
      if (sh->auto_explore) auto_s += "E";
      if (sh->auto_freight) auto_s += (auto_s.empty() ? "F" : " F");
      if (sh->auto_salvage) auto_s += (auto_s.empty() ? "S" : " S");
      if (sh->auto_mine) auto_s += (auto_s.empty() ? "M" : " M");
      if (sh->auto_colonize) auto_s += (auto_s.empty() ? "C" : " C");
      if (sh->auto_colonist_transport) auto_s += (auto_s.empty() ? "P" : " P");
      if (sh->auto_tanker) auto_s += (auto_s.empty() ? "T" : " T");
      if (sh->auto_troop_transport) auto_s += (auto_s.empty() ? "G" : " G");
      if (sh->auto_refuel) auto_s += (auto_s.empty() ? "Rf" : " Rf");
      if (sh->auto_repair) auto_s += (auto_s.empty() ? "Rp" : " Rp");
      if (sh->auto_rearm) auto_s += (auto_s.empty() ? "Ra" : " Ra");
      const bool automated = !auto_s.empty();
      if (only_automated && !automated) continue;

      // Condition.
      double hp_frac = 1.0;
      if (d && d->max_hp > 1e-9) {
        hp_frac = std::clamp(sh->hp / d->max_hp, 0.0, 1.0);
      }
      const bool damaged = (hp_frac < 0.999);
      if (only_damaged && !damaged) continue;

      const double maint = std::clamp(sh->maintenance_condition, 0.0, 1.0);

      double fuel_frac = -1.0;
      if (d && d->fuel_capacity_tons > 1e-9 && sh->fuel_tons >= 0.0) {
        fuel_frac = std::clamp(sh->fuel_tons / d->fuel_capacity_tons, 0.0, 1.0);
      }
      if (only_low_fuel) {
        if (!(fuel_frac >= 0.0 && fuel_frac < 0.25)) continue;
      }

      const double heat_frac = std::clamp(sim.ship_heat_fraction(*sh), 0.0, 1.0);
      if (only_overheated) {
        if (!(heat_frac > 0.85)) continue;
      }

      if (only_in_fleet && fleet_id == kInvalidId) continue;

      // Search matches: ship name, design, system, fleet, faction.
      const std::string sys_name = sys ? sys->name : "?";
      const std::string fac_name = fac ? fac->name : "?";
      const std::string fleet_name = fl ? fl->name : std::string("-");
      const std::string design_name = (d && !d->name.empty()) ? d->name : sh->design_id;

      if (!q.empty()) {
        if (!case_insensitive_contains(sh->name, q) &&
            !case_insensitive_contains(design_name, q) &&
            !case_insensitive_contains(sys_name, q) &&
            !case_insensitive_contains(fleet_name, q) &&
            !case_insensitive_contains(fac_name, q)) {
          continue;
        }
      }

      ShipRow r;
      r.id = sh->id;
      r.faction_id = sh->faction_id;
      r.system_id = sh->system_id;
      r.fleet_id = fleet_id;

      r.name = sh->name;
      r.faction = fac_name;
      r.system = sys_name;
      r.fleet = fleet_name;
      r.design = design_name;
      r.role = role;

      r.orders = orders;
      r.sensor_mode = sh->sensor_mode;

      r.hp_frac = hp_frac;
      r.maint = maint;
      r.heat_frac = heat_frac;
      r.fuel_frac = fuel_frac;

      r.auto_flags = auto_s.empty() ? "-" : auto_s;
      r.automated = automated;

      rows.push_back(std::move(r));
    }

    ImGui::Separator();
    ImGui::TextDisabled("Showing %d ships", (int)rows.size());

    // Bulk actions.
    {
      const int sel_count = (int)bulk_selected.size();
      ImGui::Text("Bulk selection: %d", sel_count);
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear##ship_bulk_clear")) {
        bulk_selected.clear();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Select all (filtered)##ship_bulk_all")) {
        for (const auto& r : rows) bulk_selected.insert(r.id);
      }

      if (sel_count > 0) {
        ImGui::Spacing();
        ImGui::SeparatorText("Bulk actions");

        static int bulk_sensor_mode_idx = 1; // Normal
        const char* smodes[] = {"Passive", "Normal", "Active"};
        bulk_sensor_mode_idx = std::clamp(bulk_sensor_mode_idx, 0, (int)IM_ARRAYSIZE(smodes) - 1);
        ImGui::Combo("Sensor mode##ship_bulk", &bulk_sensor_mode_idx, smodes, IM_ARRAYSIZE(smodes));
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply##ship_bulk_sensor_apply")) {
          const SensorMode mode = (bulk_sensor_mode_idx == 0) ? SensorMode::Passive
                                  : (bulk_sensor_mode_idx == 2) ? SensorMode::Active
                                                                : SensorMode::Normal;
          for (Id sid : bulk_selected) {
            if (auto* sh = find_ptr(s.ships, sid)) {
              sh->sensor_mode = mode;
            }
          }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Clear orders##ship_bulk_clear_orders")) {
          for (Id sid : bulk_selected) {
            sim.clear_orders(sid);
          }
        }

        static int bulk_primary_auto_idx = 0; // 0=None
        const char* autos[] = {"(no change)", "Disable mission automation", "Auto-explore", "Auto-freight", "Auto-salvage",
                               "Auto-mine", "Auto-colonize", "Auto-colonist transport", "Auto-tanker", "Auto-troop transport"};
        ImGui::Combo("Mission automation##ship_bulk_auto", &bulk_primary_auto_idx, autos, IM_ARRAYSIZE(autos));
        ImGui::SameLine();
        static std::string bulk_status;
        if (ImGui::SmallButton("Apply##ship_bulk_auto_apply")) {
          int changed = 0;
          int skipped_fleet = 0;
          int skipped_cap = 0;

          auto clear_mission_auto = [&](Ship& sh) {
            sh.auto_explore = false;
            sh.auto_freight = false;
            sh.auto_salvage = false;
            sh.auto_mine = false;
            sh.auto_colonize = false;
            sh.auto_tanker = false;
            sh.auto_troop_transport = false;
            sh.auto_colonist_transport = false;
          };

          for (Id sid : bulk_selected) {
            Ship* sh = find_ptr(s.ships, sid);
            if (!sh) continue;

            // Respect fleet membership: fleets handle movement/stance.
            if (sim.fleet_for_ship(sid) != kInvalidId) {
              skipped_fleet++;
              continue;
            }

            const ShipDesign* d = sim.find_design(sh->design_id);

            if (bulk_primary_auto_idx == 0) continue; // no change
            if (bulk_primary_auto_idx == 1) {
              clear_mission_auto(*sh);
              changed++;
              continue;
            }

            // Mission automation modes are mutually exclusive.
            clear_mission_auto(*sh);

            bool ok = true;
            switch (bulk_primary_auto_idx) {
              case 2: // explore
                sh->auto_explore = true;
                break;
              case 3: // freight
                ok = (d && d->cargo_tons > 0.0);
                if (ok) sh->auto_freight = true;
                break;
              case 4: // salvage
                ok = (d && d->cargo_tons > 0.0);
                if (ok) sh->auto_salvage = true;
                break;
              case 5: // mine
                ok = (d && d->cargo_tons > 0.0 && d->mining_tons_per_day > 0.0);
                if (ok) sh->auto_mine = true;
                break;
              case 6: // colonize
                ok = (d && d->colony_capacity_millions > 0.0);
                if (ok) sh->auto_colonize = true;
                break;
              case 7: // colonist transport
                ok = (d && d->colony_capacity_millions > 0.0);
                if (ok) sh->auto_colonist_transport = true;
                break;
              case 8: // tanker
                ok = (d && d->fuel_capacity_tons > 0.0);
                if (ok) sh->auto_tanker = true;
                break;
              case 9: // troop transport
                ok = (d && d->troop_capacity > 0.0);
                if (ok) sh->auto_troop_transport = true;
                break;
              default:
                break;
            }

            if (!ok) {
              skipped_cap++;
              // Leave it as "none" (cleared) to avoid enabling invalid modes.
              continue;
            }

            changed++;
          }

          bulk_status = "Changed " + std::to_string(changed);
          if (skipped_fleet > 0) bulk_status += " | skipped " + std::to_string(skipped_fleet) + " (in fleet)";
          if (skipped_cap > 0) bulk_status += " | skipped " + std::to_string(skipped_cap) + " (capability)";
        }
        if (!bulk_status.empty()) {
          ImGui::TextDisabled("%s", bulk_status.c_str());
        }

        // Repair priority.
        static int bulk_repair_pri = 1; // Normal
        const char* pris[] = {"Low", "Normal", "High"};
        ImGui::Combo("Repair priority##ship_bulk_pri", &bulk_repair_pri, pris, IM_ARRAYSIZE(pris));
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply##ship_bulk_pri_apply")) {
          const RepairPriority rp = (bulk_repair_pri == 0) ? RepairPriority::Low
                                   : (bulk_repair_pri == 2) ? RepairPriority::High
                                                            : RepairPriority::Normal;
          for (Id sid : bulk_selected) {
            if (auto* sh = find_ptr(s.ships, sid)) sh->repair_priority = rp;
          }
        }

        ImGui::TextDisabled("Auto flags legend: E=Explore F=Freight S=Salvage M=Mine C=Colonize P=Colonists T=Tanker G=Troop  Rf=Refuel Rp=Repair Ra=Rearm");
      } else {
        ImGui::TextDisabled("Tip: Use the checkboxes to select multiple ships for bulk actions.");
      }
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("ship_directory", 13, flags, ImVec2(avail.x, avail.y))) {
      ImGui::TableSetupScrollFreeze(0, 1);

      ImGui::TableSetupColumn("Sel", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 32.0f, 0);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 0.0f, 1);
      ImGui::TableSetupColumn("Role", 0, 0.0f, 2);
      ImGui::TableSetupColumn("Faction", 0, 0.0f, 3);
      ImGui::TableSetupColumn("System", 0, 0.0f, 4);
      ImGui::TableSetupColumn("Fleet", 0, 0.0f, 5);
      ImGui::TableSetupColumn("Orders", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 6);
      ImGui::TableSetupColumn("HP", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 7);
      ImGui::TableSetupColumn("Maint", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 8);
      ImGui::TableSetupColumn("Heat", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 9);
      ImGui::TableSetupColumn("Fuel", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 10);
      ImGui::TableSetupColumn("Sensors", 0, 0.0f, 11);
      ImGui::TableSetupColumn("Auto", 0, 0.0f, 12);

      ImGui::TableHeadersRow();

      if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
        if (sort->SpecsDirty && sort->SpecsCount > 0) {
          const ImGuiTableColumnSortSpecs* spec = &sort->Specs[0];
          const bool asc = (spec->SortDirection == ImGuiSortDirection_Ascending);
          auto cmp = [&](const ShipRow& a, const ShipRow& b) {
            auto lt = [&](auto x, auto y) { return asc ? (x < y) : (x > y); };
            switch (spec->ColumnUserID) {
              case 1: return lt(a.name, b.name);
              case 2: return lt((int)a.role, (int)b.role);
              case 3: return lt(a.faction, b.faction);
              case 4: return lt(a.system, b.system);
              case 5: return lt(a.fleet, b.fleet);
              case 6: return lt(a.orders, b.orders);
              case 7: return lt(a.hp_frac, b.hp_frac);
              case 8: return lt(a.maint, b.maint);
              case 9: return lt(a.heat_frac, b.heat_frac);
              case 10: return lt(a.fuel_frac, b.fuel_frac);
              case 11: return lt((int)a.sensor_mode, (int)b.sensor_mode);
              case 12: return lt(a.auto_flags, b.auto_flags);
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
          const ShipRow& r = rows[i];
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          ImGui::PushID((void*)(intptr_t)r.id);
          bool sel = (bulk_selected.find(r.id) != bulk_selected.end());
          if (ImGui::Checkbox("##sel", &sel)) {
            if (sel) bulk_selected.insert(r.id);
            else bulk_selected.erase(r.id);
          }
          ImGui::PopID();

          ImGui::TableSetColumnIndex(1);
          const bool is_sel = (selected_ship == r.id);
          std::string label = r.name + "##ship_" + std::to_string(static_cast<unsigned long long>(r.id));
          if (ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_SpanAllColumns)) {
            selected_ship = r.id;
            if (r.system_id != kInvalidId) {
              s.selected_system = r.system_id;
            }
          }
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", r.name.c_str());
            ImGui::TextDisabled("Design: %s", r.design.c_str());
            ImGui::TextDisabled("Auto: %s", r.auto_flags.c_str());
            ImGui::TextDisabled("Sensor mode: %s", sensor_mode_label(r.sensor_mode));
            ImGui::EndTooltip();
          }

          // Context menu (right click).
          if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Center system map")) {
              if (r.system_id != kInvalidId) {
                s.selected_system = r.system_id;
                ui.request_map_tab = MapTab::System;
                ui.request_system_map_center = true;
                ui.request_system_map_center_system_id = r.system_id;
                if (const Ship* sh = find_ptr(s.ships, r.id)) {
                  ui.request_system_map_center_x_mkm = sh->position_mkm.x;
                  ui.request_system_map_center_y_mkm = sh->position_mkm.y;
                } else {
                  ui.request_system_map_center_x_mkm = 0.0;
                  ui.request_system_map_center_y_mkm = 0.0;
                }
                ui.request_system_map_center_zoom = 0.0;
              }
            }
            if (ImGui::MenuItem("Clear orders")) {
              sim.clear_orders(r.id);
            }
            if (ImGui::MenuItem("Select (bulk checkbox)")) {
              bulk_selected.insert(r.id);
            }
            ImGui::EndPopup();
          }

          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(ship_role_label(r.role));

          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(r.faction.c_str());

          ImGui::TableSetColumnIndex(4);
          ImGui::TextUnformatted(r.system.c_str());

          ImGui::TableSetColumnIndex(5);
          if (r.fleet_id != kInvalidId) ImGui::TextUnformatted(r.fleet.c_str());
          else ImGui::TextDisabled("-");

          ImGui::TableSetColumnIndex(6);
          if (r.orders > 0) {
            ImGui::Text("%d", r.orders);
            if (ImGui::IsItemHovered()) {
              if (const ShipOrders* so = find_ptr(s.ship_orders, r.id)) {
                if (!so->queue.empty()) {
                  ImGui::BeginTooltip();
                  ImGui::TextUnformatted("Current orders:");
                  const int max_show = std::min(6, (int)so->queue.size());
                  for (int oi = 0; oi < max_show; ++oi) {
                    const std::string os = nebula4x::order_to_string(so->queue[(std::size_t)oi]);
                    ImGui::BulletText("%s", os.c_str());
                  }
                  if ((int)so->queue.size() > max_show) {
                    ImGui::TextDisabled("... (%d more)", (int)so->queue.size() - max_show);
                  }
                  ImGui::EndTooltip();
                }
              }
            }
          } else {
            ImGui::TextDisabled("0");
          }

          auto bar = [&](double frac) {
            frac = std::clamp(frac, 0.0, 1.0);
            const std::string pct = format_fixed(frac * 100.0, 0) + "%";
            ImGui::ProgressBar((float)frac, ImVec2(-1.0f, 0.0f), pct.c_str());
          };

          ImGui::TableSetColumnIndex(7);
          bar(r.hp_frac);

          ImGui::TableSetColumnIndex(8);
          bar(r.maint);

          ImGui::TableSetColumnIndex(9);
          bar(r.heat_frac);

          ImGui::TableSetColumnIndex(10);
          if (r.fuel_frac >= 0.0) {
            bar(r.fuel_frac);
          } else {
            ImGui::TextDisabled("-");
          }

          ImGui::TableSetColumnIndex(11);
          // Editable per-row sensor mode.
          if (auto* sh = find_ptr(s.ships, r.id)) {
            const char* cur = sensor_mode_label(sh->sensor_mode);
            std::string key = "##sensor_" + std::to_string(static_cast<unsigned long long>(r.id));
            if (ImGui::BeginCombo(key.c_str(), cur, ImGuiComboFlags_HeightSmall)) {
              const SensorMode modes[] = {SensorMode::Passive, SensorMode::Normal, SensorMode::Active};
              for (SensorMode m : modes) {
                const bool selm = (sh->sensor_mode == m);
                if (ImGui::Selectable(sensor_mode_label(m), selm)) sh->sensor_mode = m;
                if (selm) ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }
          } else {
            ImGui::TextDisabled("-");
          }

          ImGui::TableSetColumnIndex(12);
          ImGui::TextUnformatted(r.auto_flags.c_str());
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
      double salvage_rp{0.0};
      std::int64_t age_days{0};
      int unknown_components{0};
    };

    std::vector<WreckRow> rows;
    rows.reserve(s.wrecks.size());

    const std::int64_t cur_day = s.date.days_since_epoch();

    const Faction* viewer_fac = (ui.viewer_faction_id != kInvalidId) ? find_ptr(s.factions, ui.viewer_faction_id) : nullptr;

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

      double salvage_rp = 0.0;
      for (const auto& [mid, v] : w.minerals) {
        const double amt = std::max(0.0, v);
        if (amt <= 1e-9) continue;
        const auto it_r = sim.content().resources.find(mid);
        if (it_r == sim.content().resources.end()) continue;
        const double rppt = std::max(0.0, it_r->second.salvage_research_rp_per_ton);
        if (rppt <= 0.0) continue;
        salvage_rp += amt * rppt;
      }

      int unknown_components = 0;
      if (viewer_fac && !w.source_design_id.empty()) {
        const ShipDesign* sd = sim.find_design(w.source_design_id);
        if (sd) {
          std::unordered_set<std::string> uniq;
          uniq.reserve(sd->components.size() * 2);
          for (const auto& cid : sd->components) {
            if (!cid.empty()) uniq.insert(cid);
          }
          for (const auto& cid : uniq) {
            if (std::find(viewer_fac->unlocked_components.begin(), viewer_fac->unlocked_components.end(), cid) ==
                viewer_fac->unlocked_components.end()) {
              ++unknown_components;
            }
          }
        }
      }

      WreckRow r;
      r.id = wid;
      r.system_id = w.system_id;
      r.pos = w.position_mkm;
      r.name = w.name.empty() ? (std::string("Wreck ") + std::to_string((int)wid)) : w.name;
      r.system = sys ? sys->name : "?";
      r.total = total;
      r.salvage_rp = salvage_rp;
      r.age_days = (w.created_day == 0) ? 0 : std::max<std::int64_t>(0, cur_day - w.created_day);
      r.unknown_components = unknown_components;

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
    if (ImGui::BeginTable("wreck_directory", 8, flags, ImVec2(avail.x, avail.y))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
      ImGui::TableSetupColumn("System", 0, 0.0f, 1);
      ImGui::TableSetupColumn("Total (t)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 2);
      ImGui::TableSetupColumn("RP", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 3);
      ImGui::TableSetupColumn("Age (d)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 4);
      ImGui::TableSetupColumn("Source", 0, 0.0f, 5);
      ImGui::TableSetupColumn("Unknown", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 6);
      ImGui::TableSetupColumn("Center", ImGuiTableColumnFlags_NoSort, 0.0f, 7);
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
              case 3: return lt(a.salvage_rp, b.salvage_rp);
              case 4: return lt(a.age_days, b.age_days);
              case 5: return lt(a.source, b.source);
              case 6: return lt(a.unknown_components, b.unknown_components);
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
          if (r.salvage_rp > 0.0) ImGui::Text("%.1f", r.salvage_rp);
          else ImGui::TextDisabled("-");
          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%lld", static_cast<long long>(r.age_days));
          ImGui::TableSetColumnIndex(5);
          ImGui::TextUnformatted(r.source.c_str());
          ImGui::TableSetColumnIndex(6);
          if (viewer_fac) {
            if (r.unknown_components > 0) ImGui::Text("%d", r.unknown_components);
            else ImGui::TextDisabled("0");
          } else {
            ImGui::TextDisabled("-");
          }
          ImGui::TableSetColumnIndex(7);
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

  // --- Anomalies tab ---
  if (ImGui::BeginTabItem("Anomalies")) {
    static char search[128] = "";
    static int system_filter_idx = 0; // 0 = All
    static bool show_resolved = false;

    const auto systems = sorted_systems(s);

    ImGui::InputTextWithHint("Search##anom", "name / kind / system / unlock / minerals", search, IM_ARRAYSIZE(search));

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
      ImGui::Combo("System##anom", &system_filter_idx, labels.data(), static_cast<int>(labels.size()));
    }

    ImGui::SameLine();
    ImGui::Checkbox("Show resolved##anom", &show_resolved);

    const Id system_filter = (system_filter_idx <= 0 || systems.empty()) ? kInvalidId : systems[system_filter_idx - 1].first;

    if (ui.fog_of_war && ui.viewer_faction_id != kInvalidId) {
      ImGui::TextDisabled("Fog-of-war is enabled: only discovered systems are listed.");
    }

    struct AnomRow {
      Id id{kInvalidId};
      Id system_id{kInvalidId};
      Vec2 pos{0.0, 0.0};
      std::string name;
      std::string kind;
      std::string system;
      int days{0};
      double rp{0.0};

      double minerals_total{0.0};
      double hazard_chance{0.0};
      double hazard_damage{0.0};

      std::string unlock;
      bool resolved{false};
    };

    std::vector<AnomRow> rows;
    rows.reserve(s.anomalies.size());

    for (const auto& [aid, a] : s.anomalies) {
      if (system_filter != kInvalidId && a.system_id != system_filter) continue;
      if (!show_resolved && a.resolved) continue;

      if (ui.fog_of_war && ui.viewer_faction_id != kInvalidId) {
        if (!sim.is_system_discovered_by_faction(ui.viewer_faction_id, a.system_id)) continue;
      }

      if (ui.fog_of_war && ui.viewer_faction_id != kInvalidId) {
        if (!sim.is_anomaly_discovered_by_faction(ui.viewer_faction_id, aid)) continue;
      }

      const StarSystem* sys = find_ptr(s.systems, a.system_id);

      const std::string nm = a.name.empty() ? (std::string("Anomaly ") + std::to_string((int)aid)) : a.name;
      const std::string kind = a.kind.empty() ? std::string("-") : a.kind;
      const std::string sys_name = sys ? sys->name : "?";

      bool mineral_match = false;
      if (search[0] != '\0') {
        for (const auto& [mid, _] : a.mineral_reward) {
          if (case_insensitive_contains(mid, search)) {
            mineral_match = true;
            break;
          }
        }
      }

      // Search matches name, kind, system, unlock id, or mineral ids.
      if (!case_insensitive_contains(nm, search) &&
          !case_insensitive_contains(kind, search) &&
          !case_insensitive_contains(sys_name, search) &&
          !case_insensitive_contains(a.unlock_component_id, search) &&
          !mineral_match) {
        continue;
      }

      AnomRow r;
      r.id = aid;
      r.system_id = a.system_id;
      r.pos = a.position_mkm;
      r.name = nm;
      r.kind = kind;
      r.system = sys_name;
      r.days = std::max(1, a.investigation_days);
      r.rp = std::max(0.0, a.research_reward);
      for (const auto& [_, tons] : a.mineral_reward) r.minerals_total += std::max(0.0, tons);
      r.hazard_chance = std::clamp(a.hazard_chance, 0.0, 1.0);
      r.hazard_damage = std::max(0.0, a.hazard_damage);

      if (!a.unlock_component_id.empty()) {
        const auto itc = sim.content().components.find(a.unlock_component_id);
        r.unlock = (itc != sim.content().components.end() && !itc->second.name.empty()) ? itc->second.name : a.unlock_component_id;
      } else {
        r.unlock = "-";
      }

      r.resolved = a.resolved;
      rows.push_back(std::move(r));
    }

    ImGui::Separator();
    ImGui::TextDisabled("Showing %d / %d anomalies", (int)rows.size(), (int)s.anomalies.size());

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                 ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                                 ImGuiTableFlags_ScrollY;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("anomaly_directory", 10, flags, ImVec2(avail.x, avail.y))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
      ImGui::TableSetupColumn("Kind", 0, 0.0f, 1);
      ImGui::TableSetupColumn("System", 0, 0.0f, 2);
      ImGui::TableSetupColumn("Days", ImGuiTableColumnFlags_PreferSortAscending, 0.0f, 3);
      ImGui::TableSetupColumn("RP", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 4);
      ImGui::TableSetupColumn("Minerals", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 5);
      ImGui::TableSetupColumn("Hazard", ImGuiTableColumnFlags_PreferSortAscending, 0.0f, 6);
      ImGui::TableSetupColumn("Unlock", 0, 0.0f, 7);
      ImGui::TableSetupColumn("Status", 0, 0.0f, 8);
      ImGui::TableSetupColumn("Center", ImGuiTableColumnFlags_NoSort, 0.0f, 9);
      ImGui::TableHeadersRow();

      if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
        if (sort->SpecsDirty && sort->SpecsCount > 0) {
          const ImGuiTableColumnSortSpecs* spec = &sort->Specs[0];
          const bool asc = (spec->SortDirection == ImGuiSortDirection_Ascending);
          auto cmp = [&](const AnomRow& a, const AnomRow& b) {
            auto lt = [&](auto x, auto y) { return asc ? (x < y) : (x > y); };
            switch (spec->ColumnUserID) {
              case 0: return lt(a.name, b.name);
              case 1: return lt(a.kind, b.kind);
              case 2: return lt(a.system, b.system);
              case 3: return lt(a.days, b.days);
              case 4: return lt(a.rp, b.rp);
              case 5: return lt(a.minerals_total, b.minerals_total);
              case 6: return lt(a.hazard_chance * a.hazard_damage, b.hazard_chance * b.hazard_damage);
              case 7: return lt(a.unlock, b.unlock);
              case 8: return lt(a.resolved, b.resolved);
              default: return lt(a.name, b.name);
            }
          };
          std::stable_sort(rows.begin(), rows.end(), cmp);
          sort->SpecsDirty = false;
        }
      }

      static Id selected_anom = kInvalidId;

      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(rows.size()));
      while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
          const AnomRow& r = rows[i];
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          const bool is_sel = (selected_anom == r.id);
          std::string label = r.name + "##anom_" + std::to_string((int)r.id);
          if (ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_SpanAllColumns)) {
            selected_anom = r.id;
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

          // Hover tooltip: procedural "fingerprint" + one-line lore.
          if (ImGui::IsItemHovered()) {
            const auto* a = find_ptr(s.anomalies, r.id);
            if (a) {
              const auto* sys = find_ptr(s.systems, a->system_id);
              const auto* reg = (sys && sys->region_id != kInvalidId) ? find_ptr(s.regions, sys->region_id) : nullptr;
              const double neb = sys ? std::clamp(sys->nebula_density, 0.0, 1.0) : 0.0;
              const double ruins = reg ? std::clamp(reg->ruins_density, 0.0, 1.0) : 0.0;
              const double pir = reg ? std::clamp(reg->pirate_risk * (1.0 - reg->pirate_suppression), 0.0, 1.0) : 0.0;

              const std::string sig = procgen_obscure::anomaly_signature_code(*a);
              const std::string glyph = procgen_obscure::anomaly_signature_glyph(*a);
              const std::string lore = procgen_obscure::anomaly_lore_line(*a, neb, ruins, pir);

              ImGui::BeginTooltip();
              ImGui::TextUnformatted((std::string("Signature: ") + sig).c_str());
              ImGui::Separator();
              ImGui::TextUnformatted(glyph.c_str());
              ImGui::Separator();
              ImGui::TextWrapped("%s", lore.c_str());

              // Obscure codex fragment: deterministic ciphertext + gradually revealed translation.
              if (sim.cfg().enable_obscure_codex_fragments) {
                const int req = std::max(1, sim.cfg().codex_fragments_required);
                int have = 0;
                if (ui.viewer_faction_id != kInvalidId) {
                  const Id root = procgen_obscure::anomaly_chain_root_id(s.anomalies, a->id);
                  have = procgen_obscure::faction_resolved_anomaly_chain_count(s.anomalies, ui.viewer_faction_id, root);
                }
                const double frac = std::clamp(static_cast<double>(have) / static_cast<double>(req), 0.0, 1.0);

                ImGui::Separator();
                ImGui::TextDisabled("Codex fragment (%d/%d)", have, req);
                const std::string cipher = procgen_obscure::codex_ciphertext(*a);
                const std::string partial = procgen_obscure::codex_partial_plaintext(*a, frac);
                ImGui::TextWrapped("%s", cipher.c_str());
                ImGui::TextWrapped("%s", partial.c_str());
              }

              ImGui::EndTooltip();
            }
          }

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(r.kind.c_str());
          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(r.system.c_str());
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%d", r.days);
          ImGui::TableSetColumnIndex(4);
          if (r.rp > 0.0) ImGui::Text("%.1f", r.rp);
          else ImGui::TextDisabled("-");
          ImGui::TableSetColumnIndex(5);
          if (r.minerals_total > 1e-6) {
            const std::string ms = format_fixed(r.minerals_total, 0) + "t";
            ImGui::TextUnformatted(ms.c_str());

            if (ImGui::IsItemHovered()) {
              const auto* a = find_ptr(s.anomalies, r.id);
              if (a && !a->mineral_reward.empty()) {
                std::vector<std::pair<std::string, double>> items;
                items.reserve(a->mineral_reward.size());
                for (const auto& [mid, tons] : a->mineral_reward) {
                  if (mid.empty()) continue;
                  if (!(tons > 1e-6)) continue;
                  items.emplace_back(mid, tons);
                }
                std::sort(items.begin(), items.end(), [](const auto& x, const auto& y) {
                  if (x.second != y.second) return x.second > y.second;
                  return x.first < y.first;
                });

                if (!items.empty()) {
                  ImGui::BeginTooltip();
                  ImGui::TextUnformatted("Mineral cache:");
                  for (const auto& [mid, tons] : items) {
                    ImGui::Text("%s: %.1ft", mid.c_str(), tons);
                  }
                  ImGui::EndTooltip();
                }
              }
            }
          } else {
            ImGui::TextDisabled("-");
          }

          ImGui::TableSetColumnIndex(6);
          if (r.hazard_chance > 1e-6 && r.hazard_damage > 1e-6) {
            const std::string hs =
                format_fixed(r.hazard_chance * 100.0, 0) + "% / " + format_fixed(r.hazard_damage, 1);
            ImGui::TextUnformatted(hs.c_str());
            if (ImGui::IsItemHovered()) {
              ImGui::BeginTooltip();
              ImGui::Text("Chance: %.0f%%", r.hazard_chance * 100.0);
              ImGui::Text("Damage: %.1f", r.hazard_damage);
              ImGui::TextDisabled("Hazards are non-lethal (hull will not drop below 1 HP).");
              ImGui::EndTooltip();
            }
          } else {
            ImGui::TextDisabled("-");
          }

          ImGui::TableSetColumnIndex(7);
          ImGui::TextUnformatted(r.unlock.c_str());
          ImGui::TableSetColumnIndex(8);
          if (r.resolved) ImGui::TextDisabled("Resolved");
          else ImGui::TextUnformatted("Unresolved");
          ImGui::TableSetColumnIndex(9);
          std::string b = "Go##anom_go_" + std::to_string((int)r.id);
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
