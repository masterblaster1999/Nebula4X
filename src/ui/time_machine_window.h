#pragma once

#include "nebula4x/core/ids.h"
#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Time Machine: an in-memory state history recorder for debugging and UX flows.
//
// Captures save-game JSON snapshots (from the live game JSON cache), computes
// compact diffs between snapshots, and can store history either as full snapshots
// or as a delta chain of RFC 7396 JSON Merge Patches with periodic checkpoints.
//
// Lets the user export/copy diffs or patches, export the full history as a delta-save, or jump
// directly to changed JSON Pointers in the JSON Explorer.
//
// NOTE: This is a UI-only tool. It does not persist snapshot history in the
// save-game; only its UI preferences are persisted via ui_prefs.json.
void draw_time_machine_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

} // namespace nebula4x::ui
