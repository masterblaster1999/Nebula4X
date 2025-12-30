#pragma once

#include "nebula4x/core/simulation.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

// Diplomacy Graph: an interactive relationship visualization between factions.
//
// - Pan/zoom canvas with node/edge rendering.
// - Click nodes/edges to inspect and edit stances.
// - Links into Details->Diplomacy and Timeline via UIState requests.
void draw_diplomacy_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                           Id& selected_body);

} // namespace nebula4x::ui
