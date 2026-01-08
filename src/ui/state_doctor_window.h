#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// UI tooling window: validates the currently loaded game state (save integrity)
// and provides a safe workflow to preview/apply the built-in state fixer.
void draw_state_doctor_window(Simulation& sim, UIState& ui);

}  // namespace nebula4x::ui
