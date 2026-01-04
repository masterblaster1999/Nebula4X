#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A UI window that previews (and optionally applies) a deterministic, best-effort
// plan of ship-to-ship fuel transfers using the game's auto-tanker rules.
void draw_fuel_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
