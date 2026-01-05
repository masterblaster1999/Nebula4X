#pragma once

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Window for managing multiple dock layout profiles.
//
// A "layout profile" maps to a Dear ImGui ini file which stores docking state
// and window positions. Users can save, duplicate, switch and delete profiles
// at runtime.
void draw_layout_profiles_window(UIState& ui);

}  // namespace nebula4x::ui
