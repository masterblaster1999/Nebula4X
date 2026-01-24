#pragma once

#include <string>

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Compute the current "primary" navigation target from selection.
// Priority: Ship > Colony > Body > System.
NavTarget current_nav_target(const Simulation& sim, Id selected_ship, Id selected_colony, Id selected_body);

// Returns true if the target can be resolved in the currently-loaded GameState.
bool nav_target_exists(const Simulation& sim, const NavTarget& t);

// Human-readable label for a target. If include_kind_prefix=true, prefixes with
// "System:", "Ship:", etc.
std::string nav_target_label(const Simulation& sim, const NavTarget& t, bool include_kind_prefix = true);

// Apply a navigation target to selection/system focus.
// If open_windows is true, will open Map/Details and request relevant tabs.
void apply_nav_target(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body,
                      const NavTarget& t, bool open_windows);

// --- History ---

void nav_history_reset(UIState& ui);

// Push a target into history (dedupes, truncates forward history, applies max cap).
// If ui.nav_history_suppress_push is set, this will clear the flag and do nothing.
void nav_history_push(UIState& ui, const NavTarget& t);

bool nav_history_can_back(const UIState& ui);
bool nav_history_can_forward(const UIState& ui);

// Navigate backward/forward in history.
// Returns true if navigation occurred.
bool nav_history_back(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body,
                      bool open_windows);
bool nav_history_forward(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body,
                         bool open_windows);

// --- Bookmarks ---

bool nav_is_bookmarked(const UIState& ui, const NavTarget& t);

// Toggle a bookmark for the current selection. Returns true if the target is
// bookmarked after the toggle.
bool nav_bookmark_toggle_current(const Simulation& sim, UIState& ui, Id selected_ship, Id selected_colony,
                                 Id selected_body);

// Remove bookmarks that no longer resolve in the current GameState.
// Returns number removed.
int nav_bookmarks_prune_missing(const Simulation& sim, UIState& ui);

} // namespace nebula4x::ui
