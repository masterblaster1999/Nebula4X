#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

void draw_system_map(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body,
                     double& zoom, Vec2& pan);

} // namespace nebula4x::ui
