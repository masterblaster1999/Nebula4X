#include "ui/hud.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula4x/core/date.h"
#include "nebula4x/core/serialization.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/time.h"

namespace nebula4x::ui {
namespace {

// --- small helpers ---

// Draw a vertical separator using only public ImGui API (no imgui_internal.h).
void VerticalSeparator(float height = 0.0f) {
  ImGui::SameLine();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float h = (height > 0.0f) ? height : ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();

  // Reserve space.
  ImGui::Dummy(ImVec2(style.ItemSpacing.x, h));

  // Draw in the reserved rect.
  const ImVec2 a = ImGui::GetItemRectMin();
  const ImVec2 b = ImGui::GetItemRectMax();
  const float x = (a.x + b.x) * 0.5f;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(x, a.y + style.FramePadding.y), ImVec2(x, b.y - style.FramePadding.y),
              ImGui::GetColorU32(ImGuiCol_Separator));

  ImGui::SameLine();
}

std::string trim_copy(std::string_view s) {
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  std::size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

char to_lower_ascii(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string to_lower_copy(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(to_lower_ascii(c));
  return out;
}

// A small fuzzy matcher to rank command palette results.
// Returns -1 if no match. Higher is better.
int fuzzy_score(std::string_view text, std::string_view query) {
  if (query.empty()) return 0;

  const std::string t = to_lower_copy(text);
  const std::string q = to_lower_copy(query);

  // Fast path: substring match.
  if (t.find(q) != std::string::npos) {
    // Prefer earlier matches and shorter strings.
    const std::size_t pos = t.find(q);
    const int base = 2000;
    return base - static_cast<int>(pos) * 3 - static_cast<int>(t.size());
  }

  // Subsequence match.
  std::size_t ti = 0;
  int score = 0;
  int streak = 0;

  for (std::size_t qi = 0; qi < q.size(); ++qi) {
    const char qc = q[qi];
    bool found = false;
    while (ti < t.size()) {
      if (t[ti] == qc) {
        found = true;
        // Base points per character.
        score += 40;
        // Bonus for consecutive characters.
        if (streak > 0) score += 25;
        ++streak;
        // Bonus when matching at start or after common separators.
        if (ti == 0 || t[ti - 1] == ' ' || t[ti - 1] == '_' || t[ti - 1] == '-' || t[ti - 1] == '/') {
          score += 15;
        }
        ++ti;
        break;
      }
      streak = 0;
      ++ti;
    }
    if (!found) return -1;
  }

  // Prefer shorter strings.
  score -= static_cast<int>(t.size());
  return score;
}

const char* event_level_short(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return "INFO";
    case EventLevel::Warn: return "WARN";
    case EventLevel::Error: return "ERROR";
  }
  return "INFO";
}

ImVec4 event_level_color(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return ImVec4(0.75f, 0.80f, 0.85f, 1.0f);
    case EventLevel::Warn: return ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
    case EventLevel::Error: return ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
  }
  return ImVec4(0.75f, 0.80f, 0.85f, 1.0f);
}

// Computes the (compact) height of the status bar window.
float status_bar_h_px() {
  const ImGuiStyle& style = ImGui::GetStyle();
  return ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
}

// --- command palette ---

enum class PaletteKind { Action, System, Ship, Colony, Body };

enum class PaletteAction {
  ToggleControls,
  ToggleMap,
  ToggleDetails,
  ToggleDirectory,
  ToggleProduction,
  ToggleEconomy,
  ToggleRegions,
  ToggleAdvisor,
  ToggleColonyProfiles,
  ToggleTimeline,
  ToggleDesignStudio,
  ToggleBalanceLab,
  ToggleIntel,
  ToggleDiplomacyGraph,
  ToggleSettings,
  ToggleSaveTools,
  ToggleOmniSearch,
  ToggleJsonExplorer,
  ToggleEntityInspector,
  ToggleReferenceGraph,
  ToggleTimeMachine,
  ToggleWatchboard,
  ToggleDataLenses,
  ToggleDashboards,
  TogglePivotTables,
  ToggleLayoutProfiles,
  ToggleStatusBar,
  ToggleFogOfWar,
  ToggleToasts,
  WorkspaceDefault,
  WorkspaceMinimal,
  WorkspaceEconomy,
  WorkspaceDesign,
  WorkspaceIntel,
  OpenLogTab,
  OpenHelp,
  FocusSystemMap,
  FocusGalaxyMap,
  NewGameDialog,
  NewGameSol,
  NewGameRandom,
  Save,
  Load,
};

struct PaletteItem {
  PaletteKind kind{PaletteKind::Action};
  int score{0};
  std::string label;

