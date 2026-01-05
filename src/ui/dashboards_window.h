#pragma once

#include <cstdint>
#include <string>

#include "nebula4x/core/simulation.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

void draw_dashboards_window(Simulation& sim, UIState& ui);

bool add_json_dashboard_for_table_view(UIState& ui, std::uint64_t table_view_id,
                                      const std::string& suggested_name = "");

bool add_json_dashboard_for_path(UIState& ui, const std::string& array_path,
                                 const std::string& suggested_name = "");

} // namespace nebula4x::ui
