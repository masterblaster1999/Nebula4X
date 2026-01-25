#pragma once

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Guided Tours (onboarding overlay).
//
// A tour is a curated sequence of steps that can spotlight existing windows and
// teach the current UI workflows. Tours are UI-only and are not persisted in
// save-games.

// Called early in the frame (before windows are drawn) so the tour can open
// required windows for the current step.
void guided_tour_preframe(UIState& ui);

// Draw the spotlight overlay + step card.
// Called at the end of the frame (after windows are drawn).
void draw_guided_tour_overlay(UIState& ui);

// Render the "Tours" tab inside Help / Codex.
void draw_help_tours_tab(UIState& ui);

}  // namespace nebula4x::ui
