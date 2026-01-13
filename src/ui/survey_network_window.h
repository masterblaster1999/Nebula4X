#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A window for monitoring and directing jump-point surveys under fog-of-war.
//
// Jump point surveys gate route knowledge: unsurveyed exits are treated as unknown
// until a ship accumulates enough survey progress near the jump point.
void draw_survey_network_window(Simulation& sim, UIState& ui,
                                Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
