#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Design Balance Lab
//
// Runs duel tournaments (round-robin) between ship designs and summarizes results.
// Intended for balancing content and custom designs.
void draw_balance_lab_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