  PaletteAction action{PaletteAction::ToggleControls};
  Id id{kInvalidId};
};

void activate_palette_item(PaletteItem& item, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                           Id& selected_body, char* save_path, char* load_path) {
  auto& s = sim.state();

  switch (item.kind) {
    case PaletteKind::Action: {
      switch (item.action) {
        case PaletteAction::ToggleControls:
          ui.show_controls_window = !ui.show_controls_window;
          break;
        case PaletteAction::ToggleMap:
          ui.show_map_window = !ui.show_map_window;
          break;
        case PaletteAction::ToggleDetails:
          ui.show_details_window = !ui.show_details_window;
          break;
        case PaletteAction::ToggleDirectory:
          ui.show_directory_window = !ui.show_directory_window;
          break;
        case PaletteAction::ToggleProduction:
          ui.show_production_window = !ui.show_production_window;
          break;
        case PaletteAction::ToggleEconomy:
          ui.show_economy_window = !ui.show_economy_window;
          break;
        case PaletteAction::ToggleRegions:
          ui.show_regions_window = !ui.show_regions_window;
          break;
        case PaletteAction::ToggleAdvisor:
          ui.show_advisor_window = !ui.show_advisor_window;
          break;
        case PaletteAction::ToggleColonyProfiles:
          ui.show_colony_profiles_window = !ui.show_colony_profiles_window;
          break;
        case PaletteAction::ToggleTimeline:
          ui.show_timeline_window = !ui.show_timeline_window;
          break;
        case PaletteAction::ToggleDesignStudio:
          ui.show_design_studio_window = !ui.show_design_studio_window;
          break;
        case PaletteAction::ToggleBalanceLab:
          ui.show_balance_lab_window = !ui.show_balance_lab_window;
          break;
        case PaletteAction::ToggleIntel:
          ui.show_intel_window = !ui.show_intel_window;
          break;
        case PaletteAction::ToggleDiplomacyGraph:
          ui.show_diplomacy_window = !ui.show_diplomacy_window;
          break;
        case PaletteAction::ToggleSettings:
          ui.show_settings_window = !ui.show_settings_window;
          break;
        case PaletteAction::ToggleSaveTools:
          ui.show_save_tools_window = !ui.show_save_tools_window;
          break;
        case PaletteAction::ToggleOmniSearch:
          ui.show_omni_search_window = !ui.show_omni_search_window;
          break;
        case PaletteAction::ToggleJsonExplorer:
          ui.show_json_explorer_window = !ui.show_json_explorer_window;
          break;
        case PaletteAction::ToggleEntityInspector:
          ui.show_entity_inspector_window = !ui.show_entity_inspector_window;
          break;
        case PaletteAction::ToggleReferenceGraph:
          ui.show_reference_graph_window = !ui.show_reference_graph_window;
          break;
        case PaletteAction::ToggleTimeMachine:
          ui.show_time_machine_window = !ui.show_time_machine_window;
          break;
        case PaletteAction::ToggleWatchboard:
          ui.show_watchboard_window = !ui.show_watchboard_window;
          break;
        case PaletteAction::ToggleDataLenses:
          ui.show_data_lenses_window = !ui.show_data_lenses_window;
          break;
        case PaletteAction::ToggleDashboards:
          ui.show_dashboards_window = !ui.show_dashboards_window;
          break;
        case PaletteAction::TogglePivotTables:
          ui.show_pivot_tables_window = !ui.show_pivot_tables_window;
          break;
        case PaletteAction::ToggleLayoutProfiles:
          ui.show_layout_profiles_window = !ui.show_layout_profiles_window;
          break;
        case PaletteAction::ToggleStatusBar:
          ui.show_status_bar = !ui.show_status_bar;
          break;
        case PaletteAction::ToggleFogOfWar:
          ui.fog_of_war = !ui.fog_of_war;
          break;
        case PaletteAction::ToggleToasts:
          ui.show_event_toasts = !ui.show_event_toasts;
          break;
        case PaletteAction::WorkspaceDefault:
          ui.show_controls_window = true;
          ui.show_map_window = true;
          ui.show_details_window = true;
          ui.show_directory_window = true;
          ui.show_production_window = false;
          ui.show_economy_window = false;
          ui.show_planner_window = false;
          ui.show_freight_window = false;
          ui.show_fuel_window = false;
          ui.show_time_warp_window = false;
          ui.show_timeline_window = false;
          ui.show_design_studio_window = false;
          ui.show_balance_lab_window = false;
          ui.show_intel_window = false;
          ui.show_diplomacy_window = false;
          ui.show_status_bar = true;
          break;
        case PaletteAction::WorkspaceMinimal:
          ui.show_controls_window = false;
          ui.show_map_window = true;
          ui.show_details_window = true;
          ui.show_directory_window = false;
          ui.show_production_window = false;
          ui.show_economy_window = false;
          ui.show_planner_window = false;
          ui.show_freight_window = false;
          ui.show_fuel_window = false;
          ui.show_time_warp_window = false;
          ui.show_timeline_window = false;
          ui.show_design_studio_window = false;
          ui.show_balance_lab_window = false;
          ui.show_intel_window = false;
          ui.show_diplomacy_window = false;
          ui.show_status_bar = true;
          break;
        case PaletteAction::WorkspaceEconomy:
          ui.show_controls_window = false;
          ui.show_map_window = true;
          ui.show_details_window = true;
          ui.show_directory_window = true;
          ui.show_production_window = true;
          ui.show_economy_window = true;
          ui.show_planner_window = true;
          ui.show_freight_window = false;
          ui.show_fuel_window = false;
          ui.show_time_warp_window = false;
          ui.show_timeline_window = true;
          ui.show_design_studio_window = false;
          ui.show_balance_lab_window = false;
          ui.show_intel_window = false;
          ui.show_diplomacy_window = false;
          ui.show_status_bar = true;
          break;
        case PaletteAction::WorkspaceDesign:
          ui.show_controls_window = false;
          ui.show_map_window = true;
          ui.show_details_window = true;
          ui.show_directory_window = false;
          ui.show_production_window = false;
          ui.show_economy_window = false;
          ui.show_planner_window = false;
          ui.show_freight_window = false;
          ui.show_fuel_window = false;
          ui.show_time_warp_window = false;
          ui.show_timeline_window = false;
          ui.show_design_studio_window = true;
          ui.show_balance_lab_window = true;
          ui.show_intel_window = false;
          ui.show_diplomacy_window = false;
          ui.show_status_bar = true;
          break;
        case PaletteAction::WorkspaceIntel:
          ui.show_controls_window = false;
          ui.show_map_window = true;
          ui.show_details_window = true;
          ui.show_directory_window = false;
          ui.show_production_window = false;
          ui.show_economy_window = false;
          ui.show_planner_window = false;
          ui.show_freight_window = false;
          ui.show_fuel_window = false;
          ui.show_time_warp_window = false;
          ui.show_timeline_window = true;
          ui.show_design_studio_window = false;
          ui.show_balance_lab_window = false;
          ui.show_intel_window = true;
          ui.show_diplomacy_window = true;
          ui.show_status_bar = true;
          break;
        case PaletteAction::OpenLogTab:
          ui.show_details_window = true;
          ui.request_details_tab = DetailsTab::Log;
          break;
        case PaletteAction::OpenHelp:
          ui.show_help_window = true;
          break;
        case PaletteAction::FocusSystemMap:
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::System;
          break;
        case PaletteAction::FocusGalaxyMap:
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::Galaxy;
          break;
        case PaletteAction::NewGameDialog:
          ui.show_new_game_modal = true;
          break;
        case PaletteAction::NewGameSol:
          sim.new_game();
          ui.request_map_tab = MapTab::System;
          break;
        case PaletteAction::NewGameRandom:
          sim.new_game_random(ui.new_game_random_seed, ui.new_game_random_num_systems);
          ui.request_map_tab = MapTab::Galaxy;
          break;
        case PaletteAction::Save:
          try {
            nebula4x::write_text_file(save_path, serialize_game_to_json(sim.state()));
          } catch (const std::exception& e) {
            nebula4x::log::error(std::string("Save failed: ") + e.what());
          }
          break;
        case PaletteAction::Load:
          try {
            sim.load_game(deserialize_game_from_json(nebula4x::read_text_file(load_path)));
            // Best-effort: clear potentially-stale selections.
            selected_ship = kInvalidId;
            selected_colony = kInvalidId;
            selected_body = kInvalidId;
          } catch (const std::exception& e) {
            nebula4x::log::error(std::string("Load failed: ") + e.what());
          }
          break;
      }
      break;
    }

    case PaletteKind::System: {
      s.selected_system = item.id;
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::System;
      break;
    }

    case PaletteKind::Ship: {
      selected_ship = item.id;
      ui.selected_fleet_id = sim.fleet_for_ship(item.id);
      if (const auto* sh = find_ptr(s.ships, item.id)) {
        s.selected_system = sh->system_id;
      }
      ui.show_details_window = true;
      ui.request_details_tab = DetailsTab::Ship;
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::System;
      break;
    }

    case PaletteKind::Colony: {
      selected_colony = item.id;
      if (const auto* c = find_ptr(s.colonies, item.id)) {
        selected_body = c->body_id;
        if (const auto* b = find_ptr(s.bodies, c->body_id)) {
          s.selected_system = b->system_id;
        }
      }
      ui.show_details_window = true;
      ui.request_details_tab = DetailsTab::Colony;
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::System;
      break;
    }

    case PaletteKind::Body: {
      selected_body = item.id;
      if (const auto* b = find_ptr(s.bodies, item.id)) {
        s.selected_system = b->system_id;
      }
      ui.show_details_window = true;
      ui.request_details_tab = DetailsTab::Body;
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::System;
      break;
    }
  }
}

} // namespace

