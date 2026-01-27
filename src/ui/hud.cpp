#include "ui/hud.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula4x/core/date.h"
#include "nebula4x/core/tech.h"
#include "nebula4x/core/serialization.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/time.h"
#include "nebula4x/util/strings.h"

#include "ui/screen_reader.h"

#include "ui/docs_browser.h"

#include "ui/guided_tour.h"

#include "ui/navigation.h"

#include "ui/notifications.h"

#include "ui/hotkeys.h"
#include "ui/window_management.h"

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
  ToggleFleetManager,
  ToggleRegions,
  ToggleAdvisor,
  ToggleColonyProfiles,
  ToggleShipProfiles,
  ToggleAutomationCenter,
  ToggleShipyardTargets,
  ToggleSurveyNetwork,
  ToggleTimeline,
  ToggleNotifications,
  ToggleDesignStudio,
  ToggleBalanceLab,
  ToggleIntel,
  ToggleIntelNotebook,
  ToggleDiplomacyGraph,
  ToggleSettings,
  ToggleSaveTools,
  ToggleOmniSearch,
  ToggleJsonExplorer,
  ToggleContentValidation,
  ToggleStateDoctor,
  ToggleEntityInspector,
  ToggleReferenceGraph,
  ToggleTimeMachine,
  ToggleCompare,
  ToggleNavigator,
  ToggleWatchboard,
  ToggleDataLenses,
  ToggleDashboards,
  TogglePivotTables,
  ToggleUIForge,
  ToggleLayoutProfiles,
  ToggleWindowManager,
  ToggleStatusBar,
  ToggleFogOfWar,
  ToggleToasts,
  ToggleFocusMode,
  WorkspaceDefault,
  WorkspaceMinimal,
  WorkspaceEconomy,
  WorkspaceDesign,
  WorkspaceIntel,
  OpenLogTab,
  OpenHelp,
  NavBack,
  NavForward,
  ToggleBookmarkCurrent,
  FocusSystemMap,
  FocusGalaxyMap,
  NewGameDialog,
  NewGameSol,
  NewGameRandom,
  ReloadContent,
  Save,
  Load,

  // Sentinel (not a real action).
  Count,
};

struct PaletteItem {
  PaletteKind kind{PaletteKind::Action};
  int score{0};
  std::string label;

  PaletteAction action{PaletteAction::ToggleControls};
  Id id{kInvalidId};
};

void remember_action_recent(UIState& ui, PaletteAction a);

