#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Fleet Manager window: global fleet list + quick actions.
//
// Complements the Fleet tab in the Details panel by providing:
//  - A sortable/searchable table of all fleets.
//  - A right-side inspector with mission summary and jump-route preview.
//  - A "Fleet Forge" tab that proposes new fleets from unassigned ships.
void draw_fleet_manager_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
