#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/ids.h"

namespace nebula4x {

class Simulation;

// A best-effort, read-only forecast item intended for planning UIs.
//
// These events are *not* persisted and are not part of the SimEvent log.
// They are derived from current in-memory state using helper estimators like:
//   - research_schedule
//   - colony_schedule
//   - order_planner
//
// The goal is to provide a single "upcoming" list that players can sort and
// filter while planning.
struct PlannerEvent {
  // Time until the event, in days from "now" (where 1.0 = 24 hours).
  //
  // - For day-level economy forecasts, this will typically be an integer.
  // - For ship order planning under sub-day ticks, this can be fractional.
  double eta_days{0.0};

  // Absolute timestamp (days since epoch + hour-of-day).
  //
  // These are derived from the simulation's current (date, hour_of_day)
  // combined with eta_days.
  std::int64_t day{0};
  int hour{0};

  EventLevel level{EventLevel::Info};
  EventCategory category{EventCategory::General};

  // Optional context for UI navigation.
  Id faction_id{kInvalidId};
  Id system_id{kInvalidId};
  Id ship_id{kInvalidId};
  Id colony_id{kInvalidId};

  // Short user-facing summary.
  std::string title;

  // Longer details (optional).
  std::string detail;
};

// Options controlling which sources are included and how aggressively the
// forecast is bounded.
struct PlannerEventsOptions {
  // Global horizon in days.
  // Items beyond this horizon are ignored.
  int max_days{3650};

  // Global maximum number of items returned.
  int max_items{512};

  bool include_research{true};
  bool include_colonies{true};
  bool include_ground_battles{true};
  // Include in-flight missile salvos as predicted combat events (impact ETA).
  bool include_missile_impacts{true};
  bool include_ships{false};

  // Ship order extraction options (when include_ships=true).
  bool include_ship_next_step{true};
  bool include_ship_queue_complete{true};

  // Safety guard: maximum number of ships to inspect per call.
  int max_ships{256};

  // Safety guard passed through to order_planner.
  int max_orders_per_ship{256};
};

struct PlannerEventsResult {
  bool ok{false};

  bool truncated{false};
  std::string truncated_reason;

  std::vector<PlannerEvent> items;
};

// Compute a merged, chronologically sorted list of upcoming events.
//
// This is a pure helper:
// - does not mutate simulation state
// - best-effort and may omit items in large games
PlannerEventsResult compute_planner_events(const Simulation& sim, Id faction_id,
                                           const PlannerEventsOptions& opt = {});

} // namespace nebula4x
