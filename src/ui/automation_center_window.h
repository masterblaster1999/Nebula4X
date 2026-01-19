#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A UI window that provides bulk management of ship automation flags.
//
// The simulation supports many ship-level automation behaviors (explore/freight/mine/etc.).
// This window makes those features actually usable at scale by:
//   - Listing ships with their automation configuration and current status
//   - Allowing multi-select + bulk operations (enable/disable, thresholds)
//   - Providing "procedural" mission presets and a heuristic "suggest" button
void draw_automation_center_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                                   Id& selected_body);

}  // namespace nebula4x::ui
