#pragma once

#include <cstdint>
#include <string>

#include "nebula4x/core/simulation.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

// Draw the Pivot Tables window.
void draw_pivot_tables_window(Simulation& sim, UIState& ui);

// Adds a new pivot config for an existing Data Lens (table view).
// Returns true if a new pivot was added.
bool add_json_pivot_for_table_view(UIState& ui, std::uint64_t table_view_id,
                                  const std::string& suggested_name = "");

// Adds a new pivot config for a JSON array path.
// Internally creates (or reuses) a Data Lens and then creates a pivot for it.
bool add_json_pivot_for_path(UIState& ui, const std::string& array_path,
                             const std::string& suggested_name = "");

} // namespace nebula4x::ui
