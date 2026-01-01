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
};

// Move to a jump point and transit to the linked system when reached.
struct TravelViaJump {
  Id jump_point_id{kInvalidId};
};

// Close and engage a target ship (combat will also happen opportunistically).
struct AttackShip {
  Id target_ship_id{kInvalidId};

  // Fog-of-war friendly: keep a last-known target position.
  // - When the target is detected, simulation updates last_known_position_mkm.
  // - When contact is lost, ships will move to last_known_position_mkm.
  bool has_last_known{false};
  Vec2 last_known_position_mkm{0.0, 0.0};
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
};

// Wait / do nothing for N simulation days.
//
// This is a simple scheduling primitive that lets players insert delays between
// other queued orders.
struct WaitDays {
  int days_remaining{0};
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

// Decommission (scrap) a ship at a friendly colony.
struct ScrapShip {
  Id colony_id{kInvalidId};
};

using Order = std::variant<MoveToPoint,
                           MoveToBody,
                           ColonizeBody,
                           OrbitBody,
                           TravelViaJump,
                           AttackShip,
                           EscortShip,
                           WaitDays,
                           LoadMineral,
                           UnloadMineral,
                           LoadTroops,
                           UnloadTroops,
                           LoadColonists,
                           UnloadColonists,
                           InvadeColony,
                           BombardColony,
                           SalvageWreck,
                           TransferCargoToShip,
                           TransferFuelToShip,
                           TransferTroopsToShip,
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
};

std::string order_to_string(const Order& order);

} // namespace nebula4x
