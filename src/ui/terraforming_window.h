#pragma once

#include "nebula4x/core/ids.h"
#include "nebula4x/core/simulation.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

// Empire-wide overview + forecast for terraforming projects.
void draw_terraforming_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

} // namespace nebula4x::ui
