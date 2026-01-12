#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

struct UIPrefActions {
  bool load_ui_prefs{false};
  bool save_ui_prefs{false};
  bool reset_ui_theme{false};
  bool reset_window_layout{false};
};

void draw_main_menu(Simulation& sim, UIState& ui, char* save_path, char* load_path, char* ui_prefs_path,
                    UIPrefActions& actions);

void draw_left_sidebar(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony);

void draw_right_sidebar(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

// Optional windows (toggled via the main menu).
void draw_directory_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);
void draw_settings_window(UIState& ui, char* ui_prefs_path, UIPrefActions& actions);

} // namespace nebula4x::ui
