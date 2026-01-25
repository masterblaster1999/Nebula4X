#pragma once

#include <cstddef>

namespace nebula4x::ui {

struct UIState;

// Built-in window-visibility presets intended to quickly declutter the UI.
//
// These are UI-only (persisted in ui_prefs.json indirectly via each window's open flags).
// They are safe to apply at any time.
struct WorkspacePresetInfo {
  const char* name;
  const char* desc;
};

// Returns the static list of built-in presets.
const WorkspacePresetInfo* workspace_preset_infos(std::size_t* count);

// Apply one of the built-in presets by name (e.g. "Default", "Economy").
// Unknown names are ignored.
void apply_workspace_preset(const char* preset_name, UIState& ui);

}  // namespace nebula4x::ui
