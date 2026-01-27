#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

class Simulation;

struct MinePlannerOptions {
  // If true, only consider ships with Ship::auto_mine enabled.
  bool require_auto_mine_flag{false};

  // If true, skip ships that have other "primary" automation modes enabled
  // (auto-salvage/freight/explore/colonize/tanker).
  bool exclude_conflicting_automation_flags{false};

  // If true, only consider ships with no queued orders.
  bool require_idle{true};

  // If true, only consider bodies in systems discovered by the faction.
  bool restrict_to_discovered{true};

  // If true, skip ships currently assigned to a fleet.
  bool exclude_fleet_ships{true};

  // If true, avoid assigning mining in systems where the faction has detected hostile ships.
  bool avoid_hostile_systems{true};

  // If true, treat bodies already targeted by existing MineBody orders as reserved.
  bool reserve_bodies_targeted_by_existing_orders{true};

  // Optional caller-provided reserved bodies (excluded from consideration).
  std::vector<Id> reserved_body_ids;

  // Minimum meaningful tons (0 => use Simulation::cfg().auto_freight_min_transfer_tons).
  // This is used as a filter threshold for tiny ship cargo caps and tiny deposits.
  double min_tons{0.0};

  // Hard caps to keep planning bounded on large games.
  int max_ships{256};
  int max_bodies{256};
};

enum class MineAssignmentKind {
  DeliverCargo,
  MineAndDeliver,
};

struct MineAssignment {
  MineAssignmentKind kind{MineAssignmentKind::MineAndDeliver};

  Id ship_id{kInvalidId};

  // Mining target (MineAndDeliver).
  Id body_id{kInvalidId};
  std::string mineral;
  bool stop_when_cargo_full{true};

  // Where to unload (DeliverCargo + MineAndDeliver).
  Id dest_colony_id{kInvalidId};

  // Estimates for UI/debugging.
  double eta_to_mine_days{0.0};
  double eta_to_dest_days{0.0};
  double eta_total_days{0.0};

  double expected_mined_tons{0.0};
  double deposit_tons{0.0};
  double mine_tons_per_day{0.0};
  double est_mine_days{0.0};

  std::string note;
};

struct MinePlannerResult {
  bool ok{false};
  bool truncated{false};
  std::string message;
  std::vector<MineAssignment> assignments;
};

MinePlannerResult compute_mine_plan(
    const Simulation& sim,
    Id faction_id,
    const MinePlannerOptions& opt = {});

bool apply_mine_assignment(
    Simulation& sim,
    const MineAssignment& asg,
    bool clear_existing_orders = true);

bool apply_mine_plan(
    Simulation& sim,
    const MinePlannerResult& plan,
    bool clear_existing_orders = true);

}  // namespace nebula4x
