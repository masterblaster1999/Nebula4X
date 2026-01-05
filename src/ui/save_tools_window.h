#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Save tooling utilities for debugging / experimentation.
//
// Features:
//  - Diff two save-game JSON documents (or a file vs the current in-memory state)
//  - Export a structured diff report (JSON), human-readable diff text, or an RFC 6902 JSON Patch
//  - Apply a JSON Patch to a JSON document and optionally load the result into the current simulation
//
// Not persisted in save-games.
void draw_save_tools_window(Simulation& sim, UIState& ui, const char* save_path, const char* load_path);

} // namespace nebula4x::ui