void draw_status_bar(Simulation& sim, UIState& ui, HUDState& /*hud*/, Id& selected_ship, Id& selected_colony,
                     Id& selected_body, char* save_path, char* load_path) {
  if (!ui.show_status_bar) return;

  ImGuiIO& io = ImGui::GetIO();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float h = status_bar_h_px();

  ImGui::SetNextWindowPos(ImVec2(0.0f, io.DisplaySize.y - h), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, h), ImGuiCond_Always);

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, 4.0f));

  if (!ImGui::Begin("##status_bar", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar(3);
    return;
  }

  // --- Quick turn controls ---
  if (ImGui::SmallButton("+1h")) sim.advance_hours(1);
  ImGui::SameLine();
  if (ImGui::SmallButton("+6h")) sim.advance_hours(6);
  ImGui::SameLine();
  if (ImGui::SmallButton("+12h")) sim.advance_hours(12);
  VerticalSeparator();
  if (ImGui::SmallButton("+1d")) sim.advance_days(1);
  ImGui::SameLine();
  if (ImGui::SmallButton("+5d")) sim.advance_days(5);
  ImGui::SameLine();
  if (ImGui::SmallButton("+30d")) sim.advance_days(30);

  ImGui::SameLine();
  if (ImGui::SmallButton("Freight")) ui.show_freight_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open the Freight Planner (auto-freight preview)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Fuel")) ui.show_fuel_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open the Fuel Planner (auto-tanker preview)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Advisor")) ui.show_advisor_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open the Advisor (issues + quick fixes)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Profiles")) ui.show_colony_profiles_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open Colony Profiles (automation presets)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Warp")) ui.show_time_warp_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Time warp until an event matches your filter");
  }

  VerticalSeparator();

  if (ImGui::SmallButton("Save")) {
    try {
      nebula4x::write_text_file(save_path, serialize_game_to_json(sim.state()));
    } catch (const std::exception& e) {
      nebula4x::log::error(std::string("Save failed: ") + e.what());
    }
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Load")) {
    try {
      sim.load_game(deserialize_game_from_json(nebula4x::read_text_file(load_path)));
      // Best-effort clear of potentially-stale selections.
      selected_ship = kInvalidId;
      selected_colony = kInvalidId;
      selected_body = kInvalidId;
    } catch (const std::exception& e) {
      nebula4x::log::error(std::string("Load failed: ") + e.what());
    }
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("AutoSave")) {
    ui.request_autosave_game_now = true;
  }
  if (ImGui::IsItemHovered()) {
    std::string tip = "Write an autosave snapshot now.";
    tip += "\nInterval: " + std::to_string(ui.autosave_game_interval_hours) + "h";
    tip += "\nKeep: " + std::to_string(ui.autosave_game_keep_files);
    tip += "\nDir: " + std::string(ui.autosave_game_dir);
    if (!ui.last_autosave_game_error.empty()) {
      tip += "\n\nLast error: " + ui.last_autosave_game_error;
    } else if (!ui.last_autosave_game_path.empty()) {
      tip += "\n\nLast autosave: " + ui.last_autosave_game_path;
    }
    ImGui::SetTooltip("%s", tip.c_str());
  }

  VerticalSeparator();

  if (ImGui::SmallButton("Palette")) ui.show_command_palette = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Command Palette (Ctrl+P)");
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Search")) ui.show_omni_search_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("OmniSearch (Ctrl+F)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Entity")) ui.show_entity_inspector_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Entity Inspector (Ctrl+G)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Graph")) {
    ui.show_reference_graph_window = true;
    if (ui.reference_graph_focus_id == 0) {
      // Try to seed focus from the current selection.
      if (selected_ship != 0) ui.reference_graph_focus_id = selected_ship;
      else if (selected_colony != 0) ui.reference_graph_focus_id = selected_colony;
      else if (selected_body != 0) ui.reference_graph_focus_id = selected_body;
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reference Graph (Ctrl+Shift+G)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("History")) ui.show_time_machine_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Time Machine (Ctrl+Shift+D)\nCapture state snapshots + inspect diffs.");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Help")) ui.show_help_window = !ui.show_help_window;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Help / Shortcuts (F1)");
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Settings")) ui.show_settings_window = true;

  VerticalSeparator();

  // --- Context / indicators ---
  auto& s = sim.state();
  const std::string dt = format_datetime(s.date, s.hour_of_day);
  ImGui::Text("Date: %s", dt.c_str());

  const auto* sys = find_ptr(s.systems, s.selected_system);
  if (sys) {
    ImGui::SameLine();
    ImGui::TextDisabled(" | System: %s", sys->name.c_str());
  }

  if (selected_ship != kInvalidId) {
    if (const auto* sh = find_ptr(s.ships, selected_ship)) {
      ImGui::SameLine();
      ImGui::TextDisabled(" | Ship: %s", sh->name.c_str());
    }
  } else if (selected_colony != kInvalidId) {
    if (const auto* c = find_ptr(s.colonies, selected_colony)) {
      ImGui::SameLine();
      ImGui::TextDisabled(" | Colony: %s", c->name.c_str());
    }
  }

  // Fog-of-war indicator (clickable).
  VerticalSeparator();
  if (ImGui::Checkbox("FoW", &ui.fog_of_war)) {
    // no-op
  }

  // Unread events indicator.
  const std::uint64_t newest_seq = (s.next_event_seq > 0) ? (s.next_event_seq - 1) : 0;
  if (ui.last_seen_event_seq > newest_seq) ui.last_seen_event_seq = 0;

  int unread = 0;
  for (const auto& ev : s.events) {
    if (ev.seq > ui.last_seen_event_seq) ++unread;
  }

  if (unread > 0) {
    ImGui::SameLine();
    VerticalSeparator();
    ImGui::SameLine();
    std::string b = "Log (" + std::to_string(unread) + ")";
    if (ImGui::SmallButton(b.c_str())) {
      ui.show_details_window = true;
      ui.request_details_tab = DetailsTab::Log;
    }
  }

  ImGui::End();
  ImGui::PopStyleVar(3);
}

