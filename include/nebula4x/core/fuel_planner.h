#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

class Simulation;

// A single tanker -> target refuel leg.
//
// The planner is best-effort: eta values are travel-only estimates based on jump
// route planning and ignore any time spent rendezvousing/transfering fuel.
struct FuelTransferLeg {
  Id target_ship_id{kInvalidId};
  double tons{0.0};

  // Best-effort travel ETA from the previous leg start position to this target.
  double eta_days{0.0};

  // Target fuel fraction (0..1) before/after the transfer, computed from the
  // target ship's current fuel_tons and design fuel capacity.
  double target_fuel_frac_before{0.0};
  double target_fuel_frac_after{0.0};
};

// A route for a single tanker, potentially consisting of multiple transfers.
struct FuelAssignment {
  Id tanker_ship_id{kInvalidId};

  // When true, any jump routing performed by apply_* helpers should only traverse
  // systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // Snapshot of tanker capacity/fuel used by the planner.
  double tanker_fuel_capacity_tons{0.0};
  double tanker_fuel_before_tons{0.0};
  double tanker_fuel_reserved_tons{0.0};
  double tanker_fuel_available_tons{0.0};

  std::vector<FuelTransferLeg> legs;

  // Best-effort travel-only ETA for the whole route.
  double eta_total_days{0.0};

  // Total planned transfer tonnage (sum of legs[].tons).
  double fuel_transfer_total_tons{0.0};

  // Optional high-level note.
  std::string note;
};

struct FuelPlannerOptions {
  // If true, only consider ships with Ship::auto_tanker enabled as tankers.
  bool require_auto_tanker_flag{true};

  // If true, only consider ships that are currently idle (no queued orders, or repeat completed).
  bool require_idle{true};

  // If true, jump routing will only traverse systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // If true, avoid assigning fleet members (tankers and targets) to prevent
  // fighting fleet-level movement logic.
  bool exclude_fleet_ships{true};

  // If true, do not service ships that already have colony auto-refuel enabled.
  // (Those ships are expected to route to a colony instead of waiting for a tanker.)
  bool exclude_ships_with_auto_refuel{true};

  // Safety caps.
  int max_targets{4096};
  int max_tankers{256};
  int max_legs_per_tanker{4};
};

struct FuelPlannerResult {
  bool ok{false};
  bool truncated{false};
  std::string message;

  std::vector<FuelAssignment> assignments;
};

// Compute a best-effort fuel transfer plan for a faction.
//
// This is designed to mirror (at a high level) the simulation's auto-tanker logic,
// but without mutating game state, and with optional multi-stop routing.
FuelPlannerResult compute_fuel_plan(const Simulation& sim, Id faction_id, const FuelPlannerOptions& opt = {});

// Apply a single assignment by enqueueing travel/transfer orders.
// Returns false if any order could not be issued.
bool apply_fuel_assignment(Simulation& sim, const FuelAssignment& asg, bool clear_existing_orders = true);

// Apply an entire plan.
bool apply_fuel_plan(Simulation& sim, const FuelPlannerResult& plan, bool clear_existing_orders = true);

}  // namespace nebula4x
