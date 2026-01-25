#pragma once

#include "nebula4x/core/simulation.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

// OmniSearch: a global fuzzy search over commands, entities, docs, and live game JSON.
//
// OmniSearch is meant to be a "universal jumper":
//   - Actions/commands (window toggles, navigation helpers)
//   - Entities (ships/colonies/bodies/systems, plus other id-bearing arrays)
//   - Docs (the in-game Codex markdown pages)
//   - Raw JSON nodes (keys/paths/scalar values) for debugging/modding
//
// It can also drive selection/navigation (selected_* ids are updated when jumping).
void draw_omni_search_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

} // namespace nebula4x::ui
