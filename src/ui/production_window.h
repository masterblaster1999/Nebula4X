#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Visual production planner: shipyard + construction queue schedules.
//
// This is a UI convenience window that helps players understand *when* queued
// production will complete, and what is currently blocking progress.
void draw_production_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

} // namespace nebula4x::ui
