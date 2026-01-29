#include "ui/window_management.h"

#include <algorithm>
#include <cstring>

namespace nebula4x::ui {

namespace {

static float status_bar_height_px(const UIState& ui) {
  if (!ui.show_status_bar) return 0.0f;
  const ImGuiStyle& style = ImGui::GetStyle();
  // Mirrors hud.cpp: frame height + vertical padding.
  return ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
}

struct Rect {
  ImVec2 min;
  ImVec2 max;

  ImVec2 size() const { return ImVec2(max.x - min.x, max.y - min.y); }
};

static Rect available_work_rect(const UIState& ui) {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  Rect r;
  r.min = vp ? vp->WorkPos : ImVec2(0, 0);
  ImVec2 sz = vp ? vp->WorkSize : ImVec2(1280, 720);

  // Keep popups out of the status bar area.
  const float sb = status_bar_height_px(ui);
  sz.y = std::max(0.0f, sz.y - sb);

  r.max = ImVec2(r.min.x + sz.x, r.min.y + sz.y);
  return r;
}

static ImVec2 clamp_pos_in_rect(const Rect& r, const ImVec2& pos, const ImVec2& size, float pad) {
  ImVec2 p = pos;
  p.x = std::clamp(p.x, r.min.x + pad, r.max.x - size.x - pad);
  p.y = std::clamp(p.y, r.min.y + pad, r.max.y - size.y - pad);
  return p;
}

static ImVec2 clamp_size_to_rect(const Rect& r, ImVec2 size) {
  const ImVec2 avail = r.size();
  // Avoid tiny popups.
  size.x = std::max(size.x, 320.0f);
  size.y = std::max(size.y, 220.0f);

  // Clamp to viewport.
  size.x = std::min(size.x, std::max(320.0f, avail.x * 0.95f));
  size.y = std::min(size.y, std::max(220.0f, avail.y * 0.95f));
  return size;
}

static const std::vector<WindowSpec>& build_specs() {
  static std::vector<WindowSpec> specs = {
      // Core workspace.
      {"controls", "Controls", "Controls", "Core", ImVec2(360, 720), &UIState::show_controls_window, true, true,
       WindowLaunchMode::Docked},
      {"map", "Map", "Map", "Core", ImVec2(980, 720), &UIState::show_map_window, true, true,
       WindowLaunchMode::Docked},
      {"details", "Details", "Details", "Core", ImVec2(420, 720), &UIState::show_details_window, true, true,
       WindowLaunchMode::Docked},

      // Main panels.
      {"directory", "Directory", "Directory", "Core", ImVec2(980, 520), &UIState::show_directory_window, false, true,
       WindowLaunchMode::Docked},
      {"production", "Production", "Production", "Core", ImVec2(1100, 680), &UIState::show_production_window, false,
       true, WindowLaunchMode::Docked},
      {"economy", "Economy", "Economy", "Core", ImVec2(1100, 680), &UIState::show_economy_window, false, true,
       WindowLaunchMode::Docked},
      {"research_roadmap", "Research Roadmap", "Research Roadmap", "Core", ImVec2(980, 720),
       &UIState::show_research_roadmap_window, false, true, WindowLaunchMode::Docked},
      {"planner", "Planner", "Planner", "Core", ImVec2(1160, 720), &UIState::show_planner_window, false, true,
       WindowLaunchMode::Docked},
      {"regions", "Regions", "Regions", "Core", ImVec2(1040, 700), &UIState::show_regions_window, false, true,
       WindowLaunchMode::Docked},
      {"timeline", "Timeline", "Timeline", "Core", ImVec2(1160, 560), &UIState::show_timeline_window, false, true,
       WindowLaunchMode::Docked},
      {"notifications", "Notification Center", "Notifications", "Core", ImVec2(900, 640),
       &UIState::show_notifications_window, false, true, WindowLaunchMode::Docked},

      // Logistics planners.
      {"freight", "Freight Planner", "Freight Planner", "Logistics", ImVec2(980, 720), &UIState::show_freight_window,
       false, true, WindowLaunchMode::Popup},
      {"mine", "Mine Planner", "Mine Planner", "Logistics", ImVec2(1040, 690), &UIState::show_mine_window, false, true,
       WindowLaunchMode::Popup},
      {"fuel", "Fuel Planner", "Fuel Planner", "Logistics", ImVec2(980, 720), &UIState::show_fuel_window, false, true,
       WindowLaunchMode::Popup},
      {"salvage", "Salvage Planner", "Salvage Planner", "Logistics", ImVec2(980, 720), &UIState::show_salvage_window,
       false, true, WindowLaunchMode::Popup},
      {"contracts", "Contracts", "Contracts", "Logistics", ImVec2(980, 720), &UIState::show_contracts_window, false,
       true, WindowLaunchMode::Popup},
      {"sustainment", "Sustainment Planner", "Sustainment Planner", "Logistics", ImVec2(980, 720),
       &UIState::show_sustainment_window, false, true, WindowLaunchMode::Popup},
      {"repair_planner", "Repair Planner", "Repair Planner", "Logistics", ImVec2(1100, 720),
       &UIState::show_repair_planner_window, false, true, WindowLaunchMode::Popup},
      {"maintenance_planner", "Maintenance Planner", "Maintenance Planner", "Logistics", ImVec2(1100, 720),
       &UIState::show_maintenance_planner_window, false, true, WindowLaunchMode::Popup},
      {"troops", "Troop Logistics", "Troop Logistics", "Logistics", ImVec2(980, 720), &UIState::show_troop_window,
       false, true, WindowLaunchMode::Popup},
      {"population", "Population Logistics", "Population Logistics", "Logistics", ImVec2(980, 720),
       &UIState::show_colonist_window, false, true, WindowLaunchMode::Popup},
      {"terraforming", "Terraforming Planner", "Terraforming Planner", "Logistics", ImVec2(980, 720),
       &UIState::show_terraforming_window, false, true, WindowLaunchMode::Popup},

      // Fleet / operations.
      {"fleet_manager", "Fleet Manager", "Fleet Manager", "Operations", ImVec2(1100, 720),
       &UIState::show_fleet_manager_window, false, true, WindowLaunchMode::Docked},
      {"security_planner", "Security Planner", "Security Planner", "Operations", ImVec2(1100, 720),
       &UIState::show_security_planner_window, false, true, WindowLaunchMode::Docked},
      {"survey_network", "Survey Network", "Survey Network", "Operations", ImVec2(980, 680),
       &UIState::show_survey_network_window, false, true, WindowLaunchMode::Docked},
      {"time_warp", "Time Warp", "Time Warp", "Operations", ImVec2(720, 420), &UIState::show_time_warp_window, false,
       true, WindowLaunchMode::Popup},

      // Automation & advisors.
      {"advisor", "Advisor##advisor", "Advisor", "Automation", ImVec2(860, 640), &UIState::show_advisor_window, false,
       true, WindowLaunchMode::Popup},
      {"colony_profiles", "Colony Profiles", "Colony Profiles", "Automation", ImVec2(900, 680),
       &UIState::show_colony_profiles_window, false, true, WindowLaunchMode::Popup},
      {"ship_profiles", "Ship Profiles", "Ship Profiles", "Automation", ImVec2(900, 680),
       &UIState::show_ship_profiles_window, false, true, WindowLaunchMode::Popup},
      {"automation_center", "Automation Center", "Automation Center", "Automation", ImVec2(1080, 720),
       &UIState::show_automation_center_window, false, true, WindowLaunchMode::Docked},
      {"shipyard_targets", "Shipyard Targets", "Shipyard Targets", "Automation", ImVec2(980, 680),
       &UIState::show_shipyard_targets_window, false, true, WindowLaunchMode::Popup},

      // Design, intel, diplomacy.
      {"design_studio", "Design Studio", "Design Studio", "Empire", ImVec2(1160, 720),
       &UIState::show_design_studio_window, false, true, WindowLaunchMode::Docked},
      {"balance_lab", "Balance Lab", "Balance Lab", "Empire", ImVec2(1100, 720), &UIState::show_balance_lab_window,
       false, true, WindowLaunchMode::Popup},
      {"battle_forecast", "Battle Forecast", "Battle Forecast", "Empire", ImVec2(980, 700),
       &UIState::show_battle_forecast_window, false, true, WindowLaunchMode::Popup},
      {"intel", "Intel", "Intel", "Empire", ImVec2(1080, 720), &UIState::show_intel_window, false, true,
       WindowLaunchMode::Docked},
      {"intel_notebook", "Intel Notebook", "Intel Notebook", "Empire", ImVec2(980, 720),
       &UIState::show_intel_notebook_window, false, true, WindowLaunchMode::Docked},
      {"diplomacy", "Diplomacy Graph", "Diplomacy", "Empire", ImVec2(1080, 720), &UIState::show_diplomacy_window,
       false, true, WindowLaunchMode::Docked},
      {"victory", "Victory & Score", "Victory & Score", "Empire", ImVec2(860, 580), &UIState::show_victory_window,
       false, true, WindowLaunchMode::Popup},

      // Atlas.
      {"procgen_atlas", "ProcGen Atlas", "ProcGen Atlas", "Atlas", ImVec2(1080, 720),
       &UIState::show_procgen_atlas_window, false, true, WindowLaunchMode::Popup},
      {"star_atlas", "Star Atlas", "Star Atlas", "Atlas", ImVec2(1080, 720), &UIState::show_star_atlas_window, false,
       true, WindowLaunchMode::Popup},

      // Tools.
      {"settings", "Settings", "Settings", "Tools", ImVec2(1080, 720), &UIState::show_settings_window, false, true,
       WindowLaunchMode::Popup},
      {"layout_profiles", "Layout Profiles", "Layout Profiles", "Tools", ImVec2(980, 640),
       &UIState::show_layout_profiles_window, false, true, WindowLaunchMode::Popup},
      {"window_manager", "Window Manager", "Window Manager", "Tools", ImVec2(980, 680),
       &UIState::show_window_manager_window, false, true, WindowLaunchMode::Popup},
      {"save_tools", "Save Tools (Diff / Patch)", "Save Tools", "Tools", ImVec2(980, 720),
       &UIState::show_save_tools_window, false, true, WindowLaunchMode::Popup},
      {"time_machine", "Time Machine", "Time Machine", "Tools", ImVec2(1100, 720),
       &UIState::show_time_machine_window, false, true, WindowLaunchMode::Popup},
      {"compare", "Compare / Diff", "Compare / Diff", "Tools", ImVec2(980, 680), &UIState::show_compare_window, false,
       true, WindowLaunchMode::Popup},
      {"omni_search", "OmniSearch", "OmniSearch", "Tools", ImVec2(980, 640), &UIState::show_omni_search_window,
       false, true, WindowLaunchMode::Popup},
      {"navigator", "Navigator", "Navigator", "Tools", ImVec2(860, 640), &UIState::show_navigator_window, false, true,
       WindowLaunchMode::Popup},
      {"json_explorer", "JSON Explorer", "JSON Explorer", "Tools", ImVec2(980, 720),
       &UIState::show_json_explorer_window, false, true, WindowLaunchMode::Popup},
      {"content_validation", "Content Validation", "Content Validation", "Tools", ImVec2(980, 680),
       &UIState::show_content_validation_window, false, true, WindowLaunchMode::Popup},
      {"state_doctor", "State Doctor", "State Doctor", "Tools", ImVec2(980, 680), &UIState::show_state_doctor_window,
       false, true, WindowLaunchMode::Popup},
      {"trace_viewer", "Trace Viewer", "Trace Viewer", "Tools", ImVec2(1100, 720),
       &UIState::show_trace_viewer_window, false, true, WindowLaunchMode::Popup},
      {"entity_inspector", "Entity Inspector", "Entity Inspector", "Tools", ImVec2(860, 540),
       &UIState::show_entity_inspector_window, false, true, WindowLaunchMode::Popup},
      {"reference_graph", "Reference Graph", "Reference Graph", "Tools", ImVec2(980, 680),
       &UIState::show_reference_graph_window, false, true, WindowLaunchMode::Popup},
      {"watchboard", "Watchboard", "Watchboard", "Tools", ImVec2(980, 680), &UIState::show_watchboard_window, false,
       true, WindowLaunchMode::Popup},
      {"data_lenses", "Data Lenses", "Data Lenses", "Tools", ImVec2(980, 680), &UIState::show_data_lenses_window,
       false, true, WindowLaunchMode::Popup},
      {"dashboards", "Dashboards", "Dashboards", "Tools", ImVec2(980, 680), &UIState::show_dashboards_window, false,
       true, WindowLaunchMode::Popup},
      {"pivot_tables", "Pivot Tables", "Pivot Tables", "Tools", ImVec2(980, 680), &UIState::show_pivot_tables_window,
       false, true, WindowLaunchMode::Popup},
      {"ui_forge", "UI Forge", "UI Forge", "Tools", ImVec2(1100, 720), &UIState::show_ui_forge_window, false, true,
       WindowLaunchMode::Popup},
      {"context_forge", "Context Forge", "Context Forge", "Tools", ImVec2(1100, 720), &UIState::show_context_forge_window,
       false, true, WindowLaunchMode::Popup},

      // Help (opens as a regular window; keep popup enabled for clutter management).
      {"help", "Help / Codex", "Help / Codex", "Tools", ImVec2(980, 720), &UIState::show_help_window, false, true,
       WindowLaunchMode::Popup},

      // Command Console has its own special positioning (top-centered palette).
      {"command_console", "Command Console", "Command Console", "Tools", ImVec2(860, 560), &UIState::show_command_palette,
       false, false, WindowLaunchMode::Popup},
  };
  return specs;
}

}  // namespace

const std::vector<WindowSpec>& window_specs() {
  return build_specs();
}

const WindowSpec* find_window_spec(const char* id) {
  if (!id) return nullptr;
  for (const auto& s : window_specs()) {
    if (std::strcmp(s.id, id) == 0) return &s;
  }
  return nullptr;
}

WindowLaunchMode effective_launch_mode(const UIState& ui, const WindowSpec& spec) {
  if (!spec.supports_popup) return WindowLaunchMode::Docked;

  // Explicit per-window override.
  if (auto it = ui.window_launch_overrides.find(spec.id); it != ui.window_launch_overrides.end()) {
    return (it->second == 1) ? WindowLaunchMode::Popup : WindowLaunchMode::Docked;
  }

  // Global popup-first mode affects all non-core windows.
  if (ui.window_popup_first_mode && !spec.core) {
    return WindowLaunchMode::Popup;
  }

  return spec.default_mode;
}

void request_popout(UIState& ui, const char* id) {
  const WindowSpec* spec = find_window_spec(id);
  if (!spec) return;

  // Ensure it's open.
  ui.*(spec->open_flag) = true;
  ui.window_popout_request[spec->id] = true;
}

void prepare_window_for_draw(UIState& ui, const char* id) {
  const WindowSpec* spec = find_window_spec(id);
  if (!spec) return;
  if (!spec->supports_popup) return;

  const bool open = ui.*(spec->open_flag);
  if (!open) return;

  // Detect first frame of being open (transition false->true).
  bool prev_open = false;
  if (auto it = ui.window_open_prev.find(spec->id); it != ui.window_open_prev.end()) prev_open = it->second;
  const bool just_opened = open && !prev_open;

  // One-shot requests (e.g. from Window Manager).
  const bool popout_requested = (ui.window_popout_request.find(spec->id) != ui.window_popout_request.end());

  WindowLaunchMode mode = effective_launch_mode(ui, *spec);
  if (popout_requested) mode = WindowLaunchMode::Popup;

  if (mode != WindowLaunchMode::Popup) return;
  if (!just_opened && !popout_requested) return;

  // Consume one-shot.
  ui.window_popout_request.erase(spec->id);

  const Rect r = available_work_rect(ui);
  const ImVec2 avail = r.size();

  ImVec2 size = spec->popup_size;
  if (size.x <= 0.0f) size.x = avail.x * 0.72f;
  if (size.y <= 0.0f) size.y = avail.y * 0.72f;
  size = clamp_size_to_rect(r, size);

  const float step = std::max(0.0f, ui.window_popup_cascade_step_px);
  const int n = ui.window_popup_cascade_index++;
  const ImVec2 offset(step * float(n % 10), step * float(n % 10));

  const ImVec2 center(r.min.x + avail.x * 0.5f, r.min.y + avail.y * 0.5f);
  ImVec2 pos(center.x - size.x * 0.5f + offset.x, center.y - size.y * 0.5f + offset.y);
  pos = clamp_pos_in_rect(r, pos, size, 8.0f);

#ifdef IMGUI_HAS_DOCK
  // Force undocked.
  ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
#endif
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
  if (ui.window_popup_auto_focus) {
    ImGui::SetNextWindowFocus();
  }
}

void window_management_end_frame(UIState& ui) {
  // Update open state tracking.
  for (const auto& s : window_specs()) {
    ui.window_open_prev[s.id] = ui.*(s.open_flag);
  }

  // Keep cascade index from growing without bound.
  ui.window_popup_cascade_index %= 1000;

  // Drop stale one-shot popout requests for windows that are no longer open.
  for (auto it = ui.window_popout_request.begin(); it != ui.window_popout_request.end();) {
    const WindowSpec* spec = find_window_spec(it->first.c_str());
    if (!spec) {
      it = ui.window_popout_request.erase(it);
      continue;
    }
    const bool open = ui.*(spec->open_flag);
    if (!open) {
      it = ui.window_popout_request.erase(it);
      continue;
    }
    ++it;
  }
}

void set_focus_mode(UIState& ui, bool enabled) {
  if (enabled == ui.window_focus_mode) return;

  if (enabled) {
    ui.window_focus_restore.clear();
    for (const auto& s : window_specs()) {
      ui.window_focus_restore[s.id] = ui.*(s.open_flag);
    }

    // Hide everything except the Map (and keep status bar as-is).
    for (const auto& s : window_specs()) {
      if (std::strcmp(s.id, "map") == 0) continue;
      ui.*(s.open_flag) = false;
    }
    ui.show_map_window = true;
    ui.window_focus_mode = true;
  } else {
    for (const auto& s : window_specs()) {
      if (auto it = ui.window_focus_restore.find(s.id); it != ui.window_focus_restore.end()) {
        ui.*(s.open_flag) = it->second;
      }
    }
    ui.window_focus_restore.clear();
    ui.window_focus_mode = false;
  }
}

void toggle_focus_mode(UIState& ui) {
  set_focus_mode(ui, !ui.window_focus_mode);
}

bool focus_mode_enabled(const UIState& ui) {
  return ui.window_focus_mode;
}

}  // namespace nebula4x::ui
