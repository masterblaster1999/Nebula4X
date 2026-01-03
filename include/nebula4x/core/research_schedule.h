#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/ids.h"

namespace nebula4x {

class Simulation;

// Options controlling how the research forecast is computed.
//
// Notes:
// - This is a UI/CLI convenience helper.
// - It does not mutate simulation state.
// - The forecast is best-effort and assumes colony installations do not change
//   during the horizon (shipyards/industry can still change RP/day in reality).
struct ResearchScheduleOptions {
  // Maximum simulated days (safety guard).
  int max_days{36500}; // 100 years

  // Maximum number of completion items to return (safety guard).
  int max_items{256};
};

struct ResearchScheduleItem {
  std::string tech_id;

  // Day offset from "now".
  // - start_day can be 0 for an already-active project.
  // - end_day is the day the project is expected to complete (>= start_day).
  int start_day{0};
  int end_day{0};

  // Tech cost and progress.
  double cost{0.0};
  double progress_at_start{0.0};

  // True if this item was the faction's active project at forecast start.
  bool was_active_at_start{false};
};

struct ResearchSchedule {
  // True if the schedule could be computed (even if truncated).
  bool ok{false};

  // If true, the forecast stopped early due to an inability to make progress
  // (e.g., queue blocked by missing prerequisites, no RP income).
  bool stalled{false};
  std::string stall_reason;

  // If true, the forecast stopped early due to max_days/max_items.
  bool truncated{false};
  std::string truncated_reason;

  // Current-state snapshot used by the forecast.
  double rp_bank_start{0.0};
  double base_rp_per_day{0.0};
  double research_multiplier{1.0};
  double effective_rp_per_day{0.0};

  std::vector<ResearchScheduleItem> items;
};

// Estimate when the faction's current active research + queued projects will
// complete.
//
// The estimator mirrors Simulation::tick_research at the day level:
// - RP is generated once per day from current colony installations.
// - Economy multipliers from newly completed techs affect RP generation starting
//   the *next* day (matching the simulation).
// - The research queue is scanned in order; the first project whose prereqs are
//   met becomes active.
// - Multiple techs may complete in the same day if enough RP is banked.
ResearchSchedule estimate_research_schedule(const Simulation& sim, Id faction_id,
                                           const ResearchScheduleOptions& opt = {});

} // namespace nebula4x
