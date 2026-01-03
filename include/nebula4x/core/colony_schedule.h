#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/ids.h"

namespace nebula4x {

class Simulation;

// Best-effort, read-only production forecast for a single colony.
//
// This is primarily a UI helper that simulates a simplified day-by-day economy
// loop for one colony and estimates when shipyard / construction work will
// complete under current assumptions.
//
// What this forecast *does* model (day-level):
// - Mining extraction vs finite body deposits (shared among colonies on a body).
// - Non-mining industry recipes (consumes_per_day -> produces_per_day).
// - Shipyard build progress consuming minerals per ton.
// - Construction queue progress consuming minerals + construction points.
// - New installations come online the next day (matching tick order).
// - Colony installation_targets auto-queueing (optional).
//
// What this forecast intentionally does *not* attempt to model:
// - Auto-shipyard queueing from faction ship design targets (global balancing).
// - Freight/import/export, reserves/targets, trade.
// - Population growth/decline, terraforming, habitability.
// - AI actions, diplomacy, combat interruptions.
// - Ship movement (refit orders require the ship to already be docked).
struct ColonyScheduleOptions {
  // Maximum simulated days (safety guard).
  int max_days{3650};

  // Maximum number of completion events to return (safety guard).
  int max_events{512};

  // If true, simulates Colony::installation_targets auto-queueing inside the
  // construction step (matching Simulation::tick_construction).
  bool include_auto_construction_targets{true};

  // If true, includes shipyard queue simulation.
  bool include_shipyard{true};

  // If true, includes construction queue simulation.
  bool include_construction{true};
};

enum class ColonyScheduleEventKind {
  Note,
  ShipyardComplete,
  ConstructionComplete,
};

struct ColonyScheduleEvent {
  ColonyScheduleEventKind kind{ColonyScheduleEventKind::Note};

  // Day offset from the forecast start "now".
  // Day 1 means "completes by end of next sim tick".
  int day{0};

  // Short label (e.g. "Shipyard", "Construction").
  std::string title;

  // Human readable details.
  std::string detail;

  // True if this completion came from an auto-queued order.
  bool auto_queued{false};
};

struct ColonySchedule {
  // True if the schedule could be computed (even if stalled/truncated).
  bool ok{false};

  bool stalled{false};
  std::string stall_reason;

  bool truncated{false};
  std::string truncated_reason;

  Id colony_id{kInvalidId};
  Id faction_id{kInvalidId};

  // Snapshot of current-state rates used by the forecast.
  double construction_cp_per_day_start{0.0};
  double shipyard_tons_per_day_start{0.0};

  // Snapshot of current faction multipliers.
  double mining_multiplier{1.0};
  double industry_multiplier{1.0};
  double construction_multiplier{1.0};
  double shipyard_multiplier{1.0};

  // Mineral stockpile snapshot.
  std::unordered_map<std::string, double> minerals_start;
  std::unordered_map<std::string, double> minerals_end;

  std::vector<ColonyScheduleEvent> events;
};

// Estimate future completion times for the colony's shipyard and construction
// queues under current local production assumptions.
//
// The estimator mirrors Simulation's daily tick ordering:
//   tick_colonies (mining/industry) -> tick_shipyards -> tick_construction
//
// It does not mutate simulation state.
ColonySchedule estimate_colony_schedule(const Simulation& sim, Id colony_id,
                                        const ColonyScheduleOptions& opt = {});

} // namespace nebula4x
