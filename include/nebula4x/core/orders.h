#pragma once

#include <string>
#include <variant>
#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x {

struct MoveToPoint {
  Vec2 target_mkm;
};

struct MoveToBody {
  Id body_id{kInvalidId};
};

// Establish a new colony on an (uncolonized) body.
//
// Notes:
// - This behaves like a MoveToBody order until the ship arrives in docking range,
//   at which point the colony is created and the colonizer ship is removed.
// - The ship must have a non-zero colony capacity (provided by a colony module
//   component).
// - Any cargo carried by the ship is transferred to the new colony as starting
//   stockpile.
struct ColonizeBody {
  Id body_id{kInvalidId};

  // Optional: if empty, the simulation will pick a default name based on the
  // target body.
  std::string colony_name;
};

// Station-keep with a body for a duration.
//
// duration_days:
//  -1  => indefinite
//   0  => complete immediately
//  >0  => decrement once per sim day while in docking range
struct OrbitBody {
  Id body_id{kInvalidId};
  int duration_days{-1};

  // Accumulated time spent orbiting (in days).
  // Used to make duration_days behave consistently under sub-day turn ticks.
  double progress_days{0.0};
};

// Move to a jump point and transit to the linked system when reached.
struct TravelViaJump {
  Id jump_point_id{kInvalidId};
};

// Move to a jump point and remain in range until it is surveyed by your faction.
//
// Notes:
// - Survey progress is earned by any ship with online sensors while within survey range
//   of an unsurveyed jump point (see jump survey rules in Simulation::tick_ships).
// - This order is a UI convenience that causes the ship to stay on-station at the jump
//   point until the survey completes (or instantly completes when surveying is disabled).
// - When transit_when_done is true, the ship will immediately transit the jump point
//   once surveyed (equivalent to enqueueing TravelViaJump next).
struct SurveyJumpPoint {
  Id jump_point_id{kInvalidId};

  // When true, transit the jump point once the survey completes.
  bool transit_when_done{false};
};


// Close and engage a target ship (combat will also happen opportunistically).
struct AttackShip {
  Id target_ship_id{kInvalidId};

  // Fog-of-war friendly: keep a last-known target position.
  // - When the target is detected, simulation updates last_known_position_mkm.
  // - When contact is lost, ships will move to last_known_position_mkm.
  bool has_last_known{false};
  Vec2 last_known_position_mkm{0.0, 0.0};

  // System containing last_known_position_mkm.
  //
  // This makes AttackShip robust when the target transits a jump point: ships
  // can continue pursuing a contact track without requiring omniscient
  // knowledge of which system the target is currently in.
  Id last_known_system_id{kInvalidId};

  // Date::days_since_epoch() when last_known_position_mkm was last refreshed by
  // an actual detection (or by pursuit heuristics such as jump-chasing).
  int last_known_day{0};

  // Safety valve: how many times this order has pursued a hypothesized jump.
  // Prevents infinite bouncing when the target repeatedly slips away.
  int pursuit_hops{0};

  // Lost-contact search state.
  //
  // When the target is not currently detected, AttackShip behaves like a
  // bounded search operation around the predicted track position
  // (last_known_position_mkm). To avoid "jitter" from retargeting a different
  // random point every day, the simulation keeps a persistent waypoint offset
  // and advances it only after reaching the current waypoint.
  //
  // - search_waypoint_index: monotonically increases as each waypoint is
  //   reached. Index 0 corresponds to the track center.
  // - has_search_offset/search_offset_mkm: current waypoint offset (mkm) from
  //   the predicted track center. When false, the active waypoint is the track
  //   center.
  int search_waypoint_index{0};
  bool has_search_offset{false};
  Vec2 search_offset_mkm{0.0, 0.0};
};

// Escort a friendly ship.
//
// Notes:
// - If the target is in another system, the escort will automatically route
//   through the jump network and transit jump points as needed.
// - In the destination system, the escort will attempt to maintain a
//   follow_distance_mkm separation.
// - This order is indefinite; cancel it manually or when the target no longer
//   exists.
struct EscortShip {
  Id target_ship_id{kInvalidId};
  double follow_distance_mkm{1.0};

  // When true, cross-system routing will only traverse systems discovered by
  // the escort's faction.
  bool restrict_to_discovered{false};

