#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Global repair routing + shipyard backlog forecasting.
// Provides a best-effort assignment of damaged ships to repair-capable colonies (shipyards),
// and allows issuing one-click travel/orbit orders to get ships repaired.
void draw_repair_planner_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
