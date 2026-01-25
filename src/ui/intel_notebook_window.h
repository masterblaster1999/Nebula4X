#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Intel Notebook: unified knowledge-base for player-authored system notes + curated journal.
//
// This UI is built on existing persisted data structures:
//   - Faction::system_notes (SystemIntelNote)
//   - Faction::journal      (JournalEntry)
//
// It does not introduce new simulation concepts; it only makes the existing data
// discoverable and editable in a dedicated window.
void draw_intel_notebook_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

} // namespace nebula4x::ui
