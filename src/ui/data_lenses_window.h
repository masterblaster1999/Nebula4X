#pragma once

#include <string>

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Adds a new Data Lens (table view) config.
// Returns true if a new lens was added.
bool add_json_table_view(UIState& ui, const std::string& array_path, const std::string& suggested_name = "");

// Draw the Data Lenses window.
void draw_data_lenses_window(Simulation& sim, UIState& ui);

} // namespace nebula4x::ui
