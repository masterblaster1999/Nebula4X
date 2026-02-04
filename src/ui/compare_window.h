#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Compare / Diff window:
// - Pick two entities (or snapshot one side) and view a flattened scalar diff.
// - Export RFC 7396 JSON Merge Patch (A -> B) for debugging / save-edit workflows.
void draw_compare_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

}  // namespace nebula4x::ui
