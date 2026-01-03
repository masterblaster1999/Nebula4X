#pragma once

#include <optional>
#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

class Simulation;

// A single mineral transfer item (mineral + tonnage), optionally annotated with
// a human-readable reason describing why the destination wants it.
struct FreightPlanItem {
  std::string mineral;
  double tons{0.0};
  std::string reason;
};

enum class FreightAssignmentKind {
  // The ship already has cargo; plan only an unload leg.
  DeliverCargo,
  // The ship is empty; plan a pickup leg at a source colony and delivery to a destination.
  PickupAndDeliver,
};

struct FreightAssignment {
  FreightAssignmentKind kind{FreightAssignmentKind::PickupAndDeliver};

  Id ship_id{kInvalidId};
  Id source_colony_id{kInvalidId};
  Id dest_colony_id{kInvalidId};

  // When true, any jump routing performed by apply_* helpers should only traverse
  // systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  std::vector<FreightPlanItem> items;

  // ETA breakdowns are best-effort travel-only estimates based on jump route planning.
  // They ignore docking/loading/unloading durations.
  double eta_to_source_days{0.0};
  double eta_to_dest_days{0.0};
  double eta_total_days{0.0};

  // Optional high-level note.
  std::string note;
};

struct FreightPlannerOptions {
  // If true, only consider ships with Ship::auto_freight enabled.
  bool require_auto_freight_flag{true};

  // If true, only consider ships that are currently idle (no queued orders, or repeat completed).
  bool require_idle{true};

  // If true, jump routing will only traverse systems discovered by the planning faction.
  bool restrict_to_discovered{true};

  // Override the config default for multi-mineral bundling.
  // If std::nullopt, uses SimConfig::auto_freight_multi_mineral.
  std::optional<bool> bundle_multi_mineral{std::nullopt};

  // Safety cap.
  int max_ships{256};
};

struct FreightPlannerResult {
  bool ok{false};
  bool truncated{false};
  std::string message;

  std::vector<FreightAssignment> assignments;
};

// Compute a best-effort freight plan for a faction.
//
// This is designed to mirror (at a high level) the simulation's auto-freight logic,
// but without mutating game state.
FreightPlannerResult compute_freight_plan(const Simulation& sim, Id faction_id,
                                          const FreightPlannerOptions& opt = {});

// Apply a single assignment by enqueueing travel/load/unload orders.
// Returns false if any order could not be issued.
bool apply_freight_assignment(Simulation& sim, const FreightAssignment& asg, bool clear_existing_orders = true);

// Apply an entire plan.
bool apply_freight_plan(Simulation& sim, const FreightPlannerResult& plan, bool clear_existing_orders = true);

}  // namespace nebula4x
