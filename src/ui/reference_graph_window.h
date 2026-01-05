#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Procedural UI: visualize entity-id relationships as an interactive graph.
void draw_reference_graph_window(Simulation& sim, UIState& ui);

} // namespace nebula4x::ui
