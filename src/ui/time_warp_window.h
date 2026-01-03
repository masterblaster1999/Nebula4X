#pragma once

#include <cstdint>
#include <string>

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A UI surface for running deterministic "time warp" in the simulation until an
// event matching a user-defined filter occurs.
//
// This is a convenience wrapper around Simulation::advance_until_event_hours.
// It intentionally advances in small chunks per frame to keep the UI responsive.
//
// Not persisted in saves.
void draw_time_warp_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body);

// Quick-start request for programmatic time warps (used by the Planner, hotkeys,
// and other UI surfaces).
//
// Notes:
// - The warp will stop early if a newly recorded SimEvent matches `stop`.
// - `stop` filters apply to *all* event levels (info/warn/error).
// - If stop_at_time_limit_is_success is true, reaching the time budget without
//   a stop event is treated as a success (useful for "warp to date/time").
struct TimeWarpQuickStart {
  // Total time budget in hours.
  int total_hours{0};

  // Simulation step granularity (typically 1/6/12/24).
  int step_hours{24};

  // How many hours of budget to spend per frame while running.
  int chunk_hours_per_frame{24};

  // Stop condition passed to Simulation::advance_until_event_hours.
  EventStopCondition stop;

  // If true, reaching the time limit without a matching stop event is considered
  // a successful completion.
  bool stop_at_time_limit_is_success{false};

  // UI affordances on hit.
  bool open_timeline_on_hit{true};
  bool focus_context_on_hit{true};

  // Optional: label + absolute target time to show while running.
  std::string target_label;
  std::int64_t target_day{0};
  int target_hour{0};
  bool has_target_time{false};
};

// Configure and immediately start a time warp job (opens the Time Warp window).
//
// This is UI-only; it does not persist in saves.
void time_warp_quick_start(const TimeWarpQuickStart& req, UIState& ui);

} // namespace nebula4x::ui