void draw_help_window(UIState& ui) {
  if (!ui.show_help_window) return;

  ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Help", &ui.show_help_window)) {
    ImGui::End();
    return;
  }

  ImGui::SeparatorText("Global shortcuts");
  ImGui::BulletText("Ctrl+P: Command palette (search actions, systems, ships, colonies, bodies)");
  ImGui::BulletText("Ctrl+F: OmniSearch (global game JSON search)");
  ImGui::BulletText("Ctrl+G: Entity Inspector (resolve an entity id + find inbound refs)");
  ImGui::BulletText("Ctrl+Shift+G: Reference Graph (visualize id relationships)");
  ImGui::BulletText("Ctrl+Shift+D: Time Machine (state history + diffs)");
  ImGui::BulletText("Ctrl+Shift+A: Advisor (issues + quick fixes)");
  ImGui::BulletText("Ctrl+Shift+B: Colony Profiles (automation presets)");
  ImGui::BulletText("F1: Toggle this help window");
  ImGui::BulletText("Ctrl+S: Save (uses current save path)");
  ImGui::BulletText("Ctrl+O: Load (uses current load path)");
  ImGui::BulletText("Space: Advance +1 day (Shift=+5, Ctrl=+30)");
  ImGui::BulletText("Ctrl+Shift+L: Layout Profiles (switch dock layouts)");

  ImGui::SeparatorText("Window toggles");
  ImGui::BulletText("Drag window tabs to dock/undock and rearrange the workspace");
  ImGui::BulletText("Ctrl+1: Controls");
  ImGui::BulletText("Ctrl+2: Map");
  ImGui::BulletText("Ctrl+3: Details");
  ImGui::BulletText("Ctrl+4: Directory");
  ImGui::BulletText("Ctrl+5: Economy");
  ImGui::BulletText("Ctrl+6: Production");
  ImGui::BulletText("Ctrl+7: Timeline");
  ImGui::BulletText("Ctrl+8: Design Studio");
  ImGui::BulletText("Ctrl+9: Intel");
  ImGui::BulletText("Ctrl+0: Diplomacy Graph");
  ImGui::BulletText("Ctrl+,: Settings");

  ImGui::SeparatorText("Map controls");
  ImGui::BulletText("Mouse wheel: zoom");
  ImGui::BulletText("Middle mouse drag: pan");
  ImGui::BulletText("System map: Left click = issue order, Right click = select");
  ImGui::BulletText("Galaxy map: Left click = select system, Right click = route ship (Shift queues)");

  ImGui::SeparatorText("Notes");
  ImGui::TextWrapped(
      "The Command Palette and event toasts are UI-only helpers. They do not change the simulation; they "
      "just help you navigate and respond to what is happening.");

  ImGui::End();
}

