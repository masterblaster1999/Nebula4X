#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Fleet sustainment planning: helps set colony stockpile targets for fueling, rearming,
// and (optionally) maintenance supplies to support a selected fleet.
void draw_sustainment_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
