#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Selection Navigator: manages selection history (back/forward) and pinned
// bookmarks for fast entity/system jumps.
void draw_navigator_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

} // namespace nebula4x::ui
