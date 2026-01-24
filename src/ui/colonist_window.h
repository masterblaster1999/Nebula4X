#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A UI window that previews (and optionally applies) the simulation's auto-colonist
// style population transports as a deterministic "dry-run" plan.
void draw_colonist_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
