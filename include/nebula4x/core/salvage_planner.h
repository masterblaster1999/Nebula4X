#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

class Simulation;

// Salvage planner
// --------------
//
// The game already supports wreck salvage via ship orders (SalvageWreck) and
// helper methods on Simulation (issue_salvage_wreck / issue_unload_mineral).
//
// This module provides a deterministic, side-effect-free planner suitable for
// UI previews. It suggests simple salvage "runs":
//   - if a ship already has cargo, deliver it to a friendly colony
//   - otherwise, salvage a wreck (until cargo is full / wreck exhausted) then
//     deliver the minerals to a friendly colony
//
// Notes / intentional limitations (for future rounds):
//   - The initial planner generates at most one wreck assignment per ship.
//   - It does not chain multiple wrecks into a single run.
//   - It does not attempt to split a single large wreck across multiple ships.

enum class SalvageAssignmentKind {
  // Ship already has cargo; deliver it to a destination colony.
  DeliverCargo,

  // Ship is (mostly) empty; salvage a wreck, then deliver.
  SalvageAndDeliver,
};

struct SalvageAssignment {
  SalvageAssignmentKind kind{SalvageAssignmentKind::SalvageAndDeliver};

  Id ship_id{kInvalidId};
  Id wreck_id{kInvalidId};
  Id dest_colony_id{kInvalidId};

  // When true, any jump routing performed by apply_* helpers should only traverse
  // systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // Planner/UI hint: avoid assigning wrecks in systems with currently detected hostiles.
  bool avoid_hostile_systems{true};

  // Planned salvage parameters.
  // - mineral == "" means "all minerals"
  // - tons <= 0 means "as much as possible"
  std::string mineral;
  double tons{0.0};

  // Best-effort estimates (travel-only ETAs via jump routing).
  // These ignore docking/load/unload overheads.
  double eta_to_wreck_days{0.0};
  double eta_to_dest_days{0.0};
  double eta_total_days{0.0};

  // Rough estimate for how many days are spent salvaging at the wreck.
  double est_salvage_days{0.0};

  // Best-effort expected tons to be loaded from the wreck on this run
  // (typically limited by ship free cargo capacity).
  double expected_salvage_tons{0.0};

  // Total salvageable tons present in the wreck at planning time.
  double wreck_total_tons{0.0};

  // Optional UI note.
  std::string note;
};

struct SalvagePlannerOptions {
  // If true, only consider ships that are currently idle (no queued orders, or repeat completed).
  bool require_idle{true};

  // If true, jump routing will only traverse systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // If true, avoid assigning ships that belong to a fleet (to prevent fighting fleet-level movement logic).
  bool exclude_fleet_ships{true};

  // If true, filter wrecks in systems with detected hostiles.
  bool avoid_hostile_systems{true};

  // Minimum tonnage threshold for considering ships/wrecks.
  // If <= 0, the planner uses SimConfig::auto_freight_min_transfer_tons as a reasonable default.
  double min_tons{0.0};

  // Safety caps.
  int max_ships{256};
  int max_wrecks{256};
};

struct SalvagePlannerResult {
  bool ok{false};
  bool truncated{false};
  std::string message;

  std::vector<SalvageAssignment> assignments;
};

// Compute a best-effort salvage plan for a faction.
//
// The planner is deterministic (tie-breaks by id) so it can be used for UI previews.
SalvagePlannerResult compute_salvage_plan(const Simulation& sim, Id faction_id, const SalvagePlannerOptions& opt = {});

// Apply a single assignment by enqueueing salvage/unload orders.
// Returns false if any order could not be issued.
bool apply_salvage_assignment(Simulation& sim, const SalvageAssignment& asg, bool clear_existing_orders = true);

// Apply an entire plan.
bool apply_salvage_plan(Simulation& sim, const SalvagePlannerResult& plan, bool clear_existing_orders = true);

}  // namespace nebula4x
