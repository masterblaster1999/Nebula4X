#pragma once

#include "nebula4x/core/simulation.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

// Scoreboard + victory rule editor.
void draw_victory_window(Simulation& sim, UIState& ui);

}  // namespace nebula4x::ui
