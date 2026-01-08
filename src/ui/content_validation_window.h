#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// UI tooling window: validates the currently loaded content bundle (blueprints + techs)
// and presents errors/warnings in a searchable table.
void draw_content_validation_window(Simulation& sim, UIState& ui);

}  // namespace nebula4x::ui
