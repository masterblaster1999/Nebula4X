#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A window for managing faction-level ship design build targets.
//
// Faction::ship_design_targets enables a lightweight "auto-shipyard" system that
// will enqueue auto_queued build orders to maintain desired counts of each design.
// This window makes those targets visible/editable and shows current/pending counts.
void draw_shipyard_targets_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
