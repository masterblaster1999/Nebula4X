#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A dockable visualization of persistent simulation events (GameState::events).
//
// The window focuses on fast navigation: click event markers to select them, then
// jump to referenced ships/colonies/systems, or open the traditional event log.
void draw_timeline_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

} // namespace nebula4x::ui