  // When true, allow escorting neutral (non-friendly) ships as long as the
  // factions are not Hostile toward each other.
  //
  // This is primarily used for escort contracts involving civilian convoys.
  bool allow_neutral{false};
};

// Wait / do nothing for N simulation days.
//
// This is a simple scheduling primitive that lets players insert delays between
// other queued orders.
struct WaitDays {
  int days_remaining{0};

  // Accumulated time waited (in days).
  // Used to make days_remaining behave consistently under sub-day turn ticks.
  double progress_days{0.0};
};

// Load minerals from a friendly colony into this ship's cargo.
// If mineral is empty, load from all minerals (until capacity or requested tons).
// If tons <= 0, load as much as possible.
struct LoadMineral {
  Id colony_id{kInvalidId};
  std::string mineral;
  double tons{0.0};
};

// Unload minerals from this ship's cargo into a friendly colony.
// If mineral is empty, unload all cargo minerals (up to requested tons).
// If tons <= 0, unload as much as possible.
struct UnloadMineral {
  Id colony_id{kInvalidId};
  std::string mineral;
  double tons{0.0};
};

// Mobile mining: extract minerals directly from a body's deposits into ship cargo.
//
// Notes:
// - Requires the ship to have mining capacity (from mining components).
// - The ship must be within docking range of the target body to mine.
// - mineral == "" means "mine all available minerals" (in deterministic order).
// - When stop_when_cargo_full is true, the order completes once the ship has no free cargo capacity.
struct MineBody {
  Id body_id{kInvalidId};
  std::string mineral;
  bool stop_when_cargo_full{true};
};

// Load troops from a friendly colony into this ship.
// If strength <= 0, load as much as possible (up to troop capacity).
struct LoadTroops {
  Id colony_id{kInvalidId};
  double strength{0.0};
};

// Unload troops from this ship into a friendly colony.
// If strength <= 0, unload as much as possible.
struct UnloadTroops {
  Id colony_id{kInvalidId};
  double strength{0.0};
};

// Load colonists / passengers from an owned colony into this ship.
//
// Notes:
// - Uses the ship design's colony_capacity_millions as passenger capacity.
// - If millions <= 0, load as many colonists as possible (up to capacity).
struct LoadColonists {
  Id colony_id{kInvalidId};
  double millions{0.0};
};

// Unload colonists / passengers from this ship into an owned colony.
// If millions <= 0, unload as many colonists as possible.
struct UnloadColonists {
  Id colony_id{kInvalidId};
  double millions{0.0};
};

// Invade a hostile colony using embarked troops.
// The ship will move into docking range of the colony's body and then
// initiate a ground battle.
struct InvadeColony {
  Id colony_id{kInvalidId};
};

// Bombard a colony from orbit.
//
// The ship will move to within weapon range of the colony's body and then
// apply damage each day during Simulation::tick_combat().
//
// duration_days:
//  -1  => bombard indefinitely (until cancelled)
//   0  => complete immediately
//  >0  => decrement once per day while bombardment successfully fires
struct BombardColony {
  Id colony_id{kInvalidId};
  int duration_days{-1};

  // Accumulated time spent bombarding (in days).
  // Used to make duration_days behave consistently under sub-day turn ticks.
  double progress_days{0.0};
};

// Salvage minerals from a wreck into this ship's cargo.
//
// - If mineral is empty, salvage all minerals until cargo is full or the wreck is empty.
// - If tons <= 0, salvage as much as possible.
struct SalvageWreck {
  Id wreck_id{kInvalidId};
  std::string mineral;
  double tons{0.0};
};

// Salvage a wreck to completion with automatic unloading to a friendly colony.
//
// Behaviour:
//  - Salvage until cargo is full or the wreck is empty.
//  - Travel to a friendly colony (same faction) and unload all minerals.
//  - Return to the wreck and repeat until it is depleted.
//
// Notes:
//  - dropoff_colony_id is optional. If invalid, the simulation will pick the
//    nearest reachable friendly colony when unloading is required.
//  - mode: 0 = salvage stage, 1 = unload stage
struct SalvageWreckLoop {
  Id wreck_id{kInvalidId};
  Id dropoff_colony_id{kInvalidId};
  bool restrict_to_discovered{false};
  int mode{0};
};

