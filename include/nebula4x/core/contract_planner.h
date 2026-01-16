#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

class Simulation;

// Contract planner
// ---------------
//
// The simulation supports lightweight faction-scoped contracts (mission board)
// that can be accepted/abandoned and assigned to ships.
//
// This module provides a deterministic, side-effect-free planner suitable for
// UI previews. It suggests simple one-ship-per-contract assignments that
// maximize a heuristic "value per day" score (reward adjusted for risk, divided
// by travel + work time).
//
// Intentional limitations (future work):
//  - Only assigns at most one contract to a ship.
//  - Does not coordinate fleet-level contract fulfillment.
//  - Does not consider fuel logistics or refueling stops.

struct ContractAssignment {
  Id contract_id{kInvalidId};
  Id ship_id{kInvalidId};

  // When true, apply_* helpers will only traverse discovered systems.
  bool restrict_to_discovered{true};

  // When true, apply_* helpers will clear a ship's existing orders before
  // enqueueing contract orders.
  bool clear_existing_orders{true};

  // Best-effort travel-only ETA (days) from the ship's current location to the
  // contract target position. (Does not include docking or combat.)
  double eta_days{0.0};

  // Best-effort estimate for the time spent "working" the contract once on
  // station (days). For example, anomaly investigation time.
  double work_days{0.0};

  // Planner score (higher is better). Primarily useful for debugging/UI.
  double score{0.0};

  // Optional UI/debug note.
  std::string note;
};

struct ContractPlannerOptions {
  // If true, only consider ships that are currently idle.
  bool require_idle{true};

  // If true, avoid assigning ships that belong to a fleet (to avoid conflicts
  // with fleet movement logic).
  bool exclude_fleet_ships{true};

  // If true, jump routing will only traverse systems discovered by the planning
  // faction.
  bool restrict_to_discovered{true};

  // If true, avoid planning contracts that are currently in systems with
  // detected hostile ships.
  bool avoid_hostile_systems{true};

  // Which contract statuses to include.
  bool include_offered{true};
  bool include_accepted_unassigned{true};
  bool include_already_assigned{false};

  // Apply helpers (UI convenience).
  bool clear_orders_before_apply{true};

  // Safety caps.
  int max_ships{256};
  int max_contracts{128};

  // Scoring knobs.
  // risk_penalty is applied as a multiplicative penalty: score *= (1 - risk * risk_penalty)
  // so risk_penalty=0 disables it.
  double risk_penalty{0.35};

  // Additional "overhead" days added per hop for scoring (not for ETA).
  double hop_overhead_days{0.25};
};

struct ContractPlannerResult {
  bool ok{false};
  bool truncated{false};
  std::string message;

  std::vector<ContractAssignment> assignments;
};

// Compute a best-effort contract assignment plan for a faction.
//
// The planner is deterministic (tie-breaks by id) so it can be used for UI
// previews.
ContractPlannerResult compute_contract_plan(const Simulation& sim, Id faction_id,
                                           const ContractPlannerOptions& opt = {});

// Apply a single contract assignment by accepting (if needed) and enqueueing
// the corresponding ship orders.
bool apply_contract_assignment(Simulation& sim, const ContractAssignment& asg,
                               bool push_event = true, std::string* error = nullptr);

// Apply an entire plan.
bool apply_contract_plan(Simulation& sim, const ContractPlannerResult& plan,
                         bool push_event = true, std::string* error = nullptr);

}  // namespace nebula4x
