#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

class Simulation;

enum class TroopAssignmentKind {
  // Ship already has embarked troops; deliver them to a destination colony.
  DeliverTroops,

  // Ship is empty (or below min transfer); load troops at a source colony and deliver.
  PickupAndDeliver,
};

struct TroopAssignment {
  TroopAssignmentKind kind{TroopAssignmentKind::PickupAndDeliver};

  Id ship_id{kInvalidId};
  Id source_colony_id{kInvalidId};
  Id dest_colony_id{kInvalidId};

  // When true, any jump routing performed by apply_* helpers should only traverse
  // systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // Troop strength to move.
  double strength{0.0};

  // Best-effort travel-only ETAs. These ignore time spent rendezvousing/loading/unloading.
  double eta_to_source_days{0.0};
  double eta_to_dest_days{0.0};
  double eta_total_days{0.0};

  // Optional human-readable reason/note (for UI).
  std::string reason;
  std::string note;
};

struct TroopPlannerOptions {
  // If true, only consider ships with Ship::auto_troop_transport enabled.
  bool require_auto_troop_transport_flag{true};

  // If true, only consider ships that are currently idle (no queued orders, or repeat completed).
  bool require_idle{true};

  // If true, jump routing will only traverse systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // If true, avoid assigning fleet members to prevent fighting fleet-level movement logic.
  bool exclude_fleet_ships{true};

  // Safety cap on candidate ships considered.
  int max_ships{256};
};

struct TroopPlannerResult {
  bool ok{false};
  bool truncated{false};
  std::string message;

  std::vector<TroopAssignment> assignments;
};

// Compute a best-effort troop transport plan for a faction.
//
// This mirrors (at a high level) the simulation's auto-troop transport logic, but
// without mutating game state. The planner is deterministic (tie-breaks by id)
// so it can be used for UI previews.
TroopPlannerResult compute_troop_plan(const Simulation& sim, Id faction_id, const TroopPlannerOptions& opt = {});

// Apply a single assignment by enqueueing travel/load/unload orders.
// Returns false if any order could not be issued.
bool apply_troop_assignment(Simulation& sim, const TroopAssignment& asg, bool clear_existing_orders = true);

// Apply an entire plan.
bool apply_troop_plan(Simulation& sim, const TroopPlannerResult& plan, bool clear_existing_orders = true);

}  // namespace nebula4x
