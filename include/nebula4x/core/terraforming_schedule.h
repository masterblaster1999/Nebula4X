#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/ids.h"

namespace nebula4x {

class Simulation;

// Options controlling terraforming forecasts.
//
// Forecasts are best-effort and based on *current* state:
// - current body conditions (temp/atm)
// - current terraforming installations (points/day)
// - current mineral stockpiles (duranium/neutronium)
//
// By default, the forecast assumes mineral stockpiles are not replenished.
// This matches a "worst case" for mineral-limited terraforming, and is useful
// for detecting when a project will stall without shipments.
struct TerraformingScheduleOptions {
  // Maximum number of days to simulate.
  int max_days{200000};

  // If true, ignore duranium/neutronium costs and treat points/day as fully
  // available for the entire forecast.
  bool ignore_mineral_costs{false};
};

struct TerraformingColonyContribution {
  Id colony_id{kInvalidId};

  // Points/day contributed by this colony at the start of the forecast.
  double points_per_day{0.0};

  // Starting mineral stockpiles (tons).
  double duranium_available{0.0};
  double neutronium_available{0.0};
};

// Result of a terraforming forecast for a single body.
struct TerraformingSchedule {
  bool ok{false};

  bool has_target{false};
  bool complete{false};

  // True when points/day drop to ~0 while still not complete.
  bool stalled{false};
  std::string stall_reason;

  // True when the forecast exceeded max_days without completion.
  bool truncated{false};
  std::string truncated_reason;

  Id body_id{kInvalidId};
  Id system_id{kInvalidId};

  // Snapshot of start/end conditions used in the forecast.
  double start_temp_k{0.0};
  double start_atm{0.0};
  double start_o2_atm{0.0};

  double target_temp_k{0.0};
  double target_atm{0.0};
  double target_o2_atm{0.0};

  double end_temp_k{0.0};
  double end_atm{0.0};
  double end_o2_atm{0.0};

  // Total points/day available at the start of the forecast.
  double points_per_day{0.0};

  // Integrated points actually applied in the simulated horizon.
  double points_applied{0.0};

  // Simulated days (0 means no simulation was needed).
  int days_simulated{0};

  // If complete==true, completion offset in days from "now".
  int days_to_complete{0};

  // Mineral costs per point (from SimConfig).
  double duranium_per_point{0.0};
  double neutronium_per_point{0.0};

  // Aggregated starting mineral stockpiles on contributing colonies.
  double duranium_available{0.0};
  double neutronium_available{0.0};

  // Estimated minerals consumed over the simulated horizon.
  double duranium_consumed{0.0};
  double neutronium_consumed{0.0};

  // Per-colony contributions (start snapshot).
  std::vector<TerraformingColonyContribution> colonies;
};

// Estimate when terraforming on a given body will complete (if ever), based on
// current installations and mineral stockpiles.
//
// This is a pure helper (does not mutate simulation state).
TerraformingSchedule estimate_terraforming_schedule(const Simulation& sim, Id body_id,
                                                    const TerraformingScheduleOptions& opt = {});

} // namespace nebula4x
