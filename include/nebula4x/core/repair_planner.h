#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

class Simulation;

// A recommended repair routing + queue forecast for a single ship.
//
// All *_days values are best-effort estimates from "now".
struct RepairAssignment {
  Id ship_id{kInvalidId};

  // Recommended repair destination. If kInvalidId, no suitable shipyard was found.
  Id target_colony_id{kInvalidId};

  // When true, any travel orders issued by apply_* helpers should only traverse
  // systems discovered by the ship's faction.
  bool restrict_to_discovered{true};

  // Snapshot of ship repair priority when the plan was computed.
  RepairPriority priority{RepairPriority::Normal};

  // Travel-only ETA to reach the target colony body (best-effort).
  double travel_eta_days{0.0};

  // Start/finish of repairs at the chosen shipyard, relative to now.
  // start_repair_days will typically be >= travel_eta_days.
  double start_repair_days{0.0};
  double finish_repair_days{0.0};

  double queue_wait_days{0.0};
  double repair_days{0.0};

  // Work estimate.
  double missing_hull_hp{0.0};
  double missing_subsystem_hp_equiv{0.0};
  double total_missing_hp_equiv{0.0};

  // Optional high-level note/warning (e.g., unreachable).
  std::string note;
};

// Summary of a repair-capable colony (shipyards) and the plan's assigned workload.
struct RepairYardPlan {
  Id colony_id{kInvalidId};
  Id body_id{kInvalidId};
  Id system_id{kInvalidId};

  int shipyards{0};

  // Nominal and effective repair capacity (hp-equivalent per day).
  double nominal_capacity_hp_per_day{0.0};
  double effective_capacity_hp_per_day{0.0};

  // Multipliers contributing to effective capacity.
  double blockade_multiplier{1.0};
  double mineral_limit_multiplier{1.0};

  int assigned_ship_count{0};
  double backlog_hp_equiv{0.0};

  // Processing-only time = backlog / effective_capacity (ignores travel/release times).
  double processing_days{0.0};

  // Makespan includes travel/release times and any idle gaps while waiting for ships to arrive.
  double makespan_days{0.0};

  // Ratio in [0..1] describing how busy the yard is over makespan_days.
  double utilization{0.0};

  std::string note;
};

struct RepairPlannerOptions {
  // If true, jump routing will only traverse systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // If true, consider shipyards owned by trade partners (not just the faction itself).
  bool include_trade_partner_yards{true};

  // If true, include subsystem integrity repairs in the work estimate.
  bool include_subsystem_repairs{true};

  // If true, scale effective capacity using blockade pressure (when enabled in config).
  bool include_blockade_multiplier{true};

  // If true, cap effective capacity using current duranium/neutronium availability.
  bool apply_mineral_limits{false};

  // Optional filters.
  bool require_idle_ships{false};
  bool exclude_fleet_ships{false};

  // Safety caps for large games.
  int max_ships{2048};
  int max_yards{512};
  int max_candidates_per_ship{12};
};

struct RepairPlannerResult {
  bool ok{false};
  bool truncated{false};
  std::string message;

  std::vector<RepairYardPlan> yards;
  std::vector<RepairAssignment> assignments;
};

// Compute a best-effort repair routing plan for the given faction.
RepairPlannerResult compute_repair_plan(const Simulation& sim, Id faction_id, const RepairPlannerOptions& opt = {});

// Enqueue travel/orbit orders to send a single ship to its target shipyard.
// Returns false if the assignment is invalid or any order could not be issued.
bool apply_repair_assignment(Simulation& sim, const RepairAssignment& asg, bool clear_existing_orders = true,
                             bool use_smart_travel = true);

// Apply an entire plan (all assignments with valid target_colony_id).
bool apply_repair_plan(Simulation& sim, const RepairPlannerResult& plan, bool clear_existing_orders = true,
                       bool use_smart_travel = true);

}  // namespace nebula4x
