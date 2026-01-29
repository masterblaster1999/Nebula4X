#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Security Planner: analyzes trade exposure (procedural trade network) against
// piracy / blockade disruption and suggests actionable patrol targets.
void draw_security_planner_window(Simulation& sim, UIState& ui,
                                  Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