void draw_command_palette(Simulation& sim, UIState& ui, HUDState& hud, Id& selected_ship, Id& selected_colony,
                          Id& selected_body, char* save_path, char* load_path) {
  if (!ui.show_command_palette) return;

  // Center-ish near the top, like common palettes.
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 pos(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.18f);

  ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(720, 420), ImGuiCond_Appearing);

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

  if (!ImGui::Begin("Command Palette", &ui.show_command_palette, flags)) {
    ImGui::End();
    return;
  }

  static bool was_open = false;
  const bool just_opened = !was_open;
  was_open = ui.show_command_palette;

  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ui.show_command_palette = false;
    ImGui::End();
    return;
  }

  if (just_opened) {
    hud.palette_selected_idx = 0;
  }

  bool enter_pressed = false;
  {
    ImGuiInputTextFlags tf = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
    if (just_opened) {
      ImGui::SetKeyboardFocusHere();
    }
    enter_pressed = ImGui::InputTextWithHint("##palette_query", "Type to search (actions, systems, ships, colonies, bodies)",
                                             hud.palette_query, IM_ARRAYSIZE(hud.palette_query), tf);
  }

  const std::string query = trim_copy(hud.palette_query);

  // Build results.
  std::vector<PaletteItem> items;
  items.reserve(128);

  auto add_action = [&](const char* label, PaletteAction action) {
    const int sc = query.empty() ? 500 : fuzzy_score(label, query);
    if (query.empty() || sc >= 0) {
      PaletteItem it;
      it.kind = PaletteKind::Action;
      it.action = action;
      it.label = label;
      it.score = sc;
      items.push_back(std::move(it));
    }
  };

  // Actions (always helpful).
  add_action("[Action] Toggle Controls window", PaletteAction::ToggleControls);
  add_action("[Action] Toggle Map window", PaletteAction::ToggleMap);
  add_action("[Action] Toggle Details window", PaletteAction::ToggleDetails);
  add_action("[Action] Toggle Directory window", PaletteAction::ToggleDirectory);
  add_action("[Action] Toggle Production window", PaletteAction::ToggleProduction);
  add_action("[Action] Toggle Economy window", PaletteAction::ToggleEconomy);
  add_action("[Action] Toggle Regions (Sectors Overview)", PaletteAction::ToggleRegions);
  add_action("[Action] Toggle Advisor (Issues)", PaletteAction::ToggleAdvisor);
  add_action("[Action] Toggle Colony Profiles (Automation Presets)", PaletteAction::ToggleColonyProfiles);
  add_action("[Action] Toggle Timeline window", PaletteAction::ToggleTimeline);
  add_action("[Action] Toggle Design Studio window", PaletteAction::ToggleDesignStudio);
  add_action("[Action] Toggle Balance Lab window", PaletteAction::ToggleBalanceLab);
  add_action("[Action] Toggle Intel window", PaletteAction::ToggleIntel);
  add_action("[Action] Toggle Diplomacy Graph window", PaletteAction::ToggleDiplomacyGraph);
  add_action("[Action] Open Settings", PaletteAction::ToggleSettings);
  add_action("[Action] Toggle Save Tools window", PaletteAction::ToggleSaveTools);
  add_action("[Action] Toggle Time Machine (State History)", PaletteAction::ToggleTimeMachine);
  add_action("[Action] Toggle OmniSearch (Game JSON)", PaletteAction::ToggleOmniSearch);
  add_action("[Action] Toggle JSON Explorer window", PaletteAction::ToggleJsonExplorer);
  add_action("[Action] Toggle Entity Inspector (ID Resolver)", PaletteAction::ToggleEntityInspector);
  add_action("[Action] Toggle Reference Graph (Entity IDs)", PaletteAction::ToggleReferenceGraph);
  add_action("[Action] Toggle Watchboard (JSON Pins)", PaletteAction::ToggleWatchboard);
  add_action("[Action] Toggle Data Lenses window", PaletteAction::ToggleDataLenses);
  add_action("[Action] Toggle Dashboards window", PaletteAction::ToggleDashboards);
  add_action("[Action] Toggle Pivot Tables window", PaletteAction::TogglePivotTables);
  add_action("[Action] Toggle Layout Profiles window", PaletteAction::ToggleLayoutProfiles);
  add_action("[Action] Toggle Status Bar", PaletteAction::ToggleStatusBar);
  add_action("[Action] Toggle Fog of War", PaletteAction::ToggleFogOfWar);
  add_action("[Action] Toggle Event Toasts", PaletteAction::ToggleToasts);
  add_action("[Action] Workspace: Default", PaletteAction::WorkspaceDefault);
  add_action("[Action] Workspace: Minimal", PaletteAction::WorkspaceMinimal);
  add_action("[Action] Workspace: Economy", PaletteAction::WorkspaceEconomy);
  add_action("[Action] Workspace: Design", PaletteAction::WorkspaceDesign);
  add_action("[Action] Workspace: Intel", PaletteAction::WorkspaceIntel);
  add_action("[Action] Open Event Log", PaletteAction::OpenLogTab);
  add_action("[Action] Help / Shortcuts", PaletteAction::OpenHelp);
  add_action("[Action] Focus System Map", PaletteAction::FocusSystemMap);
  add_action("[Action] Focus Galaxy Map", PaletteAction::FocusGalaxyMap);
  add_action("[Action] New Game...", PaletteAction::NewGameDialog);
  add_action("[Action] New Game (Sol)", PaletteAction::NewGameSol);
  add_action("[Action] New Game (Random)", PaletteAction::NewGameRandom);
  add_action("[Action] Save game", PaletteAction::Save);
  add_action("[Action] Load game", PaletteAction::Load);

  const auto& s = sim.state();

  // Only add entity hits when the user typed something.
  if (!query.empty()) {
    // Systems
    for (const auto& [sid, sys] : s.systems) {
      std::string label = "[System] " + sys.name;
      const int sc = fuzzy_score(label, query);
      if (sc < 0) continue;
      PaletteItem it;
      it.kind = PaletteKind::System;
      it.id = sid;
      it.label = std::move(label);
      it.score = sc;
      items.push_back(std::move(it));
    }

    // Ships
    for (const auto& [shid, sh] : s.ships) {
      const auto* sys = find_ptr(s.systems, sh.system_id);
      std::string label = "[Ship] " + sh.name;
      if (sys) label += "  (" + sys->name + ")";
      const int sc = fuzzy_score(label, query);
      if (sc < 0) continue;
      PaletteItem it;
      it.kind = PaletteKind::Ship;
      it.id = shid;
      it.label = std::move(label);
      it.score = sc;
      items.push_back(std::move(it));
    }

    // Colonies
    for (const auto& [cid, c] : s.colonies) {
      const auto* b = (c.body_id != kInvalidId) ? find_ptr(s.bodies, c.body_id) : nullptr;
      const auto* sys = (b && b->system_id != kInvalidId) ? find_ptr(s.systems, b->system_id) : nullptr;
      std::string label = "[Colony] " + c.name;
      if (sys) label += "  (" + sys->name + ")";
      const int sc = fuzzy_score(label, query);
      if (sc < 0) continue;
      PaletteItem it;
      it.kind = PaletteKind::Colony;
      it.id = cid;
      it.label = std::move(label);
      it.score = sc;
      items.push_back(std::move(it));
    }

    // Bodies
    for (const auto& [bid, b] : s.bodies) {
      const auto* sys = (b.system_id != kInvalidId) ? find_ptr(s.systems, b.system_id) : nullptr;
      std::string label = "[Body] " + b.name;
      if (sys) label += "  (" + sys->name + ")";
      const int sc = fuzzy_score(label, query);
      if (sc < 0) continue;
      PaletteItem it;
      it.kind = PaletteKind::Body;
      it.id = bid;
      it.label = std::move(label);
      it.score = sc;
      items.push_back(std::move(it));
    }
  }

  // Sort by score (desc), then label.
  std::sort(items.begin(), items.end(), [](const PaletteItem& a, const PaletteItem& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.label < b.label;
  });

  constexpr int kMaxItems = 80;
  if ((int)items.size() > kMaxItems) items.resize(kMaxItems);

  if (items.empty()) {
    ImGui::Spacing();
    ImGui::TextDisabled("No matches.");
    ImGui::TextDisabled("Tip: Try a substring (e.g. 'Sol', 'Survey', 'Colony').");
    ImGui::End();
    return;
  }

  // Keyboard navigation.
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
    hud.palette_selected_idx = std::min(hud.palette_selected_idx + 1, (int)items.size() - 1);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
    hud.palette_selected_idx = std::max(hud.palette_selected_idx - 1, 0);
  }

  ImGui::Separator();

  ImGui::BeginChild("##palette_results", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);

  int clicked_idx = -1;
  for (int i = 0; i < (int)items.size(); ++i) {
    const bool sel = (i == hud.palette_selected_idx);
    if (ImGui::Selectable(items[i].label.c_str(), sel)) {
      clicked_idx = i;
    }

    // Ensure the selected row stays visible when navigating.
    if (sel && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {
      ImGui::SetScrollHereY(0.5f);
    }
  }

  ImGui::EndChild();

  ImGui::TextDisabled("Enter: apply  |  Esc: close  |  ↑/↓: navigate");

  // Activation.
  const bool do_activate = enter_pressed || (clicked_idx >= 0);
  if (do_activate) {
    const int idx = (clicked_idx >= 0) ? clicked_idx : std::clamp(hud.palette_selected_idx, 0, (int)items.size() - 1);
    PaletteItem item = items[static_cast<std::size_t>(idx)];
    activate_palette_item(item, sim, ui, selected_ship, selected_colony, selected_body, save_path, load_path);
    ui.show_command_palette = false;

    // Keep the query for rapid repeated use, but reset selection.
    hud.palette_selected_idx = 0;
  }

  ImGui::End();
}

