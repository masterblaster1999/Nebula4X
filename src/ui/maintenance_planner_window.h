#pragma once

#include "nebula4x/core/game_state.h"

namespace nebula4x {
class Simulation;
}

namespace nebula4x::ui {

struct UIState;

// Strategic logistics helper: plan and apply ship maintenance routing.
void draw_maintenance_planner_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                                    Id& selected_body);

}  // namespace nebula4x::ui
