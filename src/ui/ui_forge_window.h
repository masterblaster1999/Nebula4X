#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// UI Forge: a small in-game UI composer that lets players define custom panels
// (KPI cards, notes, previews) driven by live game-state JSON.
//
// Panels are persisted in ui_prefs.json and can be docked like other windows.

// Draw the UI Forge editor window.
void draw_ui_forge_window(Simulation& sim, UIState& ui, Id selected_ship, Id selected_colony, Id selected_body);

// Draw all user-defined panel windows that are currently open.
void draw_ui_forge_panel_windows(Simulation& sim, UIState& ui);

// One-shot base-game integration helper.
//
// If no custom panels exist yet, this seeds a starter curated panel so
// procedural UI elements are available in normal gameplay immediately.
// Returns true once integration is complete for this session (either already
// present or successfully created).
bool ensure_ui_forge_base_panels(Simulation& sim, UIState& ui);

} // namespace nebula4x::ui
