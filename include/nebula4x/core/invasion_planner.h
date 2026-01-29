#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/ground_battle_forecast.h"

namespace nebula4x {

class Simulation;

// Options controlling invasion analysis.
struct InvasionPlannerOptions {
  // Attacking faction perspective for discovery checks and colony ownership.
  Id attacker_faction_id{kInvalidId};

  // If true, route/ETA queries and candidate staging colonies are limited to
  // systems discovered by attacker_faction_id.
  bool restrict_to_discovered{true};

  // Start state used when ranking staging colonies (typically a fleet leader).
  Id start_system_id{kInvalidId};
  Vec2 start_pos_mkm{0.0, 0.0};

  // Used for ETA estimates (km/s). If <=0, staging ETA values will be 0.
  double planning_speed_km_s{0.0};

  // How much of a colony's troop surplus is considered "available" for staging.
  // If <0, defaults to Simulation::cfg().auto_troop_max_take_fraction_of_surplus.
  double max_take_fraction_of_surplus{-1.0};

  // Maximum number of staging options returned (sorted by score).
  int max_staging_options{6};
};

// Candidate staging colony suggestion.
struct InvasionStagingOption {
  Id colony_id{kInvalidId};

  // Raw surplus strength above the colony's garrison target.
  double surplus_strength{0.0};

  // Capped amount of surplus considered safely available (surplus * take_fraction).
  double take_cap_strength{0.0};

  // ETA from the start position to the staging colony's body position.
  double eta_start_to_stage_days{0.0};

  // ETA from staging colony to target colony (body position).
  double eta_stage_to_target_days{0.0};

  // Total ETA = eta_start_to_stage_days + eta_stage_to_target_days.
  double eta_total_days{0.0};

  // Internal score used for ranking (higher is better).
  double score{0.0};
};

// Ground invasion analysis of a target colony.
struct InvasionTargetAnalysis {
  Id colony_id{kInvalidId};
  Id system_id{kInvalidId};
  Id defender_faction_id{kInvalidId};

  // Snapshot of defender strength (uses active battle state if present).
  double defender_strength{0.0};

  // Fortification points (total and effective after any in-progress fort damage).
  double forts_total{0.0};
  double forts_effective{0.0};
  double fort_damage_points{0.0};

  // Defender artillery weapon damage per day (installation weapons, scaled by fort integrity).
  double defender_artillery_weapon_damage_per_day{0.0};

  // Required attacker strength (best-effort) including the margin factor.
  double required_attacker_strength{0.0};

  // Forecast at required_attacker_strength.
  GroundBattleForecast forecast_at_required{};

  // Alternate scenario: assume forts and artillery are 0 (fully breached/suppressed).
  double required_attacker_strength_no_forts{0.0};
  GroundBattleForecast forecast_at_required_no_forts{};

  // Optional forecast for a user-provided attacker strength (e.g. current embarked troops).
  bool has_attacker_strength_forecast{false};
  double attacker_strength_test{0.0};
  GroundBattleForecast forecast_at_attacker_strength{};
};

struct InvasionPlannerResult {
  bool ok{false};
  std::string message;

  InvasionTargetAnalysis target{};
  std::vector<InvasionStagingOption> staging_options{};
};

// Computes invasion analysis for a target colony from the perspective of
// opt.attacker_faction_id.
//
// The returned analysis is best-effort. When attacker_strength_for_forecast >= 0,
// the result also includes a forecast for that specific attacker strength.
InvasionPlannerResult analyze_invasion_target(
    const Simulation& sim,
    Id target_colony_id,
    const InvasionPlannerOptions& opt,
    double troop_margin_factor,
    double attacker_strength_for_forecast = -1.0);

}  // namespace nebula4x
