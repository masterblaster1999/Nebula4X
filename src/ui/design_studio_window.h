#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A dockable design visualization window.
//
// The goal of the Design Studio is to give a fast, graphical understanding of a
// ship design's component mix and major derived stats, with clickable links back
// into the existing Details/Map workflows.
void draw_design_studio_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
