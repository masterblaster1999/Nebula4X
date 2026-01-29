#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x {

class Simulation;

// Tooling feature: partition star systems into procedural regions using k-means.
// Compute a plan first (previewable), then apply it to a GameState.

struct RegionPlannerOptions {
  // Desired number of regions (clusters). The planner will clamp this to the
  // number of eligible systems.
  int k{12};

  // Random seed used for initialization (k-means++).
  std::uint32_t seed{1};

  // Maximum k-means refinement iterations.
  int max_iters{25};

  // If true, only systems whose region_id == kInvalidId are included.
  bool only_unassigned_systems{false};

  // If true, only include systems discovered by viewer_faction_id.
  // (Useful when using Fog-of-War and you want to avoid spoilers.)
  bool restrict_to_discovered{false};
  Id viewer_faction_id{kInvalidId};

  // Safety cap for extremely large scenarios.
  int max_systems{8192};
};

struct RegionClusterPlan {
  // Prototype region fields (id is assigned during apply).
  Region region;

  // Assigned systems for this cluster.
  std::vector<Id> system_ids;

  // Sum of squared distances to the cluster center.
  double inertia{0.0};
};

struct RegionPlannerResult {
  bool ok{false};
  std::string message;

  // Centers and membership lists.
  std::vector<RegionClusterPlan> clusters;

  // Deterministic mapping: system id -> cluster index.
  std::vector<std::pair<Id, int>> assignment;

  // Total inertia across all clusters.
  double total_inertia{0.0};

  // Metadata for UI convenience.
  int eligible_systems{0};
  int used_k{0};
  int iters_run{0};
};

struct RegionPlannerApplyOptions {
  // When true, remove all existing regions and clear all system.region_id
  // fields before applying. This is a hard reset.
  bool wipe_existing_regions{false};

  // When false, systems not included in the plan keep their current region_id.
  // When true, systems not included in the plan are set to kInvalidId.
  bool clear_unplanned_system_assignments{false};

  // Name prefix used when the plan clusters do not already have a name.
  // Example: "Sector" -> "Sector 1", "Sector 2", ...
  std::string name_prefix{"Region"};
};

// Computes a k-means based region partition plan.
RegionPlannerResult compute_region_partition_plan(const Simulation& sim, const RegionPlannerOptions& opt);

// Applies a computed plan to the game state.
// Returns false and sets error on failure.
bool apply_region_partition_plan(GameState& s, const RegionPlannerResult& plan,
                                 const RegionPlannerApplyOptions& opt,
                                 std::string* error = nullptr);

}  // namespace nebula4x