void activate_palette_item(PaletteItem& item, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                           Id& selected_body, char* save_path, char* load_path) {
  auto& s = sim.state();

  switch (item.kind) {
    case PaletteKind::Action: {
      remember_action_recent(ui, item.action);
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
        case PaletteAction::ToggleFleetManager:
          ui.show_fleet_manager_window = !ui.show_fleet_manager_window;
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
        case PaletteAction::ToggleShipProfiles:
          ui.show_ship_profiles_window = !ui.show_ship_profiles_window;
          break;
        case PaletteAction::ToggleAutomationCenter:
          ui.show_automation_center_window = !ui.show_automation_center_window;
          break;
        case PaletteAction::ToggleShipyardTargets:
          ui.show_shipyard_targets_window = !ui.show_shipyard_targets_window;
          break;
        case PaletteAction::ToggleSurveyNetwork:
          ui.show_survey_network_window = !ui.show_survey_network_window;
          break;
        case PaletteAction::ToggleTimeline:
          ui.show_timeline_window = !ui.show_timeline_window;
          break;
        case PaletteAction::ToggleNotifications:
          ui.show_notifications_window = !ui.show_notifications_window;
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
        case PaletteAction::ToggleIntelNotebook:
          ui.show_intel_notebook_window = !ui.show_intel_notebook_window;
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
        case PaletteAction::ToggleContentValidation:
          ui.show_content_validation_window = !ui.show_content_validation_window;
          break;
        case PaletteAction::ToggleStateDoctor:
          ui.show_state_doctor_window = !ui.show_state_doctor_window;
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
        case PaletteAction::ToggleNavigator:
          ui.show_navigator_window = !ui.show_navigator_window;
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
        case PaletteAction::ToggleUIForge:
          ui.show_ui_forge_window = !ui.show_ui_forge_window;
          break;
        case PaletteAction::ToggleLayoutProfiles:
          ui.show_layout_profiles_window = !ui.show_layout_profiles_window;
          break;
        case PaletteAction::ToggleWindowManager:
          ui.show_window_manager_window = !ui.show_window_manager_window;
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
        case PaletteAction::ToggleFocusMode:
          toggle_focus_mode(ui);
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
          ui.show_mine_window = false;
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
          ui.show_mine_window = false;
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
          ui.show_mine_window = false;
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
          ui.show_mine_window = false;
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
          ui.show_mine_window = false;
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
        case PaletteAction::NavBack:
          nav_history_back(sim, ui, selected_ship, selected_colony, selected_body, ui.nav_open_windows_on_jump);
          break;
        case PaletteAction::NavForward:
          nav_history_forward(sim, ui, selected_ship, selected_colony, selected_body, ui.nav_open_windows_on_jump);
          break;
        case PaletteAction::ToggleBookmarkCurrent:
          nav_bookmark_toggle_current(sim, ui, selected_ship, selected_colony, selected_body);
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
        case PaletteAction::ReloadContent:
          try {
            std::vector<std::string> content_paths = sim.content().content_source_paths;
            if (content_paths.empty()) content_paths.push_back("data/blueprints/starting_blueprints.json");
            std::vector<std::string> tech_paths = sim.content().tech_source_paths;
            if (tech_paths.empty()) tech_paths.push_back("data/tech/tech_tree.json");

            auto new_content = nebula4x::load_content_db_from_files(content_paths);
            new_content.techs = nebula4x::load_tech_db_from_files(tech_paths);
            new_content.tech_source_paths = tech_paths;
            if (new_content.content_source_paths.empty()) new_content.content_source_paths = content_paths;

            auto res = sim.reload_content_db(std::move(new_content), true);
            if (!res.ok) {
              nebula4x::log::error("Hot Reload: failed (" + std::to_string(res.errors.size()) + " errors)");
            } else if (!res.warnings.empty()) {
              nebula4x::log::warn("Hot Reload: applied with " + std::to_string(res.warnings.size()) + " warning(s)");
            } else {
              nebula4x::log::info("Hot Reload: applied");
            }
          } catch (const std::exception& e) {
            nebula4x::log::error(std::string("Hot Reload failed: ") + e.what());
          }
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



// --- command console metadata (labels, categories, tooltips) ---

struct ActionMeta {
  PaletteAction action;
  const char* category;
  const char* label;
  const char* tooltip;   // may be nullptr (falls back to label)
  const char* shortcut;  // may be nullptr
  const char* keywords;  // may be nullptr
  bool toggles{false};
};

// NOTE: This table acts as lightweight "reflection" for command actions.
// It drives both the collapsible panels (browse mode) and auto-generated tooltips.
constexpr ActionMeta kActionMetas[] = {
    // Navigation
    {PaletteAction::FocusSystemMap, "Navigation", "Focus System Map", "Switch the Map window to the System tab.", nullptr,
     "map system", false},
    {PaletteAction::FocusGalaxyMap, "Navigation", "Focus Galaxy Map", "Switch the Map window to the Galaxy tab.", nullptr,
     "map galaxy", false},
    {PaletteAction::OpenLogTab, "Navigation", "Open Event Log", "Open the Details window on the Event Log tab.", nullptr,
     "log events", false},
    {PaletteAction::OpenHelp, "Navigation", "Help / Shortcuts", "Open the shortcuts/help overlay.", "F1",
     "help shortcuts", false},

    {PaletteAction::ToggleNavigator, "Navigation", "Navigator window",
     "Open the Navigator window (selection history + bookmarks).", "Ctrl+Shift+N", "navigator history bookmarks pin", true},
    {PaletteAction::NavBack, "Navigation", "Back (Selection History)",
     "Navigate back through selection history.", "Alt+Left", "back history previous selection", false},
    {PaletteAction::NavForward, "Navigation", "Forward (Selection History)",
     "Navigate forward through selection history.", "Alt+Right", "forward history next selection", false},
    {PaletteAction::ToggleBookmarkCurrent, "Navigation", "Pin/Unpin Current Selection",
     "Toggle a pinned bookmark for your current selection.", nullptr, "pin unpin bookmark favorite", false},

    // Windows
    {PaletteAction::ToggleControls, "Windows", "Controls window", "Show/hide the Controls window.", "Ctrl+1",
     "controls", true},
    {PaletteAction::ToggleMap, "Windows", "Map window", "Show/hide the Map window.", "Ctrl+2", "map", true},
    {PaletteAction::ToggleDetails, "Windows", "Details window", "Show/hide the Details window.", "Ctrl+3",
     "details", true},
    {PaletteAction::ToggleDirectory, "Windows", "Directory window", "Show/hide the Directory window.", "Ctrl+4",
     "directory", true},
    {PaletteAction::ToggleEconomy, "Windows", "Economy window", "Show/hide the Economy window.", "Ctrl+5", "economy",
     true},
    {PaletteAction::ToggleProduction, "Windows", "Production window", "Show/hide the Production window.", "Ctrl+6",
     "production", true},
    {PaletteAction::ToggleTimeline, "Windows", "Timeline window", "Show/hide the Timeline window.", "Ctrl+7",
     "timeline", true},
    {PaletteAction::ToggleNotifications, "Windows", "Notification Center", "Show/hide the Notification Center inbox.", "F3",
     "notifications inbox alerts", true},
    {PaletteAction::ToggleDesignStudio, "Windows", "Design Studio window", "Show/hide the Design Studio.", "Ctrl+8",
     "design", true},
    {PaletteAction::ToggleBalanceLab, "Windows", "Balance Lab window", "Show/hide the Balance Lab (combat/economy tuning sandbox).", nullptr,
     "balance lab", true},
    {PaletteAction::ToggleIntel, "Windows", "Intel window", "Show/hide the Intel window.", "Ctrl+9", "intel", true},
    {PaletteAction::ToggleIntelNotebook, "Windows", "Intel Notebook",
     "Unified knowledge-base: system notes + curated journal (tags, pins, export).", "Ctrl+Shift+I",
     "notebook notes journal intel", true},
    {PaletteAction::ToggleDiplomacyGraph, "Windows", "Diplomacy Graph window", "Show/hide the Diplomacy Graph.", "Ctrl+0",
     "diplomacy", true},

    {PaletteAction::ToggleFleetManager, "Windows", "Fleet Manager", "Global fleet list + quick mission tools.", "Ctrl+Shift+F",
     "fleet", true},
    {PaletteAction::ToggleRegions, "Windows", "Regions (Sectors Overview)", "Sectors/regions overview and management.", "Ctrl+Shift+R",
     "regions sectors", true},
    {PaletteAction::ToggleAdvisor, "Windows", "Advisor (Issues)", "Issues list and recommended quick fixes.", "Ctrl+Shift+A",
     "advisor issues", true},

    // Automation (grouped separately from generic tools)
    {PaletteAction::ToggleColonyProfiles, "Automation", "Colony Profiles", "Automation presets for colony behavior.", "Ctrl+Shift+B",
     "colony profiles", true},
    {PaletteAction::ToggleShipProfiles, "Automation", "Ship Profiles", "Automation presets for ship behavior.", "Ctrl+Shift+M",
     "ship profiles", true},
    {PaletteAction::ToggleAutomationCenter, "Automation", "Automation Center", "Bulk ship automation flags + triage.", nullptr,
     "automation center", true},
    {PaletteAction::ToggleShipyardTargets, "Automation", "Shipyard Targets", "Design targets and shipyard production intents.", "Ctrl+Shift+Y",
     "shipyard targets", true},
    {PaletteAction::ToggleSurveyNetwork, "Automation", "Survey Network", "Jump point survey planning and progress.", "Ctrl+Shift+J",
     "survey network", true},

    // Tools
    {PaletteAction::ToggleSettings, "Tools", "Settings", "Open the Settings window (theme, layout, UI options).", "Ctrl+,",
     "settings", true},
    {PaletteAction::ToggleSaveTools, "Tools", "Save Tools", "Save inspection/export helpers.", nullptr, "save tools", true},
    {PaletteAction::ToggleTimeMachine, "Tools", "Time Machine", "State history + diffs (debug / analysis).", "Ctrl+Shift+D",
     "time machine", true},
    {PaletteAction::ToggleCompare, "Tools", "Compare / Diff", 
     "Compare two entities (or snapshots) with a flattened diff + merge patch export.", "Ctrl+Shift+X",
     "compare diff merge patch", true},
    {PaletteAction::ToggleOmniSearch, "Tools", "OmniSearch", "Search the live game JSON and run commands.", "Ctrl+F",
     "omnisearch", true},
    {PaletteAction::ToggleJsonExplorer, "Tools", "JSON Explorer", "Browse the live game JSON tree.", nullptr,
     "json explorer", true},
    {PaletteAction::ToggleContentValidation, "Tools", "Content Validation", "Validate content bundle errors/warnings.", "Ctrl+Shift+V",
     "content validation", true},
    {PaletteAction::ToggleStateDoctor, "Tools", "State Doctor", "Validate/fix save integrity; preview merge patch.", "Ctrl+Shift+K",
     "state doctor", true},
    {PaletteAction::ToggleEntityInspector, "Tools", "Entity Inspector", "Resolve an entity id and inspect inbound refs.", "Ctrl+G",
     "entity inspector", true},
    {PaletteAction::ToggleReferenceGraph, "Tools", "Reference Graph", "Visualize entity id relationships.", "Ctrl+Shift+G",
     "reference graph", true},

    {PaletteAction::ToggleWatchboard, "Tools", "Watchboard", "Pin JSON pointers/queries with history + alerts.", nullptr,
     "watchboard", true},
    {PaletteAction::ToggleDataLenses, "Tools", "Data Lenses", "Build tables over JSON arrays (inspect/sort/filter).", nullptr,
     "data lenses", true},
    {PaletteAction::ToggleDashboards, "Tools", "Dashboards", "Procedural KPI cards over JSON arrays.", nullptr,
     "dashboards", true},
    {PaletteAction::TogglePivotTables, "Tools", "Pivot Tables", "Group/summarize JSON arrays into pivots.", nullptr,
     "pivot tables", true},
    {PaletteAction::ToggleUIForge, "Tools", "UI Forge", "Build custom panels from JSON pointers/queries.", "Ctrl+Shift+U",
     "ui forge", true},
    {PaletteAction::ToggleLayoutProfiles, "Tools", "Layout Profiles", "Save/load dock layouts (including procedural layouts).", "Ctrl+Shift+L",
     "layout profiles", true},

    {PaletteAction::ToggleWindowManager, "Tools", "Window Manager",
     "Open the Window Manager (visibility, pop-outs, and per-window launch modes).", "Ctrl+Shift+W",
     "window manager popout popup", true},

    // UI
    {PaletteAction::ToggleStatusBar, "UI", "Status Bar", "Show/hide the bottom status bar.", nullptr, "status bar", true},
    {PaletteAction::ToggleFogOfWar, "UI", "Fog of War", "Toggle fog-of-war rendering on maps.", nullptr, "fog of war", true},
    {PaletteAction::ToggleToasts, "UI", "Event Toasts", "Show/hide HUD toast notifications.", nullptr, "toasts", true},

    // Workspace
    {PaletteAction::ToggleFocusMode, "Workspace", "Focus Mode (Map only)",
     "Toggle a decluttered view by hiding all windows except the Map (toggles back restores your previous set).", "F10",
     "focus zen unclutter", true},
    {PaletteAction::WorkspaceDefault, "Workspace", "Workspace: Default", "Apply the default workspace window preset.", nullptr,
     "workspace default", false},
    {PaletteAction::WorkspaceMinimal, "Workspace", "Workspace: Minimal", "Apply a minimal workspace window preset.", nullptr,
     "workspace minimal", false},
    {PaletteAction::WorkspaceEconomy, "Workspace", "Workspace: Economy", "Apply an economy-focused workspace window preset.", nullptr,
     "workspace economy", false},
    {PaletteAction::WorkspaceDesign, "Workspace", "Workspace: Design", "Apply a design-focused workspace window preset.", nullptr,
     "workspace design", false},
    {PaletteAction::WorkspaceIntel, "Workspace", "Workspace: Intel", "Apply an intel-focused workspace window preset.", nullptr,
     "workspace intel", false},

    // Game
    {PaletteAction::NewGameDialog, "Game", "New Game...", "Open the new-game dialog.", nullptr, "new game", false},
    {PaletteAction::NewGameSol, "Game", "New Game (Sol)", "Start a new game using the Sol preset scenario.", nullptr,
     "new game sol", false},
    {PaletteAction::NewGameRandom, "Game", "New Game (Random)", "Start a new game using procedural/random parameters.", nullptr,
     "new game random", false},
    {PaletteAction::ReloadContent, "Game", "Reload Content Bundle", "Hot-reload content/tech JSON from disk.", nullptr,
     "reload content", false},
    {PaletteAction::Save, "Game", "Save game", "Save to the current save path.", "Ctrl+S", "save", false},
    {PaletteAction::Load, "Game", "Load game", "Load from the current load path.", "Ctrl+O", "load", false},
};

const ActionMeta* find_action_meta(PaletteAction a) {
  for (const auto& m : kActionMetas) {
    if (m.action == a) return &m;
  }
  return nullptr;
}

// --- Command Console persistence helpers (favorites + recent) ---

std::string slugify(std::string_view s) {
  std::string out;
  out.reserve(s.size());

  bool prev_underscore = false;
  for (char c : s) {
    unsigned char uc = static_cast<unsigned char>(c);
    const bool is_alnum = (uc >= '0' && uc <= '9') || (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z');
    if (is_alnum) {
      out.push_back(static_cast<char>(std::tolower(uc)));
      prev_underscore = false;
    } else if (c == ' ' || c == '-' || c == '.' || c == '/' || c == '\\') {
      if (!prev_underscore && !out.empty()) {
        out.push_back('_');
        prev_underscore = true;
      }
    }
  }

  // Trim trailing underscores.
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

std::string make_action_persistent_id(const ActionMeta& m) {
  // Keep IDs readable and stable across launches.
  // NOTE: if the user-facing label changes, the ID will change; this is acceptable
  // for now (beta UI), and we garbage-collect unknown IDs automatically.
  std::string id;
  const std::string cat = slugify(m.category);
  const std::string name = slugify(m.label);
  id.reserve(cat.size() + name.size() + 1);
  id += cat;
  id += ':';
  id += name;
  return id;
}

const std::vector<std::pair<PaletteAction, std::string>>& action_id_table() {
  static const std::vector<std::pair<PaletteAction, std::string>> kTable = [] {
    std::vector<std::pair<PaletteAction, std::string>> tbl;
    tbl.reserve(std::size(kActionMetas));
    for (const auto& m : kActionMetas) {
      tbl.emplace_back(m.action, make_action_persistent_id(m));
    }
    return tbl;
  }();
  return kTable;
}

const std::string& action_persistent_id(PaletteAction a) {
  for (const auto& [act, id] : action_id_table()) {
    if (act == a) return id;
  }
  static const std::string kEmpty;
  return kEmpty;
}

PaletteAction action_from_persistent_id(std::string_view id) {
  for (const auto& [act, known] : action_id_table()) {
    if (known == id) return act;
  }
  return PaletteAction::Count;
}

bool action_is_favorited(const UIState& ui, PaletteAction a) {
  const std::string& id = action_persistent_id(a);
  if (id.empty()) return false;
  return std::find(ui.command_favorites.begin(), ui.command_favorites.end(), id) != ui.command_favorites.end();
}

void toggle_action_favorite(UIState& ui, PaletteAction a) {
  const std::string& id = action_persistent_id(a);
  if (id.empty()) return;

  auto it = std::find(ui.command_favorites.begin(), ui.command_favorites.end(), id);
  if (it != ui.command_favorites.end()) {
    ui.command_favorites.erase(it);
  } else {
    ui.command_favorites.push_back(id);
  }
}

void remember_action_recent(UIState& ui, PaletteAction a) {
  const std::string& id = action_persistent_id(a);
  if (id.empty()) return;

  // De-dup.
  ui.command_recent.erase(std::remove(ui.command_recent.begin(), ui.command_recent.end(), id), ui.command_recent.end());

  // Most-recent first.
  ui.command_recent.insert(ui.command_recent.begin(), id);

  // Enforce cap.
  const int limit = std::clamp(ui.command_recent_limit, 0, 200);
  ui.command_recent_limit = limit;
  if (limit == 0) {
    ui.command_recent.clear();
  } else if (static_cast<int>(ui.command_recent.size()) > limit) {
    ui.command_recent.resize(static_cast<std::size_t>(limit));
  }
}

void gc_unknown_persistent_actions(UIState& ui) {
  auto is_known = [](const std::string& id) {
    return action_from_persistent_id(id) != PaletteAction::Count;
  };
  ui.command_favorites.erase(std::remove_if(ui.command_favorites.begin(), ui.command_favorites.end(),
                                            [&](const std::string& id) { return !is_known(id); }),
                             ui.command_favorites.end());
  ui.command_recent.erase(std::remove_if(ui.command_recent.begin(), ui.command_recent.end(),
                                         [&](const std::string& id) { return !is_known(id); }),
                          ui.command_recent.end());
}

bool action_toggle_state(const UIState& ui, PaletteAction a) {
  switch (a) {
    case PaletteAction::ToggleControls: return ui.show_controls_window;
    case PaletteAction::ToggleMap: return ui.show_map_window;
    case PaletteAction::ToggleDetails: return ui.show_details_window;
    case PaletteAction::ToggleDirectory: return ui.show_directory_window;
    case PaletteAction::ToggleProduction: return ui.show_production_window;
    case PaletteAction::ToggleEconomy: return ui.show_economy_window;
    case PaletteAction::ToggleFleetManager: return ui.show_fleet_manager_window;
    case PaletteAction::ToggleRegions: return ui.show_regions_window;
    case PaletteAction::ToggleAdvisor: return ui.show_advisor_window;
    case PaletteAction::ToggleColonyProfiles: return ui.show_colony_profiles_window;
    case PaletteAction::ToggleShipProfiles: return ui.show_ship_profiles_window;
    case PaletteAction::ToggleAutomationCenter: return ui.show_automation_center_window;
    case PaletteAction::ToggleShipyardTargets: return ui.show_shipyard_targets_window;
    case PaletteAction::ToggleSurveyNetwork: return ui.show_survey_network_window;
    case PaletteAction::ToggleTimeline: return ui.show_timeline_window;
    case PaletteAction::ToggleNotifications: return ui.show_notifications_window;
    case PaletteAction::ToggleDesignStudio: return ui.show_design_studio_window;
    case PaletteAction::ToggleBalanceLab: return ui.show_balance_lab_window;
    case PaletteAction::ToggleIntel: return ui.show_intel_window;
    case PaletteAction::ToggleIntelNotebook: return ui.show_intel_notebook_window;
    case PaletteAction::ToggleDiplomacyGraph: return ui.show_diplomacy_window;
    case PaletteAction::ToggleSettings: return ui.show_settings_window;
    case PaletteAction::ToggleSaveTools: return ui.show_save_tools_window;
    case PaletteAction::ToggleOmniSearch: return ui.show_omni_search_window;
    case PaletteAction::ToggleJsonExplorer: return ui.show_json_explorer_window;
    case PaletteAction::ToggleContentValidation: return ui.show_content_validation_window;
    case PaletteAction::ToggleStateDoctor: return ui.show_state_doctor_window;
    case PaletteAction::ToggleEntityInspector: return ui.show_entity_inspector_window;
    case PaletteAction::ToggleReferenceGraph: return ui.show_reference_graph_window;
    case PaletteAction::ToggleTimeMachine: return ui.show_time_machine_window;
    case PaletteAction::ToggleCompare: return ui.show_compare_window;
    case PaletteAction::ToggleNavigator: return ui.show_navigator_window;
    case PaletteAction::ToggleWatchboard: return ui.show_watchboard_window;
    case PaletteAction::ToggleDataLenses: return ui.show_data_lenses_window;
    case PaletteAction::ToggleDashboards: return ui.show_dashboards_window;
    case PaletteAction::TogglePivotTables: return ui.show_pivot_tables_window;
    case PaletteAction::ToggleUIForge: return ui.show_ui_forge_window;
    case PaletteAction::ToggleLayoutProfiles: return ui.show_layout_profiles_window;
    case PaletteAction::ToggleWindowManager: return ui.show_window_manager_window;
    case PaletteAction::ToggleStatusBar: return ui.show_status_bar;
    case PaletteAction::ToggleFogOfWar: return ui.fog_of_war;
    case PaletteAction::ToggleToasts: return ui.show_event_toasts;
    case PaletteAction::ToggleFocusMode: return ui.window_focus_mode;
    default: return false;
  }
}

const char* hotkey_id_for_action(PaletteAction a) {
  // Map palette actions to configurable Hotkeys ids.
  // Only actions that are processed by the App-level global hotkey dispatcher
  // are mapped here.
  switch (a) {
    case PaletteAction::OpenHelp: return "ui.toggle.help";
    case PaletteAction::ToggleOmniSearch: return "ui.toggle.omnisearch";
    case PaletteAction::ToggleEntityInspector: return "ui.toggle.entity_inspector";
    case PaletteAction::ToggleReferenceGraph: return "ui.toggle.reference_graph";
    case PaletteAction::ToggleTimeMachine: return "ui.toggle.time_machine";
    case PaletteAction::ToggleCompare: return "ui.toggle.compare";
    case PaletteAction::ToggleNavigator: return "ui.toggle.navigator";
    case PaletteAction::ToggleAdvisor: return "ui.toggle.advisor";
    case PaletteAction::ToggleColonyProfiles: return "ui.toggle.colony_profiles";
    case PaletteAction::ToggleShipProfiles: return "ui.toggle.ship_profiles";
    case PaletteAction::ToggleShipyardTargets: return "ui.toggle.shipyard_targets";
    case PaletteAction::ToggleSurveyNetwork: return "ui.toggle.survey_network";
    case PaletteAction::ToggleRegions: return "ui.toggle.regions";
    case PaletteAction::ToggleFleetManager: return "ui.toggle.fleet_manager";
    case PaletteAction::ToggleContentValidation: return "ui.toggle.content_validation";
    case PaletteAction::ToggleStateDoctor: return "ui.toggle.state_doctor";
    case PaletteAction::ToggleControls: return "ui.toggle.controls";
    case PaletteAction::ToggleMap: return "ui.toggle.map";
    case PaletteAction::ToggleDetails: return "ui.toggle.details";
    case PaletteAction::ToggleDirectory: return "ui.toggle.directory";
    case PaletteAction::ToggleEconomy: return "ui.toggle.economy";
    case PaletteAction::ToggleProduction: return "ui.toggle.production";
    case PaletteAction::ToggleTimeline: return "ui.toggle.timeline";
    case PaletteAction::ToggleNotifications: return "ui.toggle.notifications";
    case PaletteAction::ToggleDesignStudio: return "ui.toggle.design_studio";
    case PaletteAction::ToggleIntel: return "ui.toggle.intel";
    case PaletteAction::ToggleIntelNotebook: return "ui.toggle.intel_notebook";
    case PaletteAction::ToggleDiplomacyGraph: return "ui.toggle.diplomacy";
    case PaletteAction::ToggleSettings: return "ui.toggle.settings";
    case PaletteAction::ToggleLayoutProfiles: return "ui.toggle.layout_profiles";
    case PaletteAction::ToggleWindowManager: return "ui.toggle.window_manager";
    case PaletteAction::ToggleUIForge: return "ui.toggle.ui_forge";
    case PaletteAction::ToggleFocusMode: return "ui.toggle.focus_mode";
    case PaletteAction::NavBack: return "nav.back";
    case PaletteAction::NavForward: return "nav.forward";
    case PaletteAction::Save: return "game.save";
    case PaletteAction::Load: return "game.load";
    default: return nullptr;
  }
}

std::string effective_shortcut(const ActionMeta& m, const UIState& ui) {
  const char* hotkey_id = hotkey_id_for_action(m.action);
  if (hotkey_id) {
    return hotkey_to_string(hotkey_get(ui, hotkey_id));
  }
  return (m.shortcut && m.shortcut[0] != '\0') ? std::string(m.shortcut) : std::string();
}

void draw_action_tooltip(const ActionMeta& m, const UIState& ui) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) return;

  ImGui::BeginTooltip();
  ImGui::TextUnformatted(m.label);
  ImGui::Separator();

  const char* tip = (m.tooltip && m.tooltip[0] != '\0') ? m.tooltip : m.label;
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
  ImGui::TextWrapped("%s", tip);
  ImGui::PopTextWrapPos();

  const std::string shortcut = effective_shortcut(m, ui);
  if (!shortcut.empty()) {
    ImGui::Spacing();
    ImGui::TextDisabled("Shortcut: %s", shortcut.c_str());
  }

  if (m.keywords && m.keywords[0] != '\0') {
    ImGui::Spacing();
    ImGui::TextDisabled("Keywords: %s", m.keywords);
  }

  ImGui::EndTooltip();
}

int action_match_score(const ActionMeta& m, std::string_view query) {
  if (query.empty()) return 0;
  int sc = fuzzy_score(m.label, query);
  if (m.keywords) sc = std::max(sc, fuzzy_score(m.keywords, query) - 10);
  return sc;
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
  if (ImGui::SmallButton("Mine")) ui.show_mine_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open the Mine Planner (auto-mine preview)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Fuel")) ui.show_fuel_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open the Fuel Planner (auto-tanker preview)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Salvage")) ui.show_salvage_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open the Salvage Planner (wreck salvage + delivery preview)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Sustain")) ui.show_sustainment_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open the Sustainment Planner (fleet base stockpile targets)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Troops")) ui.show_troop_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open Troop Logistics (auto-troop preview + apply plan)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Pop")) ui.show_colonist_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open Population Logistics (auto-colonist preview + apply plan)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Terra")) ui.show_terraforming_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open Terraforming Planner (empire-wide overview + ETA)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Advisor")) ui.show_advisor_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open the Advisor (issues + quick fixes)");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Profiles")) ImGui::OpenPopup("profiles_popup");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open automation preset windows");
  }
  if (ImGui::BeginPopup("profiles_popup")) {
    if (ImGui::MenuItem("Colony Profiles", "Ctrl+Shift+B", ui.show_colony_profiles_window)) {
      ui.show_colony_profiles_window = !ui.show_colony_profiles_window;
    }
    if (ImGui::MenuItem("Ship Profiles", "Ctrl+Shift+M", ui.show_ship_profiles_window)) {
      ui.show_ship_profiles_window = !ui.show_ship_profiles_window;
    }
    ImGui::EndPopup();
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

  if (ImGui::SmallButton("Console")) ui.show_command_palette = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Command Console (Ctrl+P)");
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

  ImGui::SameLine();
  if (ImGui::SmallButton("Nav")) ui.show_navigator_window = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Navigator (Ctrl+Shift+N)\nSelection history + bookmarks (Alt+Left/Alt+Right).");
  }

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

  // Notifications inbox indicator.
  const int inbox_unread = notifications_unread_count(ui);
  ImGui::SameLine();
  VerticalSeparator();
  ImGui::SameLine();
  if (inbox_unread > 0) {
    std::string b = "Inbox (" + std::to_string(inbox_unread) + ")";
    if (ImGui::SmallButton(b.c_str())) ui.show_notifications_window = true;
  } else {
    if (ImGui::SmallButton("Inbox")) ui.show_notifications_window = true;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open Notification Center\nShortcut: F3");
  }

  // Intel Notebook quick access (uses pinned system notes as a lightweight indicator).
  {
    Id vf = ui.viewer_faction_id;
    if (selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(sim.state().ships, selected_ship)) vf = sh->faction_id;
    }
    const Faction* fac = (vf != kInvalidId) ? find_ptr(sim.state().factions, vf) : nullptr;
    if (fac) {
      int pinned = 0;
      for (const auto& kv : fac->system_notes) {
        if (kv.second.pinned) ++pinned;
      }
      ImGui::SameLine();
      VerticalSeparator();
      ImGui::SameLine();
      std::string b = (pinned > 0) ? ("Notebook (★" + std::to_string(pinned) + ")") : "Notebook";
      if (ImGui::SmallButton(b.c_str())) ui.show_intel_notebook_window = true;
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open Intel Notebook (system notes + journal)\nShortcut: Ctrl+Shift+I");
      }
    }
  }

  ImGui::End();
  ImGui::PopStyleVar(3);
}

