#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// JSON Explorer (procedural inspector)
//
// A data-driven tree/table viewer for JSON documents. Primarily intended for
// inspecting the current in-memory game state (via serialization) and for
// browsing save files / autosaves.
void draw_json_explorer_window(Simulation& sim, UIState& ui);

} // namespace nebula4x::ui
