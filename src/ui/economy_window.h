#pragma once

#include "nebula4x/core/simulation.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

// Global economy overview: Industry + Mining + Tech Tree.
void draw_economy_window(Simulation& sim, UIState& ui, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
