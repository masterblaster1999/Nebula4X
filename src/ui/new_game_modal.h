#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A small modal dialog that lets the user start a new scenario.
//
// This is UI-only: it does not persist in the save-game JSON.
void draw_new_game_modal(Simulation& sim, UIState& ui);

} // namespace nebula4x::ui
