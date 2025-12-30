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

// Invade a hostile colony using embarked troops.
// The ship will move into docking range of the colony's body and then
// initiate a ground battle.
struct InvadeColony {
  Id colony_id{kInvalidId};
};

// Transfer minerals from this ship's cargo into another friendly ship.
// If mineral is empty, transfer all minerals (until capacity or requested tons).
// If tons <= 0, transfer as much as possible.
struct TransferCargoToShip {
  Id target_ship_id{kInvalidId};
  std::string mineral;
  double tons{0.0};
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
                           WaitDays,
                           LoadMineral,
                           UnloadMineral,
                           LoadTroops,
                           UnloadTroops,
                           InvadeColony,
                           TransferCargoToShip,
                           ScrapShip>;

struct ShipOrders {
  std::vector<Order> queue;

  // If enabled, when the order queue becomes empty it will automatically be
  // refilled from repeat_template.
  //
  // This is a lightweight way to support repeating logistics routes/patrols
  // without introducing a new Order variant.
  bool repeat{false};
  std::vector<Order> repeat_template;
};

std::string order_to_string(const Order& order);

} // namespace nebula4x
