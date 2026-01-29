#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Research Roadmap: multi-target prerequisite planning + schedule preview.
//
// Allows selecting one or more target techs, computes the missing prerequisites
// (in a safe queue order), previews the completion timeline, and can apply the
// plan to the faction's active project / research queue.
void draw_research_roadmap_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
