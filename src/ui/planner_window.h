#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A strategic planning dashboard that merges best-effort forecasts into a
// single chronological list (research, colony production, optionally ship
// order ETAs).
//
// This is a UI-only surface (not persisted in saves).
void draw_planner_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

} // namespace nebula4x::ui
