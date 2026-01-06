#pragma once

#include "nebula4x/core/simulation.h"

#include "ui_state.h"

namespace nebula4x::ui {

// Advisor window: aggregated issue list + quick actions.
//
// This window is intentionally "gameplay focused" (unlike developer tools
// such as JSON Explorer). It helps players spot empire problems quickly:
//  - logistics shortfalls
//  - low fuel / damage ships
//  - colony habitation shortfalls
//  - garrison target problems
void draw_advisor_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
