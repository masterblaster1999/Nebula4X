#include "ui/workspace_presets.h"

#include <string>

#include "ui/ui_state.h"
#include "ui/window_management.h"

namespace nebula4x::ui {

namespace {

static const WorkspacePresetInfo kPresets[] = {
    {"Default", "Core + Directory. A balanced baseline."},
    {"Minimal", "Just the essentials (Map + Details)."},
    {"Economy", "Production / Economy / Planner + Timeline."},
    {"Design", "Design Studio + Balance Lab."},
    {"Intel", "Intel + Diplomacy + Timeline."},
};

}  // namespace

const WorkspacePresetInfo* workspace_preset_infos(std::size_t* count) {
  if (count) *count = sizeof(kPresets) / sizeof(kPresets[0]);
  return kPresets;
}

void apply_workspace_preset(const char* preset_name, UIState& ui) {
  const std::string p = preset_name ? std::string(preset_name) : std::string();

  // Integration with the window management system: if the user is in Focus Mode,
  // applying a workspace should exit Focus Mode first (Focus Mode is a temporary declutter tool).
  if (focus_mode_enabled(ui)) {
    set_focus_mode(ui, false);
  }

  auto hide_all_major = [&]() {
    ui.show_controls_window = false;
    ui.show_map_window = false;
    ui.show_details_window = false;
    ui.show_directory_window = false;
    ui.show_production_window = false;
    ui.show_economy_window = false;
    ui.show_planner_window = false;
    ui.show_freight_window = false;
    ui.show_fuel_window = false;
    ui.show_sustainment_window = false;
    ui.show_time_warp_window = false;
    ui.show_timeline_window = false;
    ui.show_design_studio_window = false;
    ui.show_balance_lab_window = false;
    ui.show_intel_window = false;
    ui.show_diplomacy_window = false;
    ui.show_save_tools_window = false;
  };

  if (p == "Default") {
    hide_all_major();
    ui.show_controls_window = true;
    ui.show_map_window = true;
    ui.show_details_window = true;
    ui.show_directory_window = true;
    ui.show_status_bar = true;
    return;
  }

  if (p == "Minimal") {
    hide_all_major();
    ui.show_map_window = true;
    ui.show_details_window = true;
    ui.show_status_bar = true;
    return;
  }

  if (p == "Economy") {
    hide_all_major();
    ui.show_map_window = true;
    ui.show_details_window = true;
    ui.show_directory_window = true;
    ui.show_production_window = true;
    ui.show_economy_window = true;
    ui.show_planner_window = true;
    ui.show_timeline_window = true;
    ui.show_status_bar = true;
    return;
  }

  if (p == "Design") {
    hide_all_major();
    ui.show_map_window = true;
    ui.show_details_window = true;
    ui.show_design_studio_window = true;
    ui.show_balance_lab_window = true;
    ui.show_status_bar = true;
    return;
  }

  if (p == "Intel") {
    hide_all_major();
    ui.show_map_window = true;
    ui.show_details_window = true;
    ui.show_intel_window = true;
    ui.show_diplomacy_window = true;
    ui.show_timeline_window = true;
    ui.show_status_bar = true;
    return;
  }
}

}  // namespace nebula4x::ui