void draw_help_window(UIState& ui) {
  if (!ui.show_help_window) return;

  ImGui::SetNextWindowSize(ImVec2(980, 720), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Help / Codex", &ui.show_help_window)) {
    ImGui::End();
    return;
  }

  // Helper: actions that are safe to trigger without touching the simulation.
  auto can_apply_ui_only_action = [&](PaletteAction a) -> bool {
    switch (a) {
      case PaletteAction::ToggleControls:
      case PaletteAction::ToggleMap:
      case PaletteAction::ToggleDetails:
      case PaletteAction::ToggleDirectory:
      case PaletteAction::ToggleProduction:
      case PaletteAction::ToggleEconomy:
      case PaletteAction::ToggleFleetManager:
      case PaletteAction::ToggleRegions:
      case PaletteAction::ToggleAdvisor:
      case PaletteAction::ToggleColonyProfiles:
      case PaletteAction::ToggleShipProfiles:
      case PaletteAction::ToggleAutomationCenter:
      case PaletteAction::ToggleShipyardTargets:
      case PaletteAction::ToggleSurveyNetwork:
      case PaletteAction::ToggleTimeline:
      case PaletteAction::ToggleNotifications:
      case PaletteAction::ToggleDesignStudio:
      case PaletteAction::ToggleBalanceLab:
      case PaletteAction::ToggleIntel:
      case PaletteAction::ToggleIntelNotebook:
      case PaletteAction::ToggleDiplomacyGraph:

      case PaletteAction::ToggleSettings:
      case PaletteAction::ToggleSaveTools:
      case PaletteAction::ToggleOmniSearch:
      case PaletteAction::ToggleJsonExplorer:
      case PaletteAction::ToggleContentValidation:
      case PaletteAction::ToggleStateDoctor:
      case PaletteAction::ToggleEntityInspector:
      case PaletteAction::ToggleReferenceGraph:
      case PaletteAction::ToggleTimeMachine:
      case PaletteAction::ToggleNavigator:
      case PaletteAction::ToggleWatchboard:
      case PaletteAction::ToggleDataLenses:
      case PaletteAction::ToggleDashboards:
      case PaletteAction::TogglePivotTables:
      case PaletteAction::ToggleUIForge:
      case PaletteAction::ToggleLayoutProfiles:

      case PaletteAction::ToggleStatusBar:
      case PaletteAction::ToggleFogOfWar:
      case PaletteAction::ToggleToasts:

      case PaletteAction::OpenLogTab:
      case PaletteAction::OpenHelp:
      case PaletteAction::FocusSystemMap:
      case PaletteAction::FocusGalaxyMap:

      case PaletteAction::WorkspaceDefault:
      case PaletteAction::WorkspaceMinimal:
      case PaletteAction::WorkspaceEconomy:
      case PaletteAction::WorkspaceDesign:
      case PaletteAction::WorkspaceIntel:
        return true;

      default:
        return false;
    }
  };

  auto apply_ui_only_action = [&](PaletteAction a) -> bool {
    switch (a) {
      // Windows (toggles)
      case PaletteAction::ToggleControls: ui.show_controls_window = !ui.show_controls_window; return true;
      case PaletteAction::ToggleMap: ui.show_map_window = !ui.show_map_window; return true;
      case PaletteAction::ToggleDetails: ui.show_details_window = !ui.show_details_window; return true;
      case PaletteAction::ToggleDirectory: ui.show_directory_window = !ui.show_directory_window; return true;
      case PaletteAction::ToggleProduction: ui.show_production_window = !ui.show_production_window; return true;
      case PaletteAction::ToggleEconomy: ui.show_economy_window = !ui.show_economy_window; return true;
      case PaletteAction::ToggleFleetManager: ui.show_fleet_manager_window = !ui.show_fleet_manager_window; return true;
      case PaletteAction::ToggleRegions: ui.show_regions_window = !ui.show_regions_window; return true;
      case PaletteAction::ToggleAdvisor: ui.show_advisor_window = !ui.show_advisor_window; return true;
      case PaletteAction::ToggleColonyProfiles: ui.show_colony_profiles_window = !ui.show_colony_profiles_window; return true;
      case PaletteAction::ToggleShipProfiles: ui.show_ship_profiles_window = !ui.show_ship_profiles_window; return true;
      case PaletteAction::ToggleAutomationCenter: ui.show_automation_center_window = !ui.show_automation_center_window; return true;
      case PaletteAction::ToggleShipyardTargets: ui.show_shipyard_targets_window = !ui.show_shipyard_targets_window; return true;
      case PaletteAction::ToggleSurveyNetwork: ui.show_survey_network_window = !ui.show_survey_network_window; return true;
      case PaletteAction::ToggleTimeline: ui.show_timeline_window = !ui.show_timeline_window; return true;
      case PaletteAction::ToggleNotifications: ui.show_notifications_window = !ui.show_notifications_window; return true;
      case PaletteAction::ToggleDesignStudio: ui.show_design_studio_window = !ui.show_design_studio_window; return true;
      case PaletteAction::ToggleBalanceLab: ui.show_balance_lab_window = !ui.show_balance_lab_window; return true;
      case PaletteAction::ToggleIntel: ui.show_intel_window = !ui.show_intel_window; return true;
      case PaletteAction::ToggleIntelNotebook: ui.show_intel_notebook_window = !ui.show_intel_notebook_window; return true;
      case PaletteAction::ToggleDiplomacyGraph: ui.show_diplomacy_window = !ui.show_diplomacy_window; return true;

      case PaletteAction::ToggleSettings: ui.show_settings_window = !ui.show_settings_window; return true;
      case PaletteAction::ToggleSaveTools: ui.show_save_tools_window = !ui.show_save_tools_window; return true;
      case PaletteAction::ToggleOmniSearch: ui.show_omni_search_window = !ui.show_omni_search_window; return true;
      case PaletteAction::ToggleJsonExplorer: ui.show_json_explorer_window = !ui.show_json_explorer_window; return true;
      case PaletteAction::ToggleContentValidation: ui.show_content_validation_window = !ui.show_content_validation_window; return true;
      case PaletteAction::ToggleStateDoctor: ui.show_state_doctor_window = !ui.show_state_doctor_window; return true;
      case PaletteAction::ToggleEntityInspector: ui.show_entity_inspector_window = !ui.show_entity_inspector_window; return true;
      case PaletteAction::ToggleReferenceGraph: ui.show_reference_graph_window = !ui.show_reference_graph_window; return true;
      case PaletteAction::ToggleTimeMachine: ui.show_time_machine_window = !ui.show_time_machine_window; return true;
      case PaletteAction::ToggleCompare: ui.show_compare_window = !ui.show_compare_window; return true;
      case PaletteAction::ToggleNavigator: ui.show_navigator_window = !ui.show_navigator_window; return true;
      case PaletteAction::ToggleWatchboard: ui.show_watchboard_window = !ui.show_watchboard_window; return true;
      case PaletteAction::ToggleDataLenses: ui.show_data_lenses_window = !ui.show_data_lenses_window; return true;
      case PaletteAction::ToggleDashboards: ui.show_dashboards_window = !ui.show_dashboards_window; return true;
      case PaletteAction::TogglePivotTables: ui.show_pivot_tables_window = !ui.show_pivot_tables_window; return true;
      case PaletteAction::ToggleUIForge: ui.show_ui_forge_window = !ui.show_ui_forge_window; return true;
      case PaletteAction::ToggleLayoutProfiles: ui.show_layout_profiles_window = !ui.show_layout_profiles_window; return true;

      // UI chrome
      case PaletteAction::ToggleStatusBar: ui.show_status_bar = !ui.show_status_bar; return true;
      case PaletteAction::ToggleFogOfWar: ui.fog_of_war = !ui.fog_of_war; return true;
      case PaletteAction::ToggleToasts: ui.show_event_toasts = !ui.show_event_toasts; return true;

      // Focus helpers (UI only)
      case PaletteAction::OpenLogTab:
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Log;
        return true;
      case PaletteAction::OpenHelp:
        ui.show_help_window = true;
        return true;
      case PaletteAction::FocusSystemMap:
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::System;
        return true;
      case PaletteAction::FocusGalaxyMap:
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::Galaxy;
        return true;

      // Workspace presets (UI only)
      case PaletteAction::WorkspaceDefault:
        ui.show_controls_window = true;
        ui.show_map_window = true;
        ui.show_details_window = true;
        ui.show_directory_window = true;
        ui.show_production_window = false;
        ui.show_economy_window = false;
        ui.show_planner_window = false;
        ui.show_freight_window = false;
        ui.show_mine_window = false;
        ui.show_fuel_window = false;
        ui.show_time_warp_window = false;
        ui.show_timeline_window = false;
        ui.show_design_studio_window = false;
        ui.show_balance_lab_window = false;
        ui.show_intel_window = false;
        ui.show_diplomacy_window = false;
        ui.show_status_bar = true;
        return true;
      case PaletteAction::WorkspaceMinimal:
        ui.show_controls_window = false;
        ui.show_map_window = true;
        ui.show_details_window = true;
        ui.show_directory_window = false;
        ui.show_production_window = false;
        ui.show_economy_window = false;
        ui.show_planner_window = false;
        ui.show_freight_window = false;
        ui.show_mine_window = false;
        ui.show_fuel_window = false;
        ui.show_time_warp_window = false;
        ui.show_timeline_window = false;
        ui.show_design_studio_window = false;
        ui.show_balance_lab_window = false;
        ui.show_intel_window = false;
        ui.show_diplomacy_window = false;
        ui.show_status_bar = true;
        return true;
      case PaletteAction::WorkspaceEconomy:
        ui.show_controls_window = false;
        ui.show_map_window = true;
        ui.show_details_window = true;
        ui.show_directory_window = true;
        ui.show_production_window = true;
        ui.show_economy_window = true;
        ui.show_planner_window = true;
        ui.show_freight_window = false;
        ui.show_mine_window = false;
        ui.show_fuel_window = false;
        ui.show_time_warp_window = false;
        ui.show_timeline_window = true;
        ui.show_design_studio_window = false;
        ui.show_balance_lab_window = false;
        ui.show_intel_window = false;
        ui.show_diplomacy_window = false;
        ui.show_status_bar = true;
        return true;
      case PaletteAction::WorkspaceDesign:
        ui.show_controls_window = false;
        ui.show_map_window = true;
        ui.show_details_window = true;
        ui.show_directory_window = false;
        ui.show_production_window = false;
        ui.show_economy_window = false;
        ui.show_planner_window = false;
        ui.show_freight_window = false;
        ui.show_mine_window = false;
        ui.show_fuel_window = false;
        ui.show_time_warp_window = false;
        ui.show_timeline_window = false;
        ui.show_design_studio_window = true;
        ui.show_balance_lab_window = true;
        ui.show_intel_window = false;
        ui.show_diplomacy_window = false;
        ui.show_status_bar = true;
        return true;
      case PaletteAction::WorkspaceIntel:
        ui.show_controls_window = false;
        ui.show_map_window = true;
        ui.show_details_window = true;
        ui.show_directory_window = false;
        ui.show_production_window = false;
        ui.show_economy_window = false;
        ui.show_planner_window = false;
        ui.show_freight_window = false;
        ui.show_mine_window = false;
        ui.show_fuel_window = false;
        ui.show_time_warp_window = false;
        ui.show_timeline_window = true;
        ui.show_design_studio_window = false;
        ui.show_balance_lab_window = false;
        ui.show_intel_window = true;
        ui.show_diplomacy_window = true;
        ui.show_status_bar = true;
        return true;

      default:
        return false;
    }
  };

  if (ImGui::BeginTabBar("help_tabs")) {
    auto flags_for_help = [&](HelpTab t) {
      return (ui.request_help_tab == t) ? ImGuiTabItemFlags_SetSelected : 0;
    };

    if (ImGui::BeginTabItem("Quick Start", nullptr, flags_for_help(HelpTab::QuickStart))) {
      ImGui::SeparatorText("Fast navigation");
      ImGui::TextWrapped(
          "Nebula4X is a UI-heavy sandbox: use the Command Console (Ctrl+P) to jump to tools and windows, then use the "
          "Map/Details panels to issue and review orders.");

      if (ImGui::Button("Open Command Console (Ctrl+P)")) ui.show_command_palette = true;
      ImGui::SameLine();
      if (ImGui::Button("Open OmniSearch (Ctrl+F)")) ui.show_omni_search_window = true;
      ImGui::SameLine();
      if (ImGui::Button("Open Settings (Ctrl+,)")) ui.show_settings_window = true;

      ImGui::SameLine();
      if (ImGui::Button("Start Guided Tour (F2)")) {
        ui.tour_active = true;
        ui.tour_active_index = 0;
        ui.tour_step_index = 0;
        // Hide the help window so the spotlight is not obscured.
        ui.show_help_window = false;
      }

      ImGui::Spacing();
      ImGui::SeparatorText("Map basics");
      ImGui::BulletText("Mouse wheel: zoom");
      ImGui::BulletText("Middle mouse drag: pan");
      ImGui::BulletText("System map: Left click = issue order, Right click = select");
      ImGui::BulletText("Galaxy map: Left click = select system, Right click = route ship (Shift queues)");

      ImGui::Spacing();
      ImGui::SeparatorText("Workspace tips");
      ImGui::BulletText("Drag window tabs to dock/undock and rearrange the workspace");
      ImGui::BulletText("Use Layout Profiles (Ctrl+Shift+L) to save/load dock layouts");

      ImGui::Spacing();
      ImGui::SeparatorText("UI-only helpers");
      ImGui::TextWrapped(
          "The Command Console, the event toasts, and most procedural inspector tools are UI-only helpers. They do not "
          "change the simulation by themselves; they help you navigate and respond to what is happening.");

      ImGui::EndTabItem();
    }

    
    if (ImGui::BeginTabItem("Tours", nullptr, flags_for_help(HelpTab::Tours))) {
      draw_help_tours_tab(ui);
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Shortcuts", nullptr, flags_for_help(HelpTab::Shortcuts))) {
      static char filter[128] = {0};

      ImGui::TextWrapped(
          "This list is generated from the Command Console action registry. Use it as a searchable cheat-sheet (and for "
          "UI-only actions, you can trigger them directly from here). For everything else, open Ctrl+P.");

      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputText("Filter (label/category/shortcut)", filter, sizeof(filter));
      const std::string q = trim_copy(filter);

      const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                 ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
      if (ImGui::BeginTable("##help_actions", 5, tf, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Run", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        for (const auto& m : kActionMetas) {
          const std::string shortcut = effective_shortcut(m, ui);
          const std::string keywords = (m.keywords && m.keywords[0] != '\0') ? std::string(m.keywords) : std::string();
          const std::string hay = std::string(m.category) + " " + m.label + " " + shortcut + " " + keywords;
          if (!q.empty() && !contains_ci(hay, q)) continue;

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(m.category);

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(m.label);
          draw_action_tooltip(m, ui);

          ImGui::TableSetColumnIndex(2);
          if (!shortcut.empty()) ImGui::TextDisabled("%s", shortcut.c_str());
          else ImGui::TextDisabled("-");

          ImGui::TableSetColumnIndex(3);
          if (m.toggles) {
            const bool on = action_toggle_state(ui, m.action);
            ImGui::TextDisabled("%s", on ? "On" : "Off");
          } else {
            ImGui::TextDisabled("-");
          }

          ImGui::TableSetColumnIndex(4);
          ImGui::PushID(static_cast<int>(m.action));
          const bool can_run = can_apply_ui_only_action(m.action);
          if (!can_run) ImGui::BeginDisabled();
          if (ImGui::SmallButton("Do") && can_run) {
            (void)apply_ui_only_action(m.action);
          }
          if (!can_run) ImGui::EndDisabled();
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Trigger UI-only actions from here.\nFor simulation actions, use Ctrl+P.");
          }
          ImGui::PopID();
        }

        ImGui::EndTable();
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Docs", nullptr, flags_for_help(HelpTab::Docs))) {
      draw_docs_browser_panel(ui);
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Accessibility", nullptr, flags_for_help(HelpTab::Accessibility))) {
      ImGui::SeparatorText("Screen reader / narration");
      ImGui::Checkbox("Enable narration", &ui.screen_reader_enabled);
      ImGui::Checkbox("Speak focused control", &ui.screen_reader_speak_focus);
      ImGui::Checkbox("Speak hovered control", &ui.screen_reader_speak_hover);
      ImGui::Checkbox("Speak window changes", &ui.screen_reader_speak_windows);
      ImGui::Checkbox("Speak toast notifications", &ui.screen_reader_speak_toasts);
      ImGui::Checkbox("Speak selection changes", &ui.screen_reader_speak_selection);
      ImGui::SliderFloat("Rate", &ui.screen_reader_rate, 0.50f, 2.0f, "%.2fx");
      ImGui::SliderFloat("Volume", &ui.screen_reader_volume, 0.0f, 1.0f, "%.2f");
      ImGui::SliderFloat("Hover delay (sec)", &ui.screen_reader_hover_delay_sec, 0.10f, 2.00f, "%.2f");
      ImGui::Spacing();
      ImGui::TextWrapped(
          "Narration is an in-game feedback layer (not a full OS accessibility tree). It can speak focus changes, toasts, "
          "and selection updates to reduce UI load.");
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("About", nullptr, flags_for_help(HelpTab::About))) {
#ifndef NEBULA4X_VERSION
#define NEBULA4X_VERSION "unknown"
#endif
      ImGui::SeparatorText("Build");
      ImGui::Text("Nebula4X v%s", NEBULA4X_VERSION);
      ImGui::Text("ImGui %s", ImGui::GetVersion());
      ImGui::Spacing();
      ImGui::SeparatorText("Documentation");
      ImGui::TextWrapped(
          "This Codex reads Markdown files from: data/docs/*.md (shipped with the build). When running from the repo, it "
          "also scans ./docs and top-level README/PATCH_NOTES.");
      ImGui::Spacing();
      ImGui::SeparatorText("Links");
      if (ImGui::SmallButton("Copy repo URL")) {
        ImGui::SetClipboardText("https://github.com/masterblaster1999/Nebula4X");
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(paste in a browser)");
      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    if (ui.request_help_tab != HelpTab::None) ui.request_help_tab = HelpTab::None;
  }

  ImGui::End();
}

void draw_command_palette(Simulation& sim, UIState& ui, HUDState& hud, Id& selected_ship, Id& selected_colony,
                          Id& selected_body, char* save_path, char* load_path) {
  if (!ui.show_command_palette) return;

  // Center-ish near the top, like common palettes.
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 pos(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.18f);

  ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_Appearing);

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

  if (!ImGui::Begin("Command Console", &ui.show_command_palette, flags)) {
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

  // Query row.
  bool enter_pressed = false;
  {
    ImGuiInputTextFlags tf = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
    if (just_opened) {
      ImGui::SetKeyboardFocusHere();
    }
    enter_pressed = ImGui::InputTextWithHint(
        "##palette_query",
        "Search actions + entities (Shift+Enter keeps the console open)",
        hud.palette_query,
        IM_ARRAYSIZE(hud.palette_query),
        tf);
  }

  const std::string query = trim_copy(hud.palette_query);

  static std::string last_query;
  if (query != last_query) {
    hud.palette_selected_idx = 0;
    last_query = query;
  }

  const auto& s = sim.state();

  // Search results (flat list) are only built when the user types a query.
  std::vector<PaletteItem> results;
  results.reserve(256);

  if (!query.empty()) {
    // Actions (metadata-driven).
    for (const auto& m : kActionMetas) {
      const int sc = action_match_score(m, query);
      if (sc < 0) continue;
      PaletteItem it;
      it.kind = PaletteKind::Action;
      it.action = m.action;
      it.label = std::string("[Action] ") + m.label;
      it.score = sc;
      results.push_back(std::move(it));
    }

    // Entities.
    for (const auto& [sid, sys] : s.systems) {
      std::string label = "[System] " + sys.name;
      const int sc = fuzzy_score(label, query);
      if (sc < 0) continue;
      PaletteItem it;
      it.kind = PaletteKind::System;
      it.id = sid;
      it.label = std::move(label);
      it.score = sc;
      results.push_back(std::move(it));
    }

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
      results.push_back(std::move(it));
    }

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
      results.push_back(std::move(it));
    }

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
      results.push_back(std::move(it));
    }

    // Sort by score (desc), then label.
    std::sort(results.begin(), results.end(), [](const PaletteItem& a, const PaletteItem& b) {
      if (a.score != b.score) return a.score > b.score;
      return a.label < b.label;
    });

    constexpr int kMaxItems = 120;
    if ((int)results.size() > kMaxItems) results.resize(kMaxItems);

    // Keyboard navigation only applies to the search results list.
    if (!results.empty()) {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        hud.palette_selected_idx = std::min(hud.palette_selected_idx + 1, (int)results.size() - 1);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        hud.palette_selected_idx = std::max(hud.palette_selected_idx - 1, 0);
      }
    } else {
      hud.palette_selected_idx = 0;
    }
  }

  // Shared helper: execute an action item through the existing activation switch.
  auto run_action = [&](PaletteAction a) {
    PaletteItem it;
    it.kind = PaletteKind::Action;
    it.action = a;
    activate_palette_item(it, sim, ui, selected_ship, selected_colony, selected_body, save_path, load_path);
  };

  auto maybe_close = [&]() {
    // Hold Shift while activating to keep the console open.
    if (!io.KeyShift) ui.show_command_palette = false;
  };

  const ActionMeta* hovered_action = nullptr;
  std::string hovered_entity_label;

  const ImGuiTableFlags table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
  if (ImGui::BeginTable("##cmd_console_table", 2, table_flags)) {
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch, 0.62f);
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.38f);
    ImGui::TableNextRow();

    // --- Left column ---
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("##cmd_console_left", ImVec2(0, 0), false);

    // Search results (query-driven).
    if (!query.empty()) {
      ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
      if (ImGui::CollapsingHeader("Search Results", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (results.empty()) {
          ImGui::TextDisabled("No matches.");
          ImGui::TextDisabled("Tip: try a substring (e.g. 'Sol', 'Survey', 'Colony').");
        } else {
          const float list_h = std::min(280.0f, ImGui::GetContentRegionAvail().y * 0.55f);
          ImGui::BeginChild("##cmd_console_results_list", ImVec2(0, list_h), true);

          int clicked_idx = -1;
          for (int i = 0; i < (int)results.size(); ++i) {
            const bool sel = (i == hud.palette_selected_idx);
            if (ImGui::Selectable(results[i].label.c_str(), sel)) {
              clicked_idx = i;
            }

            // Ensure the selected row stays visible when navigating.
            if (sel && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {
              ImGui::SetScrollHereY(0.5f);
            }

            // Hover details + tooltips.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
              hovered_entity_label = results[i].label;
              if (results[i].kind == PaletteKind::Action) {
                hovered_action = find_action_meta(results[i].action);
                if (hovered_action) draw_action_tooltip(*hovered_action, ui);
              } else {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(results[i].label.c_str());
                ImGui::EndTooltip();
              }
            }
          }

          ImGui::EndChild();

          ImGui::TextDisabled("Enter: apply   Esc: close   ↑/↓: navigate   Shift: keep open");

          const bool do_activate = enter_pressed || (clicked_idx >= 0);
          if (do_activate) {
            const int idx = (clicked_idx >= 0) ? clicked_idx
                                               : std::clamp(hud.palette_selected_idx, 0, (int)results.size() - 1);
            PaletteItem item = results[static_cast<std::size_t>(idx)];
            activate_palette_item(item, sim, ui, selected_ship, selected_colony, selected_body, save_path, load_path);
            maybe_close();

            // Keep the query for rapid repeated use, but reset selection.
            hud.palette_selected_idx = 0;
          }
        }
      }

      ImGui::Separator();
    } else {
      // No query: show persistence helpers above the contextual + browse panels.
      gc_unknown_persistent_actions(ui);

      if (!ui.command_favorites.empty()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
        if (ImGui::CollapsingHeader("Favorites", ImGuiTreeNodeFlags_DefaultOpen)) {
          for (const auto& id : ui.command_favorites) {
            const auto a = action_from_persistent_id(id);
            const auto* m = find_action_meta(a);
            if (!m) continue;

            ImGui::PushID(id.c_str());
            const std::string label = std::string("★ ") + m->label;
            if (ImGui::Selectable(label.c_str(), false)) {
              PaletteItem item;
              item.kind = PaletteKind::Action;
              item.action = a;
              activate_palette_item(item, sim, ui, selected_ship, selected_colony, selected_body, save_path, load_path);
              maybe_close();
              hud.palette_selected_idx = 0;
            }
            if (ImGui::IsItemHovered()) {
              hovered_action = m;
            }
            if (ImGui::BeginPopupContextItem("##fav_ctx")) {
              if (ImGui::MenuItem("Remove from Favorites")) toggle_action_favorite(ui, a);
              ImGui::EndPopup();
            }
            ImGui::PopID();
          }
        }
      }

      if (!ui.command_recent.empty()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
        if (ImGui::CollapsingHeader("Recent", ImGuiTreeNodeFlags_DefaultOpen)) {
          for (const auto& id : ui.command_recent) {
            const auto a = action_from_persistent_id(id);
            const auto* m = find_action_meta(a);
            if (!m) continue;

            ImGui::PushID(id.c_str());
            const std::string label = std::string("⟲ ") + m->label;
            if (ImGui::Selectable(label.c_str(), false)) {
              PaletteItem item;
              item.kind = PaletteKind::Action;
              item.action = a;
              activate_palette_item(item, sim, ui, selected_ship, selected_colony, selected_body, save_path, load_path);
              maybe_close();
              hud.palette_selected_idx = 0;
            }
            if (ImGui::IsItemHovered()) {
              hovered_action = m;
            }
            if (ImGui::BeginPopupContextItem("##recent_ctx")) {
              if (ImGui::MenuItem("Remove")) {
                // Remove a single entry.
                auto it = std::find(ui.command_recent.begin(), ui.command_recent.end(), id);
                if (it != ui.command_recent.end()) ui.command_recent.erase(it);
              }
              ImGui::EndPopup();
            }
            ImGui::PopID();
          }

          if (ImGui::SmallButton("Clear recent")) ui.command_recent.clear();
        }
      }

      ImGui::Separator();
    }

    // Context-sensitive actions.
    ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
    if (ImGui::CollapsingHeader("Context Actions", ImGuiTreeNodeFlags_DefaultOpen)) {
      // Selected system
      if (s.selected_system != kInvalidId) {
        const auto* sys = find_ptr(s.systems, s.selected_system);
        ImGui::TextUnformatted("System");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", sys ? sys->name.c_str() : "<unknown>");

        if (ImGui::SmallButton("System Map")) {
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::System;
          maybe_close();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Open the System Map focused on the selected system.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Galaxy Map")) {
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::Galaxy;
          maybe_close();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Open the Galaxy Map.");
        }

        ImGui::Separator();
      } else {
        ImGui::TextDisabled("No system selected (pick one on the Galaxy map or via search). ");
      }

      // Selected ship
      if (selected_ship != kInvalidId) {
        const auto* sh = find_ptr(s.ships, selected_ship);
        ImGui::TextUnformatted("Ship");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", sh ? sh->name.c_str() : "<unknown>");

        if (ImGui::SmallButton("Details")) {
          PaletteItem it;
          it.kind = PaletteKind::Ship;
          it.id = selected_ship;
          activate_palette_item(it, sim, ui, selected_ship, selected_colony, selected_body, save_path, load_path);
          maybe_close();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Open Details + System Map for the selected ship.");
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Fleet")) {
          ui.show_fleet_manager_window = true;
          ui.selected_fleet_id = sim.fleet_for_ship(selected_ship);
          maybe_close();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Open Fleet Manager focused on the ship's fleet.");
        }

        ImGui::Separator();
      } else {
        ImGui::TextDisabled("No ship selected.");
      }

      // Selected colony
      if (selected_colony != kInvalidId) {
        const auto* c = find_ptr(s.colonies, selected_colony);
        ImGui::TextUnformatted("Colony");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", c ? c->name.c_str() : "<unknown>");

        if (ImGui::SmallButton("Details")) {
          PaletteItem it;
          it.kind = PaletteKind::Colony;
          it.id = selected_colony;
          activate_palette_item(it, sim, ui, selected_ship, selected_colony, selected_body, save_path, load_path);
          maybe_close();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Open Details + System Map for the selected colony.");
        }

        ImGui::Separator();
      } else {
        ImGui::TextDisabled("No colony selected.");
      }

      // Selected body
      if (selected_body != kInvalidId) {
        const auto* b = find_ptr(s.bodies, selected_body);
        ImGui::TextUnformatted("Body");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", b ? b->name.c_str() : "<unknown>");

        if (ImGui::SmallButton("Details")) {
          PaletteItem it;
          it.kind = PaletteKind::Body;
          it.id = selected_body;
          activate_palette_item(it, sim, ui, selected_ship, selected_colony, selected_body, save_path, load_path);
          maybe_close();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Open Details + System Map for the selected body.");
        }
      } else {
        ImGui::TextDisabled("No body selected.");
      }

      ImGui::Spacing();
      ImGui::TextDisabled("Tip: use Search to jump to systems/ships/colonies/bodies.");
    }

    ImGui::Spacing();

    // Browsing panels for actions.
    bool show_browse = query.empty();
    if (!query.empty()) {
      ImGui::SetNextItemOpen(false, ImGuiCond_Appearing);
      show_browse = ImGui::CollapsingHeader("Browse Actions", 0);
    }

    if (show_browse) {
      auto draw_category = [&](const char* category, ImGuiTreeNodeFlags hflags) {
        // Only show the header if there is at least one matching action.
        bool has_any = false;
        for (const auto& m : kActionMetas) {
          if (std::string_view(m.category) != category) continue;
          if (!query.empty() && action_match_score(m, query) < 0) continue;
          has_any = true;
          break;
        }
        if (!has_any) return;

        if (!ImGui::CollapsingHeader(category, hflags)) return;

        const bool has_save_path = save_path && save_path[0] != '\0';
        const bool has_load_path = load_path && load_path[0] != '\0';

        for (const auto& m : kActionMetas) {
          if (std::string_view(m.category) != category) continue;
          if (!query.empty() && action_match_score(m, query) < 0) continue;

          bool enabled = true;
          if (m.action == PaletteAction::Save) enabled = has_save_path;
          if (m.action == PaletteAction::Load) enabled = has_load_path;

          ImGui::PushID((int)m.action);

          const std::string shortcut = effective_shortcut(m, ui);
          const char* shortcut_c = shortcut.empty() ? nullptr : shortcut.c_str();

          bool activated = false;
          if (m.toggles) {
            const bool checked = action_toggle_state(ui, m.action);
            activated = ImGui::MenuItem(m.label, shortcut_c, checked, enabled);
          } else {
            activated = ImGui::MenuItem(m.label, shortcut_c, false, enabled);
          }

          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            hovered_action = &m;
            draw_action_tooltip(m, ui);

            // Extra contextual hints for save/load paths.
            if ((m.action == PaletteAction::Save || m.action == PaletteAction::Load) && !enabled) {
              ImGui::BeginTooltip();
              ImGui::TextUnformatted(m.label);
              ImGui::Separator();
              ImGui::TextWrapped("No %s path set. Use the Save Tools window to configure paths.",
                                 (m.action == PaletteAction::Save) ? "save" : "load");
              ImGui::EndTooltip();
            }
          }

          if (activated) {
            run_action(m.action);
            maybe_close();
            hud.palette_selected_idx = 0;
          }

          ImGui::PopID();
        }
      };

      draw_category("Navigation", ImGuiTreeNodeFlags_DefaultOpen);
      draw_category("Windows", ImGuiTreeNodeFlags_DefaultOpen);
      draw_category("Automation", 0);
      draw_category("Tools", 0);
      draw_category("UI", 0);
      draw_category("Workspace", 0);
      draw_category("Game", 0);
    }

    ImGui::EndChild();

    // --- Right column ---
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("##cmd_console_right", ImVec2(0, 0), true);

    // Prefer hovered item details.
    if (hovered_action) {
      ImGui::TextUnformatted(hovered_action->label);
      {
        const bool is_fav = action_is_favorited(ui, hovered_action->action);
        if (ImGui::Button(is_fav ? "★ Unfavorite" : "☆ Favorite")) {
          toggle_action_favorite(ui, hovered_action->action);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(is_fav ? "Pinned" : "Pin this action for quick access");
      }
      ImGui::TextDisabled("Category: %s", hovered_action->category);
      {
        const std::string shortcut = effective_shortcut(*hovered_action, ui);
        if (!shortcut.empty()) ImGui::TextDisabled("Shortcut: %s", shortcut.c_str());
      }
      ImGui::Separator();
      const char* tip = (hovered_action->tooltip && hovered_action->tooltip[0] != '\0') ? hovered_action->tooltip
                                                                                        : hovered_action->label;
      ImGui::TextWrapped("%s", tip);
    } else if (!query.empty() && !results.empty()) {
      // Otherwise: show the currently selected search result details.
      const int idx = std::clamp(hud.palette_selected_idx, 0, (int)results.size() - 1);
      const PaletteItem& it = results[static_cast<std::size_t>(idx)];

      ImGui::TextUnformatted(it.label.c_str());
      ImGui::Separator();

      if (it.kind == PaletteKind::Action) {
        if (const ActionMeta* m = find_action_meta(it.action)) {
          {
            const bool is_fav = action_is_favorited(ui, it.action);
            if (ImGui::Button(is_fav ? "★ Unfavorite" : "☆ Favorite")) {
              toggle_action_favorite(ui, it.action);
            }
            ImGui::SameLine();
            ImGui::TextDisabled(is_fav ? "Pinned" : "Pin this action for quick access");
          }
          ImGui::TextDisabled("Category: %s", m->category);
          {
            const std::string shortcut = effective_shortcut(*m, ui);
            if (!shortcut.empty()) ImGui::TextDisabled("Shortcut: %s", shortcut.c_str());
          }
          ImGui::Spacing();
          const char* tip = (m->tooltip && m->tooltip[0] != '\0') ? m->tooltip : m->label;
          ImGui::TextWrapped("%s", tip);
        } else {
          ImGui::TextWrapped("(No metadata found for this action.)");
        }
      } else {
        ImGui::TextWrapped("Entity navigation: activating this will jump maps and open Details.");
        ImGui::Spacing();
        ImGui::TextDisabled("Tip: prefix search with a tag like 'System' or 'Ship' to narrow.");
      }
    } else {
      ImGui::TextUnformatted("Command Console");
      ImGui::Separator();
      ImGui::BulletText("Type to search across actions and entities.");
      ImGui::BulletText("Or browse collapsible panels (Windows/Tools/Workspace/Game). ");
      ImGui::BulletText("Context Actions adapt to your current selection.");
      ImGui::Spacing();
      ImGui::TextDisabled("Shift keeps the console open after running a command.");
    }

    ImGui::EndChild();

    ImGui::EndTable();
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

      // Optional narration.
      if (ui.screen_reader_enabled && ui.screen_reader_speak_toasts) {
        std::string msg = ev->message;
        if (msg.size() > 240) {
          msg.resize(237);
          msg += "...";
        }

        const char* prefix = (ev->level == EventLevel::Error) ? "Error: " : "Warning: ";
        ScreenReader::instance().announce_toast(std::string(prefix) + msg);
      }
    }

    hud.last_toast_seq = newest_seq;

    // Cap toast backlog.
    constexpr std::size_t kMaxToasts = 10;
    if (hud.toasts.size() > kMaxToasts) {
      hud.toasts.erase(hud.toasts.begin(), hud.toasts.begin() + (hud.toasts.size() - kMaxToasts));
    }
  }

  // Expire old toasts here as well, so toasts do not accumulate if rendering is paused
  // (e.g. guided tours that temporarily hide toast windows).
  const double now_prune = ImGui::GetTime();
  const double ttl_prune = std::max(0.5, (double)ui.event_toast_duration_sec);
  hud.toasts.erase(std::remove_if(hud.toasts.begin(), hud.toasts.end(), [&](const EventToast& t) {
                    return (now_prune - t.created_time_s) > ttl_prune;
                  }),
                  hud.toasts.end());
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
    if (t.custom) {
      ImGui::TextColored(event_level_color(t.level), "%s", "ALERT");
      ImGui::SameLine();
      const unsigned long long aid = (unsigned long long)(t.seq & 0x7fffffffffffffffull);
      ImGui::TextDisabled("#A%llu", aid);
    } else {
      ImGui::TextColored(event_level_color(t.level), "%s", event_level_short(t.level));
      ImGui::SameLine();
      ImGui::TextDisabled("#%llu", (unsigned long long)t.seq);
    }

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
    if (!t.custom) {
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
    } else {
      // Custom (UI-generated) toast actions.
      if (ImGui::SmallButton("Watchboard")) {
        ui.show_watchboard_window = true;
        ui.request_watchboard_focus_id = t.watch_id;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Copy path")) {
        if (!t.watch_path.empty()) ImGui::SetClipboardText(t.watch_path.c_str());
      }

      const std::string goto_ptr = !t.watch_rep_ptr.empty() ? t.watch_rep_ptr : t.watch_path;
      const bool can_goto = !goto_ptr.empty() && (goto_ptr.find('*') == std::string::npos);
      ImGui::SameLine();
      ImGui::BeginDisabled(!can_goto);
      if (ImGui::SmallButton("JSON Explorer")) {
        ui.show_json_explorer_window = true;
        ui.request_json_explorer_goto_path = goto_ptr;
      }
      ImGui::EndDisabled();

      if (t.watch_id != 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Mute")) {
          for (auto& w : ui.json_watch_items) {
            if (w.id == t.watch_id) {
              w.alert_enabled = false;
              break;
            }
          }
        }
      }
    }


    // Advance stack.
    const ImVec2 win_sz = ImGui::GetWindowSize();
    y += win_sz.y + 8.0f;

    ImGui::End();
  }
}

} // namespace nebula4x::ui
