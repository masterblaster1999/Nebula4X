#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

class Simulation;

enum class ColonistAssignmentKind {
  // Ship already has embarked colonists; deliver them to a destination colony.
  DeliverColonists,

  // Ship is empty (or below min transfer); load colonists at a source colony and deliver.
  PickupAndDeliver,
};

struct ColonistAssignment {
  ColonistAssignmentKind kind{ColonistAssignmentKind::PickupAndDeliver};

  Id ship_id{kInvalidId};
  Id source_colony_id{kInvalidId};
  Id dest_colony_id{kInvalidId};

  // When true, any jump routing performed by apply_* helpers should only traverse
  // systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // Colonists to move (in millions).
  double millions{0.0};

  // Best-effort travel-only ETAs. These ignore time spent rendezvousing/loading/unloading.
  double eta_to_source_days{0.0};
  double eta_to_dest_days{0.0};
  double eta_total_days{0.0};

  // Optional human-readable reason/note (for UI).
  std::string reason;
  std::string note;
};

struct ColonistPlannerOptions {
  // If true, only consider ships with Ship::auto_colonist_transport enabled.
  bool require_auto_colonist_transport_flag{true};

  // If true, only consider ships that are currently idle (no queued orders, or repeat completed).
  bool require_idle{true};

  // If true, jump routing will only traverse systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // If true, avoid assigning fleet members to prevent fighting fleet-level movement logic.
  bool exclude_fleet_ships{true};

  // Safety cap on candidate ships considered.
  int max_ships{256};
};

struct ColonistPlannerResult {
  bool ok{false};
  bool truncated{false};
  std::string message;

  std::vector<ColonistAssignment> assignments;
};

// Compute a best-effort colonist transport plan for a faction.
//
// Colonist transport is driven by per-colony knobs:
// - Colony::population_target_millions: colonies below this target are eligible destinations.
// - Colony::population_reserve_millions: colonies will not export population below this floor.
//
// The planner is deterministic (tie-breaks by id) so it can be used for UI previews.
ColonistPlannerResult compute_colonist_plan(const Simulation& sim, Id faction_id, const ColonistPlannerOptions& opt = {});

// Apply a single assignment by enqueueing travel/load/unload orders.
// Returns false if any order could not be issued.
bool apply_colonist_assignment(Simulation& sim, const ColonistAssignment& asg, bool clear_existing_orders = true);

// Apply an entire plan.
bool apply_colonist_plan(Simulation& sim, const ColonistPlannerResult& plan, bool clear_existing_orders = true);

}  // namespace nebula4x