void update_event_toasts(const Simulation& sim, UIState& ui, HUDState& hud) {
  if (!ui.show_event_toasts) {
    hud.toasts.clear();
    return;
  }

  const auto& s = sim.state();
  const std::uint64_t newest_seq = (s.next_event_seq > 0) ? (s.next_event_seq - 1) : 0;
  if (hud.last_toast_seq > newest_seq) hud.last_toast_seq = 0;

  // Gather new events since last_toast_seq (iterate from the back for efficiency).
  std::vector<const SimEvent*> new_events;
  new_events.reserve(16);

  for (auto it = s.events.rbegin(); it != s.events.rend(); ++it) {
    if (it->seq <= hud.last_toast_seq) break;
    new_events.push_back(&(*it));
  }

  if (!new_events.empty()) {
    std::reverse(new_events.begin(), new_events.end());

    const double now = ImGui::GetTime();

    for (const SimEvent* ev : new_events) {
      // By default, show only warn/error toasts to keep noise down.
      if (ev->level == EventLevel::Info) continue;

      EventToast t;
      t.seq = ev->seq;
      t.day = ev->day;
      t.level = ev->level;
      t.category = ev->category;
      t.faction_id = ev->faction_id;
      t.faction_id2 = ev->faction_id2;
      t.system_id = ev->system_id;
      t.ship_id = ev->ship_id;
      t.colony_id = ev->colony_id;
      t.message = ev->message;
      t.created_time_s = now;
      hud.toasts.push_back(std::move(t));
    }

    hud.last_toast_seq = newest_seq;

    // Cap toast backlog.
    constexpr std::size_t kMaxToasts = 6;
    if (hud.toasts.size() > kMaxToasts) {
      hud.toasts.erase(hud.toasts.begin(), hud.toasts.begin() + (hud.toasts.size() - kMaxToasts));
    }
  }
}

