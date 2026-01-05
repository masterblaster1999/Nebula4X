#pragma once

#include "nebula4x/core/simulation.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

// OmniSearch (procedural): a global fuzzy search over the live game JSON.
//
// This is a developer/tooling window intended to quickly jump to any
// piece of state by searching keys/paths and scalar values.
void draw_omni_search_window(Simulation& sim, UIState& ui);

} // namespace nebula4x::ui
