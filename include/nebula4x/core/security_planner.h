#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/trade_network.h"

namespace nebula4x {

class Simulation;

// High-level, UI-facing analysis of where a faction is economically exposed to
// piracy and disruption in the procedural trade network.
//
// This module intentionally reuses the same inputs as the simulation's
// AI trade-security patrol logic (trade lanes + piracy suppression), but
// presents the results to the player with actionable targets:
//  - hotspot regions/systems
//  - high-volume corridors
//  - jump-link chokepoints
//
// The analysis is best-effort and deterministic given the current GameState.

struct SecurityPlannerOptions {
  // Faction to compute "economic exposure" for.
  // When kInvalidId, lanes are not filtered by colony ownership.
  Id faction_id{kInvalidId};

  // If true, only consider travel through systems discovered by faction_id.
  // (When faction_id is invalid, this has no effect.)
  bool restrict_to_discovered{true};

  // If true, only consider trade lanes where at least one endpoint contains a
  // colony owned by faction_id.
  bool require_own_colony_endpoints{true};

  // Safety cap on how many trade lanes are considered.
  int max_lanes{48};

  // Ignore lanes below this volume.
  double min_lane_volume{1.0};

  // How strongly risk amplifies security need.
  // Need ~= volume_share * (0.20 + risk_weight * risk)
  double risk_weight{1.2};

  // Extra multiplier when a corridor passes through a system that contains a
  // colony owned by faction_id.
  double own_colony_weight{1.5};

  // Desired regional suppression to show a "patrol power" target.
  // (Used only for Region summaries.)
  double desired_region_suppression{0.75};

  // Route planning speed (km/s) used to choose jump entry points.
  // This does not affect the need score directly, but makes corridor routes
  // stable for UI presentation.
  double planning_speed_km_s{1000.0};

  // How many rows to emit for each table (regions/systems/corridors/chokepoints).
  int max_results{32};
};

// Per-system security demand summary.
struct SecuritySystemNeed {
  Id system_id{kInvalidId};
  Id region_id{kInvalidId};

  // Aggregate need score (dimensionless).
  double need{0.0};

  // Trade volume share attributed to this system (sum of per-corridor shares).
  double trade_throughput{0.0};

  // Risk breakdown (0..1 each).
  double piracy_risk{0.0};
  double blockade_pressure{0.0};
  double shipping_loss_pressure{0.0};
  double endpoint_risk{0.0};

  bool has_own_colony{false};
};

// Per-region security demand summary.
struct SecurityRegionNeed {
  Id region_id{kInvalidId};
  double need{0.0};

  // Region piracy parameters.
  double pirate_risk{0.0};
  double pirate_suppression{0.0};
  double effective_piracy_risk{0.0};

  // "Implied" patrol power from current suppression using:
  //   suppression = 1 - exp(-power / scale)
  double implied_patrol_power{0.0};
  double desired_patrol_power{0.0};
  double additional_patrol_power{0.0};

  // Representative system (highest need within the region).
  Id representative_system_id{kInvalidId};
  double representative_system_need{0.0};
};

// A high-volume lane in the trade network, annotated with route + risk.
struct SecurityCorridor {
  Id from_system_id{kInvalidId};
  Id to_system_id{kInvalidId};

  double volume{0.0};
  double avg_risk{0.0};
  double max_risk{0.0};

  // Planned route systems (inclusive endpoints).
  std::vector<Id> route_systems;

  // Top trade goods contributing to the lane (for UI tooltips).
  std::vector<TradeGoodFlow> top_flows;
};

// A jump-link "edge" that carries high trade traffic and/or risk.
struct SecurityChokepoint {
  Id system_a_id{kInvalidId};
  Id system_b_id{kInvalidId};
  double traffic{0.0};
  double avg_risk{0.0};
  double max_risk{0.0};

  // Jump point ids, if resolvable.
  Id jump_a_to_b{kInvalidId};
  Id jump_b_to_a{kInvalidId};
};

struct SecurityPlannerResult {
  bool ok{false};
  bool truncated{false};
  std::string message;

  std::vector<SecurityRegionNeed> top_regions;
  std::vector<SecuritySystemNeed> top_systems;
  std::vector<SecurityCorridor> top_corridors;
  std::vector<SecurityChokepoint> top_chokepoints;
};

// Compute a best-effort security analysis for a faction.
SecurityPlannerResult compute_security_plan(const Simulation& sim, const SecurityPlannerOptions& opt = {});

}  // namespace nebula4x
