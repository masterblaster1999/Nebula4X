#pragma once

#include "nebula4x/core/game_state.h"

namespace nebula4x::sim_nav {

struct PredictedNavState {
  Id system_id{kInvalidId};
  Vec2 position_mkm{0.0, 0.0};
};

// Predict which system/position a ship would be in after executing the queued
// TravelViaJump orders currently in its ShipOrders queue.
//
// This is a lightweight helper used for:
// - Shift-queue previews (UI)
// - Ensuring subsequent travel commands pathfind from the end-of-queue system.
inline PredictedNavState predicted_nav_state_after_queued_jumps(const GameState& s, Id ship_id,
                                                               bool include_queued_jumps) {
  const auto* ship = find_ptr(s.ships, ship_id);
  if (!ship) return PredictedNavState{};

  PredictedNavState out;
  out.system_id = ship->system_id;
  out.position_mkm = ship->position_mkm;
  if (!include_queued_jumps) return out;

  auto it = s.ship_orders.find(ship_id);
  if (it == s.ship_orders.end()) return out;

  Id sys = out.system_id;
  Vec2 pos = out.position_mkm;

  for (const auto& ord : it->second.queue) {
    if (!std::holds_alternative<TravelViaJump>(ord)) continue;
    const Id jump_id = std::get<TravelViaJump>(ord).jump_point_id;
    const auto* jp = find_ptr(s.jump_points, jump_id);
    if (!jp) continue;
    if (jp->system_id != sys) continue;
    if (jp->linked_jump_id == kInvalidId) continue;
    const auto* dest = find_ptr(s.jump_points, jp->linked_jump_id);
    if (!dest) continue;
    if (dest->system_id == kInvalidId) continue;
    if (!find_ptr(s.systems, dest->system_id)) continue;
    sys = dest->system_id;
    pos = dest->position_mkm;
  }

  out.system_id = sys;
  out.position_mkm = pos;
  return out;
}

} // namespace nebula4x::sim_nav