void draw_event_toasts(Simulation& sim, UIState& ui, HUDState& hud, Id& selected_ship, Id& selected_colony,
                       Id& selected_body) {
  if (!ui.show_event_toasts) return;
  if (hud.toasts.empty()) return;

  ImGuiIO& io = ImGui::GetIO();

  // Stack from top-right.
  const float margin = 10.0f;
  const float top = ImGui::GetFrameHeight() + margin;
  float y = top;

  // Expire old toasts.
  const double now = ImGui::GetTime();
  const double ttl = std::max(0.5, (double)ui.event_toast_duration_sec);

  hud.toasts.erase(std::remove_if(hud.toasts.begin(), hud.toasts.end(), [&](const EventToast& t) {
                    return (now - t.created_time_s) > ttl;
                  }),
                  hud.toasts.end());

  // Draw after pruning.
  for (std::size_t i = 0; i < hud.toasts.size(); ++i) {
    EventToast& t = hud.toasts[i];

    const std::string name = "##toast_" + std::to_string(static_cast<unsigned long long>(t.seq));

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove;

    ImGui::Begin(name.c_str(), nullptr, flags);

    // Header
    ImGui::TextColored(event_level_color(t.level), "%s", event_level_short(t.level));
    ImGui::SameLine();
    ImGui::TextDisabled("#%llu", (unsigned long long)t.seq);

    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
      ImGui::End();
      hud.toasts.erase(hud.toasts.begin() + static_cast<std::ptrdiff_t>(i));
      // Restart loop to avoid invalidated references.
      i = static_cast<std::size_t>(-1);
      y = top;
      continue;
    }

    const nebula4x::Date d(t.day);
    ImGui::TextDisabled("%s", d.to_string().c_str());

    ImGui::Separator();
    ImGui::TextWrapped("%s", t.message.c_str());

    ImGui::Separator();

    // Quick actions.
    if (ImGui::SmallButton("Log")) {
      ui.show_details_window = true;
      ui.request_details_tab = DetailsTab::Log;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Timeline")) {
      ui.show_timeline_window = true;
      ui.request_focus_event_seq = t.seq;
    }

    auto& s = sim.state();

    if (t.system_id != kInvalidId) {
      ImGui::SameLine();
      if (ImGui::SmallButton("View system")) {
        s.selected_system = t.system_id;
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::System;
      }
    }

    if (t.colony_id != kInvalidId) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Select colony")) {
        selected_colony = t.colony_id;
        if (const auto* c = find_ptr(s.colonies, t.colony_id)) {
          selected_body = c->body_id;
          if (const auto* b = (c->body_id != kInvalidId) ? find_ptr(s.bodies, c->body_id) : nullptr) {
            s.selected_system = b->system_id;
          }
        }
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Colony;
      }
    }

    if (t.ship_id != kInvalidId) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Select ship")) {
        selected_ship = t.ship_id;
        ui.selected_fleet_id = sim.fleet_for_ship(t.ship_id);
        if (const auto* sh = find_ptr(s.ships, t.ship_id)) {
          s.selected_system = sh->system_id;
        }
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Ship;
      }
    }

    // Advance stack.
    const ImVec2 win_sz = ImGui::GetWindowSize();
    y += win_sz.y + 8.0f;

    ImGui::End();
  }
}

} // namespace nebula4x::ui
