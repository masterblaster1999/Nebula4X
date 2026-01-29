#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

class Simulation;

// A recommended maintenance routing + recovery forecast for a single ship.
//
// This planner targets the ship_maintenance system (maintenance_condition) and
// its associated colony/ship cargo resource consumption (cfg.ship_maintenance_resource_id).
struct MaintenanceAssignment {
  Id ship_id{kInvalidId};

  // Recommended maintenance destination. If kInvalidId, no suitable colony was found.
  Id target_colony_id{kInvalidId};

  // Whether travel orders issued by apply_* helpers should only traverse systems
  // discovered by the ship's faction.
  bool restrict_to_discovered{true};

  // Travel-only ETA to reach the target colony body (best-effort).
  double travel_eta_days{0.0};

  // Start/finish of maintenance recovery relative to now.
  // start_days will typically be >= travel_eta_days.
  double start_days{0.0};
  double finish_days{0.0};

  // Processing time at the destination (recovery time).
  double maintenance_days{0.0};

  // Supplies estimate.
  double supplies_per_day_tons{0.0};
  double supplies_needed_total_tons{0.0};
  double supplies_from_ship_cargo_tons{0.0};
  double supplies_from_colony_tons{0.0};

  // Condition snapshot.
  double start_condition{1.0};
  double target_condition{1.0};

  // Readiness risk (breakdowns) based on current maintenance_condition.
  // These mirror the ship_maintenance failure model in Simulation::tick_ship_maintenance_failures.
  double breakdown_rate_per_day{0.0};      // lambda
  double breakdown_p_per_day{0.0};         // 1 - exp(-lambda)
  double breakdown_p_during_travel{0.0};   // 1 - exp(-lambda * travel_eta_days)

  // Target colony metadata (snapshotted for UI convenience).
  bool target_has_shipyard{false};
  bool target_owned_by_faction{false};

  // Optional high-level note/warning (e.g., unreachable, no supplies).
  std::string note;
};

// Summary of a maintenance-capable colony and the plan's assigned supply draw.
struct MaintenanceColonyPlan {
  Id colony_id{kInvalidId};
  Id body_id{kInvalidId};
  Id system_id{kInvalidId};

  bool owned_by_faction{false};
  bool has_shipyard{false};

  double available_supplies_tons{0.0};
  double reserved_supplies_tons{0.0};
  double remaining_supplies_tons{0.0};

  int assigned_ship_count{0};

  std::string note;
};

struct MaintenancePlannerOptions {
  // If true, jump routing will only traverse systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // If true, consider colonies owned by trade partners (not just the faction itself).
  bool include_trade_partner_colonies{true};

  // When true, prefer colonies with shipyards (they suppress maintenance failures while docked).
  bool prefer_shipyards{true};

  // When true, ships below cfg.ship_maintenance_breakdown_start_fraction will only be
  // assigned to colonies with shipyards if any such option exists.
  bool require_shipyard_when_critical{true};

  // When true, only assign a ship to a colony when that colony's current stockpile can
  // cover the estimated supplies_from_colony_tons (after reserve_buffer_fraction).
  bool require_supplies_available{true};

  // Only plan ships with maintenance_condition < threshold_fraction.
  double threshold_fraction{0.75};

  // Target maintenance_condition after recovery.
  double target_fraction{0.95};

  // Reserve some fraction of each colony's stockpile for local/unmodeled usage.
  double reserve_buffer_fraction{0.10};

  // Optional filters.
  bool require_idle_ships{false};
  bool exclude_fleet_ships{false};

  // Safety caps for large games.
  int max_ships{2048};
  int max_colonies{2048};
  int max_candidates_per_ship{12};
};

struct MaintenancePlannerResult {
  bool ok{false};
  bool truncated{false};
  std::string message;

  // Convenience: cfg.ship_maintenance_resource_id at planning time.
  std::string resource_id;

  std::vector<MaintenanceColonyPlan> colonies;
  std::vector<MaintenanceAssignment> assignments;
};

// Compute a best-effort maintenance routing plan for the given faction.
MaintenancePlannerResult compute_maintenance_plan(const Simulation& sim, Id faction_id,
                                                 const MaintenancePlannerOptions& opt = {});

// Enqueue travel/orbit orders to send a single ship to its maintenance destination.
// Returns false if the assignment is invalid or any order could not be issued.
bool apply_maintenance_assignment(Simulation& sim, const MaintenanceAssignment& asg,
                                 bool clear_existing_orders = true, bool use_smart_travel = true);

// Apply an entire plan (all assignments with valid target_colony_id).
bool apply_maintenance_plan(Simulation& sim, const MaintenancePlannerResult& plan,
                            bool clear_existing_orders = true, bool use_smart_travel = true);

}  // namespace nebula4x
