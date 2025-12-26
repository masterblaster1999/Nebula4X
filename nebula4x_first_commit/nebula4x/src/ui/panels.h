#pragma once

#include "nebula4x/core/simulation.h"

namespace nebula4x::ui {

void draw_main_menu(Simulation& sim, char* save_path, char* load_path);

void draw_left_sidebar(Simulation& sim, Id& selected_ship, Id& selected_colony);

void draw_right_sidebar(Simulation& sim, Id selected_ship, Id& selected_colony);

} // namespace nebula4x::ui