// Investigate an anomaly (point of interest) in a system.
//
// The ship will move to the anomaly position and (once implemented) remain on
// station for duration_days to resolve it.
//
// duration_days:
//  0 => use anomaly default (filled by issue helper)
// >0 => explicit duration for this investigation
//
// progress_days accumulates fractional days under sub-day ticks.
struct InvestigateAnomaly {
  Id anomaly_id{kInvalidId};

  int duration_days{0};
  double progress_days{0.0};
};

// Transfer minerals from this ship's cargo into another friendly ship.
// If mineral is empty, transfer all minerals (until capacity or requested tons).
// If tons <= 0, transfer as much as possible.
struct TransferCargoToShip {
  Id target_ship_id{kInvalidId};
  std::string mineral;
  double tons{0.0};
};

// Transfer fuel from this ship's tanks into another friendly ship.
// If tons <= 0, transfer as much as possible (up to target free capacity).
struct TransferFuelToShip {
  Id target_ship_id{kInvalidId};
  double tons{0.0};
};

// Transfer embarked troops from this ship into another friendly ship.
// If strength <= 0, transfer as much as possible (up to target free troop capacity).
struct TransferTroopsToShip {
  Id target_ship_id{kInvalidId};
  double strength{0.0};
};

// Transfer embarked colonists (population) from this ship into another friendly ship.
// If millions <= 0, transfer as much as possible (up to target free colony capacity).
struct TransferColonistsToShip {
  Id target_ship_id{kInvalidId};
  double millions{0.0};
};

// Decommission (scrap) a ship at a friendly colony.
struct ScrapShip {
  Id colony_id{kInvalidId};
};

using Order = std::variant<MoveToPoint,
                           MoveToBody,
                           ColonizeBody,
                           OrbitBody,
                           TravelViaJump,
                           SurveyJumpPoint,
                           AttackShip,
                           EscortShip,
                           WaitDays,
                           LoadMineral,
                           UnloadMineral,
                           MineBody,
                           LoadTroops,
                           UnloadTroops,
                           LoadColonists,
                           UnloadColonists,
                           InvadeColony,
                           BombardColony,
                           SalvageWreck,
                           SalvageWreckLoop,
                           InvestigateAnomaly,
                           TransferCargoToShip,
                           TransferFuelToShip,
                           TransferTroopsToShip,
                           TransferColonistsToShip,
                           ScrapShip>;

struct ShipOrders {
  std::vector<Order> queue;

  // If enabled, when the order queue becomes empty it will automatically be
  // refilled from repeat_template.
  //
  // repeat_count_remaining controls how many times the template will be
  // re-enqueued once the active queue finishes:
  //   -1 => infinite repeats
  //    0 => do not refill again (repeat stops once the current queue finishes)
  //   >0 => remaining number of refills allowed
  //
  // This is a lightweight way to support repeating logistics routes/patrols
  // without introducing a new Order variant.
  bool repeat{false};

  int repeat_count_remaining{0};

  std::vector<Order> repeat_template;

  // Emergency suspension (used by auto-retreat).
  //
  // When suspended is true, queue contains a temporary emergency plan
  // (or may be empty while the ship waits to recover), and the original
  // queue/repeat settings are stored in the suspended_* fields.
  //
  // This avoids permanently destroying player-issued orders while still
  // allowing ships to disengage when they are about to be lost.
  bool suspended{false};

  std::vector<Order> suspended_queue;
  bool suspended_repeat{false};
  int suspended_repeat_count_remaining{0};
  std::vector<Order> suspended_repeat_template;
};

// Returns true when a ship's orders are considered "idle" for automation/planners.
//
// A ship is NOT idle when:
//  - it is suspended (auto-retreat temporary plan),
//  - it has any queued orders, or
//  - it has active repeating orders that will auto-refill on the next ship tick.
//
// This is used to prevent planners/automation from overwriting ships that are
// running player-defined repeating routes/patrols.
inline bool ship_orders_is_idle_for_automation(const ShipOrders& so) {
  if (so.suspended) return false;
  if (!so.queue.empty()) return false;
  if (so.repeat && !so.repeat_template.empty() && so.repeat_count_remaining != 0) return false;
  return true;
}

inline bool ship_orders_is_idle_for_automation(const ShipOrders* so) {
  if (!so) return true;
  return ship_orders_is_idle_for_automation(*so);
}

std::string order_to_string(const Order& order);

} // namespace nebula4x
