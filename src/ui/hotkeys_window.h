#pragma once

#include "ui/ui_state.h"

namespace nebula4x::ui {

struct UIPrefActions;

// Draw the "Hotkeys" tab inside the Settings window.
// This is UI-only; changes are persisted via ui_prefs.json.
void draw_hotkeys_settings_tab(UIState& ui, UIPrefActions& actions);

}  // namespace nebula4x::ui
