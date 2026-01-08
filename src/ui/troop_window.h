#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A UI window that previews (and optionally applies) the simulation's auto-troop
// style troop transports as a deterministic "dry-run" plan.
void draw_troop_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
