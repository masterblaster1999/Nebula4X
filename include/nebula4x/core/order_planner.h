#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/orders.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x {

class Simulation;

// Options controlling how the planner estimates ETAs and fuel usage.
//
// Notes:
// - This is a best-effort "mission planner" intended for UI previews.
// - It does not mutate the simulation state.
// - It intentionally trades perfect fidelity for speed and robustness.
struct OrderPlannerOptions {
  // If true, uses the body's Keplerian orbit parameters to predict future body
  // positions when estimating MoveToBody/Orbit/Colony-target travel.
  //
  // If false, uses the body's cached position (Body::position_mkm) for all
  // calculations, matching the simulation's per-tick "chase the moving body"
  // behaviour more closely but without predicting future motion.
  bool predict_orbits{true};

  // If true, models instantaneous refueling when the ship is within docking
  // range of a mutually-friendly colony with Fuel available.
  //
  // This is approximate, but provides useful "can this route complete?" fuel
  // forecasts for logistics planning.
  bool simulate_refuel{true};

  // Maximum number of orders to simulate (safety guard for repeat loops).
  int max_orders{512};
};

struct PlannedOrderStep {
  // Cumulative ETA at the end of this step (days).
  double eta_days{0.0};

  // Time spent in this order only (days).
  double delta_days{0.0};

  // Fuel before and after this step (tons).
  double fuel_before_tons{0.0};
  double fuel_after_tons{0.0};

  // Simulated ship state after completing this step.
  Id system_id{kInvalidId};
  Vec2 position_mkm{0.0, 0.0};

  // False if the plan determined this step cannot be executed (e.g., no fuel,
  // missing target, no engines). When false, planning stops at this step.
  bool feasible{true};

  // Human-readable note (warnings, refuel info, truncation reason, etc).
  std::string note;
};

struct OrderPlan {
  // True if a plan could be produced (even if truncated).
  bool ok{false};

  // True if planning stopped early due to an unsupported or indefinite order
  // (combat, infinite orbit, invalid target, etc).
  bool truncated{false};

  // When truncated, a short summary of why.
  std::string truncated_reason;

  double start_fuel_tons{0.0};
  double end_fuel_tons{0.0};

  // Total ETA for all simulated steps (days).
  double total_eta_days{0.0};

  std::vector<PlannedOrderStep> steps;
};

// Compute a best-effort plan for the ship's current queued orders.
//
// The returned plan's `steps` aligns 1:1 with the ship's current order queue
// until planning truncates.
//
// If the ship has no queued orders, returns ok=true with steps empty.
OrderPlan compute_order_plan(const Simulation& sim, Id ship_id, const OrderPlannerOptions& opts = {});

} // namespace nebula4x
