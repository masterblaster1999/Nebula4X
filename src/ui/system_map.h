#pragma once

#include "nebula4x/core/simulation.h"

namespace nebula4x::ui {

void draw_system_map(Simulation& sim, Id& selected_ship, double& zoom, Vec2& pan);

} // namespace nebula4x::ui
