#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Battle Forecast window: quick fleet-vs-fleet outcome estimate.
//
// This is intentionally a "planning" tool:
//  - It uses a simplified combat model (see core/fleet_battle_forecast.*).
//  - It is deterministic and fast enough to run interactively in the UI.
//  - It should be treated as guidance, not a promise.
void draw_battle_forecast_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
