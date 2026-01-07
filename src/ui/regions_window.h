#pragma once

#include "nebula4x/core/simulation.h"

#include "ui_state.h"

namespace nebula4x::ui {

// Regions/Sectors window.
//
// Provides a galaxy-level overview of procedural Regions ("sectors"):
//  - filtering + sorting
//  - summary stats (systems/colonies/pop)
//  - quick focus actions (center/fit galaxy map)
//  - selection integration with galaxy-map region overlays
void draw_regions_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

} // namespace nebula4x::ui
