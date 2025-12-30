#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A dockable intel window focused on contacts (sensor detections) and system awareness.
//
// Features:
// - Radar view for the selected system (contacts + friendly sensors + bodies)
// - Filterable/sortable contact list
// - One-click navigation: jump to the system map and center on last-known contact positions
void draw_intel_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

} // namespace nebula4x::ui
