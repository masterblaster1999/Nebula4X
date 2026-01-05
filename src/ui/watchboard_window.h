#pragma once

#include <string>

#include "nebula4x/core/simulation.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

void draw_watchboard_window(Simulation& sim, UIState& ui);

// Adds a JSON watch item ("pin") to the watchboard configuration.
// Returns true if the item was added (false if ignored/duplicate).
bool add_watch_item(UIState& ui, const std::string& path, const std::string& label = "",
                    bool track_history = true, bool show_sparkline = true, int history_len = 120);

// Adds a watch item in query/aggregate mode (wildcards * and **).
// query_op:
//   0=count matches, 1=sum, 2=avg, 3=min, 4=max
bool add_watch_query_item(UIState& ui, const std::string& pattern, int query_op,
                          const std::string& label = "", bool track_history = true,
                          bool show_sparkline = true, int history_len = 120);

} // namespace nebula4x::ui
