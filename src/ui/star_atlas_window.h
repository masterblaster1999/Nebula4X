#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// UI-only window: a procedural Star Atlas that groups visible systems into
// deterministic "constellations".
//
// This is intended as a flavor/navigation aid and as a debugging lens for the
// procedural generator: the atlas should feel coherent under fog-of-war and
// should never leak undiscovered systems.
void draw_star_atlas_window(Simulation& sim, UIState& ui);

}  // namespace nebula4x::ui
