#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Contracts window: lightweight "mission board" backed by GameState::contracts.
//
// Contracts are procedurally generated from existing world state (anomalies,
// unsurveyed jump points, salvageable wrecks). Players can accept/abandon and
// assign them to ships.
void draw_contracts_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
