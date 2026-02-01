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

// Predict which system/position a ship would be in after executing the queued
// orders currently in its ShipOrders queue.
//
// This is a best-effort approximation intended for UI helpers and goal-aware
// routing when shift-queuing additional orders. It simulates the subset of
// orders that deterministically affect position and cross-system travel.
inline PredictedNavState predicted_nav_state_after_queued_orders(const GameState& s, Id ship_id,
                                                                bool include_queued_orders) {
  const auto* ship = find_ptr(s.ships, ship_id);
  if (!ship) return PredictedNavState{};

  PredictedNavState out;
  out.system_id = ship->system_id;
  out.position_mkm = ship->position_mkm;
  if (!include_queued_orders) return out;

  auto it = s.ship_orders.find(ship_id);
  if (it == s.ship_orders.end()) return out;

  Id sys = out.system_id;
  Vec2 pos = out.position_mkm;

  auto transit_jump_best_effort = [&](Id jump_id) {
    const auto* jp = find_ptr(s.jump_points, jump_id);
    if (!jp) return;
    if (jp->system_id != sys) return;
    if (jp->linked_jump_id == kInvalidId) return;
    const auto* dest = find_ptr(s.jump_points, jp->linked_jump_id);
    if (!dest) return;
    if (dest->system_id == kInvalidId) return;
    if (!find_ptr(s.systems, dest->system_id)) return;
    sys = dest->system_id;
    pos = dest->position_mkm;
  };

  auto move_to_jump_point_best_effort = [&](Id jump_id) {
    const auto* jp = find_ptr(s.jump_points, jump_id);
    if (!jp) return;
    if (jp->system_id != sys) return;
    pos = jp->position_mkm;
  };

  auto move_to_body_best_effort = [&](Id body_id) {
    const auto* b = find_ptr(s.bodies, body_id);
    if (!b) return;
    if (b->system_id != sys) return;
    pos = b->position_mkm;
  };

  auto move_to_colony_best_effort = [&](Id colony_id) {
    const auto* c = find_ptr(s.colonies, colony_id);
    if (!c) return;
    move_to_body_best_effort(c->body_id);
  };

  auto move_to_wreck_best_effort = [&](Id wreck_id) {
    const auto* w = find_ptr(s.wrecks, wreck_id);
    if (!w) return;
    if (w->system_id != sys) return;
    pos = w->position_mkm;
  };

  auto move_to_anomaly_best_effort = [&](Id anomaly_id) {
    const auto* a = find_ptr(s.anomalies, anomaly_id);
    if (!a) return;
    if (a->system_id != sys) return;
    pos = a->position_mkm;
  };

  auto move_to_ship_best_effort = [&](Id target_ship_id) {
    const auto* t = find_ptr(s.ships, target_ship_id);
    if (!t) return;
    if (t->system_id != sys) return;
    pos = t->position_mkm;
  };

  for (const auto& ord : it->second.queue) {
    if (std::holds_alternative<MoveToPoint>(ord)) {
      pos = std::get<MoveToPoint>(ord).target_mkm;
    } else if (std::holds_alternative<MoveToBody>(ord)) {
      move_to_body_best_effort(std::get<MoveToBody>(ord).body_id);
    } else if (std::holds_alternative<ColonizeBody>(ord)) {
      move_to_body_best_effort(std::get<ColonizeBody>(ord).body_id);
      // Colonization removes the ship; any subsequent orders would be meaningless.
      break;
    } else if (std::holds_alternative<OrbitBody>(ord)) {
      move_to_body_best_effort(std::get<OrbitBody>(ord).body_id);
    } else if (std::holds_alternative<MineBody>(ord)) {
      move_to_body_best_effort(std::get<MineBody>(ord).body_id);
    } else if (std::holds_alternative<TravelViaJump>(ord)) {
      transit_jump_best_effort(std::get<TravelViaJump>(ord).jump_point_id);
    } else if (std::holds_alternative<SurveyJumpPoint>(ord)) {
      const auto& sj = std::get<SurveyJumpPoint>(ord);
      move_to_jump_point_best_effort(sj.jump_point_id);
      if (sj.transit_when_done) {
        transit_jump_best_effort(sj.jump_point_id);
      }
    } else if (std::holds_alternative<LoadMineral>(ord)) {
      move_to_colony_best_effort(std::get<LoadMineral>(ord).colony_id);
    } else if (std::holds_alternative<UnloadMineral>(ord)) {
      move_to_colony_best_effort(std::get<UnloadMineral>(ord).colony_id);
    } else if (std::holds_alternative<LoadTroops>(ord)) {
      move_to_colony_best_effort(std::get<LoadTroops>(ord).colony_id);
    } else if (std::holds_alternative<UnloadTroops>(ord)) {
      move_to_colony_best_effort(std::get<UnloadTroops>(ord).colony_id);
    } else if (std::holds_alternative<LoadColonists>(ord)) {
      move_to_colony_best_effort(std::get<LoadColonists>(ord).colony_id);
    } else if (std::holds_alternative<UnloadColonists>(ord)) {
      move_to_colony_best_effort(std::get<UnloadColonists>(ord).colony_id);
    } else if (std::holds_alternative<InvadeColony>(ord)) {
      move_to_colony_best_effort(std::get<InvadeColony>(ord).colony_id);
    } else if (std::holds_alternative<BombardColony>(ord)) {
      move_to_colony_best_effort(std::get<BombardColony>(ord).colony_id);
    } else if (std::holds_alternative<ScrapShip>(ord)) {
      move_to_colony_best_effort(std::get<ScrapShip>(ord).colony_id);
      // Scrapping removes the ship; any subsequent orders would be meaningless.
      break;
    } else if (std::holds_alternative<SalvageWreck>(ord)) {
      move_to_wreck_best_effort(std::get<SalvageWreck>(ord).wreck_id);
    } else if (std::holds_alternative<SalvageWreckLoop>(ord)) {
      const auto& sl = std::get<SalvageWreckLoop>(ord);
      if (sl.mode == 1 && sl.dropoff_colony_id != kInvalidId) {
        move_to_colony_best_effort(sl.dropoff_colony_id);
      } else {
        move_to_wreck_best_effort(sl.wreck_id);
      }
    } else if (std::holds_alternative<InvestigateAnomaly>(ord)) {
      move_to_anomaly_best_effort(std::get<InvestigateAnomaly>(ord).anomaly_id);
    } else if (std::holds_alternative<AttackShip>(ord)) {
      const auto& a = std::get<AttackShip>(ord);
      if (a.has_last_known && a.last_known_system_id == sys) {
        pos = a.last_known_position_mkm;
      } else {
        move_to_ship_best_effort(a.target_ship_id);
      }
    } else if (std::holds_alternative<EscortShip>(ord)) {
      move_to_ship_best_effort(std::get<EscortShip>(ord).target_ship_id);
    } else if (std::holds_alternative<TransferCargoToShip>(ord)) {
      move_to_ship_best_effort(std::get<TransferCargoToShip>(ord).target_ship_id);
    } else if (std::holds_alternative<TransferFuelToShip>(ord)) {
      move_to_ship_best_effort(std::get<TransferFuelToShip>(ord).target_ship_id);
    } else if (std::holds_alternative<TransferTroopsToShip>(ord)) {
      move_to_ship_best_effort(std::get<TransferTroopsToShip>(ord).target_ship_id);
    } else if (std::holds_alternative<TransferColonistsToShip>(ord)) {
      move_to_ship_best_effort(std::get<TransferColonistsToShip>(ord).target_ship_id);
    }
  }

  out.system_id = sys;
  out.position_mkm = pos;
  return out;
}

} // namespace nebula4x::sim_nav
