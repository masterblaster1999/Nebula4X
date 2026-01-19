#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Procedural, context-aware UI Forge panel generator.
//
// Context Forge can generate a "living" UI Forge panel that follows the
// currently selected entity (ship/colony/body) or a pinned entity.
//
// Call once per frame (before draw_ui_forge_panel_windows) so the
// generated panel updates in the same frame selection changes.
void update_context_forge(Simulation& sim, UIState& ui, Id selected_ship, Id selected_colony, Id selected_body);

// Control window: toggles and generation controls.
void draw_context_forge_window(Simulation& sim, UIState& ui, Id selected_ship, Id selected_colony, Id selected_body);

}  // namespace nebula4x::ui
